// SPDX-License-Identifier: GPL-3.0-or-later
// Windows: the display's vblank, for display pacing where there's no Wayland (pacing.cpp): when the
// display last refreshed, and how often it does. From the graphics kernel's own vblank events for the
// window's monitor (a thread, once start()ed), else the desktop compositor's (DWM's) estimate.
// Windows only.
#pragma once

#include <cstdint>
#include <string>

namespace hk::host::dwm {

// Follow the vblanks of the monitor the window (an HWND) is on: again when it moves to another
void start(void *hwnd);

// The last vblank, on prof::now_ns()'s clock, and the refresh period, both ns; false where neither
// the kernel nor DWM says
bool vblank(int64_t &last_ns, int64_t &period_ns);

bool kernel();          // the kernel's vblanks are what vblank() gives (else DWM's estimate)
std::string report();   // the periods the kernel's vblanks gave over the run ("" if none)

}  // namespace hk::host::dwm
