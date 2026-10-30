// SPDX-License-Identifier: GPL-3.0-or-later
// Fast start-up (--fast-boot): the game's own start-up (its checks, its loading, the screens before
// its title) run as fast as the machine can go, with no sound, until the machine's time reaches the
// game's ready point (games/<id>/game.toml boot_seconds), or a player presses one of the game's
// buttons; then the game goes on at its own speed. The game still boots: only the wait goes. Its
// pictures show meanwhile, at most 60 a second, under a box saying so (its speed, how far it is).
// Nothing the game computes changes: only when the window waits.
#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

namespace hk::host::warp {

// (here, so that the cabinet I/O boards, built without SDL, need nothing else of it)
inline std::atomic<bool> g_on{false}, g_pressed{false};

// Before the first frame: run fast until the machine's time is `until` seconds (0: off)
void set(double until);
inline bool on() { return g_on; }
// A game button (or a coin, the test switch...) pressed by a player: the start-up goes on at the
// game's speed from now (host/input.cpp, the cabinet I/O boards)
inline void pressed() {
    if (g_on) g_pressed = true;
}
// Each frame's start, with the machine's time: ends it at the ready point; true while it runs
bool frame(double vsec);
// The box over the picture: false when there's none. pixels ARGB, w x h; alpha fades it at the end
struct Panel {
    const std::vector<uint32_t> *pixels;
    int w, h;
    float alpha;
    unsigned id;   // changes with the picture
};
bool panel(Panel &p);
// The log's line, once it has ended ("" before)
const char *report();

}  // namespace hk::host::warp
