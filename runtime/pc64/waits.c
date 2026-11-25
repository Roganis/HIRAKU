// SPDX-License-Identifier: GPL-3.0-or-later
// The game's own frame limiter switched off (SleepEx_ below), and HK_PC64_FRAME_LOG's wait
// accounting (tests, off by default — with the switch off only the limiter's SleepEx is hooked):
// the time the game's MAIN thread spends per frame inside the waits it calls from the
// game's own DLL — Sleep, SleepEx, WaitForSingleObject(Ex), WaitForMultipleObjects(Ex),
// MsgWaitForMultipleObjects(Ex), hooked at the game's import table only, so every call seen here
// comes from the game's code. Per 1200-frame window (link.c calls pc64_waits_window) the three
// call sites (caller RVA) with the most total time are logged. The device's WaitForVBlank and
// GetRasterStatus are timed in display.c (pc64_note_devwait).
#include "pc64.h"

#include <stdlib.h>
#include <string.h>

static int g_on;
static DWORD g_main;          // the game's main thread (the one that runs dll_entry_main)
static HMODULE g_game;        // the game's module, for the caller's RVA

#define MAXSITES 32
static struct { uint32_t rva; uint64_t sum_us; unsigned n; const char *name; } g_sites[MAXSITES];
static int g_nsites;

static void note(const char *name, uint32_t rva, uint32_t us) {
    for (int i = 0; i < g_nsites; i++)
        if (g_sites[i].rva == rva && g_sites[i].name == name) { g_sites[i].sum_us += us; g_sites[i].n++; return; }
    if (g_nsites < MAXSITES) {
        g_sites[g_nsites].rva = rva;
        g_sites[g_nsites].sum_us = us;
        g_sites[g_nsites].n = 1;
        g_sites[g_nsites].name = name;
        g_nsites++;
    }
}
// one wrapper shape: time the real call when it is the game's main thread calling from the game
static DWORD(WINAPI *r_WaitForSingleObject)(HANDLE, DWORD);
static DWORD(WINAPI *r_WaitForSingleObjectEx)(HANDLE, DWORD, BOOL);
static DWORD(WINAPI *r_WaitForMultipleObjects)(DWORD, const HANDLE *, BOOL, DWORD);
static DWORD(WINAPI *r_WaitForMultipleObjectsEx)(DWORD, const HANDLE *, BOOL, DWORD, BOOL);
static DWORD(WINAPI *r_MsgWaitForMultipleObjects)(DWORD, const HANDLE *, BOOL, DWORD, DWORD);
static DWORD(WINAPI *r_MsgWaitForMultipleObjectsEx)(DWORD, const HANDLE *, BOOL, DWORD, DWORD, DWORD);
#define WAITHOOK(ret, name, proto, args, callargs) \
    static ret WINAPI name##_ proto { \
        if (!g_on || GetCurrentThreadId() != g_main) return r_##name callargs; \
        LARGE_INTEGER q0, q1, fq; \
        if (!QueryPerformanceCounter(&q0)) return r_##name callargs; \
        const void *ret_addr = __builtin_return_address(0); \
        ret r = r_##name callargs; \
        if (QueryPerformanceCounter(&q1) && QueryPerformanceFrequency(&fq)) \
            note(#name, (uint32_t)((const char *)ret_addr - (const char *)g_game), \
                 (uint32_t)((q1.QuadPart - q0.QuadPart) * 1000000 / fq.QuadPart)); \
        return r; \
    }

static void(WINAPI *r_Sleep)(DWORD);
static void WINAPI Sleep_(DWORD ms) {
    if (!g_on) { r_Sleep(ms); return; }
    LARGE_INTEGER q0, q1, fq;
    const void *ra = __builtin_return_address(0);
    if (GetCurrentThreadId() != g_main || !QueryPerformanceCounter(&q0)) { r_Sleep(ms); return; }
    r_Sleep(ms);
    if (QueryPerformanceCounter(&q1) && QueryPerformanceFrequency(&fq))
        note("Sleep", (uint32_t)((const char *)ra - (const char *)g_game),
             (uint32_t)((q1.QuadPart - q0.QuadPart) * 1000000 / fq.QuadPart));
}
// The game's own frame limiter (a 64-bit PC IIDX). Before each Present its main loop sleeps what is
// left of 1000/rate ms, less one:  comisd; jbe +13; cvttsd2si rcx,xmm3; xor edx,edx; call [SleepEx].
// HIRAKU paces the game at its Present already (the link's frame_go), and the two together make
// every frame take a little over its time: with display sync each one then misses its refresh
// (three refreshes a picture at 240 Hz: 80 pictures a second). The sleep called from that shape
// returns at once; the game then waits for its next frame at Present, like the other PC games.
// HK_PC64_OWN_LIMITER=1 keeps the game's sleep (to compare).
static int g_keep_limiter;
static int limiter_call(const unsigned char *ra) {
    static const unsigned char shape[] = {0x76, 0x0d, 0xf2, 0x48, 0x0f, 0x2c, 0xcb, 0x33, 0xd2, 0xff, 0x15};
    static const unsigned char *known;
    if (ra == known) return 1;
    if (known || memcmp(ra - 15, shape, sizeof shape) != 0) return 0;
    known = ra;
    pc64_log("the game's own frame limiter (its sleep before each picture, game+%lx): off, HIRAKU paces the game (HK_PC64_OWN_LIMITER=1 keeps it)",
             (unsigned long)(ra - 6 - (const unsigned char *)g_game));
    return 1;
}
static DWORD(WINAPI *r_SleepEx)(DWORD, BOOL);
static DWORD WINAPI SleepEx_(DWORD ms, BOOL alert) {
    const void *ra = __builtin_return_address(0);
    if (!g_keep_limiter && limiter_call(ra)) return 0;
    if (!g_on) return r_SleepEx(ms, alert);
    LARGE_INTEGER q0, q1, fq;
    if (GetCurrentThreadId() != g_main || !QueryPerformanceCounter(&q0)) return r_SleepEx(ms, alert);
    DWORD r = r_SleepEx(ms, alert);
    if (QueryPerformanceCounter(&q1) && QueryPerformanceFrequency(&fq))
        note("SleepEx", (uint32_t)((const char *)ra - (const char *)g_game),
             (uint32_t)((q1.QuadPart - q0.QuadPart) * 1000000 / fq.QuadPart));
    return r;
}
WAITHOOK(DWORD, WaitForSingleObject, (HANDLE h, DWORD ms), (h, ms), (h, ms))
WAITHOOK(DWORD, WaitForSingleObjectEx, (HANDLE h, DWORD ms, BOOL a), (h, ms, a), (h, ms, a))
WAITHOOK(DWORD, WaitForMultipleObjects, (DWORD n, const HANDLE *h, BOOL w, DWORD ms), (n, h, w, ms), (n, h, w, ms))
WAITHOOK(DWORD, WaitForMultipleObjectsEx, (DWORD n, const HANDLE *h, BOOL w, DWORD ms, BOOL a), (n, h, w, ms, a), (n, h, w, ms, a))
WAITHOOK(DWORD, MsgWaitForMultipleObjects, (DWORD n, const HANDLE *h, BOOL w, DWORD ms, DWORD m), (n, h, w, ms, m), (n, h, w, ms, m))
WAITHOOK(DWORD, MsgWaitForMultipleObjectsEx, (DWORD n, const HANDLE *h, BOOL w, DWORD ms, DWORD m, DWORD f), (n, h, w, ms, m, f), (n, h, w, ms, m, f))

void pc64_waits_window(void) {   // link.c's per-1200-frame window: the top three wait sites
    if (!g_on) return;
    for (int i = 0; i < g_nsites; i++)   // sort by total time, descending (small n)
        for (int j = i + 1; j < g_nsites; j++)
            if (g_sites[j].sum_us > g_sites[i].sum_us) {
                typeof(g_sites[0]) t = g_sites[i]; g_sites[i] = g_sites[j]; g_sites[j] = t;
            }
    for (int i = 0; i < g_nsites && i < 3; i++)
        pc64_log("frame log: wait site %s game+%lx: total %.3f ms in %u calls (mean %.1f us)",
                 g_sites[i].name, (unsigned long)g_sites[i].rva, g_sites[i].sum_us / 1000.0, g_sites[i].n,
                 g_sites[i].n ? g_sites[i].sum_us / (double)g_sites[i].n : 0.0);
    g_nsites = 0;
}

void pc64_waits_install(HMODULE game) {
    const char *v = getenv("HK_PC64_FRAME_LOG");
    const char *k = getenv("HK_PC64_OWN_LIMITER");
    g_on = v && *v == '1';
    g_keep_limiter = k && *k == '1';
    g_game = game;
    g_main = GetCurrentThreadId();
#define HOOKW(name) do { void *o = pc64_import(game, "kernel32.dll", #name, (void *)name##_); \
        if (o) *(void **)&r_##name = o; else pc64_log("waits: the game doesn't import %s", #name); } while (0)
    if (!g_on) {   // the frame limiter's sleep only (a 64-bit PC IIDX); the other waits stay unhooked
        if (pc64_game_is_iidx() && !g_keep_limiter) HOOKW(SleepEx);
        return;
    }
    HOOKW(Sleep); HOOKW(SleepEx);
    HOOKW(WaitForSingleObject); HOOKW(WaitForSingleObjectEx);
    HOOKW(WaitForMultipleObjects); HOOKW(WaitForMultipleObjectsEx);
    HOOKW(MsgWaitForMultipleObjects); HOOKW(MsgWaitForMultipleObjectsEx);
    pc64_log("waits: HK_PC64_FRAME_LOG's wait hooks installed on the game's imports (main thread only)");
}
