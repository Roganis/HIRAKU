// SPDX-License-Identifier: GPL-3.0-or-later
// Windows: the window's pictures presented through a DXGI flip-model swap chain, OpenGL drawing
// them (WGL_NV_DX_interop2: a Direct3D 11 texture shared with OpenGL as a renderbuffer).
//
// OpenGL's own swap on Windows goes through DWM as a "blt" present: DWM copies the window's
// picture into its composition at its own time, a picture presented twice in a refresh replaces
// the first, and one presented a little late waits for the next composition. A flip-model swap
// chain hands DWM the picture itself, in a queue: each picture shown for a refresh at least, in
// the order presented, none replaced; fullscreen with nothing over it, Windows lets the swap chain
// flip the display itself (independent flip: no composition, its latency gone). And it says when
// each picture reached the screen (its frame statistics), which OpenGL's swap never did: display
// pacing then finds its lead from that, as on Wayland (pacing.cpp).
//
// OpenGL draws the window's picture into a framebuffer of its own (fbo(): in place of 0); present()
// turns it the right way up into the shared texture, copies that to the swap chain's back buffer
// and presents it at the next refresh after the last one's. Where any of it isn't there (the
// driver has no WGL_NV_DX_interop2: Wine; HK_NO_DXGI set), start() says why and the window keeps
// OpenGL's swap. Windows only.
#pragma once

#include <cstdint>
#include <string>

namespace hk::host::dxgi {

// The swap chain on the window (an HWND) at its size in pixels, OpenGL's context current; false
// (with why) if it can't be: OpenGL's swap then, as before
bool start(void *hwnd, int w, int h, std::string &why);
bool on();
// OpenGL's framebuffer for the window's picture (0 when off: the window's own)
unsigned fbo();
// The window's size in pixels, each picture: the buffers follow it
void resize(int w, int h);
// The picture drawn into fbo(), to the screen: in_turn (display pacing), at the refresh after the
// last picture's, none replaced; else at the next refresh, replacing one still waiting. An extra
// one (a frame's in-between picture, or its refreshes' others) is left out when two are already
// waiting: the queue drains instead of showing everything later. true if presented
bool present(bool in_turn, bool extra = false);
// The swap chain's present number of the last picture presented (0 before any)
uint32_t last_present();

// The latest picture that reached the screen (frame statistics)
struct Shown {
    uint32_t id;          // its present number (last_present()'s)
    int64_t vblank_ns;    // the refresh the statistics were taken at, on prof::now_ns()'s clock
    uint32_t since;       // ... that many refreshes after the picture's
    uint32_t queued;      // presents not on the screen yet
    bool direct;          // not composed by DWM (independent flip, or an overlay plane)
};
bool shown(Shown &s);

void stop();
std::string describe();   // "" when off

}  // namespace hk::host::dxgi
