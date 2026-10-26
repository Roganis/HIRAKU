// SPDX-License-Identifier: GPL-3.0-or-later
// The game's music and chart tables made bigger (PLAN.md section 35, step 5b's second part), for
// the songs past the music table's end (the console games' songs, players' own) and the charts
// past its chart table's: songs.c fills them. After popnhax's database patcher (GPL-3.0; its
// make_omnimix_patch and patch_database): the tables copied into bigger ones, the code that reads
// them pointed at the copies, and the code sized by the song or chart count grown with it.
//
// What the code holds that changes, found by walking popn22.dll's code (x86.c) for each
// instruction's address and number fields:
//   - an address in the music table (172 bytes a song) or the chart table (32 bytes a chart): the
//     same place in the copy;
//   - the song count N or N-1, the chart count or it -1, as an instruction's number: the new count;
//   - the sizes of the things kept per song, as popnhax has them (base 1780 songs, the size a song
//     adds): 4N and 4N-1, 0x18N, 6N-1, the charts' records (0x18 a chart, 0x90 a song), and four
//     lists of 0x48 a song (0x120 for the four, 0x440 with the array of 0xc8 four times a song after
//     them). popnhax's 0x5370 + 0xc a song is left out: in this game that number is a field of an
//     unrelated object;
//   - the player's record (one object: the songs' records, 0x3d0 each, then the charts' (0x9c), then
//     the characters' (0x2a0), each array followed by its own fields): popnhax moves the arrays in
//     its constructor only; here every offset past the songs' array moves by what it grew, and those
//     past the charts' array by what that grew too, and the arrays' sizes (the code clearing them)
//     are the new ones;
//   - the list of songs to choose from at random (two bytes a song, a static array): a bigger one.
// And the code that tells a chart's file format (popnhax's hook of the same place): a chart from the
// music data is in the old format, unless it has hold notes or says otherwise (songs.c's).
#include "pc32.h"
#include "x86.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RECORD 172u
#define CHART 32u
#define BEFORE 85u   // the songs before song 0 the game's id check lets through (-84..-1): copied too

static uint32_t u32(const BYTE *p) { uint32_t v; memcpy(&v, p, 4); return v; }

// --- the patterns: 0x100 any byte. Each in the code once, or not at all
#define X 0x100
static BYTE *g_text, *g_text_end;
static BYTE *find(const short *pat, size_t n) {
    BYTE *hit = NULL;
    int count = 0;
    for (BYTE *p = g_text; p + n <= g_text_end; p++) {
        size_t k = 0;
        while (k < n && (pat[k] == X || p[k] == pat[k])) k++;
        if (k == n) hit = p, count++;
    }
    return count == 1 ? hit : NULL;
}

// the player's record: its songs' array (lea esi,[edi+A]; mov ebx,N-1; mov ecx,esi; call; add esi,
// 0x3d0; sub ebx,1; jns), the charts' and characters' after it, and its size (push SIZE; call new;
// add esp,4; mov [esp+0x14],eax; mov dword [esp+0x20],1)
struct record {
    uint32_t songs_at, charts_at, charas_at, ncharas, size;
};
static int find_record(unsigned songs, unsigned charts, struct record *r) {
    static const short loop[] = {0x8d, 0xb7, X, X, X, X, 0xbb, X, X, X, X, 0x8b, 0xce, 0xe8, X, X, X, X, 0x81, 0xc6, X, X, X, X, 0x83, 0xeb, 0x01, 0x79};
    static const short alloc[] = {0x68, X, X, X, X, 0xe8, X, X, X, X, 0x83, 0xc4, 0x04, 0x89, 0x44, 0x24, 0x14, 0xc7, 0x44, 0x24, 0x20, 0x01, 0, 0, 0};
    short pat[sizeof loop / sizeof *loop];
    memcpy(pat, loop, sizeof pat);
    const uint32_t stride[3] = {0x3d0, 0x9c, 0x2a0};
    uint32_t at[3], count[3];
    for (int i = 0; i < 3; i++) {
        for (int k = 0; k < 4; k++) pat[20 + k] = (short)((stride[i] >> (8 * k)) & 0xff);
        if (i < 2) {
            const uint32_t n = (i ? charts : songs) - 1;
            for (int k = 0; k < 4; k++) pat[7 + k] = (short)((n >> (8 * k)) & 0xff);
        } else {
            for (int k = 0; k < 4; k++) pat[7 + k] = X;
        }
        const BYTE *p = find(pat, sizeof pat / sizeof *pat);
        if (!p) return 0;
        at[i] = u32(p + 2), count[i] = u32(p + 7) + 1;
    }
    const BYTE *a = find(alloc, sizeof alloc / sizeof *alloc);
    if (!a) return 0;
    r->songs_at = at[0], r->charts_at = at[1], r->charas_at = at[2], r->ncharas = count[2], r->size = u32(a + 1);
    // in order, each array's fields after it (at most 0x40 bytes), the size past the characters'
    return at[0] + songs * 0x3d0 <= at[1] && at[1] - (at[0] + songs * 0x3d0) <= 0x40 && at[1] + charts * 0x9c <= at[2] &&
           at[2] - (at[1] + charts * 0x9c) <= 0x40 && at[2] + count[2] * 0x2a0 <= r->size && r->size - (at[2] + count[2] * 0x2a0) < 0x1000;
}

// --- the chart format check (mov eax,esi; call; cmp eax,0x18; jge; push esi; mov al,bl; call; add
// esp,4; cmp eax,N; jge): the song in si, the chart (0-5) in bl, al 1 for the old format
static signed char *g_format;   // by chart: -1 the game's rule, 0 the new format, 1 the old
static unsigned g_nformat;
static const BYTE *g_music_now;
void *pc32_format_call, *pc32_format_back;
int pc32_format_of(unsigned song, unsigned chart);
int pc32_format_of(unsigned song, unsigned chart) {
    if (chart >= 6 || !g_format) return -1;
    uint16_t c;
    memcpy(&c, g_music_now + (size_t)(int16_t)song * RECORD + 50 + 2 * chart, 2);
    return c < g_nformat ? g_format[c] : -1;
}
void pc32_format_hook(void);
__asm__(".intel_syntax noprefix\n"
        ".globl _pc32_format_hook\n"
        "_pc32_format_hook:\n"
        "  movzx eax, bl\n"
        "  push eax\n"
        "  movzx eax, si\n"
        "  push eax\n"
        "  call _pc32_format_of\n"
        "  add esp, 8\n"
        "  test eax, eax\n"
        "  js 1f\n"
        "  ret\n"
        "1:\n"
        "  mov eax, esi\n"
        "  call dword ptr [_pc32_format_call]\n"
        "  jmp dword ptr [_pc32_format_back]\n"
        ".att_syntax prefix\n");
void pc32_chart_format(unsigned chart, int old) {
    if (g_format && chart < g_nformat) g_format[chart] = (signed char)(old ? 1 : 0);
}

// --- what changes: a field (where, its size) and its new value
struct patch {
    BYTE *at;
    uint32_t value;
};
static struct patch *g_patches;
static size_t g_npatches, g_cap;
static int g_short;   // out of memory: a place not kept (nothing is changed then)
static void add(BYTE *at, uint32_t value) {
    if (g_npatches == g_cap) {
        struct patch *more = realloc(g_patches, sizeof *more * (g_cap ? g_cap * 2 : 1024));
        if (!more) {
            g_short = 1;
            return;
        }
        g_patches = more, g_cap = g_cap ? g_cap * 2 : 1024;
    }
    g_patches[g_npatches++] = (struct patch){at, value};
}

int pc32_tables_grow(HMODULE game, BYTE *music, unsigned songs, unsigned new_songs, BYTE *charts, unsigned nchart, unsigned new_nchart,
                     BYTE **music_out, BYTE **charts_out) {
    BYTE *base = (BYTE *)game;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++)
        if (sec->Characteristics & IMAGE_SCN_CNT_CODE) {
            g_text = base + sec->VirtualAddress, g_text_end = g_text + sec->Misc.VirtualSize;
            break;
        }
    if (!g_text || new_songs < songs || new_nchart < nchart) return 0;
    struct record rec;
    if (!find_record(songs, nchart, &rec)) {
        pc32_log("tables: the player's record not found (songs %u, charts %u): the tables stay as they are", songs, nchart);
        return 0;
    }
    static const short format_pat[] = {0x8b, 0xc6, 0xe8, X, X, X, X, 0x83, 0xf8, X, 0x7d, X, 0x56, 0x8a, 0xc3, 0xe8, X, X, X, X, 0x83, 0xc4, 0x04, 0x3d};
    BYTE *format = find(format_pat, sizeof format_pat / sizeof *format_pat);
    // the random list: push 2N; xor ebp,ebp; push ebp; push LIST; call (memset)
    const uint32_t list_size = 2 * songs;
    const short list_pat[] = {0x68, list_size & 0xff, (list_size >> 8) & 0xff, (list_size >> 16) & 0xff, list_size >> 24, 0x33, 0xed, 0x55, 0x68, X, X, X, X, 0xe8};
    const BYTE *list_at = find(list_pat, sizeof list_pat / sizeof *list_pat);
    const uint32_t list = list_at ? u32(list_at + 9) : 0;

    // the copies: the songs before song 0 too; songs and charts past the old ones as a song the game
    // left out and its chart 0
    BYTE *m = calloc(BEFORE + new_songs, RECORD), *c = calloc(new_nchart, CHART);
    BYTE *l = list ? calloc(2 * new_songs + 2, 1) : NULL;
    signed char *format_of = malloc(new_nchart);
    if (!m || !c || (list && !l) || !format_of) {
        free(m), free(c), free(l), free(format_of);
        return 0;
    }
    memset(format_of, -1, new_nchart);
    memcpy(m, music - BEFORE * RECORD, (BEFORE + songs) * RECORD);
    unsigned left_out = 1;   // the game's first song left out, the songs past its own like it
    for (uint32_t mask; left_out < songs && (memcpy(&mask, music + left_out * RECORD + 28, 4), !(mask & 0x80000000u));) left_out++;
    if (left_out == songs) {
        free(m), free(c), free(l), free(format_of);
        return 0;
    }
    for (unsigned id = songs; id < new_songs; id++) memcpy(m + (BEFORE + id) * RECORD, music + left_out * RECORD, RECORD);
    memcpy(c, charts, (size_t)nchart * CHART);
    for (unsigned j = nchart; j < new_nchart; j++) memcpy(c + (size_t)j * CHART, charts, CHART);
    BYTE *const new_music = m + BEFORE * RECORD;
    if (l) memcpy(l, (BYTE *)(uintptr_t)list - 2, list_size + 2);

    // what the code holds
    const uint32_t M = (uint32_t)(uintptr_t)music, C = (uint32_t)(uintptr_t)charts, dm = new_songs - songs, dc = new_nchart - nchart;
    const uint32_t music_end = rec.songs_at + songs * 0x3d0, charts_end = rec.charts_at + nchart * 0x9c,
                   charas_end = rec.charas_at + rec.ncharas * 0x2a0;
    struct { uint32_t base, k; } SIZES[] = {   // popnhax's, by its base of 1780 songs
        {0x1BD0, 4}, {0x1BCF, 4}, {0xA6E0, 0x18}, {0x29B7, 6}, {0x3E944, 0x90}, {0x3E948, 0x90}, {0x1F4F4, 0x48},
        {0x1F4C0, 0x48}, {0x1F4F0, 0x48}, {0x7D3D8, 0x120}, {0x7D3D4, 0x120}, {0x1D8E58, 0x440}, {0x1D9188, 0x440}};
    unsigned counts[8] = {0};   // by kind, for the log: table, count, size, record, list
    for (BYTE *p = g_text; p < g_text_end;) {
        struct x86_insn in;
        if (!x86_decode(p, &in) || p + in.len > g_text_end) {
            p++;
            continue;
        }
        for (int field = 0; field < 2; field++) {
            const unsigned at = field ? in.imm_at : in.disp_at, size = field ? in.imm_size : in.disp_size;
            if (size != 4 || (field && in.relative)) continue;
            BYTE *f = p + at;
            const uint32_t v = u32(f);
            const int stack = !field && in.base == 4;   // [esp+..]: a local (ebp is often an object's pointer)
            if (v >= M && v - M < songs * RECORD) add(f, (uint32_t)(uintptr_t)new_music + (v - M)), counts[0]++;
            else if (v >= C && v - C < nchart * CHART) add(f, (uint32_t)(uintptr_t)c + (v - C)), counts[0]++;
            else if (l && v >= list - 2 && v < list + list_size) add(f, (uint32_t)(uintptr_t)l + 2 + (v - list)), counts[4]++;
            else if (field && list && v == list_size) add(f, 2 * new_songs), counts[4]++;
            else if (field && (v == songs || v == songs - 1)) add(f, v + dm), counts[1]++;
            else if (field && (v == nchart || v == nchart - 1)) add(f, v + dc), counts[1]++;
            else if (field && v == songs * 0x3d0) add(f, new_songs * 0x3d0), counts[3]++;
            else if (field && v == nchart * 0x9c) add(f, new_nchart * 0x9c), counts[3]++;
            else if (!stack && v >= music_end && v < rec.charts_at + 0x9c) add(f, v + dm * 0x3d0), counts[3]++;
            else if (!stack && ((v >= charts_end && v < rec.charas_at + 0x2a0) || (v >= charas_end && v <= rec.size)))
                add(f, v + dm * 0x3d0 + dc * 0x9c), counts[3]++;
            else if (!stack)
                for (size_t k = 0; k < sizeof SIZES / sizeof *SIZES; k++)
                    if (v == SIZES[k].base + (songs - 1780) * SIZES[k].k) {
                        add(f, v + dm * SIZES[k].k), counts[2]++;
                        break;
                    }
        }
        p += in.len;
    }
    DWORD old;
    if (g_short || g_npatches < 40 || !counts[0] || !counts[1] || !counts[3] ||
        !VirtualProtect(g_text, (size_t)(g_text_end - g_text), PAGE_EXECUTE_READWRITE, &old)) {
        pc32_log("tables: too little of the code found (%zu places): the tables stay as they are", g_npatches);
        free(m), free(c), free(l), free(format_of), free(g_patches);
        g_patches = NULL, g_npatches = g_cap = 0;
        return 0;
    }
    for (size_t i = 0; i < g_npatches; i++) {
        if (pc32_trace()) pc32_log("tables: %p %08x -> %08x", (void *)g_patches[i].at, u32(g_patches[i].at), g_patches[i].value);
        memcpy(g_patches[i].at, &g_patches[i].value, 4);
    }
    if (format) {   // jmp to the hook (the first 7 bytes: mov eax,esi; call)
        pc32_format_call = format + 7 + (int32_t)u32(format + 3), pc32_format_back = format + 7;
        format[0] = 0xe9;
        const int32_t rel = (int32_t)((BYTE *)pc32_format_hook - (format + 5));
        memcpy(format + 1, &rel, 4);
        format[5] = format[6] = 0x90;
    }
    VirtualProtect(g_text, (size_t)(g_text_end - g_text), old, &old);
    FlushInstructionCache(GetCurrentProcess(), g_text, (size_t)(g_text_end - g_text));
    g_music_now = new_music, g_format = format_of, g_nformat = new_nchart;
    *music_out = new_music, *charts_out = c;
    pc32_log("tables: songs %u -> %u, charts %u -> %u: %u places in the code for the tables, %u counts, %u sizes, %u in the player's "
             "record, %u for the random list%s%s",
             songs, new_songs, nchart, new_nchart, counts[0], counts[1], counts[2], counts[3], counts[4], list ? "" : " (not found)",
             format ? "" : "; the chart format check not found");
    free(g_patches), g_patches = NULL, g_npatches = g_cap = 0;
    return 1;
}
