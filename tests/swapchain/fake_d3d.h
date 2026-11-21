/* SPDX-License-Identifier: GPL-3.0-or-later */
/* A pretend Direct3D 11 device, DXGI flip-model swap chain and display, and the OpenGL calls
   runtime/host/swapchain.cpp makes (fake_d3d.c), for test_swapchain.cpp: no graphics card, driver or
   window is needed, so the queue of a frame's other pictures and display pacing's accounting can be
   run anywhere (Wine included). */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* The pretend display: its refresh rate, and how long before a vblank a picture must be presented
   to be shown at it (else the next). Before the device is made */
void fake_display(double hz, long long compose_ns);
/* Its last vblank and its period, ns on the performance counter's clock (fake_now_ns) */
long long fake_now_ns(void);
long long fake_last_vblank_ns(long long *period_ns);

/* Every nth present made by the thread that calls this held `ms` ms first (0: none), as a display that
   takes its pictures slowly holds them */
void fake_present_hold(int every, int ms);

/* A window the display isn't showing (Alt+Tab to another, covered, minimized), from now until called
   again with FAKE_SHOWN. Its presents still succeed and count (GetLastPresentCount), and still take
   their refreshes, but aren't shown:
   FAKE_COVER_JUMP: the statistics stand still while it lasts, then name the latest present at once;
   FAKE_COVER_LOST: the statistics stand still, and the presents made meanwhile are never named;
   FAKE_COVER_LIVE: as LOST, but the statistics' vblank (SyncRefreshCount, SyncQPCTime) goes on;
   FAKE_OCCLUDED: each Present returns DXGI_STATUS_OCCLUDED, its picture neither counted nor shown */
enum { FAKE_SHOWN, FAKE_COVER_JUMP, FAKE_COVER_LOST, FAKE_COVER_LIVE, FAKE_OCCLUDED };
void fake_cover(int how);
/* The next present (whichever thread's) held `ms` ms, as Windows holds one when the window changes */
void fake_present_stall(int ms);
/* The next n frame-statistics readings fail (DXGI_ERROR_FRAME_STATISTICS_DISJOINT) */
void fake_statistics_fail(int n);
/* A composed window: every picture shown `refreshes` after the first vblank it is in time for, and the
   statistics naming a vblank only `lag_ns` after it */
void fake_composed(int refreshes, long long lag_ns);
/* The statistics brought up to date only when a present is made (as found then), not at each vblank:
   with no present after it, the last ones are never named shown (a Windows 11 VM's, measured) */
void fake_statistics_at_present(int on);

/* The number the next picture OpenGL hands over carries (set before each is "drawn") */
extern volatile long long fake_drawn;

/* Every present so far: which picture, when, by which thread, the vblank it is shown at */
struct fake_present {
    unsigned id;            /* its present number */
    long long stamp;        /* fake_drawn when OpenGL handed the picture over */
    long long at_ns;        /* when Present was called (fake_now_ns) */
    long long vblank_ns;    /* when it reaches the screen */
    unsigned long thread;
    int covered;            /* presented while the window wasn't shown (fake_cover's how) */
};
int fake_presents(const struct fake_present **log);

/* What the pretend device saw that the real ones don't allow (each also printed):
   two threads in Direct3D at once, or a second thread without the multithread protection; a copy
   from a texture OpenGL still holds; OpenGL drawing into a texture it hasn't locked; the swap
   chain's buffers resized with the back buffer still held */
extern volatile long fake_errors;

/* SDL_GL_GetProcAddress's stand-in */
void *fake_get_proc(const char *name);

#ifdef __cplusplus
}
#endif
