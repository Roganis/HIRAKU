// SPDX-License-Identifier: GPL-3.0-or-later
// Presses in the PC pop'n music games (PLAN.md section 30): the board's samples of the pad, and
// presses judged by when they happened.
//
// The board's samples (pop'n 16-18). These games take their buttons in play from usbPadReadLast:
// the board's latest samples, a press being a sample with the button after four without it, and a
// release four without it. Konami's board samples many times a frame; one sample a read, as HIRAKU
// gave before, made a release need four frames, so a button pressed again sooner (a fast repeat on
// one button) wasn't a press. The samples are made here from the pad's timed changes (link.h:
// pad_ev) at 360 a second, the six a frame the games take at most, as the board's would be.
//
// Judging. Each of pop'n 15-18 reads its pad once a frame, takes its song's time then (timeGetTime,
// in 4 ms steps), and for each button pressed judges the distance from that time to the lane's
// nearest note, kept per lane (a 16-bit count of 4 ms steps, from a table of 9 lanes, 8 bytes
// each): a press anywhere in the frame before counts as at the frame's start. With
// HK_PC_PRESS_TIME, that distance is taken back to when the press happened (hiraku-pc hands the
// controller's own event time), and HK_PC_JUDGE_OFFSET moves every press by that many
// milliseconds. The judging routine is found by its shape, in every program date of the four games:
// it compares the distance with the judgement's windows, six 16-bit words a routine sets to
// -44, -28, -12, 12, 28, 44 (in 4 ms steps; a chart can change them), and the distance table is
// the one it reads just before. Its one call is made a call to judge_press here, which moves the
// lane's distance and calls it.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cabinet.h"

// --- the board's samples

// 360 a second; for tests, HK_PC_SAMPLE_US another spacing (16667: a sample a frame, as before)
static uint32_t sample_us(void) {
    static uint32_t us;
    if (!us) {
        const char *e = getenv("HK_PC_SAMPLE_US");
        us = e && atoi(e) > 0 ? (uint32_t)atoi(e) : 2778u;
    }
    return us;
}

int hk_pad_samples(uint16_t last[10]) {
    struct hk_pc_link *l = hk_link();
    const uint32_t SAMPLE_US = sample_us();
    static uint32_t at, applied, state;   // the newest sample's time; the changes in the samples so far
    static int started;
    uint32_t now;
    // what isn't hiraku-pc's (the keys the DLL reads itself, the test script): as they are now
    const uint32_t pad = hk_pad_now(), other = (pad & ~l->pad) >> 8 & 0x1ff;
    if (!hk_host_us(&now)) {   // no times yet: a sample a read
        memmove(last + 1, last, 9 * sizeof *last);
        last[0] = (uint16_t)(pad >> 8 & 0x1ff);
        return 1;
    }
    const uint32_t end = l->pad_ev_seq;
    if (!started || end - applied > HK_PC_PAD_EVENTS) {
        if (!started) at = now - SAMPLE_US;
        applied = end, state = l->pad, started = 1;
    }
    uint32_t n = (now - at) / SAMPLE_US;
    if (n > 10) at = now - 10 * SAMPLE_US, n = 10;
    for (uint32_t i = 0; i < n; i++) {
        at += SAMPLE_US;
        // the changes up to the sample's time; one handed on after an earlier sample was made is
        // in the first sample made since, and a button that went down in the sample's time is down
        // in it, even if up again (a short press)
        uint32_t downs = 0;
        for (; applied != end; applied++) {
            const uint32_t us = l->pad_ev[applied % HK_PC_PAD_EVENTS].us, pad = l->pad_ev[applied % HK_PC_PAD_EVENTS].pad;
            if ((int32_t)(us - at) > 0) break;
            downs |= pad & ~state;
            state = pad;
        }
        memmove(last + 1, last, 9 * sizeof *last);
        last[0] = (uint16_t)(((state | downs) >> 8 & 0x1ff) | other);
    }
    return (int)n;
}

// --- judging

static int16_t *g_distance;   // the lane's distance, 8 bytes a lane; its note 4 bytes before
typedef uint32_t(__cdecl *judge_t)(uint32_t lane);
static judge_t g_judge;
static int g_press_time;
static int32_t g_offset_us;

// How long ago the press of `button` (1-9) the game has just seen happened, in microseconds: its
// newest going down among the changes the game had read (link.h: pad_ev_seen), as long ago as that
// from hiraku-pc's time now. 0 if there's none in the last 50 ms (a key the DLL read itself)
static uint32_t press_age(int button) {
    struct hk_pc_link *l = hk_link();
    uint32_t now;
    if (!hk_host_us(&now)) return 0;
    const uint32_t bit = HK_PC_PAD_BUTTON(button), end = l->pad_ev_seen;
    const uint32_t kept = end < HK_PC_PAD_EVENTS - 8 ? end : HK_PC_PAD_EVENTS - 8;   // still in the ring
    for (uint32_t k = 0; k < kept; k++) {
        const uint32_t s = end - 1 - k;
        if (!(l->pad_ev[s % HK_PC_PAD_EVENTS].pad & bit)) continue;
        if (k + 1 < kept && (l->pad_ev[(s - 1) % HK_PC_PAD_EVENTS].pad & bit)) continue;   // held before it
        const uint32_t age = now - l->pad_ev[s % HK_PC_PAD_EVENTS].us;
        return age <= 50000 ? age : 0;
    }
    return 0;
}

static uint32_t __cdecl judge_press(uint32_t lane) {
    const uint32_t k = lane & 0xffff;
    if (k < 9 && *(void *const *)((uint8_t *)(g_distance + k * 4) - 4)) {
        int32_t us = g_offset_us;
        if (g_press_time) us += (int32_t)press_age((int)k + 1);
        const int32_t steps = (us >= 0 ? us + 2000 : us - 2000) / 4000;
        static int log = -1;
        if (log < 0) log = getenv("HK_PC_JUDGE_LOG") != NULL;
        const int16_t was = g_distance[k * 4];
        g_distance[k * 4] = (int16_t)(was - steps);
        const uint32_t r = g_judge(lane);
        if (log) hk_log("judge: lane %u, %d steps from its note, %d earlier (%d us): %08x", k, was, steps, us, r);
        return r;
    }
    return g_judge(lane);
}

// The judging routine's start: back from `p` to the 16-byte boundary after its predecessor's end
// (two int3s, or a ret and an int3)
static uint8_t *routine_start(uint8_t *code, uint8_t *p) {
    for (uint8_t *s = p; s > p - 128 && s - 2 > code; s--)
        if (!((uintptr_t)s & 15) && s[-1] == 0xcc && (s[-2] == 0xcc || s[-2] == 0xc3)) return s;
    return NULL;
}

void hk_press_patch_program(void) {
    const char *pt = getenv("HK_PC_PRESS_TIME"), *off = getenv("HK_PC_JUDGE_OFFSET");
    g_press_time = pt && *pt && *pt != '0';
    g_offset_us = off && *off ? (int32_t)(atof(off) * 1000) : 0;
    if (!g_press_time && !g_offset_us) return;
    char *base = (char *)hk_program();
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    for (WORD s = 0; s < nt->FileHeader.NumberOfSections; s++, sec++) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uint8_t *code = (uint8_t *)base + sec->VirtualAddress;
        const size_t size = sec->Misc.VirtualSize;
        // the windows' defaults: mov word [w0 + 2k], -44 -28 -12 12 28 44
        static const uint16_t def[6] = {0xffd4, 0xffe4, 0xfff4, 0x0c, 0x1c, 0x2c};
        uint32_t w0 = 0;
        int found = 0;
        for (size_t i = 0; i + 54 < size; i++) {
            int k = 0;
            uint32_t a0;
            memcpy(&a0, code + i + 3, 4);
            for (; k < 6; k++) {
                const uint8_t *q = code + i + 9 * k;
                uint32_t a;
                uint16_t v;
                memcpy(&a, q + 3, 4), memcpy(&v, q + 7, 2);
                if (q[0] != 0x66 || q[1] != 0xc7 || q[2] != 0x05 || a != a0 + 2u * k || v != def[k]) break;
            }
            if (k == 6) w0 = a0, found++;
        }
        if (found != 1) continue;
        // the routine comparing a distance with the first: cmp r16, [w0]
        uint8_t *cmp = NULL;
        found = 0;
        for (size_t i = 0; i + 7 < size; i++)
            if (code[i] == 0x66 && code[i + 1] == 0x3b && (code[i + 2] & 0xc7) == 0x05 && !memcmp(code + i + 3, &w0, 4))
                cmp = code + i, found++;
        uint8_t *start = found == 1 ? routine_start(code, cmp) : NULL;
        if (!start) continue;
        // the distance it reads: movzx r32, word [reg(+reg*s) + table], the table just after the windows
        uint32_t table = 0;
        for (uint8_t *q = start; q + 8 < cmp; q++) {
            if (q[0] != 0x0f || q[1] != 0xb7 || q[2] >> 6 != 2) continue;
            uint32_t d;
            memcpy(&d, q + ((q[2] & 7) == 4 ? 4 : 3), 4);
            if (d - w0 >= 0x10 && d - w0 <= 0x100) table = d;
        }
        // its one call
        uint8_t *call = NULL;
        found = 0;
        for (size_t i = 0; i + 5 <= size; i++) {
            int32_t rel;
            memcpy(&rel, code + i + 1, 4);
            if (code[i] == 0xe8 && code + i + 5 + rel == start) call = code + i, found++;
        }
        if (!table || found != 1) {
            hk_log("hiraku-pc: the judging routine at %p, but not its distances or its one call: presses judged as the game does",
                   (void *)start);
            return;
        }
        g_distance = (int16_t *)(uintptr_t)table;
        g_judge = (judge_t)(void *)start;
        const int32_t rel = (int32_t)((uint8_t *)(void *)judge_press - (call + 5));
        DWORD old;
        VirtualProtect(call + 1, 4, PAGE_EXECUTE_READWRITE, &old);
        memcpy(call + 1, &rel, 4);
        VirtualProtect(call + 1, 4, old, &old);
        FlushInstructionCache(GetCurrentProcess(), call, 5);
        hk_log("hiraku-pc: presses judged %s, then %.1f ms earlier (the judging at %p, its distances at %08x)",
               g_press_time ? "by when they happened" : "at the frame they're read in", g_offset_us / 1000.0, (void *)start, table);
        return;
    }
    hk_log("hiraku-pc: no judging routine of pop'n music's found: presses judged as the game does");
}
