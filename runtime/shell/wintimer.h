// SPDX-License-Identifier: GPL-3.0-or-later
// Windows' timer, for a process that keeps a game's time. C and C++ (the PC games' DLLs and
// loaders, the board's go-between, the host); nothing here on another system.
//
// A wait on Windows ends at a tick of the process's timer: every 15.6 ms, unless the process asks
// for finer (0.5 ms at best), and since Windows 10 2004 each process for itself: what another one
// asked doesn't count. Measured on Windows 11 (build 26300, 2026-11-12): a 0.2 ms nap lasts 15.6 ms
// in a process that asked for nothing and 0.5 ms once it asked for 0.5 ms; Sleep(1) lasts 15.6 ms,
// 2 ms at 1 ms and 1.5 ms at 0.5 ms.
//
// Windows 11 also says it needn't keep that for a process whose windows are all hidden, minimized
// or covered and which plays no sound, and that it may slow such a process down. A PC game under
// hiraku-pc is exactly that (HIRAKU's window shows its pictures and plays its sound), as is one of
// two cabinets on a PC whose window the other covers. hk_win_full_speed says not to. (The test PC
// kept a hidden or minimized process's 0.5 ms for a minute without it; nothing promises that.)
#pragma once
#ifdef _WIN32
#include <windows.h>

// Windows isn't to hold this process's timer or its speed back, whatever its windows show.
// 2: told; 1: its speed only (Windows 10: it holds no timer back); 0: neither (older: nothing to tell)
static inline int hk_win_full_speed(void) {
    typedef BOOL(WINAPI * set_t)(HANDLE, int, void *, DWORD);
    // PROCESS_POWER_THROTTLING_STATE: of EXECUTION_SPEED (1) and IGNORE_TIMER_RESOLUTION (4), neither
    struct {
        ULONG version, control, state;
    } s = {1, 0x4 | 0x1, 0};
    HMODULE k = GetModuleHandleA("kernel32.dll");
    set_t set = k ? (set_t)(void *)GetProcAddress(k, "SetProcessInformation") : NULL;   // (Windows 8 on)
    if (!set) return 0;
    if (set(GetCurrentProcess(), 4 /* ProcessPowerThrottling */, &s, sizeof s)) return 2;
    s.control = 0x1;
    return set(GetCurrentProcess(), 4, &s, sizeof s) ? 1 : 0;
}

// This process's timer asked for at `want` (100 ns units: 5000 is 0.5 ms, the finest there is),
// until it ends. What it now has, in the same units (0: Windows didn't say)
static inline unsigned long hk_win_timer(unsigned long want) {
    typedef LONG(NTAPI * set_t)(ULONG, BOOLEAN, PULONG);
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    set_t set = nt ? (set_t)(void *)GetProcAddress(nt, "NtSetTimerResolution") : NULL;
    ULONG now = 0;
    if (!set || set(want, TRUE, &now) < 0) return 0;
    return now;
}

// A nap of `t` (100 ns units), which ends at the timer's first tick after it: 2000 is 0.5 ms with
// the timer at 0.5 ms. (Sleep counts in milliseconds and adds a tick: Sleep(1) is 1.5 ms then.)
static inline void hk_win_nap(unsigned long t) {
    typedef LONG(NTAPI * delay_t)(BOOLEAN, PLARGE_INTEGER);
    static delay_t delay;
    static int looked;
    if (!looked) {
        HMODULE nt = GetModuleHandleA("ntdll.dll");
        delay = nt ? (delay_t)(void *)GetProcAddress(nt, "NtDelayExecution") : NULL;
        looked = 1;
    }
    LARGE_INTEGER d;
    d.QuadPart = -(LONGLONG)t;
    if (delay) delay(FALSE, &d);
    else Sleep((t + 9999) / 10000);
}
#endif
