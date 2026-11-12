/* SPDX-License-Identifier: GPL-3.0-or-later */
/* hiraku-ezusb.exe: a 32-bit go-between for the pop'n music cabinet's I/O board on Windows
 * (link.h). It loads an ezusb.dll and drives it as a game does, after CrazyRedMachine's
 * PopnForwarder: usbStart(0), usbFirmResult() until 0, usbCoinMode(3), then usbPadRead and usbLamp
 * in a loop, trading the controls and the lamps with HIRAKU through shared memory.
 *
 * Only these calls are made (and usbCoinGet, usbEnd): none of the DLL's EEPROM, security or
 * watchdog ones. Built 32-bit by CMakeLists.txt (clang --target=i686-w64-mingw32). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "link.h"
#include "../../shell/wintimer.h"

typedef int(__cdecl *int_fn)(int);
typedef int(__cdecl *void_fn)(void);
typedef int(__cdecl *pad_fn)(unsigned long *);

static HMODULE dll;

/* The games' ezusb.dll is C++ (MSVC's names); pop'n music 15's and replacements built as C have
 * plain ones */
static FARPROC find(const char *mangled, const char *plain) {
    FARPROC f = GetProcAddress(dll, mangled);
    return f ? f : GetProcAddress(dll, plain);
}

static hk_ezusb_link *link;
static HANDLE parent;

static int stopping(void) { return link->quit || WaitForSingleObject(parent, 0) == WAIT_OBJECT_0; }

static int fail(int state, int result, const char *what) {
    fprintf(stderr, "popn-io (ezusb.dll): %s (%d)\n", what, result);
    link->result = result;
    link->state = state;
    return 1;
}

/* Calls f until it gives 0, at most `ms`, or (`final`) until it gives anything but 0x60, "busy";
 * the last value it gave */
static int until_zero(int (*call)(void *), void *f, int ms, int final) {
    DWORD t0 = timeGetTime();
    int r;
    while ((r = call(f)) != 0 && !stopping() && !(final && r != 0x60) && (int)(timeGetTime() - t0) < ms) Sleep(50);
    return r;
}
static int call_start(void *f) { return ((int_fn)f)(0); }
static int call_firm(void *f) { return ((void_fn)f)(); }
static int call_coin_mode(void *f) { return ((int_fn)f)(3); }

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "hiraku-ezusb MAPPING DLL PARENT_PID: started by HIRAKU (--popn-io on Windows)\n");
        return 2;
    }
    HANDLE map = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, argv[1]);
    link = map ? (hk_ezusb_link *)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof *link) : NULL;
    if (!link || link->magic != HK_EZUSB_MAGIC) {
        fprintf(stderr, "popn-io (ezusb.dll): no shared memory %s\n", argv[1]);
        return 2;
    }
    parent = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)strtoul(argv[3], NULL, 10));
    if (!parent) return fail(HK_EZUSB_FAILED, (int)GetLastError(), "HIRAKU's process isn't there");
    /* (no window, no sound: what Windows 11 may slow down and give 15.6 ms ticks, shell/wintimer.h) */
    hk_win_full_speed();
    hk_win_timer(5000);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    /* the DLL, with its folder first for what it loads in turn. LOAD_WITH_ALTERED_SEARCH_PATH
     * needs a path with backslashes: with the launcher's C:/... the DLL's own folder isn't searched,
     * so a DLL that brings others of its own beside it (a replacement's) wouldn't load */
    link->state = HK_EZUSB_LOADING;
    for (char *c = argv[2]; *c; c++)
        if (*c == '/') *c = '\\';
    dll = LoadLibraryExA(argv[2], NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!dll) {
        const DWORD e = GetLastError();
        /* (Windows says 126 both for a missing file and for a missing DLL it needs) */
        const int there = GetFileAttributesA(argv[2]) != INVALID_FILE_ATTRIBUTES;
        fprintf(stderr, "popn-io (ezusb.dll): %s doesn't load: %s\n", argv[2],
                !there ? "no such file (Settings > pop'n music I/O board: its ezusb.dll)"
                : e == ERROR_BAD_EXE_FORMAT ? "it isn't a 32-bit DLL (a 64-bit one can't be used)"
                : e == ERROR_MOD_NOT_FOUND ? "a DLL it needs is missing (put the files that came with it in its folder)"
                : "Windows refused it");
        return fail(HK_EZUSB_NO_DLL, (int)e, "the DLL doesn't load");
    }
    int_fn start = (int_fn)find("?usbStart@@YAHH@Z", "usbStart");
    void_fn firm = (void_fn)find("?usbFirmResult@@YAHXZ", "usbFirmResult");
    int_fn coin_mode = (int_fn)find("?usbCoinMode@@YAHH@Z", "usbCoinMode");
    pad_fn pad_read = (pad_fn)find("?usbPadRead@@YAHPAK@Z", "usbPadRead");
    int_fn lamp = (int_fn)find("?usbLamp@@YAHH@Z", "usbLamp");
    int_fn coin_get = (int_fn)find("?usbCoinGet@@YAHH@Z", "usbCoinGet");
    void_fn end = (void_fn)find("?usbEnd@@YAHXZ", "usbEnd");
    if (!start || !firm || !coin_mode || !pad_read || !lamp)
        return fail(HK_EZUSB_NO_EXPORT, 0, "the DLL lacks usbStart, usbFirmResult, usbCoinMode, usbPadRead or usbLamp");

    /* as a game starts the board: the real one takes its firmware here (about 17 s) */
    link->state = HK_EZUSB_STARTING;
    int r = until_zero(call_start, (void *)start, 10000, 0);
    if (r) return stopping() ? 0 : fail(HK_EZUSB_FAILED, r, "usbStart(0) failed: no board found through this DLL (Konami's "
                                                            "ezusb.dll needs Cypress's CyUSB driver installed for the board, "
                                                            "04b4:8613; a replacement DLL, its own board plugged in)");
    link->state = HK_EZUSB_FIRMWARE;
    r = until_zero(call_firm, (void *)firm, 90000, 1);   /* ezusb.dll tries four loads, 17 s each */
    if (r) return stopping() ? 0 : fail(HK_EZUSB_FAILED, r, "usbFirmResult() never gave 0 (0x60: still loading)");
    link->state = HK_EZUSB_COIN_MODE;
    r = until_zero(call_coin_mode, (void *)coin_mode, 10000, 0);
    if (r) return stopping() ? 0 : fail(HK_EZUSB_FAILED, r, "usbCoinMode(3) failed");
    link->state = HK_EZUSB_RUNNING;

    unsigned long pad;
    uint32_t sent = 0xffffffffu;
    DWORD last_lamp = 0;
    while (!stopping()) {
        pad = 0;
        pad_read(&pad);
        link->pad = (uint32_t)pad;
        if (coin_get) link->coins = (uint32_t)coin_get(0);
        link->reads++;
        const uint32_t lamps = link->lamps;
        const DWORD now = timeGetTime();
        if (lamps != sent || now - last_lamp > 500) {   /* on a change, and twice a second */
            lamp((int)lamps);
            sent = lamps;
            last_lamp = now;
        }
        /* to the timer's tick a millisecond after the last one: 1000 reads a second (Sleep(1) goes a
         * tick further: 2 ms with the timer at 1 ms, as it was, 504 reads a second on Windows 11) */
        hk_win_nap(8000);
    }
    lamp(0);
    if (end) end();
    return 0;
}
