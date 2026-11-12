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
//
// A frame's other refreshes' pictures (smooth motion's in-between ones, the CRT beam's) don't wait
// for their times on the game's thread: all are drawn right after the frame's own, and queue() keeps
// each in a shared texture of its own with the time to present it. A thread of the swap chain's
// presents them then, in order (Direct3D only on it: a copy to the back buffer and the present; the
// device has its multithread protection on). The game's thread is back at the machine at once, as
// with Wayland's fifo, where the compositor holds them: waiting there, a frame's emulation had one
// refresh to fit in whatever the frame's length (7 ms at 120 Hz, 3 at 240). The thread reads the
// frame statistics before each of its presents and keeps them for display pacing (seen(), went()).
// HK_DXGI_NO_QUEUE: the old way, each picture waited for on the game's thread (to compare).
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

// The picture drawn into fbo(), kept and presented at `at_ns` (prof::now_ns()'s clock) by the swap
// chain's own thread, in turn, after those queued before it; left out there when two are already
// waiting, as present()'s extra ones. false: not taken (no texture free: its thread is far behind;
// or queues() is false). present() and settle() wait for the queued ones first, so a frame's own
// picture never goes before the in-between ones of the frame before
bool queue(int64_t at_ns, uint32_t ticket);
bool queues();   // queue() is of use (on, not HK_DXGI_NO_QUEUE, the driver shares the textures)
void settle();   // wait until every queued picture has been presented (or left out)
// The queued pictures dealt with since the last call, oldest first: each one's ticket and its
// present number (0: left out, or given up at a resize)
struct Went {
    uint32_t ticket, id;
};
size_t went(Went *out, size_t max);

// The latest picture that reached the screen (frame statistics)
struct Shown {
    uint32_t id;          // its present number (last_present()'s)
    int64_t vblank_ns;    // the refresh the statistics were taken at, on prof::now_ns()'s clock
    uint32_t since;       // ... that many refreshes after the picture's
    uint32_t queued;      // presents not on the screen yet
    bool direct;          // not composed by DWM (independent flip, or an overlay plane)
};
bool shown(Shown &s);
// The statistics the queue's thread read before each of its presents since the last call, oldest
// first (each a different picture's): what shown() would have said then
size_t seen(Shown *out, size_t max);

void stop();
std::string describe();   // "" when off

}  // namespace hk::host::dxgi
