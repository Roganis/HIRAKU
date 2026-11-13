// SPDX-License-Identifier: GPL-3.0-or-later
// How the game runs on this PC, over its picture (F7, --stats-overlay): what a general tool can't
// know. Whether each of the game's frames held its place on the display and its sound stayed whole,
// and where a slow frame's time went.
//
//   F7          the next level: one line, the panel, everything, off
//   Shift+F7    the panel to the next corner (for a cabinet with no mouse)
//   the mouse   drag the panel anywhere on the picture (in full screen the pointer shows while the
//               mouse moves); where it was left is kept for the next game
//
// One line: the game's pictures a second, a frame's work against its time, the game thread's share
// of a processor, the sound queued, frames shown late.
// The panel: the rates (the game's, the display's, the speed the game runs at for display sync);
// frames late, by the display's own account where it gives one; a frame's work and the processor
// time; a graph of the last three seconds, each frame a bar (the game's work, the window's on top)
// against the frame's time (the line), red where a frame was late, amber where the sound broke, grey
// a frame the game drew no picture for (a PC game loading); the
// sound's queue, target and way out; the display sync's state and what else is on.
// Everything adds: the latencies (a press to its frame's start, a frame's start to its picture and
// to its sound), this program's busiest threads, the graphics card (host/sysload.h), the
// processors' clock, and the core's own lines (Core::overlay_lines).
//
// A frame whose work didn't fit its time (or shown late with display sync, or with a break in the
// sound) is kept on the panel in red for a few seconds, with where its time went; and, the overlay
// shown or not, the worst such frame of each five seconds is a line in the log ("slow frame ..."),
// forty a run at most, so a report of a stutter says what it was.
//
// A panel in SDL's 8x8 font, drawn at most ten times a second (its text when a number changes); the window lays it over the picture
// it shows, so no snapshot, digest or recording has it, and nothing the game computes changes.
#pragma once

#include <SDL3/SDL_events.h>

#include <cstdint>
#include <string>
#include <vector>

#include "../core/prof.h"

namespace hk::host::statsview {

enum class Level { off, line, panel, full };
bool parse_level(const std::string &s, Level &l);   // "off", "line", "panel", "full"
void set_level(Level l);
Level level();
// The file the panel's place is kept in ("": not kept), read now
void set_store(const std::string &file);

// A frame's account, from the window, every frame (shown or not: the graph and the log)
struct Frame {
    uint64_t number = 0;
    float game_ms = 0;      // the machine's work
    float draw_ms = 0;      // the window's: the picture drawn and handed over
    float period_ms = 0;    // the frame's time in real time (the game's period at the speed it runs at)
    bool late = false;      // shown late: the display's word with display sync, else the frame came long after its time
    bool sync = false;      // ... which of the two
    int breaks = 0;         // breaks in the sound since the frame before
    bool again = false;     // the last picture shown again, the game having drawn none (Core::frame_repeated: a PC
                            // game loading or held up; its own report has those, they aren't "slow frames")
    float part_ms[prof::NCAT] = {};   // the work by part (core/prof.h)
    int cpu_mhz = 0, cpu_nominal_mhz = 0;   // the processors' clock about now and their nominal one (0: not known:
                                            // Windows says; a slow frame's line has it when the PC ran them slow)
};
void frame(const Frame &f);
void gap();   // the frames aren't at the game's pace now (fast-forward): none counted, the next starts afresh

// The window's own numbers, asked for once a second while the panel shows
struct Window {
    bool sound = false;
    std::string output;                              // the sound's way out
    float queue_ms = 0, target_ms = 0, device_ms = 0;
    uint64_t breaks = 0;
    std::string picture;                             // the picture's mode, "" sharp pixels
    bool smooth = false, beam = false;
    float picture_p50 = 0, picture_p99 = 0;          // a frame's start to its picture handed to the display (0: none yet)
    float sound_p50 = 0, sound_p99 = 0;              // ... to that moment's sound entering the device
};
void set_source(void (*fill)(Window &));

// An event of the main window. scale: the window's pixels per unit of the event's coordinates.
// true: the overlay's own (F7, a drag of the panel), not the game's
bool event(const SDL_Event &e, float scale);
bool dragging();

// The panel as it is now: false when hidden. ARGB, w x h; id changes with the picture.
struct Panel {
    const std::vector<uint32_t> *pixels;
    int w, h;
    unsigned id;
};
bool panel(Panel &p);
// Where the window draws it, in a window of win_w x win_h pixels: whole times its size, at the
// place it was left (the top right at first)
void place(const Panel &p, int win_w, int win_h, int &x, int &y, int &dw, int &dh);

// For tests: the clock (ns) in place of the steady one, and the place as fractions of the free room
void set_clock(uint64_t (*now)());
void where(double &fx, double &fy);

}  // namespace hk::host::statsview
