// SPDX-License-Identifier: GPL-3.0-or-later
#include "cabinet.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define COBJMACROS
#include <d3d9.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- the log

static FILE *g_logf;

void hk_log(const char *fmt, ...) {
    if (!g_logf) {
        const char *f = getenv("HK_PC_LOG");
        g_logf = f && *f ? fopen(f, "a") : NULL;
        if (!g_logf) g_logf = stderr;
    }
    va_list a;
    va_start(a, fmt);
    vfprintf(g_logf, fmt, a);
    va_end(a);
    fputc('\n', g_logf);
    fflush(g_logf);
}

// --- the link

static struct hk_pc_link g_alone;   // no hiraku-pc: the keys below, nothing else
static struct hk_pc_link *g_link;

static void default_keys(struct hk_pc_link *l) {
    // HIRAKU's pop'n keys (shell/cabinet.cpp): the buttons zig-zag Z S X D C F V G B, F2 test,
    // 9 service, 5 a coin, Insert the card, the keypad the 10-key
    static const char row[9] = {'Z', 'S', 'X', 'D', 'C', 'F', 'V', 'G', 'B'};
    for (int b = 0; b < 9; b++) l->keys[7 + b + 1][0] = (uint8_t)row[b];
    l->keys[7][0] = VK_F2;
    l->keys[6][0] = '9';
    l->coin_keys[0] = '5';
    l->card_keys[0] = VK_INSERT;
    for (int k = 0; k < 10; k++) l->keypad_keys[k][0] = (uint8_t)(VK_NUMPAD0 + k);
    l->keypad_keys[11][0] = VK_DECIMAL;
}

struct hk_pc_link *hk_link(void) {
    if (g_link) return g_link;
    const char *f = getenv("HK_PC_LINK");
    if (f && *f) {
        HANDLE h = CreateFileA(f, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        HANDLE m = h != INVALID_HANDLE_VALUE ? CreateFileMappingA(h, NULL, PAGE_READWRITE, 0, HK_PC_LINK_SIZE, NULL) : NULL;
        struct hk_pc_link *l = m ? (struct hk_pc_link *)MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, HK_PC_LINK_SIZE) : NULL;
        if (m) CloseHandle(m);   // the view keeps the mapping
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        if (l && l->magic == HK_PC_LINK_MAGIC && l->version == HK_PC_LINK_VERSION) {
            g_link = l;
            return g_link;
        }
        hk_log("hiraku-pc: %s isn't hiraku-pc's link (error %lu): the keyboard alone", f, GetLastError());
    }
    default_keys(&g_alone);
    g_link = &g_alone;
    return g_link;
}

// --- the test script (HK_PAD_SCRIPT)

static struct { double at; uint32_t bits; int ms; } g_ev[256];
static int g_nev = -1;
static DWORD g_t0;

static uint32_t scripted(void) {
    if (g_nev < 0) {
        g_nev = 0;
        g_t0 = GetTickCount();
        const char *f = getenv("HK_PAD_SCRIPT");
        FILE *fh = f && *f ? fopen(f, "r") : NULL;
        if (fh) {
            unsigned long bits;
            while (g_nev < 256 && fscanf(fh, "%lf %lx %d", &g_ev[g_nev].at, &bits, &g_ev[g_nev].ms) == 3)
                g_ev[g_nev++].bits = (uint32_t)bits;
            fclose(fh);
        }
    }
    const double now = (GetTickCount() - g_t0) / 1000.0;
    uint32_t b = 0;
    for (int i = 0; i < g_nev; i++)
        if (now >= g_ev[i].at && now < g_ev[i].at + g_ev[i].ms / 1000.0) b |= g_ev[i].bits;
    return b;
}

static int key_down(int vk) { return vk && (GetAsyncKeyState(vk) & 0x8000) != 0; }

// Escape in the game's window ends the game, as it closes HIRAKU's other games (runtime/host/
// window.cpp): at once, since the game has nothing to save that it hasn't (its backup is written as
// it changes). Only in its own window: the keys' state is the whole desktop's
static void escape_quits(void) {
    if (!key_down(VK_ESCAPE)) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    if (pid != GetCurrentProcessId()) {
        static int said;
        if (!said++) hk_log("hiraku-pc: Escape, but another program's window is in front (%lu)", pid);
        return;
    }
    hk_log("hiraku-pc: Escape: the game ends");
    TerminateProcess(GetCurrentProcess(), 0);
}

// The coin mode the game set (usbCoinMode): 1 (pop'n 15 online, 17 and 18 in play) and 2 (pop'n 18's
// test mode) count the coin switch itself, on the pad's bit 22, where 3 reads the board's count
// (usbCoinGet)
static int g_coin_mode = 3;
void hk_set_coin_mode(int mode) { g_coin_mode = mode; }

// Then each coin is a pulse of the coin switch, as a coin mech gives it: on 60 ms, off 60 ms
// (by time: pop'n 18 reads the pad more than once a frame, and a pulse of a few reads was too
// short for it)
static uint32_t coin_switch(void) {
    static uint32_t given;
    static DWORD since;
    static int on;
    const uint32_t coins = hk_coins_now();
    if (g_coin_mode != 1 && g_coin_mode != 2) {
        given = coins, on = 0, since = 0;
        return 0;
    }
    const DWORD now = GetTickCount();
    if (since && now - since < 60) return on ? 1u << 22 : 0;
    if (on) {   // the pulse's end: off for as long
        on = 0, since = now, given++;
        return 0;
    }
    if (given != coins) {
        on = 1, since = now;
        return 1u << 22;
    }
    since = 0;
    return 0;
}

// Microseconds since frame_go was last seen (link.h: go_qpc, set in this DLL or the other)
static uint32_t us_since_go(struct hk_pc_link *l) {
    static LARGE_INTEGER f;
    LARGE_INTEGER now;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&now);
    return (uint32_t)((uint64_t)(uint32_t)((uint32_t)now.QuadPart - l->go_qpc) * 1000000 / (uint64_t)f.QuadPart);
}

int hk_host_us(uint32_t *us) {
    struct hk_pc_link *l = hk_link();
    if (l == &g_alone || !l->go_count) return 0;
    *us = l->go_seen_host_us + us_since_go(l);
    return 1;
}

// The buttons that went down since the last read and are up again (a press shorter than the time
// between two reads, or two changes handed on at once): down for this read, so the game sees them
static uint32_t taps(struct hk_pc_link *l) {
    static uint32_t seen, state;
    static int started;
    const uint32_t end = l->pad_ev_seq;
    uint32_t downs = 0;
    if (!started || end - seen > HK_PC_PAD_EVENTS) seen = end, state = l->pad, started = 1;
    for (; seen != end; seen++) {
        const uint32_t pad = l->pad_ev[seen % HK_PC_PAD_EVENTS].pad;
        downs |= pad & ~state;
        state = pad;
    }
    return downs & ~l->pad & 0x1ff00;
}

uint32_t hk_pad_now(void) {
    struct hk_pc_link *l = hk_link();
    static uint32_t read_at_go;   // the game's first read of its pad in a frame: when (pad_read_us)
    if (read_at_go != l->go_count) {
        read_at_go = l->go_count;
        l->pad_read_us = us_since_go(l);
    }
    uint32_t host;
    if (hk_host_us(&host)) l->pad_read_host_us = host;
    l->pad_ev_seen = l->pad_ev_seq;
    // the keyboard (each key a call that Wine may take to its server): read at most once a
    // millisecond, however often the game reads its pad (pop'n 18 reads it many times a frame)
    static uint32_t keys;
    static LARGE_INTEGER keys_at, qpf;
    LARGE_INTEGER now;
    if (!qpf.QuadPart) QueryPerformanceFrequency(&qpf);
    QueryPerformanceCounter(&now);
    if (!keys_at.QuadPart || now.QuadPart - keys_at.QuadPart >= qpf.QuadPart / 1000) {
        keys_at = now;
        escape_quits();
        keys = 0;
        for (int i = 0; i < 32; i++)
            if (key_down(l->keys[i][0]) || key_down(l->keys[i][1])) keys |= 1u << i;
    }
    uint32_t b = l->pad | taps(l) | keys;
    l->game_alive++;
    b |= (scripted() & ~(1u << 22)) | coin_switch();
    static uint32_t was;
    if (b & ~was & 0x1ff00) l->key_edges = l->key_edges + 1;   // a button down: its sound is the next started
    was = b;
    return b;
}

uint32_t hk_coins_now(void) {
    static uint32_t extra;
    static int was;
    struct hk_pc_link *l = hk_link();
    const int now = key_down(l->coin_keys[0]) || key_down(l->coin_keys[1]) || (scripted() & (1u << 22));
    if (now && !was) extra++;
    was = now;
    return l->coins + extra;
}

uint32_t hk_keypad_now(void) {
    struct hk_pc_link *l = hk_link();
    uint32_t b = l->keypad;
    for (int i = 0; i < 12; i++)
        if (key_down(l->keypad_keys[i][0]) || key_down(l->keypad_keys[i][1])) b |= i < 10 ? 1u << i : i == 10 ? HK_PC_KEY_00 : HK_PC_KEY_DECIMAL;
    return b;
}

uint32_t hk_card_seq(void) {
    static uint32_t extra;
    static int was;
    struct hk_pc_link *l = hk_link();
    const int now = key_down(l->card_keys[0]) || key_down(l->card_keys[1]);
    if (now && !was) extra++;
    was = now;
    return l->card_seq + extra;
}

uint32_t hk_keypad2_now(void) { return hk_link()->keypad2; }
uint32_t hk_card_seq2(void) { return hk_link()->card_seq2; }

// --- the game's paths (HK_PC_MAP)

static struct { char from[MAX_PATH], to[MAX_PATH]; size_t n; } g_map[8];
static int g_nmap;

static void read_map(void) {
    const char *s = getenv("HK_PC_MAP");
    while (s && *s && g_nmap < 8) {
        const char *end = strchr(s, ';'), *eq = strchr(s, '=');
        if (!end) end = s + strlen(s);
        if (eq && eq > s && eq < end && eq - s < MAX_PATH && end - eq - 1 < MAX_PATH) {
            memcpy(g_map[g_nmap].from, s, eq - s);
            memcpy(g_map[g_nmap].to, eq + 1, end - eq - 1);
            g_map[g_nmap].n = eq - s;
            for (char *c = g_map[g_nmap].from; *c; c++)
                if (*c == '/') *c = '\\';
            hk_log("hiraku-pc: %s -> %s", g_map[g_nmap].from, g_map[g_nmap].to);
            g_nmap++;
        }
        s = *end ? end + 1 : end;
    }
}

// --- mods (docs/MODS.md): the game's files a mod's file takes the place of (HK_PC_FILES)

static struct { char *game, *mod; int said; } *g_files;
static int g_nfiles;

static char *read_text(const char *path) {
    FILE *f = path && *path ? fopen(path, "rb") : NULL;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *t = (char *)malloc((size_t)n + 1);
    t[fread(t, 1, (size_t)n, f)] = 0;
    fclose(f);
    return t;
}

static void read_files(void) {
    char *t = read_text(getenv("HK_PC_FILES"));
    for (char *line = t ? strtok(t, "\r\n") : NULL; line; line = strtok(NULL, "\r\n")) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        g_files = realloc(g_files, sizeof *g_files * (size_t)(g_nfiles + 1));
        g_files[g_nfiles].game = line, g_files[g_nfiles].mod = tab + 1, g_files[g_nfiles].said = 0;
        g_nfiles++;
    }
    if (g_nfiles) hk_log("hiraku-pc: mods: %d of the game's files are a mod's", g_nfiles);
}

// The path the game asked for, or where HK_PC_MAP sends it (in buf)
static const char *mapped(const char *p, char *buf) {
    if (!p) return p;
    for (int i = 0; i < g_nfiles; i++) {   // a mod's file instead of the game's
        const char *a = p, *b = g_files[i].game;
        for (; *a && *b; a++, b++) {
            const char x = *a == '/' ? '\\' : *a;
            if ((x | 0x20) != (*b | 0x20) && x != *b) break;
        }
        if (!*a && !*b && strlen(g_files[i].mod) < MAX_PATH) {
            if (!g_files[i].said) g_files[i].said = 1, hk_log("hiraku-pc: mods: %s from %s", p, g_files[i].mod);
            return strcpy(buf, g_files[i].mod);
        }
    }
    for (int i = 0; i < g_nmap; i++) {
        const size_t n = g_map[i].n;
        size_t k = 0;
        for (; k < n && p[k]; k++) {
            const char a = p[k] == '/' ? '\\' : p[k], b = g_map[i].from[k];
            if ((a | 0x20) != (b | 0x20) && a != b) break;
        }
        if (k < n) continue;
        // a whole folder name: "D:\popn15" doesn't take "D:\popn15x"
        if (g_map[i].from[n - 1] != '\\' && p[n] && p[n] != '\\' && p[n] != '/') continue;
        if (snprintf(buf, MAX_PATH, "%s%s", g_map[i].to, p + n) >= MAX_PATH) return p;
        return buf;
    }
    return p;
}

// HIRAKU's RtEffect.dll is the game's way in (rteffect.c); read as a file (10th style checks its
// folder's files), it is Konami's (HK_PC_RTEFFECT_FILE)
static const char *konami_file(const char *p) {
    static const char *konami;
    if (!konami) konami = getenv("HK_PC_RTEFFECT_FILE") ? getenv("HK_PC_RTEFFECT_FILE") : "";
    if (!p || !*konami) return NULL;
    const char *base = p + strlen(p);
    while (base > p && base[-1] != '\\' && base[-1] != '/' && base[-1] != ':') base--;
    return _stricmp(base, "RtEffect.dll") ? NULL : konami;
}

// HK_PC_FILELOG (bring-up): each file the game can't open, as it named it and as HIRAKU mapped it
static HANDLE WINAPI my_CreateFileA(LPCSTR p, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD cd, DWORD fl,
                                    HANDLE t) {
    char b[MAX_PATH];
    const char *to = konami_file(p) ? konami_file(p) : mapped(p, b);
    const HANDLE h = CreateFileA(to, acc, share, sa, cd, fl, t);
    static int log = -1;
    if (log < 0) log = getenv("HK_PC_FILELOG") != NULL;
    if (log && h == INVALID_HANDLE_VALUE) hk_log("hiraku-pc: can't open %s (%s): %lu", p ? p : "(null)", to ? to : "(null)", GetLastError());
    return h;
}
static BOOL WINAPI my_CreateDirectoryA(LPCSTR p, LPSECURITY_ATTRIBUTES sa) {
    char b[MAX_PATH];
    return CreateDirectoryA(mapped(p, b), sa);
}
static HANDLE WINAPI my_FindFirstFileA(LPCSTR p, LPWIN32_FIND_DATAA d) {
    char b[MAX_PATH];
    return FindFirstFileA(mapped(p, b), d);
}
static DWORD WINAPI my_GetFileAttributesA(LPCSTR p) {
    char b[MAX_PATH];
    return GetFileAttributesA(mapped(p, b));
}
static BOOL WINAPI my_DeleteFileA(LPCSTR p) {
    char b[MAX_PATH];
    return DeleteFileA(mapped(p, b));
}
static BOOL WINAPI my_RemoveDirectoryA(LPCSTR p) {
    char b[MAX_PATH];
    return RemoveDirectoryA(mapped(p, b));
}
static BOOL WINAPI my_MoveFileA(LPCSTR from, LPCSTR to) {
    char b1[MAX_PATH], b2[MAX_PATH];
    return MoveFileA(mapped(from, b1), mapped(to, b2));
}
static DWORD WINAPI my_GetFullPathNameA(LPCSTR p, DWORD n, LPSTR out, LPSTR *file) {
    char b[MAX_PATH];
    return GetFullPathNameA(mapped(p, b), n, out, file);
}

static int g_waitlog = -1;   // HK_PC_WAITLOG (below)
static void waits_report(void);

// --- mods: memory patches (HK_PC_PATCHES), each in place whenever its original bytes are there
// (or, without original bytes, every time: a cheat), once if asked; at the program's own addresses

// rate: 1 tied to a [[rate]] (in place only while it's on), 2 also only between the cabinet's ticks
static struct patch { uint8_t *at, *original, *replace; size_t n; int once, done, rate; } *g_patches;
static int g_npatches;

static int unhex(const char *h, uint8_t **out) {
    const size_t n = strlen(h) / 2;
    *out = (uint8_t *)malloc(n ? n : 1);
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(h + 2 * i, "%2x", &v) != 1) return -1;
        (*out)[i] = (uint8_t)v;
    }
    return (int)n;
}

static void read_patches(void) {
    char *t = read_text(getenv("HK_PC_PATCHES"));
    for (char *line = t ? strtok(t, "\r\n") : NULL; line && g_npatches < 256; line = strtok(NULL, "\r\n")) {
        unsigned long addr;
        int once;
        char orig[2048], repl[2048], kind[8] = "";
        // "ADDRESS ONCE ORIGINAL REPLACE [rate|ticks]"
        if (sscanf(line, "%lx %d %2047s %2047s %7s", &addr, &once, orig, repl, kind) < 4) continue;
        struct patch p = {(uint8_t *)(uintptr_t)addr, NULL, NULL, 0, once, 0, !strcmp(kind, "rate") ? 1 : !strcmp(kind, "ticks") ? 2 : 0};
        const int n = unhex(repl, &p.replace);
        if (n <= 0 || (strcmp(orig, "-") && unhex(orig, &p.original) != n)) continue;
        if (p.rate && (!p.original || once)) continue;   // (mods.cpp refuses these)
        p.n = (size_t)n;
        g_patches = realloc(g_patches, sizeof *g_patches * (size_t)(g_npatches + 1));
        g_patches[g_npatches++] = p;
    }
    if (g_npatches) hk_log("hiraku-pc: mods: %d memory patches", g_npatches);
}

// Whether [p, p + n) is all there to read (committed, not a guard or no-access page)
static int present(const uint8_t *p, size_t n) {
    for (const uint8_t *end = p + n; p < end;) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(p, &mi, sizeof mi) || mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
        p = (const uint8_t *)mi.BaseAddress + mi.RegionSize;
    }
    return 1;
}

static void put(uint8_t *at, const uint8_t *bytes, size_t n) {
    DWORD old;
    VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old);
    memcpy(at, bytes, n);
    VirtualProtect(at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, n);
}

// A [[rate]]'s state for the coming frame (hiraku-pc's game_hz_milli: above the cabinet's 60 while
// one is on): on, and whether the frame ends a cabinet frame's worth of time (1/59.94 s, as
// mods.cpp counts it for the emulated machines): between_ticks patches are out then
static int g_rate_on, g_tick = 1;
static void rate_frame(void) {
    static double owed;
    const uint32_t hz = hk_link()->game_hz_milli;
    g_rate_on = hz > 60500;
    if (!g_rate_on) {
        owed = 0, g_tick = 1;
        return;
    }
    owed += 59940.0 / hz;
    g_tick = owed >= 1 - 1e-9;
    if (g_tick) owed -= 1;
}

static void apply_patches(void) {
    for (int i = 0; i < g_npatches; i++) {
        struct patch *p = &g_patches[i];
        if (p->rate && (!g_rate_on || (p->rate == 2 && g_tick))) {   // the original back, if the patch is there
            if (present(p->at, p->n) && !memcmp(p->at, p->replace, p->n)) put(p->at, p->original, p->n);
            continue;
        }
        if (p->done || !present(p->at, p->n) || !memcmp(p->at, p->replace, p->n)) continue;   // gone, not there, in place
        if (p->original && memcmp(p->at, p->original, p->n)) continue;   // not loaded (yet): wait
        put(p->at, p->replace, p->n);
        if (!hk_link()->patch_done[i]) hk_log("hiraku-pc: mods: patch at %p (%u bytes) in place", (void *)p->at, (unsigned)p->n);
        hk_link()->patch_done[i] = 1;
        if (p->once) p->done = 1;
    }
}

// --- Direct3D 9: a window instead of the whole screen, and the game's own rate

static int g_win_w, g_win_h, g_win_full;   // HK_PC_WINDOW
static double g_hz = 60;
static HWND g_hwnd;
static int g_shown_off;   // the game's window shown off the screen: Direct3D wanted it shown (my_CreateDevice)
static int g_game_w = 640, g_game_h = 480;

typedef HRESULT(WINAPI *CreateDevice_t)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *,
                                        IDirect3DDevice9 **);
typedef HRESULT(WINAPI *Reset_t)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
typedef HRESULT(WINAPI *Present_t)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *);
typedef HRESULT(WINAPI *CreateTexture_t)(IDirect3DDevice9 *, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9 **,
                                         HANDLE *);
static CreateDevice_t g_CreateDevice;
static Reset_t g_Reset;
static Present_t g_Present;
static CreateTexture_t g_CreateTexture;

// A texture the game draws into in a 16-bit format Wine's OpenGL can't render to (pop'n 16:
// A4R4G4B4, its sprites' texture group never made and the game faulting at start-up): made as
// A8R8G8B8 instead (the game draws into it, so its own bytes' layout isn't needed)
static HRESULT WINAPI my_CreateTexture(IDirect3DDevice9 *d, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                                       IDirect3DTexture9 **tex, HANDLE *shared) {
    HRESULT r = g_CreateTexture(d, w, h, levels, usage, fmt, pool, tex, shared);
    if (FAILED(r) && (usage & D3DUSAGE_RENDERTARGET) &&
        (fmt == D3DFMT_A4R4G4B4 || fmt == D3DFMT_X4R4G4B4 || fmt == D3DFMT_A1R5G5B5 || fmt == D3DFMT_X1R5G5B5)) {
        static int said;
        if (!said++) hk_log("hiraku-pc: a render target in format %d refused: made as A8R8G8B8", (int)fmt);
        r = g_CreateTexture(d, w, h, levels, usage, D3DFMT_A8R8G8B8, pool, tex, shared);
    }
    return r;
}

static void *patch_slot(void **slot, void *to) {
    DWORD old;
    void *was = *slot;
    if (was == to) return NULL;
    VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old);
    *slot = to;
    VirtualProtect(slot, sizeof *slot, old, &old);
    return was;
}

// HIRAKU's window shows the pictures (hiraku-pc's link says so): the game's stays hidden
static int host_shows(void) { return hk_link()->host_shows != 0; }
int hk_host_shows(void) { return host_shows(); }
void *hk_patch_slot(void **slot, void *to) { return patch_slot(slot, to); }

static void windowed(D3DPRESENT_PARAMETERS *pp) {
    if (!g_win_w && !g_win_full && !host_shows()) return;
    g_game_w = pp->BackBufferWidth ? (int)pp->BackBufferWidth : g_game_w;
    g_game_h = pp->BackBufferHeight ? (int)pp->BackBufferHeight : g_game_h;
    pp->Windowed = TRUE;
    pp->FullScreen_RefreshRateInHz = 0;
    pp->BackBufferFormat = D3DFMT_UNKNOWN;   // the desktop's
}

// The game's window: no border, at HK_PC_WINDOW's size, in the middle of its screen
static void place_window(HWND w) {
    if (w && host_shows()) {
        g_hwnd = w;
        SetWindowLongA(w, GWL_EXSTYLE, WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
        ShowWindow(w, SW_HIDE);
        return;
    }
    if (!w || (!g_win_w && !g_win_full)) return;
    g_hwnd = w;
    MONITORINFO mi;
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    GetMonitorInfoA(MonitorFromWindow(w, MONITOR_DEFAULTTOPRIMARY), &mi);
    const RECT m = mi.rcMonitor;
    const int sw = m.right - m.left, sh = m.bottom - m.top;
    const int ww = g_win_full ? sw : g_win_w, wh = g_win_full ? sh : g_win_h;
    SetWindowLongA(w, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    SetWindowLongA(w, GWL_EXSTYLE, 0);
    SetWindowPos(w, HWND_TOP, m.left + (sw - ww) / 2, m.top + (sh - wh) / 2, ww, wh, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}

// The picture just drawn, into the link's pixels (0x00RRGGBB): the back buffer read back through
// a surface in system memory, of its format (the desktop's, X8R8G8B8, in a window)
static IDirect3DSurface9 *g_readback;
static UINT g_rb_w, g_rb_h;
static D3DFORMAT g_rb_fmt;

// A picture's pixels into the link's (0x00RRGGBB), from a surface of D3DFORMAT `format` (the same
// numbers in Direct3D 8 and 9)
void hk_put_pixels(const void *bits, int pitch, unsigned w, unsigned h, int format) {
    struct hk_pc_link *l = hk_link();
    hk_convert_pixels((uint32_t *)((char *)l + HK_PC_PIXELS), bits, pitch, w, h, format);
    l->frame_w = w, l->frame_h = h;
}

void hk_convert_pixels(uint32_t *out, const void *bits, int pitch, unsigned w, unsigned h, int format) {
    for (UINT y = 0; y < h; y++) {
        const uint8_t *row = (const uint8_t *)bits + (size_t)y * pitch;
        uint32_t *o = out + (size_t)y * w;
        if (format == D3DFMT_X8R8G8B8 || format == D3DFMT_A8R8G8B8) {
            const uint32_t *p = (const uint32_t *)row;
            for (UINT x = 0; x < w; x++) o[x] = p[x] & 0xffffff;
        } else if (format == D3DFMT_R5G6B5) {
            const uint16_t *p = (const uint16_t *)row;
            for (UINT x = 0; x < w; x++) {
                const uint32_t v = p[x], r = v >> 11, g = v >> 5 & 63, b = v & 31;
                o[x] = (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
            }
        } else if (format == D3DFMT_X1R5G5B5 || format == D3DFMT_A1R5G5B5) {
            const uint16_t *p = (const uint16_t *)row;
            for (UINT x = 0; x < w; x++) {
                const uint32_t v = p[x], r = v >> 10 & 31, g = v >> 5 & 31, b = v & 31;
                o[x] = (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
            }
        }
    }
}

static int capture(IDirect3DDevice9 *d) {
    IDirect3DSurface9 *bb = NULL;
    int ok = 0;
    static int said_bb, said_rt;
    const HRESULT hb = IDirect3DDevice9_GetBackBuffer(d, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    if (FAILED(hb)) {
        if (!said_bb++) hk_log("hiraku-pc: the game's picture: no back buffer (%#lx)", (unsigned long)hb);
        return 0;
    }
    D3DSURFACE_DESC desc;
    IDirect3DSurface9_GetDesc(bb, &desc);
    if (desc.Width > HK_PC_MAX_W || desc.Height > HK_PC_MAX_H) {
        IDirect3DSurface9_Release(bb);
        return 0;
    }
    if (!g_readback || g_rb_w != desc.Width || g_rb_h != desc.Height || g_rb_fmt != desc.Format) {
        if (g_readback) IDirect3DSurface9_Release(g_readback);
        g_readback = NULL;
        if (FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(d, desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM,
                                                                &g_readback, NULL))) {
            hk_log("hiraku-pc: can't read the pictures back (%ux%u, format %d)", desc.Width, desc.Height, (int)desc.Format);
            IDirect3DSurface9_Release(bb);
            return 0;
        }
        g_rb_w = desc.Width, g_rb_h = desc.Height, g_rb_fmt = desc.Format;
        hk_log("hiraku-pc: the pictures go to HIRAKU's window (%ux%u, format %d)", desc.Width, desc.Height, (int)desc.Format);
    }
    D3DLOCKED_RECT lr;
    const HRESULT hr = IDirect3DDevice9_GetRenderTargetData(d, bb, g_readback);
    const HRESULT hl = SUCCEEDED(hr) ? IDirect3DSurface9_LockRect(g_readback, &lr, NULL, D3DLOCK_READONLY) : hr;
    if (SUCCEEDED(hl)) {
        hk_put_pixels(lr.pBits, lr.Pitch, desc.Width, desc.Height, (int)desc.Format);
        IDirect3DSurface9_UnlockRect(g_readback);
        ok = 1;
    } else if (!said_rt++) {
        hk_log("hiraku-pc: the game's picture not read back (%s %#lx)", FAILED(hr) ? "GetRenderTargetData" : "LockRect", (unsigned long)hl);
    }
    IDirect3DSurface9_Release(bb);
    return ok;
}

// A fifth of a millisecond's nap. Sleep(1) is a millisecond under Wine and more on Windows, all of
// it between hiraku-pc starting a frame and the game reading its controls; ntdll's NtDelayExecution
// takes 100 ns units (Wine sleeps that long; Windows to its timer's resolution, asked for at 0.5 ms)
typedef LONG(NTAPI *NtDelayExecution_t)(BOOLEAN, PLARGE_INTEGER);
typedef LONG(NTAPI *NtSetTimerResolution_t)(ULONG, BOOLEAN, PULONG);

static void nap(void) {
    static NtDelayExecution_t delay;
    static int looked;
    if (!looked) {
        looked = 1;
        HMODULE nt = GetModuleHandleA("ntdll.dll");
        delay = nt ? (NtDelayExecution_t)(void *)GetProcAddress(nt, "NtDelayExecution") : NULL;
        NtSetTimerResolution_t res = nt ? (NtSetTimerResolution_t)(void *)GetProcAddress(nt, "NtSetTimerResolution") : NULL;
        ULONG cur;
        if (res) res(5000, TRUE, &cur);
    }
    LARGE_INTEGER t;
    t.QuadPart = -2000;   // relative, 200 us
    if (delay) delay(FALSE, &t);
    else Sleep(1);
}

// Until hiraku-pc has shown the picture and its pacing lets the next frame start (or a quarter of
// a second, if it has stopped answering)
void (*hk_between)(void);

static void wait_for_go(void) {
    struct hk_pc_link *l = hk_link();
    const uint32_t seq = l->frame_seq;
    const DWORD t0 = GetTickCount();
    for (int spins = 0; l->frame_go != seq; spins++) {
        if (GetTickCount() - t0 > 250) break;
        // an in-between picture asked for (smooth motion): drawn at once, and the next soon after
        if (hk_between && l->between_req != l->between_done) {
            hk_between();
            spins = 0;
            continue;
        }
        if (spins < 200) YieldProcessor();
        else if (spins < 400 || l->smooth) SwitchToThread();
        else nap();
    }
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    l->go_seen_host_us = l->go_host_us;
    l->go_qpc = (uint32_t)t.QuadPart;
    l->go_count = l->go_count + 1;
}

// debug HK_DBG_REC=[=]PTRADDR:SIZE:DIR[,...] (= the address is the buffer's): each buffer a pointer has held, dumped
// when it changes (DIR\\rec<n>_<buffer>_f<frame>.bin): how a game keeps what the server sent it
static void dbg_records(void) {
    static int on = -1;
    static struct { uintptr_t ptr; size_t size; char dir[MAX_PATH]; const uint8_t *bufs[8]; uint32_t last[8], nbufs; int direct; } w[4];
    static uint32_t nw, n;
    if (on < 0) {
        const char *e = getenv("HK_DBG_REC");
        on = 0;
        while (e && nw < 4) {
            unsigned p, z;
            int used = 0;
            const int direct = *e == '=';   // =ADDR: the buffer itself, not a pointer to it
            if (sscanf(e + direct, "%x:%x:%259[^,]%n", &p, &z, w[nw].dir, &used) != 3) break;
            used += direct;
            w[nw].ptr = p, w[nw].size = z, w[nw].direct = direct, nw++, on = 1;
            hk_log("hiraku-pc: debug: watching the buffers at %08x (%x bytes) into %s", p, z, w[nw - 1].dir);
            e = e[used] == ',' ? e + used + 1 : NULL;
        }
    }
    for (uint32_t i = 0; on > 0 && i < nw; i++) {
        if (!present((const uint8_t *)w[i].ptr, 4)) continue;
        const uint8_t *now = w[i].direct ? (const uint8_t *)w[i].ptr : *(const uint8_t *const *)w[i].ptr;
        uint32_t k = 0;
        while (k < w[i].nbufs && w[i].bufs[k] != now) k++;
        if (now && k == w[i].nbufs && w[i].nbufs < 8) {
            w[i].bufs[w[i].nbufs++] = now;
            hk_log("hiraku-pc: debug: %08x holds %p", (unsigned)w[i].ptr, (const void *)now);
        }
        for (k = 0; k < w[i].nbufs; k++) {
            const uint8_t *rec = w[i].bufs[k];
            if (!present(rec, w[i].size)) continue;
            uint32_t h = 2166136261u;
            for (size_t j = 0; j < w[i].size; j++) h = (h ^ rec[j]) * 16777619u;
            if (h == w[i].last[k]) continue;
            w[i].last[k] = h;
            char f[MAX_PATH + 48];
            snprintf(f, sizeof f, "%s\\rec%03u_%08x_f%u.bin", w[i].dir, n++, (unsigned)(uintptr_t)rec, (unsigned)hk_link()->frames);
            FILE *o = fopen(f, "wb");
            if (o) fwrite(rec, 1, w[i].size, o), fclose(o);
        }
    }
}

// debug HK_PC_DUMP=PICTURE[,PICTURE...]:DIR (or +N:DIR: N, N + 10 and N + 100 pictures after a
// [[rate]] first comes on): the program's writable sections (its globals), each written whole at those pictures (DIR\\dump_f<picture>_<address>.bin): two dumps a few frames apart
// show the game's counters (the words that grew by as many), for mods that change its frame rate
static void dbg_dumps(void) {
    static int on = -1, n, after_rate;
    static uint32_t at[16], rate_from;
    static char dir[MAX_PATH];
    if (on < 0) {
        on = 0;
        const char *e = getenv("HK_PC_DUMP");
        const char *colon = e ? strchr(e, ':') : NULL;
        if (colon && colon - e < 200 && *e == '+') {   // "+N": N, N + 10 and N + 100 pictures after a [[rate]] comes on
            char *end;
            const uint32_t k = (uint32_t)strtoul(e + 1, &end, 10);
            const int run = *end == '/' ? atoi(end + 1) : 0;   // "+N/K": K pictures in a row from N
            if (run > 0) {
                for (n = 0; n < run && n < 16; n++) at[n] = k + (uint32_t)n;
            } else {
                at[0] = k, at[1] = k + 10, at[2] = k + 100, n = 3;
            }
            after_rate = 1;
            snprintf(dir, sizeof dir, "%s", colon + 1);
            on = 1;
        } else if (colon && colon - e < 200) {
            for (const char *p = e; p < colon && n < 16; p = strchr(p, ',') ? strchr(p, ',') + 1 : colon) at[n++] = (uint32_t)strtoul(p, NULL, 10);
            snprintf(dir, sizeof dir, "%s", colon + 1);
            on = n > 0;
        }
    }
    if (on <= 0) return;
    uint32_t frame = hk_link()->frames;
    if (after_rate) {
        if (!rate_from && g_rate_on) rate_from = frame;
        if (!rate_from) return;
        frame -= rate_from;
    }
    for (int i = 0; i < n; i++) {
        if (at[i] != frame) continue;
        char *base = (char *)hk_program();
        IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
        IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
        for (WORD k = 0; k < nt->FileHeader.NumberOfSections; k++, sec++) {
            if (!(sec->Characteristics & IMAGE_SCN_MEM_WRITE) || (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            char f[MAX_PATH + 48];
            snprintf(f, sizeof f, "%s\\dump_f%u_%08x.bin", dir, (unsigned)frame, (unsigned)(uintptr_t)(base + sec->VirtualAddress));
            FILE *o = fopen(f, "wb");
            if (o) fwrite(base + sec->VirtualAddress, 1, sec->Misc.VirtualSize, o), fclose(o);
        }
        // HK_PC_DUMP_HEAP=1: the game's own memory too (committed, private, read-write: its heaps and
        // the objects in them), each region a file, up to 1 GB
        if (getenv("HK_PC_DUMP_HEAP")) {
            size_t total = 0;
            MEMORY_BASIC_INFORMATION mi;
            for (uint8_t *q = (uint8_t *)0x10000; q < (uint8_t *)0x7fff0000 && VirtualQuery(q, &mi, sizeof mi); q = (uint8_t *)mi.BaseAddress + mi.RegionSize) {
                if (mi.State != MEM_COMMIT || mi.Type != MEM_PRIVATE || mi.Protect != PAGE_READWRITE || total > (1u << 30)) continue;
                char f[MAX_PATH + 48];
                snprintf(f, sizeof f, "%s\\heap_f%u_%08x.bin", dir, (unsigned)frame, (unsigned)(uintptr_t)mi.BaseAddress);
                FILE *o = fopen(f, "wb");
                if (o) fwrite(mi.BaseAddress, 1, mi.RegionSize, o), fclose(o);
                total += mi.RegionSize;
            }
        }
        hk_log("hiraku-pc: debug: the program's globals dumped at picture %u", (unsigned)frame);
    }
}

// debug HK_PC_WATCH=ADDRESS[,ADDRESS...][:PICTURE]: data watchpoints (the CPU's debug registers, every thread of
// the game's) on the words at ADDRESS from that picture on: each place that writes one (the
// instruction after the write) to the log once, the first 32. For mods: what counts a counter
// found by HK_PC_DUMP that's reached through a pointer. An address written xADDRESS is code: each
// time it runs (the first 40), the registers and the stack's top eight words to the log
static uint32_t g_watch_exec;   // which of the four are code
static LONG CALLBACK watch_hit(EXCEPTION_POINTERS *e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(e->ContextRecord->Dr6 & 15)) return EXCEPTION_CONTINUE_SEARCH;
    const int which = e->ContextRecord->Dr6 & 1 ? 0 : e->ContextRecord->Dr6 & 2 ? 1 : e->ContextRecord->Dr6 & 4 ? 2 : 3;
    const DWORD dr[4] = {e->ContextRecord->Dr0, e->ContextRecord->Dr1, e->ContextRecord->Dr2, e->ContextRecord->Dr3};
    if (g_watch_exec >> which & 1) {
        static int hits[4];
        const CONTEXT *c = e->ContextRecord;
        if (hits[which]++ < 40) {
            const DWORD *sp = (const DWORD *)(uintptr_t)c->Esp;
            hk_log("hiraku-pc: debug: %08lx run: eax %08lx ebx %08lx ecx %08lx edx %08lx esi %08lx edi %08lx ebp %08lx esp %08lx;"
                   " stack %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx",
                   (unsigned long)dr[which], (unsigned long)c->Eax, (unsigned long)c->Ebx, (unsigned long)c->Ecx, (unsigned long)c->Edx,
                   (unsigned long)c->Esi, (unsigned long)c->Edi, (unsigned long)c->Ebp, (unsigned long)c->Esp, (unsigned long)sp[0],
                   (unsigned long)sp[1], (unsigned long)sp[2], (unsigned long)sp[3], (unsigned long)sp[4], (unsigned long)sp[5],
                   (unsigned long)sp[6], (unsigned long)sp[7]);
        }
        e->ContextRecord->Dr6 = 0;
        e->ContextRecord->EFlags |= 0x10000;   // RF: the instruction runs this time
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    static uintptr_t seen[32];
    static int n;
    const uintptr_t at = e->ContextRecord->Eip;
    int k = 0;
    while (k < n && seen[k] != at) k++;
    if (k == n && n < 32) {
        seen[n++] = at;
        hk_log("hiraku-pc: debug: %08lx written before %08lx (thread %lu, now %08x)", (unsigned long)dr[which], (unsigned long)at,
               GetCurrentThreadId(), *(const uint32_t *)(uintptr_t)dr[which]);
    }
    e->ContextRecord->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void watch_regs(CONTEXT *c, const uint32_t *addr, int naddr) {
    DWORD *dr[4] = {&c->Dr0, &c->Dr1, &c->Dr2, &c->Dr3};
    for (int i = 0; i < naddr; i++) {   // Li, write, 4 bytes (code: run, 1 byte)
        *dr[i] = addr[i];
        c->Dr7 = (c->Dr7 & ~(3u << (2 * i)) & ~(0xfu << (16 + 4 * i))) | (1u << (2 * i)) |
                 ((g_watch_exec >> i & 1 ? 0u : 0xdu) << (16 + 4 * i));
    }
}
static uint32_t g_watch_addr[4];
static int g_watch_n;   // set once the watch is on: threads started later get it too (hk_watch_thread)

void hk_watch_thread(void) {
    if (!g_watch_n) return;
    CONTEXT c = {.ContextFlags = CONTEXT_DEBUG_REGISTERS};
    if (GetThreadContext(GetCurrentThread(), &c)) {
        watch_regs(&c, g_watch_addr, g_watch_n);
        SetThreadContext(GetCurrentThread(), &c);
    }
}

static void dbg_watch(void) {
    static int state = -1, naddr;
    static uint32_t addr[4], from;
    if (state < 0) {   // "ADDRESS[,ADDRESS...]:PICTURE", up to four
        const char *e = getenv("HK_PC_WATCH");
        state = 2;
        for (const char *p = e; p && *p && naddr < 4;) {
            char *end;
            if (*p == 'x') g_watch_exec |= 1u << naddr, p++;
            addr[naddr++] = (uint32_t)strtoul(p, &end, 16);
            if (*end == ',') p = end + 1;
            else {
                if (*end == ':') from = (uint32_t)strtoul(end + 1, NULL, 10);
                break;
            }
        }
        if (naddr) state = 0;
    }
    if (state != 0 || hk_link()->frames < from) return;
    state = 1;
    AddVectoredExceptionHandler(1, watch_hit);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te;
    memset(&te, 0, sizeof te);
    te.dwSize = sizeof te;
    int threads = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
        const int self = te.th32ThreadID == GetCurrentThreadId();
        HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!t) continue;
        if (!self) SuspendThread(t);
        CONTEXT c = {.ContextFlags = CONTEXT_DEBUG_REGISTERS};
        if (GetThreadContext(t, &c)) {
            watch_regs(&c, addr, naddr);
            if (SetThreadContext(t, &c)) threads++;
        }
        if (!self) ResumeThread(t);
        CloseHandle(t);
    }
    CloseHandle(snap);
    memcpy(g_watch_addr, addr, sizeof addr), g_watch_n = naddr;
    hk_log("hiraku-pc: debug: watching %d addresses from %08x (%d threads, and those started later)", naddr, addr[0], threads);
}

// Each Present, Direct3D 9's or 8's (d3d8.c): the mods' patches and the debug watch first, before
// the next frame runs
void hk_present_begin(void) {
    // the words a mod's [[rate]] looks at, for hiraku-pc (link.h, v9)
    struct hk_pc_link *l = hk_link();
    for (uint32_t i = 0; i < HK_PC_WATCHES; i++) {
        const uint8_t *w = (const uint8_t *)(uintptr_t)l->watch_addr[i];
        if (w) l->watch_val[i] = present(w, 4) ? *(const uint32_t *)w : 0;
    }
    rate_frame();
    if (g_npatches) apply_patches();
    dbg_records();
    dbg_watch();
    dbg_dumps();
}

// A picture the game has drawn, in HIRAKU's window (its pixels in the link if `captured`): counted
// with its frame's sound, then the game waits for hiraku-pc's next frame
static void profile_frame(void);

void hk_picture_drawn(int captured) {
    struct hk_pc_link *l = hk_link();
    profile_frame();
    if (captured) {
        hk_dsound_picture();   // the frame's sound, with its picture
        l->drawn_us = l->go_count ? us_since_go(l) : 0;
        MemoryBarrier();
        l->frame_seq = l->frame_seq + 1;
    }
    l->frames++;
    if (g_waitlog > 0) waits_report();
    wait_for_go();
}

static HRESULT WINAPI my_Present(IDirect3DDevice9 *d, const RECT *src, const RECT *dst, HWND wnd, const RGNDATA *dirty) {
    static int said;
    if (!said++) hk_log("hiraku-pc: the game's first Present");
    hk_present_begin();
    if (host_shows()) {   // HIRAKU's window shows it, at the time its pacing chooses
        const int captured = capture(d);
        hk_d3d9_frame_end(d, captured);   // (smooth motion: the frame's quads, d3d9_smooth.c)
        hk_picture_drawn(captured);       // (its in-between pictures drawn while the game waits)
        hk_d3d9_frame_start(d);
        return D3D_OK;
    }
    RECT fit;
    if (!dst && g_hwnd && (g_win_w || g_win_full)) {
        // the picture at the window's height, its shape kept, in the middle
        RECT c;
        GetClientRect(g_hwnd, &c);
        const int cw = c.right, ch = c.bottom;
        int w = cw, h = cw * g_game_h / g_game_w;
        if (h > ch) h = ch, w = ch * g_game_w / g_game_h;
        fit.left = (cw - w) / 2, fit.top = (ch - h) / 2, fit.right = fit.left + w, fit.bottom = fit.top + h;
        dst = &fit;
        if (w < cw || h < ch) {   // the bands beside it black
            HDC dc = GetDC(g_hwnd);
            RECT l = {0, 0, fit.left, ch}, r = {fit.right, 0, cw, ch}, t = {0, 0, cw, fit.top}, b = {0, fit.bottom, cw, ch};
            HBRUSH k = (HBRUSH)GetStockObject(BLACK_BRUSH);
            FillRect(dc, &l, k), FillRect(dc, &r, k), FillRect(dc, &t, k), FillRect(dc, &b, k);
            ReleaseDC(g_hwnd, dc);
        }
    }
    const HRESULT r = g_Present(d, src, dst, wnd, dirty);
    hk_link()->frames++;
    if (g_hz > 0) {   // no more than g_hz a second, on a steady grid
        static LARGE_INTEGER f, next;
        LARGE_INTEGER now;
        if (!f.QuadPart) QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&now);
        const LONGLONG period = (LONGLONG)(f.QuadPart / g_hz);
        if (!next.QuadPart || now.QuadPart > next.QuadPart + period) next.QuadPart = now.QuadPart;
        next.QuadPart += period;
        for (;;) {
            QueryPerformanceCounter(&now);
            const LONGLONG left = next.QuadPart - now.QuadPart;
            if (left <= 0) break;
            const LONGLONG ms = left * 1000 / f.QuadPart;
            if (ms >= 2) Sleep((DWORD)(ms - 1));
            else YieldProcessor();
        }
    }
    return r;
}

static HRESULT WINAPI my_Reset(IDirect3DDevice9 *d, D3DPRESENT_PARAMETERS *pp) {
    windowed(pp);
    hk_d3d9_reset();
    return g_Reset(d, pp);
}

// The game's Present kept HIRAKU's: Windows' Direct3D 9 puts another table's in its place after the
// device is made (pop'n 15-18 on Windows 10, GitHub issue 4: d3d9.dll+e6120; the game ran, HIRAKU's
// window stayed black). Put back, with what was put there called by ours, as the game's own was;
// looked at every 100 ms for the first minute, then every half second (a Reset may do it again).
static IDirect3DDevice9 *g_dev;
static const char *module_of(const void *p, uintptr_t *offset) {
    static char path[MAX_PATH];
    HMODULE m = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)p, &m);
    *offset = (uintptr_t)p - (uintptr_t)m;
    if (!m || !GetModuleFileNameA(m, path, sizeof path)) return "?";
    const char *b = strrchr(path, '\\');
    return b ? b + 1 : path;
}
static DWORD WINAPI present_watch(LPVOID unused) {
    (void)unused;
    for (int i = 0;; i++) {
        Sleep(i < 600 ? 100 : 500);
        void **vt = *(void ***)g_dev;   // the device's table now (it may be another)
        if (vt[17] != (void *)my_Present) {
            uintptr_t at;
            const char *m = module_of(vt[17], &at);
            hk_log("hiraku-pc: the game's Present was replaced (by %s+%lx): HIRAKU's put back, that one called by it", m, (unsigned long)at);
            g_Present = (Present_t)vt[17];
            patch_slot(&vt[17], (void *)my_Present);
        }
        if (vt[16] != (void *)my_Reset) {
            g_Reset = (Reset_t)vt[16];
            patch_slot(&vt[16], (void *)my_Reset);
        }
    }
}

static HRESULT WINAPI my_CreateDevice(IDirect3D9 *self, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                                      D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **dev) {
    hk_log("hiraku-pc: the game's Direct3D: %ux%u, %s, %u Hz, interval %#x", pp->BackBufferWidth, pp->BackBufferHeight,
           pp->Windowed ? "windowed" : "full screen", pp->FullScreen_RefreshRateInHz, pp->PresentationInterval);
    windowed(pp);
    const HWND w = pp->hDeviceWindow ? pp->hDeviceWindow : focus;
    place_window(w);
    HRESULT r = g_CreateDevice(self, adapter, type, focus, flags, pp, dev);
    // Windows' Direct3D 9 (not Wine's) said the device was lost (pop'n 17 on Windows 10, GitHub
    // issue 4): the screen held by another program for a moment, or a window never shown refused.
    // Again a few times, then with the hidden window shown off the screen; each try in the log.
    for (int i = 0; r == D3DERR_DEVICELOST && i < 10; i++) {
        Sleep(100);
        r = g_CreateDevice(self, adapter, type, focus, flags, pp, dev);
        hk_log("hiraku-pc: CreateDevice lost, again after %d ms: %#lx", 100 * (i + 1), (unsigned long)r);
    }
    if (r == D3DERR_DEVICELOST && w && host_shows()) {
        SetWindowPos(w, HWND_BOTTOM, -32000, -32000, g_game_w, g_game_h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        r = g_CreateDevice(self, adapter, type, focus, flags, pp, dev);
        hk_log("hiraku-pc: CreateDevice lost, again with the game's window shown off the screen: %#lx", (unsigned long)r);
        if (r != D3D_OK) ShowWindow(w, SW_HIDE);
        else g_shown_off = 1;
    }
    if (r == D3D_OK && dev && *dev) {
        void **vt = *(void ***)*dev;
        void *was = patch_slot(&vt[16], (void *)my_Reset);
        if (was) g_Reset = (Reset_t)was;
        was = patch_slot(&vt[17], (void *)my_Present);
        if (was) g_Present = (Present_t)was;
        hk_d3d9_hooks(*dev);
        if (!g_dev && host_shows()) {
            g_dev = *dev;
            IDirect3DDevice9_AddRef(g_dev);   // (the watcher's)
            CloseHandle(CreateThread(NULL, 0, present_watch, NULL, 0, NULL));
        }
        was = patch_slot(&vt[23], (void *)my_CreateTexture);
        if (was) g_CreateTexture = (CreateTexture_t)was;
    } else {
        hk_log("hiraku-pc: CreateDevice failed (%#lx)", (unsigned long)r);
    }
    return r;
}

// The game's D3DX effect (IIDX GOLD's): its OnResetDevice, called once after it is made, fails under
// Wine (Microsoft's d3dx9_24 says so for no reason it gives, Wine's own has it as a stub) and the
// game stops at an "OnResetDevice" message box. The effect's resources are made already, so a
// failure there is let pass (once in the log)
typedef HRESULT(WINAPI *effect_call_t)(void *);
static effect_call_t g_OnResetDevice;
static HRESULT WINAPI my_OnResetDevice(void *effect) {
    const HRESULT r = g_OnResetDevice(effect);
    static int said;
    if (FAILED(r) && !said++) hk_log("hiraku-pc: the game's D3DX effect's OnResetDevice said %#lx: let pass", (unsigned long)r);
    return FAILED(r) ? D3D_OK : r;
}
typedef HRESULT(WINAPI *D3DXCreateEffect_t)(void *, const void *, UINT, const void *, void *, DWORD, void *, void **, void **);
static HRESULT WINAPI my_D3DXCreateEffect(void *dev, const void *data, UINT n, const void *defines, void *include, DWORD flags,
                                          void *pool, void **effect, void **errors) {
    static D3DXCreateEffect_t real;
    if (!real) real = (D3DXCreateEffect_t)(void *)GetProcAddress(LoadLibraryA("d3dx9_24.dll"), "D3DXCreateEffect");
    const HRESULT r = real ? real(dev, data, n, defines, include, flags, pool, effect, errors) : E_FAIL;
    if (SUCCEEDED(r) && effect && *effect) {
        void **vt = *(void ***)*effect;   // ID3DXEffect: OnResetDevice is the 71st call
        void *was = patch_slot(&vt[70], (void *)my_OnResetDevice);
        if (was) g_OnResetDevice = (effect_call_t)was;
    }
    return r;
}

typedef IDirect3D9 *(WINAPI *Direct3DCreate9_t)(UINT);

static IDirect3D9 *WINAPI my_Direct3DCreate9(UINT sdk) {
    static Direct3DCreate9_t real;
    if (!real) real = (Direct3DCreate9_t)(void *)GetProcAddress(LoadLibraryA("d3d9.dll"), "Direct3DCreate9");
    IDirect3D9 *d = real ? real(sdk) : NULL;
    if (d) {
        void *was = patch_slot(&(*(void ***)d)[16], (void *)my_CreateDevice);
        if (was) g_CreateDevice = (CreateDevice_t)was;
    }
    return d;
}

// The game showing its window again, once HIRAKU's shows the pictures: it stays hidden
// The cabinet's graphics card (HK_PC_GPU_ID, VVVV:DDDD; hiraku-pc gives the IIDX games ATI's Radeon
// X1300, 1002:7146): IIDX EMPRESS reads the first display's device id and makes no Direct3D device
// unless it names one of the cabinets' cards (DEV_7146, or DEV_95C5 the HD 3450): "failed to get the
// device"
static BOOL WINAPI my_EnumDisplayDevicesA(LPCSTR name, DWORD i, PDISPLAY_DEVICEA dev, DWORD flags) {
    const BOOL r = EnumDisplayDevicesA(name, i, dev, flags);
    const char *id = getenv("HK_PC_GPU_ID");
    unsigned v, d;
    if (r && !name && dev && dev->cb >= sizeof *dev && id && sscanf(id, "%x:%x", &v, &d) == 2)
        snprintf(dev->DeviceID, sizeof dev->DeviceID, "PCI\\VEN_%04X&DEV_%04X&SUBSYS_00000000&REV_00", v, d);
    return r;
}

static BOOL WINAPI my_ShowWindow(HWND w, int how) {
    if (g_shown_off && w == g_hwnd) return TRUE;   // kept as it is, off the screen
    if (host_shows() && (w == g_hwnd || !g_hwnd)) how = SW_HIDE;
    return ShowWindow(w, how);
}

void hk_place_window(void *w) { place_window((HWND)w); }

// --- HK_PC_PROFILE: where the game's drawing thread (the one that counts the pictures) spends its
// time: sampled a thousand times a second, its instruction and its caller (the return address at
// [ebp+4]), the busiest places each 600 pictures (finding a frame clock that isn't a wait)

static volatile DWORD g_prof_thread;
static struct { uintptr_t eip, caller; DWORD n; } g_prof[512];
static volatile LONG g_prof_total;
static CRITICAL_SECTION g_prof_lock;

static int readable(uintptr_t p) {
    MEMORY_BASIC_INFORMATION mi;
    return p >= 0x10000 && VirtualQuery((void *)p, &mi, sizeof mi) && mi.State == MEM_COMMIT &&
           !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
}
static DWORD WINAPI profiler(LPVOID unused) {
    (void)unused;
    typedef UINT(WINAPI * period_t)(UINT);
    period_t begin = (period_t)(void *)GetProcAddress(LoadLibraryA("winmm.dll"), "timeBeginPeriod");
    if (begin) begin(1);
    HANDLE th = NULL;
    for (;;) {
        Sleep(1);
        if (!g_prof_thread) continue;
        if (!th && !(th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, g_prof_thread))) continue;
        if (SuspendThread(th) == (DWORD)-1) continue;
        CONTEXT c;
        c.ContextFlags = CONTEXT_CONTROL;
        uintptr_t eip = 0, caller = 0;
        if (GetThreadContext(th, &c)) {
            eip = c.Eip;
            if (readable(c.Ebp + 4)) caller = *(const uintptr_t *)(c.Ebp + 4);
        }
        ResumeThread(th);
        if (!eip) continue;
        EnterCriticalSection(&g_prof_lock);
        for (int i = 0; i < 512; i++)
            if (!g_prof[i].n || (g_prof[i].eip == eip && g_prof[i].caller == caller)) {
                g_prof[i].eip = eip, g_prof[i].caller = caller, g_prof[i].n++;
                break;
            }
        g_prof_total++;
        LeaveCriticalSection(&g_prof_lock);
    }
}
static void profile_frame(void) {
    static int on = -1;
    static DWORD frames;
    if (on < 0) {
        on = getenv("HK_PC_PROFILE") != NULL;
        if (on) {
            InitializeCriticalSection(&g_prof_lock);
            CloseHandle(CreateThread(NULL, 0, profiler, NULL, 0, NULL));
        }
    }
    if (!on) return;
    g_prof_thread = GetCurrentThreadId();
    if (++frames % 600) return;
    EnterCriticalSection(&g_prof_lock);
    const LONG total = g_prof_total;
    for (int k = 0; k < 15; k++) {   // the busiest, taken out one by one
        int best = -1;
        for (int i = 0; i < 512 && g_prof[i].n; i++)
            if (g_prof[i].n != (DWORD)-1 && (best < 0 || g_prof[i].n > g_prof[best].n)) best = i;
        if (best < 0) break;
        uintptr_t o1, o2;
        const char *m1 = module_of((void *)g_prof[best].eip, &o1);
        char m1c[64];
        snprintf(m1c, sizeof m1c, "%s", m1);
        const char *m2 = g_prof[best].caller ? module_of((void *)g_prof[best].caller, &o2) : "-";
        if (!g_prof[best].caller) o2 = 0;
        hk_log("profile: %4.1f %% at %s+%lx (from %s+%lx)", total ? 100.0 * g_prof[best].n / total : 0.0, m1c,
               (unsigned long)o1, m2, (unsigned long)o2);
        g_prof[best].n = (DWORD)-1;
    }
    memset(g_prof, 0, sizeof g_prof);
    g_prof_total = 0;
    LeaveCriticalSection(&g_prof_lock);
}

// --- HK_PC_WAITLOG: where the game waits between frame_go and its Present (finding its frame clock)

static struct { void *site; DWORD calls; double ms; } g_waits[16];

static DWORD g_draw_thread;   // the thread that counts the pictures (the game's drawing): its waits only

static void waited(void *site, LARGE_INTEGER t0) {
    if (g_draw_thread && GetCurrentThreadId() != g_draw_thread) return;
    LARGE_INTEGER t1, f;
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    const double ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
    for (int i = 0; i < 16; i++)
        if (g_waits[i].site == site || !g_waits[i].site) {
            g_waits[i].site = site, g_waits[i].calls++, g_waits[i].ms += ms;
            return;
        }
}

static void waits_report(void) {
    static DWORD frames;
    g_draw_thread = GetCurrentThreadId();
    if (++frames % 600) return;
    for (int i = 0; i < 16 && g_waits[i].site; i++)
        hk_log("waitlog: site %p: %lu calls, %.1f ms a frame", g_waits[i].site, g_waits[i].calls, g_waits[i].ms / 600);
    memset(g_waits, 0, sizeof g_waits);
}

// --- HK_PC_THREADLOG: the threads the game's modules start (where, and from where), for bringing a game up
static HANDLE WINAPI my_CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T stack, LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD flags,
                                     LPDWORD id) {
    DWORD tid = 0;
    HANDLE h = CreateThread(sa, stack, fn, arg, flags, &tid);
    hk_log("hiraku-pc: thread %04lx starts at %p (arg %p), from %p", (unsigned long)tid, (void *)fn, arg, __builtin_return_address(0));
    if (id) *id = tid;
    return h;
}

static void WINAPI my_Sleep(DWORD ms) {
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    Sleep(ms);
    waited(__builtin_return_address(0), t0);
}
static DWORD WINAPI my_WaitForSingleObject(HANDLE h, DWORD ms) {
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    const DWORD r = WaitForSingleObject(h, ms);
    waited(__builtin_return_address(0), t0);
    return r;
}
static DWORD WINAPI my_MsgWaitForMultipleObjects(DWORD n, const HANDLE *h, BOOL all, DWORD ms, DWORD mask) {
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    const DWORD r = MsgWaitForMultipleObjects(n, h, all, ms, mask);
    waited(__builtin_return_address(0), t0);
    return r;
}

// --- HK_PC_AVS_LOG: libavs's file and package calls (their arguments as text where they're text,
// and what they returned), for bringing up a game whose data comes through them

static int printable(const void *p) {
    if ((uintptr_t)p < 0x10000 || !present((const uint8_t *)p, 1)) return 0;
    const unsigned char *s = (const unsigned char *)p;
    int n = 0;
    while (n < 200 && present(s + n, 1) && s[n] >= 0x20 && s[n] < 0x7f) n++;
    return n >= 1 && present(s + n, 1) && s[n] == 0;
}

static void avs_said(const char *fn, void *const *a, intptr_t r) {
    // the status calls a game polls every frame: said when their result changes
    static const char *const polled[] = {"ea3_get_status", "avs_keepalive_get_status", "ea3_get_boot_status", "avs_net_ctrl",
                                         "sonet_finish"};
    static intptr_t last[5] = {-12345, -12345, -12345, -12345, -12345};
    for (int i = 0; i < 5; i++)
        if (!strcmp(fn, polled[i])) {
            intptr_t v = r;
            if (i == 0 && present((const uint8_t *)a[0], 4) && present((const uint8_t *)a[1], 4))
                v = r * 1000003 + *(const int *)a[0] * 1009 + *(const int *)a[1];
            if (i == 4) v = r * 1000003 + (intptr_t)a[1];   // sonet_finish: each request's result
            if (last[i] == v) return;
            last[i] = v;
        }
    char line[1024];
    int n = snprintf(line, sizeof line, "avs: %s(", fn);
    for (int i = 0; i < 5 && n < 900; i++)
        n += printable(a[i]) ? snprintf(line + n, sizeof line - (size_t)n, "%s\"%s\"", i ? ", " : "", (const char *)a[i])
                             : snprintf(line + n, sizeof line - (size_t)n, "%s%p", i ? ", " : "", a[i]);
    if (!strcmp(fn, "ea3_get_status") && present((const uint8_t *)a[0], 4) && present((const uint8_t *)a[1], 4))
        n += snprintf(line + n, sizeof line - (size_t)n, " -> %d, %d", *(const int *)a[0], *(const int *)a[1]);
    hk_log("%s) = %ld", line, (long)r);
}

typedef intptr_t(__cdecl *avs6_t)(void *, void *, void *, void *, void *, void *);
#define AVS_LOGGED(i)                                                                                          \
    static avs6_t g_avs_real##i;                                                                               \
    static const char *g_avs_name##i;                                                                          \
    static intptr_t __cdecl avs_wrap##i(void *a, void *b, void *c, void *d, void *e, void *f) {               \
        void *args[5] = {a, b, c, d, e};                                                                       \
        const intptr_t r = g_avs_real##i(a, b, c, d, e, f);                                                    \
        avs_said(g_avs_name##i, args, r);                                                                      \
        return r;                                                                                              \
    }
AVS_LOGGED(0) AVS_LOGGED(1) AVS_LOGGED(2) AVS_LOGGED(3) AVS_LOGGED(4) AVS_LOGGED(5) AVS_LOGGED(6) AVS_LOGGED(7)
AVS_LOGGED(8) AVS_LOGGED(9) AVS_LOGGED(10) AVS_LOGGED(11) AVS_LOGGED(12) AVS_LOGGED(13) AVS_LOGGED(14)
AVS_LOGGED(15) AVS_LOGGED(16)

// libavs's own log (HK_PC_AVS_LOG): the game boots it with a writer of its own, often nowhere; HIRAKU's
// takes the lines into pc.log ("avs log:"), at the game's level or the one HK_PC_AVS_LOG names (a
// number past 1: log_change_level's). The writer's arguments: (context, text, length) in the older libavs (IIDX
// GOLD's), (text, length, context) in the later; the length is the small one
static void __cdecl avs_writer(void *a, void *b, void *c) {
    const char *t = (uintptr_t)b < 0x10000 ? (const char *)a : (const char *)b;
    unsigned n = (unsigned)(uintptr_t)((uintptr_t)b < 0x10000 ? b : c);
    while (n && (t[n - 1] == '\n' || t[n - 1] == '\r')) n--;
    hk_log("avs log: %.*s", (int)n, t);
}
typedef void(__cdecl *avs_boot_t)(void *, void *, size_t, void *, size_t, void *, void *);
static avs_boot_t g_avs_boot;
static void __cdecl my_avs_boot(void *config, void *std_heap, size_t n_std, void *avs_heap, size_t n_avs, void *writer, void *ctx) {
    (void)writer;
    g_avs_boot(config, std_heap, n_std, avs_heap, n_avs, (void *)avs_writer, ctx);
    typedef void(__cdecl * level_t)(int);
    const level_t level = (level_t)(void *)GetProcAddress(GetModuleHandleA("libavs-win32.dll"), "log_change_level");
    const int l = atoi(getenv("HK_PC_AVS_LOG"));
    if (level && l > 1) level(l);
}

// DistorteD's libavs (older) starts its log alone: log_boot(writer, context)
typedef void(__cdecl *log_boot_t)(void *, void *);
static log_boot_t g_log_boot;
static void __cdecl my_log_boot(void *writer, void *ctx) {
    (void)writer;
    g_log_boot((void *)avs_writer, ctx);
    typedef void(__cdecl * level_t)(int);
    const level_t level = (level_t)(void *)GetProcAddress(GetModuleHandleA("libavs-win32.dll"), "log_change_level");
    const int l = atoi(getenv("HK_PC_AVS_LOG"));
    if (level && l > 1) level(l);
}

static void *avs_logged(const char *dll, const char *fn) {
    static const char *const names[] = {"avs_fs_mount", "avs_fs_open", "avs_fs_lstat", "avs_fs_opendir", "avs_fs_umount",
                                        "ea3_pkglist_open", "ea3_pkglist_get", "ea3_pkglist_next", "ea3_pkg_open_by_name",
                                        "ea3_pkg_read", "ea3_boot", "ea3_get_boot_status", "ea3_get_status",
                                        "avs_keepalive_get_status", "avs_keepalive_find_by_name",
                                        "sonet_request", "sonet_finish"};   // IIDX 9th style's network glue
    static avs6_t *const real[] = {&g_avs_real0, &g_avs_real1, &g_avs_real2, &g_avs_real3, &g_avs_real4, &g_avs_real5,
                                   &g_avs_real6, &g_avs_real7, &g_avs_real8, &g_avs_real9, &g_avs_real10, &g_avs_real11,
                                   &g_avs_real12, &g_avs_real13, &g_avs_real14, &g_avs_real15, &g_avs_real16};
    static const char **const name[] = {&g_avs_name0, &g_avs_name1, &g_avs_name2, &g_avs_name3, &g_avs_name4, &g_avs_name5,
                                        &g_avs_name6, &g_avs_name7, &g_avs_name8, &g_avs_name9, &g_avs_name10, &g_avs_name11,
                                        &g_avs_name12, &g_avs_name13, &g_avs_name14, &g_avs_name15, &g_avs_name16};
    static void *const wrap[] = {(void *)avs_wrap0, (void *)avs_wrap1, (void *)avs_wrap2, (void *)avs_wrap3,
                                 (void *)avs_wrap4, (void *)avs_wrap5, (void *)avs_wrap6, (void *)avs_wrap7,
                                 (void *)avs_wrap8, (void *)avs_wrap9, (void *)avs_wrap10, (void *)avs_wrap11,
                                 (void *)avs_wrap12, (void *)avs_wrap13, (void *)avs_wrap14, (void *)avs_wrap15,
                                 (void *)avs_wrap16};
    static int on = -1;
    if (on < 0) on = getenv("HK_PC_AVS_LOG") != NULL;
    if (!on) return NULL;
    if (!strcmp(fn, "avs_boot") && !_stricmp(dll, "libavs-win32.dll")) {
        if (!g_avs_boot) g_avs_boot = (avs_boot_t)(void *)GetProcAddress(GetModuleHandleA(dll), fn);
        return g_avs_boot ? (void *)my_avs_boot : NULL;
    }
    if (!strcmp(fn, "log_boot") && !_stricmp(dll, "libavs-win32.dll")) {
        if (!g_log_boot) g_log_boot = (log_boot_t)(void *)GetProcAddress(GetModuleHandleA(dll), fn);
        return g_log_boot ? (void *)my_log_boot : NULL;
    }
    for (int i = 0; i < (int)(sizeof names / sizeof *names); i++)
        if (!strcmp(fn, names[i])) {
            if (!*real[i]) *real[i] = (avs6_t)(void *)GetProcAddress(GetModuleHandleA(dll), fn), *name[i] = names[i];
            return *real[i] ? wrap[i] : NULL;
        }
    return NULL;
}

// --- hooking a module's imports

static HMODULE g_self;

void *hk_program(void) {
    static HMODULE program;
    if (!program) {
        const char *name = getenv("HK_PC_PROGRAM");
        if (name && *name) program = GetModuleHandleA(name);
        if (!program) program = GetModuleHandleA(NULL);
    }
    return program;
}

// HK_PC_STDOUT (hk_cabinet_start): the game's printf, a line at a time to pc.log
static HANDLE g_stdout;
static BOOL WINAPI my_WriteFile(HANDLE h, LPCVOID buf, DWORD n, LPDWORD done, LPOVERLAPPED ov) {
    if (h != g_stdout || !buf) return WriteFile(h, buf, n, done, ov);
    static char line[512];
    static size_t len;
    for (DWORD i = 0; i < n; i++) {
        const char c = ((const char *)buf)[i];
        if (c == '\n' || len + 1 >= sizeof line) {
            line[len] = 0;
            if (len) hk_log("game: %s", line);
            len = 0;
        }
        if (c != '\n' && c != '\r') line[len++] = c;
    }
    if (done) *done = n;
    return TRUE;
}

// HK_PC_XRPC_LOG (bring-up, IIDX DistorteD's eam3lib): what the game's answer handlers read of an
// answer (each attribute asked for and what it was), their element depths and the statuses they set
typedef const char *(__cdecl *attr_get_t)(void *, const char *);
typedef int(__cdecl *stack_t)(void *);
typedef int(__cdecl *status_set_t)(void *, int);
static attr_get_t g_attr_get;
static stack_t g_stack_peek;
static status_set_t g_status_set;
static int xrpc_said(void) {
    static volatile LONG n;
    return InterlockedIncrement(&n) <= 600;
}
static const char *__cdecl dbg_attr_get(void *node, const char *name) {
    const char *v = g_attr_get(node, name);
    if (xrpc_said()) hk_log("xrpc: %p attr %s = %s (from %p)", node, name ? name : "?", v ? v : "(none)", __builtin_return_address(0));
    return v;
}
static int __cdecl dbg_stack_peek(void *st) {
    const int v = g_stack_peek(st);
    if (xrpc_said()) hk_log("xrpc: depth %d (from %p)", v, __builtin_return_address(0));
    return v;
}
static int __cdecl dbg_status_set(void *packet, int code) {
    if (xrpc_said()) hk_log("xrpc: status set %d (from %p)", code, __builtin_return_address(0));
    return g_status_set(packet, code);
}
static void *xrpc_logged(const char *dll, const char *fn) {
    static int on = -1;
    if (on < 0) on = getenv("HK_PC_XRPC_LOG") != NULL;
    if (!on || _stricmp(dll, "eam3lib.dll")) return NULL;
    HMODULE m = GetModuleHandleA(dll);
    if (!strcmp(fn, "xml_attr_getvalue_byname")) return (g_attr_get = (attr_get_t)(void *)GetProcAddress(m, fn)) ? (void *)dbg_attr_get : NULL;
    if (!strcmp(fn, "minstack_peek")) return (g_stack_peek = (stack_t)(void *)GetProcAddress(m, fn)) ? (void *)dbg_stack_peek : NULL;
    if (!strcmp(fn, "xrpc_status_set")) return (g_status_set = (status_set_t)(void *)GetProcAddress(m, fn)) ? (void *)dbg_status_set : NULL;
    return NULL;
}

static void *cabinet_hook_for(const char *dll, const char *fn) {
    if (!_stricmp(dll, "eam3lib.dll")) return xrpc_logged(dll, fn);
    if (!_stricmp(dll, "ws2_32.dll") || !_stricmp(dll, "wsock32.dll")) return hk_net_hook(fn);   // net.c
    if (!_stricmp(dll, "iphlpapi.dll")) return hk_net_card_hook(fn);
    if (!_stricmp(dll, "libsocket.dll") || !_stricmp(dll, "libavs-win32.dll") || !_stricmp(dll, "libavs-win32-ea3.dll")) {
        void *v = hk_vsock_hook(fn);   // vsock.c
        return v ? v : avs_logged(dll, fn);
    }
    if (!_stricmp(dll, "libcomm.dll")) return hk_comm_hook(fn);
    if (!_stricmp(dll, "winmm.dll") && !strncmp(fn, "mixer", 5)) return host_shows() ? hk_mixer_hook(fn) : NULL;   // dsound.c
    if (!_stricmp(dll, "sonet.dll")) return avs_logged(dll, fn);
    if (g_waitlog < 0) g_waitlog = getenv("HK_PC_WAITLOG") != NULL;
    if (g_waitlog && !_stricmp(dll, "kernel32.dll") && !strcmp(fn, "Sleep")) return (void *)my_Sleep;
    if (g_waitlog && !_stricmp(dll, "kernel32.dll") && !strcmp(fn, "WaitForSingleObject")) return (void *)my_WaitForSingleObject;
    if (g_waitlog && !_stricmp(dll, "user32.dll") && !strcmp(fn, "MsgWaitForMultipleObjects")) return (void *)my_MsgWaitForMultipleObjects;
    if (!_stricmp(dll, "kernel32.dll") && !strcmp(fn, "CreateThread") && getenv("HK_PC_THREADLOG")) return (void *)my_CreateThread;
    if (!_stricmp(dll, "kernel32.dll") && !strcmp(fn, "WriteFile") && g_stdout) return (void *)my_WriteFile;
    static const struct { const char *dll, *fn; void *to; } hooks[] = {
        {"kernel32.dll", "CreateFileA", (void *)my_CreateFileA},
        {"kernel32.dll", "CreateDirectoryA", (void *)my_CreateDirectoryA},
        {"kernel32.dll", "FindFirstFileA", (void *)my_FindFirstFileA},
        {"kernel32.dll", "GetFileAttributesA", (void *)my_GetFileAttributesA},
        {"kernel32.dll", "DeleteFileA", (void *)my_DeleteFileA},
        {"kernel32.dll", "RemoveDirectoryA", (void *)my_RemoveDirectoryA},
        {"kernel32.dll", "MoveFileA", (void *)my_MoveFileA},
        {"kernel32.dll", "GetFullPathNameA", (void *)my_GetFullPathNameA},
        {"d3d9.dll", "Direct3DCreate9", (void *)my_Direct3DCreate9},
        {"d3d8.dll", "Direct3DCreate8", (void *)hk_Direct3DCreate8},   // d3d8.c
        {"d3dx9_24.dll", "D3DXCreateEffect", (void *)my_D3DXCreateEffect},
        {"user32.dll", "ShowWindow", (void *)my_ShowWindow},
        {"user32.dll", "EnumDisplayDevicesA", (void *)my_EnumDisplayDevicesA},
        {"dsound.dll", "DirectSoundCreate8", (void *)hk_DirectSoundCreate8},
        {"dsound.dll", "DirectSoundCreate", (void *)hk_DirectSoundCreate},
    };
    for (size_t i = 0; i < sizeof hooks / sizeof *hooks; i++)
        if (!_stricmp(dll, hooks[i].dll) && !strcmp(fn, hooks[i].fn)) {
            if (hooks[i].to == (void *)my_Direct3DCreate9 && !g_win_w && !g_win_full && g_hz <= 0 && !host_shows()) return NULL;
            if (hooks[i].to == (void *)hk_Direct3DCreate8 && !host_shows()) return NULL;   // HIRAKU's window only
            if (hooks[i].dll[0] == 'k' && !g_nmap && !g_nfiles) return NULL;
            // the window and the sound are HIRAKU's only when its window shows the game
            if ((!_stricmp(hooks[i].dll, "user32.dll") || !_stricmp(hooks[i].dll, "dsound.dll")) && !host_shows()) return NULL;
            return hooks[i].to;
        }
    return NULL;
}

// A device of HIRAKU's own under its driver (ezusb_dev.c): it sees each import first, and is handed
// what the cabinet would put there (NULL: the call itself)
static void *(*g_device_hook)(const char *dll, const char *fn, void *next);
void hk_set_device_hook(void *(*hook)(const char *dll, const char *fn, void *next)) { g_device_hook = hook; }

static void *hook_for(const char *dll, const char *fn) {
    void *to = cabinet_hook_for(dll, fn);
    if (g_device_hook) {
        void *d = g_device_hook(dll, fn, to);
        if (d) return d;
    }
    return to;
}

// The calls the hooks take that games import by number
static const char *by_ordinal(const char *dll, unsigned ord) {
    static const struct { const char *dll; unsigned ord; const char *fn; } names[] = {
        {"dsound.dll", 1, "DirectSoundCreate"}, {"dsound.dll", 11, "DirectSoundCreate8"},
        {"ws2_32.dll", 2, "bind"},              {"ws2_32.dll", 3, "closesocket"},       {"ws2_32.dll", 4, "connect"},
        {"ws2_32.dll", 17, "recvfrom"},         {"ws2_32.dll", 20, "sendto"},
        {"ws2_32.dll", 23, "socket"},           {"ws2_32.dll", 52, "gethostbyname"},
        {"wsock32.dll", 2, "bind"},             {"wsock32.dll", 3, "closesocket"},      {"wsock32.dll", 4, "connect"},
        {"wsock32.dll", 17, "recvfrom"},        {"wsock32.dll", 20, "sendto"},
        {"wsock32.dll", 23, "socket"},          {"wsock32.dll", 52, "gethostbyname"},
    };
    for (size_t i = 0; i < sizeof names / sizeof *names; i++)
        if (names[i].ord == ord && !_stricmp(dll, names[i].dll)) return names[i].fn;
    return NULL;
}

static void patch_imports(HMODULE m) {
    char *base = (char *)m;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY *dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir->VirtualAddress) return;
    for (IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir->VirtualAddress); imp->Name; imp++) {
        const char *dll = base + imp->Name;
        IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        IMAGE_THUNK_DATA *addrs = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, addrs++) {
            const char *fn;
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {   // by number: dsound.dll's and ws2_32.dll's (pop'n 15)
                fn = by_ordinal(dll, (unsigned)IMAGE_ORDINAL(names->u1.Ordinal));
                if (!fn) continue;
            } else {
                fn = (const char *)((IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData))->Name;
            }
            void *to = hook_for(dll, fn);
            if (to) patch_slot((void **)&addrs->u1.Function, to);
        }
    }
}

// The game's own modules: the program and the DLLs in its folder (not Windows', not ours)
static void patch_game_modules(void) {
    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    char *slash = strrchr(exe, '\\');
    const size_t dirlen = slash ? (size_t)(slash - exe) : 0;
    HMODULE mods[256];
    DWORD bytes = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &bytes)) return;
    for (DWORD i = 0; i < bytes / sizeof *mods; i++) {
        char p[MAX_PATH];
        if (mods[i] == g_self || !GetModuleFileNameA(mods[i], p, MAX_PATH)) continue;
        if (mods[i] != GetModuleHandleA(NULL) && (strlen(p) <= dirlen || _strnicmp(p, exe, dirlen) || p[dirlen] != '\\'))
            continue;
        patch_imports(mods[i]);
    }
}

// Modules the game loads later (LoadLibrary) are its own too
static HMODULE WINAPI my_LoadLibraryA(LPCSTR p) {
    HMODULE m = LoadLibraryA(p);
    if (m) patch_game_modules();
    return m;
}

// The game's own frame clock: pop'n 15 waits, before each Present, until 14 ms have passed since the
// last one (timeGetTime, then Sleep: `call reg; sub eax, last; cmp eax, 14; jae go_on`; IIDX RED 13 ms). With HIRAKU's
// pacing starting each frame (link.h), that wait would hold the picture until the frame's end, so
// the branch is made to always go on. Found by its shape and the game's own import slots (every
// program date of pop'n 15 has two, at different places). IIDX EMPRESS's is the same 14 ms with
// SleepEx, 2 ms at least: on Windows its sleep rounded up to the timer's 15.6 ms, every picture a
// refresh late (a cabinet's EMPRESS ran slow and hitched every few seconds)
static void *import_slot(HMODULE m, const char *dll, const char *fn) {
    char *base = (char *)m;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY *dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir->VirtualAddress); dir->VirtualAddress && imp->Name; imp++) {
        if (_stricmp(base + imp->Name, dll)) continue;
        IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        IMAGE_THUNK_DATA *addrs = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, addrs++)
            if (!IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal) &&
                !strcmp((const char *)((IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData))->Name, fn))
                return &addrs->u1.Function;
    }
    return NULL;
}

static int mentions(const uint8_t *p, size_t n, const void *slot) {
    const uint32_t a = (uint32_t)(uintptr_t)slot;
    for (size_t i = 0; i + 4 <= n; i++)
        if (!memcmp(p + i, &a, 4)) return 1;
    return 0;
}

static void unlimit_frames(void) {
    HMODULE exe = (HMODULE)hk_program();
    void *tgt = import_slot(exe, "winmm.dll", "timeGetTime"), *sleep = import_slot(exe, "kernel32.dll", "Sleep");
    void *sleep_ex = import_slot(exe, "kernel32.dll", "SleepEx");
    if (!tgt || (!sleep && !sleep_ex)) return;
    char *base = (char *)exe;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    int n = 0;
    for (WORD k = 0; k < nt->FileHeader.NumberOfSections; k++, sec++) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uint8_t *code = (uint8_t *)base + sec->VirtualAddress;
        const size_t size = sec->Misc.VirtualSize;
        for (size_t i = 16; i + 40 < size; i++) {
            uint8_t *p = code + i;
            // cmp eax, 12-15 ms (pop'n 15: 14; IIDX RED: 13); jae
            if (p[0] != 0x83 || p[1] != 0xf8 || p[2] < 0x0c || p[2] > 0x0f || p[3] != 0x73) continue;
            // before it: timeGetTime's slot loaded, a call through a register, a subtraction
            int call = 0, sub = 0;
            for (int b = 1; b <= 8; b++) call |= p[-b] == 0xff && (p[-b + 1] & 0xf8) == 0xd0, sub |= p[-b] == 0x2b;
            if (!call || !sub || !mentions(p - 16, 16, tgt)) continue;
            // what it skips: a Sleep (or SleepEx)
            const size_t skipped = p[4] < 32 ? p[4] : 32;
            if (!(sleep && mentions(p + 5, skipped, sleep)) && !(sleep_ex && mentions(p + 5, skipped, sleep_ex))) continue;
            DWORD old;
            VirtualProtect(p + 3, 1, PAGE_EXECUTE_READWRITE, &old);
            p[3] = 0xeb;   // jae -> jmp
            VirtualProtect(p + 3, 1, old, &old);
            FlushInstructionCache(GetCurrentProcess(), p, 4);
            hk_log("hiraku-pc: the game's own %d ms wait between pictures at %p: left to HIRAKU's pacing", p[2], (void *)p);
            n++;
        }
    }
    if (!n) hk_log("hiraku-pc: no frame wait of the game's own found (its pictures may come late in the frame)");
}

// A fault's place and the way to it, in the log (the game's own handler ends it quietly): the module
// and registers, and the return addresses on the stack that are in the program's code
static LONG CALLBACK crash_log(EXCEPTION_POINTERS *e) {
    const DWORD code = e->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != EXCEPTION_STACK_OVERFLOW)
        return EXCEPTION_CONTINUE_SEARCH;
    static int said;
    if (said++ >= 4) return EXCEPTION_CONTINUE_SEARCH;
    const CONTEXT *c = e->ContextRecord;
    hk_log("crash: %08lx at %08lx (address %08lx), eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx ebp=%08lx esp=%08lx",
           code, (unsigned long)c->Eip,
           e->ExceptionRecord->NumberParameters > 1 ? (unsigned long)e->ExceptionRecord->ExceptionInformation[1] : 0ul,
           c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    HMODULE in = NULL;   // where: the program, its DLLs, Direct3D's, the graphics driver's
    char path[MAX_PATH] = "?";
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)c->Eip, &in);
    if (in) GetModuleFileNameA(in, path, sizeof path);
    const char *name = strrchr(path, '\\');
    hk_log("crash: in %s+%lx", name ? name + 1 : path, in ? (unsigned long)(c->Eip - (uintptr_t)in) : 0ul);
    char *base = (char *)hk_program();
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    const uintptr_t lo = (uintptr_t)base + nt->OptionalHeader.BaseOfCode, hi = lo + nt->OptionalHeader.SizeOfCode;   // its code
    char line[1024];
    int n = 0;
    const uint32_t *sp = (const uint32_t *)c->Esp;
    for (int i = 0; i < 2048 && n < (int)sizeof line - 16 && present((const uint8_t *)(sp + i), 4); i++)
        if (sp[i] > lo && sp[i] < hi) n += snprintf(line + n, sizeof line - (size_t)n, " %08x", sp[i]);
    hk_log("crash: the program's addresses on the stack:%s", n ? line : " none");
    return EXCEPTION_CONTINUE_SEARCH;
}

void hk_cabinet_start(void) {
    if (GetEnvironmentVariableA("HK_PC_STARTED", NULL, 0)) return;   // the other DLL did
    SetEnvironmentVariableA("HK_PC_STARTED", "1");
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)(void *)hk_cabinet_start, &g_self);
    // a fault: the first few in pc.log (on Windows a game that stops leaves nothing else; one the
    // game handles itself is told too)
    AddVectoredExceptionHandler(1, crash_log);
    // HK_PC_STDOUT=1 (bring-up): the game's own printf (its network library's messages, beatmania
    // IIDX's), which a window program has nowhere to write: the NUL device as its output, made before
    // its C library starts (a character device: written at each printf), each write put in pc.log
    // (my_WriteFile)
    if (getenv("HK_PC_STDOUT")) {
        g_stdout = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (g_stdout != INVALID_HANDLE_VALUE) SetStdHandle(STD_OUTPUT_HANDLE, g_stdout), SetStdHandle(STD_ERROR_HANDLE, g_stdout);
    }
    read_map();
    read_files();
    read_patches();
    const char *w = getenv("HK_PC_WINDOW");
    if (w && !strcmp(w, "full")) g_win_full = 1;
    else if (w && sscanf(w, "%dx%d", &g_win_w, &g_win_h) != 2) g_win_w = g_win_h = 0;
    const char *hz = getenv("HK_PC_HZ");
    if (hz && *hz) g_hz = atof(hz);
    hk_link();
    if (host_shows()) unlimit_frames();
    hk_vsock_patch_program();
    hk_press_patch_program();
    if (g_npatches) apply_patches();   // the program's own code and data, as it's loaded
    patch_game_modules();
    // LoadLibraryA too, in the program, so modules loaded later get the same
    HMODULE exe = GetModuleHandleA(NULL);
    char *base = (char *)exe;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY *dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir->VirtualAddress); dir->VirtualAddress && imp->Name; imp++) {
        if (_stricmp(base + imp->Name, "kernel32.dll")) continue;
        IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        IMAGE_THUNK_DATA *addrs = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, addrs++)
            if (!IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal) &&
                !strcmp((const char *)((IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData))->Name, "LoadLibraryA"))
                patch_slot((void **)&addrs->u1.Function, (void *)my_LoadLibraryA);
    }
    hk_log("hiraku-pc: the cabinet is HIRAKU's (%s)", g_link == &g_alone ? "keyboard only" : "linked to hiraku-pc");
}
