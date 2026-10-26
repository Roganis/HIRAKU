// SPDX-License-Identifier: GPL-3.0-or-later
// The game's sound (PLAN.md section 32): WASAPI, shared mode, 10 ms, followed from CoCreateInstance
// to its IAudioClient. Its sound library resamples to the mix format through Windows' resampler
// DMO, which fails under Wine (dmo resampler_init -22) and stalled its sound after 30 ms; the mix
// format is reported at the game's own 44.1 kHz, so nothing is resampled in the game (Wine
// converts).
// Linked to hiraku-pc, the loader is the sound device, as runtime/pc/dsound.c is for pop'n 15-18:
// Wine's stream is set up but never started, the game's IAudioRenderClient is the loader's, and
// what it writes waits in a buffer (its padding) until the device's clock plays it into the link's
// ring (to_ring), and the game's event says there is room. The game uses only IAudioClient and
// IAudioRenderClient.
#define COBJMACROS
#define CINTERFACE
#include "pc64.h"
#include "../pc/link.h"

#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM   // (older mingw headers: Ubuntu 22.04's)
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

static const GUID float_format = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};   // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT

#define BUF_FRAMES 2048u   // the buffer's most (a power of two)

static struct {
    int on;                          // the link takes the sound
    int started;
    UINT32 size;                     // the buffer's frames, as the game is told
    UINT32 channels, bytes;          // the game's format: its channels, a sample's bytes
    int is_float;
    HANDLE event;                    // the game's (event-driven streams)
    volatile UINT32 written, played; // frames, since the start
    float fifo[2 * BUF_FRAMES];
    BYTE staging[BUF_FRAMES * 8 * 4];
    CRITICAL_SECTION lock;
} g_s;

static IMMDeviceEnumeratorVtbl r_enumerator;
static IMMDeviceVtbl r_device;
static IAudioClientVtbl r_client;

static HRESULT WINAPI GetMixFormat_(IAudioClient *c, WAVEFORMATEX **f) {
    HRESULT h = r_client.GetMixFormat(c, f);
    if (SUCCEEDED(h) && *f) {
        (*f)->nSamplesPerSec = 44100;
        (*f)->nAvgBytesPerSec = 44100 * (*f)->nBlockAlign;
    }
    return h;
}
// The device's clock (to_ring): its frequency and start, frames played, the event's period
static LARGE_INTEGER g_freq, g_t0;
static uint64_t g_clock;      // frames the device has played
static uint32_t g_period;
static uint32_t g_late;       // times the game hadn't written what the device played
static int g_clock_on;

static void start_mixer(void);

static HRESULT WINAPI Initialize_(IAudioClient *c, AUDCLNT_SHAREMODE m, DWORD flags, REFERENCE_TIME d, REFERENCE_TIME p, const WAVEFORMATEX *f,
                                  LPCGUID session) {
    HRESULT h = r_client.Initialize(c, m, flags, d, p, f, session);
    if (!f) return h;
    // Windows' shared mode takes only the mix format's own rate, and the game asks for the 44.1 kHz
    // GetMixFormat_ reported (a device at 48 kHz: AUDCLNT_E_UNSUPPORTED_FORMAT, the game's sound never
    // started and its sound thread faulted): Windows converts it, as Wine does unasked
    const HRESULT first = h;
    if (FAILED(h) && !m) h = r_client.Initialize(c, m, flags | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, d, p, f, session);
    int is_float = f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && f->cbSize >= 22)
        is_float = IsEqualGUID(&((const WAVEFORMATEXTENSIBLE *)f)->SubFormat, &float_format);
    const int fits = f->nSamplesPerSec == HK_PC_AUDIO_RATE && f->nChannels >= 1 && f->nChannels <= 8 &&
                     ((is_float && f->wBitsPerSample == 32) || (!is_float && (f->wBitsPerSample == 16 || f->wBitsPerSample == 32)));
    const int ours = pc64_link() && fits && !g_s.on && !getenv("HK_PC64_WINE_SOUND");   // (tests: Wine's device)
    if (ours && FAILED(h)) {   // the device never plays the stream: the loader is the device all the same
        pc64_log("sound: the PC's device refused the game's format (%08lx, %08lx): through hiraku-pc all the same", (unsigned long)first,
                 (unsigned long)h);
        h = S_OK;
    }
    if (SUCCEEDED(h) && ours) {
        InitializeCriticalSection(&g_s.lock);
        g_s.on = 1;
        g_s.channels = f->nChannels, g_s.bytes = f->wBitsPerSample / 8, g_s.is_float = is_float;
        // the device's period: 2.5 ms (HK_PC64_PERIOD_US), which the game takes (GetDevicePeriod) and
        // mixes in; its buffer four periods (10 ms), which the game keeps full: the sound's latency
        const char *us = getenv("HK_PC64_PERIOD_US");
        g_period = (uint32_t)((us ? atoi(us) : 2500) * (long long)HK_PC_AUDIO_RATE / 1000000);
        if (g_period < 32) g_period = 32;
        if (g_period > BUF_FRAMES / 4) g_period = BUF_FRAMES / 4;
        g_s.size = 4 * g_period;
        start_mixer();
    }
    pc64_log("sound: %s, %u Hz, %u channels, %u bits%s, %lld ms%s: %s", m ? "exclusive" : "shared", (unsigned)f->nSamplesPerSec, f->nChannels,
             f->wBitsPerSample, is_float ? " float" : "", (long long)(d / 10000), flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK ? ", events" : "",
             !SUCCEEDED(h) ? "failed" : g_s.on ? "through hiraku-pc" : "on");
    return h;
}

// --- the loader as the device (g_s.on)
static HRESULT WINAPI GetBufferSize_(IAudioClient *c, UINT32 *n) {
    if (!g_s.on) return r_client.GetBufferSize(c, n);
    if (!n) return E_POINTER;
    *n = g_s.size;
    return S_OK;
}
static HRESULT WINAPI GetCurrentPadding_(IAudioClient *c, UINT32 *n) {
    if (!g_s.on) return r_client.GetCurrentPadding(c, n);
    if (!n) return E_POINTER;
    *n = g_s.written - g_s.played;
    return S_OK;
}
// The device's period: HK_PC64_PERIOD_US (tests), else the game's own (10 ms)
static HRESULT WINAPI GetDevicePeriod_(IAudioClient *c, REFERENCE_TIME *def, REFERENCE_TIME *min) {
    if (!g_s.on) return r_client.GetDevicePeriod(c, def, min);
    const REFERENCE_TIME p = (REFERENCE_TIME)g_period * 10000000 / HK_PC_AUDIO_RATE;
    if (def) *def = p;
    if (min) *min = p;
    return S_OK;
}
static HRESULT WINAPI SetEventHandle_(IAudioClient *c, HANDLE e) {
    if (!g_s.on) return r_client.SetEventHandle(c, e);
    g_s.event = e;
    return S_OK;
}
static HRESULT WINAPI Start_(IAudioClient *c) {
    if (!g_s.on) return r_client.Start(c);
    if (g_s.started) return AUDCLNT_E_NOT_STOPPED;
    EnterCriticalSection(&g_s.lock);
    g_s.started = 1;
    if (!g_clock_on) {
        QueryPerformanceFrequency(&g_freq);
        QueryPerformanceCounter(&g_t0);
        g_clock_on = 1;
    }
    LeaveCriticalSection(&g_s.lock);
    struct hk_pc_link *l = pc64_link();
    if (!l->sound_on) {
        l->sound_on = 1;
        pc64_log("sound: started, through hiraku-pc (%u frames' buffer)", g_s.size);
    }
    if (g_s.event) SetEvent(g_s.event);
    return S_OK;
}
static HRESULT WINAPI Stop_(IAudioClient *c) {
    if (!g_s.on) return r_client.Stop(c);
    const int was = g_s.started;
    g_s.started = 0;
    return was ? S_OK : S_FALSE;
}
static HRESULT WINAPI Reset_(IAudioClient *c) {
    if (!g_s.on) return r_client.Reset(c);
    if (g_s.started) return AUDCLNT_E_NOT_STOPPED;
    EnterCriticalSection(&g_s.lock);
    g_s.played = g_s.written;
    LeaveCriticalSection(&g_s.lock);
    return S_OK;
}

// The game's render client: GetBuffer gives it the staging area, ReleaseBuffer puts it into the buffer
static HRESULT WINAPI rc_QueryInterface(IAudioRenderClient *r, REFIID iid, void **out) {
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IAudioRenderClient)) {
        *out = r;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI rc_AddRef(IAudioRenderClient *r) { return (void)r, 2; }
static ULONG WINAPI rc_Release(IAudioRenderClient *r) { return (void)r, 1; }
static UINT32 g_asked;
static HRESULT WINAPI rc_GetBuffer(IAudioRenderClient *r, UINT32 n, BYTE **data) {
    (void)r;
    if (!data) return E_POINTER;
    if (n > g_s.size - (g_s.written - g_s.played)) return AUDCLNT_E_BUFFER_TOO_LARGE;
    g_asked = n;
    *data = g_s.staging;
    return S_OK;
}
static float sample(const BYTE *p) {
    if (g_s.is_float) return *(const float *)p;
    if (g_s.bytes == 2) return *(const int16_t *)p / 32768.0f;
    return (float)(*(const int32_t *)p / 2147483648.0);
}
static HRESULT WINAPI rc_ReleaseBuffer(IAudioRenderClient *r, UINT32 n, DWORD flags) {
    (void)r;
    if (n > g_asked) return AUDCLNT_E_INVALID_SIZE;
    EnterCriticalSection(&g_s.lock);
    const UINT32 frame = g_s.channels * g_s.bytes;
    for (UINT32 i = 0; i < n; i++) {
        float *o = g_s.fifo + 2 * ((g_s.written + i) & (BUF_FRAMES - 1));
        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
            o[0] = o[1] = 0.0f;
            continue;
        }
        const BYTE *in = g_s.staging + i * frame;
        o[0] = sample(in);
        o[1] = g_s.channels > 1 ? sample(in + g_s.bytes) : o[0];
    }
    if (getenv("HK_PC64_SOUND_DUMP")) {   // tests: what the game writes, as float stereo, to that file
        static FILE *dump;
        if (!dump) dump = fopen(getenv("HK_PC64_SOUND_DUMP"), "wb");
        for (UINT32 i = 0; dump && i < n; i++) fwrite(g_s.fifo + 2 * ((g_s.written + i) & (BUF_FRAMES - 1)), sizeof(float), 2, dump);
    }
    g_s.written += n;
    LeaveCriticalSection(&g_s.lock);
    g_asked = 0;
    if (getenv("HK_PC64_SOUND_LOG")) {   // tests: what the game writes, every 5 seconds
        static float peak;
        static UINT32 said;
        for (UINT32 i = 0; i < n; i++) {
            const float *o = g_s.fifo + 2 * ((g_s.written - n + i) & (BUF_FRAMES - 1));
            if (o[0] > peak || -o[0] > peak) peak = o[0] > 0 ? o[0] : -o[0];
        }
        if (g_s.written - said >= 5 * HK_PC_AUDIO_RATE) {
            struct hk_pc_link *l = pc64_link();
            pc64_log("sound: written %u, played %u, peak %.3f; ring made %u, taken %u; the game late %u times", g_s.written, g_s.played,
                     (double)peak, l->audio_made, l->audio_taken, g_late);
            said = g_s.written, peak = 0;
        }
    }
    return S_OK;
}
static IAudioRenderClientVtbl g_rc_vtbl = {rc_QueryInterface, rc_AddRef, rc_Release, rc_GetBuffer, rc_ReleaseBuffer};
static IAudioRenderClient g_rc = {&g_rc_vtbl};

// HK_PC64_SOUND_DUMP without hiraku-pc (tests): what the game writes to Wine's own device
static IAudioRenderClientVtbl r_wine_rc;
static BYTE *g_wine_data;
static HRESULT WINAPI wine_GetBuffer(IAudioRenderClient *r, UINT32 n, BYTE **data) {
    const HRESULT h = r_wine_rc.GetBuffer(r, n, data);
    g_wine_data = SUCCEEDED(h) && data ? *data : NULL;
    return h;
}
static HRESULT WINAPI wine_ReleaseBuffer(IAudioRenderClient *r, UINT32 n, DWORD flags) {
    static FILE *dump;
    if (!dump) dump = fopen(getenv("HK_PC64_SOUND_DUMP"), "wb");
    if (dump && g_wine_data && !(flags & AUDCLNT_BUFFERFLAGS_SILENT)) fwrite(g_wine_data, 8, n, dump);   // float stereo
    return r_wine_rc.ReleaseBuffer(r, n, flags);
}

static HRESULT WINAPI GetService_(IAudioClient *c, REFIID iid, void **out) {
    if (g_s.on && IsEqualIID(iid, &IID_IAudioRenderClient)) {
        if (!out) return E_POINTER;
        *out = &g_rc;
        return S_OK;
    }
    const HRESULT h = r_client.GetService(c, iid, out);
    if (SUCCEEDED(h) && out && *out && IsEqualIID(iid, &IID_IAudioRenderClient) && getenv("HK_PC64_SOUND_DUMP") && !r_wine_rc.GetBuffer) {
        IAudioRenderClientVtbl *v = ((IAudioRenderClient *)*out)->lpVtbl;
        r_wine_rc = *v;
        pc64_patch(&v->GetBuffer, wine_GetBuffer);
        pc64_patch(&v->ReleaseBuffer, wine_ReleaseBuffer);
    }
    return h;
}

// The device's clock: the PC's (QueryPerformanceCounter), 44.1 kHz from the stream's first Start,
// played a period at a time (the game's 10 ms): each period's frames go from the buffer into the
// link's ring, where hiraku-pc takes them, and the game's event says there is room, as a real
// device's. Whole periods matter: the game mixes in periods and stretches its mix to the space it
// is given, so space freed in other amounts (a frame's sound as hiraku-pc asked for it, or a
// millisecond's) made it resample its music all the time, its high notes a fizzle (a song through
// the game compared with the decoded .2dx, PLAN.md section 32). The device plays silence where the game is late,
// or stopped; frames the ring has no room for (hiraku-pc not taking them) are played and lost.

static uint32_t to_ring(void) {
    if (!g_clock_on) return 0;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    // whole periods, as a real device plays (WASAPI's shared mode: the game's 10 ms, 441 frames):
    // the game fills what each frees, and space of another size it fills by stretching its mix
    uint64_t due = (uint64_t)(now.QuadPart - g_t0.QuadPart) * HK_PC_AUDIO_RATE / (uint64_t)g_freq.QuadPart;
    due -= due % g_period;
    if (due <= g_clock) return 0;
    uint32_t n = due - g_clock > 8 * g_period ? 8 * g_period : (uint32_t)(due - g_clock);
    struct hk_pc_link *l = pc64_link();
    float *ring = (float *)((char *)l + HK_PC_AUDIO);
    const uint32_t made = l->audio_made, taken = l->audio_taken, room = HK_PC_AUDIO_FRAMES - (made - taken);
    const uint32_t have = g_s.written - g_s.played, from_game = !g_s.started ? 0 : n < have ? n : have;
    const uint32_t kept = n < room ? n : room;
    for (uint32_t i = 0; i < kept; i++) {
        const uint32_t at = (made + i) & (HK_PC_AUDIO_FRAMES - 1);
        if (i < from_game) {
            const float *f = g_s.fifo + 2 * ((g_s.played + i) & (BUF_FRAMES - 1));
            ring[2 * at] = f[0], ring[2 * at + 1] = f[1];
        } else {
            ring[2 * at] = ring[2 * at + 1] = 0.0f;
        }
    }
    if (g_s.started && from_game < n) g_late++;   // the game late: silence played
    g_s.played += from_game;
    g_clock += n;
    MemoryBarrier();
    l->audio_made = made + kept;
    if (g_s.started && g_s.event) SetEvent(g_s.event);   // a period played: room in the buffer
    return n;
}

// The device, from the stream's first Start
static DWORD WINAPI mixer(void *unused) {
    (void)unused;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    for (;;) {
        EnterCriticalSection(&g_s.lock);
        to_ring();
        LeaveCriticalSection(&g_s.lock);
        pc64_nap();
    }
    return 0;
}
static void start_mixer(void) { CreateThread(NULL, 0, mixer, NULL, 0, NULL); }

static void hook_device(IMMDevice *d);
static HRESULT WINAPI Activate_(IMMDevice *d, REFIID iid, DWORD ctx, PROPVARIANT *p, void **out) {
    HRESULT h = r_device.Activate(d, iid, ctx, p, out);
    if (SUCCEEDED(h) && IsEqualIID(iid, &IID_IAudioClient) && !r_client.Initialize) {
        IAudioClientVtbl *v = ((IAudioClient *)*out)->lpVtbl;
        r_client = *v;
        pc64_patch(&v->GetMixFormat, GetMixFormat_);
        pc64_patch(&v->Initialize, Initialize_);
        pc64_patch(&v->GetBufferSize, GetBufferSize_);
        pc64_patch(&v->GetCurrentPadding, GetCurrentPadding_);
        pc64_patch(&v->SetEventHandle, SetEventHandle_);
        pc64_patch(&v->GetDevicePeriod, GetDevicePeriod_);
        pc64_patch(&v->Start, Start_);
        pc64_patch(&v->Stop, Stop_);
        pc64_patch(&v->Reset, Reset_);
        pc64_patch(&v->GetService, GetService_);
    }
    return h;
}
static void hook_device(IMMDevice *d) {
    if (r_device.Activate) return;
    IMMDeviceVtbl *v = d->lpVtbl;
    r_device = *v;
    pc64_patch(&v->Activate, Activate_);
}
static HRESULT WINAPI GetDefaultAudioEndpoint_(IMMDeviceEnumerator *e, EDataFlow f, ERole r, IMMDevice **out) {
    HRESULT h = r_enumerator.GetDefaultAudioEndpoint(e, f, r, out);
    if (SUCCEEDED(h)) hook_device(*out);
    return h;
}
static HRESULT WINAPI GetDevice_(IMMDeviceEnumerator *e, LPCWSTR id, IMMDevice **out) {
    HRESULT h = r_enumerator.GetDevice(e, id, out);
    if (SUCCEEDED(h)) hook_device(*out);
    return h;
}
static HRESULT(WINAPI *r_CoCreateInstance)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID *);
static HRESULT WINAPI CoCreateInstance_(REFCLSID c, LPUNKNOWN outer, DWORD ctx, REFIID iid, LPVOID *out) {
    HRESULT h = r_CoCreateInstance(c, outer, ctx, iid, out);
    if (getenv("HK_PC64_SOUND_LOG"))
        pc64_log("CoCreateInstance {%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}: %08lx", (unsigned long)c->Data1, c->Data2, c->Data3,
                 c->Data4[0], c->Data4[1], c->Data4[2], c->Data4[3], c->Data4[4], c->Data4[5], c->Data4[6], c->Data4[7], (unsigned long)h);
    if (SUCCEEDED(h) && IsEqualCLSID(c, &CLSID_MMDeviceEnumerator) && !r_enumerator.GetDefaultAudioEndpoint) {
        IMMDeviceEnumeratorVtbl *v = ((IMMDeviceEnumerator *)*out)->lpVtbl;
        r_enumerator = *v;
        pc64_patch(&v->GetDefaultAudioEndpoint, GetDefaultAudioEndpoint_);
        pc64_patch(&v->GetDevice, GetDevice_);
    }
    return h;
}

void pc64_sound_install(HMODULE game) {
    r_CoCreateInstance = pc64_import(game, "ole32.dll", "CoCreateInstance", (void *)CoCreateInstance_);
    if (!r_CoCreateInstance) pc64_log("the game doesn't import CoCreateInstance: its sound as it is");
}
