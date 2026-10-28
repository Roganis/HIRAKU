// SPDX-License-Identifier: GPL-3.0-or-later
// HIRAKU's RtEffect.dll for the C02 cabinet's later games (beatmania IIDX RED, HAPPY SKY and
// DistorteD, PLAN.md section 36).
// Konami's is Realtek's sound effects library for the cabinet PC's sound chip (an equaliser, the
// speakers' mode); on anything else it stops the game, and bemanitools stubs it out. From RED on the
// game has no DLL of the board's for HIRAKU to take the place of (the board's library is built into
// the program: ezusb_dev.c answers under it), so this one is HIRAKU's way in: it starts the cabinet
// (cabinet.c) and the board, sets up a new cabinet, and the five calls the game makes (cdecl, as
// bm2dx.exe calls them) do nothing and say they did. Every call Konami's DLL exports is here
// (rteffect.def).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "cabinet.h"
#include "ezusb_dev.h"

#define CALL __cdecl
#define FINE(name) int CALL name(int a, int b, int c, int d) { (void)a, (void)b, (void)c, (void)d; return TRUE; }

FINE(EnableEqualizer)
FINE(GetEqualizerGain)
FINE(GetEqualizerStatus)
FINE(GetGlobalEnvironment)
FINE(GetMaximumChannels)
FINE(GetRTSEVersion)
FINE(GetSPDIFOut44KMode)
FINE(GetSpdifOutCaps)
FINE(GetSpdifOutStatus)
FINE(GetSpeakerMode)
FINE(Is_Realtek_Audio_Driver_Exist)
FINE(SetEqualizerGain)
FINE(SetGlobalEnvironment)
FINE(SetSPDIFOut44KMode)
FINE(SetSpdifOutStatus)
FINE(SetSpeakerMode)

// --- a new cabinet (as 9th style's, ezusb_c02.c): the operator settings (RED 0xc18 bytes, HAPPY SKY
// 0x202c, DistorteD 0x2270; a checksum at +0x6c) are loaded at start-up over the game's defaults, from settings.bin on
// each backup drive. With none saved yet the start-up says BACKUP DATA ERROR, and the settings' check
// (RED JAD 0x44a650, HAPPY SKY JAD 0x43de30) CLOCK ERROR: the "clock set" word (+0x68) is 0 until the
// operator's CLOCK SETUP saves it as 1. HIRAKU's cabinet comes set up: when nothing loads, the
// defaults are saved, the clock set, by the game's own routine, and the start-up goes on as if they
// had loaded. Found by shape: RED's (one each in JAA and JAD), and HAPPY SKY's, which DistorteD keeps
// with a bigger block (one each in HAPPY SKY's JAA, JAB, JAD and DistorteD's JAA...JAF).
#define CLOCK_SET 0x68
#define SETTINGS_MAX 0x4000

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

static int __cdecl load_or_set_up(void) {
    const int r = g_load_settings();
    if (r == 0) return 0;
    static uint8_t s[SETTINGS_MAX];
    memcpy(s, g_settings, g_settings_size);
    *(uint32_t *)(s + CLOCK_SET) = 1;
    const int saved = g_save_settings(s);
    hk_log("hiraku-pc: C02: no operator settings saved: the game's defaults saved, the clock set (%s)", saved == 0 ? "saved" : "not saved");
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

static void call_instead(uint8_t *call) {
    const int32_t to = (int32_t)((uintptr_t)load_or_set_up - ((uintptr_t)call + 5));
    DWORD old;
    VirtualProtect(call + 1, 4, PAGE_EXECUTE_READWRITE, &old);
    memcpy(call + 1, &to, 4);
    VirtualProtect(call + 1, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), call, 5);
}

static void set_up_cabinet(void) {
    for (size_t i = 0; i < sizeof shapes / sizeof shapes[0]; i++) {
        const struct settings_shape *g = &shapes[i];
        // the loader: the one whose size the saver copies and that the start-up calls
        for (uint8_t *load = find(g->loader, g->loader_m, g->loader_n, NULL); load;
             load = find(g->loader, g->loader_m, g->loader_n, load + 1)) {
            const size_t size = g->size_at ? *(const uint32_t *)(load + g->size_at) : g->size;
            if (size > SETTINGS_MAX || size <= CLOCK_SET + 8) continue;
            uint8_t *save = NULL;
            for (uint8_t *at = find(g->saver, g->saver_m, g->saver_n, NULL); at && !save;
                 at = find(g->saver, g->saver_m, g->saver_n, at + 1))
                if (!g->words_at || *(const uint32_t *)(at + g->words_at) * 4 == size) save = at;
            uint8_t *calls[4];
            const int n = save ? loader_calls(load, calls, 4) : 0;
            if (!n) continue;
            g_load_settings = (load_settings_t)(void *)load;
            g_save_settings = (save_settings_t)(void *)save;
            g_settings = (uint8_t *)(uintptr_t)*(const uint32_t *)(load + g->settings);
            g_settings_size = size;
            for (int k = 0; k < n; k++) call_instead(calls[k]);
            hk_log("hiraku-pc: C02: the operator settings' loader found (%s, 0x%x bytes, called from %d place%s)", g->game,
                   (unsigned)size, n, n == 1 ? "" : "s");
            return;
        }
    }
    hk_log("hiraku-pc: C02: the operator settings' loader not found: a new cabinet stops at BACKUP DATA and CLOCK ERROR");
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD why, LPVOID r) {
    (void)h, (void)r;
    if (why == DLL_PROCESS_ATTACH) {
        hk_ezusb_dev_start();
        hk_cabinet_start();
        set_up_cabinet();
    }
    return TRUE;
}
