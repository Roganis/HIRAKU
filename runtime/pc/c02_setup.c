// SPDX-License-Identifier: GPL-3.0-or-later
// A new C02 cabinet's set-up and its movies, for beatmania IIDX RED to GOLD (PLAN.md section 36):
// started by HIRAKU's RtEffect.dll (RED, HAPPY SKY, DistorteD: rteffect.c) or, from GOLD on, which
// has no RtEffect.dll, by HIRAKU's libacio.dll (libacio.c)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <strmif.h>
#include <amvideo.h>
#include <dvdmedia.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cabinet.h"
#include "c02_setup.h"

// --- a new cabinet (as 9th style's, ezusb_c02.c): the operator settings (RED 0xc18 bytes, HAPPY SKY
// 0x202c, DistorteD 0x2270; a checksum at +0x6c) are loaded at start-up over the game's defaults, from settings.bin on
// each backup drive. With none saved yet the start-up says BACKUP DATA ERROR, and the settings' check
// (RED JAD 0x44a650, HAPPY SKY JAD 0x43de30) CLOCK ERROR: the "clock set" word (+0x68) is 0 until the
// operator's CLOCK SETUP saves it as 1. HIRAKU's cabinet comes set up: when nothing loads, the
// defaults are saved, the clock set, by the game's own routine, and the start-up goes on as if they
// had loaded. Found by shape: RED's (one each in JAA and JAD), and HAPPY SKY's, which DistorteD keeps
// with a bigger block (one each in HAPPY SKY's JAA, JAB, JAD and DistorteD's JAA...JAF).
#define SETTINGS_MAX 0x4000
static size_t g_clock_set = 0x68;   // GOLD's and DJ TROOPERS' 0x6c (clock_word)
static size_t g_display_type;       // DJ TROOPERS on: the monitor's kind, 0 unset (DISPLAY TYPE ERROR), 1 A, 2 B

static uint8_t *find(const uint8_t *pat, const uint8_t *mask, size_t n, uint8_t *from) {
    char *base = (char *)hk_program();
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    for (WORD s = 0; s < nt->FileHeader.NumberOfSections; s++, sec++) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uint8_t *code = (uint8_t *)base + sec->VirtualAddress;
        for (size_t i = 0; i + n <= sec->Misc.VirtualSize; i++) {
            if (code + i < from) continue;
            size_t k = 0;
            while (k < n && (code[i + k] & mask[k]) == (pat[k] & mask[k])) k++;
            if (k == n) return code + i;
        }
    }
    return NULL;
}

static uint32_t rel32(const uint8_t *call) { return (uint32_t)(uintptr_t)call + 5 + *(const uint32_t *)(call + 1); }

// an address in the program's code (a call's target may be anything when its bytes weren't a call)
static int in_code(const uint8_t *at) {
    char *base = (char *)hk_program();
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    for (WORD s = 0; s < nt->FileHeader.NumberOfSections; s++, sec++)
        if ((sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) && at >= (uint8_t *)base + sec->VirtualAddress &&
            at + 5 <= (uint8_t *)base + sec->VirtualAddress + sec->Misc.VirtualSize)
            return 1;
    return 0;
}

typedef int(__cdecl *load_settings_t)(void);
typedef int(__cdecl *save_settings_t)(void *);
static load_settings_t g_load_settings;
static save_settings_t g_save_settings;
static uint8_t *g_settings;
static size_t g_settings_size;

// e-AMUSEMENT's (the same in all three; RED JAD 0x44a1e0 makes their defaults): on/off (a byte),
// the shop's prefecture (a word: 0 none set, 1 Hokkaido ... 47) and name; on with no name or no
// prefecture, the operator menu says PLEASE SET SHOP NAME / PREFECTURE and turns it off
// (GOLD's 4 bytes on, as its clock word: each at the same distance before it)
#define EAMUSE_ON (g_clock_set - 0x20)
#define PREFECTURE (g_clock_set - 0x1e)
#define SHOP_NAME (g_clock_set - 0x1c)   // Shift-JIS, 未設定 when none, to the clock
#define SHOP_NAME_SIZE 26

static int __cdecl load_or_set_up(void) {
    const int r = g_load_settings();
    static uint8_t s[SETTINGS_MAX];
    memcpy(s, g_settings, g_settings_size);
    const char *why = NULL;
    if (r != 0) {
        *(uint32_t *)(s + g_clock_set) = 1;
        why = "no operator settings saved: the game's defaults saved, the clock set";
    }
    if (g_display_type && !s[g_display_type]) {   // DJ TROOPERS: DISPLAY TYPE A, the cabinet's own
        s[g_display_type] = 1;
        if (!why) why = "the display type set (A)";
    }
    // HIRAKU's arcade in Tokyo where the operator has set no shop (as 9th style's, ezusb_c02.c)
    static const char unset[] = "\x96\xa2\x90\xdd\x92\xe8";   // 未設定, Shift-JIS
    if (!s[SHOP_NAME] || !memcmp(s + SHOP_NAME, unset, sizeof unset)) {
        static const char name[] = "\x82\x67\x82\x68\x82\x71\x82\x60\x82\x6a\x82\x74";   // ＨＩＲＡＫＵ
        memset(s + SHOP_NAME, 0, SHOP_NAME_SIZE);
        memcpy(s + SHOP_NAME, name, sizeof name);
        if (!why) why = "the shop's name set";
    }
    if (*(uint16_t *)(s + PREFECTURE) == 0 || *(uint16_t *)(s + PREFECTURE) > 47) {
        *(uint16_t *)(s + PREFECTURE) = 13;   // Tokyo
        if (!why) why = "the shop's prefecture set";
    }
    if (!why) return 0;
    const int saved = g_save_settings(s);
    hk_log("hiraku-pc: C02: %s (%s)", why, saved == 0 ? "saved" : "not saved");
    return saved == 0 ? 0 : r;
}

// a game's loader and saver, by shape. `settings`: where the loader pushes the settings' address;
// `size_at`: where the loader's frame size (its settings' size) is, 0 if the shape has it; `words_at`:
// where the saver's copy count (the size in words) is, 0 if the shape has it
struct settings_shape {
    const char *game;
    size_t size;
    uint8_t loader[16], loader_m[16];
    size_t loader_n, settings, size_at;
    uint8_t saver[24], saver_m[24];
    size_t saver_n, words_at;
};

static const struct settings_shape shapes[] = {
    // RED: sub esp,0xc18; push ebx; push esi; push edi; push SETTINGS; call defaults
    //      sub esp,0xc18; mov ecx,0x306; push ebx; mov ebx,[esp+0xc20]
    {"RED", 0xc18,
     {0x81, 0xec, 0x18, 0x0c, 0x00, 0x00, 0x53, 0x56, 0x57, 0x68, 0, 0, 0, 0, 0xe8},
     {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0, 0xff}, 15, 10, 0,
     {0x81, 0xec, 0x18, 0x0c, 0x00, 0x00, 0xb9, 0x06, 0x03, 0x00, 0x00, 0x53, 0x8b, 0x9c, 0x24, 0x20, 0x0c, 0x00, 0x00},
     {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}, 19, 0},
    // HAPPY SKY (0x202c) and DistorteD (0x2270):
    //   mov eax,SIZE; call stack; push ebx; push esi; push edi; push SETTINGS; call defaults
    //   push ebp; mov ebp,esp; and esp,-8; mov eax,FRAME; call stack; push ebx; mov ebx,[ebp+8];
    //   push esi; push edi; mov ecx,SIZE/4
    {"HAPPY SKY's kind", 0,
     {0xb8, 0, 0, 0x00, 0x00, 0xe8, 0, 0, 0, 0, 0x53, 0x56, 0x57, 0x68, 0, 0},
     {0xff, 0, 0, 0xff, 0xff, 0xff, 0, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0, 0}, 14, 14, 1,
     {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8, 0xb8, 0, 0, 0x00, 0x00, 0xe8, 0, 0, 0, 0, 0x53, 0x8b, 0x5d, 0x08, 0x56, 0x57, 0xb9},
     {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0, 0, 0xff, 0xff, 0xff, 0, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}, 23, 23},
};

// the start-up's calls of the loader, its result kept in a flag (call loader; mov [F],eax), straight
// or through a jump (DistorteD's program is linked with a jump for each routine, and calls it twice)
static int loader_calls(const uint8_t *load, uint8_t **calls, int most) {
    static const uint8_t call[] = {0xe8, 0, 0, 0, 0, 0xa3};
    static const uint8_t call_m[] = {0xff, 0, 0, 0, 0, 0xff};
    int n = 0;
    for (uint8_t *at = find(call, call_m, sizeof call, NULL); at && n < most; at = find(call, call_m, sizeof call, at + 1)) {
        const uint8_t *to = (const uint8_t *)(uintptr_t)rel32(at);
        if (to == load || (in_code(to) && to[0] == 0xe9 && rel32(to) == (uint32_t)(uintptr_t)load)) calls[n++] = at;
    }
    return n;
}

// The clock word: the first field the settings' check tests (CLOCK ERROR when 0), on a copy of the
// block on its stack: `mov esi,SETTINGS; lea edi,[esp+K]; (push ...;) rep movsd; (call ...;) mov
// eax,[esp+X]; test eax,eax` (GOLD, DJ TROOPERS); else just before the checksum the saver stores
// (mov [ebx+X],eax: RED, HAPPY SKY, DistorteD, GOLD; DJ TROOPERS has a field between)
static size_t clock_word(uint32_t settings, const uint8_t *save) {
    uint8_t pat[9] = {0xbe, 0, 0, 0, 0, 0x8d, 0x7c, 0x24, 0};
    static const uint8_t mask[9] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0};
    memcpy(pat + 1, &settings, 4);
    for (uint8_t *at = find(pat, mask, 9, NULL); at; at = find(pat, mask, 9, at + 1)) {
        const unsigned k = at[8];
        int pushes = 0, i = 9;
        while (i < 14 && at[i] >= 0x50 && at[i] <= 0x57) pushes++, i++;
        if (at[i] != 0xf3 || at[i + 1] != 0xa5) continue;
        i += 2;
        if (at[i] == 0xe8) i += 5;   // a call between
        unsigned x = 0;
        if (at[i] == 0x8b && at[i + 1] == 0x44 && at[i + 2] == 0x24) x = at[i + 3], i += 4;
        else if (at[i] == 0x8b && at[i + 1] == 0x84 && at[i + 2] == 0x24) x = *(const uint32_t *)(at + i + 3), i += 7;
        else continue;
        if (at[i] == 0x83 && at[i + 1] == 0xc4) i += 3;   // the call's arguments dropped
        if (at[i] != 0x85 || at[i + 1] != 0xc0) continue;
        const unsigned off = x - k - 4 * (unsigned)pushes;
        if (off < 0x60 || off >= 0x80) continue;
        // DJ TROOPERS then tests the display type: mov al,[esp+Y]; test al,al; jne; mov eax,2
        for (int j = i; j < i + 32; j++)
            if (at[j] == 0x8a && at[j + 1] == 0x44 && at[j + 2] == 0x24 && at[j + 4] == 0x84 && at[j + 5] == 0xc0 &&
                at[j + 6] == 0x75 && at[j + 8] == 0xb8 && at[j + 9] == 2 && !at[j + 10] && at[j + 3] > k)
                g_display_type = at[j + 3] - k;
        return off;
    }
    for (int k = 0; k < 96; k++)
        if (save[k] == 0x89 && save[k + 1] == 0x43 && save[k + 2] >= 0x6c && save[k + 2] < 0x80) return (size_t)save[k + 2] - 4;
    return 0x68;
}

static void call_instead(uint8_t *call) {
    const int32_t to = (int32_t)((uintptr_t)load_or_set_up - ((uintptr_t)call + 5));
    DWORD old;
    VirtualProtect(call + 1, 4, PAGE_EXECUTE_READWRITE, &old);
    memcpy(call + 1, &to, 4);
    VirtualProtect(call + 1, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), call, 5);
}

void hk_c02_set_up_cabinet(void) {
    for (size_t i = 0; i < sizeof shapes / sizeof shapes[0]; i++) {
        const struct settings_shape *g = &shapes[i];
        // the loader: the one whose size the saver copies and that the start-up calls
        for (uint8_t *load = find(g->loader, g->loader_m, g->loader_n, NULL); load;
             load = find(g->loader, g->loader_m, g->loader_n, load + 1)) {
            const size_t size = g->size_at ? *(const uint32_t *)(load + g->size_at) : g->size;
            if (size > SETTINGS_MAX || size <= 0x80) continue;
            uint8_t *save = NULL;
            for (uint8_t *at = find(g->saver, g->saver_m, g->saver_n, NULL); at && !save;
                 at = find(g->saver, g->saver_m, g->saver_n, at + 1))
                if (!g->words_at || *(const uint32_t *)(at + g->words_at) * 4 == size) save = at;
            uint8_t *calls[4];
            const int n = save ? loader_calls(load, calls, 4) : 0;
            if (!n) continue;
            g_clock_set = clock_word((uint32_t)(uintptr_t)*(const uint32_t *)(load + g->settings), save);
            g_load_settings = (load_settings_t)(void *)load;
            g_save_settings = (save_settings_t)(void *)save;
            g_settings = (uint8_t *)(uintptr_t)*(const uint32_t *)(load + g->settings);
            g_settings_size = size;
            for (int k = 0; k < n; k++) call_instead(calls[k]);
            hk_log("hiraku-pc: C02: the operator settings' loader found (%s, 0x%x bytes, the clock at +0x%x, called from %d place%s)",
                   g->game, (unsigned)size, (unsigned)g_clock_set, n, n == 1 ? "" : "s");
            return;
        }
    }
    hk_log("hiraku-pc: C02: the operator settings' loader not found: a new cabinet stops at BACKUP DATA and CLOCK ERROR");
}

// --- the movies (HAPPY SKY and DistorteD): the game plays them through DirectShow into a renderer of
// its own ("TEXTURERENDERER"), which takes RGB24 described by a VIDEOINFOHEADER2 only (the
// cabinet's CyberLink MPEG-2 decoder's way). Wine's MPEG video decoder offers RGB24 with a
// VIDEOINFOHEADER: refused, the graph put a renderer of its own in, the game's texture was never
// made and the stages stayed black (RED's renderer takes a VIDEOINFOHEADER: its movies played).
// The renderer's CheckMediaType and SetMediaType (found by shape: both compare the format with
// FORMAT_VideoInfo2, then read the size from it) are given the same picture in a VIDEOINFOHEADER2;
// a decoder that offers one goes straight through.
typedef HRESULT(__thiscall *media_type_fn)(void *self, const AM_MEDIA_TYPE *mt);
static media_type_fn g_check_media_type, g_set_media_type;

static const GUID format_video_info = {0x05589f80, 0xc356, 0x11ce, {0xbf, 0x01, 0x00, 0xaa, 0x00, 0x55, 0x59, 0x5a}};
static const GUID format_video_info2 = {0xf72a76a0, 0xeb0a, 0x11d0, {0xac, 0xe4, 0x00, 0x00, 0xc0, 0xcc, 0x16, 0xba}};

// mt as the game's renderer wants it (in m and v2), or mt itself
static const AM_MEDIA_TYPE *as_video_info2(const AM_MEDIA_TYPE *mt, AM_MEDIA_TYPE *m, VIDEOINFOHEADER2 *v2) {
    if (!mt || !IsEqualGUID(&mt->formattype, &format_video_info) || !mt->pbFormat || mt->cbFormat < sizeof(VIDEOINFOHEADER))
        return mt;
    const VIDEOINFOHEADER *v = (const VIDEOINFOHEADER *)mt->pbFormat;
    memset(v2, 0, sizeof *v2);
    v2->rcSource = v->rcSource;
    v2->rcTarget = v->rcTarget;
    v2->dwBitRate = v->dwBitRate;
    v2->dwBitErrorRate = v->dwBitErrorRate;
    v2->AvgTimePerFrame = v->AvgTimePerFrame;
    v2->bmiHeader = v->bmiHeader;
    *m = *mt;
    m->formattype = format_video_info2;
    m->cbFormat = sizeof *v2;
    m->pbFormat = (BYTE *)v2;
    return m;
}

static HRESULT __thiscall check_media_type(void *self, const AM_MEDIA_TYPE *mt) {
    AM_MEDIA_TYPE m;
    VIDEOINFOHEADER2 v2;
    return g_check_media_type(self, as_video_info2(mt, &m, &v2));
}

static HRESULT __thiscall set_media_type(void *self, const AM_MEDIA_TYPE *mt) {
    static int said;
    AM_MEDIA_TYPE m;
    VIDEOINFOHEADER2 v2;
    const AM_MEDIA_TYPE *to = as_video_info2(mt, &m, &v2);
    if (to != mt && !said++) hk_log("hiraku-pc: a movie's pictures handed to the game as its renderer wants them (VIDEOINFOHEADER2)");
    return g_set_media_type(self, to);
}

// each pointer to fn in the program's data (its vtables) set to to
static int patch_pointers(uint8_t *fn, void *to) {
    char *base = (char *)hk_program();
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    int n = 0;
    for (WORD s = 0; s < nt->FileHeader.NumberOfSections; s++, sec++) {
        // the data (EMPRESS's program, as dumped from memory, marks its .data executable too)
        if ((sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) && memcmp(sec->Name, ".data", 6)) continue;
        uint32_t *p = (uint32_t *)(base + sec->VirtualAddress);
        for (size_t i = 0; i < sec->Misc.VirtualSize / 4; i++)
            if (p[i] == (uint32_t)(uintptr_t)fn && hk_patch_slot((void **)&p[i], to)) n++;
    }
    return n;
}

void hk_c02_movies(void) {
    // CheckMediaType: format FORMAT_VideoInfo2 (else E_INVALIDARG), then MEDIATYPE_Video and MEDIASUBTYPE_RGB24
    static const uint8_t check[] = {0x8b, 0x54, 0x24, 0x04, 0x53, 0x56, 0x57, 0xb9, 0x04, 0, 0, 0, 0xbf, 0, 0, 0, 0,
                                    0x8d, 0x72, 0x2c, 0x33, 0xdb, 0xf3, 0xa7, 0xb8, 0x05, 0x40, 0x00, 0x80, 0x74, 0,
                                    0x5f, 0x5e, 0xb8, 0x57, 0x00, 0x07, 0x80, 0x5b, 0xc2, 0x04, 0x00};
    static const uint8_t check_m[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0,
                                      0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0,
                                      0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    // SetMediaType: the same format compared, then the picture's size read from the VIDEOINFOHEADER2
    static const uint8_t set[] = {0x8b, 0x44, 0x24, 0x04, 0x83, 0xec, 0x20, 0x53, 0x56, 0x57, 0x8b, 0xd9, 0xb9, 0x04, 0, 0, 0,
                                  0xbf, 0, 0, 0, 0, 0x8d, 0x70, 0x2c, 0x33, 0xd2, 0xf3, 0xa7, 0x74};
    static const uint8_t set_m[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                    0xff, 0, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    uint8_t *c = find(check, check_m, sizeof check, NULL);
    uint8_t *s = find(set, set_m, sizeof set, NULL);
    if (!c || !s) return;   // 9th style's and RED's renderers take a VIDEOINFOHEADER
    const GUID *format = (const GUID *)(uintptr_t)*(const uint32_t *)(c + 13);
    if (*(const uint32_t *)(s + 18) != (uint32_t)(uintptr_t)format || !IsEqualGUID(format, &format_video_info2)) return;
    g_check_media_type = (media_type_fn)(void *)c;
    g_set_media_type = (media_type_fn)(void *)s;
    const int nc = patch_pointers(c, (void *)check_media_type), ns = patch_pointers(s, (void *)set_media_type);
    hk_log("hiraku-pc: C02: the movies' renderer found (CheckMediaType in %d vtable%s, SetMediaType in %d)", nc, nc == 1 ? "" : "s", ns);
}

// HK_PC_JNET_LOG: the game's network library's debug lines (DistorteD's and GOLD's "jnet2_common_cb
// data[..] status[..] ...", each call's result), which it hands to an empty routine; that routine,
// found as the one the line's format is handed to, made HIRAKU's log ("jnet:"). Every other debug
// line given to the same routine comes along
static void __cdecl jnet_print(const char *fmt, ...) {
    char line[512];
    va_list a;
    va_start(a, fmt);
    vsnprintf(line, sizeof line, fmt, a);
    va_end(a);
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    hk_log("jnet: %s", line);
}

void hk_c02_debug_lines(void) {
    if (!getenv("HK_PC_JNET_LOG")) return;
    // (EMPRESS has no common_cb line: its coin settings' message is handed to the same routine)
    static const char *const fmts[] = {"jnet2_common_cb data[", "jnet2_set_coin : failed to property_write."};
    char *base = (char *)hk_program();
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    const uint8_t *text = NULL;
    const IMAGE_SECTION_HEADER *first = sec;
    for (int f = 0; f < 2 && !text; f++) {
        const size_t n = strlen(fmts[f]);
        sec = (IMAGE_SECTION_HEADER *)first;
        for (WORD w = 0; w < nt->FileHeader.NumberOfSections && !text; w++, sec++) {   // (every section: a dumped program's are all executable)
            const uint8_t *p = (const uint8_t *)base + sec->VirtualAddress;
            for (size_t i = 0; i + n <= sec->Misc.VirtualSize; i++)
                if (!memcmp(p + i, fmts[f], n)) {
                    text = p + i;
                    break;
                }
        }
    }
    if (!text) {
        hk_log("hiraku-pc: HK_PC_JNET_LOG: the network library's lines not found");
        return;
    }
    uint8_t push[5] = {0x68};
    const uint32_t at = (uint32_t)(uintptr_t)text;
    memcpy(push + 1, &at, 4);
    static const uint8_t mask[5] = {0xff, 0xff, 0xff, 0xff, 0xff};
    uint8_t *use = find(push, mask, 5, NULL);
    for (int k = 5; use && k < 12; k++)
        if (use[k] == 0xe8) {
            uint8_t *fn = (uint8_t *)(uintptr_t)rel32(use + k);
            static const uint8_t empty[5] = {0x55, 0x8b, 0xec, 0x5d, 0xc3};   // push ebp; mov ebp,esp; pop ebp; ret
            if (!in_code(fn) || memcmp(fn, empty, 5)) break;
            DWORD old;
            VirtualProtect(fn, 5, PAGE_EXECUTE_READWRITE, &old);
            fn[0] = 0xe9;
            const int32_t to = (int32_t)((uintptr_t)jnet_print - ((uintptr_t)fn + 5));
            memcpy(fn + 1, &to, 4);
            VirtualProtect(fn, 5, old, &old);
            FlushInstructionCache(GetCurrentProcess(), fn, 5);
            hk_log("hiraku-pc: HK_PC_JNET_LOG: the network library's lines to this log");
            return;
        }
    hk_log("hiraku-pc: HK_PC_JNET_LOG: the network library's print routine not found");
}
