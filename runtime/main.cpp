// SPDX-License-Identifier: GPL-3.0-or-later
// hiraku-<game>: the shell around a machine core (shell/core.h). It parses the
// command line, records pictures, sound and digests, runs input scripts, drives
// the window, controls and lamps, and decides when the run ends. The core
// (Firebeat, DJ Main, ...) is whichever this binary was linked with.
//
//   hiraku-<game> [options]
//   hiraku-<core> --game ID [options]   (a release's djmain, sys573, twinkle: one binary per core)
//     (the options every core takes: COMMON_OPTIONS below, which --help prints)
//   plus the core's own options (--help lists them).
//
// The machine is described by dumps/<game>/machine.txt and dumps/<game>/rom/,
// both written by tools/ingest.py from the user's own dumps.

#include <chrono>
#include <algorithm>
#include <atomic>
#include <csignal>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef _WIN32
extern char **environ;   // (the run's test switches, for its log: shell/switches.h)
#endif

#include "core/prof.h"
#include "host/macro.h"
#include "host/menukey.h"
#include "host/menupic.h"
#include "host/mods.h"
#include "host/modscript.h"
#include "host/record.h"
#include "host/screenshot.h"
#include "host/script.h"
#include "lang/lang.h"
#include "shell/cabinet.h"
#include "shell/built.h"
#include "shell/core.h"
#include "shell/userdir.h"
#include "shell/saves.h"
#include "shell/level.h"
#include "shell/smoothgate.h"
#include "shell/sys.h"
#include "shell/switches.h"
#include "host/bio2.h"
#include "host/popnio.h"
#include "host/readers.h"
#ifdef HK_WITH_SDL
#include "host/lights.h"
#include "host/notice.h"
#include "host/warp.h"
#include "host/window.h"
#include "host/pacing.h"
#include "host/statsview.h"
#endif

// The options every core takes (--help prints them: a release has no source to read them in)
static const char COMMON_OPTIONS[] = R"HK(  --frames N        stop after N video frames (default: run until killed)
  --snap-every N    write every Nth frame as PPM into --out (default off)
  --snap-from F     ... from frame F on
  --snap-between N  with --smooth-motion, each snapshot's in-between pictures at 1/N, 2/N...
                    of a frame (default: halfway only)
  --out DIR         output directory for snapshots/trace (default out/run/<game>)
  --trace           write an oracle-format MMIO trace (out/mmio.bin), see oracle/
  --ata-log         log ATA/ATAPI commands
  --script FILE     run an oracle input script (.his): same inputs, same frames as in MAME
  --wav FILE        record the sound output (16-bit stereo, at the core's rate: 44.1 kHz, DJ Main 48 kHz)
  --record [FILE]   record the game to a video file from the start: its own frames at their own size
                    and rate, with its sound, timed by the machine's clock (a slow moment on the PC
                    doesn't slow the video). FFV1 and FLAC in Matroska (.mkv, lossless); FILE a
                    folder or none: a file named by the game and the time in it, or in
                    --record-folder's. With --window, F6 starts and stops it. host/record.h
  --record-folder DIR  where F6's recordings go (default: <saves' folder>/recordings, as --nvram's)
  --screenshot-folder DIR  where F5's screenshots go (PNG, the game's own picture; default:
                    <saves' folder>/screenshots). host/screenshot.h
  --record-format F ffv1 (the default) or h264 (.mp4, H.264 and AAC: for sharing, about a sixth of
                    the size; where this build's FFmpeg has an H.264 encoder)
  --record-encoder E  h264's encoder: auto (the default: the graphics card's when one works here,
                    else x264; x264 when no real time is kept), gpu (the card's, else x264), x264,
                    or an FFmpeg encoder's name (h264_vaapi, h264_mf...: that one only)
  --record-scale N  h264: the picture enlarged N times, whole pixels (sharp after H.264's colour
                    halving); auto (the default): the most within 1920x1440 with a graphics card's
                    encoder, 1 (the game's own size) with the processor's. ffv1 is always 1
  --convert-recording IN [OUT]  a lossless recording (.mkv, and its -2, -3... parts) to H.264 MP4
                    (OUT, default IN's name .mp4), with --record-encoder and --record-scale, then exit:
                    0 done, 3 done as far as a recording cut short or damaged could be read, 1 failed
  --digest FILE     write one line per frame: frame number, a hash of the picture and a running
                    hash of all sound so far (tools/regress.py checks runs are bit-identical with it)
  --debug-every N   print the CPU state every N frames
  --window [SCALE]  show the game in a window, in real time, with sound (SDL3 builds);
                    F2 test, 9 service, 5 coin, 1 start, Esc quits; controls per cabinet
  --fullscreen      start the window fullscreen (F11 toggles)
  --display-sync on|off  on (the default): on a fixed-refresh display, show every frame for a
                    whole number of refreshes, running the game at the display's rate (up to
                    3 % off: pop'n 2.6 % fast at 60 Hz); off: the game's exact speed, frames
                    shown as they come (for variable-refresh displays). runtime/host/pacing.h
  --smooth-motion   with display sync, on any display (60, 144, 240 Hz): the game at its own
                    speed, and every refresh shows what moves carried on to that refresh's
                    moment (the core's in-between pictures: DJ Main's sprites, the Firebeat
                    GCU's objects, the PlayStation GPU's primitives on Twinkle and System
                    573) instead of frames held for
                    uneven numbers of refreshes.
                    Needs the compositor's fifo-v1 (KWin 6.7), or Windows (DWM's vblank).
                    --smooth-motion songs (the default): only while a song is played, for a
                    game whose game.toml says when ([smooth]; the others everywhere);
                    --smooth-motion everywhere: on every screen. docs/SMOOTH.md
  --crt-beam        the Blur Busters CRT beam simulator over the picture, on a display faster
                    than the game (120 Hz and up; best at 240): each refresh lights the part of
                    the tube the beam sweeps then, for a CRT's motion clarity. Needs what
                    --smooth-motion does, and replaces its in-between pictures. docs/SMOOTH.md
  --crt-beam-gamma G  the display's gamma for it (2.2; a mismatch shows as bands, washed out)
  --crt-beam-gain X   its brightness against motion blur (0.7; 1 as bright as without it)
  --lights-overlay  the game's lights over its picture: every lamp the core decodes, lit or not
                    as the game drives it, and how often they change (F9 shows or hides it)
  --stats-overlay [LEVEL]  how the game runs on this PC, over its picture: line (the pictures a
                    second, a frame's work against its time, the processor, the sound queued,
                    late frames), panel (the default: with the display's rate, a graph of the
                    last three seconds' frames, the sound and the display sync) or full (with
                    the latencies, the busiest threads, the graphics card and the core's own
                    numbers). F7 goes through them; the mouse drags the panel, Shift+F7 puts it
                    in the next corner. A frame that didn't fit its time is a "slow frame" line
                    in the log either way. runtime/host/statsview.h
  --macro NAME[@F]  run one of the game's code macros (games/<game>/macros.txt) at frame F, or
                    at its own `at` frame (power-on codes); repeatable. In the window,
                    Ctrl+1..9 types the 1st..9th macro
  --picture MODE    off (sharp pixels, the default), light (CRT: crt-guest-advanced-fast),
                    full (CRT: crt-royale) or lola (CRT, the analog chain: Lola Retro); F10
                    cycles them. --crt is --picture light
  --picture-config DIR  read DIR/<shader>.params over the shader's settings (repeatable,
                    later ones win; `name = value` lines; read again when they change)
  --shader-key ACTION=BINDING  a control of the shader's settings in the game (F8: show, up,
                    down, less, more, saved, default, save, save_game), as --bindings spells it
                    ("key:F8", "joy[DAO FPS]:button3"); repeatable; host/shaderview.h
  --bindings FILE   controls (see runtime/host/input.h)
  --lights FILE     drive HID controller LEDs from the cabinet lamps (see runtime/host/lights.h)
  --lights-socket P publish the lamps on a UNIX socket at path P, or on TCP at HOST:PORT (docs/LIGHTS.md)
  --lights-log FILE every lamp change, a line each: the frame, the machine's time, the lamp (its
                    output number, as the F9 overlay shows it) and on or off; FILE "-" is the
                    console (stderr). Headless too
  --reader PATH, --scard, --cardio  real e-AMUSEMENT card readers, with --window (see
                    runtime/host/readers.h)
  --bio2 PATH       beatmania IIDX's BIO2 I/O board as the cabinet's controls and lamps, with
                    --window (runtime/host/bio2.h)
  --popn-io WHERE, --popn-io-dll FILE  a pop'n music cabinet's USB I/O board as its buttons
                    and lamps, with --window (runtime/host/popnio.h)
  --audio-ms N      the audio queue target (default 8), and its least: it tunes itself to the
                    lead that covers all but the two worst seconds of the last minute (not in
                    the first 10 s), up to 30 ms, and comes back down when late frames stop
  --audio-fixed     the audio queue stays at --audio-ms (no self-tuning)
  --normal-priority with --window, leave the game's threads at normal priority (by default they
                    go ahead of other programs: Linux nice -11 and timer slack 1 us, as far as
                    the limits or RealtimeKit allow; Windows above normal)
  --audio-device D  the sound card itself, through ALSA, with no sound server in between: an ALSA
                    device name such as hw:CARD=Generic,DEV=0 (aplay -l lists them; the launcher's
                    Settings > Audio too). If it's busy, the system's default output instead
                    Windows: an output device by the name Windows shows (shared, through the system
                    mixer), or with --audio-exclusive alone
  --split-screens   a machine with several monitors (Keyboardmania: one a player) shows each in
                    a window of its own: player 2's on the next display, both fullscreen with
                    --fullscreen (F11 toggles the window with the keyboard)
  --mod FOLDER      a mod (docs/MODS.md: the folder's mod.toml), several in order: patches
                    to the game's memory, replaced bytes or files on its media
  --mod-setting NAME=VALUE  a mod's [[setting]] for this run (not kept); MOD/NAME where two have it
  --mod-api         the mod API this program has (its version, capabilities, registers), and exit
  --fast-boot       with --window, the game's start-up as fast as the machine can go (no sound,
                    its pictures with a box over them) until its ready point (games/<id>/game.toml
                    boot_seconds: the machine's seconds to its title or attract), or until a
                    player presses one of its buttons. host/warp.h
  --fast-boot-seconds S  ... up to S seconds of the machine's time instead (implies --fast-boot)
  --game-menu on|off  with --window (on by default): 5, 7 and 3 of an e-AMUSEMENT reader's 10-key
                    held together (either player's), or the Test and Service switches held
                    together, leave the game as a closed window does, with exit code 73: the
                    launcher then shows its game menu, driven by the cabinet's own controls
                    (host/menukey.h). A box on the picture says so while they are held
  --game-menu-hold S  ... held for S seconds (2 by default)
  --game-menu-key BINDING  a key or button of your own that does the same, held (as a bindings
                    file spells it: key:F12, joy0:button11)
  --menu-picture FILE  the game's picture for that menu: one frame written to FILE (a BMP) once
                    the machine has run for the game's title_seconds (its game.toml), unless a
                    control was pressed before then (host/menupic.h)
  --menu-picture-at S  ... after S seconds of the machine's time instead
  --menu-picture-own FILE  with --window, F12 writes the screen to FILE as the game's picture (the
                    player's own: the launcher's menu-pictures/<id>.bmp), whatever it shows
  --volume DB       the sound out of the window, louder or quieter (e.g. 6 or -3; 0 by default):
                    what the speakers get, not what --wav and --digest record
  --even-volume on|off  on (the default): every game about as loud as the others, by a trim
                    for its family on top of --volume (System 573 -5 dB, Twinkle -4, PC IIDX -4,
                    PC pop'n -1, Firebeat 0, Python +1, DJ Main +3; a game.toml's [sound]
                    level_db its own); off: each machine's own level. The speakers' sound
                    only, as --volume (runtime/shell/level.h)
  --audio-exclusive Windows: the output (--audio-device's, else the default one) in WASAPI exclusive
                    mode: no system mixer in between, other programs silent meanwhile
  --audio-asio NAME Windows: the sound card through its ASIO driver (the name its control panel and
                    the launcher show; the error lists the installed ones). Else Windows' audio
  --audio-buffer N  the device buffer in frames (default 128): the sound server's quantum, or with
                    --audio-device the ALSA period (a ring of four), or the WASAPI exclusive buffer,
                    or the ASIO buffer (default there: the driver's own). Smaller is sooner, until
                    it crackles
  --nvram DIR       keep the machine's NVRAM in DIR: loaded at start, saved on exit.
                    Default with --window: ~/.local/share/hiraku/<game> (a portable install's
                    user/data/<game>: shell/userdir.h); otherwise none (every run starts from
                    the ROM set's defaults, so scripted runs are reproducible)
  --save-every S    ... and saved while the game is played, every S seconds, when it changed
                    (shell/saves.h): a crash or a power cut costs seconds, not the session.
                    Default with --window: 15; otherwise 0, at exit only. The game's results
                    are the same either way
)HK";

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
    FILE *log = nullptr;   // HK_FRAMETIMES=file: every frame's host work, "frame ms cpu_ms major_faults", then
                           // the frame's milliseconds in each part (core/prof.h's, in its order)
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
            if (log) {
                fprintf(log, "%llu %.3f %.3f %ld", (unsigned long long)f, ms, cpu - last_cpu, faults - last_faults);
                for (int c = 0; c < prof::NCAT; c++)
                    fprintf(log, " %.3f", (double)(prof::g_total[c] - last_cat[c]) * prof::ns_per_tick() / 1e6);
                fputc('\n', log);
            }
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
// built on this PC would otherwise take), else the checkout the binary was built from (HK_ROOT;
// "" in a release's binaries, which know no checkout), else the folder holding the binary or its
// parent (a copied build, a package).
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
    const std::string built_in = HK_ROOT;
    if (!built_in.empty() && has_dumps(built_in)) return built_in;
    for (const std::string &d : {dir, up})
        if (!d.empty() && has_dumps(d)) return d;
    if (!built_in.empty()) return built_in;
    return up.empty() ? "." : up;   // (a package whose dumps/ is gone: still the folder above bin/)
}

// A game.toml's `KEY = N` (its top, before any [section]), a time of the machine's in seconds; 0 if
// none. boot_seconds: the fast start-up's ready point. title_seconds: when its title screen shows
// (the game menu's picture, host/menupic.h)
double manifest_seconds(const std::string &toml, const char *key) {
    FILE *f = fopen(toml.c_str(), "r");
    if (!f) return 0;
    const std::string format = std::string(" ") + key + " = %lf";
    char line[512];
    double s = 0;
    while (fgets(line, sizeof line, f) && line[0] != '[')
        if (sscanf(line, format.c_str(), &s) == 1) break;
    fclose(f);
    return s;
}

}  // namespace

int main(int argc, char **argv) {
    // a crash's report in the run log (the exception or signal, the calls on the stack)
    static std::string program = std::string(argv[0]).substr(std::string(argv[0]).find_last_of("/\\") + 1);
    hk::sys::report_crashes(program.c_str());
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
    bool mod_api = false;   // --mod-api
    std::string picture_mode = "off";
    std::vector<std::string> picture_config, shader_keys;
    std::vector<std::string> macro_specs;
    bool display_sync = true, smooth_motion = false, lights_overlay = false, crt_beam = false;
    std::string smooth_where = "songs";   // --smooth-motion songs|everywhere (shell/smoothgate.h)
    float beam_gamma = 2.2f, beam_gain = 0.7f;
    std::string lights_log_path;
    bool nvram_given = false;
    double save_every = -1;   // (not given: 15 s in a window)
    int audio_ms = 8, audio_buffer = 0;
    bool audio_fixed = false, normal_priority = false;
    std::string audio_device;
    bool audio_exclusive = false;
    double volume_db = 0;
    bool even_volume = true;   // --even-volume (shell/level.h)
    bool split_screens = false;
    std::vector<std::string> mod_dirs;   // --mod: docs/MODS.md
    std::string audio_asio;
    bool fast_boot = false;
    double fast_boot_seconds = 0;
    std::string stats_overlay = "off";
    bool game_menu = true;
    double game_menu_hold = 2.0;
    std::string game_menu_key, menu_picture, menu_picture_own;
    double menu_picture_at = 0;
    bool record_at_start = false;   // --record: host/record.h
    std::string record_path, record_folder, record_format = "ffv1";
    std::string screenshot_folder;   // --screenshot-folder: F5's (host/screenshot.h)
    std::string record_encoder = "auto", convert_in, convert_out;   // --record-encoder, --convert-recording
    int record_scale = 0;   // --record-scale (0: auto)
    for (int k = 1; k < argc; k++) {
        std::string a = argv[k];
        // one dash is two, as spice2x writes its options (-url, -p, -card0: shell/eaoptions.h)
        if (a.size() >= 2 && a[0] == '-' && a[1] != '-' && a != "-h") a = "-" + a;
        auto val = [&]() -> std::string {
            if (k + 1 >= argc) { fprintf(stderr, "%s needs a value\n", a.c_str()); exit(2); }
            return argv[++k];
        };
        try {   // (a value that isn't the number the option takes: said, not a crash's report)
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
        else if (a == "--record") record_at_start = true, record_path = (k + 1 < argc && argv[k + 1][0] != '-') ? val() : "";
        else if (a == "--record-folder") record_folder = val();
        else if (a == "--screenshot-folder") screenshot_folder = val();
        else if (a == "--record-format") record_format = val();
        else if (a == "--record-encoder") record_encoder = val();
        else if (a == "--record-scale") { const std::string v = val(); record_scale = v == "auto" ? 0 : std::max(1, std::stoi(v)); }
        else if (a == "--convert-recording") convert_in = val(), convert_out = (k + 1 < argc && argv[k + 1][0] != '-') ? val() : "";
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
        else if (a == "--smooth-motion") {
            smooth_motion = true;
            smooth_where = (k + 1 < argc && argv[k + 1][0] != '-') ? val() : "songs";
            if (smooth_where != "songs" && smooth_where != "everywhere") {
                fprintf(stderr, "--smooth-motion %s: songs (the default) or everywhere\n", smooth_where.c_str());
                return 2;
            }
        }
        else if (a == "--crt-beam") crt_beam = true;
        else if (a == "--crt-beam-gamma") beam_gamma = std::stof(val());
        else if (a == "--crt-beam-gain") beam_gain = std::stof(val());
        else if (a == "--lights-overlay") lights_overlay = true;
        else if (a == "--stats-overlay") stats_overlay = (k + 1 < argc && argv[k + 1][0] != '-') ? val() : "panel";
        else if (a == "--lights-log") lights_log_path = val();
        else if (a == "--nvram") { nvram_dir = val(); nvram_given = true; }
        else if (a == "--save-every") save_every = std::stod(val());
        else if (a == "--audio-ms") audio_ms = std::stoi(val());
        else if (a == "--audio-fixed") audio_fixed = true;
        else if (a == "--normal-priority") normal_priority = true;
        else if (a == "--audio-device") audio_device = val();
        else if (a == "--audio-exclusive") audio_exclusive = true;
        else if (a == "--volume") volume_db = std::stod(val());
        else if (a == "--even-volume") {
            const std::string v = val();
            if (v != "on" && v != "off") { fprintf(stderr, "--even-volume %s: on (the default) or off\n", v.c_str()); return 2; }
            even_volume = v == "on";
        }
        else if (a == "--split-screens") split_screens = true;
        else if (a == "--fast-boot") fast_boot = true;
        else if (a == "--fast-boot-seconds") fast_boot = true, fast_boot_seconds = std::stod(val());
        else if (a == "--mod") mod_dirs.push_back(val());
        else if (a == "--mod-setting") mods::override_setting(val());
        else if (a == "--mod-api") mod_api = true;
        else if (a == "--game-menu") game_menu = val() != "off";
        else if (a == "--game-menu-hold") game_menu_hold = std::stod(val());
        else if (a == "--game-menu-key") game_menu_key = val();
        else if (a == "--menu-picture") menu_picture = val();
        else if (a == "--menu-picture-at") menu_picture_at = std::stod(val());
        else if (a == "--menu-picture-own") menu_picture_own = val();
        else if (a == "--audio-asio") audio_asio = val();
        else if (a == "--audio-buffer") audio_buffer = std::stoi(val());
        else if (a == "--window") window_scale = (k + 1 < argc && argv[k + 1][0] != '-') ? std::stoi(val()) : 2;
        else if (a == "--help" || a == "-h") {
            printf("hiraku-%s [options]\n\nEvery core takes:\n%s\nThis core also takes:\n%s%s",
                   game.c_str(), COMMON_OPTIONS, machine.options_help().c_str(), (host::readers_help() + host::bio2_help() + host::popnio_help()).c_str());
            return 0;
        }
        else if (host::bio2_option(a, val)) {}
        else if (host::popnio_option(a, val)) {}
        else if (std::string error; host::readers_option(a, val, &error)) {
            if (!error.empty()) { fprintf(stderr, "%s\n", error.c_str()); return 2; }
        }
        else if (!machine.option(a, val)) { fprintf(stderr, "unknown option %s (--help lists them)\n", a.c_str()); return 2; }
        } catch (const std::logic_error &) {   // std::stoi and its kin: not a number, or too large a one
            fprintf(stderr, "%s: its value isn't a number it takes\n", a.c_str());
            return 2;
        }
    }
#ifdef _WIN32
    setvbuf(stdout, nullptr, _IONBF, 0);   // Windows has no line buffering (_IOLBF is full), and MSVC's runtime rejects its size 0
#else
    setvbuf(stdout, nullptr, _IOLBF, 0);
#endif
    if (!convert_in.empty()) {   // --convert-recording (host/record.h): no game runs; "convert: N %" lines for the launcher
        host::record::Capture::Setup cs;
        cs.encoder = record_encoder, cs.scale = record_scale;
        int said = -1;
        std::string line, error;
        printf("convert: %s to %s\n", convert_in.c_str(), host::record::converted_name(convert_in, convert_out).c_str());
        bool early = false;
        const bool ok = host::record::convert(convert_in, convert_out, cs, [&](double f) {
            if (const int pc = (int)(f * 100); pc != said) printf("convert: %d %%\n", said = pc);
        }, line, error, &early);
        if (!ok) fprintf(stderr, "convert: %s\n", error.c_str());
        else printf("%s\n", line.c_str());
        return !ok ? 1 : early ? 3 : 0;   // (3: converted up to where the recording ends early)
    }
    // the mod API (host/mods.h): what the shell offers besides the core's (actions: a window's
    // keys and controllers), and --mod-api's answer
#ifdef HK_WITH_SDL
    mods::offer("actions");
    mods::offer("draw");   // (scripts' drawing: over the window's picture, host/modscript.h)
#endif
    if (mod_api) {
        printf("%s", mods::api_text(opt.root, game).c_str());
        return 0;
    }
    mkdirs(out);
    opt.out = out;

    if (!nvram_given && window_scale) nvram_dir = hk::user_data_dir(opt.root) + "/" + game;
    opt.nvram_dir = nvram_dir;
    opt.window = window_scale != 0;
    if (window_scale) printf("build: %s %s\n", program.c_str(), hk::sys::build_id());   // which ran (a test zip swaps one program)
    // and with which test switches (two runs with one command line can differ by them)
#ifdef _WIN32
    if (window_scale) printf("%s\n", hk::switches_line(_environ).c_str());
#else
    if (window_scale) printf("%s\n", hk::switches_line(environ).c_str());
#endif
    if (!nvram_dir.empty()) {
        mkdirs(nvram_dir);
        printf("nvram: %s\n", nvram_dir.c_str());
        hk::saves::recover(nvram_dir);   // (a save cut short by a kill or a power cut: finished or dropped, before it is read)
    }
    if (save_every < 0) save_every = window_scale ? 15 : 0;
    if (nvram_dir.empty()) save_every = 0;

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
    static host::record::Capture capture;   // --record, F6 (host/record.h): nothing while it doesn't record
    static host::screenshot::Saver screenshots;   // F5 (host/screenshot.h): a thread of its own once one is taken
    // the sink goes in before init(): machines make sound while the skipped BIOS time passes
    machine.set_audio_sink([&](const float *lr, int n) {
        capture.sound(lr, n);
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
            mods::hand_over(argc, argv, game);   // (a hook mod and a recompiled game: hiraku-firebeat, if it's here)
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
    // HK_BETWEEN_BENCH=K: what --smooth-motion's in-between pictures cost the core, without a display:
    // every frame, the K - 1 a display at K times the game's rate asks for (2: 120 Hz, 4: 240 Hz).
    // Their time counts in the frame's (the frametime lines), and has a line of its own at exit
    static int between_bench = 0;
    static uint64_t between_n = 0, between_ticks = 0, between_max = 0, between_hash = 0xcbf29ce484222325ull;
    if (const char *bb = getenv("HK_BETWEEN_BENCH"); bb && atoi(bb) >= 2) between_bench = atoi(bb);
    std::string level_line;   // --even-volume's (shell/level.h): in the log at the start and in the report
    auto report = [&]() {
        frame_times.print();
        if (between_n)
            printf("in-between pictures (HK_BETWEEN_BENCH=%d): %llu drawn, mean %.2f ms each, max %.2f; their pixels' hash %016llx\n",
                   between_bench, (unsigned long long)between_n,
                   (double)between_ticks * prof::ns_per_tick() / 1e6 / (double)between_n,
                   (double)between_max * prof::ns_per_tick() / 1e6, (unsigned long long)between_hash);
        if (const hk::smoothgate::State &g = hk::smoothgate::state(); smooth_motion && g.has)
            printf("smooth motion in songs: in-between pictures on %llu of %llu frames (the song's word changing)\n",
                   (unsigned long long)g.open_frames, (unsigned long long)g.frames);
        machine.report();
        mods::report();
        close_wav();
        // (the file finished, and any F6 ended still being finished: every way out comes here)
        if (const std::string line = capture.stop(); !line.empty()) printf("%s\n", line.c_str());
        screenshots.wait();   // (and any F5 still being written)
        if (digest) { fclose(digest); digest = nullptr; }
#ifdef HK_WITH_SDL
        if (window_scale) printf("%s\n", host::audio_describe().c_str());
        if (!level_line.empty()) printf("%s\n", level_line.c_str());
#endif
        printf("%s", host::readers_report().c_str());
        if (const std::string line = host::menukey::report(); !line.empty()) printf("%s\n", line.c_str());
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
        host::menukey::hand_screen_to_launcher();   // (a core's own end, an error: its window goes next)
    };
    machine.set_on_fatal(finish);

    // --smooth-motion songs: the game's [smooth] (its game.toml) says when a song is played; the
    // in-between pictures only then (shell/smoothgate.h)
    auto game_toml = [&]() {   // games/<game>/game.toml's text ("" if none: the game menu)
        std::string text;
        if (FILE *f = fopen((opt.root + "/games/" + game + "/game.toml").c_str(), "rb")) {
            char b[4096];
            for (size_t n; (n = fread(b, 1, sizeof b, f)) > 0;) text.append(b, n);
            fclose(f);
        }
        return text;
    };
    if (smooth_motion) {
        std::string text = game_toml(), err;
        hk::smoothgate::State &g = hk::smoothgate::state();
        bool has = false;
        if (!hk::smoothgate::parse(text, g.cond, has, err)) {
            fprintf(stderr, "games/%s/%s\n", game.c_str(), err.c_str());
            return 2;
        }
        g.has = has && smooth_where == "songs";
        g.big_endian = machine.mod_big_endian();   // (while_equal's value in the machine's own order)
        if (g.has)
            printf("smooth motion: in songs only (while the word at 0x%08x changes, games/%s/game.toml [smooth]); the "
                   "other screens each frame held for its refreshes\n", g.cond.address, game.c_str());
        else
            printf("smooth motion: everywhere (%s)\n", smooth_where == "songs" ? "this game has no song condition yet" : "asked for");
    }

#ifdef HK_WITH_SDL
    host::window_set_picture(picture_mode, opt.root + "/shaders", picture_config);
    host::window_set_display_sync(display_sync);
    host::window_set_smooth_motion(smooth_motion);
    host::window_set_beam(crt_beam, beam_gamma, beam_gain);
    host::window_set_lights_overlay(lights_overlay);
    // (its place on the picture kept with the user's data, for every game)
    if (!host::window_set_stats_overlay(stats_overlay, window_scale ? hk::user_data_dir(opt.root) + "/stats-overlay.txt" : "")) {
        fprintf(stderr, "--stats-overlay %s: not line, panel, full or off\n", stats_overlay.c_str());
        return 2;
    }
    for (const std::string &k : shader_keys)
        if (!host::window_set_shader_key(k)) fprintf(stderr, "--shader-key %s: not ACTION=BINDING with a known action\n", k.c_str());
    host::window_set_audio_output(audio_device, audio_buffer, audio_exclusive);
    host::window_set_audio_asio(audio_asio);
    host::window_set_volume(volume_db);
    if (window_scale) {   // --even-volume: the game's family's trim on top (shell/level.h)
        std::string err;
        const hk::level::Trim t = hk::level::for_game(game_toml(), even_volume, err);
        if (!err.empty()) fprintf(stderr, "games/%s/%s; its family's level instead\n", game.c_str(), err.c_str());
        host::window_set_level(t.db);
        char b[512];
        if (!even_volume)
            snprintf(b, sizeof b, "audio: the machine's own level (--even-volume off: no trim for its family)");
        else if (t.own)
            snprintf(b, sizeof b, "audio: level evened out: this game's own %+.1f dB (games/%s/game.toml [sound] level_db); "
                     "--even-volume off for the machine's own level (the launcher: Settings > Audio)", t.db, game.c_str());
        else if (t.known) {
            char trim[64];
            if (t.db == 0) snprintf(trim, sizeof trim, "as the machine makes them (0 dB)");
            else snprintf(trim, sizeof trim, "%+.0f dB", t.db);
            snprintf(b, sizeof b, "audio: level evened out: %s's games %s (runtime/shell/level.h); --even-volume off "
                     "for the machine's own level (the launcher: Settings > Audio)", t.what.c_str(), trim);
        } else
            snprintf(b, sizeof b, "audio: the machine's own level (no family's trim for this game)");
        level_line = b;
        printf("%s\n", b);
    }
    host::window_set_audio_fixed(audio_fixed);
    host::window_set_split(split_screens);
    if (fast_boot && window_scale) {   // the ready point: --fast-boot-seconds, else the game's
        double s = fast_boot_seconds;
        if (s <= 0) s = manifest_seconds(opt.root + "/games/" + game + "/game.toml", "boot_seconds");
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
        // the boxes' and the overlay's language: the launcher's (HK_LANG), else its saved setting
        // for a game started on its own, else the system's (runtime/lang/lang.h). Logs stay English
        lang::start(opt.root);
#ifdef HK_WITH_SDL
        host::notice::warn(machine.setup_warning());   // e.g. Keyboardmania's blank sound flash
        if (modscript::any()) {   // mods' scripts: their text's font, their hk.notice on the window
            modscript::set_font(host::statsview::font());
            modscript::set_notice([](const std::vector<std::string> &lines) { host::notice::hint(lines, 3); });
        }
        host::notice::start(opt.root);   // a new release, said on the window (not from the launcher)
#endif
        host::readers_start();
        host::bio2_start();
        host::popnio_start();
        // the way out to the game menu, from the cabinet's controls (not from the menu itself)
        host::menukey::set(game_menu && game != "menu", game_menu_hold);
#ifdef HK_WITH_SDL
        if (game_menu) host::window_set_menu_key(game_menu_key);
#endif
    }
    if (!script_path.empty() && !script::load(script_path, out)) return 2;
    if (!macro::load(opt.root + "/games/" + game + "/macros.txt")) return 2;
    for (const std::string &d : mod_dirs)   // a mod's own codes, after the game's (docs/MODS.md)
        if (!macro::load(d + "/macros.txt")) return 2;
    for (const auto &m : macro_specs)
        if (!macro::schedule(m)) return 2;
    int script_snaps = 0;
    // Ctrl-C / SIGTERM: leave at the next frame, saving NVRAM like a normal exit
    static std::atomic<bool> stop_requested{false};
    // (the window's waits on the sound are told too: a frame waiting there would never end)
    static const auto stop = [](int) {
        stop_requested = true;
#ifdef HK_WITH_SDL
        host::window_interrupt();
#endif
    };
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    // The game menu's picture of this game (host/menupic.h): the first frame once the machine has
    // run to the game's title screen, if nobody has touched a control by then; one try a run
    if (!menu_picture.empty() && menu_picture_at <= 0) menu_picture_at = manifest_seconds(opt.root + "/games/" + game + "/game.toml", "title_seconds");
    if (menu_picture_at <= 0) menu_picture.clear();
    auto title_picture = [&](const uint32_t *rgb, int w, int h) {
#ifdef HK_WITH_SDL
        // F12 in the window: this screen is the game's picture, the player's own (--menu-picture-own)
        if (host::window_picture_asked()) {
            const bool kept = !menu_picture_own.empty() && host::menupic::save(menu_picture_own, rgb, w, h, machine.display_aspect(), true);
            if (kept) printf("game menu: this screen kept as the game's picture (F12), %.1f s in\n", machine.seconds());
            // (the boxes' texts: English here, as the log has them; shown in the player's language)
            host::notice::hint({kept ? HK_N("boxes", "Picture kept") : HK_N("boxes", "No picture kept"),
                                kept                       ? HK_N("boxes", "This screen is the game's picture in the game menu")
                                : menu_picture_own.empty() ? HK_N("boxes", "The game wasn't started from the launcher")
                                                           : HK_N("boxes", "It couldn't be written")},
                               4, host::ui::Icon::picture);
        }
#endif
        if (menu_picture.empty() || machine.seconds() < menu_picture_at) return;
        if (!host::menukey::touched() && host::menupic::save(menu_picture, rgb, w, h, machine.display_aspect()))
            printf("game menu: this game's picture kept, %.1f s in\n", machine.seconds());
        menu_picture.clear();
    };
    // --record and F6 (host/record.h): the frames the window shows (a core's picture at the end of
    // vertical blank, where it has one), else the frame hook's; finished in report() at any way out
    host::record::Capture::Setup record_setup;
    record_setup.format = record_format;
    record_setup.encoder = record_encoder;
    record_setup.scale = record_scale;
    record_setup.audio_rate = machine.audio_rate();
    record_setup.native_rate = machine.native_rate();
    record_setup.aspect = machine.display_aspect();
    record_setup.cover = [&machine](int w, int h, double &cw, double &ch) { machine.display_cover(w, h, cw, ch); };
    record_setup.wait = !window_scale;   // headless: no real time to keep, so the game waits for the encoder
    std::string record_error;
    const bool can_record = capture.setup(record_setup, record_error);
    if (record_at_start && !can_record) {
        fprintf(stderr, "--record: %s\n", record_error.c_str());
        return 2;
    }
    if (record_folder.empty()) record_folder = hk::user_data_dir(opt.root) + "/recordings";
    auto record_start = [&](std::string path) {   // a file, or a folder ("": --record-folder's) to name one in
        if (path.empty() || path.back() == '/' || path.back() == '\\' || hk::sys::is_dir(path))
            path = host::record::file_name(path.empty() ? record_folder : path, game, record_format);
        if (!capture.start(path, record_error)) {
            fprintf(stderr, "record: %s\n", record_error.c_str());
            return false;
        }
        printf("record: to %s, from %.2f s of the machine's time\n", path.c_str(), machine.seconds());
        return true;
    };
    if (record_at_start && !record_start(record_path)) return 2;
#ifdef HK_WITH_SDL
    host::record::set_mark(record_at_start && window_scale);   // (over the window's picture, not in the file)
#endif
    auto record_picture = [&](const uint32_t *rgb, int w, int h) {
#ifdef HK_WITH_SDL
        if (window_scale && host::window_record_asked()) {   // F6
            if (capture.on()) {
                host::record::set_mark(false);
                capture.stop_later([](const std::string &line, const std::string &path) {
                    printf("%s\n", line.c_str());
                    host::notice::hint({HK_N("boxes", "Recording kept"), path}, 8, host::ui::Icon::picture);
                });
            } else if (can_record && record_start("")) {
                host::record::set_mark(true);
                host::notice::hint({HK_N("boxes", "Recording (F6 ends it)"), capture.path()}, 4, host::ui::Icon::picture);
            } else {
                host::notice::hint({HK_N("boxes", "No recording"), record_error}, 8, host::ui::Icon::alert);
            }
        }
        // the fast start-up keeps no real time either: the game waits for the encoder meanwhile
        if (window_scale && capture.on()) capture.set_wait(host::warp::on());
#endif
        capture.picture(rgb, w, h, machine.seconds());
    };
    // F5 (host/screenshot.h): the picture the recorder takes, in the screen's shape, as a PNG in
    // --screenshot-folder's.
    // HK_SCREENSHOT_FRAMES=F1,F2,... (tests): one asked for at those frames of the frame hook (with
    // a window, where the picture comes at the end of vertical blank: the frame before's number)
    if (screenshot_folder.empty()) screenshot_folder = hk::user_data_dir(opt.root) + "/screenshots";
    std::vector<uint64_t> screenshot_frames;
    if (const char *e = getenv("HK_SCREENSHOT_FRAMES"))
        for (const char *p = e; *p;) {
            char *end = nullptr;
            const unsigned long long v = strtoull(p, &end, 10);
            if (end == p) break;
            screenshot_frames.push_back(v);
            p = *end == ',' ? end + 1 : end;
        }
    uint64_t hook_frame = 0;   // the frame hook's last frame (set there)
    auto screenshot_picture = [&](const uint32_t *rgb, int w, int h) {
        bool asked = false;
        for (uint64_t &at : screenshot_frames)
            if (at == hook_frame) asked = true, at = UINT64_MAX;
#ifdef HK_WITH_SDL
        if (window_scale && host::window_screenshot_asked()) asked = true;
#endif
        if (!asked) return;
        // the pixels' shape as a recording of this frame has it (record_setup: the screen's, and the
        // part of it the frame covers, asked now)
        double cw = 1, ch = 1;
        machine.display_cover(w, h, cw, ch);
        const double shape = host::screenshot::pixel_shape(machine.display_aspect(), cw, ch, w, h);
        screenshots.take(rgb, w, h, shape, screenshot_folder, game, [notify = window_scale != 0](bool ok, const std::string &said) {
            if (ok) printf("screenshot: %s\n", said.c_str());
            else fprintf(stderr, "screenshot: none kept: %s\n", said.c_str());
#ifdef HK_WITH_SDL
            if (notify)
                host::notice::hint({ok ? HK_N("boxes", "Screenshot kept") : HK_N("boxes", "No screenshot"), said}, 4,
                                   ok ? host::ui::Icon::picture : host::ui::Icon::alert);
#else
            (void)notify;
#endif
        });
    };
#ifdef HK_WITH_SDL
    // a core that can show the window its picture at the end of vertical blank does: the picture
    // the monitor then shows, the game's flip during the blank included, which the frame hook's
    // (vblank start) has only a frame later, after the frame's active lines have been emulated.
    // The frame hook below then only paces and reads the controls.
    const bool present_at_vblank_end =
        window_scale && machine.set_present_hook([&](const uint32_t *rgb, int w, int h) {
            host::window_present(rgb, w, h, machine.seconds());
            title_picture(rgb, w, h);
            record_picture(rgb, w, h);
            screenshot_picture(rgb, w, h);
        });
#endif
    // HK_BETWEEN_BENCH: the in-between pictures where a window asks for them (with the picture at
    // the end of vertical blank, for a core that has one; else with the frame's)
    auto between_draw = [&](int w, int h) {
        if (!hk::smoothgate::open()) {   // (outside songs: one asked for, not counted, as the window does)
            int bw = w, bh = h;
            machine.render_between(0.0, bw, bh);
            return;
        }
        for (int i = 1; i < between_bench; i++) {
            int bw = w, bh = h;
            const uint64_t t0 = prof::ticks();
            const uint32_t *b = machine.render_between((double)i / between_bench, bw, bh);
            const uint64_t dt = prof::ticks() - t0;
            if (b) {
                between_n++, between_ticks += dt, between_max = std::max(between_max, dt);
                between_hash = hash_words(between_hash, b, (size_t)bw * bh);
            }
        }
    };
    const bool between_at_frame =
        between_bench && !window_scale && !machine.set_present_hook([&](const uint32_t *, int w, int h) { between_draw(w, h); });
#ifdef HK_WITH_SDL
    // the frame hook's picture, when nothing below reads it (the window shows the present hook's):
    // the core needn't compose it
    if (present_at_vblank_end && !digest && script_path.empty() && !snap_every) machine.frame_picture_unused();
#endif
    auto last_save = std::chrono::steady_clock::now();
    // Lines for the launcher's black cover between full-screen programs (launcher/gamemenu.h): the
    // window's first picture, and a hold of the way out to the game menu begun or let go
    const bool tell_launcher = window_scale && getenv("HK_LAUNCHER");
    bool told_picture = false, told_hold = false;
    machine.set_frame_hook([&](uint64_t f, const uint32_t *rgb, int w, int h) {
        frame_times.frame(f);
        hook_frame = f;
        // the machine's saved memories, every so often (the core hands what changed to another thread)
        if (save_every > 0)
            if (const auto t = std::chrono::steady_clock::now(); std::chrono::duration<double>(t - last_save).count() >= save_every) {
                last_save = t;
                const uint64_t t0 = prof::now_ns();
                machine.save_now();
                // (what the game's thread spends on it: a frame dropped if it's long, a 573's flash 13-34 ms)
                static int said = 0;
                if (const double ms = (double)(prof::now_ns() - t0) / 1e6; ms >= 4.0 && said < 10) {
                    said++;
                    fprintf(stderr, "saves: the machine's memories looked at in %.1f ms of frame %llu\n", ms, (unsigned long long)f);
                }
            }
        if (digest)
            fprintf(digest, "%llu %016llx %016llx\n", (unsigned long long)f,
                    (unsigned long long)hash_words(HASH_SEED, rgb, (size_t)w * h), (unsigned long long)audio_hash);
        if (stop_requested) {
            finish();
#ifdef HK_WITH_SDL
            if (window_scale) host::window_close();
#endif
            hk::sys::exit(130);
        }
#ifdef HK_WITH_SDL
        if (window_scale) mods::display_rate(host::pacing::refresh_hz());
#endif
        mods::frame();
        if (smooth_motion) hk::smoothgate::frame([&](uint32_t a) { return machine.mem(a, 4); }, machine.frame_repeated());
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
                hk::sys::exit(machine.exit_code());
            }
        }
        // mods' scripts (host/modscript.h): the controls given, on_frame, their drawing where there's a window
        if (modscript::any()) modscript::frame(f, w, h, window_scale != 0);
        // the lamps the frame just emulated lit, out now: before the wait for the frame's time below
        // (most of a frame, on a PC that keeps up), not after it. The controls are read after it.
#ifdef HK_WITH_SDL
        host::lights_frame();
#endif
        host::bio2_lamps(machine);
        host::popnio_lamps(machine);
#ifdef HK_WITH_SDL
        if (window_scale && !(present_at_vblank_end ? host::window_next(machine.seconds())
                                                    : host::window_frame(rgb, w, h, machine.seconds()))) {
            machine.shutdown();
            lights_off();
            report();
            host::menukey::hand_screen_to_launcher();
            host::window_close();
            hk::sys::exit(0);
        }
#endif
        if (debug_every && f % debug_every == 0) machine.debug_line(f);
#ifdef HK_WITH_SDL
        if (!present_at_vblank_end)
#endif
        {
            title_picture(rgb, w, h);
            record_picture(rgb, w, h);
            screenshot_picture(rgb, w, h);
        }
        if (between_at_frame) between_draw(w, h);
        if (prof::g_sample_frame) prof::g_sample_frame(f);   // HK_PROFILE_FRAMES
        if (lights_log) lights_log(f);
#ifdef HK_WITH_SDL
        if (window_scale)   // the core's brief lines for the player (a memory card in or out), a few seconds each
            for (std::string said = machine.take_notice(); !said.empty(); said = machine.take_notice())
                host::notice::hint({said}, 3, host::ui::Icon::disk);
#endif
        // (the readers' 10-keys go to the way out to the game menu from readers_frame itself, each
        // press and release, whatever the game has)
        host::readers_frame([&](int p, const std::string &n) { return machine.card_read(p, n); },
                            [&](const std::string &name, int v) { return machine.set_input(name, v); });
        host::bio2_frame(machine);
        host::popnio_frame(machine);
        if (tell_launcher) {
            if (!told_picture) {   // (shown by the window_frame/next above)
                told_picture = true;
                host::menukey::window_up_for_launcher();
                printf("window: first picture\n");
            }
            if (const bool held = host::menukey::progress(host::menukey::now()) > 0; held != told_hold)
                told_hold = held, printf(held ? "game menu: hold begun\n" : "game menu: hold let go\n");
        }
        // 5, 7 and 3 held (host/menukey.h): the game ends as a closed window ends it, and says where to
        if (host::menukey::frame(host::menukey::now())) {
            host::menukey::hand_screen_to_launcher();   // (before the line: the launcher acts on it)
            printf("game menu: asked for from the cabinet's controls\n");
            finish();
#ifdef HK_WITH_SDL
            if (window_scale) host::window_close();
#endif
            hk::sys::exit(host::menukey::EXIT_CODE);
        }
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
            host::menukey::hand_screen_to_launcher();
            if (window_scale) host::window_close();   // the sound card back to its owner
#endif
            hk::sys::exit(machine.exit_code());
        }
    });

    machine.run();
}
