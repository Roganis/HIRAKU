// SPDX-License-Identifier: GPL-3.0-or-later
// The 64-bit PC pop'n's music and chart tables made bigger (PLAN.md section 34: songs past the music
// table), as runtime/pc32/tables.c does for the 32-bit game: the tables copied into bigger ones and
// the code that reads them pointed at the copies.
//
// The code is walked function by function (the image's .pdata lists them; some carry their jump
// tables, which the walk stops at) with runtime/pc32/x86.c's 64-bit decoding, for each
// instruction's address field:
//   - [rip + displacement] reaching into a table: the same place in its copy (the copies within 2 GB
//     of the code, as the displacement has 32 bits);
//   - [base + index + displacement] whose displacement is an address in a table counted from the
//     image's start (the base register holds the image's start: MSVC's way with big static arrays):
//     the copy's, counted the same way (the copies within 2 GB above the image).
// Nothing in the image's data points into the tables (its relocations: none). The music table's
// copy starts 85 songs early: the id check lets ids -85..-1 through (lea eax,[rcx+0x55]; cmp ax,dx),
// and those read what lies before the table (other variables), copied with it as it is then. The
// code that names those variables keeps the originals.
#include "pc64.h"
#include "../pc32/x86.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define RECORD 312u
#define CHART 40u
#define BEFORE 85u
#define X 0x100   // a pattern's any byte

struct patch {
    BYTE *at;   // a displacement field (4 bytes)
    int32_t value;
};

// The image's functions (.pdata: begin, end, unwind RVAs)
static const uint32_t *functions(BYTE *base, size_t *n) {
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    *n = dir.Size / 12;
    return dir.VirtualAddress ? (const uint32_t *)(base + dir.VirtualAddress) : NULL;
}

// Memory for the copies after the image, within 2 GB of it
static BYTE *near_image(BYTE *base, size_t size) {
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    for (uintptr_t a = ((uintptr_t)base + nt->OptionalHeader.SizeOfImage + 0xffff) & ~(uintptr_t)0xffff;
         a + size < (uintptr_t)base + 0x7f000000u; a += 0x10000) {
        void *p = VirtualAlloc((void *)a, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (p) return p;
    }
    return NULL;
}

int pc64_tables_move(HMODULE game, BYTE **music, unsigned songs, unsigned new_songs, BYTE **charts, unsigned nchart, unsigned new_nchart) {
    BYTE *base = (BYTE *)game;
    size_t nf;
    const uint32_t *f = functions(base, &nf);
    if (!f || new_songs < songs || new_nchart < nchart) return 0;
    // the copies: the music table (and the 85 songs' worth before it), the chart table
    const size_t music_size = (size_t)(BEFORE + new_songs) * RECORD, chart_size = (size_t)new_nchart * CHART;
    BYTE *mem = near_image(base, music_size + chart_size);
    if (!mem) {
        pc64_log("songs: no memory near the game for its tables: they stay as they are");
        return 0;
    }
    BYTE *new_music = mem + (size_t)BEFORE * RECORD, *new_charts = mem + music_size;
    BYTE *const old_music = *music, *const old_charts = *charts;
    BYTE *const music_lo = old_music - (size_t)BEFORE * RECORD, *const music_hi = old_music + (size_t)songs * RECORD;
    BYTE *const chart_hi = old_charts + (size_t)nchart * CHART;
    // where each old place goes: NULL if it's in neither table
    struct patch *patches = NULL;
    size_t np = 0, cap = 0, rip = 0, rel = 0, short_of = 0;
    for (size_t i = 0; i < nf; i++) {
        BYTE *p = base + f[3 * i], *end = base + f[3 * i + 1];
        while (p < end) {
            struct x86_insn in;
            const int len = x86_decode64(p, &in);
            if (!len) break;   // a jump table in the function: the rest of it is data
            if (in.disp_size == 4) {
                int32_t disp;
                memcpy(&disp, p + in.disp_at, 4);
                BYTE *target = NULL, *moved = NULL;
                if (in.rip) target = p + len + disp;
                else if (in.base >= 0 && disp > 0) target = base + disp;   // counted from the image's start
                if (target && target >= old_music && target < music_hi) moved = new_music + (target - old_music);
                else if (target && target >= old_charts && target < chart_hi) moved = new_charts + (target - old_charts);
                if (moved) {
                    const int64_t v = in.rip ? moved - (p + len) : moved - base;
                    if (v != (int32_t)v || (!in.rip && v < 0)) {
                        short_of++;
                    } else {
                        if (np == cap && !(patches = realloc(patches, (cap = cap ? 2 * cap : 256) * sizeof *patches))) return 0;
                        patches[np++] = (struct patch){p + in.disp_at, (int32_t)v};
                        in.rip ? rip++ : rel++;
                    }
                }
            }
            p += len;
        }
    }
    if (short_of) {   // a place the copies can't be reached from: nothing changed
        pc64_log("songs: %zu places can't reach the tables' copies: they stay as they are", short_of);
        VirtualFree(mem, 0, MEM_RELEASE);
        free(patches);
        return 0;
    }
    memcpy(mem, music_lo, (size_t)(BEFORE + songs) * RECORD);
    memcpy(new_charts, old_charts, (size_t)nchart * CHART);
    for (size_t i = 0; i < np; i++) {
        DWORD old;
        VirtualProtect(patches[i].at, 4, PAGE_EXECUTE_READWRITE, &old);
        memcpy(patches[i].at, &patches[i].value, 4);
        VirtualProtect(patches[i].at, 4, old, &old);
    }
    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
    free(patches);
    pc64_log("songs: the tables moved for %u songs and %u charts (%zu places: %zu by rip, %zu by the image's start)", new_songs, new_nchart,
             rip + rel, rip, rel);
    *music = new_music, *charts = new_charts;
    return 1;
}

// --- the counts: where the code holds the song count N (or N-1, 4N, 7N, the id check's N+84), the
// chart count, and the player's best scores' object (CPlayerMusicGameData: 8 bytes, then seven
// charts' records of 32 bytes for each song, chart-major, then a field): its size, the field's place,
// the charts' strides. Each place is the 4 bytes after a pattern of the code (8 bytes or more before
// it, some bytes after), found once; all of them, or nothing changes. The places the songs past the
// table pass through: the music select's lists, the songs' availability (events, locks), the id
// and chart checks, the count getter, the start-up check of the charts' folders, the card's last
// song, the player's best scores. Left at the game's count, behind their own checks of an id: the
// server's songs and charts (musicsub, tracksub), the friends' scores, the licence list's buffer,
// the random choice (x % N by a reciprocal: the new songs aren't drawn), debug scenes, and N as
// "no song". And the player's items: as many song items (type 0, and type 8) as songs, from the
// capacities' table (0x1804e3240, read by movsxd rax,edx; lea rcx,[table]; mov eax,[rcx+rax*4]; ret):
// the card's last song is kept only when its item is (0x1800a6b50), and an item's default is owned.
enum { N, N1, N4, N7, ID, NC, BEST_AT, BEST_SIZE, SHEET1, SHEET6 };
static const struct site {
    int kind;
    unsigned char before, after;
    const char *pat, *post;
} SITES[] = {
    {N,         8,  0, "\xec\x20\x0f\xb7\xfa\x33\xdb\xbe", ""},
    {N,         8,  0, "\x8b\xcd\x48\x89\x6c\x24\x20\xba", ""},
    {N4,        8,  0, "\x00\x00\xe8\xe2\x4b\x00\x00\xba", ""},
    {N,         8,  0, "\x00\x00\x41\xff\xc5\x41\x81\xfd", ""},
    {ID,        8,  0, "\x00\x00\x75\x2c\x8d\x41\x55\xba", ""},
    {NC,        8,  0, "\x8d\x3d\x4b\xea\x68\x00\x41\xbc", ""},
    {N,         8,  0, "\x81\xc6\x38\x01\x00\x00\x81\xff", ""},
    {N,         8,  0, "\x80\x3d\xa9\xe2\xf6\x00\x00\xb8", ""},
    {NC,        8,  0, "\xcc\xcc\xb8\x7c\x04\x00\x00\xba", ""},
    {N1,        8,  5, "\xcc\xcc\xcc\xcc\xcc\xcc\xcc\xb8", "\x66\x3b\xd0\x76\x07"},
    {N1,        8,  0, "\x0f\xbf\xf2\x4c\x8b\xf1\x81\xfe", ""},
    {N1,        8,  0, "\xcc\xcc\xcc\x48\x83\xec\x28\xb8", ""},
    {N,         8,  0, "\xcb\xba\xaa\xff\xff\xff\x41\xb8", ""},
    {N1,        8,  0, "\xc0\x0f\x84\xa8\x00\x00\x00\xbe", ""},
    {N,         8,  0, "\x19\x10\xfd\xff\xff\xc3\x81\xfb", ""},
    {N,         8,  0, "\xcf\x7e\xeb\x00\xff\xc3\x81\xfb", ""},
    {N,         8,  0, "\xb7\x9c\x41\x7c\x29\x00\x00\xb8", ""},
    {N,         8,  0, "\xfe\xff\x48\x8b\xf8\x33\xdb\xbe", ""},
    {N,         8,  0, "\x8b\xf0\x41\x0f\xb7\xfc\x41\xbf", ""},
    {N7,        8,  0, "\x48\x89\x01\x48\x8d\x79\x08\xbb", ""},
    {N,         8,  0, "\x33\xc9\x4c\x8d\x51\x08\x41\xbb", ""},
    {N1,        8,  5, "\xcc\xcc\xcc\xcc\xcc\xcc\xcc\xb8", "\x66\x3b\xd0\x77\x24"},
    {N,         8,  0, "\x1e\x41\x0f\xb6\xc0\x4c\x69\xc0", ""},
    {N,         8,  0, "\x48\x8b\xf1\x0f\xb7\xfd\x41\xbe", ""},
    {N,         8,  0, "\x0f\xb7\xdf\x48\x8b\xe9\x41\xbe", ""},
    {N,         8,  0, "\x0f\xb7\xf7\x48\x8b\xe9\x41\xbc", ""},
    {N1,        8,  0, "\x73\xf8\xff\xff\xcc\xcc\xcc\xb8", ""},
    {N,         8,  0, "\xc0\x48\x83\xc1\x08\x4c\x69\xc0", ""},
    {N,         8,  0, "\x74\x21\x0f\xb6\xc3\x48\x69\xc8", ""},
    {N,         8,  0, "\xc3\x48\x8d\x4d\x08\x48\x69\xf8", ""},
    {N,         8,  0, "\xec\x20\x48\x8b\xf9\x33\xdb\xbe", ""},
    {N,         8,  0, "\x81\x48\x1d\x08\x00\x41\x81\xf8", ""},
    {N,         8,  0, "\x7d\x7c\x48\x63\xc2\x48\x69\xd8", ""},
    {N,         8,  0, "\x87\x48\x1d\x08\x00\x41\x81\xf8", ""},
    {BEST_AT,   8,  0, "\x48\x83\xeb\x01\x75\xee\x89\x9e", ""},
    {BEST_AT,   8,  0, "\x48\x8b\xcb\xff\x50\x58\xc7\x83", ""},
    {BEST_AT,   8,  0, "\x57\x48\x83\xec\x20\x44\x8b\x89", ""},
    {BEST_AT,   8,  0, "\x2b\xd0\x41\x8d\x41\x01\x89\x81", ""},
    {BEST_AT,   8,  0, "\xff\xff\x84\xc0\x75\x49\x8b\x8f", ""},
    {BEST_AT,   8,  0, "\x07\x2b\xd0\x8d\x41\x01\x89\x87", ""},
    {BEST_AT,   8,  0, "\xc4\x20\x5f\xc3\xcc\xcc\xc7\x81", ""},
    {SHEET1,    8,  0, "\xff\xff\x41\xfe\xc0\x48\x81\xc1", ""},
    {SHEET6,    8,  0, "\xcb\x48\xc1\xe1\x05\x48\x81\xc1", ""},
    {BEST_SIZE,  8,  0, "\x48\x89\x05\x48\xc0\xf4\x00\xb9", ""},
    {BEST_SIZE,  8,  0, "\x48\x83\xec\x20\x48\x8b\xea\xba", ""},
};
#define NSITES (sizeof SITES / sizeof *SITES)

static uint32_t count_value(int kind, uint32_t n, uint32_t nc) {
    switch (kind) {
    case N: return n;
    case N1: return n - 1;
    case N4: return 4 * n;
    case N7: return 7 * n;
    case ID: return n + 84;
    case NC: return nc;
    case BEST_AT: return 7 * n * 32 + 8;
    case BEST_SIZE: return 7 * n * 32 + 16;
    case SHEET1: return n * 32;
    default: return 6 * n * 32 + 8;   // SHEET6
    }
}

// The code section's place in the image
static BYTE *code_range(BYTE *base, BYTE **end) {
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (sec[i].Characteristics & IMAGE_SCN_CNT_CODE) {
            *end = base + sec[i].VirtualAddress + sec[i].Misc.VirtualSize;
            return base + sec[i].VirtualAddress;
        }
    return NULL;
}

// A site's field: after its pattern, the old value, then its bytes after; found once, or NULL
static BYTE *find_site(BYTE *code, BYTE *end, const struct site *s, uint32_t old) {
    BYTE *hit = NULL;
    int count = 0;
    for (BYTE *p = code; p + s->before + 4 + s->after <= end; p++)
        if (!memcmp(p, s->pat, s->before) && !memcmp(p + s->before, &old, 4) && !memcmp(p + s->before + 4, s->post, s->after))
            hit = p + s->before, count++;
    return count == 1 ? hit : NULL;
}

// The items' capacities by type (nine of them), or NULL
static uint32_t *item_capacities(BYTE *code, BYTE *end) {
    static const BYTE pat[] = {0x48, 0x63, 0xc2, 0x48, 0x8d, 0x0d};   // movsxd rax,edx; lea rcx,[rip+X]
    static const BYTE post[] = {0x8b, 0x04, 0x81, 0xc3};             // mov eax,[rcx+rax*4]; ret
    uint32_t *hit = NULL;
    int count = 0;
    for (BYTE *p = code; p + 14 <= end; p++)
        if (!memcmp(p, pat, sizeof pat) && !memcmp(p + 10, post, sizeof post)) {
            int32_t d;
            memcpy(&d, p + 6, 4);
            hit = (uint32_t *)(p + 10 + d), count++;
        }
    return count == 1 ? hit : NULL;
}

int pc64_tables_grow(HMODULE game, BYTE **music, unsigned songs, unsigned new_songs, BYTE **charts, unsigned nchart, unsigned new_nchart) {
    BYTE *base = (BYTE *)game, *end, *code = code_range(base, &end);
    BYTE *at[NSITES];
    if (!code) return 0;
    uint32_t *items = item_capacities(code, end);
    if (!items || items[0] != songs || items[8] != songs) {
        pc64_log("songs: the game's code isn't the one known (the items' capacities): no songs past its table");
        return 0;
    }
    for (size_t i = 0; i < NSITES; i++)   // (before the tables move: some patterns hold a table's address)
        if (!(at[i] = find_site(code, end, &SITES[i], count_value(SITES[i].kind, songs, nchart)))) {
            pc64_log("songs: the game's code isn't the one known (place %zu): no songs past its table", i);
            return 0;
        }
    // a song left out, the new slots' start (the music data fills those it describes)
    const BYTE *blank = NULL;
    for (unsigned id = 0; id < songs && !blank; id++) {
        uint32_t mask;
        memcpy(&mask, *music + (size_t)id * RECORD + 60, 4);
        if (mask == 0x80000000u) blank = *music + (size_t)id * RECORD;
    }
    if (!blank || !pc64_tables_move(game, music, songs, new_songs, charts, nchart, new_nchart)) return 0;
    for (unsigned id = songs; id < new_songs; id++) memcpy(*music + (size_t)id * RECORD, blank, RECORD);
    for (size_t i = 0; i < NSITES; i++) {
        const uint32_t v = count_value(SITES[i].kind, new_songs, new_nchart);
        DWORD old;
        VirtualProtect(at[i], 4, PAGE_EXECUTE_READWRITE, &old);
        memcpy(at[i], &v, 4);
        VirtualProtect(at[i], 4, old, &old);
    }
    DWORD old;
    VirtualProtect(items, 9 * 4, PAGE_READWRITE, &old);
    items[0] = items[8] = new_songs;
    VirtualProtect(items, 9 * 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
    pc64_log("songs: room for %u songs and %u charts (%zu counts in the code, the song items)", new_songs, new_nchart, (size_t)NSITES);
    return 1;
}

// --- a chart's format. The game tells an old chart (8-byte events) from a new one (12, long notes)
// by numbers (0x1801d3da0): old when the song's version is under 0x18 and its chart's number under
// 0x6dc, but 0x70b. The charts the music data adds have numbers the rule doesn't know; theirs as
// popnhax decides: old unless it has hold notes or says force_new_chart_format (songs.c). The
// function's start becomes a jump to the same rule with that exception.
static signed char *g_format;   // by chart: -1 the game's rule, 0 new, 1 old
static unsigned g_nformat;
static int (*g_version_of)(unsigned short);
static int (*g_chart_of)(unsigned short, unsigned char);
static int g_rule_version, g_rule_chart, g_rule_except;
static char chart_is_old(unsigned short song, unsigned char sheet) {
    const int chart = g_chart_of(song, sheet);
    if (chart >= 0 && (unsigned)chart < g_nformat && g_format[chart] >= 0) return (char)g_format[chart];
    return (char)(g_version_of(song) < g_rule_version && chart < g_rule_chart && chart != g_rule_except);
}
void pc64_chart_format(unsigned chart, int old) {
    if (chart < g_nformat) g_format[chart] = (signed char)(old != 0);
}
int pc64_chart_formats_install(HMODULE game, unsigned nchart) {
    BYTE *end, *code = code_range((BYTE *)game, &end);
    // mov [rsp+8],rbx; push rdi; sub rsp,0x20; movzx edi,dl; movzx ebx,cx; call VERSION; cmp eax,V; jge;
    // movzx edx,dil; movzx ecx,bx; call CHART; cmp eax,C; jge; movzx edx,dil; movzx ecx,bx; call CHART;
    // cmp eax,E
    static const short pat[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x0f, 0xb6, 0xfa, 0x0f, 0xb7, 0xd9,
                                0xe8, X, X, X, X, 0x83, 0xf8, X, 0x7d, X, 0x40, 0x0f, 0xb6, 0xd7, 0x0f, 0xb7, 0xcb,
                                0xe8, X, X, X, X, 0x3d, X, X, X, X, 0x7d, X, 0x40, 0x0f, 0xb6, 0xd7, 0x0f, 0xb7, 0xcb,
                                0xe8, X, X, X, X, 0x3d};
    const size_t n = sizeof pat / sizeof *pat;
    BYTE *hit = NULL;
    int count = 0;
    for (BYTE *p = code; code && p + n + 4 <= end; p++) {
        size_t k = 0;
        while (k < n && (pat[k] == X || p[k] == pat[k])) k++;
        if (k == n) hit = p, count++;
    }
    if (count != 1 || !(g_format = malloc(nchart))) {
        pc64_log("songs: the charts' format check not found: the music data's charts as the game tells them");
        return 0;
    }
    memset(g_format, -1, nchart);
    g_nformat = nchart;
    int32_t d;
    memcpy(&d, hit + 17, 4), g_version_of = (int (*)(unsigned short))(hit + 21 + d);
    memcpy(&d, hit + 34, 4), g_chart_of = (int (*)(unsigned short, unsigned char))(hit + 38 + d);
    g_rule_version = (signed char)hit[23];
    memcpy(&g_rule_chart, hit + 39, 4);
    memcpy(&g_rule_except, hit + n, 4);
    BYTE jump[12] = {0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xe0};   // mov rax,chart_is_old; jmp rax
    const uint64_t to = (uint64_t)(uintptr_t)chart_is_old;
    memcpy(jump + 2, &to, 8);
    DWORD old;
    VirtualProtect(hit, sizeof jump, PAGE_EXECUTE_READWRITE, &old);
    memcpy(hit, jump, sizeof jump);
    VirtualProtect(hit, sizeof jump, old, &old);
    FlushInstructionCache(GetCurrentProcess(), hit, sizeof jump);
    return 1;
}
