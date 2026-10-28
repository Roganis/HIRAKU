// SPDX-License-Identifier: GPL-3.0-or-later
// A machine core: everything the shell (runtime/main.cpp, the host front end,
// input scripts, lamps) needs from an emulated board, and nothing more.
//
// Each hiraku-<game> binary links exactly one core, which defines hk::core().
// Firebeat (runtime/firebeat/core.cpp) runs recompiled PowerPC; DJ Main
// (runtime/djmain/core.cpp) interprets a 68EC020. The shell never sees which.
//
// Control flow: run() boots the machine and never returns. Once per video
// frame the core calls the frame hook, which is where the shell checks its
// stop conditions and ends the program (after shutdown() and report()).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hk {

class Core {
public:
    struct Options {
        std::string game;        // game id (games/<id>/game.toml, dumps/<id>/)
        std::string root;        // the HIRAKU checkout
        std::string out;         // snapshots, traces
        std::string nvram_dir;   // persistent NVRAM ("" = none: every run starts from the defaults)
        bool trace = false;      // write an oracle-format MMIO trace into `out`
        bool ata_log = false;    // log disk commands
        bool window = false;     // HIRAKU's window shows it (else headless: nothing paces but the core)
        // a file named in machine.txt: absolute, or relative to dumps/<game>/
        std::string data_path(const std::string &p) const {
            // as it is: absolute (/..., \\..., C:...), where machine.txt names a file left in its place
            if (p.empty() || p[0] == '/' || p[0] == '\\' || (p.size() > 1 && p[1] == ':')) return p;
            return root + "/dumps/" + game + "/" + p;
        }
    };
    using FrameHook = std::function<void(uint64_t frame, const uint32_t *rgb, int w, int h)>;
    using AudioSink = std::function<void(const float *lr, int frames)>;   // stereo, at audio_rate()

    virtual ~Core() = default;

    // A command-line option only this core knows (e.g. Firebeat's --interp-only).
    // `value` fetches the option's argument. Returns false for an unknown option.
    virtual bool option(const std::string &, const std::function<std::string()> &) { return false; }
    // Usage lines for those options.
    virtual std::string options_help() const { return {}; }

    // Load the machine (dumps/<game>/machine.txt and rom/). false: a message was printed.
    virtual bool init(const Options &o) = 0;
    virtual const std::string &family() const = 0;   // selects controls and lamps (shell/cabinet.h)

    virtual void set_frame_hook(FrameHook h) = 0;
    // The picture as the end of vertical blank leaves it, which the monitor then shows through
    // the frame's active lines: the game's flip during the blank (in its vblank interrupt) is in
    // it, where the frame hook's picture (vblank start) has it a frame later. A core that can, calls
    // `h` at every vblank end and returns true; the shell then shows the window this picture. The
    // frame hook stays at vblank start for everything else (digests, scripts, snapshots).
    using PresentHook = std::function<void(const uint32_t *rgb, int w, int h)>;
    virtual bool set_present_hook(PresentHook) { return false; }
    virtual void set_audio_sink(AudioSink s) = 0;
    // Called by the core when it cannot continue, before it exits.
    virtual void set_on_fatal(std::function<void()> f) = 0;

    [[noreturn]] virtual void run() = 0;
    virtual void shutdown() = 0;        // flush NVRAM and traces (before exiting)
    virtual void report() = 0;          // run statistics, one stdout line each
    virtual int exit_code() const { return 0; }   // non-zero if the run found a fault
    virtual void debug_line(uint64_t /*frame*/) {}  // --debug-every: one line of CPU state

    virtual double seconds() const = 0;           // virtual time since power-on
    // `len` bytes of guest memory from `addr`, in the guest's (big-endian) byte
    // order, for input scripts' watches, hashes and dumps. nullptr if the range is
    // not plain memory.
    virtual const uint8_t *mem(uint32_t addr, uint32_t len) const = 0;
    // A mod wrote len bytes at addr (mem()'s memory, host/mods.h): a core running translated or
    // recorded code from there translates it again (or runs it interpreted); one that interprets
    // needs nothing
    virtual void code_changed(uint32_t addr, uint32_t len) { (void)addr, (void)len; }
    // A mod asks for the machine's vsync to come at hz (host/mods.h [[rate]]); 0: the machine's
    // own rate again. The CPUs keep their clocks: the game gets more frames a second, each with
    // less time. false where the core can't
    virtual bool set_vsync_rate(double hz) { (void)hz; return false; }
    // The machine's own vsync rate (the cabinet's ticks, host/mods.h between_ticks)
    virtual double native_rate() const { return 59.94; }
    // This frame shows the last picture again, the game having drawn none (a game in a process of its
    // own, loading or held up): a [[rate]]'s watched word can't have changed, so it isn't looked at
    virtual bool frame_repeated() const { return false; }
    // Between the machine's frames, for a display faster than the game (--smooth-motion): the last
    // frame with what moves drawn `t` (0 < t < 1) of a frame further along its move, w x h.
    // nullptr: the core can't (or not yet)
    virtual const uint32_t *render_between(double t, int &w, int &h) { (void)t, (void)w, (void)h; return nullptr; }
    virtual uint64_t frame_count() const = 0;

    // Inputs by MAME name ("P1 Button 3", "Coin 1", "TURNTABLE_P1/Turntable"):
    // 1/0 for buttons, the position for analog inputs. false for an unknown name.
    virtual bool set_input(const std::string &name, int value) = 0;
    // Frames by which an input script's button changes (press, hold, release) lag
    // behind their frame number, to match the MAME oracle: MAME latches digital
    // inputs in its frame update, which runs just before the Lua script's frame
    // callback, so a button pressed at frame F is seen from frame F+1's latch.
    // Analog values set by a script are read live in MAME and never lag. Host
    // input doesn't go through this. 0 keeps the cores whose recorded runs predate it.
    virtual int script_button_delay() const { return 0; }
    // The sample rate of what the audio sink gets: the machine's own where the
    // host can take it as it is (DJ Main's sound chips run at 48 kHz).
    virtual int audio_rate() const { return 44100; }
    // The sound comes to the audio sink once per video frame, just before the frame hook (the
    // whole frame's), not as the machine makes it. Without display sync the window then waits for
    // the sound before letting the next frame run, not before showing this one
    // (runtime/host/window.cpp, window_frame), so the picture comes a frame sooner.
    virtual bool audio_per_frame() const { return false; }
    // A real card reader read this card (16 hex digits, E004...): it is now `player`'s (0, 1) card,
    // put to the game's reader as the player would. false for a machine without that reader.
    virtual bool card_read(int /*player*/, const std::string & /*number*/) { return false; }
    // Move player's turntable by `delta` encoder counts (cores without one ignore it).
    virtual void turntable_add(int player, int delta) = 0;
    // Lamps as the game last drove them, in cab::lamps(family()) order (1 = lit),
    // and a counter that moves on every change.
    virtual const std::vector<uint8_t> &lamp_state() const = 0;
    virtual uint64_t lamp_changes() const = 0;
    // A text display the game drives (beatmania IIDX's 16-segment ticker), as it reads now; ""
    // for machines without one. lamp_changes() moves when it changes.
    virtual std::string display_text() const { return {}; }
    // Something to set up before playing, said on the window for a while when the game starts
    // (runtime/host/notice.h), two lines; empty when all is set (Keyboardmania and ParaParaParadise
    // with their sound flash still blank)
    virtual std::vector<std::string> setup_warning() const { return {}; }
    // The monitor's shape, width over height, that every frame fills whatever its pixel count: a
    // board can switch video modes (the PlayStation-based System 573 shows 320x240, 512x480 and
    // 640x480 on the same 4:3 tube; Twinkle's on a 16:9 one), so its pixels aren't square.
    // 0: square pixels.
    virtual double display_aspect() const { return 4.0 / 3.0; }
    // The monitor's line rate, kHz: 15 (a TV-like arcade monitor: System 573, Twinkle), 24 (medium
    // resolution: DJ Main) or 31 (VGA-like: Firebeat). The CRT pictures follow it
    // (runtime/host/picture.h, set_monitor)
    virtual int monitor_khz() const { return 15; }
    // The monitors turned on their side, the tube's first line at the frame's left (Keyboardmania's,
    // MAME ROT270): the CRT beam simulator sweeps left to right in each of screens()
    virtual bool monitor_turned() const { return false; }
    // How many monitors a frame holds, side by side and equally wide (Keyboardmania: 2, one a
    // player). display_aspect is the whole frame's; the window can show each in its own
    // (runtime/host/window.h, window_set_split)
    virtual int screens() const { return 1; }
    // The part of that screen a w x h frame covers, width and height as fractions, centred: 1, 1
    // when it fills the screen. The Python core's frames are the GS's display area, which on a TV
    // sits inside a wider and taller raster with black around it.
    virtual void display_cover(int w, int h, double &cw, double &ch) const { (void)w, (void)h, cw = ch = 1.0; }
};

Core &core();   // defined by the core this binary is built with

}  // namespace hk
