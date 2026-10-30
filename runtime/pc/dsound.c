// SPDX-License-Identifier: GPL-3.0-or-later
// HIRAKU's DirectSound for the PC games (PLAN.md section 30): when HIRAKU's window shows the game
// (hiraku-pc's link), the game's DirectSoundCreate8 gets this instead of Windows' (or Wine's). Its
// sound buffers are mixed here, as far as hiraku-pc asks (runtime/pc/link.h: 735 frames a frame
// shown), into the link's ring, and hiraku-pc plays them through HIRAKU's sound output with its
// device, latency and pacing: the game's sound in step with its pictures, as an emulated machine's.
//
// What pop'n music 15 uses (the DirectX SDK's CSoundManager / CStreamingSound): the device, a
// primary buffer whose format is set, secondary PCM buffers (duplicated for overlapping plays) with
// volume, pan and frequency, and position notifications for the streamed music. 3D sound and
// property sets are refused (and logged): the game has them in its code but plays in stereo.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmreg.h>
#ifndef _LPCWAVEFORMATEX_DEFINED   // older mingw-w64 (Ubuntu 22.04's 8.0) lacks it where dsound.h wants it
#define _LPCWAVEFORMATEX_DEFINED
typedef const WAVEFORMATEX *LPCWAVEFORMATEX;
#endif
#include <dsound.h>
#include <mmsystem.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cabinet.h"

#define OUT_RATE HK_PC_AUDIO_RATE

// The sample data, shared by a buffer and its duplicates
struct data {
    LONG refs;
    uint8_t *bytes;
    DWORD size;
};

struct notify_pos {
    DWORD offset;   // DSBPN_OFFSETSTOP: when it stops
    HANDLE event;
};

struct buf {
    void **vt;          // IDirectSoundBuffer8
    void **notify_vt;   // IDirectSoundNotify, the same object
    LONG refs;
    int primary;
    struct data *data;
    WAVEFORMATEX fmt;
    int bytes_per_sample, channels;
    double pos;         // the play position, in sample frames (fractional: the frequency)
    int playing, looping;
    int played;         // (HK_PC_DSOUND_LOG: written again after it played)
    LONG volume, pan;   // hundredths of a dB; -10000 (left) .. 10000 (right)
    DWORD freq;         // 0: the format's
    struct notify_pos *notes;
    DWORD n_notes;
    struct buf *next;   // the mixer's list
};

static CRITICAL_SECTION g_lock;
static struct buf *g_bufs;
static volatile int g_started;

static void lock(void) { EnterCriticalSection(&g_lock); }
static void unlock(void) { LeaveCriticalSection(&g_lock); }

static int same_iid(REFIID a, const GUID *b) { return !memcmp(a, b, sizeof(GUID)); }

// HK_PC_DSOUND_LOG: the buffers made, and what the game asks of them that HIRAKU's DirectSound
// doesn't do (effects, other interfaces)
static int dlog(void) {
    static int on = -1;
    if (on < 0) on = getenv("HK_PC_DSOUND_LOG") != NULL;
    return on;
}
static const char *guid_text(const GUID *g, char *b) {
    sprintf(b, "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", g->Data1, g->Data2, g->Data3, g->Data4[0], g->Data4[1],
            g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
    return b;
}

// ---------------------------------------------------------------- the mixer

static double gain_of(LONG hundredths_db) { return hundredths_db <= -10000 ? 0.0 : pow(10.0, hundredths_db / 2000.0); }

// A sample frame of the buffer, -1..1, at index i (whole frames)
static void frame_at(const struct buf *b, DWORD i, float *l, float *r) {
    const uint8_t *p = b->data->bytes + (size_t)i * b->fmt.nBlockAlign;
    float s0, s1;
    if (b->bytes_per_sample == 1) {
        s0 = (p[0] - 128) / 128.0f;
        s1 = b->channels > 1 ? (p[1] - 128) / 128.0f : s0;
    } else if (b->bytes_per_sample == 2) {
        s0 = (int16_t)(p[0] | p[1] << 8) / 32768.0f;
        s1 = b->channels > 1 ? (int16_t)(p[2] | p[3] << 8) / 32768.0f : s0;
    } else {   // 32-bit float
        memcpy(&s0, p, 4);
        if (b->channels > 1) memcpy(&s1, p + 4, 4);
        else s1 = s0;
    }
    *l = s0, *r = s1;
}

static void notify_between(struct buf *b, DWORD from_byte, DWORD to_byte, int wrapped) {
    for (DWORD k = 0; k < b->n_notes; k++) {
        const DWORD o = b->notes[k].offset;
        if (o == (DWORD)DSBPN_OFFSETSTOP) continue;
        const int hit = wrapped ? (o >= from_byte || o < to_byte) : (o >= from_byte && o < to_byte);
        if (hit && b->notes[k].event) SetEvent(b->notes[k].event);
    }
}

static void notify_stop(struct buf *b) {
    for (DWORD k = 0; k < b->n_notes; k++)
        if (b->notes[k].offset == (DWORD)DSBPN_OFFSETSTOP && b->notes[k].event) SetEvent(b->notes[k].event);
}

// n frames of every playing buffer, added into out (stereo)
static void mix(float *out, int n) {
    memset(out, 0, sizeof(float) * 2 * (size_t)n);
    for (struct buf *b = g_bufs; b; b = b->next) {
        if (!b->playing || b->primary || !b->data || !b->fmt.nBlockAlign) continue;
        const DWORD frames = b->data->size / b->fmt.nBlockAlign;
        if (!frames) continue;
        const double step = (double)(b->freq ? b->freq : b->fmt.nSamplesPerSec) / OUT_RATE;
        const double g = gain_of(b->volume);
        const double gl = g * (b->pan > 0 ? gain_of(-b->pan) : 1.0), gr = g * (b->pan < 0 ? gain_of(b->pan) : 1.0);
        const DWORD from_byte = (DWORD)b->pos * b->fmt.nBlockAlign;
        int wrapped = 0, ended = 0;
        for (int i = 0; i < n; i++) {
            const DWORD i0 = (DWORD)b->pos;
            DWORD i1 = i0 + 1;
            if (i1 >= frames) i1 = b->looping ? 0 : i0;
            const float t = (float)(b->pos - i0);
            float l0, r0, l1, r1;
            frame_at(b, i0, &l0, &r0);
            frame_at(b, i1, &l1, &r1);
            out[2 * i] += (float)gl * (l0 + (l1 - l0) * t);
            out[2 * i + 1] += (float)gr * (r0 + (r1 - r0) * t);
            b->pos += step;
            if (b->pos >= frames) {
                if (b->looping) {
                    b->pos -= frames * (double)(DWORD)(b->pos / frames);
                    wrapped = 1;
                } else {
                    ended = 1;
                    break;
                }
            }
        }
        if (ended) {
            notify_between(b, from_byte, b->data->size, 0);
            b->playing = 0;
            b->pos = 0;
            notify_stop(b);
        } else {
            notify_between(b, from_byte, (DWORD)b->pos * b->fmt.nBlockAlign, wrapped);
        }
    }
}

// What audio_want asks for and the ring has room for, mixed into the ring (with g_lock held: the
// mixer thread and a picture's Present both mix). The frames mixed
static uint32_t mix_to_want(void) {
    struct hk_pc_link *l = hk_link();
    float *ring = (float *)((char *)l + HK_PC_AUDIO);
    float chunk[2 * 512];
    uint32_t done = 0;
    for (;;) {
        const uint32_t want = l->audio_want, made = l->audio_made, taken = l->audio_taken;
        uint32_t n = want - made;
        const uint32_t room = HK_PC_AUDIO_FRAMES - (made - taken);
        if (n > room) n = room;
        if (n > 512) n = 512;
        if (!n || (int32_t)(want - made) < 0) return done;
        mix(chunk, (int)n);
        for (uint32_t i = 0; i < n; i++) {
            const uint32_t at = (made + i) & (HK_PC_AUDIO_FRAMES - 1);
            ring[2 * at] = chunk[2 * i];
            ring[2 * at + 1] = chunk[2 * i + 1];
        }
        MemoryBarrier();
        l->audio_made = made + n;
        done += n;
    }
}

// As far as hiraku-pc asks, into the ring; a millisecond's nap when it has all it asked for
static DWORD WINAPI mixer_thread(LPVOID unused) {
    (void)unused;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    for (;;) {
        lock();
        const uint32_t n = mix_to_want();
        unlock();
        if (!n) Sleep(1);
    }
    return 0;
}

void hk_dsound_picture(void) {
    struct hk_pc_link *l = hk_link();
    const uint32_t end = l->audio_frame_end;
    if (!g_started || !end) return;
    lock();
    for (;;) {   // audio_want moved on to `end` (hiraku-pc may move it too, never back)
        const uint32_t want = l->audio_want;
        if ((int32_t)(end - want) <= 0 ||
            (uint32_t)InterlockedCompareExchange((volatile LONG *)&l->audio_want, (LONG)end, (LONG)want) == want)
            break;
    }
    mix_to_want();
    unlock();
}

// ---------------------------------------------------------------- IDirectSoundBuffer8, IDirectSoundNotify

static void **g_buf_vt, **g_notify_vt;

static struct buf *new_buf(void) {
    struct buf *b = (struct buf *)calloc(1, sizeof *b);
    b->vt = g_buf_vt;
    b->notify_vt = g_notify_vt;
    b->refs = 1;
    lock();
    b->next = g_bufs;
    g_bufs = b;
    unlock();
    return b;
}

static void set_format(struct buf *b, const WAVEFORMATEX *f) {
    b->fmt = *f;
    b->fmt.cbSize = 0;
    b->channels = f->nChannels ? f->nChannels : 1;
    b->bytes_per_sample = f->wBitsPerSample / 8;
    if (b->bytes_per_sample < 1 || b->bytes_per_sample > 4 || b->bytes_per_sample == 3) b->bytes_per_sample = 2;
    if (!b->fmt.nBlockAlign) b->fmt.nBlockAlign = (WORD)(b->channels * b->bytes_per_sample);
}

static HRESULT WINAPI buf_QueryInterface(struct buf *b, REFIID iid, void **out) {
    if (same_iid(iid, &IID_IUnknown) || same_iid(iid, &IID_IDirectSoundBuffer) || same_iid(iid, &IID_IDirectSoundBuffer8)) {
        *out = b;
    } else if (same_iid(iid, &IID_IDirectSoundNotify) && !b->primary) {
        *out = &b->notify_vt;
    } else {
        static int said;
        char g[40];
        if (!said++ || dlog()) hk_log("hiraku-pc: the game asked a sound buffer for an interface HIRAKU's DirectSound doesn't have (%s)", guid_text(iid, g));
        *out = NULL;
        return E_NOINTERFACE;
    }
    InterlockedIncrement(&b->refs);
    return S_OK;
}

static ULONG WINAPI buf_AddRef(struct buf *b) { return (ULONG)InterlockedIncrement(&b->refs); }

static ULONG WINAPI buf_Release(struct buf *b) {
    const LONG r = InterlockedDecrement(&b->refs);
    if (r) return (ULONG)r;
    lock();
    for (struct buf **p = &g_bufs; *p; p = &(*p)->next)
        if (*p == b) {
            *p = b->next;
            break;
        }
    unlock();
    if (b->data && !InterlockedDecrement(&b->data->refs)) {
        free(b->data->bytes);
        free(b->data);
    }
    free(b->notes);
    free(b);
    return 0;
}

static HRESULT WINAPI buf_GetCaps(struct buf *b, DSBCAPS *c) {
    if (!c) return DSERR_INVALIDPARAM;
    memset(c, 0, sizeof *c);
    c->dwSize = sizeof *c;
    c->dwFlags = b->primary ? DSBCAPS_PRIMARYBUFFER : DSBCAPS_LOCSOFTWARE | DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLPAN | DSBCAPS_CTRLFREQUENCY;
    c->dwBufferBytes = b->data ? b->data->size : 0;
    return DS_OK;
}

// The write cursor: a frame shown's worth (735 frames at 44.1 kHz) past the play cursor, as the
// mixer may already have taken that
static HRESULT WINAPI buf_GetCurrentPosition(struct buf *b, DWORD *play, DWORD *write) {
    lock();
    DWORD p = 0, w = 0;
    if (b->data && b->fmt.nBlockAlign) {
        const DWORD size = b->data->size;
        p = ((DWORD)b->pos * b->fmt.nBlockAlign) % (size ? size : 1);
        const double step = (double)(b->freq ? b->freq : b->fmt.nSamplesPerSec) / OUT_RATE;
        w = b->playing ? (p + (DWORD)(735 * step) * b->fmt.nBlockAlign) % (size ? size : 1) : p;
    }
    unlock();
    if (play) *play = p;
    if (write) *write = w;
    return DS_OK;
}

static HRESULT WINAPI buf_GetFormat(struct buf *b, WAVEFORMATEX *f, DWORD size, DWORD *written) {
    if (written) *written = sizeof(WAVEFORMATEX);
    if (f) memcpy(f, &b->fmt, size < sizeof(WAVEFORMATEX) ? size : sizeof(WAVEFORMATEX));
    return DS_OK;
}

static HRESULT WINAPI buf_GetVolume(struct buf *b, LONG *v) { *v = b->volume; return DS_OK; }
static HRESULT WINAPI buf_GetPan(struct buf *b, LONG *v) { *v = b->pan; return DS_OK; }
static HRESULT WINAPI buf_GetFrequency(struct buf *b, DWORD *v) { *v = b->freq ? b->freq : b->fmt.nSamplesPerSec; return DS_OK; }

static HRESULT WINAPI buf_GetStatus(struct buf *b, DWORD *s) {
    lock();
    *s = (b->playing ? DSBSTATUS_PLAYING : 0) | (b->playing && b->looping ? DSBSTATUS_LOOPING : 0);
    unlock();
    return DS_OK;
}

static HRESULT WINAPI buf_Initialize(struct buf *b, void *ds, const DSBUFFERDESC *d) { (void)b, (void)ds, (void)d; return DSERR_ALREADYINITIALIZED; }

static HRESULT WINAPI buf_Lock(struct buf *b, DWORD offset, DWORD bytes, void **p1, DWORD *n1, void **p2, DWORD *n2, DWORD flags) {
    if (!b->data || !b->data->size) return DSERR_INVALIDCALL;
    const DWORD size = b->data->size;
    if (flags & DSBLOCK_FROMWRITECURSOR) buf_GetCurrentPosition(b, NULL, &offset);
    if (flags & DSBLOCK_ENTIREBUFFER) bytes = size;
    if (offset >= size || bytes > size) return DSERR_INVALIDPARAM;
    const DWORD first = offset + bytes > size ? size - offset : bytes;
    static int said;
    if (b->played && dlog() && said++ < 300) hk_log("dsound: buffer %p written again after it played: %lu bytes at %lu", (void *)b, bytes, offset);
    *p1 = b->data->bytes + offset;
    *n1 = first;
    if (p2) *p2 = first < bytes ? b->data->bytes : NULL;
    if (n2) *n2 = bytes - first;
    return DS_OK;
}

// A button went down (link.h: key_edges, counted where the game reads its pad, maybe in the other
// DLL): the next sound the game starts is taken for its sound (key_sound_at)
static uint32_t g_key_seen;

static HRESULT WINAPI buf_Play(struct buf *b, DWORD r1, DWORD prio, DWORD flags) {
    (void)r1, (void)prio;
    lock();
    struct hk_pc_link *l = hk_link();
    if (l->key_edges != g_key_seen) {   // it starts at the ring's next frame (the mixing holds the lock)
        g_key_seen = l->key_edges;
        l->key_sound_at = l->audio_made;
        MemoryBarrier();
        l->key_sound_seq = l->key_sound_seq + 1;
    }
    b->playing = 1;
    b->played = 1;
    b->looping = (flags & DSBPLAY_LOOPING) != 0;
    unlock();
    return DS_OK;
}

static HRESULT WINAPI buf_SetCurrentPosition(struct buf *b, DWORD pos) {
    lock();
    if (b->fmt.nBlockAlign && b->data && pos < b->data->size) b->pos = pos / b->fmt.nBlockAlign;
    unlock();
    return DS_OK;
}

static HRESULT WINAPI buf_SetFormat(struct buf *b, const WAVEFORMATEX *f) {
    if (!f) return DSERR_INVALIDPARAM;
    if (b->primary) hk_log("hiraku-pc: the game's sound: %lu Hz, %u bits, %u channels (mixed at %u Hz)", f->nSamplesPerSec,
                           f->wBitsPerSample, f->nChannels, OUT_RATE);
    set_format(b, f);
    return DS_OK;
}

static HRESULT WINAPI buf_SetVolume(struct buf *b, LONG v) {
    b->volume = v < DSBVOLUME_MIN ? DSBVOLUME_MIN : v > DSBVOLUME_MAX ? DSBVOLUME_MAX : v;
    return DS_OK;
}
static HRESULT WINAPI buf_SetPan(struct buf *b, LONG v) {
    b->pan = v < DSBPAN_LEFT ? DSBPAN_LEFT : v > DSBPAN_RIGHT ? DSBPAN_RIGHT : v;
    return DS_OK;
}
static HRESULT WINAPI buf_SetFrequency(struct buf *b, DWORD v) {
    if (dlog() && (v == DSBFREQUENCY_ORIGINAL ? 0 : v) != b->freq) hk_log("dsound: buffer %p at %lu Hz", (void *)b, v);
    b->freq = v == DSBFREQUENCY_ORIGINAL ? 0 : v;
    return DS_OK;
}

static HRESULT WINAPI buf_Stop(struct buf *b) {
    lock();
    const int was = b->playing;
    b->playing = 0;
    if (was) notify_stop(b);
    unlock();
    return DS_OK;
}

static HRESULT WINAPI buf_Unlock(struct buf *b, void *p1, DWORD n1, void *p2, DWORD n2) { (void)b, (void)p1, (void)n1, (void)p2, (void)n2; return DS_OK; }
static HRESULT WINAPI buf_Restore(struct buf *b) { (void)b; return DS_OK; }
static HRESULT WINAPI buf_SetFX(struct buf *b, DWORD n, void *d, DWORD *r) {
    (void)b, (void)r;
    char g[40];
    for (DWORD k = 0; dlog() && k < n; k++) hk_log("dsound: SetFX %lu: %s", k, guid_text(&((const DSEFFECTDESC *)d)[k].guidDSFXClass, g));
    return DSERR_CONTROLUNAVAIL;
}
static HRESULT WINAPI buf_AcquireResources(struct buf *b, DWORD f, DWORD n, DWORD *r) { (void)b, (void)f, (void)n, (void)r; return DS_OK; }
static HRESULT WINAPI buf_GetObjectInPath(struct buf *b, REFGUID o, DWORD i, REFGUID iid, void **out) {
    (void)b;
    char g[40], h[40];
    if (dlog()) hk_log("dsound: GetObjectInPath %s %lu %s", guid_text(o, g), i, guid_text(iid, h));
    if (out) *out = NULL;
    return DSERR_OBJECTNOTFOUND;
}

// IDirectSoundNotify: the same buffer, its second interface pointer
static struct buf *of_notify(void *n) { return (struct buf *)((char *)n - offsetof(struct buf, notify_vt)); }
static HRESULT WINAPI notify_QueryInterface(void *n, REFIID iid, void **out) { return buf_QueryInterface(of_notify(n), iid, out); }
static ULONG WINAPI notify_AddRef(void *n) { return buf_AddRef(of_notify(n)); }
static ULONG WINAPI notify_Release(void *n) { return buf_Release(of_notify(n)); }
static HRESULT WINAPI notify_SetNotificationPositions(void *n, DWORD count, const DSBPOSITIONNOTIFY *pos) {
    struct buf *b = of_notify(n);
    lock();
    free(b->notes);
    b->notes = count ? (struct notify_pos *)calloc(count, sizeof *b->notes) : NULL;
    b->n_notes = count;
    for (DWORD k = 0; k < count; k++) b->notes[k].offset = pos[k].dwOffset, b->notes[k].event = pos[k].hEventNotify;
    unlock();
    return DS_OK;
}

static void *g_buf_methods[] = {
    (void *)buf_QueryInterface, (void *)buf_AddRef, (void *)buf_Release, (void *)buf_GetCaps, (void *)buf_GetCurrentPosition,
    (void *)buf_GetFormat, (void *)buf_GetVolume, (void *)buf_GetPan, (void *)buf_GetFrequency, (void *)buf_GetStatus,
    (void *)buf_Initialize, (void *)buf_Lock, (void *)buf_Play, (void *)buf_SetCurrentPosition, (void *)buf_SetFormat,
    (void *)buf_SetVolume, (void *)buf_SetPan, (void *)buf_SetFrequency, (void *)buf_Stop, (void *)buf_Unlock,
    (void *)buf_Restore, (void *)buf_SetFX, (void *)buf_AcquireResources, (void *)buf_GetObjectInPath};
static void *g_notify_methods[] = {(void *)notify_QueryInterface, (void *)notify_AddRef, (void *)notify_Release,
                                   (void *)notify_SetNotificationPositions};

// ---------------------------------------------------------------- IDirectSound8

struct ds {
    void **vt;
    LONG refs;
};

static HRESULT WINAPI ds_QueryInterface(struct ds *d, REFIID iid, void **out) {
    if (same_iid(iid, &IID_IUnknown) || same_iid(iid, &IID_IDirectSound) || same_iid(iid, &IID_IDirectSound8)) {
        *out = d;
        InterlockedIncrement(&d->refs);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI ds_AddRef(struct ds *d) { return (ULONG)InterlockedIncrement(&d->refs); }
static ULONG WINAPI ds_Release(struct ds *d) {
    const LONG r = InterlockedDecrement(&d->refs);
    if (!r) free(d);
    return (ULONG)r;
}

static HRESULT WINAPI ds_CreateSoundBuffer(struct ds *d, const DSBUFFERDESC *desc, struct buf **out, IUnknown *outer) {
    (void)d, (void)outer;
    if (!desc || !out) return DSERR_INVALIDPARAM;
    struct buf *b = new_buf();
    if (desc->dwFlags & DSBCAPS_PRIMARYBUFFER) {
        b->primary = 1;
        WAVEFORMATEX f = {WAVE_FORMAT_PCM, 2, OUT_RATE, OUT_RATE * 4, 4, 16, 0};
        set_format(b, &f);
    } else {
        if (!desc->lpwfxFormat || !desc->dwBufferBytes) {
            buf_Release(b);
            return DSERR_INVALIDPARAM;
        }
        set_format(b, desc->lpwfxFormat);
        if (dlog())
            hk_log("dsound: buffer %p, %lu bytes, format %u, %lu Hz, %u bits, %u channels, flags %#lx", (void *)b, desc->dwBufferBytes,
                   desc->lpwfxFormat->wFormatTag, desc->lpwfxFormat->nSamplesPerSec, desc->lpwfxFormat->wBitsPerSample,
                   desc->lpwfxFormat->nChannels, desc->dwFlags);
        b->data = (struct data *)calloc(1, sizeof *b->data);
        b->data->refs = 1;
        b->data->size = desc->dwBufferBytes;
        b->data->bytes = (uint8_t *)calloc(1, desc->dwBufferBytes);
        if (b->bytes_per_sample == 1) memset(b->data->bytes, 0x80, desc->dwBufferBytes);
    }
    *out = b;
    return DS_OK;
}

static HRESULT WINAPI ds_GetCaps(struct ds *d, DSCAPS *c) {
    (void)d;
    if (!c) return DSERR_INVALIDPARAM;
    const DWORD size = c->dwSize ? c->dwSize : sizeof *c;
    memset(c, 0, size);
    c->dwSize = size;
    c->dwFlags = DSCAPS_PRIMARYSTEREO | DSCAPS_PRIMARY16BIT | DSCAPS_SECONDARYSTEREO | DSCAPS_SECONDARY16BIT |
                 DSCAPS_SECONDARYMONO | DSCAPS_SECONDARY8BIT | DSCAPS_PRIMARYMONO | DSCAPS_PRIMARY8BIT;
    c->dwMinSecondarySampleRate = 100;
    c->dwMaxSecondarySampleRate = 200000;
    c->dwPrimaryBuffers = 1;
    return DS_OK;
}

static HRESULT WINAPI ds_DuplicateSoundBuffer(struct ds *d, struct buf *src, struct buf **out) {
    (void)d;
    if (!src || !out || src->primary || !src->data) return DSERR_INVALIDPARAM;
    struct buf *b = new_buf();
    lock();
    b->fmt = src->fmt;
    b->channels = src->channels, b->bytes_per_sample = src->bytes_per_sample;
    b->volume = src->volume, b->pan = src->pan, b->freq = src->freq;
    b->data = src->data;
    InterlockedIncrement(&b->data->refs);
    unlock();
    *out = b;
    return DS_OK;
}

static HRESULT WINAPI ds_SetCooperativeLevel(struct ds *d, HWND w, DWORD level) { (void)d, (void)w, (void)level; return DS_OK; }
static HRESULT WINAPI ds_Compact(struct ds *d) { (void)d; return DS_OK; }
static HRESULT WINAPI ds_GetSpeakerConfig(struct ds *d, DWORD *c) {
    (void)d;
    *c = DSSPEAKER_COMBINED(DSSPEAKER_STEREO, DSSPEAKER_GEOMETRY_WIDE);
    return DS_OK;
}
static HRESULT WINAPI ds_SetSpeakerConfig(struct ds *d, DWORD c) { (void)d, (void)c; return DS_OK; }
static HRESULT WINAPI ds_Initialize(struct ds *d, const GUID *g) { (void)d, (void)g; return DS_OK; }
static HRESULT WINAPI ds_VerifyCertification(struct ds *d, DWORD *c) {
    (void)d;
    *c = DS_CERTIFIED;
    return DS_OK;
}

static void *g_ds_methods[] = {(void *)ds_QueryInterface, (void *)ds_AddRef, (void *)ds_Release, (void *)ds_CreateSoundBuffer,
                               (void *)ds_GetCaps, (void *)ds_DuplicateSoundBuffer, (void *)ds_SetCooperativeLevel,
                               (void *)ds_Compact, (void *)ds_GetSpeakerConfig, (void *)ds_SetSpeakerConfig,
                               (void *)ds_Initialize, (void *)ds_VerifyCertification};

long __stdcall hk_DirectSoundCreate8(const void *device, void **out, void *outer) {
    (void)device, (void)outer;
    if (!out) return DSERR_INVALIDPARAM;
    if (!g_started) {
        InitializeCriticalSection(&g_lock);
        g_buf_vt = g_buf_methods;
        g_notify_vt = g_notify_methods;
        MemoryBarrier();
        g_started = 1;   // (a picture's Present mixes from now on)
        hk_link()->sound_on = 1;
        CloseHandle(CreateThread(NULL, 0, mixer_thread, NULL, 0, NULL));
        hk_log("hiraku-pc: the game's sound goes to HIRAKU (its own DirectSound, mixed at %u Hz)", OUT_RATE);
    }
    struct ds *d = (struct ds *)calloc(1, sizeof *d);
    d->vt = g_ds_methods;
    d->refs = 1;
    *out = d;
    return DS_OK;
}

long __stdcall hk_DirectSoundCreate(const void *device, void **out, void *outer) { return hk_DirectSoundCreate8(device, out, outer); }

// ---------------------------------------------------------------- the mixer
// With HIRAKU's sound the game's winmm mixer calls get a pretend mixer: one destination, the
// speakers, with a volume and a mute control. pop'n music 15-18 look for the speakers' mute and
// unmute it before starting DirectSound, and without it start none (and fault at the first sound
// played): Windows' mixer has no speakers line on a PC whose output is HDMI, digital or headphones
// (pop'n 17 on Windows 10, GitHub issue 4). The game also sets the speakers' volume to its own
// setting; here the values are kept, not applied (HIRAKU's sound output has its own), and the PC's
// own volume and mute are left alone. The speakers have one source, the wave out (no controls of its
// own), as a PC's always have some: IIDX RED counts its way through the speakers' sources up to their
// number less one, unsigned, and with none went round some 4 billion times before starting.

#define MIX_HANDLE ((HMIXER)(uintptr_t)0x4b4d4958)   // "XIMK"
#define MIX_LINE 0xffff0000u
#define MIX_SOURCE 0x00000000u   // the speakers' source 0 (its id as Windows makes them: source << 16)
enum { MIX_VOLUME = 1, MIX_MUTE = 2 };
static DWORD g_mix_value[3] = {0, 65535, 0};

static int mix_ours(HMIXEROBJ h, DWORD flags) {   // mixer 0 or the pretend handle, as the flags say
    switch (flags & 0xf0000000u) {
    case MIXER_OBJECTF_MIXER: return (uintptr_t)h == 0 || h == (HMIXEROBJ)MIX_HANDLE;   // (games pass the handle as an id)
    case MIXER_OBJECTF_HMIXER: return h == (HMIXEROBJ)MIX_HANDLE;
    default: return 0;
    }
}
static void mix_line(MIXERLINEA *l) {
    const DWORD cb = l->cbStruct;
    memset(l, 0, cb < sizeof *l ? cb : sizeof *l);
    l->cbStruct = cb;
    l->dwDestination = 0;
    l->dwLineID = MIX_LINE;
    l->fdwLine = MIXERLINE_LINEF_ACTIVE;
    l->dwComponentType = MIXERLINE_COMPONENTTYPE_DST_SPEAKERS;
    l->cChannels = 1;
    l->cConnections = 1;
    l->cControls = 2;
    strcpy(l->szShortName, "Speakers");
    strcpy(l->szName, "Speakers");
    l->Target.dwType = MIXERLINE_TARGETTYPE_WAVEOUT;
    strcpy(l->Target.szPname, "HIRAKU");
}
static void mix_source(MIXERLINEA *l) {
    const DWORD cb = l->cbStruct;
    memset(l, 0, cb < sizeof *l ? cb : sizeof *l);
    l->cbStruct = cb;
    l->dwDestination = 0;
    l->dwSource = 0;
    l->dwLineID = MIX_SOURCE;
    l->fdwLine = MIXERLINE_LINEF_ACTIVE | MIXERLINE_LINEF_SOURCE;
    l->dwComponentType = MIXERLINE_COMPONENTTYPE_SRC_WAVEOUT;
    l->cChannels = 1;
    strcpy(l->szShortName, "Wave");
    strcpy(l->szName, "Wave");
    l->Target.dwType = MIXERLINE_TARGETTYPE_WAVEOUT;
    strcpy(l->Target.szPname, "HIRAKU");
}
static void mix_control(MIXERCONTROLA *c, DWORD id) {
    memset(c, 0, sizeof *c);
    c->cbStruct = sizeof *c;
    c->dwControlID = id;
    c->dwControlType = id == MIX_VOLUME ? MIXERCONTROL_CONTROLTYPE_VOLUME : MIXERCONTROL_CONTROLTYPE_MUTE;
    c->fdwControl = MIXERCONTROL_CONTROLF_UNIFORM;
    strcpy(c->szShortName, id == MIX_VOLUME ? "Volume" : "Mute");
    strcpy(c->szName, id == MIX_VOLUME ? "Volume" : "Mute");
    c->Bounds.dwMinimum = 0;
    c->Bounds.dwMaximum = id == MIX_VOLUME ? 65535 : 1;
    c->Metrics.cSteps = id == MIX_VOLUME ? 65536 : 2;
}

static UINT WINAPI mix_GetNumDevs(void) { return 1; }
static MMRESULT WINAPI mix_Open(LPHMIXER out, UINT id, DWORD_PTR cb, DWORD_PTR inst, DWORD flags) {
    (void)cb, (void)inst, (void)flags;
    if (!out) return MMSYSERR_INVALPARAM;
    if (id != 0 && id != (UINT)(uintptr_t)MIX_HANDLE) return MMSYSERR_BADDEVICEID;
    *out = MIX_HANDLE;
    static int said;
    if (!said++) hk_log("hiraku-pc: the game's mixer is HIRAKU's (its speakers' volume and mute kept here, the PC's left alone)");
    return MMSYSERR_NOERROR;
}
static MMRESULT WINAPI mix_Close(HMIXER h) { return h == MIX_HANDLE ? MMSYSERR_NOERROR : MMSYSERR_INVALHANDLE; }
static MMRESULT WINAPI mix_GetDevCapsA(UINT_PTR id, LPMIXERCAPSA caps, UINT size) {
    if (id != 0 && id != (UINT_PTR)MIX_HANDLE) return MMSYSERR_BADDEVICEID;
    if (!caps || size < sizeof(MIXERCAPSA)) return MMSYSERR_INVALPARAM;
    memset(caps, 0, sizeof *caps);
    caps->wMid = 1;
    caps->wPid = 1;
    strcpy(caps->szPname, "HIRAKU");
    caps->cDestinations = 1;
    return MMSYSERR_NOERROR;
}
static MMRESULT WINAPI mix_GetLineInfoA(HMIXEROBJ h, LPMIXERLINEA l, DWORD flags) {
    if (!mix_ours(h, flags)) return MMSYSERR_INVALHANDLE;
    if (!l || l->cbStruct < sizeof *l) return MMSYSERR_INVALPARAM;
    switch (flags & MIXER_GETLINEINFOF_QUERYMASK) {
    case MIXER_GETLINEINFOF_DESTINATION:
        if (l->dwDestination != 0) return MIXERR_INVALLINE;
        break;
    case MIXER_GETLINEINFOF_SOURCE:
        if (l->dwDestination != 0 || l->dwSource != 0) return MIXERR_INVALLINE;
        mix_source(l);
        return MMSYSERR_NOERROR;
    case MIXER_GETLINEINFOF_LINEID:
        if (l->dwLineID == MIX_SOURCE) {
            mix_source(l);
            return MMSYSERR_NOERROR;
        }
        if (l->dwLineID != MIX_LINE) return MIXERR_INVALLINE;
        break;
    case MIXER_GETLINEINFOF_COMPONENTTYPE:
        if (l->dwComponentType == MIXERLINE_COMPONENTTYPE_SRC_WAVEOUT) {
            mix_source(l);
            return MMSYSERR_NOERROR;
        }
        if (l->dwComponentType != MIXERLINE_COMPONENTTYPE_DST_SPEAKERS) return MIXERR_INVALLINE;
        break;
    default:   // by target
        return MIXERR_INVALLINE;
    }
    mix_line(l);
    return MMSYSERR_NOERROR;
}
static MMRESULT WINAPI mix_GetLineControlsA(HMIXEROBJ h, LPMIXERLINECONTROLSA lc, DWORD flags) {
    if (!mix_ours(h, flags)) return MMSYSERR_INVALHANDLE;
    if (!lc || lc->cbStruct < sizeof *lc || !lc->pamxctrl || lc->cbmxctrl < sizeof(MIXERCONTROLA)) return MMSYSERR_INVALPARAM;
    switch (flags & MIXER_GETLINECONTROLSF_QUERYMASK) {
    case MIXER_GETLINECONTROLSF_ALL:
        if (lc->dwLineID != MIX_LINE) return MIXERR_INVALLINE;
        if (lc->cControls < 2) return MMSYSERR_INVALPARAM;
        mix_control(lc->pamxctrl, MIX_VOLUME);
        mix_control((MIXERCONTROLA *)((char *)lc->pamxctrl + lc->cbmxctrl), MIX_MUTE);
        return MMSYSERR_NOERROR;
    case MIXER_GETLINECONTROLSF_ONEBYID:
        if (lc->dwControlID != MIX_VOLUME && lc->dwControlID != MIX_MUTE) return MIXERR_INVALCONTROL;
        mix_control(lc->pamxctrl, lc->dwControlID);
        lc->dwLineID = MIX_LINE;
        return MMSYSERR_NOERROR;
    case MIXER_GETLINECONTROLSF_ONEBYTYPE:
        if (lc->dwLineID != MIX_LINE) return MIXERR_INVALLINE;
        if (lc->dwControlType == MIXERCONTROL_CONTROLTYPE_VOLUME) mix_control(lc->pamxctrl, MIX_VOLUME);
        else if (lc->dwControlType == MIXERCONTROL_CONTROLTYPE_MUTE) mix_control(lc->pamxctrl, MIX_MUTE);
        else return MIXERR_INVALCONTROL;
        return MMSYSERR_NOERROR;
    default:
        return MMSYSERR_INVALFLAG;
    }
}
static MMRESULT mix_details(HMIXEROBJ h, LPMIXERCONTROLDETAILS d, DWORD flags, int set) {
    if (!mix_ours(h, flags)) return MMSYSERR_INVALHANDLE;
    if (!d || !d->paDetails || d->cbDetails < sizeof(DWORD)) return MMSYSERR_INVALPARAM;
    if (d->dwControlID != MIX_VOLUME && d->dwControlID != MIX_MUTE) return MIXERR_INVALCONTROL;
    if ((flags & MIXER_GETCONTROLDETAILSF_QUERYMASK) != MIXER_GETCONTROLDETAILSF_VALUE) return MMSYSERR_NOTSUPPORTED;
    const DWORD n = d->cChannels ? d->cChannels : 1;
    for (DWORD i = 0; i < n; i++) {
        DWORD *v = (DWORD *)((char *)d->paDetails + i * d->cbDetails);
        if (set) {
            if (!i) g_mix_value[d->dwControlID] = *v;
        } else {
            *v = g_mix_value[d->dwControlID];
        }
    }
    return MMSYSERR_NOERROR;
}
static MMRESULT WINAPI mix_GetControlDetailsA(HMIXEROBJ h, LPMIXERCONTROLDETAILS d, DWORD flags) { return mix_details(h, d, flags, 0); }
static MMRESULT WINAPI mix_SetControlDetails(HMIXEROBJ h, LPMIXERCONTROLDETAILS d, DWORD flags) { return mix_details(h, d, flags, 1); }

void *hk_mixer_hook(const char *fn) {
    static const struct { const char *fn; void *to; } hooks[] = {
        {"mixerGetNumDevs", (void *)mix_GetNumDevs},
        {"mixerOpen", (void *)mix_Open},
        {"mixerClose", (void *)mix_Close},
        {"mixerGetDevCapsA", (void *)mix_GetDevCapsA},
        {"mixerGetLineInfoA", (void *)mix_GetLineInfoA},
        {"mixerGetLineControlsA", (void *)mix_GetLineControlsA},
        {"mixerGetControlDetailsA", (void *)mix_GetControlDetailsA},
        {"mixerSetControlDetails", (void *)mix_SetControlDetails},
    };
    for (size_t i = 0; i < sizeof hooks / sizeof *hooks; i++)
        if (!strcmp(fn, hooks[i].fn)) return hooks[i].to;
    return NULL;
}
