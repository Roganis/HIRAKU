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

/* The number the next picture OpenGL hands over carries (set before each is "drawn") */
extern volatile long long fake_drawn;

/* Every present so far: which picture, when, by which thread, the vblank it is shown at */
struct fake_present {
    unsigned id;            /* its present number */
    long long stamp;        /* fake_drawn when OpenGL handed the picture over */
    long long at_ns;        /* when Present was called (fake_now_ns) */
    long long vblank_ns;    /* when it reaches the screen */
    unsigned long thread;
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
