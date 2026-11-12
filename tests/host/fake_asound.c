// SPDX-License-Identifier: GPL-3.0-or-later
// A pretend libasound.so.2 for tests: the calls runtime/host/alsa.cpp makes, on a card that plays
// what it is given at its rate by the clock, and that a test can take away. Put before the real
// library with LD_LIBRARY_PATH (HIRAKU loads libasound by name at run time).
//
//   FAKE_ASOUND_GONE=FILE    while FILE exists the card is pulled out: every call on it answers
//                            "no such device", and it can't be opened
//   FAKE_ASOUND_STUCK=FILE   while FILE exists the card plays nothing: its ring fills and stays full
//   FAKE_ASOUND_LOG=FILE     each open and close, a line
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef unsigned long uframes;
typedef struct {
    unsigned rate;
    uframes period, ring;
    int64_t written;     // frames since it (re)started
    double played;       // ... of them played, up to `at`
    uint64_t at;         // ns
    int running, xrun;
} pcm;
typedef struct {
    unsigned rate;
    uframes period, ring;
} hw;

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
static int there(const char *env) {
    const char *f = getenv(env);
    return f && access(f, F_OK) == 0;
}
static void said(const char *what, const char *name) {
    const char *f = getenv("FAKE_ASOUND_LOG");
    FILE *fp = f ? fopen(f, "a") : NULL;
    if (fp) fprintf(fp, "%s %s\n", what, name), fclose(fp);
}

// the card's clock, up to now: what it has played of what was written
static void play(pcm *p) {
    const uint64_t t = now_ns();
    if (p->running && !there("FAKE_ASOUND_STUCK")) {
        p->played += (double)(t - p->at) * p->rate / 1e9;
        if (p->played > (double)p->written) p->played = (double)p->written, p->xrun = 1, p->running = 0;   // ran dry
    }
    p->at = t;
}
static long avail(pcm *p) { return (long)p->ring - (long)(p->written - (int64_t)p->played); }

int snd_pcm_open(pcm **out, const char *name, int stream, int mode) {
    (void)stream, (void)mode;
    if (there("FAKE_ASOUND_GONE")) return said("open-refused", name), -ENOENT;
    *out = calloc(1, sizeof(pcm));
    said("open", name);
    return 0;
}
int snd_pcm_close(pcm *p) {
    said("close", "");
    free(p);
    return 0;
}
int snd_pcm_hw_params_malloc(hw **h) { return *h = calloc(1, sizeof(hw)), 0; }
void snd_pcm_hw_params_free(hw *h) { free(h); }
int snd_pcm_hw_params_any(pcm *p, hw *h) { return (void)p, (void)h, 0; }
int snd_pcm_hw_params_set_access(pcm *p, hw *h, int a) { return (void)p, (void)h, (void)a, 0; }
int snd_pcm_hw_params_set_format(pcm *p, hw *h, int f) { return (void)p, (void)h, f == 14 /* float */ ? 0 : -EINVAL; }
int snd_pcm_hw_params_set_channels(pcm *p, hw *h, unsigned c) { return (void)p, (void)h, c == 2 ? 0 : -EINVAL; }
int snd_pcm_hw_params_set_rate_near(pcm *p, hw *h, unsigned *rate, int *dir) { return (void)p, (void)dir, h->rate = *rate, 0; }
int snd_pcm_hw_params_set_period_size_near(pcm *p, hw *h, uframes *n, int *dir) { return (void)p, (void)dir, h->period = *n, 0; }
int snd_pcm_hw_params_set_buffer_size_near(pcm *p, hw *h, uframes *n) { return (void)p, h->ring = *n, 0; }
int snd_pcm_hw_params_get_period_size(const hw *h, uframes *n, int *dir) { return (void)dir, *n = h->period, 0; }
int snd_pcm_hw_params_get_buffer_size(const hw *h, uframes *n) { return *n = h->ring, 0; }
int snd_pcm_hw_params(pcm *p, hw *h) {
    if (there("FAKE_ASOUND_GONE")) return -ENODEV;
    p->rate = h->rate, p->period = h->period, p->ring = h->ring;
    p->at = now_ns();
    return 0;
}

long snd_pcm_avail_update(pcm *p) {
    if (there("FAKE_ASOUND_GONE")) return -ENODEV;
    play(p);
    return p->xrun ? -EPIPE : avail(p);
}
// until a period is free, or `ms` have passed: 1 or 0
int snd_pcm_wait(pcm *p, int ms) {
    for (const uint64_t end = now_ns() + (uint64_t)ms * 1000000ull;;) {
        if (there("FAKE_ASOUND_GONE")) return -ENODEV;
        play(p);
        if (p->xrun) return -EPIPE;
        if (avail(p) >= (long)p->period) return 1;
        if (now_ns() >= end) return 0;
        usleep(200);
    }
}
long snd_pcm_writei(pcm *p, const void *data, uframes n) {
    (void)data;
    if (there("FAKE_ASOUND_GONE")) return -ENODEV;
    play(p);
    if (p->xrun) return -EPIPE;
    if ((long)n > avail(p)) n = avail(p) > 0 ? (uframes)avail(p) : 0;   // (HIRAKU writes only what is free)
    p->written += (int64_t)n;
    p->running = 1;   // it starts with the first frames
    return (long)n;
}
// an underrun: the card ready again, empty. Anything else: not this call's to mend
int snd_pcm_recover(pcm *p, int err, int silent) {
    (void)silent;
    if (there("FAKE_ASOUND_GONE")) return -ENODEV;
    if (err != -EPIPE) return err;
    p->written = 0, p->played = 0, p->xrun = 0, p->running = 0, p->at = now_ns();
    return 0;
}
int snd_pcm_drop(pcm *p) {
    if (there("FAKE_ASOUND_GONE")) return -ENODEV;
    p->running = 0;
    return 0;
}
const char *snd_strerror(int e) { return strerror(e < 0 ? -e : e); }
int snd_card_get_index(const char *name) { return (void)name, -ENODEV; }   // (no card to reserve from a sound server)
