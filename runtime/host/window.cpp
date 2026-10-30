// SPDX-License-Identifier: GPL-3.0-or-later
#include "window.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <cstdlib>
#include <thread>
#ifdef __linux__
#include <sched.h>
#include <sys/prctl.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "../shell/core.h"
#include "macro.h"
#include "pacing.h"
#include "alsa.h"
#include "asio.h"
#include "wasapi.h"
#include "lampview.h"
#include "shaderview.h"
#include "notice.h"
#include "warp.h"
#include "picture.h"
#include "input.h"
#include "../core/prof.h"
#ifdef _WIN32
#include "dxgi.h"
#endif

namespace hk::host {

namespace {
uint64_t g_settle_until = 0;   // window/renderer setup right after opening: not a stall
std::atomic<uint64_t> g_device_pulls{0}, g_device_short{0};   // the device's pulls, and those the queue fell short of
std::atomic<uint64_t> g_device_short_late{0};                  // ... of those, after the game's boot (g_tune_from)
uint64_t g_tune_from = 0;          // the queue target self-tunes from here on (window_open + 10 s)

// window_set_split: each of the frame's monitors (Core::screens) in a window of its own
bool g_split_wanted = false, g_split = false;
SDL_Window *g_win2 = nullptr;     // the second monitor's window (player 2's)
SDL_Renderer *g_ren2 = nullptr;   // ... its SDL renderer, without OpenGL
SDL_Texture *g_tex2 = nullptr;
std::vector<uint32_t> g_half[2];  // the frame's two halves, when split

// A new window: 480 lines a scale step, as wide as the core's monitor shape makes them (640 for
// 4:3; IIDX's 16:9 tube, 853), or one of its monitors' when split
static int window_width(int scale) {
    double a = core().display_aspect();
    if (a <= 0) a = 4.0 / 3.0;
    if (g_split) a /= core().screens();
    return (int)std::lround(480.0 * scale * a);
}
SDL_Window *g_win = nullptr;
SDL_Renderer *g_ren = nullptr;
SDL_Texture *g_tex = nullptr;
SDL_GLContext g_gl = nullptr;     // OpenGL (the picture modes, runtime/host/picture.h), else the SDL renderer
std::string g_shaders_dir, g_title, g_macro;   // g_macro: the code macro being typed, for the title
bool g_title_dirty = false;
std::vector<std::string> g_picture_config;
picture::Mode g_mode = picture::Mode::off, g_shown = picture::Mode::off;
SDL_AudioStream *g_audio = nullptr;
int g_tw = 0, g_th = 0;
SDL_FRect g_dst{};                // where the frame goes in the logical presentation (SDL renderer path)
std::chrono::steady_clock::time_point g_t0;
bool g_started = false;

int g_rate = 44100;               // the core's audio rate (Core::audio_rate)
std::string g_audio_device;       // window_set_audio_output: an ALSA device to have alone ("": the default);
                                  // Windows: an output device by name
bool g_audio_exclusive = false;   // Windows: WASAPI exclusive mode (wasapi.h)
std::string g_audio_asio;         // Windows: an ASIO driver (asio.h)
int g_audio_buffer = 0;           // ... and the device buffer in frames (0: 128)
std::string g_audio_output;       // what it got, for the report
int g_target_frames = 0;          // audio queued ahead, when audio paces
int g_target_min = 0;             // ... the starting setting: self-tuning keeps it at least this
bool g_audio_fixed = false;       // window_set_audio_fixed: no self-tuning
bool g_fast = false;              // Tab held, or the fast start-up (host/warp.h): run as fast as possible (audio is dropped)
bool g_tab = false;               // Tab held
int g_device_frames = 0;
bool g_quit = false;
bool g_display_sync = true;       // display pacing (pacing.h)
bool g_smooth_motion = false;     // --smooth-motion: the core's in-between pictures on the frame's other refreshes
uint64_t g_in_between = 0;        // how many it showed (the exit report)
bool g_beam = false, g_beam_ok = false;   // --crt-beam, and its shader built
float g_beam_gamma = 2.2f, g_beam_gain = 0.7f;
uint64_t g_beam_frames = 0, g_beam_without = 0;   // frames through it, and not (the exit report)
const char *g_beam_why = nullptr;                 // the last reason a frame went without
double g_ratio = 1.0;             // the audio stream's frequency ratio: the game's speed, steered
// Display sync off, and the core hands its sound on once per frame (Core::audio_per_frame): the
// window waits for the sound after showing a frame, before the next one runs (window_frame), and
// push_chunk doesn't wait. Waiting in push_chunk, which the core calls just before the frame hook,
// put that wait between a frame's ~2 ms of emulation and its picture: a frame (16.7 ms) from the
// controls being read to the picture, where display sync takes a few milliseconds.
bool g_frame_paced = false;
uint64_t g_frame_start_ns = 0;    // when the window last let a frame run (the controls just read)
uint64_t g_last_push_end = 0;     // the host's work since then is what must fit (push_chunk)
int g_queued_after = 0;           // frames queued at g_last_push_end
bool g_log_stalls = getenv("HK_LOG_STALLS") != nullptr;   // each stall, break and second's lead, to stderr

// The queue target's self-tuning (unless window_set_audio_fixed). As each chunk comes, the lead it
// needed: the target, less what was left of the queue, plus a device buffer (the device takes
// whole buffers: less than one left can break the sound). The target is what covers all but the
// two worst seconds of the last minute, never below the starting setting, at most 30 ms: it grows
// for late frames that keep coming and comes back down when they stop, and a stall no sensible
// lead covers (a rare one) breaks the sound once instead of adding to every keysound for the rest
// of the run.
struct LeadTuner {
    static constexpr int SECS = 60, SPARED = 2;   // a minute, but for its 2 worst seconds
    float worst[SECS] = {};                       // each second's largest needed lead, ms
    uint64_t sec_of[SECS] = {};                   // the second each slot holds (+1; 0: none)
    int lo = 0, hi = 0;                           // the target's range over the run (frames)
    uint64_t cur = 0;
    void add(uint64_t now_ns, float need) {
        const double target_ms = g_target_frames * 1000.0 / g_rate;
        const uint64_t sec = now_ns / 1'000'000'000ull + 1;
        const int i = (int)(sec % SECS);
        if (sec_of[i] != sec) sec_of[i] = sec, worst[i] = 0;
        const bool past = need > worst[i] && need > target_ms;
        worst[i] = std::max(worst[i], need);
        if (g_log_stalls && sec != cur && cur)
            fprintf(stderr, "lead: second %llu needed %.2f ms, target %.2f ms\n", (unsigned long long)cur, worst[cur % SECS], target_ms);
        if (past || sec != cur) {   // a lead past the target, or a new second (one falls out of the minute)
            cur = sec;
            retune(sec);
        }
    }
    void retune(uint64_t sec) {
        float all[SECS];   // seconds not seen yet (the first minute of tuning) count as needing nothing
        for (int i = 0; i < SECS; i++) all[i] = sec_of[i] && sec_of[i] + SECS > sec ? worst[i] : 0.0f;
        std::nth_element(all, all + SPARED, all + SECS, std::greater<float>());
        const int frames = (int)std::ceil(all[SPARED] * g_rate / 1000.0);
        g_target_frames = std::clamp(frames, g_target_min, std::max(g_target_min, g_rate * 30 / 1000));
        lo = std::min(lo, g_target_frames);
        hi = std::max(hi, g_target_frames);
    }
} g_tuner;

// The run's three latest chunks (after the first 10 s): the lead each needed (LeadTuner) and where
// the time since the chunk before went, for the exit report
struct LateChunks {
    struct Late {
        float need = 0, gap = 0, wait = 0, cat[prof::NCAT] = {};
        uint64_t frame = 0;
    } worst[3];
    uint64_t wait_mark = 0;   // prof::g_wait_ticks at the chunk before
    void add(float need, double gap) {
        if (need > worst[2].need) {
            Late l;
            l.need = need, l.gap = (float)gap, l.frame = core().frame_count();
            l.wait = (float)((prof::g_wait_ticks - wait_mark) * prof::ns_per_tick() / 1e6);
            for (int c = 0; c < prof::NCAT; c++) l.cat[c] = (float)(prof::ns((prof::Cat)c) / 1e6);
            int k = 2;
            for (; k > 0 && need > worst[k - 1].need; k--) worst[k] = worst[k - 1];
            worst[k] = l;
        }
    }
} g_late;


// With g_frame_paced, each picture is held until a steady time after its frame started: the slow
// end of recent frames' emulation and drawing (as display sync's burst budget, pacing.cpp), so the
// pictures keep the game's cadence while a frame's work varies (pop'n 9: 1-8 ms with the GS).
struct FrameBudget {
    std::vector<int64_t> samples = std::vector<int64_t>(256, 0);
    size_t at = 0;
    int64_t budget = 0;           // ns; 0 until 64 frames are in
    void add(int64_t ns) {
        samples[at++ % samples.size()] = ns;
        if (at % 64) return;      // the 99th percentile of the recent ones, plus 0.5 ms
        std::vector<int64_t> s(samples.begin(), samples.begin() + (long)std::min(at, samples.size()));
        std::nth_element(s.begin(), s.begin() + (long)(s.size() * 99 / 100), s.end());
        budget = std::min(s[s.size() * 99 / 100] + 500'000, (int64_t)12'000'000);
    }
    // the picture is ready: wait until its time
    void hold(uint64_t start_ns) {
        if (!start_ns) return;
        const int64_t ready = (int64_t)(prof::now_ns() - start_ns);
        add(ready);
        if (budget > ready) {
            const uint64_t w0 = prof::ticks();
            SDL_DelayPrecise((Uint64)(budget - ready));
            prof::g_wait_ticks += prof::ticks() - w0;
        }
    }
} g_frame_budget;
double g_last_vsec = -1;
void (*g_glFlush)() = nullptr;
auto g_last_poll = std::chrono::steady_clock::now();

// Poll controls: called every few milliseconds of emulated time.
void poll() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED ||   // (either window, when split)
            (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE))
            g_quit = true;
        if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat &&
            (e.key.key == SDLK_F11 || (e.key.key == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT)))) {
            SDL_Window *w = SDL_GetWindowFromEvent(&e);   // the window with the keyboard
            if (!w) w = g_win;
            bool fs = SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN;
            SDL_SetWindowFullscreen(w, !fs);
        }
        if ((e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP) && e.key.key == SDLK_TAB) {
            g_tab = e.type == SDL_EVENT_KEY_DOWN;
            const bool was = g_fast;
            g_fast = g_tab || warp::on();
            if (g_fast && !was && g_audio) SDL_ClearAudioStream(g_audio);
        }
        if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && (e.key.mod & SDL_KMOD_CTRL) &&
            e.key.key >= SDLK_1 && e.key.key <= SDLK_9) {   // Ctrl+1..9: the game's code macros
            macro::trigger((size_t)(e.key.key - SDLK_1));
            continue;   // not a game input
        }
        if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_F9) {   // F9: the game's lights
            lampview::toggle();
            continue;
        }
        if (shaderview::event(e, g_mode, g_gl != nullptr))   // F8: the shader's settings (its controls: not the game's)
            continue;
        if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_F10 && g_gl)   // F10: off, light, full, lola
            g_mode = g_mode == picture::Mode::off     ? picture::Mode::light
                   : g_mode == picture::Mode::light ? picture::Mode::full
                   : g_mode == picture::Mode::full  ? picture::Mode::lola
                                                    : picture::Mode::off;
        if (e.type == SDL_EVENT_WINDOW_DISPLAY_CHANGED || e.type == SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED)
            pacing::display_changed();
        // no pointer over the game in fullscreen (it would also keep the compositor from
        // scanning the picture out directly, where the pointer isn't a plane of its own)
        if (e.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN) SDL_HideCursor();
        if (e.type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN) SDL_ShowCursor();
        if (e.type == SDL_EVENT_JOYSTICK_ADDED) input_joystick_added(e.jdevice.which);
        if (e.type == SDL_EVENT_JOYSTICK_REMOVED) input_joystick_removed(e.jdevice.which);
        input_event(e);
    }
    auto now = std::chrono::steady_clock::now();
    input_update(std::chrono::duration<double, std::milli>(now - g_last_poll).count());
    g_last_poll = now;
}

// An OpenGL 3.3 window; false (and nothing left open) if it can't run here.
bool open_gl(const char *title, int scale) {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
    // opaque, 8 bits a channel: a buffer the compositor can scan out directly in fullscreen,
    // skipping its own composition (and a frame's worth of its latency)
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
    g_win = SDL_CreateWindow(title, window_width(scale), 480 * scale, SDL_WINDOW_RESIZABLE | SDL_WINDOW_OPENGL);
    if (g_win) g_gl = SDL_GL_CreateContext(g_win);
    if (g_gl) SDL_GL_SetSwapInterval(0);   // as with the renderer: presenting never waits for the display
    if (g_gl && picture::init(g_shaders_dir, g_picture_config)) {
        picture::set_monitor(core().monitor_khz());
        if (g_beam) {
            g_beam_ok = picture::set_beam(true, {g_beam_gamma, g_beam_gain, core().monitor_turned(), core().screens()});
            if (!g_beam_ok) fprintf(stderr, "picture: the CRT beam's shader doesn't build here: no beam\n");
        }
        return true;
    }
    fprintf(stderr, "picture: no OpenGL 3.3 (%s): the SDL renderer, sharp pixels only\n", g_gl ? "see above" : SDL_GetError());
    picture::shutdown();
    if (g_gl) SDL_GL_DestroyContext(g_gl);
    if (g_win) SDL_DestroyWindow(g_win);
    g_gl = nullptr;
    g_win = nullptr;
    return false;
}
}  // namespace

void window_set_picture(const std::string &mode, const std::string &shaders_dir, const std::vector<std::string> &config_dirs) {
    if (!picture::parse_mode(mode, g_mode)) fprintf(stderr, "picture: %s is not off, light, full or lola\n", mode.c_str());
    g_shaders_dir = shaders_dir;
    g_picture_config = config_dirs;
}

void window_set_display_sync(bool on) { g_display_sync = on; }
void window_set_lights_overlay(bool on) { lampview::set_shown(on); }
bool window_set_shader_key(const std::string &spec) {
    const size_t eq = spec.find('=');
    return eq != std::string::npos && shaderview::set_control(spec.substr(0, eq), spec.substr(eq + 1));
}
void window_set_smooth_motion(bool on) { g_smooth_motion = on, pacing::set_smooth_motion(on || g_beam); }
void window_set_beam(bool on, float gamma, float gain) {
    g_beam = on, g_beam_gamma = gamma, g_beam_gain = gain;
    pacing::set_smooth_motion(on || g_smooth_motion);   // the game at its own speed, every refresh a picture
    pacing::set_whole_refreshes(on);                     // ... the same refreshes every frame where it nearly fits
}

void window_set_audio_output(const std::string &device, int frames, bool exclusive) {
    g_audio_device = device;
    g_audio_buffer = frames;
    g_audio_exclusive = exclusive;
}

void window_set_audio_asio(const std::string &driver) { g_audio_asio = driver; }

namespace {
float g_volume = 1.0f;
std::vector<float> g_out;   // audio_push's samples after the DC blocker and the volume
double g_dc_x[2], g_dc_y[2];
}
void window_set_volume(double db) { g_volume = (float)std::pow(10.0, db / 20.0); }
void window_set_audio_fixed(bool on) { g_audio_fixed = on; }
void window_set_split(bool on) { g_split_wanted = on; }

namespace {
// When split: player 2's window beside the first; with two displays or more, player 1's on the
// leftmost and player 2's on the next (where the system lets a program place its windows: not
// on Wayland, where they're moved by hand), each then fullscreen there if asked
bool open_second(const std::string &title, int scale, bool fullscreen) {
    const int ww = window_width(scale), wh = 480 * scale;
    if (g_gl) {
        g_win2 = SDL_CreateWindow((title + " - 2P").c_str(), ww, wh, SDL_WINDOW_RESIZABLE | SDL_WINDOW_OPENGL);
        if (!g_win2 || !SDL_GL_MakeCurrent(g_win2, g_gl)) return false;
        SDL_GL_SetSwapInterval(0);
        SDL_GL_MakeCurrent(g_win, g_gl);
    } else {
        if (!SDL_CreateWindowAndRenderer((title + " - 2P").c_str(), ww, wh, SDL_WINDOW_RESIZABLE, &g_win2, &g_ren2)) return false;
        SDL_SetRenderVSync(g_ren2, 0);
    }
    SDL_SetWindowSize(g_win, ww, wh);
    int n = 0;
    SDL_DisplayID *ds = SDL_GetDisplays(&n);
    std::vector<std::pair<int, SDL_DisplayID>> byx;
    for (int k = 0; ds && k < n; k++) {
        SDL_Rect r;
        if (SDL_GetDisplayBounds(ds[k], &r)) byx.push_back({r.x, ds[k]});
    }
    SDL_free(ds);
    std::sort(byx.begin(), byx.end());
    if (byx.size() >= 2) {
        SDL_SetWindowPosition(g_win, SDL_WINDOWPOS_CENTERED_DISPLAY(byx[0].second), SDL_WINDOWPOS_CENTERED_DISPLAY(byx[0].second));
        SDL_SetWindowPosition(g_win2, SDL_WINDOWPOS_CENTERED_DISPLAY(byx[1].second), SDL_WINDOWPOS_CENTERED_DISPLAY(byx[1].second));
    } else {
        int x, y;
        SDL_GetWindowPosition(g_win, &x, &y);
        SDL_SetWindowPosition(g_win2, x + ww + 8, y);
    }
    if (fullscreen) SDL_SetWindowFullscreen(g_win2, true);
    g_title_dirty = true;   // " - 1P" on the first
    fprintf(stderr, "window: each monitor in its own window (%d displays)\n", (int)byx.size());
    return true;
}
}  // namespace


bool g_prio_rtkit = false;   // raise_priority went through RealtimeKit: window_open raises the other threads

// Feral's GameMode, if installed: asked for while the game runs (the CPU governor at performance
// and whatever else its gamemode.ini says), through its own library, as games that use it do
int (*g_gamemode_end)() = nullptr;
static std::string gamemode_start() {
#ifdef __linux__
    SDL_SharedObject *lib = SDL_LoadObject("libgamemode.so.0");
    if (!lib) return "";   // not installed
    auto start = reinterpret_cast<int (*)()>(SDL_LoadFunction(lib, "real_gamemode_request_start"));
    auto error = reinterpret_cast<const char *(*)()>(SDL_LoadFunction(lib, "real_gamemode_error_string"));
    g_gamemode_end = reinterpret_cast<int (*)()>(SDL_LoadFunction(lib, "real_gamemode_request_end"));
    if (!start) return "GameMode: its library has no real_gamemode_request_start";
    if (start() < 0) {
        g_gamemode_end = nullptr;
        return std::string("GameMode: refused (") + (error ? error() : "?") + ")";
    }
    return "GameMode: on while the game runs";
#else
    return "";
#endif
}

#ifdef __linux__
// The process's threads at normal priority raised through RealtimeKit, as raise_priority raised the
// one that asked: a thread it raises starts its own threads at normal priority again
// (SCHED_RESET_ON_FORK). Those that lowered themselves (Mesa's, at 19) or run real-time are left
static void raise_threads_rtkit(int nice) {
    std::error_code ec;
    for (const auto &e : std::filesystem::directory_iterator("/proc/self/task", ec)) {
        const pid_t tid = (pid_t)atoi(e.path().filename().c_str());
        if (tid <= 0 || sched_getscheduler(tid) != SCHED_OTHER) continue;
        errno = 0;
        if (getpriority(PRIO_PROCESS, (id_t)tid) == 0 && errno == 0) alsa::rtkit("MakeThreadHighPriority", (uint64_t)tid, false, nice);
    }
}
#endif

void preload_media() {
#ifdef __linux__
    // the files the machine has open now (its media), and the binary itself (the recompiled code is
    // most of it: a page first run is otherwise read from the disk then, a seek on a hard disk)
    std::vector<std::string> files;
    std::error_code ec;
    if (auto exe = std::filesystem::canonical("/proc/self/exe", ec); !ec) files.push_back(exe.string());
    for (const auto &e : std::filesystem::directory_iterator("/proc/self/fd", ec)) {
        auto t = std::filesystem::read_symlink(e.path(), ec);
        if (ec || t.empty() || t.string()[0] != '/' || t.string().rfind("/dev/", 0) == 0 || t.string().rfind("/proc/", 0) == 0 ||
            t.string().rfind("/sys/", 0) == 0)
            continue;
        if (!std::filesystem::is_regular_file(t, ec) || std::find(files.begin(), files.end(), t.string()) != files.end()) continue;
        files.push_back(t.string());
    }
    std::thread([files] {
        setpriority(PRIO_PROCESS, (id_t)gettid(), 19);   // behind everything; the disk too: idle class (bfq, mq-deadline honour it)
        syscall(SYS_ioprio_set, 1 /* IOPRIO_WHO_PROCESS */, (int)gettid(), 3 << 13 /* IOPRIO_CLASS_IDLE */);
        constexpr uint64_t MOST = 1ull << 30;   // a file up to 1 GB (a disc's or a ROM's; not a hard disk's image)
        // and locked there where RLIMIT_MEMLOCK allows (a PC short of memory otherwise takes the pages
        // back: under memory pressure, a page of code run the first time in minutes was read again)
        rlimit ml{};
        getrlimit(RLIMIT_MEMLOCK, &ml);
        uint64_t budget = ml.rlim_cur == RLIM_INFINITY ? UINT64_MAX : (uint64_t)ml.rlim_cur;
        if (const long pages = sysconf(_SC_PHYS_PAGES); pages > 0) budget = std::min(budget, (uint64_t)pages * 4096 / 16);   // at most 1/16 of the RAM
        const uint64_t t0 = prof::now_ns();
        uint64_t total = 0, locked = 0;
        int n = 0;
        std::vector<char> buf(4 << 20);
        for (size_t k = 0; k < files.size(); k++) {
            const std::string &f = files[k];
            std::error_code e2;
            const uint64_t size = std::filesystem::file_size(f, e2);
            if (e2 || size == 0 || size > MOST) continue;
            bool done = false;
            if (k == 0 && locked + size <= budget) {   // the binary: its own mappings (code and data), locked
                if (FILE *maps = fopen("/proc/self/maps", "r")) {
                    char line[4096];
                    uint64_t got = 0;
                    while (fgets(line, sizeof line, maps)) {
                        unsigned long a, b;
                        char perm[8];
                        int at = 0;
                        if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %n", &a, &b, perm, &at) < 3 || !at) continue;
                        std::string path = line + at;
                        while (!path.empty() && (path.back() == '\n' || path.back() == ' ')) path.pop_back();
                        if (path == f && perm[0] == 'r' && mlock((void *)a, b - a) == 0) got += b - a;
                    }
                    fclose(maps);
                    if (got) locked += got, done = true;
                }
            } else if (k > 0 && locked + size <= budget) {   // a medium: mapped, locked, kept for the run
                if (int fd = open(f.c_str(), O_RDONLY | O_CLOEXEC); fd >= 0) {
                    void *p = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
                    close(fd);
                    if (p != MAP_FAILED && mlock(p, size) == 0) locked += size, done = true;
                    else if (p != MAP_FAILED) munmap(p, size);
                }
            }
            if (!done) {   // read into the page cache, at least
                FILE *fp = fopen(f.c_str(), "rb");
                if (!fp) continue;
                while (fread(buf.data(), 1, buf.size(), fp) == buf.size()) {}
                fclose(fp);
            }
            total += size, n++;
        }
        fprintf(stderr, "preload: %d files, %.0f MB (the game's binary and media up to 1 GB) in memory in %.1f s, %.0f MB of it "
                        "locked there\n", n, total / 1e6, (prof::now_ns() - t0) / 1e9, locked / 1e6);
    }).detach();
#endif
}

std::string raise_priority() {
    if (std::string gm = gamemode_start(); !gm.empty()) fprintf(stderr, "%s\n", gm.c_str());
#ifdef __linux__
    // timer slack: a sleep may otherwise end up to 50 us late (the kernel's default, to batch wake-ups)
    prctl(PR_SET_TIMERSLACK, 1UL, 0UL, 0UL, 0UL);
    constexpr int WANT = -11;   // ahead of other programs; the sound server and the compositor are elsewhere
    rlimit rl{};
    getrlimit(RLIMIT_NICE, &rl);
    const int lowest = rl.rlim_cur == RLIM_INFINITY ? -20 : 20 - (int)rl.rlim_cur;   // RLIMIT_NICE's floor
    const pid_t tid = gettid();
    std::string via = "its own limits";
    if (lowest < 0) setpriority(PRIO_PROCESS, (id_t)tid, std::max(WANT, lowest));
    if (getpriority(PRIO_PROCESS, (id_t)tid) > WANT && alsa::rtkit("MakeThreadHighPriority", (uint64_t)tid, false, WANT).empty())
        via = "RealtimeKit", g_prio_rtkit = true;
    const int got = getpriority(PRIO_PROCESS, (id_t)tid);
    if (got >= 0) return "priority: normal (neither its limits nor RealtimeKit let it go higher); timer slack 1 us";
    return "priority: nice " + std::to_string(got) + " for the game's threads (through " + via + "); timer slack 1 us";
#elif defined(_WIN32)
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
    return "priority: above normal";
#else
    return "priority: normal";
#endif
}

#ifdef _WIN32
// The window's pictures through a DXGI flip-model swap chain (dxgi.h), where the driver lets
// OpenGL draw into it; else OpenGL's own swap, as before
static void start_dxgi() {
    void *hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(g_win), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(g_win, &w, &h);
    std::string why;
    if (hwnd && dxgi::start(hwnd, w, h, why)) fprintf(stderr, "video: presented through a DXGI flip-model swap chain\n");
    else fprintf(stderr, "video: OpenGL's own swap (no DXGI flip-model swap chain: %s)\n", hwnd ? why.c_str() : "no window handle");
    picture::set_window_fbo(dxgi::fbo());
}
#endif

// The window's picture to the screen: through the DXGI swap chain on Windows, else OpenGL's swap.
// sync: display pacing's (each picture in turn); extra: one of a frame's other refreshes' pictures
// (dxgi::present may leave it out)
static void swap_window(bool sync, bool extra = false) {
#ifdef _WIN32
    if (dxgi::on()) {
        dxgi::present(sync, extra);
        return;
    }
#endif
    (void)sync, (void)extra;
    SDL_GL_SwapWindow(g_win);
}

static bool open_window(const char *title, int scale, const std::string &bindings, int target_ms, bool fullscreen, int rate) {
    g_rate = rate;
    // the device's buffer: the sound server's quantum (PipeWire: node.latency)
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, std::to_string(g_audio_buffer > 0 ? g_audio_buffer : 128).c_str());
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) return false;
    input_init(bindings);
    SDL_AudioSpec spec{SDL_AUDIO_F32, 2, g_rate};
#ifdef _WIN32
    if (!g_audio_asio.empty()) {   // the card through its own ASIO driver: no Windows audio in between
        std::string what, why = asio::open(g_audio_asio, spec, g_audio_buffer, g_audio, what);
        if (why.empty()) {
            g_audio_output = "ASIO: " + what;
            g_device_frames = asio::period_frames();
        } else {
            fprintf(stderr, "audio: ASIO driver \"%s\": %s; Windows' audio instead\n", g_audio_asio.c_str(), why.c_str());
        }
    }
    if (!g_audio && g_audio_exclusive) {   // the device alone, in WASAPI exclusive mode: no system mixer in between
        std::string what, why = wasapi::open(g_audio_device, spec, g_audio_buffer > 0 ? g_audio_buffer : 128, g_audio, what);
        if (why.empty()) {
            g_audio_output = (g_audio_device.empty() ? std::string("the default output") : g_audio_device) +
                             " (alone, WASAPI exclusive: " + what + ")";
            g_device_frames = wasapi::period_frames();
        } else {
            fprintf(stderr, "audio: exclusive mode on %s: %s; shared mode instead\n",
                    g_audio_device.empty() ? "the default output" : g_audio_device.c_str(), why.c_str());
        }
    }
    if (!g_audio && !g_audio_device.empty()) {   // that device, shared: SDL's WASAPI output, by its name
        int n = 0;
        SDL_AudioDeviceID *ids = SDL_GetAudioPlaybackDevices(&n);
        for (int k = 0; ids && k < n && !g_audio; k++)
            if (const char *name = SDL_GetAudioDeviceName(ids[k]); name && g_audio_device == name) {
                g_audio = SDL_OpenAudioDeviceStream(ids[k], &spec, nullptr, nullptr);
                if (g_audio) g_audio_output = g_audio_device + std::string(" (shared, ") + SDL_GetCurrentAudioDriver() + ")";
            }
        SDL_free(ids);
        if (!g_audio) fprintf(stderr, "audio: no output device named \"%s\"; the system's default output instead\n", g_audio_device.c_str());
    }
#else
    if (!g_audio_device.empty()) {   // the card itself, through ALSA: no sound server in between
        std::string what, why = alsa::open(g_audio_device, spec, g_audio_buffer > 0 ? g_audio_buffer : 128, g_audio, what);
        if (why.empty()) {
            g_audio_output = g_audio_device + " (the card alone, through ALSA: " + what + ")";
            g_device_frames = alsa::period();
        } else {   // busy (the sound server has it open while anything plays), or no such device
            fprintf(stderr, "audio: %s: %s; the system's default output instead (the sound server lets go of a card "
                            "a few seconds after the last sound on it stops)\n", g_audio_device.c_str(), why.c_str());
        }
    }
#endif
    if (!g_audio) g_audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (g_audio_output.empty() && g_audio) g_audio_output = std::string("the system's default (") + SDL_GetCurrentAudioDriver() + ")";
    if (g_audio) {
        SDL_AudioSpec dev{};
        if (SDL_GetAudioStreamDevice(g_audio)) SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(g_audio), &dev, &g_device_frames);
        g_target_frames = g_target_min = g_rate * target_ms / 1000;
        g_tuner.lo = g_tuner.hi = g_target_frames;
        // start with the target lead of silence: the first frames' window setup
        // would otherwise find an empty queue (and grow the target for nothing)
        std::vector<float> silence((size_t)g_target_frames * 2, 0.0f);
        SDL_PutAudioStreamData(g_audio, silence.data(), (int)(silence.size() * sizeof(float)));
        // the device's own view: a pull the queue can't fill breaks the sound. The queue level
        // when a chunk arrives misses most of these: with a core whose sound comes once per frame
        // the queue runs dry between chunks and is full again when the next one is counted
        SDL_SetAudioStreamGetCallback(g_audio, [](void *, SDL_AudioStream *s, int, int total) {
            g_device_pulls.fetch_add(1, std::memory_order_relaxed);
            const uint64_t t = prof::now_ns();
            if (SDL_GetAudioStreamAvailable(s) < total && !g_fast && t >= g_settle_until) {
                g_device_short.fetch_add(1, std::memory_order_relaxed);
                if (t >= g_tune_from) g_device_short_late.fetch_add(1, std::memory_order_relaxed);
            }
        }, nullptr);
        SDL_ResumeAudioStreamDevice(g_audio);
        g_settle_until = prof::now_ns() + 250'000'000;
        g_tune_from = prof::now_ns() + 10'000'000'000ull;
    }
    g_title = title;
    g_split = g_split_wanted && core().screens() == 2;
    if (g_split_wanted && !g_split) fprintf(stderr, "window: this machine has one monitor: one window\n");
    macro::on_change([](const std::string &name) {
        g_macro = name;
        g_title_dirty = true;
    });
    if (open_gl(title, scale)) {
        picture::prepare(g_mode);
        if (g_split && !open_second(title, scale, fullscreen)) {
            fprintf(stderr, "window: no second window (%s): one window\n", SDL_GetError());
            g_split = false;
        }
        if (fullscreen) SDL_SetWindowFullscreen(g_win, true);
        g_glFlush = (void (*)())SDL_GL_GetProcAddress("glFlush");
#ifdef _WIN32
        start_dxgi();
#endif
        pacing::init(g_win, g_display_sync);
        return true;
    }
    if (!SDL_CreateWindowAndRenderer(title, window_width(scale), 480 * scale, SDL_WINDOW_RESIZABLE, &g_win, &g_ren)) return false;
    SDL_SetRenderVSync(g_ren, 0);   // presenting must never block on the display: we choose when (pacing.h)
    SDL_SetRenderLogicalPresentation(g_ren, 640, 480, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    if (g_split && !open_second(title, scale, fullscreen)) {
        fprintf(stderr, "window: no second window (%s): one window\n", SDL_GetError());
        g_split = false;
    }
    if (fullscreen) SDL_SetWindowFullscreen(g_win, true);
    pacing::init(g_win, g_display_sync);
    return true;
}

bool window_open(const char *title, int scale, const std::string &bindings, int target_ms, bool fullscreen, int rate) {
    const bool ok = open_window(title, scale, bindings, target_ms, fullscreen, rate);
    g_fast = g_tab || warp::on();   // the fast start-up's from the first sound on
#ifdef __linux__
    if (ok && g_prio_rtkit) raise_threads_rtkit(getpriority(PRIO_PROCESS, (id_t)gettid()));   // the threads it has by now
#endif
    return ok;
}

// Present pacing: real time between presents, against the machine's own frame period. With the
// display not waiting (vsync off), this is what a variable-refresh display shows; a fixed one
// shows each frame for a whole number of its refreshes (docs/PLAY.md, "Frame pacing").
struct PresentStats {
    uint64_t n = 0, last_ns = 0;
    double last_vsec = 0, sum = 0, sum2 = 0, worst = 0;   // deviation from the period, ms
    uint64_t hist[6] = {};   // |deviation| <0.5, <1, <2, <4, <8, >=8 ms
    void add(double vsec) {
        uint64_t t = prof::now_ns();
        if (last_ns && !g_fast && t >= g_settle_until) {
            double d = (t - last_ns) / 1e6 - (vsec - last_vsec) * 1e3, a = std::fabs(d);
            n++, sum += d, sum2 += d * d, worst = std::max(worst, a);
            hist[a < 0.5 ? 0 : a < 1 ? 1 : a < 2 ? 2 : a < 4 ? 3 : a < 8 ? 4 : 5]++;
        }
        last_ns = t, last_vsec = vsec;
    }
} g_present;

// From the window letting a frame run (the controls just read: the games read theirs at the
// frame's start) to that frame's picture handed to the display, in 0.1 ms steps up to 50 ms.
// The display adds its own on top (pacing.h: commit to screen).
struct StartToPicture {
    uint64_t hist[501] = {}, n = 0;
    double sum = 0;
    void add(uint64_t start_ns) {
        const uint64_t t = prof::now_ns();
        if (!start_ns || g_fast || t < g_settle_until) return;
        const double ms = (t - start_ns) / 1e6;
        hist[std::min(500, (int)(ms * 10))]++, n++, sum += ms;
    }
    double percentile(double q) const {
        uint64_t need = (uint64_t)(q * (double)n), acc = 0;
        for (int k = 0; k <= 500; k++)
            if ((acc += hist[k]) > need) return (k + 1) / 10.0;
        return 50.0;
    }
} g_start_to_picture;

// From the window letting a frame run (the controls read) to that moment's sound entering the
// sound device: the sound queued ahead of it, and how far the sound handed on lags the machine
// (MAME's System 573 and Twinkle hand theirs on 50 times a second, up to 20 ms behind, unless
// --fine-sound). The device's buffer comes on top. In 0.1 ms steps up to 100 ms.
uint64_t g_machine_frames = 0;   // the machine's sound frames handed on (queued, or dropped by fast-forward)
struct StartToSound {
    uint64_t hist[1001] = {}, n = 0;
    double sum = 0, behind_sum = 0, behind_max = 0;
    void add(double ms, double behind) {
        hist[std::clamp((int)(ms * 10), 0, 1000)]++, n++, sum += ms;
        behind_sum += behind, behind_max = std::max(behind_max, behind);
    }
    double percentile(double q) const {
        uint64_t need = (uint64_t)(q * (double)n), acc = 0;
        for (int k = 0; k <= 1000; k++)
            if ((acc += hist[k]) > need) return (k + 1) / 10.0;
        return 100.0;
    }
} g_start_to_sound;

// Display pacing: steer the sound's playback rate to the game's speed, keeping the queue at its
// target where it is lowest (as the burst starts). The device's clock is not the system's (a card's
// crystal: 0.1-0.3 % off): a part learnt over seconds (g_steer_i) takes that up, or the queue settles
// short of its target by as much as it does (a card 0.25 % off: at half of it), and the lead tuner,
// reading the shortfall as late frames, raised the target, and with it the shortfall, to 30 ms.
double g_steer_i = 0;
void steer_audio() {
    if (!g_audio) return;
    double q_ms = SDL_GetAudioStreamQueued(g_audio) / 8 * 1000.0 / (g_rate * g_ratio);
    double target_ms = g_target_frames * 1000.0 / g_rate;
    if (q_ms < target_ms / 2) {   // starting, or after a stall: fill up at once, not over seconds
        std::vector<float> silence((size_t)((target_ms - q_ms) * g_rate * g_ratio / 1000) * 2, 0.0f);
        SDL_PutAudioStreamData(g_audio, silence.data(), (int)(silence.size() * sizeof(float)));
        q_ms = target_ms;
    }
    if (q_ms > target_ms * 3 + 50) {   // far too long (the steering below takes minutes): back to the target at once
        SDL_ClearAudioStream(g_audio);
        std::vector<float> silence((size_t)(target_ms * g_rate * g_ratio / 1000) * 2, 0.0f);
        SDL_PutAudioStreamData(g_audio, silence.data(), (int)(silence.size() * sizeof(float)));
        q_ms = target_ms;
    }
    double e = std::clamp((q_ms - target_ms) / target_ms, -1.0, 1.0);
    g_steer_i = std::clamp(g_steer_i + 0.00002 * e, -0.005, 0.005);   // a frame at a time: 0.12 % a second at e = 1 (60 Hz)
    double r = pacing::speed() * (1.0 + 0.005 * e + g_steer_i);
    if (g_log_stalls) {
        static uint64_t last = 0;
        if (const uint64_t t = prof::now_ns(); t - last > 1'000'000'000ull)
            last = t, fprintf(stderr, "steer: queue %.2f ms, target %.2f, ratio %.5f (learnt %+.4f %%)\n", q_ms, target_ms, r, g_steer_i * 100);
    }
    if (std::fabs(r - g_ratio) > 1e-6) SDL_SetAudioStreamFrequencyRatio(g_audio, (float)r), g_ratio = r;
}

// The shape a frame is shown at: the core's monitor, or the frame's own with square pixels
static double display_aspect(int w, int h) {
    const double a = core().display_aspect();
    return a > 0 ? a : (double)w / h;
}

bool g_sync = false;   // display pacing for the frame last presented

// host/notice.h's banner: whole times its size (as large as a 360-line screen's), centred near
// the window's bottom
static void place_banner(const notice::Banner &b, int win_w, int win_h, int &x, int &y, int &dw, int &dh) {
    int k = std::max(1, win_h / 360);
    while (k > 1 && b.w * k > win_w - 16) k--;
    dw = b.w * k;
    dh = b.h * k;
    x = (win_w - dw) / 2;
    y = win_h - dh - 8 * k;
}

// host/lampview.h's panel: top left, whole times its size, up to 60 % of the window's width
static void place_lights(const lampview::Panel &p, int win_w, int win_h, int &x, int &y, int &dw, int &dh) {
    int k = std::max(1, std::min(win_h / 540, win_w * 6 / 10 / std::max(1, p.w)));
    dw = p.w * k;
    dh = p.h * k;
    x = y = 8 * k;
}

// host/warp.h's box: whole times its size (as the banner's), in the middle of the window
static void place_warp(const warp::Panel &p, int win_w, int win_h, int &x, int &y, int &dw, int &dh) {
    int k = std::max(1, win_h / 360);
    while (k > 1 && p.w * k > win_w - 16) k--;
    dw = p.w * k;
    dh = p.h * k;
    x = (win_w - dw) / 2;
    y = (win_h - dh) / 2;
}

// The overlays over a picture in the GL window: the banner (host/notice.h) and the lights
static void draw_overlays(int ww, int wh) {
    if (warp::Panel p; warp::panel(p)) {   // the fast start-up's box (host/warp.h): in the middle
        int px, py, pw, ph;
        place_warp(p, ww, wh, px, py, pw, ph);
        picture::overlay(p.pixels->data(), p.w, p.h, px, py, pw, ph, p.alpha, ww, wh);
    }
    if (notice::Banner b; notice::banner(b)) {
        int bx, by, bw, bh;
        place_banner(b, ww, wh, bx, by, bw, bh);
        picture::overlay(b.pixels->data(), b.w, b.h, bx, by, bw, bh, b.alpha, ww, wh);
    }
    if (lampview::Panel p; lampview::panel(p)) {
        int px, py, pw, ph;
        place_lights(p, ww, wh, px, py, pw, ph);
        picture::overlay(p.pixels->data(), p.w, p.h, px, py, pw, ph, 1.0f, ww, wh);
    }
    // host/shaderview.h's panel: at the top right (or left), whole times its size, as the lights'
    if (shaderview::Panel p; shaderview::panel(p, g_mode)) {
        const int k = std::max(1, std::min(wh / 540, ww * 4 / 10 / std::max(1, p.w)));
        const int pw = p.w * k, ph = p.h * k;
        const int px = shaderview::on_left() ? 8 * k : std::max(0, ww - pw - 8 * k);
        picture::overlay(p.pixels->data(), p.w, p.h, px, 8 * k, pw, ph, 1.0f, ww, wh);
    }
}

// The second monitor's half in player 2's window (window_set_split), after the first window's
// picture: nothing waits here, the first window's pacing stands for both
static void present_second(const uint32_t *rgb, int w, int h, double aspect, uint64_t frame) {
    if (g_gl) {
        SDL_GL_MakeCurrent(g_win2, g_gl);
        int ww = 0, wh = 0;
        SDL_GetWindowSizeInPixels(g_win2, &ww, &wh);
        int vw = ww, vh = (int)std::lround(ww / aspect);
        if (vh > wh) vh = wh, vw = (int)std::lround(wh * aspect);
        picture::set_window_fbo(0);   // (this window's own: the DXGI swap chain is the first's)
        picture::draw(rgb, w, h, (ww - vw) / 2, (wh - vh) / 2, std::max(1, vw), std::max(1, vh), ww, wh, g_mode, 1, frame);
        SDL_GL_SwapWindow(g_win2);
        SDL_GL_MakeCurrent(g_win, g_gl);
#ifdef _WIN32
        picture::set_window_fbo(dxgi::fbo());
#endif
        return;
    }
    static int tw = 0, th = 0;
    if (!g_tex2 || w != tw || h != th) {
        if (g_tex2) SDL_DestroyTexture(g_tex2);
        g_tex2 = SDL_CreateTexture(g_ren2, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
        SDL_SetTextureScaleMode(g_tex2, SDL_SCALEMODE_NEAREST);
        SDL_SetRenderLogicalPresentation(g_ren2, std::max(1, (int)std::lround(h * aspect)), h, SDL_LOGICAL_PRESENTATION_LETTERBOX);
        tw = w, th = h;
    }
    const SDL_FRect dst{(float)((std::lround(h * aspect) - w) / 2), 0, (float)w, (float)h};
    SDL_UpdateTexture(g_tex2, nullptr, rgb, w * 4);
    SDL_RenderClear(g_ren2);
    SDL_RenderTexture(g_ren2, g_tex2, nullptr, &dst);
    SDL_RenderPresent(g_ren2);
}

void window_present(const uint32_t *rgb, int w, int h, double vsec) {
    // split: each half (one monitor) in its own window, at half the frame's shape
    double frame_aspect = display_aspect(w, h);
    const int full_w = w;
    if (g_split) {
        const int hw = w / 2;
        for (int k = 0; k < 2; k++) {
            g_half[k].resize((size_t)hw * h);
            for (int y = 0; y < h; y++)
                memcpy(&g_half[k][(size_t)y * hw], rgb + (size_t)y * full_w + (size_t)k * hw, (size_t)hw * 4);
        }
        rgb = g_half[0].data();
        w = hw;
        frame_aspect /= 2;
    }
    const double period = g_last_vsec < 0 || g_fast ? 0.0 : vsec - g_last_vsec;
    g_last_vsec = vsec;
    const bool sync = pacing::frame(period) && !g_fast;
    g_sync = sync;
    // the fast start-up (host/warp.h): its pictures at most 60 a second, the rest not shown
    if (warp::on()) {
        static uint64_t last_ns = 0;
        const uint64_t t = prof::now_ns();
        if (t - last_ns < 16000000) return;
        last_ns = t;
    }
    // --smooth-motion at the game's own speed (pacing::exact_speed): a frame that falls between two
    // refreshes, the game faster than the display, isn't shown
    if (sync && pacing::refreshes_per_frame() == 0) return;
    if (g_gl) {
        prof::Scope ps(prof::PRESENT);
        int ww = 0, wh = 0;
        SDL_GetWindowSizeInPixels(g_win, &ww, &wh);
#ifdef _WIN32
        dxgi::resize(ww, wh);   // the swap chain's buffers at the window's size (fullscreen, a resize)
        picture::set_window_fbo(dxgi::fbo());
#endif
        // letterboxed at the monitor's shape (Core::display_aspect), as SDL_LOGICAL_PRESENTATION_LETTERBOX
        // does: a 512x480 menu fills the same 4:3 as a 320x240 game
        const double aspect = frame_aspect;
        int vw = ww, vh = (int)std::lround(ww / aspect);
        if (vh > wh) vh = wh, vw = (int)std::lround(wh * aspect);
        // and of that, the part the frame covers (Core::display_cover)
        double cw, ch;
        core().display_cover(w, h, cw, ch);
        vw = std::max(1, (int)std::lround(vw * cw)), vh = std::max(1, (int)std::lround(vh * ch));
        // --crt-beam: each of the frame's refreshes is the beam's pass at its moment (picture.h), with
        // two refreshes a frame or more (120 Hz and up): the display's rate over the game's decides,
        // not the frame's own count (2 or 3 at 144 Hz), so it doesn't come and go from frame to frame
        const double frame_refreshes = sync ? pacing::refreshes_in_frame() : 1;
        const bool beam = g_beam && g_beam_ok && sync && !g_split && frame_refreshes >= 1.9 && pacing::can_fill_refreshes();
        if (g_beam && !beam) {
            g_beam_without++;
            g_beam_why = !g_beam_ok ? "its shader doesn't build here"
                         : !sync ? "display sync is off (or fast-forward)"
                         : g_split ? "the monitors are in windows of their own"
                         : frame_refreshes < 1.9 ? "the display is under twice the game's rate: it needs 120 Hz or more"
                                                 : "this display can't take a picture every refresh (Wayland fifo-v1 or Windows' DWM)";
        }
        if (beam) g_beam_frames++;
        const double beam_fph = beam ? frame_refreshes : 0;
        // --smooth-motion at the game's own speed: the frame's first refresh comes after its moment
        // (up to a refresh), so it too is shown carried on to the refresh's
        const bool exact = !beam && sync && g_smooth_motion && !g_split && pacing::exact_speed();
        const uint32_t *first = rgb;
        int fw = w, fh = h;
        // (at its moment too: what scrolls is drawn on the scroll's steady line, up to a pixel off
        // the frame's own place: hiraku/scroll_guess.h)
        if (exact)
            if (const uint32_t *b = core().render_between(pacing::phase(0), fw, fh)) first = b;
        // the game's frame number: the shaders' FrameCount, and what makes the in-between pictures
        // below refreshes of this frame for them (picture.h)
        const uint64_t number = core().frame_count();
        picture::Mode shown = picture::draw(first, first == rgb ? w : fw, first == rgb ? h : fh, (ww - vw) / 2, (wh - vh) / 2,
                                            vw, vh, ww, wh, g_mode, 0, number, beam ? pacing::phase(0) : -1, beam_fph);
        draw_overlays(ww, wh);
        if (sync) {
            if (g_glFlush) g_glFlush();   // the GPU draws while we wait for the commit time
            pacing::wait_commit();
            pacing::commit();
        } else if (g_frame_paced && !g_fast) {
            if (g_glFlush) g_glFlush();
            g_frame_budget.hold(g_frame_start_ns);
        }
        if (!sync) pacing::observe();
        swap_window(sync);
        g_start_to_picture.add(g_frame_start_ns);
        // --smooth-motion: the frame's other refreshes each with a picture of its own, the core's
        // in-between one (what moves further along its move), instead of the frame held
        const int k = sync ? pacing::refreshes_per_frame() : 1;
        // A refresh where nothing has moved a whole pixel yet (render_between gives nullptr) shows the
        // frame itself, and the next may still move (a slow note: 1 pixel a frame is 0 at a quarter, 1
        // at a half). The frame is held as it is only when neither the first refresh nor the last
        // (the furthest on) has anything moved, or the core can't
        int bw = w, bh = h;
        const bool can = !beam && g_smooth_motion && !g_split && k > 1 && pacing::can_fill_refreshes();
        const uint32_t *between = can ? core().render_between(pacing::phase(1), bw, bh) : nullptr;
        bool fill = beam || between != nullptr;
        if (can && !fill && k > 2) {
            int lw, lh;
            fill = core().render_between(pacing::phase(k - 1), lw, lh) != nullptr;
        }
        if (sync) pacing::committed(!fill);
        for (int i = 1; fill && i < k; i++) {
            if (beam) {   // the same frame, the beam further down
                picture::draw(rgb, w, h, (ww - vw) / 2, (wh - vh) / 2, vw, vh, ww, wh, g_mode, 0, number, pacing::phase(i), beam_fph);
                draw_overlays(ww, wh);
                pacing::commit_extra(i);
                swap_window(true, true);
                continue;
            }
            if (i > 1) bw = w, bh = h, between = core().render_between(pacing::phase(i), bw, bh);
            if (between) g_in_between++;
            else between = rgb, bw = w, bh = h;
            picture::draw(between, bw, bh, (ww - vw) / 2, (wh - vh) / 2, vw, vh, ww, wh, g_mode, 0, number);
            draw_overlays(ww, wh);
            pacing::commit_extra(i);
            swap_window(true, true);
        }
        if (g_split) present_second(g_half[1].data(), w, h, frame_aspect, number);
        if (shown != g_shown) {
            g_shown = shown;
            g_title_dirty = true;
        }
    } else if (!g_tex || w != g_tw || h != g_th) {
        if (g_tex) SDL_DestroyTexture(g_tex);
        g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
        SDL_SetTextureScaleMode(g_tex, SDL_SCALEMODE_NEAREST);
        // the logical size at the monitor's shape (as above), the frame's own part of it in g_dst
        double cw, ch;
        core().display_cover(w, h, cw, ch);
        const float lh = (float)(h / ch), lw = (float)std::lround(lh * frame_aspect);
        SDL_SetRenderLogicalPresentation(g_ren, std::max(1, (int)lw), std::max(1, (int)std::lround(lh)),
                                         SDL_LOGICAL_PRESENTATION_LETTERBOX);
        g_dst = {(lw - (float)(lw * cw)) / 2, (lh - (float)h) / 2, (float)(lw * cw), (float)h};
        g_tw = w;
        g_th = h;
    }
    if (!g_gl) {
        prof::Scope ps(prof::PRESENT);
        SDL_UpdateTexture(g_tex, nullptr, rgb, w * 4);
        SDL_RenderClear(g_ren);
        SDL_RenderTexture(g_ren, g_tex, nullptr, &g_dst);
        if (notice::Banner b; notice::banner(b)) {
            // in the window's own pixels, not the game's logical size
            static SDL_Texture *bt = nullptr;
            static unsigned bt_id = 0;
            if (bt && bt_id != b.id) SDL_DestroyTexture(bt), bt = nullptr;
            if (!bt && (bt = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, b.w, b.h))) {
                bt_id = b.id;
                SDL_UpdateTexture(bt, nullptr, b.pixels->data(), b.w * 4);
                SDL_SetTextureBlendMode(bt, SDL_BLENDMODE_BLEND);
                SDL_SetTextureScaleMode(bt, SDL_SCALEMODE_NEAREST);
            }
            int lw, lh, ow, oh;
            SDL_RendererLogicalPresentation lm;
            if (bt && SDL_GetRenderLogicalPresentation(g_ren, &lw, &lh, &lm)) {
                SDL_SetRenderLogicalPresentation(g_ren, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);
                SDL_GetCurrentRenderOutputSize(g_ren, &ow, &oh);
                int bx, by, bw, bh;
                place_banner(b, ow, oh, bx, by, bw, bh);
                const SDL_FRect r{(float)bx, (float)by, (float)bw, (float)bh};
                SDL_SetTextureAlphaModFloat(bt, b.alpha);
                SDL_RenderTexture(g_ren, bt, nullptr, &r);
                SDL_SetRenderLogicalPresentation(g_ren, lw, lh, lm);
            }
        }
        if (lampview::Panel p; lampview::panel(p)) {   // the lights, likewise
            static SDL_Texture *lt = nullptr;
            static unsigned lt_id = 0;
            static int lt_w = 0, lt_h = 0;
            if (lt && (lt_w != p.w || lt_h != p.h)) SDL_DestroyTexture(lt), lt = nullptr;
            if (!lt && (lt = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, p.w, p.h))) {
                lt_w = p.w, lt_h = p.h, lt_id = p.id - 1;
                SDL_SetTextureBlendMode(lt, SDL_BLENDMODE_BLEND);
                SDL_SetTextureScaleMode(lt, SDL_SCALEMODE_NEAREST);
            }
            if (lt && lt_id != p.id) SDL_UpdateTexture(lt, nullptr, p.pixels->data(), p.w * 4), lt_id = p.id;
            int lw, lh, ow, oh;
            SDL_RendererLogicalPresentation lm;
            if (lt && SDL_GetRenderLogicalPresentation(g_ren, &lw, &lh, &lm)) {
                SDL_SetRenderLogicalPresentation(g_ren, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);
                SDL_GetCurrentRenderOutputSize(g_ren, &ow, &oh);
                int px, py, pw, ph;
                place_lights(p, ow, oh, px, py, pw, ph);
                const SDL_FRect r{(float)px, (float)py, (float)pw, (float)ph};
                SDL_RenderTexture(g_ren, lt, nullptr, &r);
                SDL_SetRenderLogicalPresentation(g_ren, lw, lh, lm);
            }
        }
        if (warp::Panel p; warp::panel(p)) {   // the fast start-up's box, likewise
            static SDL_Texture *wt = nullptr;
            static unsigned wt_id = 0;
            if (wt && wt_id != p.id) SDL_DestroyTexture(wt), wt = nullptr;
            if (!wt && (wt = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, p.w, p.h))) {
                wt_id = p.id;
                SDL_UpdateTexture(wt, nullptr, p.pixels->data(), p.w * 4);
                SDL_SetTextureBlendMode(wt, SDL_BLENDMODE_BLEND);
                SDL_SetTextureScaleMode(wt, SDL_SCALEMODE_NEAREST);
            }
            int lw, lh, ow, oh;
            SDL_RendererLogicalPresentation lm;
            if (wt && SDL_GetRenderLogicalPresentation(g_ren, &lw, &lh, &lm)) {
                SDL_SetRenderLogicalPresentation(g_ren, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);
                SDL_GetCurrentRenderOutputSize(g_ren, &ow, &oh);
                int px, py, pw, ph;
                place_warp(p, ow, oh, px, py, pw, ph);
                const SDL_FRect r{(float)px, (float)py, (float)pw, (float)ph};
                SDL_SetTextureAlphaModFloat(wt, p.alpha);
                SDL_RenderTexture(g_ren, wt, nullptr, &r);
                SDL_SetRenderLogicalPresentation(g_ren, lw, lh, lm);
            }
        }
        if (sync) {
            SDL_FlushRenderer(g_ren);
            pacing::wait_commit();
            pacing::commit();
        } else if (g_frame_paced && !g_fast) {
            SDL_FlushRenderer(g_ren);
            g_frame_budget.hold(g_frame_start_ns);
        }
        if (!sync) pacing::observe();
        SDL_RenderPresent(g_ren);
        g_start_to_picture.add(g_frame_start_ns);
        if (sync) pacing::committed();
        if (g_split) present_second(g_half[1].data(), w, h, frame_aspect, core().frame_count());
    }
    if (!sync) g_present.add(vsec);

    // a text display the game drives (IIDX's 16-segment ticker): in the title while it shows something
    static std::string display;
    if (std::string d = core().display_text(); d != display) {
        display = d;
        g_title_dirty = true;
    }
    if (g_title_dirty && g_win) {
        g_title_dirty = false;
        std::string t = g_title;
        if (display.find_first_not_of(' ') != std::string::npos) t += " - [" + display + "]";
        if (g_shown != picture::Mode::off) t += std::string(" - ") + picture::mode_label(g_shown);
        if (!g_macro.empty()) t += " - typing: " + g_macro;
        SDL_SetWindowTitle(g_win, (g_split ? t + " - 1P" : t).c_str());
        if (g_win2) SDL_SetWindowTitle(g_win2, (t + " - 2P").c_str());
    }
}

// a frame starts at machine time vsec: where its sound will be (g_start_to_sound)
static void sound_lag(double vsec) {
    if (!g_audio || g_fast || prof::now_ns() < g_settle_until || !g_machine_frames) return;
    const double queued = SDL_GetAudioStreamQueued(g_audio) / 8 * 1000.0 / (g_rate * g_ratio);
    const double behind = std::max(0.0, (vsec - (double)g_machine_frames / g_rate) * 1000.0 / g_ratio);
    g_start_to_sound.add(queued + behind, behind);
}

bool window_next(double vsec) {
    const bool sync = g_sync;
    if (warp::on() && !warp::frame(vsec)) g_fast = g_tab;   // the fast start-up's end
    poll();
    g_frame_start_ns = 0;
    g_frame_paced = false;
    if (g_quit) return false;
    if (g_fast) return true;    // fast-forward: no pacing at all
    if (sync) {                 // the display's grid paces time; the sound follows
        pacing::wait_next();
        poll();
        pacing::pump();
        steer_audio();
        g_frame_start_ns = prof::now_ns();
        sound_lag(vsec);
        return !g_quit;
    }
    if (g_audio && g_ratio != 1.0) SDL_SetAudioStreamFrequencyRatio(g_audio, 1.0f), g_ratio = 1.0;
    if (g_audio && core().audio_per_frame()) {
        // the sound paces time, as in audio_push, but here: until the queue is down to its
        // target, then the next frame runs (with this frame's controls read last) and is shown
        // as soon as it's drawn. The queue's lowest point is the same as waiting in push_chunk.
        g_frame_paced = true;
        const uint64_t w0 = prof::ticks();
        while (SDL_GetAudioStreamQueued(g_audio) > g_target_frames * 8 && !g_quit) {
            poll();
            SDL_DelayPrecise(250'000);   // 0.25 ms
        }
        prof::g_wait_ticks += prof::ticks() - w0;
        poll();
        g_frame_start_ns = g_last_push_end = prof::now_ns();   // the wait isn't host work
        g_queued_after = SDL_GetAudioStreamQueued(g_audio) / 8;
        g_late.wait_mark = prof::g_wait_ticks;
        sound_lag(vsec);
        return !g_quit;
    }
    if (g_audio) {              // audio paces time (audio_push)
        g_frame_start_ns = prof::now_ns();
        sound_lag(vsec);
        return true;
    }

    // no audio device: pace virtual time to real time on video frames
    auto now = std::chrono::steady_clock::now();
    if (!g_started) { g_t0 = now - std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(vsec)); g_started = true; }
    auto due = g_t0 + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(vsec));
    if (due > now) {
        const uint64_t w0 = prof::ticks();
        std::this_thread::sleep_until(due);
        prof::g_wait_ticks += prof::ticks() - w0;
    }
    else if (now - due > std::chrono::milliseconds(250)) g_t0 += now - due;   // fell behind: don't try to catch up
    g_frame_start_ns = prof::now_ns();
    return true;
}

bool window_frame(const uint32_t *rgb, int w, int h, double vsec) {
    window_present(rgb, w, h, vsec);
    return window_next(vsec);
}

// Underrun accounting: the queue level when each chunk arrives, and what the
// host did since the previous chunk when it ran low.
struct AudioStats {
    uint64_t chunks = 0, low = 0, empty = 0;
    double worst_gap_ms = 0;
    uint64_t gap_hist[8] = {};   // work time per chunk: <1,<2,<4,<8,<16,<32,<64,>=64 ms
    uint64_t low_ns[prof::NCAT] = {};
} g_ast;

// g_device_short(_late) as the last chunk came: the breaks the sound came back from (the report's;
// when the run ends, the device keeps pulling from an empty queue until the window closes)
uint64_t g_breaks = 0, g_breaks_late = 0;

// One chunk to the device, with pacing, underrun accounting and input polling.
void push_chunk(const float *lr, int frames) {
    g_machine_frames += (uint64_t)frames;
    if (g_fast) {               // fast-forward: no pacing, and the time isn't a stall
        poll();
        g_last_push_end = 0;
        return;
    }
    uint64_t t = prof::now_ns();
    if (g_last_push_end && t >= g_settle_until) {
        double gap = (t - g_last_push_end) / 1e6;
        int b = gap < 1 ? 0 : gap < 2 ? 1 : gap < 4 ? 2 : gap < 8 ? 3 : gap < 16 ? 4 : gap < 32 ? 5 : gap < 64 ? 6 : 7;
        g_ast.gap_hist[b]++;
        g_ast.worst_gap_ms = std::max(g_ast.worst_gap_ms, gap);
        int queued = SDL_GetAudioStreamQueued(g_audio) / 8;
        g_ast.chunks++;
        if (queued == 0) g_ast.empty++;
        // what was left of the queue as this chunk came (the device takes it in whole buffers, so
        // estimated from the queue after the last chunk and the time since)
        const double left = g_queued_after * 1000.0 / (g_rate * g_ratio) - gap;
        const float need = (float)(g_target_frames * 1000.0 / g_rate - left + g_device_frames * 1000.0 / g_rate);
        if (t >= g_tune_from) {
            if (!g_audio_fixed && gap < 100) g_tuner.add(t, need);
            g_late.add(need, gap);
        }
        if (g_log_stalls)
            if (const uint64_t s = g_device_short.load(std::memory_order_relaxed); s != g_breaks) {
                fprintf(stderr, "break: frame %llu, %llu short pulls, %.2f ms since the last chunk, queue after it %.2f ms, "
                                "target %.1f ms\n", (unsigned long long)core().frame_count(), (unsigned long long)(s - g_breaks),
                        gap, g_queued_after * 1000.0 / g_rate, g_target_frames * 1000.0 / g_rate);
            }
        if (queued < g_rate / 1000) {   // under 1 ms left: about to crackle
            g_ast.low++;
            for (int c = 0; c < prof::NCAT; c++) g_ast.low_ns[c] += (uint64_t)prof::ns((prof::Cat)c);
            if (g_log_stalls) {
                fprintf(stderr, "stall: frame %llu, %.2f ms of work, queue %d frames:", (unsigned long long)core().frame_count(), gap, queued);
                for (int c = 0; c < prof::NCAT; c++) fprintf(stderr, " %s %.2f", prof::NAMES[c], prof::ns((prof::Cat)c) / 1e6);
                fprintf(stderr, "\n");
            }
        }
    }
    g_breaks = g_device_short.load(std::memory_order_relaxed);
    g_breaks_late = g_device_short_late.load(std::memory_order_relaxed);
    SDL_PutAudioStreamData(g_audio, lr, frames * 8);
    // audio is the clock: wait while more than the target is queued, polling controls
    // (with display pacing, the display is the clock, and with a core whose sound comes once
    // per frame the window waits after the frame: window_frame waits instead)
    const uint64_t w0 = prof::ticks();
    while (!pacing::active() && !g_frame_paced && SDL_GetAudioStreamQueued(g_audio) > g_target_frames * 8 && !g_quit) {
        poll();
        SDL_DelayPrecise(250'000);   // 0.25 ms
    }
    prof::g_wait_ticks += prof::ticks() - w0;
    poll();
    prof::reset();
    g_late.wait_mark = prof::g_wait_ticks;
    g_last_push_end = prof::now_ns();
    g_queued_after = SDL_GetAudioStreamQueued(g_audio) / 8;
}

// The emulated sound hardware may deliver a few samples at a time (the SPU catching
// up on every DMA word while a song loads); batch them so pacing and input polling
// run once per >= 64 frames (1.5 ms), not per sample.
std::vector<float> g_pending;

void audio_push(const float *lr, int frames) {
    if (!g_audio) {   // before the window opens (Firebeat's skipped BIOS time): still the machine's sound
        g_machine_frames += (uint64_t)frames;
        return;
    }
    {   // DC blocker, 1-pole high-pass at 5 Hz (y = x - x1 + R y1), then the volume, held to full scale
        const double r = 1.0 - 6.283185307179586 * 5.0 / g_rate;
        g_out.resize(2 * (size_t)frames);
        for (int i = 0; i < 2 * frames; i++) {
            const int c = i & 1;
            const double y = lr[i] - g_dc_x[c] + r * g_dc_y[c];
            g_dc_x[c] = lr[i];
            g_dc_y[c] = y;
            g_out[i] = std::clamp((float)y * g_volume, -1.0f, 1.0f);
        }
        lr = g_out.data();
    }
    if (g_pending.empty() && frames >= 64) { push_chunk(lr, frames); return; }
    g_pending.insert(g_pending.end(), lr, lr + 2 * frames);
    if (g_pending.size() >= 2 * 64) {
        push_chunk(g_pending.data(), (int)(g_pending.size() / 2));
        g_pending.clear();
    }
}

std::string audio_describe() {
    if (!g_audio) return "no audio device";
    char b[4096], tuned[160];
    if (g_audio_fixed) snprintf(tuned, sizeof tuned, "fixed");
    else snprintf(tuned, sizeof tuned, "self-tuning: %.1f to %.1f ms over the run", g_tuner.lo * 1000.0 / g_rate, g_tuner.hi * 1000.0 / g_rate);
    int n = snprintf(b, sizeof b,
                     "audio: %s, device buffer %d frames (%.1f ms), queue target at the end %d frames (%.1f ms; %s)\n"
                     "audio: %llu chunks, %llu ran low (<1 ms queued), %llu found the queue empty; worst gap %.1f ms\n"
                     "audio: the device pulled %llu times, %llu of them more than was queued (a break in the sound), %llu after the first 10 s\n"
                     "audio: host work per chunk  <1:%llu <2:%llu <4:%llu <8:%llu <16:%llu <32:%llu <64:%llu >=64:%llu ms\n"
                     "audio: during low chunks:",
                     g_audio_output.c_str(), g_device_frames, g_device_frames * 1000.0 / g_rate, g_target_frames, g_target_frames * 1000.0 / g_rate, tuned,
                     (unsigned long long)g_ast.chunks, (unsigned long long)g_ast.low, (unsigned long long)g_ast.empty,
                     g_ast.worst_gap_ms, (unsigned long long)g_device_pulls.load(), (unsigned long long)g_breaks,
                     (unsigned long long)g_breaks_late,
                     (unsigned long long)g_ast.gap_hist[0], (unsigned long long)g_ast.gap_hist[1],
                     (unsigned long long)g_ast.gap_hist[2], (unsigned long long)g_ast.gap_hist[3],
                     (unsigned long long)g_ast.gap_hist[4], (unsigned long long)g_ast.gap_hist[5],
                     (unsigned long long)g_ast.gap_hist[6], (unsigned long long)g_ast.gap_hist[7]);
    for (int c = 0; c < prof::NCAT; c++)
        n += snprintf(b + n, sizeof b - n, " %s %.0f ms", prof::NAMES[c], g_ast.low_ns[c] / 1e6);
    if (g_late.worst[0].need > 0) {
        n += snprintf(b + n, sizeof b - n, "\naudio: the latest chunks (the lead each needed; the time since the chunk "
                                           "before, the pacing's waits in it, and the host's work, present including waits):");
        for (const LateChunks::Late &l : g_late.worst) {
            if (l.need <= 0) break;
            n += snprintf(b + n, sizeof b - n, "%s frame %llu %.1f ms (%.1f, waiting %.1f", &l == g_late.worst ? "" : ";",
                          (unsigned long long)l.frame, l.need, l.gap, l.wait);
            for (int c = 0; c < prof::NCAT; c++)
                if (l.cat[c] >= 0.05f) n += snprintf(b + n, sizeof b - n, ", %s %.1f", prof::NAMES[c], l.cat[c]);
            n += snprintf(b + n, sizeof b - n, ")");
        }
    }
#ifdef _WIN32
    if (wasapi::active()) n += snprintf(b + n, sizeof b - n, "\n%s", wasapi::describe().c_str());
    if (asio::active()) n += snprintf(b + n, sizeof b - n, "\n%s", asio::describe().c_str());
#endif
    if (const std::string card = alsa::describe(); !card.empty())   // the card's own pace
        n += snprintf(b + n, sizeof b - n, "\n%s", card.c_str());
    if (g_present.n) {
        const PresentStats &p = g_present;
        double mean = p.sum / p.n, sd = std::sqrt(std::max(0.0, p.sum2 / p.n - mean * mean));
        snprintf(b + n, sizeof b - n,
                 "\nvideo: %llu presents, time between them minus the frame period: mean %+.3f ms, sd %.2f ms, worst %.2f ms; "
                 "|off| <0.5:%llu <1:%llu <2:%llu <4:%llu <8:%llu >=8:%llu ms",
                 (unsigned long long)p.n, mean, sd, p.worst, (unsigned long long)p.hist[0], (unsigned long long)p.hist[1],
                 (unsigned long long)p.hist[2], (unsigned long long)p.hist[3], (unsigned long long)p.hist[4],
                 (unsigned long long)p.hist[5]);
    }
    std::string out = std::string(b) + "\n" + pacing::describe();
#ifdef __linux__
    {   // what the CPU ran at by the end: GameMode, when allowed, sets the governor to performance
        auto read = [](const char *f) {
            std::string v;
            if (FILE *fp = fopen(f, "r")) {
                char line[64] = {};
                if (fgets(line, sizeof line, fp)) v = line;
                fclose(fp);
                while (!v.empty() && (v.back() == '\n' || v.back() == ' ')) v.pop_back();
            }
            return v;
        };
        const std::string gov = read("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
        const std::string epp = read("/sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference");
        if (!gov.empty())
            out += "\nsystem: CPU governor " + gov + (epp.empty() ? "" : ", energy preference " + epp) +
                   (!g_gamemode_end ? "" : gov == "performance" ? " (GameMode on)"
                                         : " (GameMode on, but it didn't change the governor: its gamemoded log says why; "
                                           "on most distributions the user must be in the gamemode group)");
    }
#endif
    if (g_smooth_motion && !g_beam) out += "\nvideo: smooth motion: " + std::to_string(g_in_between) + " in-between pictures shown";
    if (g_beam)
        out += "\nvideo: CRT beam: " + std::to_string(g_beam_frames) + " frames through it" +
               (g_beam_without ? ", " + std::to_string(g_beam_without) + " without (" + g_beam_why + ")" : std::string());
    if (std::string sc = pacing::scanout_describe(); !sc.empty()) out += "\n" + sc;
#ifdef _WIN32
    if (std::string d = dxgi::describe(); !d.empty()) out += "\n" + d;
#endif
    if (const StartToPicture &l = g_start_to_picture; l.n) {
        snprintf(b, sizeof b,
                 "\nvideo: from a frame starting (the controls read) to its picture handed to the display: mean %.1f ms, "
                 "p50 %.1f, p99 %.1f (%s)",
                 l.sum / (double)l.n, l.percentile(0.5), l.percentile(0.99),
                 pacing::active() ? "display sync"
                 : g_frame_paced  ? "the sound once per frame: waited for after the picture"
                                  : "the sound paces the machine as it comes");
        out += b;
        if (g_frame_paced && g_frame_budget.budget) {
            snprintf(b, sizeof b, ", each picture held to %.1f ms after its frame's start", g_frame_budget.budget / 1e6);
            out += b;
        }
    }
    if (const StartToSound &l = g_start_to_sound; l.n) {
        snprintf(b, sizeof b,
                 "\naudio: from a frame starting (the controls read) to that moment's sound entering the device: mean "
                 "%.1f ms, p50 %.1f, p99 %.1f; of it, the sound handed on behind the machine: mean %.1f, worst %.1f "
                 "(the device's buffer comes on top)",
                 l.sum / (double)l.n, l.percentile(0.5), l.percentile(0.99), l.behind_sum / (double)l.n, l.behind_max);
        out += b;
    }
    return out;
}

void window_close() {
    if (g_gamemode_end) g_gamemode_end(), g_gamemode_end = nullptr;   // (GameMode also notices a game that's gone)
    pacing::shutdown();
    alsa::close();
#ifdef _WIN32
    wasapi::close();
    asio::close();
#endif
    if (g_audio) SDL_DestroyAudioStream(g_audio);
    if (g_tex) SDL_DestroyTexture(g_tex);
    if (g_ren) SDL_DestroyRenderer(g_ren);
    if (g_tex2) SDL_DestroyTexture(g_tex2);
    if (g_ren2) SDL_DestroyRenderer(g_ren2);
    if (g_win2) SDL_DestroyWindow(g_win2);
    if (g_gl) {
#ifdef _WIN32
        dxgi::stop();
#endif
        picture::shutdown();
        SDL_GL_DestroyContext(g_gl);
    }
    if (g_win) SDL_DestroyWindow(g_win);
    SDL_Quit();
}

}  // namespace hk::host
