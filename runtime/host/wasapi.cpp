// SPDX-License-Identifier: GPL-3.0-or-later
// WASAPI exclusive mode (wasapi.h).
#include "wasapi.h"

#ifdef _WIN32
#include <initguid.h>   // the interface and property ids below, defined here
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <mmreg.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

#include "../core/prof.h"

namespace hk::host::wasapi {

namespace {

IMMDevice *g_dev = nullptr;
IAudioClient *g_client = nullptr;
IAudioRenderClient *g_render = nullptr;
HANDLE g_event = nullptr;
UINT32 g_buffer = 0;          // the device's buffer, frames: filled whole on each of its events
int g_frame_bytes = 0;
unsigned g_rate = 0;
SDL_AudioStream *g_stream = nullptr;
std::atomic<bool> g_stop{false};
std::atomic<bool> g_lost{false};   // Windows took the device back: the thread has stopped filling it
std::thread *g_thread = nullptr;   // never destroyed: a game ends with exit(), the thread still writing
std::atomic<uint64_t> g_written{0}, g_short{0};
uint64_t g_t0 = 0;

// the sample formats' ids (ksmedia.h's KSDATAFORMAT_SUBTYPE_IEEE_FLOAT and _PCM, which mingw's
// libraries don't define)
const GUID SUBTYPE_FLOAT = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
const GUID SUBTYPE_PCM = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
// the interfaces' ids, defined here too: MSVC's libraries don't (mingw's initguid.h route does)
const GUID HK_CLSID_MMDeviceEnumerator = {0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
const GUID HK_IID_IMMDeviceEnumerator = {0xa95664d2, 0x9614, 0x4f35, {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}};
const GUID HK_IID_IAudioClient = {0x1cb9ad4c, 0xdbfa, 0x4c32, {0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2}};
constexpr HRESULT HK_E_RESOURCES_INVALIDATED = (HRESULT)0x88890026;   // AUDCLNT_E_RESOURCES_INVALIDATED (newer headers only)
const GUID HK_IID_IAudioRenderClient = {0xf294acfc, 0x3146, 0x4483, {0xa7, 0xbf, 0xad, 0xdc, 0xa7, 0xc2, 0x60, 0xe2}};

std::string narrow(const wchar_t *w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? (size_t)n - 1 : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

// the output device by the name Windows shows (SDL's names are the same), or the default one
IMMDevice *find(IMMDeviceEnumerator *e, const std::string &name) {
    IMMDevice *found = nullptr;
    if (name.empty()) {
        e->GetDefaultAudioEndpoint(eRender, eConsole, &found);
        return found;
    }
    IMMDeviceCollection *c = nullptr;
    if (FAILED(e->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &c))) return nullptr;
    UINT n = 0;
    c->GetCount(&n);
    for (UINT i = 0; i < n && !found; i++) {
        IMMDevice *d = nullptr;
        if (FAILED(c->Item(i, &d)) || !d) continue;
        IPropertyStore *ps = nullptr;
        if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR && narrow(v.pwszVal) == name)
                found = d;
            PropVariantClear(&v);
            ps->Release();
        }
        if (d != found) d->Release();
    }
    c->Release();
    return found;
}

std::string hresult(HRESULT hr) {
    if (hr == AUDCLNT_E_DEVICE_IN_USE) return "the device is in use by another program in exclusive mode";
    if (hr == AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED) return "exclusive mode is turned off for this device (Windows' Sound settings: the device's properties, Advanced, \"Allow applications to take exclusive control of this device\")";
    if (hr == AUDCLNT_E_UNSUPPORTED_FORMAT) return "the device takes none of the formats tried";
    char b[48];
    snprintf(b, sizeof b, "error 0x%08lx", (unsigned long)hr);
    return b;
}

void run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD task = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    const int bytes = (int)g_buffer * g_frame_bytes;
    while (!g_stop.load(std::memory_order_relaxed)) {
        if (WaitForSingleObject(g_event, 200) != WAIT_OBJECT_0) continue;
        BYTE *p = nullptr;
        const HRESULT hr = g_render->GetBuffer(g_buffer, &p);
        // the device unplugged or turned off, its format changed in Windows' settings, the audio
        // service stopped: this client is finished, for good (lost(): the window opens another)
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_SERVICE_NOT_RUNNING || hr == HK_E_RESOURCES_INVALIDATED) {
            g_lost = true;
            break;
        }
        if (FAILED(hr)) continue;
        const int got = std::max(0, SDL_GetAudioStreamData(g_stream, p, bytes));
        if (got < bytes) {   // the game's sound didn't come in time: silence for the rest (a break)
            memset(p + got, 0, (size_t)(bytes - got));
            g_short.fetch_add(1, std::memory_order_relaxed);
        }
        g_render->ReleaseBuffer(g_buffer, 0);
        g_written.fetch_add(g_buffer, std::memory_order_relaxed);
    }
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    CoUninitialize();
}

}  // namespace

std::string open(const std::string &device, const SDL_AudioSpec &src, int frames, SDL_AudioStream *&stream, std::string &what) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // (SDL may have it already, in another mode: fine)
    IMMDeviceEnumerator *e = nullptr;
    HRESULT hr = CoCreateInstance(HK_CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, HK_IID_IMMDeviceEnumerator, (void **)&e);
    if (FAILED(hr)) return "no audio devices (" + hresult(hr) + ")";
    g_dev = find(e, device);
    e->Release();
    if (!g_dev) return device.empty() ? "no default output device" : "no output device named \"" + device + "\"";
    hr = g_dev->Activate(HK_IID_IAudioClient, CLSCTX_ALL, nullptr, (void **)&g_client);
    if (FAILED(hr)) return close(), hresult(hr);

    // a format the device takes alone: the game's rate first, float first
    const struct { GUID sub; WORD bits, valid; SDL_AudioFormat sdl; const char *name; } formats[] = {
        {SUBTYPE_FLOAT, 32, 32, SDL_AUDIO_F32LE, "float"},
        {SUBTYPE_PCM, 32, 32, SDL_AUDIO_S32LE, "32-bit"},
        {SUBTYPE_PCM, 32, 24, SDL_AUDIO_S32LE, "24-bit"},
        {SUBTYPE_PCM, 16, 16, SDL_AUDIO_S16LE, "16-bit"}};
    const unsigned rates[] = {(unsigned)src.freq, 48000, 44100, 96000};
    WAVEFORMATEXTENSIBLE fmt{};
    SDL_AudioSpec dst{};
    HRESULT first_refusal = S_OK;   // what the driver said to the first format (for the message)
    const char *fname = nullptr;
    for (unsigned rate : rates) {
        for (const auto &f : formats) {
            fmt = WAVEFORMATEXTENSIBLE{};
            fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
            fmt.Format.nChannels = 2;
            fmt.Format.nSamplesPerSec = rate;
            fmt.Format.wBitsPerSample = f.bits;
            fmt.Format.nBlockAlign = (WORD)(2 * f.bits / 8);
            fmt.Format.nAvgBytesPerSec = rate * fmt.Format.nBlockAlign;
            fmt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
            fmt.Samples.wValidBitsPerSample = f.valid;
            fmt.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
            fmt.SubFormat = f.sub;
            const HRESULT ok = g_client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, (WAVEFORMATEX *)&fmt, nullptr);
            if (first_refusal == S_OK && ok != S_OK) first_refusal = ok;
            if (ok == S_OK) {
                dst = SDL_AudioSpec{f.sdl, 2, (int)rate};
                g_frame_bytes = fmt.Format.nBlockAlign;
                fname = f.name;
                break;
            }
        }
        if (fname) break;
    }
    // some drivers want the plain layout (WAVEFORMATEX), or only their own mix format
    for (unsigned rate : rates) {
        for (const auto &f : formats) {
            if (fname || f.valid != f.bits) continue;
            WAVEFORMATEX plain{};
            plain.wFormatTag = f.sub == SUBTYPE_FLOAT ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
            plain.nChannels = 2;
            plain.nSamplesPerSec = rate;
            plain.wBitsPerSample = f.bits;
            plain.nBlockAlign = (WORD)(2 * f.bits / 8);
            plain.nAvgBytesPerSec = rate * plain.nBlockAlign;
            if (g_client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &plain, nullptr) == S_OK) {
                fmt = WAVEFORMATEXTENSIBLE{};
                fmt.Format = plain;
                dst = SDL_AudioSpec{f.sdl, 2, (int)rate};
                g_frame_bytes = plain.nBlockAlign;
                fname = f.name;
            }
        }
        if (fname) break;
    }
    if (!fname) {
        WAVEFORMATEX *mix = nullptr;
        if (SUCCEEDED(g_client->GetMixFormat(&mix)) && mix && mix->nChannels == 2 &&
            g_client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, mix, nullptr) == S_OK) {
            const bool ext = mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE;
            const bool flt = mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                             (ext && ((WAVEFORMATEXTENSIBLE *)mix)->SubFormat == SUBTYPE_FLOAT);
            const SDL_AudioFormat sf = flt ? SDL_AUDIO_F32LE : mix->wBitsPerSample == 16 ? SDL_AUDIO_S16LE : SDL_AUDIO_S32LE;
            if (flt || mix->wBitsPerSample == 16 || mix->wBitsPerSample == 32) {
                memcpy(&fmt, mix, ext ? sizeof(WAVEFORMATEXTENSIBLE) : sizeof(WAVEFORMATEX));
                dst = SDL_AudioSpec{sf, 2, (int)mix->nSamplesPerSec};
                g_frame_bytes = mix->nBlockAlign;
                fname = flt ? "float (the mix format)" : "the mix format";
            }
        }
        if (mix) CoTaskMemFree(mix);
    }
    if (!fname) {   // the driver's own reason when it gave one (exclusive mode turned off, the device busy)
        if (first_refusal == AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED || first_refusal == AUDCLNT_E_DEVICE_IN_USE)
            return close(), hresult(first_refusal);
        char b[64];
        snprintf(b, sizeof b, " (the driver's answer: 0x%08lx)", (unsigned long)first_refusal);
        return close(), hresult(AUDCLNT_E_UNSUPPORTED_FORMAT) + b;
    }
    g_rate = (unsigned)dst.freq;

    // the device's period: the frames asked for, no shorter than the device allows; a size the
    // device can't align to is given back as its nearest, and asked again
    REFERENCE_TIME def = 0, minimum = 0;
    g_client->GetDevicePeriod(&def, &minimum);
    REFERENCE_TIME period = std::max<REFERENCE_TIME>(minimum, (REFERENCE_TIME)(frames * 1e7 / src.freq + 0.5));
    hr = g_client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, period,
                              (WAVEFORMATEX *)&fmt, nullptr);
    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        UINT32 n = 0;
        g_client->GetBufferSize(&n);
        period = (REFERENCE_TIME)(1e7 * n / g_rate + 0.5);
        g_client->Release();
        g_client = nullptr;
        hr = g_dev->Activate(HK_IID_IAudioClient, CLSCTX_ALL, nullptr, (void **)&g_client);
        if (SUCCEEDED(hr))
            hr = g_client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, period,
                                      (WAVEFORMATEX *)&fmt, nullptr);
    }
    if (FAILED(hr)) return close(), hresult(hr);
    g_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_client->SetEventHandle(g_event);
    g_client->GetBufferSize(&g_buffer);
    hr = g_client->GetService(HK_IID_IAudioRenderClient, (void **)&g_render);
    if (FAILED(hr)) return close(), hresult(hr);
    BYTE *p = nullptr;   // a buffer of silence before it starts
    if (SUCCEEDED(g_render->GetBuffer(g_buffer, &p))) g_render->ReleaseBuffer(g_buffer, AUDCLNT_BUFFERFLAGS_SILENT);

    g_stream = stream = SDL_CreateAudioStream(&src, &dst);
    char b[200];
    snprintf(b, sizeof b, "%s, %u Hz, buffer %u frames (%.1f ms; the device's shortest %.1f ms)", fname, g_rate,
             (unsigned)g_buffer, g_buffer * 1000.0 / g_rate, minimum / 1e4);
    what = b;
    g_stop = false, g_lost = false;
    g_written = 0, g_short = 0;   // (opened again after a device was lost: the report is this one's)
    g_t0 = prof::now_ns();
    g_thread = new std::thread(run);
    g_client->Start();
    return "";
}

void close() {
    if (g_thread) {
        g_stop = true;
        g_thread->join();
        delete g_thread;
        g_thread = nullptr;
    }
    g_lost = false;   // (a device that was lost is closed: whatever plays next isn't)
    if (g_client) g_client->Stop();
    if (g_render) g_render->Release(), g_render = nullptr;
    if (g_client) g_client->Release(), g_client = nullptr;
    if (g_dev) g_dev->Release(), g_dev = nullptr;
    if (g_event) CloseHandle(g_event), g_event = nullptr;
}

bool active() { return g_thread != nullptr; }
bool lost() { return g_lost.load(std::memory_order_relaxed); }
int period_frames() { return (int)g_buffer; }

std::string describe() {
    if (!g_t0) return {};
    const double secs = (prof::now_ns() - g_t0) / 1e9;
    char b[200];
    snprintf(b, sizeof b, "audio: the device took %.2f %% of its rate over %.1f s; %llu of its periods found the sound short",
             secs > 0 ? 100.0 * (double)g_written.load() / (secs * g_rate) : 0.0, secs,
             (unsigned long long)g_short.load());
    return b;
}

}  // namespace hk::host::wasapi
#endif
