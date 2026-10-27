// SPDX-License-Identifier: GPL-3.0-or-later
// hiraku-pc's link (runtime/pc/link.h), as pop'n 15-18's DLLs share it (runtime/pc/cabinet.c):
// HK_PC_LINK names the file hiraku-pc maps. The controls come in (the pad's bits, the coins, the
// keys bound to them: the game's window may have the keyboard), the main screen's pictures go out
// in step with hiraku-pc's pacing. Without it the loader runs alone (the game's own windows).
#include "pc64.h"
#include "../pc/link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct hk_pc_link *g_link;
static int g_looked;

struct hk_pc_link *pc64_link(void) {
    if (g_looked) return g_link;
    g_looked = 1;
    const char *f = getenv("HK_PC_LINK");
    if (!f || !*f) return NULL;
    HANDLE h = CreateFileA(f, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    HANDLE m = h != INVALID_HANDLE_VALUE ? CreateFileMappingA(h, NULL, PAGE_READWRITE, 0, HK_PC_LINK_SIZE, NULL) : NULL;
    struct hk_pc_link *l = m ? (struct hk_pc_link *)MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, HK_PC_LINK_SIZE) : NULL;
    if (m) CloseHandle(m);   // the view keeps the mapping
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (l && l->magic == HK_PC_LINK_MAGIC && l->version == HK_PC_LINK_VERSION) {
        g_link = l;
        pc64_log("linked to hiraku-pc (%s)", f);
    } else {
        pc64_log("HK_PC_LINK=%s: not hiraku-pc's link (version %u)", f, l ? l->version : 0);
    }
    return g_link;
}

int pc64_host_shows(void) {
    struct hk_pc_link *l = pc64_link();
    return l && l->host_shows;
}

static int key_down(int vk) { return vk && (GetAsyncKeyState(vk) & 0x8000) != 0; }

// Escape in one of the game's windows ends it (HIRAKU's window ends with it)
static void escape_quits(void) {
    if (!key_down(VK_ESCAPE)) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    if (pid != GetCurrentProcessId()) return;
    pc64_log("Escape: the game ends");
    TerminateProcess(GetCurrentProcess(), 0);
}

// The inputs held now (PC64_IN_*): the link's pad and the keys bound to its bits, read at most
// once a millisecond (each key a call Wine may take to its server), and the coins as the coin
// switch's pulses (four status reads down, four up: a coin each)
unsigned pc64_link_inputs(void) {
    struct hk_pc_link *l = pc64_link();
    if (!l) return 0;
    static uint32_t keys;
    static DWORD keys_at;
    const DWORD now = GetTickCount();
    if (now != keys_at) {
        keys_at = now;
        escape_quits();
        keys = 0;
        for (int i = 0; i < 32; i++)
            if (key_down(l->keys[i][0]) || key_down(l->keys[i][1])) keys |= 1u << i;
    }
    const uint32_t pad = l->pad | keys;
    l->game_alive++;
    unsigned in = 0;
    if (pad & HK_PC_PAD_TEST) in |= PC64_IN_TEST;
    if (pad & HK_PC_PAD_SERVICE) in |= PC64_IN_SERVICE;
    for (int b = 1; b <= 9; b++)
        if (pad & HK_PC_PAD_BUTTON(b)) in |= PC64_IN_BUTTON(b);
    if (pad & HK_PC_PAD_RED) in |= PC64_IN_RED;
    if (pad & HK_PC_PAD_BLUE) in |= PC64_IN_BLUE;
    if (pad & HK_PC_PAD_HEADPHONES) in |= PC64_IN_HEADPHONES;
    static uint32_t coins_seen, phase;
    static int coin_was;
    const int coin_key = key_down(l->coin_keys[0]) || key_down(l->coin_keys[1]);
    if (coin_key && !coin_was) coins_seen--;   // a key's coin: one more to pulse
    coin_was = coin_key;
    if (phase) {
        if (++phase <= 4) in |= PC64_IN_COIN;
        if (phase == 8) phase = 0;
    } else if (coins_seen != l->coins) {
        coins_seen++;
        phase = 1;
        in |= PC64_IN_COIN;
    }
    return in;
}

// The buttons held now (PC64_IN_BUTTON), the link's and the keys bound to them, with nothing else
// done (pc64_link_inputs counts coins and the game's polls: the game's board reads only)
unsigned pc64_link_buttons(void) {
    struct hk_pc_link *l = pc64_link();
    if (!l) return 0;
    uint32_t pad = l->pad;
    for (int b = 1; b <= 9; b++) {
        const int i = __builtin_ctz(HK_PC_PAD_BUTTON(b));
        if (key_down(l->keys[i][0]) || key_down(l->keys[i][1])) pad |= HK_PC_PAD_BUTTON(b);
    }
    unsigned in = 0;
    for (int b = 1; b <= 9; b++)
        if (pad & HK_PC_PAD_BUTTON(b)) in |= PC64_IN_BUTTON(b);
    return in;
}

// Player 1's 10-key: the link's, and the keys bound to it (the game's window may have the keyboard)
volatile unsigned pc64_keys_held;
unsigned pc64_link_keypad(void) {
    struct hk_pc_link *l = pc64_link();
    if (!l) return pc64_keys_held;
    unsigned k = l->keypad | pc64_keys_held;
    for (int i = 0; i < 12; i++)
        if (key_down(l->keypad_keys[i][0]) || key_down(l->keypad_keys[i][1])) k |= 1u << i;
    return k;
}

// The card held to the reader: a count that moves on with each (the link's, a key bound to it,
// HK_SCRIPT's "card"), and its UID (the link's, else HK_PC64_CARD's 16 hex digits)
volatile unsigned pc64_card_presses;
uint32_t pc64_card(uint8_t uid[8]) {
    static int was;
    static uint32_t keys;
    struct hk_pc_link *l = pc64_link();
    const int now = l && (key_down(l->card_keys[0]) || key_down(l->card_keys[1]));
    if (now && !was) keys++;
    was = now;
    memset(uid, 0, 8);
    if (l) {
        memcpy(uid, (const void *)l->card_uid, 8);
    } else {
        const char *hex = getenv("HK_PC64_CARD");
        for (int i = 0; hex && i < 8 && hex[2 * i] && hex[2 * i + 1]; i++) {
            unsigned b;
            if (sscanf(hex + 2 * i, "%2x", &b) == 1) uid[i] = (uint8_t)b;
        }
    }
    return (l ? l->card_seq : 0) + keys + pc64_card_presses;
}

// A fifth of a millisecond's nap (NtDelayExecution: 100 ns units), as cabinet.c's
void pc64_nap(void) {
    typedef LONG(NTAPI * delay_t)(BOOLEAN, PLARGE_INTEGER);
    static delay_t delay;
    static int looked;
    if (!looked) {
        looked = 1;
        HMODULE nt = GetModuleHandleA("ntdll.dll");
        delay = nt ? (delay_t)(void *)GetProcAddress(nt, "NtDelayExecution") : NULL;
    }
    LARGE_INTEGER t;
    t.QuadPart = -2000;
    if (delay) delay(FALSE, &t);
    else Sleep(1);
}

// The main screen's picture is in the link's pixels (pc64_link_pixels): counted, then the game
// waits until hiraku-pc has shown it and its pacing starts the next frame (a quarter of a second
// at most, if it has stopped answering)
void pc64_link_picture(unsigned w, unsigned h) {
    pc64_patches_apply();   // (the mods' patches: each frame, as the 32-bit games' DLL)
    struct hk_pc_link *l = pc64_link();
    l->frame_w = w, l->frame_h = h;
    MemoryBarrier();
    l->frame_seq = l->frame_seq + 1;
    l->frames++;
    const uint32_t seq = l->frame_seq;
    const DWORD t0 = GetTickCount();
    for (int spins = 0; l->frame_go != seq; spins++) {
        if (GetTickCount() - t0 > 250) break;
        if (spins < 200) YieldProcessor();
        else if (spins < 400) SwitchToThread();
        else pc64_nap();
    }
    l->go_count = l->go_count + 1;
}

int pc64_link_not_shown(void) {
    struct hk_pc_link *l = pc64_link();
    return l && l->not_shown;
}

uint32_t *pc64_link_pixels(void) {
    struct hk_pc_link *l = pc64_link();
    return l ? (uint32_t *)((char *)l + HK_PC_PIXELS) : NULL;
}
