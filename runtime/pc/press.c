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
//
// beatmania IIDX (9th style to EMPRESS, PLAN.md section 36) judges in whole frames: each frame
// (each frame of its chart: a frame held up is made up in a loop), for a lane pressed, its distance
// to the lane's nearest note (the frame it's read in less the note's, late positive), from a table
// of 16 lanes, 8 bytes each (a note, then the 16-bit distance; player 1's keys 1-7 and turntable,
// then player 2's), against windows of frames (EMPRESS's keys: PGREAT 0 or 1, GREAT 2 either way,
// GOOD 7, BAD 15). A press is read in the frame after it happened, so the game's judging is right
// when frames are read on a steady grid, as on its cabinet. With HK_PC_PRESS_TIME the distance is
// taken back by the frames between the press and its frame's start (hiraku-pc's go, link.h): a
// press counts in the frame it happened in whenever the game read it (a frame held up, or the
// frames caught up after one: core.cpp). HK_PC_JUDGE_OFFSET moves every press by that many
// milliseconds, by whole frames from each press's own time: part of a frame on average. The
// turntable is a position, not presses: the offset only.
//
// Every game of the family has that table and that judging, each compiled its own way (EMPRESS
// reads a lane's distance once, after its press; DJ TROOPERS reads every lane's first; 9th style to
// DistorteD walk a pointer along the table), so the judging routine's one call is made a call to
// iidx_judge here, which moves the pressed lanes' distances in the table, judges, and puts back
// any the game left as moved. The table is found by the routine that empties it (each lane's note
// 0, its distance 1024: mov [eax-4], edx; mov dword [eax], 0x400; add eax, 8; dec ecx; jnz; cmp
// eax, END), the judging as the one routine with one call that uses both the table and the lanes'
// pad bits (16 words: keys 1-7 one bit each, the turntable 0x300000, then player 2's).
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
static uint32_t press_age_of(uint32_t bit) {
    struct hk_pc_link *l = hk_link();
    uint32_t now;
    if (!hk_host_us(&now)) return 0;
    const uint32_t end = l->pad_ev_seen;
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
static uint32_t press_age(int button) { return press_age_of(HK_PC_PAD_BUTTON(button)); }

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

// --- IIDX

static const uint8_t *g_iidx_table;   // the lanes' distances (8 bytes a lane, the distance at 4)
static uint8_t g_tt[2];
static uint32_t g_tt_moved[2];        // hiraku-pc's time a turntable last moved, 0: not yet
static int g_tt_read[2];

void hk_turntable_seen(int player, uint8_t position) {
    uint32_t now;
    player &= 1;
    if (g_tt_read[player] && position != g_tt[player] && hk_host_us(&now)) g_tt_moved[player] = now ? now : 1;
    g_tt[player] = position, g_tt_read[player] = 1;
}

int16_t __cdecl hk_iidx_distance(uint32_t lane) {
    const int16_t d = *(const int16_t *)(g_iidx_table + (lane & 15) * 8 + 4);
    if (d >= 1024 || d <= -1024) return d;   // no note in the lane (1024)
    struct hk_pc_link *l = hk_link();
    const uint32_t key = lane & 7, player = lane >> 3 & 1;
    // a player's press, not the game's own (its demo's, auto scratch): a key's press just read
    // (link.h: pad_ev), a turntable moved in the last 50 ms; else as the game judges it
    int32_t us = g_offset_us;
    uint32_t now;
    if (!hk_host_us(&now)) return d;
    if (key < 7) {
        const uint32_t age = press_age_of(HK_PC_C02_KEY(player + 1, key + 1));
        if (!age) return d;
        // how long before its frame began (hiraku-pc's go) the press happened (after it: negative)
        if (g_press_time) us += (int32_t)age - (int32_t)(now - l->go_seen_host_us);
    } else if (!g_tt_moved[player] || now - g_tt_moved[player] > 50000) {
        return d;
    }
    const int32_t frame_us = l->game_hz_milli ? (int32_t)(1000000000u / l->game_hz_milli) : 16667;
    int32_t frames = us >= 0 ? us / frame_us : -((-us + frame_us - 1) / frame_us);   // floor
    const int32_t most = 15 * hk_chart_scale;   // (the chart's frames: shorter ones with a high frame rate's mod)
    if (frames > most) frames = most;
    if (frames < -most) frames = -most;
    static int log = -1;
    if (log < 0) log = getenv("HK_PC_JUDGE_LOG") != NULL;
    if (log && frames) hk_log("judge: lane %u, %d frames from its note, %d frames earlier (%d us)", lane, d, frames, us);
    return (int16_t)(d - frames);
}

// The pressed lanes' distances moved in the table for the judging, and put back after it
static int16_t g_iidx_was[16], g_iidx_set[16];
static uint16_t g_iidx_moved;   // a bit a lane
void *hk_iidx_judge_fn;          // the game's judging
uint32_t hk_iidx_ret;            // its caller's return (the game's drawing thread only judges)

// A distance in a high frame rate's shorter chart frames (hk_chart_scale to a cabinet frame) as the
// cabinet's frames: the frame it falls in (floor), so every window the game judges by (all in its own
// frames: the chart's, PGREAT's 0 and 1, the turntable's one frame) is as long in time as on the cabinet
static int16_t cabinet_frames(int16_t d) {
    const int k = hk_chart_scale;
    if (k <= 1 || d >= 1024 || d <= -1024) return d;
    return (int16_t)(d >= 0 ? d / k : -((-d + k - 1) / k));
}

void __cdecl hk_iidx_before(void) {
    for (uint32_t lane = 0; lane < 16; lane++) {
        int16_t *d = (int16_t *)(g_iidx_table + lane * 8 + 4);
        const int16_t was = *d, set = cabinet_frames(hk_iidx_distance(lane));
        if (set == was) continue;
        g_iidx_was[lane] = was, g_iidx_set[lane] = set, g_iidx_moved |= (uint16_t)(1u << lane);
        *d = set;
    }
}

void __cdecl hk_iidx_after(void) {
    for (uint32_t lane = 0; g_iidx_moved && lane < 16; lane++) {
        if (!(g_iidx_moved & 1u << lane)) continue;
        int16_t *d = (int16_t *)(g_iidx_table + lane * 8 + 4);
        if (*d == g_iidx_set[lane]) *d = g_iidx_was[lane];   // else the game set it anew
        g_iidx_moved &= (uint16_t)~(1u << lane);
    }
}

// In place of the judging's call: every register and the stack as the game's caller left them
// (9th style to RED pass an argument on the stack; the routines may take registers too)
__attribute__((naked)) static void iidx_judge(void) {
    __asm__ volatile(
        "pushal\n\t"
        "call _hk_iidx_before\n\t"
        "popal\n\t"
        "popl _hk_iidx_ret\n\t"
        "call *_hk_iidx_judge_fn\n\t"
        "pushl _hk_iidx_ret\n\t"
        "pushal\n\t"
        "call _hk_iidx_after\n\t"
        "popal\n\t"
        "ret\n\t");
}

// A routine's start and end in the program's image: a 16-byte boundary after a ret (and any
// padding of int3s or nops)
static int after_ret(const uint8_t *s) {
    const uint8_t *j = s - 1;
    while (*j == 0xcc || *j == 0x90) j--;
    return *j == 0xc3 || j[-2] == 0xc2;
}
static const uint8_t *iidx_start(const uint8_t *lo, const uint8_t *p) {
    for (const uint8_t *s = p; s > p - 0x1000 && s - 4 > lo; s--)
        if (!((uintptr_t)s & 15) && (s[-1] == 0xcc || s[-1] == 0x90 || s[-1] == 0xc3) && after_ret(s)) return s;
    return NULL;
}
static const uint8_t *iidx_end(const uint8_t *s, const uint8_t *hi) {
    for (const uint8_t *e = s + 16; e < s + 0x2000 && e < hi; e += 16)
        if ((e[-1] == 0xcc || e[-1] == 0x90 || e[-1] == 0xc3) && after_ret(e)) return e;
    return s + 0x2000 < hi ? s + 0x2000 : hi;
}
static int has_word(const uint8_t *p, const uint8_t *e, uint32_t w) {
    for (; p + 4 <= e; p++)
        if (!memcmp(p, &w, 4)) return 1;
    return 0;
}

// The table, the judging and its call, as above; 1 if found and the call made iidx_judge's
static int iidx_patch(char *base, IMAGE_NT_HEADERS *nt) {
    static const uint32_t bits[16] = {1, 2, 4, 8, 0x10, 0x20, 0x40, 0x300000, 0x80, 0x100, 0x200, 0x400, 0x800, 0x1000, 0x2000, 0xc00000};
    static const uint8_t empty[] = {0x89, 0x50, 0xfc, 0xc7, 0x00, 0x00, 0x04, 0x00, 0x00, 0x83, 0xc0, 0x08, 0x49, 0x75};
    IMAGE_SECTION_HEADER *const first = IMAGE_FIRST_SECTION(nt);
    const WORD n = nt->FileHeader.NumberOfSections;
    uint32_t table = 0, masks[4];
    int found = 0, nmasks = 0;
    for (WORD s = 0; s < n; s++) {
        const uint8_t *code = (uint8_t *)base + first[s].VirtualAddress;
        const size_t size = first[s].Misc.VirtualSize;
        for (size_t i = 0; i + 64 <= size; i++) {
            if (!memcmp(code + i, empty, sizeof empty) && code[i + 15] == 0x3d) {
                memcpy(&table, code + i + 16, 4), found++;
            }
            if (!memcmp(code + i, bits, 64) && nmasks < 4) masks[nmasks++] = (uint32_t)(uintptr_t)(code + i);
        }
    }
    if (found != 1 || !nmasks) return 0;
    table -= 0x84;   // the lanes' end, from their first distance
    const uint8_t *judge = NULL;
    uint8_t *call = NULL;
    found = 0;
    for (WORD s = 0; s < n; s++) {
        if (!(first[s].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const uint8_t *code = (uint8_t *)base + first[s].VirtualAddress, *hi = code + first[s].Misc.VirtualSize;
        const uint8_t *last = NULL;
        for (const uint8_t *p = code; p + 4 <= hi; p++) {
            int m = 0;
            while (m < nmasks && memcmp(p, &masks[m], 4)) m++;
            if (m == nmasks) continue;
            const uint8_t *st = iidx_start(code, p);
            if (!st || st == last) continue;
            last = st;
            const uint8_t *en = iidx_end(st, hi);
            if (!has_word(st, en, table) && !has_word(st, en, table + 4)) continue;
            // its direct calls, in every section of code
            uint8_t *at = NULL;
            int calls = 0;
            for (WORD t = 0; t < n; t++) {
                if (!(first[t].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
                uint8_t *c = (uint8_t *)base + first[t].VirtualAddress;
                const size_t size = first[t].Misc.VirtualSize;
                for (size_t i = 0; i + 5 <= size; i++) {
                    int32_t rel;
                    memcpy(&rel, c + i + 1, 4);
                    if (c[i] == 0xe8 && c + i + 5 + rel == st) at = c + i, calls++;
                }
            }
            if (calls == 1) judge = st, call = at, found++;
        }
    }
    if (found != 1) {
        hk_log("hiraku-pc: IIDX's lanes at %08x, but %d judging routines: presses judged as the game does", (unsigned)table, found);
        return 1;
    }
    g_iidx_table = (const uint8_t *)(uintptr_t)table;
    hk_iidx_judge_fn = (void *)judge;
    const int32_t rel = (int32_t)((uint8_t *)(void *)iidx_judge - (call + 5));
    DWORD old;
    VirtualProtect(call + 1, 4, PAGE_EXECUTE_READWRITE, &old);
    memcpy(call + 1, &rel, 4);
    VirtualProtect(call + 1, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), call, 5);
    hk_log("hiraku-pc: IIDX presses judged %s, then %.1f ms earlier (by whole frames%s; the lanes' distances at %08x, "
           "the judging at %p, its call at %p)",
           g_press_time ? "by when they happened" : "at the frame they're read in", g_offset_us / 1000.0,
           hk_chart_scale > 1 ? ", the cabinet's: the charts' shorter ones taken back to them" : "", (unsigned)table, (void *)judge, (void *)call);
    return 1;
}

void hk_press_patch_program(void) {
    const char *pt = getenv("HK_PC_PRESS_TIME"), *off = getenv("HK_PC_JUDGE_OFFSET");
    g_press_time = pt && *pt && *pt != '0';
    g_offset_us = off && *off ? (int32_t)(atof(off) * 1000) : 0;
    if (!g_press_time && !g_offset_us && hk_chart_scale <= 1) return;
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
    if (iidx_patch(base, nt)) return;
    hk_log("hiraku-pc: no judging routine of pop'n music's or IIDX's found: presses judged as the game does");
}
