// SPDX-License-Identifier: GPL-3.0-or-later
// hiraku-<game>: the shell around a machine core (shell/core.h). It parses the
// command line, records pictures, sound and digests, runs input scripts, drives
// the window, controls and lamps, and decides when the run ends. The core
// (Firebeat, DJ Main, ...) is whichever this binary was linked with.
//
//   hiraku-<game> [options]
//   hiraku-<core> --game ID [options]   (a release's djmain, sys573, twinkle: one binary per core)
//     --frames N        stop after N video frames (default: run until killed)
//     --snap-every N    write every Nth frame as PPM into --out (default off)
//     --snap-from F     ... from frame F on
//     --snap-between N  with --smooth-motion, each snapshot's in-between pictures at 1/N, 2/N...
//                       of a frame (default: halfway only)
//     --out DIR         output directory for snapshots/trace (default out/run/<game>)
//     --trace           write an oracle-format MMIO trace (out/mmio.bin), see oracle/
//     --ata-log         log ATA/ATAPI commands
//     --script FILE     run an oracle input script (.his): same inputs, same frames as in MAME
//     --wav FILE        record the sound output (16-bit stereo, at the core's rate: 44.1 kHz, DJ Main 48 kHz)
//     --digest FILE     write one line per frame: frame number, a hash of the picture and a running
//                       hash of all sound so far (tools/regress.py checks runs are bit-identical with it)
//     --debug-every N   print the CPU state every N frames
//     --window [SCALE]  show the game in a window, in real time, with sound (SDL3 builds);
//                       F2 test, 9 service, 5 coin, 1 start, Esc quits; controls per cabinet
//     --fullscreen      start the window fullscreen (F11 toggles)
//     --display-sync on|off  on (the default): on a fixed-refresh display, show every frame for a
//                       whole number of refreshes, running the game at the display's rate (up to
//                       3 % off: pop'n 2.6 % fast at 60 Hz); off: the game's exact speed, frames
//                       shown as they come (for variable-refresh displays). runtime/host/pacing.h
//     --smooth-motion   with display sync, on any display (60, 144, 240 Hz): the game at its own
//                       speed, and every refresh shows what moves carried on to that refresh's
//                       moment (the core's in-between pictures: DJ Main's sprites, the Firebeat
//                       GCU's objects, the PlayStation GPU's primitives on Twinkle and System
//                       573) instead of frames held for
//                       uneven numbers of refreshes.
//                       Needs the compositor's fifo-v1 (KWin 6.7), or Windows (DWM's vblank).
//                       docs/SMOOTH.md
//     --crt-beam        the Blur Busters CRT beam simulator over the picture, on a display faster
//                       than the game (120 Hz and up; best at 240): each refresh lights the part of
//                       the tube the beam sweeps then, for a CRT's motion clarity. Needs what
//                       --smooth-motion does, and replaces its in-between pictures. docs/SMOOTH.md
//     --crt-beam-gamma G  the display's gamma for it (2.2; a mismatch shows as bands, washed out)
//     --crt-beam-gain X   its brightness against motion blur (0.7; 1 as bright as without it)
//     --lights-overlay  the game's lights over its picture: every lamp the core decodes, lit or not
//                       as the game drives it, and how often they change (F9 shows or hides it)
//     --macro NAME[@F]  run one of the game's code macros (games/<game>/macros.txt) at frame F, or
//                       at its own `at` frame (power-on codes); repeatable. In the window,
//                       Ctrl+1..9 types the 1st..9th macro
//     --picture MODE    off (sharp pixels, the default), light (CRT: crt-guest-advanced-fast),
//                       full (CRT: crt-royale) or lola (CRT, the analog chain: Lola Retro); F10
//                       cycles them. --crt is --picture light
//     --picture-config DIR  read DIR/<shader>.params over the shader's settings (repeatable,
//                       later ones win; `name = value` lines; read again when they change)
//     --shader-key ACTION=BINDING  a control of the shader's settings in the game (F8: show, up,
//                       down, less, more, saved, default, save, save_game), as --bindings spells it
//                       ("key:F8", "joy[DAO FPS]:button3"); repeatable; host/shaderview.h
//     --bindings FILE   controls (see runtime/host/input.h)
//     --lights FILE     drive HID controller LEDs from the cabinet lamps (see runtime/host/lights.h)
//     --lights-socket P publish the lamps on a UNIX socket at path P, or on TCP at HOST:PORT (docs/LIGHTS.md)
//     --lights-log FILE every lamp change, a line each: the frame, the machine's time, the lamp (its
//                       output number, as the F9 overlay shows it) and on or off; FILE "-" is the
//                       console (stderr). Headless too
//     --reader PATH, --scard, --cardio  real e-AMUSEMENT card readers, with --window (see
//                       runtime/host/readers.h)
//     --bio2 PATH       beatmania IIDX's BIO2 I/O board as the cabinet's controls and lamps, with
//                       --window (runtime/host/bio2.h)
//     --popn-io WHERE, --popn-io-dll FILE  a pop'n music cabinet's USB I/O board as its buttons
//                       and lamps, with --window (runtime/host/popnio.h)
//     --audio-ms N      the audio queue target (default 8), and its least: it tunes itself to the
//                       lead that covers all but the two worst seconds of the last minute (not in
//                       the first 10 s), up to 30 ms, and comes back down when late frames stop
//     --audio-fixed     the audio queue stays at --audio-ms (no self-tuning)
//     --normal-priority with --window, leave the game's threads at normal priority (by default they
//                       go ahead of other programs: Linux nice -11 and timer slack 1 us, as far as
//                       the limits or RealtimeKit allow; Windows above normal)
//     --audio-device D  the sound card itself, through ALSA, with no sound server in between: an ALSA
//                       device name such as hw:CARD=Generic,DEV=0 (aplay -l lists them; the launcher's
//                       Settings > Audio too). If it's busy, the system's default output instead
//                       Windows: an output device by the name Windows shows (shared, through the system
//                       mixer), or with --audio-exclusive alone
//     --split-screens   a machine with several monitors (Keyboardmania: one a player) shows each in
//                       a window of its own: player 2's on the next display, both fullscreen with
//                       --fullscreen (F11 toggles the window with the keyboard)
//     --mod FOLDER      a mod (docs/MODS.md: the folder's mod.toml), several in order: patches
//                       to the game's memory, replaced bytes or files on its media
//     --fast-boot       with --window, the game's start-up as fast as the machine can go (no sound,
//                       its pictures with a box over them) until its ready point (games/<id>/game.toml
//                       boot_seconds: the machine's seconds to its title or attract), or until a
//                       player presses one of its buttons. host/warp.h
//     --fast-boot-seconds S  ... up to S seconds of the machine's time instead (implies --fast-boot)
//     --volume DB       the sound out of the window, louder or quieter (e.g. 6 or -3; 0 by default):
//                       what the speakers get, not what --wav and --digest record
//     --audio-exclusive Windows: the output (--audio-device's, else the default one) in WASAPI exclusive
//                       mode: no system mixer in between, other programs silent meanwhile
//     --audio-asio NAME Windows: the sound card through its ASIO driver (the name its control panel and
//                       the launcher show; the error lists the installed ones). Else Windows' audio
//     --audio-buffer N  the device buffer in frames (default 128): the sound server's quantum, or with
//                       --audio-device the ALSA period (a ring of four), or the WASAPI exclusive buffer,
//                       or the ASIO buffer (default there: the driver's own). Smaller is sooner, until
//                       it crackles
//     --nvram DIR       keep the machine's NVRAM in DIR: loaded at start, saved on exit.
//                       Default with --window: ~/.local/share/hiraku/<game> (a portable install's
//                       user/data/<game>: shell/userdir.h); otherwise none (every run starts from
//                       the ROM set's defaults, so scripted runs are reproducible)
//   plus the core's own options (--help lists them).
//
// The machine is described by dumps/<game>/machine.txt and dumps/<game>/rom/,
// both written by tools/ingest.py from the user's own dumps.

#include <algorithm>
#include <atomic>
#include <csignal>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "core/prof.h"
#include "host/macro.h"
#include "host/mods.h"
#include "host/script.h"
#include "shell/cabinet.h"
#include "shell/built.h"
#include "shell/core.h"
#include "shell/userdir.h"
#include "shell/sys.h"
#ifdef HK_WITH_SDL
#include "host/lights.h"
#include "host/bio2.h"
#include "host/popnio.h"
#include "host/readers.h"
#include "host/notice.h"
#include "host/warp.h"
#include "host/window.h"
#include "host/pacing.h"
#endif

#ifndef HK_ROOT
#define HK_ROOT "."
#endif

using namespace hk;

namespace {

void write_ppm(const std::string &path, const uint32_t *rgb, int w, int h) {
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    std::vector<uint8_t> px((size_t)w * h * 3);
    for (size_t k = 0; k < (size_t)w * h; k++) {
        px[3 * k] = (uint8_t)(rgb[k] >> 16);
        px[3 * k + 1] = (uint8_t)(rgb[k] >> 8);
        px[3 * k + 2] = (uint8_t)rgb[k];
    }
    fwrite(px.data(), 1, px.size(), f);
    fclose(f);
}

// Host work per video frame: the time between frames, minus deliberate waits
// (audio pacing). This is what has to fit a frame on a slow PC; the worst
// frames, not the mean, decide whether the sound crackles.
struct FrameTimes {
    static constexpr int BINS = 2000;   // 0.05 ms each, up to 100 ms
    uint64_t hist[BINS + 1] = {};
    uint64_t n = 0, last = 0, last_wait = 0, worst_frame = 0;
    uint64_t last_cat[prof::NCAT] = {}, worst_cat[prof::NCAT] = {};
    uint64_t first_cat[prof::NCAT] = {}, sum_cat[prof::NCAT] = {};   // each part's time over the frames counted
    double sum_ms = 0, max_ms = 0;
    FILE *log = nullptr;   // HK_FRAMETIMES=file: every frame's host work, "frame ms cpu_ms major_faults"
    double last_cpu = 0;   // the emulation thread's CPU time and the process's major page faults
    long last_faults = 0;  // (a long frame with little CPU waited: on the disk, or for the CPU)

    void frame(uint64_t f) {
        const uint64_t t = prof::ticks(), w = prof::g_wait_ticks;
        double cpu = 0;
        long faults = 0;
        if (log) {
            cpu = hk::sys::thread_cpu_ms();
            faults = hk::sys::major_faults();
        }
        if (last) {
            double ms = (double)((t - last) - (w - last_wait)) * prof::ns_per_tick() / 1e6;
            if (log) fprintf(log, "%llu %.3f %.3f %ld\n", (unsigned long long)f, ms, cpu - last_cpu, faults - last_faults);
            hist[std::min(BINS, (int)(ms / 0.05))]++;
            n++;
            for (int c = 0; c < prof::NCAT; c++) sum_cat[c] = prof::g_total[c] - first_cat[c];
            sum_ms += ms;
            if (ms > max_ms) {
                max_ms = ms, worst_frame = f;
                for (int c = 0; c < prof::NCAT; c++) worst_cat[c] = prof::g_total[c] - last_cat[c];
            }
        }
        if (!last)
            for (int c = 0; c < prof::NCAT; c++) first_cat[c] = prof::g_total[c];
        last = t;
        last_wait = w;
        last_cpu = cpu;
        last_faults = faults;
        for (int c = 0; c < prof::NCAT; c++) last_cat[c] = prof::g_total[c];
    }
    double percentile(double q) const {
        uint64_t need = (uint64_t)(q * (double)n), acc = 0;
        for (int k = 0; k <= BINS; k++)
            if ((acc += hist[k]) > need) return (k + 1) * 0.05;
        return max_ms;
    }
    void print() const {
        if (!n) return;
        printf("frametime: host work per frame over %llu frames: mean %.2f ms, p50 %.2f, p99 %.2f, p99.9 %.2f, "
               "max %.2f (frame %llu:",
               (unsigned long long)n, sum_ms / (double)n, percentile(0.5), percentile(0.99), percentile(0.999), max_ms,
               (unsigned long long)worst_frame);
        for (int c = 0; c < prof::NCAT; c++) printf(" %s %.2f", prof::NAMES[c], (double)worst_cat[c] * prof::ns_per_tick() / 1e6);
        printf(")\n");
        // where a frame's time goes, on average: the parts measured (core/prof.h), the rest "other"
        double parts = 0;
        std::string by;
        for (int c = 0; c < prof::NCAT; c++) {
            const double ms = (double)sum_cat[c] * prof::ns_per_tick() / 1e6 / (double)n;
            parts += ms;
            if (ms >= 0.005) by += std::string(by.empty() ? " " : ", ") + prof::NAMES[c] + " " + std::to_string(ms).substr(0, 4);
        }
        printf("frametime: by part, mean ms per frame:%s, other %.2f\n", by.c_str(), std::max(0.0, sum_ms / (double)n - parts));
    }
};

// 64-bit FNV-1a over 32-bit words: cheap enough to run on every frame
uint64_t hash_words(uint64_t h, const void *p, size_t n_words) {
    const uint32_t *w = static_cast<const uint32_t *>(p);
    for (size_t k = 0; k < n_words; k++) h = (h ^ w[k]) * 0x100000001b3ull;
    return h;
}
constexpr uint64_t HASH_SEED = 0xcbf29ce484222325ull;

inline void mkdirs(const std::string &path) { hk::sys::make_dirs(path); }

// The HIRAKU root (dumps/, games/, out/): $HIRAKU_ROOT (the launcher passes its own), else a
// portable install holding the binary (shell/userdir.h; before the checkout, which a package
// built on this PC would otherwise take), else the checkout the binary was built from, else the
// folder holding the binary or its parent (a copied build).
std::string find_root() {
    if (const char *r = getenv("HIRAKU_ROOT"); r && *r) return r;
    auto has_dumps = [](const std::string &d) { return hk::sys::is_dir(d + "/dumps"); };
    std::string dir, up;
    if (const std::string exe = hk::sys::exe_path(); !exe.empty()) {
        dir = exe;
        dir.erase(dir.rfind('/'));
        up = dir.substr(0, dir.rfind('/'));
    }
    for (const std::string &d : {dir, up})
        if (!d.empty() && has_dumps(d) && hk::portable(d)) return d;
    if (has_dumps(HK_ROOT)) return HK_ROOT;
    for (const std::string &d : {dir, up})
        if (!d.empty() && has_dumps(d)) return d;
    return HK_ROOT;
}

// A game.toml's `boot_seconds = N` (its top, before any [section]): the fast start-up's ready
// point; 0 if none
double boot_seconds(const std::string &toml) {
    FILE *f = fopen(toml.c_str(), "r");
    if (!f) return 0;
    char line[512];
    double s = 0;
    while (fgets(line, sizeof line, f) && line[0] != '[')
        if (sscanf(line, " boot_seconds = %lf", &s) == 1) break;
    fclose(f);
    return s;
}

}  // namespace

int main(int argc, char **argv) {
    Core &machine = core();
    Core::Options opt;
    opt.root = find_root();
    opt.game = built_game ? built_game : "unknown";
    // a core's own binary (hiraku-djmain, hiraku-sys573, hiraku-twinkle: a release's, with no
    // game built in) runs any of the core's games: --game ID, or its name hiraku-ID (a link to it)
    for (int k = 1; k + 1 < argc; k++)
        if (std::string a = argv[k]; a == "--game" || a == "-game") opt.game = argv[k + 1];
    if (!built_game && opt.game == "unknown") {
        std::string self = argv[0];
        self = self.substr(self.find_last_of("/\\") + 1);
        if (self.size() > 4 && self.compare(self.size() - 4, 4, ".exe") == 0) self.resize(self.size() - 4);
        if (self.rfind("hiraku-", 0) == 0) opt.game = self.substr(7);
    }
    const std::string &game = opt.game;
    std::string out = opt.root + "/out/run/" + game;
    uint64_t frames = 0, snap_every = 0, snap_from = 0, snap_between = 0, debug_every = 0;
    int window_scale = 0;
    std::string wav_path, digest_path, script_path, bindings, nvram_dir, lights_file, lights_socket;
    bool fullscreen = false;
    std::string picture_mode = "off";
    std::vector<std::string> picture_config, shader_keys;
    std::vector<std::string> macro_specs;
    bool display_sync = true, smooth_motion = false, lights_overlay = false, crt_beam = false;
    float beam_gamma = 2.2f, beam_gain = 0.7f;
    std::string lights_log_path;
    bool nvram_given = false;
    int audio_ms = 8, audio_buffer = 0;
    bool audio_fixed = false, normal_priority = false;
    std::string audio_device;
    bool audio_exclusive = false;
    double volume_db = 0;
    bool split_screens = false;
    std::vector<std::string> mod_dirs;   // --mod: docs/MODS.md
    std::string audio_asio;
    bool fast_boot = false;
    double fast_boot_seconds = 0;
    for (int k = 1; k < argc; k++) {
        std::string a = argv[k];
        // one dash is two, as spice2x writes its options (-url, -p, -card0: shell/eaoptions.h)
        if (a.size() >= 2 && a[0] == '-' && a[1] != '-' && a != "-h") a = "-" + a;
        auto val = [&]() -> std::string {
            if (k + 1 >= argc) { fprintf(stderr, "%s needs a value\n", a.c_str()); exit(2); }
            return argv[++k];
        };
        if (a == "--game") val();   // read above
        else if (a == "--frames") frames = std::stoull(val());
        else if (a == "--snap-every") snap_every = std::stoull(val());
        else if (a == "--snap-from") snap_from = std::stoull(val());
        else if (a == "--snap-between") snap_between = std::stoull(val());
        else if (a == "--out") out = val();
        else if (a == "--trace") opt.trace = true;
        else if (a == "--ata-log") opt.ata_log = true;
        else if (a == "--debug-every") debug_every = std::stoull(val());
        else if (a == "--wav") wav_path = val();
        else if (a == "--digest") digest_path = val();
        else if (a == "--script") script_path = val();
        else if (a == "--bindings") bindings = val();
        else if (a == "--lights") lights_file = val();
        else if (a == "--lights-socket") lights_socket = val();
        else if (a == "--fullscreen") fullscreen = true;
        else if (a == "--crt") picture_mode = "light";
        else if (a == "--picture") picture_mode = val();
        else if (a == "--picture-config") picture_config.push_back(val());
        else if (a == "--shader-key") shader_keys.push_back(val());
        else if (a == "--macro") macro_specs.push_back(val());
        else if (a == "--display-sync") display_sync = val() != "off";
        else if (a == "--smooth-motion") smooth_motion = true;
        else if (a == "--crt-beam") crt_beam = true;
        else if (a == "--crt-beam-gamma") beam_gamma = std::stof(val());
        else if (a == "--crt-beam-gain") beam_gain = std::stof(val());
        else if (a == "--lights-overlay") lights_overlay = true;
        else if (a == "--lights-log") lights_log_path = val();
        else if (a == "--nvram") { nvram_dir = val(); nvram_given = true; }
        else if (a == "--audio-ms") audio_ms = std::stoi(val());
        else if (a == "--audio-fixed") audio_fixed = true;
        else if (a == "--normal-priority") normal_priority = true;
        else if (a == "--audio-device") audio_device = val();
        else if (a == "--audio-exclusive") audio_exclusive = true;
        else if (a == "--volume") volume_db = std::stod(val());
        else if (a == "--split-screens") split_screens = true;
        else if (a == "--fast-boot") fast_boot = true;
        else if (a == "--fast-boot-seconds") fast_boot = true, fast_boot_seconds = std::stod(val());
        else if (a == "--mod") mod_dirs.push_back(val());
        else if (a == "--audio-asio") audio_asio = val();
        else if (a == "--audio-buffer") audio_buffer = std::stoi(val());
        else if (a == "--window") window_scale = (k + 1 < argc && argv[k + 1][0] != '-') ? std::stoi(val()) : 2;
        else if (a == "--help" || a == "-h") {
            printf("hiraku-%s [options]: see runtime/main.cpp for the common options. This core also takes:\n%s%s",
                   game.c_str(), machine.options_help().c_str(), (host::readers_help() + host::bio2_help() + host::popnio_help()).c_str());
            return 0;
        }
        else if (host::bio2_option(a, val)) {}
        else if (host::popnio_option(a, val)) {}
        else if (std::string error; host::readers_option(a, val, &error)) {
            if (!error.empty()) { fprintf(stderr, "%s\n", error.c_str()); return 2; }
        }
        else if (!machine.option(a, val)) { fprintf(stderr, "unknown option %s (--help lists them)\n", a.c_str()); return 2; }
    }
#ifdef _WIN32
    setvbuf(stdout, nullptr, _IONBF, 0);   // Windows has no line buffering (_IOLBF is full), and MSVC's runtime rejects its size 0
#else
    setvbuf(stdout, nullptr, _IOLBF, 0);
#endif
    mkdirs(out);
    opt.out = out;

    if (!nvram_given && window_scale) nvram_dir = hk::user_data_dir(opt.root) + "/" + game;
    opt.nvram_dir = nvram_dir;
    opt.window = window_scale != 0;
    if (!nvram_dir.empty()) {
        mkdirs(nvram_dir);
        printf("nvram: %s\n", nvram_dir.c_str());
    }

    FILE *wav = nullptr;
    uint64_t wav_frames = 0;
    if (!wav_path.empty()) {
        wav = fopen(wav_path.c_str(), "wb");
        uint8_t hdr[44] = {};
        if (wav) fwrite(hdr, 1, 44, wav);   // header written at shutdown
    }
    FILE *digest = digest_path.empty() ? nullptr : fopen(digest_path.c_str(), "w");
    if (!digest_path.empty() && !digest) { fprintf(stderr, "cannot write %s\n", digest_path.c_str()); return 1; }
    uint64_t audio_hash = HASH_SEED;
    // the sink goes in before init(): machines make sound while the skipped BIOS time passes
    machine.set_audio_sink([&](const float *lr, int n) {
        if (digest) audio_hash = hash_words(audio_hash, lr, 2 * (size_t)n);
#ifdef HK_WITH_SDL
        if (window_scale) host::audio_push(lr, n);
#endif
        if (!wav) return;
        for (int k = 0; k < 2 * n; k++) {
            float f = lr[k] * 32768.0f;   // full scale 1.0 = 32768, as the chips and MAME's WAVs have it
            int16_t v = (int16_t)(f > 32767 ? 32767 : f < -32768 ? -32768 : f);
            fwrite(&v, 2, 1, wav);
        }
        wav_frames += (uint64_t)n;
    });
    // mods: their media ranges before the core opens its images, their patches every frame
    if (!mod_dirs.empty()) {
        std::string err;
        if (!mods::load(mod_dirs, game, opt.root, err)) {
            fprintf(stderr, "mods: %s\n", err.c_str());
            return 2;
        }
        for (const std::string &m : mods::describe()) fprintf(stderr, "mods: %s\n", m.c_str());
    }
#ifdef HK_WITH_SDL
    // before the machine's threads start: they inherit it
    if (window_scale && !normal_priority) fprintf(stderr, "%s\n", host::raise_priority().c_str());
#endif
    if (!machine.init(opt)) return 1;
#ifdef HK_WITH_SDL
    if (window_scale) host::preload_media();
#endif
#ifdef HK_WITH_SDL
    if (!lights_file.empty() || !lights_socket.empty()) host::lights_init(game, machine.family(), lights_file, lights_socket);
#else
    if (!lights_file.empty() || !lights_socket.empty()) fprintf(stderr, "this build has no lights (needs SDL3)\n");
#endif
    auto close_wav = [&]() {
        if (!wav) return;
        uint32_t data = (uint32_t)(wav_frames * 4), riff = data + 36, rate = (uint32_t)machine.audio_rate(), brate = rate * 4, fmt = 16;
        uint16_t pcm = 1, ch = 2, align = 4, bits = 16;
        fseek(wav, 0, SEEK_SET);
        fwrite("RIFF", 1, 4, wav); fwrite(&riff, 4, 1, wav); fwrite("WAVEfmt ", 1, 8, wav);
        fwrite(&fmt, 4, 1, wav); fwrite(&pcm, 2, 1, wav); fwrite(&ch, 2, 1, wav); fwrite(&rate, 4, 1, wav);
        fwrite(&brate, 4, 1, wav); fwrite(&align, 2, 1, wav); fwrite(&bits, 2, 1, wav);
        fwrite("data", 1, 4, wav); fwrite(&data, 4, 1, wav);
        fclose(wav);
        wav = nullptr;
    };

    static FrameTimes frame_times;
    if (const char *ft = getenv("HK_FRAMETIMES"); ft && *ft) frame_times.log = fopen(ft, "w");
    auto report = [&]() {
        frame_times.print();
        machine.report();
        close_wav();
        if (digest) { fclose(digest); digest = nullptr; }
#ifdef HK_WITH_SDL
        if (window_scale) printf("%s\n", host::audio_describe().c_str());
#endif
    };
    // --lights-log: each lamp's changes, as the frame hook sees them (lamps all off at first)
    std::function<void(uint64_t)> lights_log;
    if (!lights_log_path.empty()) {
        FILE *lf = lights_log_path == "-" ? stderr : fopen(lights_log_path.c_str(), "w");
        if (!lf) {
            fprintf(stderr, "cannot write %s\n", lights_log_path.c_str());
            return 1;
        }
        lights_log = [&machine, lf, seen = std::vector<uint8_t>(), changes = ~0ull](uint64_t f) mutable {
            if (machine.lamp_changes() == changes) return;
            changes = machine.lamp_changes();
            const std::vector<uint8_t> &now = machine.lamp_state();
            const std::vector<cab::Lamp> &defs = cab::lamps(machine.family());
            seen.resize(now.size(), 0);
            for (size_t i = 0; i < now.size() && i < defs.size(); i++) {
                if (now[i] == seen[i]) continue;
                const cab::Lamp &l = defs[i];
                fprintf(lf, "f%llu %.4f %s [%s%d] %s\n", (unsigned long long)f, machine.seconds(), l.name.c_str(),
                        l.reg ? (std::to_string(l.reg) + ".").c_str() : "", l.bit, now[i] ? "on" : "off");
            }
            seen = now;
            fflush(lf);
        };
    }
    auto lights_off = []() {
#ifdef HK_WITH_SDL
        host::lights_close();
#endif
    };
    // every way out: the machine saves its NVRAM, lamps go dark, statistics are printed
    auto finish = [&]() {
        script::close();
        machine.shutdown();
        lights_off();
        report();
    };
    machine.set_on_fatal(finish);

#ifdef HK_WITH_SDL
    host::window_set_picture(picture_mode, opt.root + "/shaders", picture_config);
    host::window_set_display_sync(display_sync);
    host::window_set_smooth_motion(smooth_motion);
    host::window_set_beam(crt_beam, beam_gamma, beam_gain);
    host::window_set_lights_overlay(lights_overlay);
    for (const std::string &k : shader_keys)
        if (!host::window_set_shader_key(k)) fprintf(stderr, "--shader-key %s: not ACTION=BINDING with a known action\n", k.c_str());
    host::window_set_audio_output(audio_device, audio_buffer, audio_exclusive);
    host::window_set_audio_asio(audio_asio);
    host::window_set_volume(volume_db);
    host::window_set_audio_fixed(audio_fixed);
    host::window_set_split(split_screens);
    if (fast_boot && window_scale) {   // the ready point: --fast-boot-seconds, else the game's
        double s = fast_boot_seconds;
        if (s <= 0) s = boot_seconds(opt.root + "/games/" + game + "/game.toml");
        if (s > 0) host::warp::set(s);
        else fprintf(stderr, "fast start-up: games/%s/game.toml has no boot_seconds: the start-up at its own speed\n", game.c_str());
    }
    if (window_scale && !host::window_open(("HIRAKU - " + game).c_str(), window_scale, bindings, audio_ms, fullscreen, machine.audio_rate())) {
        fprintf(stderr, "cannot open a window\n");
        return 1;
    }
#else
    if (window_scale) { fprintf(stderr, "this build has no SDL3 front end\n"); return 1; }
#endif
    if (window_scale) {   // a player's readers and cabinet; scripted runs keep to their inputs
#ifdef HK_WITH_SDL
        host::notice::warn(machine.setup_warning());   // e.g. Keyboardmania's blank sound flash
        host::notice::start(opt.root);   // a new release, said on the window (not from the launcher)
#endif
        host::readers_start();
        host::bio2_start();
        host::popnio_start();
    }
    if (!script_path.empty() && !script::load(script_path, out)) return 2;
    if (!macro::load(opt.root + "/games/" + game + "/macros.txt")) return 2;
    for (const auto &m : macro_specs)
        if (!macro::schedule(m)) return 2;
    int script_snaps = 0;
    // Ctrl-C / SIGTERM: leave at the next frame, saving NVRAM like a normal exit
    static std::atomic<bool> stop_requested{false};
    std::signal(SIGINT, [](int) { stop_requested = true; });
    std::signal(SIGTERM, [](int) { stop_requested = true; });
#ifdef HK_WITH_SDL
    // a core that can show the window its picture at the end of vertical blank does: the picture
    // the monitor then shows, the game's flip during the blank included, which the frame hook's
    // (vblank start) has only a frame later, after the frame's active lines have been emulated.
    // The frame hook below then only paces and reads the controls.
    const bool present_at_vblank_end =
        window_scale && machine.set_present_hook([&](const uint32_t *rgb, int w, int h) {
            host::window_present(rgb, w, h, machine.seconds());
        });
#endif
    machine.set_frame_hook([&](uint64_t f, const uint32_t *rgb, int w, int h) {
        frame_times.frame(f);
        if (digest)
            fprintf(digest, "%llu %016llx %016llx\n", (unsigned long long)f,
                    (unsigned long long)hash_words(HASH_SEED, rgb, (size_t)w * h), (unsigned long long)audio_hash);
        if (stop_requested) {
            finish();
#ifdef HK_WITH_SDL
            if (window_scale) host::window_close();
#endif
            exit(130);
        }
#ifdef HK_WITH_SDL
        if (window_scale) mods::display_rate(host::pacing::refresh_hz());
#endif
        mods::frame();
        macro::frame(f);
        if (!script_path.empty()) {
            bool keep = script::frame(f, [&]() {
                char name[64];
                snprintf(name, sizeof name, "/snap%04d-f%06llu.ppm", script_snaps++, (unsigned long long)f);
                write_ppm(out + name, rgb, w, h);
            });
            if (!keep) {
                finish();
#ifdef HK_WITH_SDL
                if (window_scale) host::window_close();
#endif
                exit(machine.exit_code());
            }
        }
#ifdef HK_WITH_SDL
        if (window_scale && !(present_at_vblank_end ? host::window_next(machine.seconds())
                                                    : host::window_frame(rgb, w, h, machine.seconds()))) {
            machine.shutdown();
            lights_off();
            report();
            host::window_close();
            exit(0);
        }
#endif
        if (debug_every && f % debug_every == 0) machine.debug_line(f);
        if (prof::g_sample_frame) prof::g_sample_frame(f);   // HK_PROFILE_FRAMES
        if (lights_log) lights_log(f);
#ifdef HK_WITH_SDL
        host::lights_frame();
#endif
        host::readers_frame([&](int p, const std::string &n) { return machine.card_read(p, n); },
                            [&](const std::string &name, int v) { return machine.set_input(name, v); });
        host::bio2_frame(machine);
        host::popnio_frame(machine);
        if (snap_every && f >= snap_from && f % snap_every == 0) {
            char name[64];
            snprintf(name, sizeof name, "/frame%06llu.ppm", (unsigned long long)f);
            write_ppm(out + name, rgb, w, h);
            // and with --smooth-motion, the in-between pictures: halfway to the next frame, or with
            // --snap-between N at 1/N, 2/N... (DJ Main: Firebeat's go with the window's picture, at
            // the end of vblank after the game's flip, not this one)
            for (uint64_t i = 1; smooth_motion && i < std::max<uint64_t>(snap_between, 2); i++) {
                const double t = snap_between ? (double)i / (double)snap_between : 0.5;
                int bw = w, bh = h;
                const uint32_t *b = machine.render_between(t, bw, bh);
                if (!b) b = rgb, bw = w, bh = h;   // nothing moved a whole pixel yet: the frame
                if (snap_between) snprintf(name, sizeof name, "/frame%06llu-%03d.ppm", (unsigned long long)f, (int)(t * 1000 + 0.5));
                else snprintf(name, sizeof name, "/frame%06llu-half.ppm", (unsigned long long)f);
                write_ppm(out + name, b, bw, bh);
            }
        }
        if (frames && f >= frames) {
            machine.shutdown();
            lights_off();
            report();
#ifdef HK_WITH_SDL
            if (window_scale) host::window_close();   // the sound card back to its owner
#endif
            exit(machine.exit_code());
        }
    });

    machine.run();
}
