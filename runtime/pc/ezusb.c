// SPDX-License-Identifier: GPL-3.0-or-later
// HIRAKU's ezusb.dll for the PC pop'n music games (PLAN.md section 30): what Konami's gives the game
// from the cabinet's FX2 board, given from hiraku-pc instead (runtime/pc/link.h). Every call
// Konami's DLL exports is here (the games look some up by name), C++-named through ezusb.def.
// The security plug and the board's EEPROM are answered as spice2x answers them for pop'n 19-21
// (games/popn/popn.cpp): nothing to check, all fine.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cabinet.h"

#define CALL __cdecl

// HK_PC_USB_LOG: the coin calls, for finding how a game counts its coins
static int usb_log(void) {
    static int on = -1;
    if (on < 0) on = getenv("HK_PC_USB_LOG") != NULL;
    return on;
}

int CALL usb_start(int a) {
    (void)a;
    hk_log("hiraku-pc: the game started its board");
    return 0;
}
int CALL usb_start_with_file(const char *f) { (void)f; return 0; }
int CALL usb_end(void) { return 0; }
int CALL usb_firm_result(void) { return 0; }
int CALL usb_check_alive(void) { return 1; }
int CALL usb_is_hi_speed(void) { return 1; }
int CALL usb_get_error(char *msg) {
    if (msg) msg[0] = 0;
    return 0;
}

int CALL usb_pad_read(unsigned long *bits) {
    *bits = hk_pad_now();
    return 0;
}
// The pad's samples since the last read: pop'n 15's buffer is 22 bytes (a count, then up to ten
// 16-bit samples, at 0x2246124), with a variable the game reads every frame right after it. Writing
// spice2x's 40 bytes for pop'n 19 on wiped that variable each frame and made held buttons repeat
// wildly (as bemanitools' shim once did; seen by crazyredmachine). Nothing is written: the game's
// zeroed buffer says "no samples", and it reads the pad from usbPadRead.
int CALL usb_pad_read_last(uint8_t *p) {
    // pop'n 16 on read their buttons in play from these: a counter that moves on with each sample
    // the board took (the game takes those since its last look, at most 6 a frame), then the ten
    // latest samples, newest first, buttons 1-9 in bits 0-8 (pop'n 18: 0x4fed10, 0x4fec90): made
    // at the board's rate from the pad's timed changes (press.c). pop'n 15 doesn't read them, and is
    // left as it was
    static int samples = -1;
    static uint8_t counter;
    static uint16_t last[10];
    if (samples < 0) {
        char exe[MAX_PATH];
        GetModuleFileNameA(NULL, exe, MAX_PATH);
        const char *base = strrchr(exe, '\\');
        samples = _stricmp(base ? base + 1 : exe, "popn15.exe") != 0;
    }
    if (!samples || !p) return 0;
    if (usb_log()) {
        static DWORD t0, n;
        n++;
        if (!t0) t0 = GetTickCount();
        if (GetTickCount() - t0 >= 10000) hk_log("usb: usbPadReadLast %lu times in 10 s", n), n = 0, t0 = GetTickCount();
    }
    counter = (uint8_t)(counter + hk_pad_samples(last));
    p[0] = counter, p[1] = 0;
    for (int i = 0; i < 10; i++) p[2 + 2 * i] = (uint8_t)last[i], p[3 + 2 * i] = (uint8_t)(last[i] >> 8);
    return 0;
}
int CALL usb_lamp(int bits) {
    hk_link()->lamps = (uint32_t)bits;
    return 0;
}
int CALL usb_set_ext_io(int bits) {
    hk_link()->ext_io = (uint32_t)bits;
    return 0;
}
int CALL usb_get_mute(void) { return 0; }
int CALL usb_mute(int a) { (void)a; return 0; }

// Coins: how many so far (the game counts the new ones itself)
int CALL usb_coin_get(int a) {
    const int n = (int)hk_coins_now();
    static int last = -1;
    if (usb_log() && n != last) hk_log("usb: usbCoinGet(%d) = %d", a, n), last = n;
    return n;
}
int CALL usb_coin_get2(void *p) {
    if (usb_log()) hk_log("usb: usbCoinGet2");
    (void)p;
    return 0;
}
int CALL usb_coin_mode(int a) {
    if (usb_log()) hk_log("usb: usbCoinMode(%d)", a);
    hk_set_coin_mode(a);
    return 0;
}
int CALL usb_coin_blocker(int a) {
    static int last = -2;
    if (usb_log() && a != last) hk_log("usb: usbCoinBlocker(%d)", a), last = a;
    return 0;
}
int CALL usb_coin_meter_up(int a) { (void)a; return 0; }
int CALL usb_coin_meter_down(int a) { (void)a; return 0; }
int CALL usb_coin_up(int a) { (void)a; return 0; }

// The watchdog: nothing bites
int CALL usb_wdt_start(int a) { (void)a; return 0; }
int CALL usb_wdt_start_done(void) { return 0; }
int CALL usb_wdt_reset(void) { return 0; }
int CALL usb_wdt_stop(void) { return 0; }

// The security plug, the EEPROM, the PCB's id: present, fine, empty
int CALL usb_security_init(void) { return 0; }
int CALL usb_security_init_done(void) { return 0; }
int CALL usb_security_search(void) { return 0; }
int CALL usb_security_search_done(void) { return 0; }
int CALL usb_security_select(int a) { (void)a; return 0; }
int CALL usb_security_select_done(void) { return 0; }
int CALL usb_security_get_id(void) { return 0; }
int CALL usb_security_get_id_done(uint8_t *p) { (void)p; return 0; }
int CALL usb_security_read(void) { return 0; }
int CALL usb_security_read_done(uint8_t *p) { (void)p; return 0; }
int CALL usb_security_write(uint8_t *p) { (void)p; return 0; }
int CALL usb_security_write_done(void) { return 0; }
int CALL usb_security_test(int a) { (void)a; return 0; }
// The plug the game asks about next (usbCheckSecurityNew), then its id (usbGetKEYID, into 12 bytes the
// game has zeroed). pop'n 15-18 check two at start-up: the game's own plug (0, 4, 6, 8), then
// e-AMUSEMENT's (3, 5, 7, 9), whose id is the cabinet's PCBID: the ten bytes the game prints as
// /ea3/id/pcbid, every call's srcid. HK_PC_PCBID (hiraku-pc's -p, 20 hex digits) is that id; without
// it the id stays as the game zeroed it (the PCBID these games had under HIRAKU before -p reached them)
static int g_plug;
int CALL usb_check_security_new(int a) {
    g_plug = a;
    return 0;
}
int CALL usb_check_security(uint8_t *p, int a, int b, int c) { (void)p, (void)a, (void)b, (void)c; return 0; }
int CALL usb_check_security_eep(int a) { (void)a; return 0; }
int CALL usb_get_security(int a, uint8_t *p) { (void)a, (void)p; return 0; }
int CALL usb_get_keyid(uint8_t *p, int a) {
    (void)a;
    const char *id = getenv("HK_PC_PCBID");
    if (p && (g_plug & 1) && id && strlen(id) == 20) {
        uint8_t b[10];
        for (int i = 0; i < 10; i++) {
            unsigned v = 0;
            if (sscanf(id + 2 * i, "%2x", &v) != 1) return 0;   // (not hex: left as it was)
            b[i] = (uint8_t)v;
        }
        memcpy(p, b, 10);
        if (usb_log()) hk_log("usb: usbGetKEYID: plug %d, the cabinet's PCBID %s", g_plug, id);
    }
    return 0;
}
int CALL usb_get_pcbid(uint8_t *p) { (void)p; return 0; }
int CALL usb_boot_security(uint8_t *p, int a, int b, int c) { (void)p, (void)a, (void)b, (void)c; return 0; }
int CALL usb_setup_security(uint8_t *p, int a, int b, int c) { (void)p, (void)a, (void)b, (void)c; return 0; }
int CALL usb_setup_security_new(int a, uint8_t *p, int b, int c, int d) { (void)a, (void)p, (void)b, (void)c, (void)d; return 0; }
int CALL usb_setup_eeprom(uint8_t *p, int a, int b, int c) { (void)p, (void)a, (void)b, (void)c; return 0; }
int CALL usb_setup_eeprom_new(int a, uint8_t *p, int b, int c, int d) { (void)a, (void)p, (void)b, (void)c, (void)d; return 0; }
int CALL usb_eep_read(void) { return 0; }
int CALL usb_eep_read_done(uint8_t *p) { (void)p; return 0; }
int CALL usb_eep_write(uint8_t *p) { (void)p; return 0; }
int CALL usb_eep_write_done(void) { return 0; }
int CALL usb_eep_test(void) { return 0; }
int CALL usb_read_eep_history(void *p) { (void)p; return 0; }
int CALL usb_factory_mode_init(uint8_t *p) { (void)p; return 0; }

BOOL WINAPI DllMain(HINSTANCE h, DWORD why, LPVOID r) {
    (void)h, (void)r;
    if (why == DLL_PROCESS_ATTACH) hk_cabinet_start();
    return TRUE;
}
