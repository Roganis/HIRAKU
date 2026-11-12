// SPDX-License-Identifier: GPL-3.0-or-later
// A sound card through its ASIO driver (asio.h).
#include "asio.h"

#ifdef _WIN32
#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "../core/prof.h"

namespace hk::host::asio {

namespace {

// --- the driver interface, from the ASIO specification ----------------------------------------
// A driver is an in-process COM object whose class id is also its interface id; `long` is 32 bits
// on Windows, as the specification's types are.
struct BufferInfo {
    long is_input, channel;
    void *buffers[2];   // the two halves of the double buffer, filled by the driver
};
struct ChannelInfo {
    long channel, is_input, active, group, type;
    char name[32];
};
struct Time;   // passed back untouched
struct Callbacks {
    void (*buffer_switch)(long index, long direct);
    void (*rate_changed)(double rate);
    long (*message)(long selector, long value, void *message, double *opt);
    Time *(*buffer_switch_time_info)(Time *time, long index, long direct);
};
struct Driver : IUnknown {
    virtual long init(void *window) = 0;
    virtual void driver_name(char *name) = 0;
    virtual long driver_version() = 0;
    virtual void error_message(char *message) = 0;   // 124 bytes
    virtual long start() = 0;
    virtual long stop() = 0;
    virtual long channels(long *inputs, long *outputs) = 0;
    virtual long latencies(long *input, long *output) = 0;
    virtual long buffer_size(long *min, long *max, long *preferred, long *granularity) = 0;
    virtual long can_sample_rate(double rate) = 0;
    virtual long sample_rate(double *rate) = 0;
    virtual long set_sample_rate(double rate) = 0;
    virtual long clock_sources(void *sources, long *count) = 0;
    virtual long set_clock_source(long index) = 0;
    virtual long sample_position(void *position, void *timestamp) = 0;
    virtual long channel_info(ChannelInfo *info) = 0;
    virtual long create_buffers(BufferInfo *infos, long count, long frames, Callbacks *callbacks) = 0;
    virtual long dispose_buffers() = 0;
    virtual long control_panel() = 0;
    virtual long future(long selector, void *opt) = 0;
    virtual long output_ready() = 0;
};
constexpr long OK = 0;
// messages (the driver's questions)
enum : long { SELECTOR_SUPPORTED = 1, ENGINE_VERSION, RESET_REQUEST, BUFFER_SIZE_CHANGE, RESYNC_REQUEST,
              LATENCIES_CHANGED, SUPPORTS_TIME_INFO, SUPPORTS_TIME_CODE, OVERLOAD = 15 };
// sample types: little-endian integers and floats; LSB16..24: 32-bit words holding that many bits
enum : long { INT16 = 16, INT24 = 17, INT32 = 18, FLOAT32 = 19, FLOAT64 = 20,
              INT32_16 = 24, INT32_18 = 25, INT32_20 = 26, INT32_24 = 27 };

Driver *g_drv = nullptr;
bool g_com = false, g_buffers = false, g_started = false, g_ready_call = false;
bool g_flowing = false;   // the game's sound has come (the buffers before it: silence, not breaks)
BufferInfo g_info[2];
Callbacks g_callbacks;
long g_type = 0, g_frames = 0, g_out_latency = 0;
double g_rate = 0;
float *g_scratch = nullptr;   // the stream's interleaved stereo, one buffer's worth
SDL_AudioStream *g_stream = nullptr;
std::atomic<uint64_t> g_written{0}, g_short{0}, g_overloads{0}, g_resets{0};
std::atomic<bool> g_restart{false};   // the driver asked to be restarted, and hasn't been yet
uint64_t g_t0 = 0;

const char *type_name(long t) {
    switch (t) {
    case INT16: return "16-bit";
    case INT24: return "24-bit";
    case INT32: return "32-bit";
    case FLOAT32: return "float";
    case FLOAT64: return "64-bit float";
    case INT32_16: return "16 bits in 32";
    case INT32_18: return "18 bits in 32";
    case INT32_20: return "20 bits in 32";
    case INT32_24: return "24 bits in 32";
    default: return nullptr;   // big-endian and DSD: not on a PC
    }
}

// one channel of the interleaved stereo `s` into the driver's buffer, in its form
void write_channel(const float *s, void *dst, long n, long type) {
    auto q = [](float v, double full) { return (int32_t)std::lrint(std::clamp(v, -1.0f, 1.0f) * full); };
    switch (type) {
    case INT16:
        for (long i = 0; i < n; i++) ((int16_t *)dst)[i] = (int16_t)q(s[2 * i], 32767.0);
        break;
    case INT24:
        for (long i = 0; i < n; i++) {
            const int32_t v = q(s[2 * i], 8388607.0);
            uint8_t *d = (uint8_t *)dst + 3 * i;
            d[0] = (uint8_t)v, d[1] = (uint8_t)(v >> 8), d[2] = (uint8_t)(v >> 16);
        }
        break;
    case INT32:
        for (long i = 0; i < n; i++) ((int32_t *)dst)[i] = q(s[2 * i], 2147483647.0);
        break;
    case FLOAT32:
        for (long i = 0; i < n; i++) ((float *)dst)[i] = s[2 * i];
        break;
    case FLOAT64:
        for (long i = 0; i < n; i++) ((double *)dst)[i] = s[2 * i];
        break;
    default: {   // INT32_16..24: right-aligned in a 32-bit word
        const double full = (double)((1 << (type == INT32_16 ? 15 : type == INT32_18 ? 17 : type == INT32_20 ? 19 : 23)) - 1);
        for (long i = 0; i < n; i++) ((int32_t *)dst)[i] = q(s[2 * i], full);
    }
    }
}

// the driver's thread, at each half of its double buffer: that half, filled
void buffer_switch(long index, long) {
    if (!g_started) return;
    const int bytes = (int)g_frames * 2 * (int)sizeof(float);
    const int got = std::max(0, SDL_GetAudioStreamData(g_stream, g_scratch, bytes));
    if (got > 0) g_flowing = true;
    if (got < bytes) {   // the game's sound didn't come in time: silence for the rest (a break, once it flows)
        memset((char *)g_scratch + got, 0, (size_t)(bytes - got));
        if (g_flowing) g_short.fetch_add(1, std::memory_order_relaxed);
    }
    for (int ch = 0; ch < 2; ch++) write_channel(g_scratch + ch, g_info[ch].buffers[index], g_frames, g_type);
    g_written.fetch_add((uint64_t)g_frames, std::memory_order_relaxed);
    if (g_ready_call) g_drv->output_ready();
}

Time *buffer_switch_time_info(Time *time, long index, long direct) {
    buffer_switch(index, direct);
    return time;
}

void rate_changed(double rate) {
    fprintf(stderr, "audio: the ASIO driver changed its rate to %.0f Hz (from %.0f): the sound plays at the wrong "
                    "speed until the game restarts\n", rate, g_rate);
}

long message(long selector, long value, void *, double *) {
    switch (selector) {
    case SELECTOR_SUPPORTED:
        return value == ENGINE_VERSION || value == RESET_REQUEST || value == RESYNC_REQUEST ||
               value == LATENCIES_CHANGED || value == OVERLOAD;
    case ENGINE_VERSION: return 2;
    case RESET_REQUEST:   // its settings changed (the control panel), or its card went: it wants to be closed
        g_resets.fetch_add(1, std::memory_order_relaxed);   // and opened again, which isn't done from its own call:
        g_restart = true;                                    // restart_wanted(), for the window (host/soundwatch.h)
        return 1;
    case RESYNC_REQUEST:
    case LATENCIES_CHANGED: return 1;
    case OVERLOAD: g_overloads.fetch_add(1, std::memory_order_relaxed); return 1;
    default: return 0;   // no time info: buffer_switch is called
    }
}

std::string error_of(const char *what, long r) {
    char m[128] = {};
    if (g_drv) g_drv->error_message(m);
    m[123] = 0;
    char b[256];
    snprintf(b, sizeof b, "%s (%ld%s%s)", what, r, m[0] ? ": " : "", m);
    return b;
}

// a buffer of about `want` frames that the driver takes
long pick_frames(long want, long min, long max, long preferred, long granularity) {
    if (want <= 0) return preferred;
    if (granularity == -1) {   // powers of two
        long n = 1;
        while (n < want && n < max) n <<= 1;
        return std::clamp(n, min, max);
    }
    if (granularity > 0) return std::clamp(min + (std::max(0L, want - min) + granularity - 1) / granularity * granularity, min, max);
    return preferred;   // one size
}

}  // namespace

std::vector<std::string> drivers() {
    std::vector<std::string> out;
    HKEY key;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\ASIO", 0, KEY_READ, &key) != ERROR_SUCCESS) return out;
    char name[256];
    for (DWORD i = 0;; i++) {
        DWORD n = sizeof name;
        if (RegEnumKeyExA(key, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        out.emplace_back(name, n);
    }
    RegCloseKey(key);
    return out;
}

std::string open(const std::string &driver, const SDL_AudioSpec &src, int frames, SDL_AudioStream *&stream,
                 std::string &what) {
    const std::vector<std::string> all = drivers();
    if (all.empty()) return "no ASIO drivers installed";
    std::string name;
    for (const std::string &d : all)
        if (driver.empty() || _stricmp(d.c_str(), driver.c_str()) == 0) { name = d; break; }
    if (name.empty()) {
        std::string list;
        for (const std::string &d : all) list += (list.empty() ? "" : ", ") + ("\"" + d + "\"");
        return "no ASIO driver named \"" + driver + "\" (installed: " + list + ")";
    }
    char clsid_text[64] = {};
    DWORD n = sizeof clsid_text;
    if (RegGetValueA(HKEY_LOCAL_MACHINE, ("SOFTWARE\\ASIO\\" + name).c_str(), "CLSID", RRF_RT_REG_SZ, nullptr,
                     clsid_text, &n) != ERROR_SUCCESS)
        return "the driver's registry entry has no CLSID";
    wchar_t wide[64];
    MultiByteToWideChar(CP_ACP, 0, clsid_text, -1, wide, 64);
    CLSID clsid;
    if (FAILED(CLSIDFromString(wide, &clsid))) return std::string("the driver's CLSID is not one: ") + clsid_text;

    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    g_com = SUCCEEDED(co);   // (RPC_E_CHANGED_MODE: COM is already up on this thread, otherwise)
    const HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, clsid, (void **)&g_drv);
    if (FAILED(hr) || !g_drv) {
        char b[96];
        snprintf(b, sizeof b, "the driver didn't load (0x%08lx: a 32-bit driver only?)", (unsigned long)hr);
        return close(), b;
    }
    if (!g_drv->init(GetDesktopWindow())) return close(), error_of("the driver didn't start", 0);

    long inputs = 0, outputs = 0;
    if (long r = g_drv->channels(&inputs, &outputs); r != OK || outputs < 2)
        return close(), error_of(outputs < 2 ? "the card has fewer than two outputs" : "no channels", r);
    for (int ch = 0; ch < 2; ch++) {
        ChannelInfo ci{};
        ci.channel = ch;
        if (long r = g_drv->channel_info(&ci); r != OK) return close(), error_of("no channel information", r);
        if (ch == 0) g_type = ci.type;
        else if (ci.type != g_type) return close(), "the card's two first outputs take different samples";
    }
    if (!type_name(g_type)) return close(), "the card takes samples of a kind HIRAKU doesn't write (" + std::to_string(g_type) + ")";

    // the rate: the game's, else a common one, else whatever the card is at
    g_rate = 0;
    for (double r : {(double)src.freq, 48000.0, 44100.0, 96000.0})
        if (g_drv->can_sample_rate(r) == OK && g_drv->set_sample_rate(r) == OK) { g_rate = r; break; }
    if (g_rate == 0 && (g_drv->sample_rate(&g_rate) != OK || g_rate <= 0)) return close(), "the card has no rate";

    long min = 0, max = 0, preferred = 0, granularity = 0;
    if (long r = g_drv->buffer_size(&min, &max, &preferred, &granularity); r != OK || preferred <= 0)
        return close(), error_of("no buffer sizes", r);
    g_frames = pick_frames(frames > 0 ? (long)std::lround((double)frames * g_rate / src.freq) : 0, min, max, preferred, granularity);

    for (int ch = 0; ch < 2; ch++) g_info[ch] = BufferInfo{0, ch, {nullptr, nullptr}};
    g_callbacks = Callbacks{buffer_switch, rate_changed, message, buffer_switch_time_info};
    g_started = false;
    if (long r = g_drv->create_buffers(g_info, 2, g_frames, &g_callbacks); r != OK) return close(), error_of("no buffers", r);
    g_buffers = true;
    long in_latency = 0;
    g_drv->latencies(&in_latency, &g_out_latency);
    g_ready_call = g_drv->output_ready() == OK;   // a driver that wants to be told each half is ready
    g_scratch = new float[(size_t)g_frames * 2];

    const SDL_AudioSpec dst{SDL_AUDIO_F32, 2, (int)g_rate};
    g_stream = stream = SDL_CreateAudioStream(&src, &dst);
    char drv_name[64] = {};
    g_drv->driver_name(drv_name);
    char b[256];
    snprintf(b, sizeof b, "%s, %s, %.0f Hz, buffer %ld frames (%.1f ms; the driver's %ld to %ld, its own %ld), output latency %.1f ms",
             drv_name[0] ? drv_name : name.c_str(), type_name(g_type), g_rate, g_frames, g_frames * 1000.0 / g_rate, min,
             max, preferred, g_out_latency * 1000.0 / g_rate);
    what = b;
    g_written = 0, g_short = 0, g_flowing = false;   // (started again: the report is this start's)
    g_t0 = prof::now_ns();
    g_started = true;
    if (long r = g_drv->start(); r != OK) {
        g_started = false;
        SDL_DestroyAudioStream(g_stream);
        g_stream = stream = nullptr;
        return close(), error_of("the driver didn't start playing", r);
    }
    g_restart = false;   // (a request made while it was being set up, the rate set: it is set up now)
    return "";
}

void close() {
    if (g_drv) {
        if (g_started) g_drv->stop();
        g_started = false;
        if (g_buffers) g_drv->dispose_buffers();
        g_buffers = false;
        g_drv->Release();
        g_drv = nullptr;
    }
    g_stream = nullptr;   // the caller's (window_close destroys it after this)
    delete[] g_scratch;
    g_scratch = nullptr;
    g_restart = false;
    if (g_com) CoUninitialize(), g_com = false;
}

bool active() { return g_started; }
bool restart_wanted() { return g_started && g_restart.load(std::memory_order_relaxed); }
int period_frames() { return (int)g_frames; }

std::string describe() {
    if (!g_t0) return {};
    const double secs = (prof::now_ns() - g_t0) / 1e9;
    char b[320];
    int n = snprintf(b, sizeof b, "audio: the ASIO driver took %.2f %% of its rate over %.1f s; %llu of its buffers found the sound short",
                     secs > 0 ? 100.0 * (double)g_written.load() / (secs * g_rate) : 0.0, secs,
                     (unsigned long long)g_short.load());
    if (g_overloads) n += snprintf(b + n, sizeof b - n, "; %llu overloads", (unsigned long long)g_overloads.load());
    if (g_resets)
        snprintf(b + n, sizeof b - n, "; the driver asked %llu times to be restarted (its settings changed), and was",
                 (unsigned long long)g_resets.load());
    return b;
}

}  // namespace hk::host::asio
#endif
