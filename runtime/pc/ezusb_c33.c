// SPDX-License-Identifier: GPL-3.0-or-later
// HIRAKU's ezusb.dll for the C33 cabinet, Konami's ee'MALL 1st Avenue station (2003): what Konami's
// gives the program from the cabinet's two EZ-USB boards and its touch panel, given from hiraku-pc
// instead (runtime/pc/link.h).
//
// - The I/O board (Konami's ezusb.dll, 60 exports, plain C names, all cdecl): two buttons A and B,
//   a dial, Test, Service, the coin mech with its blocker and counters, two lamps, the security
//   plug, and the magnetic card slot (usbCard*, on the board itself: no H8 as on the C02 cabinet).
//   Coins: the program counts usbCoinGet(0)'s counter only (SSRIoUtil's IoUsbUpdateCoin, each frame);
//   its coin blocker is bit 4 of usbLamp's byte (SSRIo's IoUsbCoinBlocker: set, coins taken). A coin
//   put in while the blocker is shut comes back, as from the cabinet's mech: neither counted nor
//   seen on the switch. The program shuts it on its title and opens it only once a card is read
//   (its entrance and the mall, where coins become tickets).
//   Konami's DLL keeps the board's status packet in a state struct: the pad's three bytes active low
//   (usbPadRead turns them active high), the coin counters, the dial (+0x52), the photo sensor
//   (+0x53), the card slot's status (+0x56); outputs in a byte (+0x58: bit 0/1 the coin meter and
//   blocker, bits 2 and 3 the lamps). Calls answer 0 done, 0x60 busy (the "...Done" calls), -33 the
//   board not answering. The pad's bits, as the program reads them (eemall.exe's own test-mode
//   reads, TestModeSetting.dll's INPUT CHECK table): A 0, B 1, the coin mech 16, Test 18, Service 19.
// - The video board (Konami's ezusb_video.dll, 67 exports, all cdecl): an FPGA picture generator
//   with its own flash (36 blocks of 16 KiB), video memory (11 pages of 64x32 cells, 3 bytes each),
//   a palette (256 x 4 bytes) and attribute, scale and scroll registers, driving the cabinet's
//   small second display (text entry). Its calls are here too (usbV*, usbFpga*, usbFlash*, usbVram*,
//   ...); HIRAKU's ezusb_video.dll only forwards them to this DLL (ezusb_video_c33.def), so the
//   board's memory is one place. Its picture (c33video.h) goes beside the game's in the link's
//   pixels, twice its size: one 1280x480 frame, the cabinet's two screens side by side. Each
//   request answers 0 (taken), each Done 0 (done): the program's start-up (UsbVideo.dll) checks
//   only that, and that the flash's bytes add up to video.img's (else it writes video.img there).
// - The touch panel on COM1 (Konami's TouchPanel.dll, kept: 4800 baud, single-byte commands,
//   reports framed by 0xF2..0xFE and 0xFF): HIRAKU plays the panel on the program's own serial
//   calls (hooked in the game's modules), so the Wine prefix's COM ports don't matter. The reset
//   and the alive polls (0x33 then 0x32) are answered (size F4 W H FF on 0x37, status F7 01 11 FF on
//   0x32). Touches come from hiraku-pc (link.h: touch_*, in TouchPanel.dll's own space, 228 x 300),
//   reported as the panel does: FE X Y FF while touched, FD X Y FF when let go, in the panel's own
//   numbers (TouchPanel.dll turns them round: its X runs down the screen, its Y right to left).
//   Keys typed on the PC keyboard (link.h: type_ring) become touches on that key of the program's
//   touch keyboard, its places read from TouchPanel.dll's own key table as the program has it.
//
// Each call is logged the first times it's made (all of them with HK_PC_USB_LOG).
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <amstream.h>
#include <ddstream.h>
#include <mmsystem.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cabinet.h"
#include "c33.h"
#include "c33video.h"

#define CALL __cdecl
#define BUSY_NONE (-33)

static int usb_log(void) {
    static int on = -1;
    if (on < 0) on = getenv("HK_PC_USB_LOG") != NULL;
    return on;
}

// A call and what it gave back, in the log: its first three, or all with HK_PC_USB_LOG. Arguments
// are printed as six words whatever the call takes (cdecl: the caller's, read and left)
static int said(const char *name, int *calls, int a, int b, int c, int d, int r) {
    if (*calls < 3 || usb_log()) hk_log("usb: %s(%#x, %#x, %#x, %#x) = %#x%s", name, a, b, c, d, r, *calls == 2 && !usb_log() ? " (no more of these logged)" : "");
    (*calls)++;
    return r;
}
#define SAID(r) said(__func__, &n_, a, b, c, d, (r))
#define CALL6(name) int CALL name(int a, int b, int c, int d, int e, int f)
#define CALL4(name) int CALL name(int a, int b, int c, int d)
#define STUB(name, r) CALL4(name) { static int n_; return SAID(r); }
#define QUIET(name, r) CALL4(name) { static int n_; return usb_log() ? SAID(r) : (r); }

// ================================================================ the I/O board

// --- starting, ending, the watchdog: the board is there and fine
STUB(usbStart, 0)
STUB(usbStartWithFile, 0)
STUB(usbEnd, 0)
STUB(usbFirmResult, 0)   // 0x60 while the firmware loads; -33, -113, 0x80, 0xfe errors
STUB(usbGetError, 0)     // (char buf[128]): the last error's text; 0 none
STUB(usbFactoryModeInit, 0)
QUIET(usbCheckAlive, 1)  // 0: the board gone
STUB(usbWdtStart, 0)
STUB(usbWdtStartDone, 0)
STUB(usbWdtStop, 0)
QUIET(usbWdtReset, 0)
QUIET(usbWdtResetOld, 0)

// --- the security plug, the EEPROM, the PCB's id. The program's own check (SSRIo.dll
// IoUsbBootSecurity / IoUsbCheckSecurity) takes 0 from usbBootSecurity and usbCheckSecurity: the
// plug's answer for its game, GQC33 (AMVersion.dll's pass key), is "matches"
STUB(usbBootSecurity, 0)
STUB(usbCheckSecurity, 0)
STUB(usbSetupSecurityComplete, 0)
STUB(usbSecurityInit, 0)
STUB(usbSecurityInitDone, 0)
STUB(usbSecuritySelect, 0)
STUB(usbSecuritySelectDone, 0)
STUB(usbSecurityGetId, 0)
STUB(usbSecurityGetIdDone, 0)
STUB(usbSecurityRead, 0)
STUB(usbSecurityReadDone, 0)
STUB(usbSecurityWrite, 0)
STUB(usbSecurityWriteDone, 0)
STUB(usbSecurityTest, 0)
STUB(usbGetKEYID, 0)
STUB(usbEepRead, 0)
STUB(usbEepReadDone, 0)
STUB(usbEepWrite, 0)
STUB(usbEepWriteDone, 0)
STUB(usbEepTest, 0)
STUB(usbReadEepHistory, 0)
// The PCB id (10 bytes, SSRIo.dll 0x10002100; e-AMUSEMENT's srcid): hiraku-pc's (HK_PC_PCBID, 20
// hex digits), else left as the program has it
CALL4(usbGetPCBID) {
    static int n_;
    const char *id = getenv("HK_PC_PCBID");
    if (a && id && strlen(id) == 20)
        for (int i = 0; i < 10; i++) {
            unsigned v;
            if (sscanf(id + 2 * i, "%2x", &v) == 1) ((uint8_t *)(intptr_t)a)[i] = (uint8_t)v;
        }
    return SAID(0);
}

// --- the controls
#define C33_PAD_BITS (HK_PC_C33_A | HK_PC_C33_B | HK_PC_C33_COIN | HK_PC_C33_TEST | HK_PC_C33_SERVICE)

// usbLamp's byte as the program last gave it: bits 2 and 3 the lamps, bit 4 the coin blocker open
// (SSRIo.dll IoUsbCoinBlocker 0x100017b0: shut, the bit clear; the program starts with it shut)
static volatile uint32_t g_outputs;
#define C33_BLOCKER_OPEN 0x10u
static uint32_t g_coins_taken;   // usbCoinGet(0)'s counter: the coins the mech took

// The coin mech: each coin hiraku-pc gives is taken (counted, and a pulse on its switch, on 60 ms,
// off 60 ms) if the blocker is open, else it comes back (neither)
static CRITICAL_SECTION g_coin_lock;
static uint32_t coin_pulse(void) {
    static uint32_t given;
    static int started, on;
    static DWORD since;
    EnterCriticalSection(&g_coin_lock);
    const uint32_t coins = hk_coins_now();
    if (!started) given = coins, started = 1;
    const DWORD now = GetTickCount();
    uint32_t r = 0;
    if (since && now - since < 60) {
        r = on ? HK_PC_C33_COIN : 0;
    } else if (on) {
        on = 0, since = now;
    } else if (given != coins) {
        given++;
        if (g_outputs & C33_BLOCKER_OPEN) {
            on = 1, since = now, g_coins_taken++;
            r = HK_PC_C33_COIN;
        } else {
            static int n;
            if (n++ < 10) hk_log("usb: a coin came back: the program has its coin blocker shut");
            since = 0;
        }
    } else {
        since = 0;
    }
    LeaveCriticalSection(&g_coin_lock);
    return r;
}

static void typing_update(void);

// TEST SWITCH, for scripted runs only (off unless HK_PC_C33_STEPS is set): presses keyed on the
// program's own scene number, where a frame count can't be (a mini-game's length varies).
//   HK_PC_C33_STEPS = "S[#K]+MS:ACTION;..."  ACTION: A, B (held 150 ms), d+N / d-N (the dial turned,
//   8 units a read)
// each step runs once, MS milliseconds after the program's K-th entry (default 1st) into scene S.
// HK_PC_C33_SCENE_LOG=1 logs each scene change. The scene number is read from the program's scene
// manager (eemall.exe: its pointer at 0x4e2940, the current scene at +0xc), guarded.
#define C33_SCENE_MGR 0x4e2940u
static uint32_t g_step_pad, g_step_pad_until;
static int g_step_dial, g_step_dial_to;   // the steps' dial turn so far, and where it goes (8 units a read)
static int scene_now(void) {
    const uint32_t *mgr = (const uint32_t *)(uintptr_t)C33_SCENE_MGR;
    if (IsBadReadPtr(mgr, 4) || !*mgr || IsBadReadPtr((const void *)(uintptr_t)(*mgr + 0xc), 4)) return -1;
    return *(const int *)(uintptr_t)(*mgr + 0xc);
}
static void test_steps(void) {
    static int on = -1, logged, last = -1, entries[64];
    static DWORD since;
    static char done[64];
    static const char *steps;
    if (on < 0) steps = getenv("HK_PC_C33_STEPS"), logged = getenv("HK_PC_C33_SCENE_LOG") != NULL, on = (steps && *steps) || logged;
    if (!on) return;
    const DWORD now = GetTickCount();
    const int sc = scene_now();
    if (sc != last) {
        if (logged) hk_log("c33 test: scene %d -> %d", last, sc);
        last = sc, since = now;
        if (sc >= 0 && sc < 64) entries[sc]++;
    }
    if (g_step_pad && now - g_step_pad_until < 0x80000000u) g_step_pad = 0;
    int i = 0;
    for (const char *p = steps; p && *p && i < 64; i++) {
        int s = -1, k = 1, ms = 0, used = 0;
        char act[16] = "";
        if (sscanf(p, "%d%n", &s, &used) == 1) {
            p += used;
            if (*p == '#' && sscanf(p + 1, "%d%n", &k, &used) == 1) p += 1 + used;
            if (*p == '+' && sscanf(p + 1, "%d%n", &ms, &used) == 1) p += 1 + used;
            if (*p == ':' && sscanf(p + 1, "%15[^;]%n", act, &used) == 1) p += 1 + used;
        }
        if (!done[i] && s == sc && entries[sc & 63] == k && now - since >= (DWORD)ms && act[0]) {
            done[i] = 1;
            if (act[0] == 'A') g_step_pad = HK_PC_C33_A, g_step_pad_until = now + 150;
            else if (act[0] == 'B') g_step_pad = HK_PC_C33_B, g_step_pad_until = now + 150;
            else if (act[0] == 'd') g_step_dial_to += atoi(act + 1);
            hk_log("c33 test: step %d (scene %d, entry %d, +%d ms): %s", i, s, k, ms, act);
        }
        const char *semi = strchr(p, ';');
        if (!semi) break;
        p = semi + 1;
    }
}

// (uint32_t *out): bits 0-23 active high
CALL4(usbPadRead) {
    static int n_;
    test_steps();
    if (a) *(uint32_t *)(intptr_t)a = (hk_pad_now() & C33_PAD_BITS) | coin_pulse() | g_step_pad;
    typing_update();
    return usb_log() ? SAID(0) : 0;
}
// The dial, 0-255 (the link's player 1 turntable)
CALL4(usbGetTurnTable) {
    static int n_;
    if (g_step_dial != g_step_dial_to) g_step_dial += g_step_dial_to > g_step_dial ? (g_step_dial_to - g_step_dial > 8 ? 8 : g_step_dial_to - g_step_dial)
                                                                    : (g_step_dial - g_step_dial_to > 8 ? -8 : g_step_dial_to - g_step_dial);
    const int v = (hk_link()->turntable[0] + g_step_dial) & 255;   // (+ the test steps' turns, 8 units a read)
    hk_turntable_seen(0, (uint8_t)hk_link()->turntable[0]);
    return usb_log() ? SAID(v) : v;
}
QUIET(usbGetPhotoSenser, 0)   // a byte of the status packet (+0x53): the card slot's sensor (the program never asks)

// The coins: usbCoinGet(0) the mech's count (16 bits), (1) the second counter, else -33
static int g_coin_mode;
CALL4(usbCoinGet) {
    static int n_;
    static int last = -1;
    coin_pulse();
    const int coins = (int)(g_coins_taken & 0xffff);
    const int r = a == 0 ? coins : a == 1 ? 0 : -33;
    if (r != last || usb_log()) said(__func__, &n_, a, b, c, d, r), last = r;
    return r;
}
// (int out[5]): the two counters, the blocker's sense (SSRIo.dll 0x100018f9: after usbCoinMode(1)
// it wants out[2] set, after usbCoinMode(2) clear), a second sense, the mech's errors
CALL4(usbCoinGet2) {
    static int n_;
    if (a) {
        int *o = (int *)(intptr_t)a;
        coin_pulse();
        o[0] = (int)(g_coins_taken & 0xffff), o[1] = 0, o[2] = g_coin_mode & 1, o[3] = 0, o[4] = 0;
    }
    return usb_log() ? SAID(0) : 0;
}
CALL4(usbCoinMode) {
    static int n_;
    g_coin_mode = a;
    return SAID(0);
}
STUB(usbCoinUp, 0)
STUB(usbCoinMeterUp, 0)
STUB(usbCoinMeterDown, 0)
CALL4(usbCoinBlocker) {
    static int n_;
    hk_link()->lamps = (hk_link()->lamps & ~2u) | (a ? 2u : 0);
    return SAID(0);
}
STUB(usbMute, 0)
QUIET(usbGetMute, 0)

// The outputs' byte: bits 2 and 3 the two lamps (SSRIo.dll IoUsbLamp), 0 and 1 the meter and blocker
CALL4(usbLamp) {
    static int n_;
    if ((a ^ (int)g_outputs) & C33_BLOCKER_OPEN) hk_log("usb: the coin blocker %s", a & C33_BLOCKER_OPEN ? "open" : "shut");
    g_outputs = (uint32_t)a;
    hk_link()->lamps = (uint32_t)(a & 0xfc) | (hk_link()->lamps & 3u);
    return usb_log() ? SAID(0) : 0;
}

// --- the magnetic card slot (on the board itself). Konami's DLL keeps the slot's state in a byte of
// the status packet (+0x56, usbCardGetStatus); its other calls start a command the board carries out
// (open 3/6, close 3/7, eject 3/4, read 0xe/2, back read 0xe/8, write 0xf/3, back write 0xf/9) and
// usbCard{Read,Write}Done tell its end: 0x60 busy, 0 done (a read gives the 64 bytes read), -33
// failed, and 0 with nothing given when no command is under way. The program's own steps (SSRIo.dll
// IoUsbCardSimple*, 0x100028f0-0x100037a0) wait on the status byte:
//   - its read (ReadStep): eject what may be there (EjectStep), open the slot (OpenStep: after the
//     close the byte must be 0, 1, 4 or 9; after usbCardOpen it waits while it is 1 or 8, takes 3, 6
//     or 7 as "a card is in", 9 as "open again"), then usbCardRead (the card's first 64 bytes: its
//     header 0-3 and the header's CRC-8 at 4, checked) and usbCardBackRead (the 64 from byte 5: the
//     58 bytes of data and their CRC-16, checked), the card left inside;
//   - its eject (EjectStep): usbCardEject when the byte says a card is in, then it waits until the
//     card is taken (9; 2 while the card is still in the slot's mouth).
//   IsInCensor is "not 9", IsInsideMore "10 or 11". usbCardInside and usbGetPhotoSenser aren't used.
// HIRAKU's slot: 9 empty; usbCardOpen makes it 1, or 3 at once when a card is held to it (hiraku-pc's
// "Card": the link's card_seq moves on, cards/NUMBER.bin the card, made by hiraku-pc); a card held
// to the closed slot waits there for the next open. usbCardEject puts the card in the mouth (2) and
// the player takes it 1.5 s later (9). The card is never written: the program writes cards only in
// test mode's READ AND WRITE CHECK, and those writes are kept here, in memory, for its read-back.
#define SLOT_EMPTY 9
#define SLOT_OPEN 1
#define SLOT_IN 3
#define SLOT_MOUTH 2
static uint8_t g_card[128];
static int g_slot = SLOT_EMPTY, g_slot_open, g_card_held, g_card_written;
static int g_card_cmd;   // 0 none, 1 read, 2 back read, 3 write: what the next Done answers for
static DWORD g_mouth_since;
static CRITICAL_SECTION g_card_lock;

// The card held to the slot: its file (hk_magnetic_card_file: cards/NUMBER.bin), else a card never
// played (pop'n 9's new card), which the program turns away itself
static void card_load(void) {
    memset(g_card, 0, sizeof g_card);
    const char *path = hk_magnetic_card_file(0);
    HANDLE h = path ? CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL) : INVALID_HANDLE_VALUE;
    DWORD got = 0;
    if (h != INVALID_HANDLE_VALUE) {
        if (!ReadFile(h, g_card, sizeof g_card, &got, NULL)) got = 0;
        CloseHandle(h);
    }
    if (got != sizeof g_card) {
        static const uint8_t fresh[] = {0x28, 0x23, 0x41, 0x01, 0x09};   // header and its CRC-8
        memset(g_card, 0, sizeof g_card);
        memcpy(g_card, fresh, sizeof fresh);
        g_card[0x3f] = 0x98, g_card[0x40] = 0x91;   // the blank data's CRC-16
        hk_log("usb: card: no card file (%s): a card never played", path ? path : "none");
    } else {
        hk_log("usb: card: %s, header %02x %02x %02x %02x", path, g_card[0], g_card[1], g_card[2], g_card[3]);
    }
    g_card_written = 0;
}

// The slot's state at each call: a new card held to it, a card in the mouth taken
static void slot_update(void) {
    static uint32_t seq;
    static int started;
    const uint32_t now_seq = hk_card_seq();
    if (!started) seq = now_seq, started = 1;
    if (now_seq != seq) {
        seq = now_seq;
        if (g_slot == SLOT_IN || g_slot == SLOT_MOUTH) {
            hk_log("usb: card: a card is in the slot already: take it first");
        } else {
            card_load();
            g_card_held = 1;
            hk_log("usb: card: held to the slot%s", g_slot_open ? ": it goes in" : " (the slot is shut: it waits)");
        }
    }
    if (g_card_held && g_slot_open && g_slot != SLOT_IN) g_slot = SLOT_IN, g_card_held = 0, hk_log("usb: card: in the slot");
    if (g_slot == SLOT_MOUTH && GetTickCount() - g_mouth_since > 1500) g_slot = SLOT_EMPTY, hk_log("usb: card: taken back by the player");
}

#define SLOT_CALL(name) CALL4(name) { static int n_; EnterCriticalSection(&g_card_lock); slot_update();
#define SLOT_END(r) { const int r_ = (r); LeaveCriticalSection(&g_card_lock); return SAID(r_); } }
SLOT_CALL(usbCardOpen) g_slot_open = 1; if (g_slot == SLOT_EMPTY) g_slot = SLOT_OPEN; slot_update(); SLOT_END(0)
SLOT_CALL(usbCardClose) g_slot_open = 0; if (g_slot == SLOT_OPEN) g_slot = SLOT_EMPTY; SLOT_END(0)
SLOT_CALL(usbCardEject)
    g_slot_open = 0;
    if (g_slot == SLOT_IN) g_slot = SLOT_MOUTH, g_mouth_since = GetTickCount(), hk_log("usb: card: out of the slot");
    else if (g_slot == SLOT_OPEN) g_slot = SLOT_EMPTY;
SLOT_END(0)
STUB(usbCardInside, 0)
STUB(usbCardFormat, 0)
STUB(usbCardWaitEnd, 0)
CALL4(usbCardGetStatus) {
    static int n_, last = -1;
    EnterCriticalSection(&g_card_lock);
    slot_update();
    const int r = g_slot;
    LeaveCriticalSection(&g_card_lock);
    if (r != last || usb_log()) said(__func__, &n_, a, b, c, d, r), last = r;
    return r;
}
SLOT_CALL(usbCardRead) g_card_cmd = g_slot == SLOT_IN ? 1 : 4; SLOT_END(0)       // the card's first 64 bytes
SLOT_CALL(usbCardBackRead) g_card_cmd = g_slot == SLOT_IN ? 2 : 4; SLOT_END(0)   // the 64 from byte 5
// (uint8_t out[64])
SLOT_CALL(usbCardReadDone)
    int r = 0;
    if (g_card_cmd == 1 || g_card_cmd == 2) {
        if (a) memcpy((void *)(intptr_t)a, g_card + (g_card_cmd == 2 ? 5 : 0), 64);
    } else if (g_card_cmd == 4) {
        r = BUSY_NONE;
    }
    g_card_cmd = 0;
SLOT_END(r)
// (const uint8_t in[62]): test mode's check writes; kept for its read-back, never in the card's file
SLOT_CALL(usbCardWrite)
    if (a && g_slot == SLOT_IN) memcpy(g_card, (const void *)(intptr_t)a, 62), g_card_written = 1;
    g_card_cmd = g_slot == SLOT_IN ? 3 : 4;
SLOT_END(0)
SLOT_CALL(usbCardBackWrite)
    if (a && g_slot == SLOT_IN) memcpy(g_card + 5, (const void *)(intptr_t)a, 62), g_card_written = 1;
    g_card_cmd = g_slot == SLOT_IN ? 3 : 4;
SLOT_END(0)
SLOT_CALL(usbCardWriteDone) const int r = g_card_cmd == 4 ? BUSY_NONE : 0; g_card_cmd = 0; SLOT_END(r)

// ================================================================ the video board

// Its memory, as the program writes it (Konami's DLL: block, page and rect limits, sizes)
#define FLASH_BLOCK 0x4000
#define FLASH_BLOCKS 36
#define VRAM_PAGE 0x1800   // 64 x 32 cells, 3 bytes each
#define VRAM_PAGES 11
static uint8_t g_flash[FLASH_BLOCK * FLASH_BLOCKS];
static uint8_t g_vram[VRAM_PAGE * VRAM_PAGES];
static uint8_t g_palette[0x400];
static uint8_t g_attr[32], g_scale[20], g_scroll[20], g_display[24], g_head[10];
static int g_control;      // bit 0 the shown VRAM bank, 1 the palette bank, 2 the display off
static int g_flash_block = -1, g_rect[5] = {-1};
static int g_flash_loaded;
static int g_vwdt;   // the watchdog's seconds (0: stopped)
static void video_dump(const char *why);

// The flash is the board's own, kept in the save folder (HK_PC_VIDEO_FLASH) as a real board keeps it:
// blank (0xff) the first time, then what the program wrote (video.img's pictures)
static void flash_load(void) {
    if (g_flash_loaded) return;
    g_flash_loaded = 1;
    memset(g_flash, 0xff, sizeof g_flash);
    const char *f = getenv("HK_PC_VIDEO_FLASH");
    FILE *fh = f && *f ? fopen(f, "rb") : NULL;
    if (fh) {
        if (fread(g_flash, 1, sizeof g_flash, fh) != sizeof g_flash) memset(g_flash, 0xff, sizeof g_flash);
        fclose(fh);
    }
}
static void flash_save(void) {
    const char *f = getenv("HK_PC_VIDEO_FLASH");
    if (f && *f) hk_write_file(f, g_flash, sizeof g_flash);
}

// --- starting, the firmware, the watchdog
static DWORD WINAPI movie_check(LPVOID arg);
CALL4(usbVStart) {
    static int n_;
    if (!n_ && getenv("HK_PC_C33_MOVIE_CHECK")) CloseHandle(CreateThread(NULL, 0, movie_check, NULL, 0, NULL));
    return SAID(0);
}
STUB(usbVStartWithFile, 0)
STUB(usbVEnd, 0)
STUB(usbVFirmResult, 0)
STUB(usbVGetError, 0)
QUIET(usbVCheckAlive, 1)
CALL4(usbVWdtStart) {
    static int n_;
    if (a < 0 || a > 60) return SAID(-33);
    g_vwdt = a;
    return SAID(0);
}
STUB(usbVWdtStartDone, 0)
CALL4(usbVWdtStop) {
    static int n_;
    if (g_vwdt) hk_log("usb: the video board's watchdog (%d s) stopped", g_vwdt);
    g_vwdt = 0;
    return SAID(0);
}
QUIET(usbVWdtReset, 0)
STUB(usbDevelop, 0)
CALL4(usbVPadRead) {
    static int n_;
    if (a) *(int *)(intptr_t)a = 0;
    return SAID(0);
}
CALL4(usbSioRead) {   // (port, char *out): nothing received
    static int n_;
    return SAID(0);
}
STUB(usbSioWrite, 0)

// --- the FPGA: its configuration (usbFpgaWrite("...\usb-v3.hex"), the DLL reads the file) and check
STUB(usbFpgaInit, 0)
STUB(usbFpgaInitDone, 0)
STUB(usbFpgaWrite, 0)
STUB(usbFpgaWriteDone, 0)
STUB(usbFpgaCheck, 0)
STUB(usbFpgaCheckDone, 0)
STUB(usbI2cChipSel, 0)
STUB(usbI2cWrite, 0)
STUB(usbI2cSet, 0)
STUB(usbI2cWriteDone, 0)
STUB(usbI2cPercent, 100)
STUB(usbSramCheck, 0)
STUB(usbSramCheckDone, 0)

// --- the flash: 36 blocks of 16 KiB
CALL4(usbFlashReset) { static int n_; flash_load(); return SAID(0); }
STUB(usbFlashResetDone, 0)
STUB(usbFlashEraseDone, 0)
STUB(usbFlashSelect, 0)
STUB(usbFlashSign, 0)
CALL4(usbFlashSignDone) {   // (uint16 *out): the chip's signature
    static int n_;
    if (a) *(uint16_t *)(intptr_t)a = 0;
    return SAID(0);
}
CALL4(usbFlashRead) {   // (block)
    static int n_;
    flash_load();
    g_flash_block = (a & 0xffff) < FLASH_BLOCKS ? (a & 0xffff) : -1;
    return usb_log() || n_ < 3 ? SAID(0) : 0;
}
CALL4(usbFlashReadDone) {   // (buf): the block's 16 KiB
    static int n_;
    if (a) {
        if (g_flash_block >= 0) memcpy((void *)(intptr_t)a, g_flash + g_flash_block * FLASH_BLOCK, FLASH_BLOCK);
        else memset((void *)(intptr_t)a, 0xff, FLASH_BLOCK);
    }
    return usb_log() || n_ < 3 ? SAID(0) : (n_++, 0);
}
CALL4(usbFlashWrite) {   // (block, buf)
    static int n_;
    flash_load();
    const int k = a & 0xffff;
    if (k < FLASH_BLOCKS && b) {
        memcpy(g_flash + k * FLASH_BLOCK, (const void *)(intptr_t)b, FLASH_BLOCK);
        if (k == FLASH_BLOCKS - 1) flash_save();
    }
    return usb_log() || n_ < 3 ? SAID(0) : (n_++, 0);
}
CALL4(usbFlashWriteDone) {
    static int n_;
    flash_save();
    return usb_log() || n_ < 3 ? SAID(0) : (n_++, 0);
}
CALL4(usbFlashErase) {   // (block)
    static int n_;
    flash_load();
    if (a >= 0 && a < FLASH_BLOCKS) memset(g_flash + a * FLASH_BLOCK, 0xff, FLASH_BLOCK);
    return SAID(0);
}
CALL4(usbFlashEraseAll) {
    static int n_;
    flash_load();
    memset(g_flash, 0xff, sizeof g_flash);
    return SAID(0);
}
STUB(usbFlashEraseAllDone, 0)
STUB(usbFlashCheck, 0)
STUB(usbFlashCheckGetBank, 0)
STUB(usbFlashCheckAbort, 0)

// --- the picture: video memory, palette, registers (kept for drawing later)
CALL4(usbPaletteSet) {   // (256 x 4 bytes; kept as the program gave them)
    static int n_;
    if (a) memcpy(g_palette, (const void *)(intptr_t)a, sizeof g_palette);
    video_dump("palette");
    return SAID(0);
}
STUB(usbPaletteSetDone, 0)
CALL4(usbVramSet) {   // (page, count, data): count pages of 0x1800 bytes from page
    static int n_;
    if (a < 0 || a >= VRAM_PAGES || !c) return SAID(0);
    if (b < 0 || b > 5) return SAID(1);
    const int n = a + b > VRAM_PAGES ? VRAM_PAGES - a : b;
    memcpy(g_vram + a * VRAM_PAGE, (const void *)(intptr_t)c, (size_t)n * VRAM_PAGE);
    return usb_log() || n_ < 3 ? SAID(0) : (n_++, 0);
}
STUB(usbVramSetDone, 0)
CALL6(usbVramSetRect) {   // (page, x, y, w, h, data): w x h cells of 3 bytes at (x, y)
    static int n_;
    if (a < 0 || a >= VRAM_PAGES || b < 0 || b >= 64 || c < 0 || c >= 32) return SAID(0);
    if (d < 0 || d > 64 || e < 0 || e > 32) return SAID(1);
    const uint8_t *src = (const uint8_t *)(intptr_t)f;
    if (src)
        for (int y = 0; y < e; y++)
            for (int x = 0; x < d; x++) {
                const int cx = b + x, cy = c + y;
                if (cx < 64 && cy < 32) memcpy(g_vram + a * VRAM_PAGE + (cy * 64 + cx) * 3, src + (y * d + x) * 3, 3);
            }
    return usb_log() || n_ < 3 ? SAID(0) : (n_++, 0);
}
STUB(usbVramSetRectDone, 0)
CALL4(usbVramRead) {
    static int n_;
    g_rect[0] = a >= 0 && a < VRAM_PAGES ? a : -1;
    return SAID(0);
}
CALL4(usbVramReadDone) {   // (buf): the page's 0x1800 bytes
    static int n_;
    if (a && g_rect[0] >= 0) memcpy((void *)(intptr_t)a, g_vram + g_rect[0] * VRAM_PAGE, VRAM_PAGE);
    return SAID(0);
}
CALL6(usbVramReadRect) {
    static int n_;
    (void)f;
    if (a < 0 || a >= VRAM_PAGES || b < 0 || b >= 64 || c < 0 || c >= 32) return SAID(0);
    if (d < 0 || d > 64 || e < 0 || e > 32) return SAID(1);
    g_rect[0] = a, g_rect[1] = b, g_rect[2] = c, g_rect[3] = d, g_rect[4] = e;
    return SAID(0);
}
CALL4(usbVramReadRectDone) {   // (buf): w x h cells of 3 bytes
    static int n_;
    uint8_t *dst = (uint8_t *)(intptr_t)a;
    if (dst && g_rect[0] >= 0)
        for (int y = 0; y < g_rect[4]; y++)
            for (int x = 0; x < g_rect[3]; x++) {
                const int cx = g_rect[1] + x, cy = g_rect[2] + y;
                if (cx < 64 && cy < 32) memcpy(dst + (y * g_rect[3] + x) * 3, g_vram + g_rect[0] * VRAM_PAGE + (cy * 64 + cx) * 3, 3);
            }
    return SAID(0);
}
#define REGISTER(name, reg)                                              \
    CALL4(name) {                                                         \
        static int n_;                                                    \
        if (a) memcpy(reg, (const void *)(intptr_t)a, sizeof reg);        \
        return SAID(0);                                                   \
    }
CALL4(usbAttrSet) {   // (32 bytes: byte n 0x0f hides layer n)
    static int n_;
    if (a) memcpy(g_attr, (const void *)(intptr_t)a, sizeof g_attr);
    video_dump("layers");
    return SAID(0);
}
REGISTER(usbScaleSet, g_scale)
REGISTER(usbScrollSet, g_scroll)
REGISTER(usbDisplaySet, g_display)
REGISTER(usbHeadSet, g_head)
STUB(usbAttrSetDone, 0)
STUB(usbScaleSetDone, 0)
STUB(usbScrollSetDone, 0)
STUB(usbDisplaySetDone, 0)
STUB(usbHeadSetDone, 0)
CALL4(usbVramBank) {
    static int n_;
    g_control = (g_control & ~1) | (a ? 1 : 0);
    video_dump("bank");
    return usb_log() || n_ < 3 ? SAID(0) : (n_++, 0);
}
CALL4(usbPaletteBank) {
    static int n_;
    g_control = (g_control & ~2) | (a ? 2 : 0);
    return SAID(0);
}
CALL4(usbDisplayOnOff) {
    static int n_;
    g_control = (g_control & ~4) | (a ? 4 : 0);
    return SAID(0);
}

// --- the picture shown (c33.h): the game's 640x480 on the left, and on the right the touch panel as
// the player sees it, the small display's picture in it at 1:1 and the keys under it
static uint32_t g_small[C33_SCREEN_W * C33_SCREEN_H];
static void video_state(struct c33_video *v) {
    v->flash = g_flash, v->vram = g_vram, v->palette = g_palette, v->attr = g_attr;
    v->control = (unsigned)g_control | (g_flash_loaded ? 0 : C33_BLANK);
}
struct c33_key;
static const struct c33_key *key_table(int *n);
static void key_places(struct c33_key_place *out, int *n);
static void touch_where(int *x, int *y);
static void video_dump(const char *why);
static void c33_picture(uint32_t *px, unsigned *w, unsigned *h) {
    static unsigned pictures;
    if (++pictures % 600 == 0) video_dump("ten seconds on");
    if (*w != 640 || *h != 480) return;
    const int W = HK_PC_C33_FRAME_W, H = HK_PC_C33_FRAME_H, top = HK_PC_C33_MAIN_Y;
    for (int y = 479; y >= 0; y--) {
        memmove(px + (size_t)(y + top) * W, px + (size_t)y * 640, 640 * 4);
        memset(px + (size_t)(y + top) * W + 640, 0, 640 * 4);
    }
    memset(px, 0, (size_t)top * W * 4);
    memset(px + (size_t)(top + 480) * W, 0, (size_t)(H - top - 480) * W * 4);
    struct c33_video v;
    video_state(&v);
    c33_video_draw(&v, g_small, C33_SCREEN_W);
    static struct c33_key_place keys[64];
    int n = 0, lit = -1;
    key_places(keys, &n);
    int tx, ty;
    touch_where(&tx, &ty);
    for (int i = 0; tx >= 0 && i < n; i++)
        if (tx >= keys[i].x && tx <= keys[i].x + keys[i].w && ty >= keys[i].y && ty <= keys[i].y + keys[i].h) lit = i;
    const int pw = HK_PC_C33_CTR_W * HK_PC_C33_PX / HK_PC_C33_CTR;
    c33_panel_draw(px + HK_PC_C33_PANEL_X, W, pw, H, HK_PC_C33_PX, HK_PC_C33_CTR, g_small, HK_PC_C33_SCREEN_X, HK_PC_C33_SCREEN_Y,
                   keys, n, lit, tx, ty);
    *w = (unsigned)W, *h = (unsigned)H;
}

// For bringing the board up: HK_PC_C33_VIDEO_DUMP=FOLDER, the board's state (its memory, palette,
// registers) written there each time the program shows another picture (a bank turned, the layers
// changed), at most 300 times: "vNNNN.bin" = the pages, the palette, attr, scale, scroll, control
static void video_dump(const char *why) {
    static int n = -1;
    static const char *dir;
    if (n < 0) dir = getenv("HK_PC_C33_VIDEO_DUMP"), n = 0;
    if (!dir || !*dir || n >= 300) return;
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\v%04d.bin", dir, n);
    static uint8_t buf[sizeof g_vram + sizeof g_palette + sizeof g_attr + sizeof g_scale + sizeof g_scroll + 4];
    uint8_t *p = buf;
    memcpy(p, g_vram, sizeof g_vram), p += sizeof g_vram;
    memcpy(p, g_palette, sizeof g_palette), p += sizeof g_palette;
    memcpy(p, g_attr, sizeof g_attr), p += sizeof g_attr;
    memcpy(p, g_scale, sizeof g_scale), p += sizeof g_scale;
    memcpy(p, g_scroll, sizeof g_scroll), p += sizeof g_scroll;
    memcpy(p, &g_control, 4);
    if (hk_write_file(path, buf, sizeof buf)) hk_log("usb: the video board's state (%s) in v%04d.bin", why, n);
    n++;
}

// ================================================================ the touch panel on COM1

// The panel's answers waiting for the program's reads
static HANDLE g_com;   // the port as the program has it (an event of HIRAKU's: a real, closable handle)
static CRITICAL_SECTION g_com_lock;
static uint8_t g_rx[0x800];
static int g_nrx;
#define TOUCH_W 0xf0   // the panel's size, as it reports it (F4 W H FF): any 1-254
#define TOUCH_H 0xf0

static void com_queue(const uint8_t *p, int n) {
    EnterCriticalSection(&g_com_lock);
    if (g_nrx + n <= (int)sizeof g_rx) memcpy(g_rx + g_nrx, p, (size_t)n), g_nrx += n;
    LeaveCriticalSection(&g_com_lock);
}

// A byte from the program: 0x37 asks the panel's size, 0x32 its status (both in the reset string;
// 0x33 then 0x32 the alive poll every few seconds)
static void com_byte(uint8_t b) {
    static int n;
    if (n++ < 40 || usb_log()) hk_log("touch: the program sent %02x", b);
    if (b == 0x37) {
        static const uint8_t size[4] = {0xf4, TOUCH_W, TOUCH_H, 0xff};
        com_queue(size, 4);
    } else if (b == 0x32) {
        static const uint8_t alive[4] = {0xf7, 0x01, 0x11, 0xff};
        com_queue(alive, 4);
    }
}

// --- touches. TouchPanel.dll reads the panel's two numbers (FE X Y FF) as places of its own, in a
// space 228 across and 300 down (GetTouchPanelCtrPosX(228) = 228 - Y * 228 / H, ...PosY(300) =
// X * 300 / W, W and H the size the panel reports): the panel's X runs down, its Y right to left
#define CTR_W HK_PC_C33_CTR_W
#define CTR_H HK_PC_C33_CTR_H
static void ctr_to_raw(int cx, int cy, uint8_t *rx, uint8_t *ry) {
    cx = cx < 0 ? 0 : cx > CTR_W ? CTR_W : cx;
    cy = cy < 0 ? 0 : cy > CTR_H ? CTR_H : cy;
    int x = (cy * TOUCH_W + CTR_H - 1) / CTR_H, y = ((CTR_W - cx) * TOUCH_H + CTR_W - 1) / CTR_W;
    *rx = (uint8_t)(x < 1 ? 1 : x > 0xfe ? 0xfe : x);
    *ry = (uint8_t)(y < 1 ? 1 : y > 0xfe ? 0xfe : y);
}
// TouchPanel.dll's key table for its keyboard (as the program has it in memory: 24-byte entries,
// the key's character, its code, x, y, w, h in the space above; 0xff ends it). NULL if it isn't
// there as expected
struct c33_key {
    uint8_t c, pad[3];
    int32_t code, x, y, w, h;
};
static const struct c33_key *key_table(int *n) {
    static const struct c33_key *t;
    static int count, tried;
    HMODULE m = tried ? NULL : GetModuleHandleA("TouchPanel.dll");
    if (m) {   // (once the program has it loaded)
        tried = 1;
        const struct c33_key *k = m ? (const struct c33_key *)((const char *)m + 0x30a8) : NULL;
        int i = 0, digits = 0;
        for (; k && i < 64 && k[i].c != 0xff; i++) {
            if (k[i].x < 0 || k[i].y < 0 || k[i].x + k[i].w > CTR_W + 64 || k[i].y + k[i].h > CTR_H) break;
            digits += k[i].c >= '0' && k[i].c <= '9';
        }
        if (k && i < 64 && k[i].c == 0xff && digits == 10) t = k, count = i;
        hk_log("touch: %s", t ? "typing goes to the program's touch keyboard" : "the touch keyboard's table isn't where it was expected: no typing");
    }
    *n = count;
    return t;
}

static void key_places(struct c33_key_place *out, int *n) {
    int count;
    const struct c33_key *k = key_table(&count);
    *n = 0;
    for (int i = 0; k && i < count && i < 64; i++)
        if (k[i].w > 0) out[*n] = (struct c33_key_place){(char)k[i].c, k[i].x, k[i].y, k[i].w, k[i].h}, ++*n;
}

// The panel's side: what it has reported, and the touch being given
static struct {
    int down;             // FE sent, no FD yet
    uint8_t x, y;         // the last place sent
    DWORD since;          // when FE was sent
    uint32_t seq;         // the link's touch_seq seen
    uint32_t typed;       // the link's type_seq taken
    int key_phase;        // a typed key: 1 touched, 2 let go (a pause before the next)
    DWORD key_since;
    int cx, cy;           // where (TouchPanel.dll's units), for the picture
} g_touch;
static void com_queue(const uint8_t *p, int n);
static void touch_send(int down, uint8_t x, uint8_t y) {
    const uint8_t p[4] = {(uint8_t)(down ? 0xfe : 0xfd), x, y, 0xff};
    com_queue(p, 4);
    static int n;
    if (n++ < 40 || usb_log()) hk_log("touch: %s at %u, %u (the panel's numbers)", down ? "touched" : "let go", x, y);
}
// Where the panel is touched now (TouchPanel.dll's units), x -1 nowhere
static void touch_where(int *x, int *y) { *x = g_touch.down ? g_touch.cx : -1, *y = g_touch.cy; }

// Called as the program polls the port: the link's touch, else the typed keys one by one. A touch
// is held for 120 ms at least (TouchPanel.dll sees it in its own polls, every 25 ms, and the program
// in its frames), and its report read before it's let go
static void touch_feed(void) {
    struct hk_pc_link *l = hk_link();
    const DWORD now = GetTickCount();
    const int held = (int)l->touch_down;
    if (held || g_touch.key_phase == 0) {
        if (held) {
            const int cx = (int)l->touch_x, cy = (int)l->touch_y;
            uint8_t rx, ry;
            ctr_to_raw(cx, cy, &rx, &ry);
            g_touch.cx = cx, g_touch.cy = cy;
            if (!g_touch.down || rx != g_touch.x || ry != g_touch.y) {
                touch_send(1, rx, ry);
                if (!g_touch.down) g_touch.since = now;
                g_touch.down = 1, g_touch.x = rx, g_touch.y = ry;
            }
            g_touch.seq = l->touch_seq;
            return;
        }
        if (g_touch.down && g_touch.key_phase == 0) {
            if (now - g_touch.since >= 120 && g_nrx == 0) touch_send(0, g_touch.x, g_touch.y), g_touch.down = 0;
            return;
        }
    }
    // the typed keys
    if (g_touch.key_phase == 1) {
        if (now - g_touch.key_since >= 120 && g_nrx == 0) {
            touch_send(0, g_touch.x, g_touch.y);
            g_touch.down = 0, g_touch.key_phase = 2, g_touch.key_since = now;
        }
        return;
    }
    if (g_touch.key_phase == 2) {
        if (now - g_touch.key_since >= 120) g_touch.key_phase = 0;
        return;
    }
    while (g_touch.typed != l->type_seq) {
        uint8_t c = l->type_ring[g_touch.typed % 16];
        g_touch.typed++;
        if (c >= 'a' && c <= 'z') c = (uint8_t)(c - 32);
        // the keyboard's own keys for deleting and for done (the two wide keys under it, '[' and ']')
        if (c == '\b') c = '[';
        else if (c == '\r') c = ']';
        int n;
        const struct c33_key *k = key_table(&n);
        for (int i = 0; k && i < n; i++)
            if (k[i].c == c && k[i].w > 0) {
                g_touch.cx = k[i].x + k[i].w / 2, g_touch.cy = k[i].y + k[i].h / 2;
                ctr_to_raw(g_touch.cx, g_touch.cy, &g_touch.x, &g_touch.y);
                touch_send(1, g_touch.x, g_touch.y);
                g_touch.down = 1, g_touch.key_phase = 1, g_touch.key_since = now;
                return;
            }
        hk_log("touch: no key %c on the touch keyboard", c >= 32 && c < 127 ? c : '?');
    }
}

// The program reading its touch keyboard (eemall.exe's GetTouchPanelCtrHangar): while it does, the
// PC keyboard types (link.h: typing)
typedef char(__cdecl *hangar_t)(int, int, int, int);
static hangar_t g_hangar;
static volatile DWORD g_hangar_at;
static char __cdecl c33_hangar(int a, int b, int mode, int d) {
    g_hangar_at = GetTickCount() | 1;
    return g_hangar(a, b, mode, d);
}
// (and the test mode's TOUCH PANEL CHECK reading it: TestModeSetting.dll's GetTouchPanelCtrCharLevel)
typedef char(__cdecl *charlevel_t)(int);
static charlevel_t g_charlevel;
static char __cdecl c33_charlevel(int mode) {
    g_hangar_at = GetTickCount() | 1;
    return g_charlevel(mode);
}
static void typing_update(void) {
    const DWORD at = g_hangar_at;
    const uint32_t on = at && GetTickCount() - at < 400;
    if (hk_link()->typing != on) hk_link()->typing = on;
}

typedef HANDLE(WINAPI *CreateFileA_t)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef BOOL(WINAPI *ReadFile_t)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef BOOL(WINAPI *WriteFile_t)(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef BOOL(WINAPI *CloseHandle_t)(HANDLE);
static CreateFileA_t g_next_create;
static ReadFile_t g_next_read;
static WriteFile_t g_next_write;
static CloseHandle_t g_next_close;

static int is_com1(const char *p) {
    if (!p) return 0;
    if (!_strnicmp(p, "\\\\.\\", 4)) p += 4;
    return !_stricmp(p, "COM1");
}

static HANDLE WINAPI touch_CreateFileA(LPCSTR p, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD cd, DWORD fl, HANDLE t) {
    if (is_com1(p)) {
        if (!g_com) g_com = CreateEventA(NULL, TRUE, FALSE, NULL);
        g_nrx = 0;
        hk_log("touch: COM1 opened: the touch panel is HIRAKU's");
        return g_com;
    }
    return (g_next_create ? g_next_create : CreateFileA)(p, acc, share, sa, cd, fl, t);
}
static BOOL WINAPI touch_ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD got, LPOVERLAPPED ov) {
    if (!h || h != g_com) return (g_next_read ? g_next_read : ReadFile)(h, buf, n, got, ov);
    EnterCriticalSection(&g_com_lock);
    const DWORD k = n < (DWORD)g_nrx ? n : (DWORD)g_nrx;
    if (buf && k) memcpy(buf, g_rx, k);
    memmove(g_rx, g_rx + k, (size_t)(g_nrx - (int)k));
    g_nrx -= (int)k;
    LeaveCriticalSection(&g_com_lock);
    if (got) *got = k;
    if (ov) ov->Internal = 0, ov->InternalHigh = k;
    return TRUE;
}
static BOOL WINAPI touch_WriteFile(HANDLE h, LPCVOID buf, DWORD n, LPDWORD done, LPOVERLAPPED ov) {
    if (!h || h != g_com) return (g_next_write ? g_next_write : WriteFile)(h, buf, n, done, ov);
    for (DWORD i = 0; buf && i < n; i++) com_byte(((const uint8_t *)buf)[i]);
    if (done) *done = n;
    if (ov) ov->Internal = 0, ov->InternalHigh = n;
    return TRUE;
}
static BOOL WINAPI touch_CloseHandle(HANDLE h) {
    if (h && h == g_com) {
        hk_log("touch: COM1 closed");
        g_com = NULL;
    }
    return (g_next_close ? g_next_close : CloseHandle)(h);
}
static BOOL WINAPI touch_GetOverlappedResult(HANDLE h, LPOVERLAPPED ov, LPDWORD n, BOOL wait) {
    if (!h || h != g_com) return GetOverlappedResult(h, ov, n, wait);
    if (n) *n = ov ? (DWORD)ov->InternalHigh : 0;
    return TRUE;
}
static BOOL WINAPI touch_ClearCommError(HANDLE h, LPDWORD errors, LPCOMSTAT st) {
    if (!h || h != g_com) return ClearCommError(h, errors, st);
    touch_feed();
    if (errors) *errors = 0;
    if (st) {
        memset(st, 0, sizeof *st);
        EnterCriticalSection(&g_com_lock);
        st->cbInQue = (DWORD)g_nrx;
        LeaveCriticalSection(&g_com_lock);
    }
    return TRUE;
}
static BOOL WINAPI touch_GetCommState(HANDLE h, LPDCB dcb) {
    if (!h || h != g_com) return GetCommState(h, dcb);
    if (dcb) {
        memset(dcb, 0, sizeof *dcb);
        dcb->DCBlength = sizeof *dcb;
        dcb->BaudRate = 9600, dcb->ByteSize = 8, dcb->fBinary = 1, dcb->XonChar = 0x11, dcb->XoffChar = 0x13;
    }
    return TRUE;
}
static BOOL WINAPI touch_SetCommState(HANDLE h, LPDCB dcb) {
    if (!h || h != g_com) return SetCommState(h, dcb);
    if (dcb) hk_log("touch: COM1 at %lu baud, %d bits", (unsigned long)dcb->BaudRate, dcb->ByteSize);
    return TRUE;
}
static BOOL WINAPI touch_SetCommMask(HANDLE h, DWORD m) { return h && h == g_com ? TRUE : SetCommMask(h, m); }
static BOOL WINAPI touch_SetupComm(HANDLE h, DWORD i, DWORD o) { return h && h == g_com ? TRUE : SetupComm(h, i, o); }
static BOOL WINAPI touch_SetCommTimeouts(HANDLE h, LPCOMMTIMEOUTS t) { return h && h == g_com ? TRUE : SetCommTimeouts(h, t); }
static BOOL WINAPI touch_PurgeComm(HANDLE h, DWORD f) {
    if (!h || h != g_com) return PurgeComm(h, f);
    if (f & PURGE_RXCLEAR) {
        EnterCriticalSection(&g_com_lock);
        g_nrx = 0;
        LeaveCriticalSection(&g_com_lock);
    }
    return TRUE;
}

// ================================================================ the station's paths

// The station names its files by whole paths (D:\C33\Resource\..., E:\C33\backupram-data.bin),
// and opens many of them through the C library (fopen, remove, _mkdir, _stat), the C++ streams
// (MSVCIRT's ifstream and ofstream), CopyFileA, CreateFileW and mmioOpenA (its sounds), none of
// which the cabinet's own path hooks see: these send them where HIRAKU's map does (HK_PC_MAP)
typedef FILE *(__cdecl *fopen_t)(const char *, const char *);
typedef int(__cdecl *path_t)(const char *);
typedef int(__cdecl *stat_t)(const char *, void *);
// (a stream's constructor: this, the name, the mode, the protection, and MSVC's hidden flag for its
// virtual base)
typedef void *(__thiscall *fstream_t)(void *, const char *, int, int, int);
typedef BOOL(WINAPI *CopyFileA_t)(LPCSTR, LPCSTR, BOOL);
typedef HANDLE(WINAPI *CreateFileW_t)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HMMIO(WINAPI *mmioOpenA_t)(LPSTR, LPMMIOINFO, DWORD);
static fopen_t g_fopen;
static path_t g_remove, g_mkdir;
static stat_t g_stat;
static fstream_t g_ifstream, g_ofstream;
static mmioOpenA_t g_mmioOpenA;

static int path_log(void) {
    static int on = -1;
    if (on < 0) on = getenv("HK_PC_FILELOG") != NULL;
    return on;
}
static FILE *__cdecl c33_fopen(const char *p, const char *mode) {
    char b[MAX_PATH];
    FILE *f = g_fopen(hk_map_path(p, b), mode);
    if (!f && path_log()) hk_log("hiraku-pc: fopen %s (%s, %s) failed", p ? p : "(null)", hk_map_path(p, b), mode ? mode : "");
    return f;
}
static int __cdecl c33_remove(const char *p) {
    char b[MAX_PATH];
    return g_remove(hk_map_path(p, b));
}
static int __cdecl c33_mkdir(const char *p) {
    char b[MAX_PATH];
    return g_mkdir(hk_map_path(p, b));
}
static int __cdecl c33_stat(const char *p, void *st) {
    char b[MAX_PATH];
    return g_stat(hk_map_path(p, b), st);
}
static void *__thiscall c33_ifstream(void *self, const char *p, int mode, int prot, int virt) {
    char b[MAX_PATH];
    return g_ifstream(self, hk_map_path(p, b), mode, prot, virt);
}
static void *__thiscall c33_ofstream(void *self, const char *p, int mode, int prot, int virt) {
    char b[MAX_PATH];
    return g_ofstream(self, hk_map_path(p, b), mode, prot, virt);
}
static BOOL WINAPI c33_CopyFileA(LPCSTR from, LPCSTR to, BOOL fail) {
    char b1[MAX_PATH], b2[MAX_PATH];
    return CopyFileA(hk_map_path(from, b1), hk_map_path(to, b2), fail);
}
static HANDLE WINAPI c33_CreateFileW(LPCWSTR p, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD cd, DWORD fl, HANDLE t) {
    char a[MAX_PATH], b[MAX_PATH];
    WCHAR w[MAX_PATH];
    if (p && WideCharToMultiByte(CP_ACP, 0, p, -1, a, sizeof a, NULL, NULL) > 0) {
        const char *to = hk_map_path(a, b);
        if (to != a && MultiByteToWideChar(CP_ACP, 0, to, -1, w, MAX_PATH) > 0) p = w;
    }
    return CreateFileW(p, acc, share, sa, cd, fl, t);
}
static HMMIO WINAPI c33_mmioOpenA(LPSTR p, LPMMIOINFO info, DWORD flags) {
    char b[MAX_PATH];
    // (a memory file's "name" is no path: only names given as one are mapped)
    return g_mmioOpenA(p && !(info && info->pchBuffer) ? (LPSTR)hk_map_path(p, b) : p, info, flags);
}

// A whole path turned into UTF-16 for Windows' own DLLs: DXShow.dll's movies (the walks between
// the places, D:\C33\Resource\movie2\NNNN.m1v), which it hands to DirectShow
// (IAMMultiMediaStream::OpenFile) and Windows opens itself: mapped as the others (a mod's file
// first), so they're the game's data's and never the Wine prefix's D:
typedef int(WINAPI *mbtowc_t)(UINT, DWORD, LPCCH, int, LPWSTR, int);
static mbtowc_t g_next_mbtowc;
static int WINAPI c33_MultiByteToWideChar(UINT page, DWORD fl, LPCCH s, int n, LPWSTR w, int wn) {
    if (s && n == -1 && (page == CP_ACP || page == CP_THREAD_ACP) && strchr(s, '\\') && strlen(s) < MAX_PATH) {
        char b[MAX_PATH];
        const char *to = hk_map_path(s, b);
        if (to != s) {
            const int r = MultiByteToWideChar(page, fl, to, -1, w, wn);
            static int said;
            if (r > 0 && strstr(s, ".m1v") && said++ < 5) hk_log("hiraku-pc: a movie, %s, from %s", s, to);
            if (r > 0) return r;   // (too long for the program's buffer: its own name)
        }
    }
    return (g_next_mbtowc ? g_next_mbtowc : MultiByteToWideChar)(page, fl, s, n, w, wn);
}

// For bringing the movies up, HK_PC_C33_MOVIE_CHECK=NAME (default 0103.m1v): the walk movie's whole
// path as the program names it, through the hook above, opened the way DXShow.dll opens it
// (IAMMultiMediaStream: Initialize, AddMediaStream(primary video), OpenFile) and its length logged
static DWORD WINAPI movie_check(LPVOID arg) {
    (void)arg;
    static const GUID clsid = {0x49c47ce5, 0x9ba4, 0x11d0, {0x82, 0x12, 0x00, 0xc0, 0x4f, 0xc3, 0x2c, 0x45}};
    static const GUID iid = {0xbebe595c, 0x9a6f, 0x11d0, {0x8f, 0xde, 0x00, 0xc0, 0x4f, 0xd9, 0x18, 0x9d}};
    static const GUID video = {0xa35ff56a, 0x9fda, 0x11d0, {0x8f, 0xdf, 0x00, 0xc0, 0x4f, 0xd9, 0x18, 0x9d}};
    const char *name = getenv("HK_PC_C33_MOVIE_CHECK");
    char path[MAX_PATH];
    snprintf(path, sizeof path, "D:\\C33\\Resource\\movie2\\%s", name && strchr(name, '.') ? name : "0103.m1v");
    WCHAR w[MAX_PATH];
    if (c33_MultiByteToWideChar(CP_ACP, 0, path, -1, w, MAX_PATH) <= 0) return 0;
    const DWORD attr = GetFileAttributesW(w);
    hk_log("hiraku-pc: movie check: %s %s", path, attr == INVALID_FILE_ATTRIBUTES ? "not found" : "found");
    HMODULE ole = LoadLibraryA("ole32.dll");
    typedef HRESULT(WINAPI * init_t)(LPVOID);
    typedef HRESULT(WINAPI * create_t)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID *);
    init_t init = ole ? (init_t)(void *)GetProcAddress(ole, "CoInitialize") : NULL;
    create_t create = ole ? (create_t)(void *)GetProcAddress(ole, "CoCreateInstance") : NULL;
    if (!init || !create) return 0;
    init(NULL);
    IAMMultiMediaStream *ms = NULL;
    HRESULT hr = create(&clsid, NULL, CLSCTX_INPROC_SERVER, &iid, (void **)&ms);
    if (SUCCEEDED(hr)) hr = IAMMultiMediaStream_Initialize(ms, STREAMTYPE_READ, 0, NULL);
    if (SUCCEEDED(hr)) hr = IAMMultiMediaStream_AddMediaStream(ms, NULL, &video, 0, NULL);
    const HRESULT added = hr;
    if (SUCCEEDED(hr)) hr = IAMMultiMediaStream_OpenFile(ms, w, 0);
    STREAM_TIME len = 0;
    if (SUCCEEDED(hr)) IAMMultiMediaStream_GetDuration(ms, &len);
    hk_log("hiraku-pc: movie check: the stream %s (%#lx, adding the video %#lx), %.2f s", SUCCEEDED(hr) ? "opened" : "not opened",
           (unsigned long)hr, (unsigned long)added, (double)len / 1e7);
    if (ms) IAMMultiMediaStream_Release(ms);
    return 0;
}

// The walk movies' pictures. DXShow.dll takes each frame from IDirectDrawMediaStream's own sample
// (CreateSample with no surface: the stream's format) and copies its rows as they are into a
// Direct3D texture it made R5G6B5 (0x1000184f: format 0x17; 0x10002240 copies the rows): it was made
// for a station whose desktop ran in 16 bits, where the stream's samples are 16-bit RGB 5-6-5. On a
// 32-bit desktop (Wine, any PC today) they are 32-bit, and every walk is drawn as stripes. So the
// stream is told 5-6-5 before the program opens the movie: IAMMultiMediaStream's OpenFile (made by
// the program's CoCreateInstance) first sets the primary video stream's format (SetFormat, pixel
// format only), as the original display gave it.
typedef HRESULT(WINAPI *cci_t)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID *);
typedef HRESULT(STDMETHODCALLTYPE *openfile_t)(IAMMultiMediaStream *, LPCWSTR, DWORD);
static cci_t g_next_cci;
static openfile_t g_openfile;
static HRESULT STDMETHODCALLTYPE c33_OpenFile(IAMMultiMediaStream *ms, LPCWSTR file, DWORD flags) {
    static const GUID video = {0xa35ff56a, 0x9fda, 0x11d0, {0x8f, 0xdf, 0x00, 0xc0, 0x4f, 0xd9, 0x18, 0x9d}};
    static const GUID iid_dd = {0xf4104fce, 0x9a70, 0x11d0, {0x8f, 0xde, 0x00, 0xc0, 0x4f, 0xd9, 0x18, 0x9d}};
    static int n;
    IMediaStream *st = NULL;
    IDirectDrawMediaStream *dd = NULL;
    HRESULT hr = IAMMultiMediaStream_GetMediaStream(ms, &video, &st);
    if (SUCCEEDED(hr)) hr = IMediaStream_QueryInterface(st, &iid_dd, (void **)&dd);
    if (SUCCEEDED(hr)) {
        DDSURFACEDESC d;
        memset(&d, 0, sizeof d);
        d.dwSize = sizeof d;
        d.dwFlags = DDSD_PIXELFORMAT;
        d.ddpfPixelFormat.dwSize = sizeof d.ddpfPixelFormat;
        d.ddpfPixelFormat.dwFlags = DDPF_RGB;
        d.ddpfPixelFormat.dwRGBBitCount = 16;
        d.ddpfPixelFormat.dwRBitMask = 0xf800;
        d.ddpfPixelFormat.dwGBitMask = 0x07e0;
        d.ddpfPixelFormat.dwBBitMask = 0x001f;
        hr = IDirectDrawMediaStream_SetFormat(dd, &d, NULL);
        IDirectDrawMediaStream_Release(dd);
    }
    if (st) IMediaStream_Release(st);
    if (n++ < 3) hk_log("hiraku-pc: a movie's pictures in 16-bit RGB 5-6-5, as the program draws them: %s (%#lx)", SUCCEEDED(hr) ? "set" : "not set", (unsigned long)hr);
    return g_openfile(ms, file, flags);
}
static HRESULT WINAPI c33_CoCreateInstance(REFCLSID clsid, LPUNKNOWN outer, DWORD ctx, REFIID riid, LPVOID *out) {
    static const GUID mms = {0x49c47ce5, 0x9ba4, 0x11d0, {0x82, 0x12, 0x00, 0xc0, 0x4f, 0xc3, 0x2c, 0x45}};
    static const GUID iid_amms = {0xbebe595c, 0x9a6f, 0x11d0, {0x8f, 0xde, 0x00, 0xc0, 0x4f, 0xd9, 0x18, 0x9d}};
    if (!g_next_cci) g_next_cci = (cci_t)(void *)GetProcAddress(LoadLibraryA("ole32.dll"), "CoCreateInstance");
    const HRESULT hr = g_next_cci(clsid, outer, ctx, riid, out);
    if (SUCCEEDED(hr) && out && *out && clsid && IsEqualGUID(clsid, &mms) && riid && IsEqualGUID(riid, &iid_amms)) {
        // the class's table of calls (one for every such object): its OpenFile, once
        IAMMultiMediaStreamVtbl *vt = (IAMMultiMediaStreamVtbl *)((IAMMultiMediaStream *)*out)->lpVtbl;
        DWORD was;
        if ((void *)vt->OpenFile != (void *)c33_OpenFile && VirtualProtect(&vt->OpenFile, sizeof vt->OpenFile, PAGE_READWRITE, &was)) {
            g_openfile = vt->OpenFile;
            vt->OpenFile = c33_OpenFile;
            VirtualProtect(&vt->OpenFile, sizeof vt->OpenFile, was, &was);
        }
    }
    return hr;
}

static void *crt_hook(const char *dll, const char *fn) {
    HMODULE m = GetModuleHandleA(dll);
    if (!m) return NULL;
#define TAKE(name, var, to)                                             \
    if (!strcmp(fn, name)) {                                            \
        if (!var) var = (void *)GetProcAddress(m, name);                \
        return var ? (void *)to : NULL;                                 \
    }
    if (!_stricmp(dll, "msvcrt.dll")) {
        TAKE("fopen", *(void **)&g_fopen, c33_fopen)
        TAKE("remove", *(void **)&g_remove, c33_remove)
        TAKE("_mkdir", *(void **)&g_mkdir, c33_mkdir)
        TAKE("_stat", *(void **)&g_stat, c33_stat)
    }
    if (!_stricmp(dll, "msvcirt.dll")) {
        TAKE("??0ifstream@@QAE@PBDHH@Z", *(void **)&g_ifstream, c33_ifstream)
        TAKE("??0ofstream@@QAE@PBDHH@Z", *(void **)&g_ofstream, c33_ofstream)
    }
    if (!_stricmp(dll, "winmm.dll")) TAKE("mmioOpenA", *(void **)&g_mmioOpenA, c33_mmioOpenA)
#undef TAKE
    return NULL;
}

// The cabinet's import hooks, for the game's modules (cabinet.c): the serial port's calls, each
// passing on what isn't COM1 (to the cabinet's own hook where it has one: `next`), and the paths above
static void *c33_hook(const char *dll, const char *fn, void *next) {
    if (!_stricmp(dll, "TouchPanel.dll") && !strcmp(fn, "?GetTouchPanelCtrHangar@@YADHHW4_EnTouchModeType@@G@Z")) {
        HMODULE m = GetModuleHandleA("TouchPanel.dll");
        if (!g_hangar && m) g_hangar = (hangar_t)(void *)GetProcAddress(m, fn);
        return g_hangar ? (void *)c33_hangar : NULL;
    }
    if (!_stricmp(dll, "TouchPanel.dll") && !strcmp(fn, "?GetTouchPanelCtrCharLevel@@YADW4_EnTouchModeType@@@Z")) {
        HMODULE m = GetModuleHandleA("TouchPanel.dll");
        if (!g_charlevel && m) g_charlevel = (charlevel_t)(void *)GetProcAddress(m, fn);
        return g_charlevel ? (void *)c33_charlevel : NULL;
    }
    if (!_stricmp(dll, "ole32.dll") && !strcmp(fn, "CoCreateInstance"))   // (the walk movies' format)
        return g_next_cci = (cci_t)next, (void *)c33_CoCreateInstance;
    if (_stricmp(dll, "kernel32.dll")) return next ? NULL : crt_hook(dll, fn);
    if (!strcmp(fn, "MultiByteToWideChar")) return g_next_mbtowc = (mbtowc_t)next, (void *)c33_MultiByteToWideChar;
    if (!strcmp(fn, "CopyFileA")) return (void *)c33_CopyFileA;
    if (!strcmp(fn, "CreateFileW")) return (void *)c33_CreateFileW;
    if (!strcmp(fn, "CreateFileA")) return g_next_create = (CreateFileA_t)next, (void *)touch_CreateFileA;
    if (!strcmp(fn, "ReadFile")) return g_next_read = (ReadFile_t)next, (void *)touch_ReadFile;
    if (!strcmp(fn, "WriteFile")) return g_next_write = (WriteFile_t)next, (void *)touch_WriteFile;
    if (!strcmp(fn, "CloseHandle")) return g_next_close = (CloseHandle_t)next, (void *)touch_CloseHandle;
    if (next) return NULL;   // (the cabinet's own for the rest: none of these)
    if (!strcmp(fn, "GetOverlappedResult")) return (void *)touch_GetOverlappedResult;
    if (!strcmp(fn, "ClearCommError")) return (void *)touch_ClearCommError;
    if (!strcmp(fn, "GetCommState")) return (void *)touch_GetCommState;
    if (!strcmp(fn, "SetCommState")) return (void *)touch_SetCommState;
    if (!strcmp(fn, "SetCommMask")) return (void *)touch_SetCommMask;
    if (!strcmp(fn, "SetupComm")) return (void *)touch_SetupComm;
    if (!strcmp(fn, "SetCommTimeouts")) return (void *)touch_SetCommTimeouts;
    if (!strcmp(fn, "PurgeComm")) return (void *)touch_PurgeComm;
    return NULL;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD why, LPVOID r) {
    (void)h, (void)r;
    if (why == DLL_PROCESS_ATTACH) {
        InitializeCriticalSection(&g_com_lock);
        InitializeCriticalSection(&g_coin_lock);
        InitializeCriticalSection(&g_card_lock);
        hk_picture_more = c33_picture;
        hk_set_device_hook(c33_hook);
        hk_cabinet_start();
        hk_log("hiraku-pc: C33: the I/O board, the video board and the touch panel are HIRAKU's");
    }
    return TRUE;
}
