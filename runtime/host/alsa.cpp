// SPDX-License-Identifier: GPL-3.0-or-later
// A sound card of our own (--audio-device): HIRAKU writes to it through ALSA itself, as aplay does,
// a period at a time into a ring kept full: of four periods on a USB card, two on others. SDL's own
// ALSA output keeps a ring of two, and a USB card's driver starves on that, playing stale sound
// without reporting it (a USB S/PDIF adapter played 4 s of sound in 6.6 s at 64 frames x 2, in
// 4.1 s at 64 x 4; at 64 x 3 still 5.2); an onboard HDA card plays two in time, and each period
// left out of the ring is that much sooner. libasound is loaded at run time, as SDL loads it: no
// build dependency.
#include "alsa.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>
#ifdef __linux__
#include <sched.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include "../core/prof.h"

namespace hk::host::alsa {

struct pcm;
struct hw;
using uframes = unsigned long;
struct Api {
    int (*open)(pcm **, const char *, int, int);
    int (*close)(pcm *);
    int (*hw_malloc)(hw **);
    void (*hw_free)(hw *);
    int (*hw_any)(pcm *, hw *);
    int (*set_access)(pcm *, hw *, int);
    int (*set_format)(pcm *, hw *, int);
    int (*set_channels)(pcm *, hw *, unsigned);
    int (*set_rate_near)(pcm *, hw *, unsigned *, int *);
    int (*set_period_near)(pcm *, hw *, uframes *, int *);
    int (*set_buffer_near)(pcm *, hw *, uframes *);
    int (*get_period)(const hw *, uframes *, int *);
    int (*get_buffer)(const hw *, uframes *);
    int (*hw_params)(pcm *, hw *);
    int (*wait)(pcm *, int);
    long (*avail_update)(pcm *);
    long (*writei)(pcm *, const void *, uframes);
    int (*recover)(pcm *, int, int);
    int (*drop)(pcm *);
    const char *(*strerror)(int);
    int (*card_index)(const char *);   // snd_card_get_index: for the card's reservation (optional)
} api;
pcm *g_pcm = nullptr;
int g_period = 0, g_frame_bytes = 0;
unsigned g_card_rate = 0;
std::atomic<uint64_t> g_written{0}, g_xruns{0};   // frames written after the ring's first fill, underruns
uint64_t g_t0 = 0;                                 // when the ring was first full
std::atomic<bool> g_stop{false};
std::atomic<bool> g_lost{false};   // the card is gone (a USB card pulled out): the thread has stopped writing
std::thread *g_thread = nullptr;   // never destroyed: a game ends with exit(), the thread still writing

bool load(std::string &why) {
    static SDL_SharedObject *lib = SDL_LoadObject("libasound.so.2");   // dlopen, RTLD_NOW | RTLD_LOCAL
    if (!lib) return why = "no libasound.so.2", false;
    bool ok = true;
    auto get = [&](auto &f, const char *name) {
        f = reinterpret_cast<std::remove_reference_t<decltype(f)>>(SDL_LoadFunction(lib, name));
        if (!f) ok = false, why = std::string("libasound has no ") + name;
    };
    get(api.open, "snd_pcm_open"), get(api.close, "snd_pcm_close"), get(api.hw_malloc, "snd_pcm_hw_params_malloc");
    get(api.hw_free, "snd_pcm_hw_params_free"), get(api.hw_any, "snd_pcm_hw_params_any");
    get(api.set_access, "snd_pcm_hw_params_set_access"), get(api.set_format, "snd_pcm_hw_params_set_format");
    get(api.set_channels, "snd_pcm_hw_params_set_channels"), get(api.set_rate_near, "snd_pcm_hw_params_set_rate_near");
    get(api.set_period_near, "snd_pcm_hw_params_set_period_size_near");
    get(api.set_buffer_near, "snd_pcm_hw_params_set_buffer_size_near");
    get(api.get_period, "snd_pcm_hw_params_get_period_size"), get(api.get_buffer, "snd_pcm_hw_params_get_buffer_size");
    get(api.hw_params, "snd_pcm_hw_params"), get(api.wait, "snd_pcm_wait"), get(api.avail_update, "snd_pcm_avail_update");
    get(api.writei, "snd_pcm_writei"), get(api.recover, "snd_pcm_recover"), get(api.drop, "snd_pcm_drop");
    get(api.strerror, "snd_strerror");
    api.card_index = reinterpret_cast<int (*)(const char *)>(SDL_LoadFunction(lib, "snd_card_get_index"));
    return ok;
}

// The card's reservation (org.freedesktop.ReserveDevice1, the protocol JACK and PulseAudio use):
// PipeWire's WirePlumber holds the bus name Audio<card> for each card it drives. Asked
// (RequestRelease), it lets the card go and waits; when the name is given up (close()), it takes
// the card back. Opened without this, WirePlumber found the card busy while a game held it and
// was left without its device after the game had closed (2026-10-19, the owner's PC). libdbus is
// loaded when needed; anything missing (no D-Bus, no PipeWire, not a hw: device): nothing done.
namespace reserve {
struct DBusError {
    const char *name, *message;
    unsigned dummy1 : 1, dummy2 : 1, dummy3 : 1, dummy4 : 1, dummy5 : 1;
    void *padding;
};
struct Conn;
struct Msg;
struct {
    Conn *(*bus_get_private)(int, DBusError *);
    void (*set_exit_on_disconnect)(Conn *, unsigned);
    int (*request_name)(Conn *, const char *, unsigned, DBusError *);
    int (*release_name)(Conn *, const char *, DBusError *);
    Msg *(*new_method_call)(const char *, const char *, const char *, const char *);
    unsigned (*append_args)(Msg *, int, ...);
    Msg *(*send_block)(Conn *, Msg *, int, DBusError *);
    unsigned (*get_args)(Msg *, DBusError *, int, ...);
    void (*unref)(Msg *);
    void (*flush)(Conn *);
    void (*close)(Conn *);
    void (*conn_unref)(Conn *);
    void (*error_init)(DBusError *);
    void (*error_free)(DBusError *);
    unsigned (*read_write)(Conn *, int);
    Msg *(*pop)(Conn *);
    int (*type)(Msg *);
    unsigned (*is_call)(Msg *, const char *, const char *);
    Msg *(*new_return)(Msg *);
    Msg *(*new_error)(Msg *, const char *, const char *);
    unsigned (*send)(Conn *, Msg *, uint32_t *);
} d;
Conn *g_conn = nullptr;
std::string g_name;

bool load() {
    static SDL_SharedObject *lib = SDL_LoadObject("libdbus-1.so.3");
    if (!lib) return false;
    bool ok = true;
    auto get = [&](auto &f, const char *name) {
        f = reinterpret_cast<std::remove_reference_t<decltype(f)>>(SDL_LoadFunction(lib, name));
        if (!f) ok = false;
    };
    get(d.bus_get_private, "dbus_bus_get_private"), get(d.set_exit_on_disconnect, "dbus_connection_set_exit_on_disconnect");
    get(d.request_name, "dbus_bus_request_name"), get(d.release_name, "dbus_bus_release_name");
    get(d.new_method_call, "dbus_message_new_method_call"), get(d.append_args, "dbus_message_append_args");
    get(d.send_block, "dbus_connection_send_with_reply_and_block"), get(d.get_args, "dbus_message_get_args");
    get(d.unref, "dbus_message_unref"), get(d.flush, "dbus_connection_flush"), get(d.close, "dbus_connection_close");
    get(d.conn_unref, "dbus_connection_unref"), get(d.error_init, "dbus_error_init"), get(d.error_free, "dbus_error_free");
    get(d.read_write, "dbus_connection_read_write"), get(d.pop, "dbus_connection_pop_message");
    get(d.type, "dbus_message_get_type"), get(d.is_call, "dbus_message_is_method_call");
    get(d.new_return, "dbus_message_new_method_return"), get(d.new_error, "dbus_message_new_error");
    get(d.send, "dbus_connection_send");
    return ok;
}

// the card a device names: hw:CARD=Generic,DEV=0, plughw:1,0 ... (-1: not a card's own device)
int card_of(const std::string &device) {
    const size_t colon = device.find(':');
    if (colon == std::string::npos || !api.card_index) return -1;
    const std::string kind = device.substr(0, colon);
    if (kind != "hw" && kind != "plughw") return -1;
    std::string card = device.substr(colon + 1);
    card = card.substr(0, card.find(','));
    if (card.rfind("CARD=", 0) == 0) card = card.substr(5);
    return card.empty() ? -1 : api.card_index(card.c_str());
}

// "": reserved (or nothing to do); else what happened, for the log
std::string acquire(const std::string &device) {
    const int card = card_of(device);
    if (card < 0 || !load()) return "";
    DBusError e;
    d.error_init(&e);
    g_conn = d.bus_get_private(0 /* session */, &e);
    if (!g_conn) return d.error_free(&e), "no session bus";
    d.set_exit_on_disconnect(g_conn, 0);
    g_name = "org.freedesktop.ReserveDevice1.Audio" + std::to_string(card);
    constexpr unsigned REPLACE_EXISTING = 2, DO_NOT_QUEUE = 4;
    int r = d.request_name(g_conn, g_name.c_str(), DO_NOT_QUEUE, &e);
    if (r == 3 /* exists: someone has the card */) {
        d.error_free(&e);
        const std::string path = "/org/freedesktop/ReserveDevice1/Audio" + std::to_string(card);
        Msg *m = d.new_method_call(g_name.c_str(), path.c_str(), "org.freedesktop.ReserveDevice1", "RequestRelease");
        int32_t priority = 100;   // above WirePlumber's own (its default is -20)
        d.append_args(m, 'i', &priority, 0);
        Msg *reply = d.send_block(g_conn, m, 2000, &e);
        d.unref(m);
        unsigned granted = 0;
        if (reply) d.get_args(reply, &e, 'b', &granted, 0), d.unref(reply);
        d.error_free(&e);
        if (!granted) return "the card's owner wouldn't release it (" + g_name + ")";
        r = d.request_name(g_conn, g_name.c_str(), DO_NOT_QUEUE | REPLACE_EXISTING, &e);
    }
    d.error_free(&e);
    if (r != 1 && r != 4) {
        d.close(g_conn), d.conn_unref(g_conn), g_conn = nullptr;
        return "couldn't take " + g_name;
    }
    d.flush(g_conn);
    return "";
}

// While we hold the card: the owner we took it from asks for it back at once (WirePlumber keeps
// trying to hold every card); "no", promptly. Unanswered, its call waited out the bus's timeout,
// it missed the name coming free when we closed, and it never took the card back. Anything else
// called on us: the standard "no such method". From the thread that plays (the only one using the
// connection while it runs), every ~100 ms.
void serve() {
    if (!g_conn) return;
    d.read_write(g_conn, 0);
    while (Msg *m = d.pop(g_conn)) {
        if (d.is_call(m, "org.freedesktop.ReserveDevice1", "RequestRelease")) {
            Msg *r = d.new_return(m);
            unsigned no = 0;
            d.append_args(r, 'b', &no, 0);
            d.send(g_conn, r, nullptr), d.unref(r);
        } else if (d.type(m) == 1 /* a method call */) {
            Msg *r = d.new_error(m, "org.freedesktop.DBus.Error.UnknownMethod", "HIRAKU holds this card");
            d.send(g_conn, r, nullptr), d.unref(r);
        }
        d.unref(m);
    }
    d.flush(g_conn);
}

void release() {
    if (!g_conn) return;
    DBusError e;
    d.error_init(&e);
    d.release_name(g_conn, g_name.c_str(), &e);
    d.error_free(&e);
    d.flush(g_conn);
    d.close(g_conn);
    d.conn_unref(g_conn);
    g_conn = nullptr;
}
}  // namespace reserve

std::string realtime_thread();   // below
std::string g_writer;            // how the thread that writes runs (the report)

// each time a period of the ring is free: that much from the stream (silence for what it lacks:
// the stream's get callback counts that as a break), written. Real-time where allowed: it sleeps
// until the card has room, and an error is never retried at once, so it can't hold a CPU. A card
// that is gone ("no such device": pulled out, and no call on it will ever work again) ends it:
// lost() then says so, for the window to open another way out (host/soundwatch.h)
void run(SDL_AudioStream *stream) {
    g_writer = realtime_thread();
    std::vector<uint8_t> buf((size_t)g_period * g_frame_bytes);
    const int bytes = (int)buf.size();
    auto gone = [](long e) {   // an error taken up (an underrun: the card started again); true: the card is gone
        g_xruns.fetch_add(1, std::memory_order_relaxed);
        if (e == -ENODEV || api.recover(g_pcm, (int)e, 1) == -ENODEV) return g_lost = true, true;
        SDL_Delay(1);
        return false;
    };
    for (unsigned turn = 0; !g_stop.load(std::memory_order_relaxed); turn++) {
        if (turn % 32 == 0) reserve::serve();   // the card's reservation: its owner's calls answered
        long avail = api.avail_update(g_pcm);
        if (avail < 0) {
            if (gone(avail)) break;
            continue;
        }
        if (avail < g_period) {
            api.wait(g_pcm, 100);
            continue;
        }
        const int got = std::max(0, SDL_GetAudioStreamData(stream, buf.data(), bytes));
        if (got < bytes) memset(buf.data() + got, 0, (size_t)(bytes - got));
        const long n = api.writei(g_pcm, buf.data(), (uframes)g_period);
        if (n < 0) {
            if (gone(n)) break;
        } else {
            g_written.fetch_add((uint64_t)n, std::memory_order_relaxed);
        }
    }
}

// `device` alone at `src`'s rate (or the nearest it has), periods of `period` frames: a stream
// converting `src` to it, which a thread of ours plays. "" or why not
std::string open(const std::string &device, const SDL_AudioSpec &src, int period, SDL_AudioStream *&stream, std::string &what) {
    std::string why;
    if (!load(why)) return why;
    const std::string reserved = reserve::acquire(device);
    if (!reserved.empty()) fprintf(stderr, "sound: %s; opening the card anyway\n", reserved.c_str());
    int e = api.open(&g_pcm, device.c_str(), 0 /* playback */, 0);
    // the owner closes the card once it has released it: a moment busy (EBUSY) at most
    for (int tries = 0; e == -16 && tries < 20; tries++) SDL_Delay(50), e = api.open(&g_pcm, device.c_str(), 0, 0);
    if (e < 0) return reserve::release(), g_pcm = nullptr, api.strerror(e);
    hw *h = nullptr;
    api.hw_malloc(&h);
    api.hw_any(g_pcm, h);
    e = api.set_access(g_pcm, h, 3 /* RW_INTERLEAVED */);
    // float, 32 or 16 bits, whichever the card takes first
    const struct { int alsa; SDL_AudioFormat sdl; int bytes; const char *name; } formats[] = {
        {14, SDL_AUDIO_F32LE, 4, "float"}, {10, SDL_AUDIO_S32LE, 4, "32-bit"}, {2, SDL_AUDIO_S16LE, 2, "16-bit"}};
    const char *fname = nullptr;
    SDL_AudioSpec dst{SDL_AUDIO_S16LE, 2, src.freq};
    for (const auto &f : formats)
        if (e >= 0 && api.set_format(g_pcm, h, f.alsa) >= 0) {
            dst.format = f.sdl, g_frame_bytes = 2 * f.bytes, fname = f.name;
            break;
        }
    unsigned rate = (unsigned)src.freq;
    uframes p = (uframes)period, ring = 0;
    // a USB card (its /proc/asound entry has a usbid), or one we can't tell: four periods
    const int card = reserve::card_of(device);
    const bool usb = card < 0 || std::filesystem::exists("/proc/asound/card" + std::to_string(card) + "/usbid");
    int periods = usb ? 4 : 2;
    if (const char *n = getenv("HK_ALSA_PERIODS"); n && atoi(n) >= 2) periods = atoi(n);
    if (e >= 0 && !fname) e = -22;
    if (e >= 0) e = api.set_channels(g_pcm, h, 2);
    if (e >= 0) e = api.set_rate_near(g_pcm, h, &rate, nullptr);
    if (e >= 0) e = api.set_period_near(g_pcm, h, &p, nullptr);
    if (e >= 0) ring = (uframes)periods * p, e = api.set_buffer_near(g_pcm, h, &ring);
    if (e >= 0) e = api.hw_params(g_pcm, h);
    if (e >= 0) api.get_period(h, &p, nullptr), api.get_buffer(h, &ring);
    api.hw_free(h);
    if (e < 0) {
        api.close(g_pcm);
        g_pcm = nullptr;
        reserve::release();
        return api.strerror(e);
    }
    g_period = (int)p;
    dst.freq = (int)rate;
    stream = SDL_CreateAudioStream(&src, &dst);
    char b[160];
    snprintf(b, sizeof b, "%s, %u Hz, ring %lu frames (%.1f ms) in periods of %lu%s", fname, rate, ring, ring * 1000.0 / rate, p,
             !usb ? "" : card >= 0 ? ", four for a USB card" : ", four (which card it is unknown)");
    what = b;
    // the ring full of silence, then the thread keeps it full
    std::vector<uint8_t> zero((size_t)ring * g_frame_bytes, 0);
    api.writei(g_pcm, zero.data(), ring);
    g_card_rate = rate;
    g_written = 0, g_xruns = 0;   // (opened again after a card was lost: the report is this card's)
    g_t0 = prof::now_ns();
    g_stop = false, g_lost = false;
    g_thread = new std::thread(run, stream);
    return "";
}

void close() {
    if (!g_pcm) return;
    g_stop = true;
    if (g_thread) g_thread->join(), delete g_thread, g_thread = nullptr;
    g_lost = false;   // (a card that was lost is closed: whatever plays next isn't)
    api.drop(g_pcm);
    api.close(g_pcm);
    g_pcm = nullptr;
    reserve::release();   // the card back to its owner (PipeWire)
}

// RealtimeKit (org.freedesktop.RealtimeKit1, on the system bus) raises a thread of a process whose
// limits don't let it raise itself, as it does PipeWire's. "" or why not
std::string rtkit(const char *method, uint64_t tid, bool realtime, int value) {
    using namespace reserve;
    if (!load()) return "no libdbus";
    DBusError e;
    d.error_init(&e);
    Conn *c = d.bus_get_private(1 /* system */, &e);
    if (!c) return d.error_free(&e), "no system bus";
    d.set_exit_on_disconnect(c, 0);
    Msg *m = d.new_method_call("org.freedesktop.RealtimeKit1", "/org/freedesktop/RealtimeKit1", "org.freedesktop.RealtimeKit1", method);
    int32_t i = value;
    uint32_t u = (uint32_t)value;
    if (realtime) d.append_args(m, 't', &tid, 'u', &u, 0);
    else d.append_args(m, 't', &tid, 'i', &i, 0);
    Msg *r = d.send_block(c, m, 1000, &e);
    d.unref(m);
    std::string why = r ? "" : e.message ? e.message : "no answer";
    if (r) d.unref(r);
    d.error_free(&e);
    d.close(c);
    d.conn_unref(c);
    return why;
}

// The calling thread real-time (SCHED_FIFO 50: under PipeWire's own, 88, over KWin's, 1), itself
// if RLIMIT_RTPRIO allows, else through RealtimeKit (its usual most, 20); failing both, as high as
// SDL gets a thread. RLIMIT_RTTIME at 200 ms (RealtimeKit's condition): a real-time thread that ran
// that long without sleeping would end the game rather than hold its CPU. What it got
std::string realtime_thread() {
#ifdef __linux__
    rlimit rt{};
    if (getrlimit(RLIMIT_RTTIME, &rt) == 0 && (rt.rlim_max == RLIM_INFINITY || rt.rlim_max > 200000)) {
        rt.rlim_cur = rt.rlim_max = 200000;
        setrlimit(RLIMIT_RTTIME, &rt);
    }
    rlimit rp{};
    getrlimit(RLIMIT_RTPRIO, &rp);
    const int most = rp.rlim_cur == RLIM_INFINITY ? 99 : (int)rp.rlim_cur;
    sched_param sp{};
    sp.sched_priority = std::min(50, most);
    if (sp.sched_priority > 0 && sched_setscheduler(0, SCHED_FIFO | SCHED_RESET_ON_FORK, &sp) == 0)
        return "real-time (FIFO " + std::to_string(sp.sched_priority) + ")";
    if (rtkit("MakeThreadRealtime", (uint64_t)gettid(), true, 20).empty()) return "real-time (RR 20, through RealtimeKit)";
#endif
    SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_TIME_CRITICAL);
    return "at SDL's time-critical priority (real-time not allowed)";
}

int period() { return g_period; }
bool lost() { return g_lost.load(std::memory_order_relaxed); }

std::string describe() {
    if (!g_pcm || !g_t0) return "";
    const double secs = (prof::now_ns() - g_t0) / 1e9;
    char b[320];
    snprintf(b, sizeof b,
             "audio: the card took %.1f %% of its rate's frames over %.0f s (100: it plays what it's given), %llu underruns; "
             "its writer %s",
             secs > 0 ? 100.0 * (double)g_written.load() / (secs * g_card_rate) : 0.0, secs,
             (unsigned long long)g_xruns.load(), g_writer.c_str());
    return b;
}

}  // namespace hk::host::alsa
