// SPDX-License-Identifier: GPL-3.0-or-later
// What the PC is doing besides the game's own thread, for the performance overlay's last page
// (host/statsview.h): the graphics card's load, the processors' speed, and this program's busiest
// threads. Read once a second by a thread of its own (files of /proc and /sys, a driver's library,
// Windows' counters: none of it on the game's thread), and only while the overlay asks.
//
// The graphics card:
// - Linux, AMD: the card's own load (gpu_busy_percent) and clock; this program's share of it from
//   the kernel's per-client times (/proc/self/fdinfo).
// - Linux, Intel: this program's share (the same way); the card's whole load isn't said without
//   more rights than a game has.
// - Linux, NVIDIA's driver: its library (libnvidia-ml), loaded if it is there.
// - Windows: the "GPU Engine" counters (the 3D engines' use, all programs' and this one's).
// A PC game (hiraku-pc) draws in its own process: the card's whole load has it, "this window" is
// only HIRAKU's part (the game's pictures put on the window, the shaders).
#pragma once

#include <string>
#include <vector>

#include "sysload_parse.h"

namespace hk::host::sysload {

struct Now {
    uint64_t n = 0;            // readings so far (0: none yet)
    std::string gpu;           // the card: "AMD", "Intel", "NVIDIA", "" unknown
    float gpu_busy = -1;       // the whole card, percent (-1: not known)
    float gpu_own = -1;        // this program's share of it, percent (-1: not known)
    int gpu_mhz = 0;           // its clock now (0: not known)
    int cpu_mhz = 0;           // the fastest processor now (0: not known)
    std::vector<Thread> threads;   // this program's busiest
};

void run(bool on);   // read once a second from now on, or stop
Now now();

#ifdef __linux__
// The card's own files under `sys` (/sys): the card at PCI address `pdev` ("": the first that says
// its load), into n's gpu, gpu_busy and gpu_mhz; false if no card is found there. (NVIDIA's goes
// through its library, not these files: gpu says "NVIDIA" and the load stays unknown here.)
bool read_card(const std::string &sys, const std::string &pdev, Now &n);
#endif

}  // namespace hk::host::sysload
