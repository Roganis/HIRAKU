// SPDX-License-Identifier: GPL-3.0-or-later
#include "cabinet.h"
#include "imagebase.h"
#include "../shell/wintimer.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
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

// The pad's keys (link.h): pop'n's 9 buttons (bits 8-16), or IIDX's 14 keys (8-21, HK_PC_KEYS=iidx), or
// the ee'MALL station's two (0-1, HK_PC_KEYS=c33)
static uint32_t key_bits(void) {
    static uint32_t bits;
    if (!bits) {
        const char *k = getenv("HK_PC_KEYS");
        bits = k && !strcmp(k, "iidx") ? 0x3fff00u : k && !strcmp(k, "c33") ? 0x3u : 0x1ff00u;   // (c33: ee'MALL's A and B)
    }
    return bits;
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
    return downs & ~l->pad & key_bits();
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
    if (b & ~was & key_bits()) l->key_edges = l->key_edges + 1;   // a button down: its sound is the next started
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

const char *hk_magnetic_card_file(int u) {
    static char path[2][MAX_PATH];
    const uint8_t *uid = u ? hk_link()->card_uid2 : hk_link()->card_uid;
    const char *dir = getenv("HK_PC_CARDS");
    int any = 0;
    for (int i = 0; i < 8; i++) any |= uid[i];
    if (!dir || !*dir || !any) return getenv(u ? "HK_PC_CARD2" : "HK_PC_CARD1");
    snprintf(path[u & 1], MAX_PATH, "%s\\%02X%02X%02X%02X%02X%02X%02X%02X.bin", dir, uid[0], uid[1], uid[2], uid[3], uid[4],
             uid[5], uid[6], uid[7]);
    return path[u & 1];
}

// A save (a magnetic card): n bytes as the file at `path`, whole or not at all. Written beside it,
// sent to the disk and checked, then put in its place: a full or failing disk keeps the card from
// before (opened "wb" it would be left empty, and the next read take it for a new card). 1: written
int hk_write_file(const char *path, const void *data, size_t n) {
    char tmp[MAX_PATH + 8];
    if (!path || !*path || snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) return 0;
    HANDLE h = CreateFileA(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD put = 0;
    int ok = WriteFile(h, data, (DWORD)n, &put, NULL) && put == n && FlushFileBuffers(h);
    ok = CloseHandle(h) && ok;
    if (ok && MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) return 1;
    DeleteFileA(tmp);
    hk_log("hiraku-pc: cannot write %s: the one from before is kept", path);
    return 0;
}

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

// --- mods (docs/MODS.md): the game's files a mod's file takes the place of, and the files and
// folders a mod adds to the game's data (HK_PC_FILES: "PATH<tab>MOD", "PATH<tab>MOD<tab>new",
// "PATH<tab>MOD<tab>folder", PATH as the game names it). A path is looked for as the table keeps
// it (key_of): "..\data\sd_data\\x.bin", "..\DATA/sd_data/x.bin" and "..\data\.\sd_data\x.bin\"
// are one file. Several thousand of them: a hash table, not a list gone through at each call

enum { FILE_REPLACED, FILE_NEW, FOLDER_NEW };
static struct mod_file {
    char *key, *mod, *name;   // the path as the table keeps it; the mod's file; the name in its folder (as given)
    unsigned hash;
    int kind, said, folder, next;   // folder: its folder's in g_folders; next: the next one there (-1: none)
} *g_files;
static int g_nfiles, g_nnew;
static int *g_file_slots;   // open addressing: an entry's number + 1, 0 empty
static unsigned g_file_mask;
// the folders with a mod's files in them (their listings show the new names too, and the replaced
// files' sizes): key, the first of those files (in g_files)
static struct mod_folder { char *key; unsigned hash; int first; } *g_folders;
static int g_nfolders;
static int *g_folder_slots;
static unsigned g_folder_mask;
static CRITICAL_SECTION g_listings_lock;

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

// p as the table keeps a path, in out (cap bytes): lower case, one '\' between parts (a '/' is one
// too), no "." parts, a "name\.." pair taken out (the ".." the game starts its paths with kept),
// none at the end; one '\' kept at the start (a path from the drive's root). -1: too long. Names in
// the PC's code page: a double-byte letter's second byte is never a separator
static int hk_path_key(const char *p, char *out, size_t cap) {
    size_t n = 0;
    int parts = 0, ups = 0;   // parts in out; how many of the first are ".."
    if (*p == '\\' || *p == '/') {
        if (cap < 2) return -1;
        out[n++] = '\\';
    }
    const size_t root = n;
    while (*p) {
        while (*p == '\\' || *p == '/') p++;
        const char *b = p;
        while (*p && *p != '\\' && *p != '/') p += IsDBCSLeadByte((BYTE)*p) && p[1] ? 2 : 1;
        const size_t len = (size_t)(p - b);
        if (!len || (len == 1 && b[0] == '.')) continue;
        if (len == 2 && b[0] == '.' && b[1] == '.' && parts > ups) {
            size_t last = n;   // out's last part goes (a drive stays: "D:\.." is "D:\")
            while (last > root && out[last - 1] != '\\') last--;
            if (out[n - 1] == ':') continue;
            n = last > root ? last - 1 : root;
            parts--;
            continue;
        }
        if (n + (parts ? 1 : 0) + len + 1 > cap) return -1;
        if (parts) out[n++] = '\\';
        for (size_t i = 0; i < len; i++) {
            const char c = b[i];
            const int lead = IsDBCSLeadByte((BYTE)c) && i + 1 < len;
            out[n++] = !lead && c >= 'A' && c <= 'Z' ? (char)(c | 0x20) : c;
            if (lead) out[n++] = b[++i];
        }
        if (len == 2 && b[0] == '.' && b[1] == '.') ups++;
        parts++;
    }
    out[n] = 0;
    return (int)n;
}

static unsigned key_hash(const char *k) {
    unsigned h = 2166136261u;
    while (*k) h = (h ^ (unsigned char)*k++) * 16777619u;
    return h;
}

static int file_at(const char *key, unsigned h) {
    if (!g_file_slots) return -1;
    for (unsigned i = h & g_file_mask;; i = (i + 1) & g_file_mask) {
        const int e = g_file_slots[i] - 1;
        if (e < 0) return -1;
        if (g_files[e].hash == h && !strcmp(g_files[e].key, key)) return e;
    }
}

static int folder_at(const char *key, unsigned h) {
    if (!g_folder_slots) return -1;
    for (unsigned i = h & g_folder_mask;; i = (i + 1) & g_folder_mask) {
        const int e = g_folder_slots[i] - 1;
        if (e < 0) return -1;
        if (g_folders[e].hash == h && !strcmp(g_folders[e].key, key)) return e;
    }
}

static void read_files(void) {
    InitializeCriticalSection(&g_listings_lock);
    char *t = read_text(getenv("HK_PC_FILES"));
    int lines = 0;
    for (char *c = t; c && *c; c++) lines += *c == '\n';
    g_files = (struct mod_file *)calloc((size_t)lines + 1, sizeof *g_files);
    for (char *line = t ? strtok(t, "\r\n") : NULL; line; line = strtok(NULL, "\r\n")) {
        char *tab = strchr(line, '\t');
        if (!tab || g_nfiles > lines) continue;
        *tab = 0;
        char *kind = strchr(tab + 1, '\t');
        if (kind) *kind++ = 0;
        char key[MAX_PATH];
        if (hk_path_key(line, key, sizeof key) <= 0) continue;
        struct mod_file *f = &g_files[g_nfiles];
        f->key = _strdup(key), f->mod = tab + 1, f->hash = key_hash(key), f->folder = f->next = -1;
        f->kind = !kind ? FILE_REPLACED : !strcmp(kind, "new") ? FILE_NEW : !strcmp(kind, "folder") ? FOLDER_NEW : -1;
        if (f->kind < 0) continue;   // (a kind of a later HIRAKU's)
        const char *slash = strrchr(line, '\\'), *fwd = strrchr(line, '/');
        f->name = (char *)(fwd > slash ? fwd + 1 : slash ? slash + 1 : line);
        g_nfiles++;
    }
    if (!g_nfiles) return;
    // the tables: at most half full
    unsigned size = 16;
    while (size < 2u * (unsigned)g_nfiles) size *= 2;
    g_file_slots = (int *)calloc(size, sizeof *g_file_slots), g_file_mask = size - 1;
    g_folder_slots = (int *)calloc(size, sizeof *g_folder_slots), g_folder_mask = size - 1;
    g_folders = (struct mod_folder *)calloc((size_t)g_nfiles, sizeof *g_folders);
    int replaced = 0, folders = 0;
    for (int e = 0; e < g_nfiles; e++) {
        struct mod_file *f = &g_files[e];
        if (file_at(f->key, f->hash) >= 0) continue;   // (hiraku-pc lists each once)
        unsigned i = f->hash & g_file_mask;
        while (g_file_slots[i]) i = (i + 1) & g_file_mask;
        g_file_slots[i] = e + 1;
        replaced += f->kind == FILE_REPLACED, folders += f->kind == FOLDER_NEW, g_nnew += f->kind == FILE_NEW;
        // its folder's listing shows a new name, and a replaced file's size
        char *cut = strrchr(f->key, '\\');
        if (!cut) continue;
        *cut = 0;
        const unsigned h = key_hash(f->key);
        int d = folder_at(f->key, h);
        if (d < 0) {
            d = g_nfolders++;
            g_folders[d].key = _strdup(f->key), g_folders[d].hash = h, g_folders[d].first = -1;
            unsigned j = h & g_folder_mask;
            while (g_folder_slots[j]) j = (j + 1) & g_folder_mask;
            g_folder_slots[j] = d + 1;
        }
        *cut = '\\';
        f->folder = d, f->next = g_folders[d].first, g_folders[d].first = e;
    }
    hk_log("hiraku-pc: mods: %d of the game's files are a mod's, %d new files and %d new folders in its data", replaced, g_nnew, folders);
}

// The mod's file for the path the game asked for (a new folder: the mod's), or NULL
static const char *mod_file(const char *p) {
    char key[MAX_PATH];
    if (!g_nfiles || !p || hk_path_key(p, key, sizeof key) <= 0) return NULL;
    const int e = file_at(key, key_hash(key));
    if (e < 0 || strlen(g_files[e].mod) >= MAX_PATH) return NULL;
    if (!g_files[e].said) g_files[e].said = 1, hk_log("hiraku-pc: mods: %s from %s", p, g_files[e].mod);
    return g_files[e].mod;
}

// The path the game asked for, or where HK_PC_MAP sends it (in buf)
static const char *mapped(const char *p, char *buf) {
    if (!p) return p;
    if (g_nfiles) {   // a mod's file instead of the game's, or one it adds
        const char *m = mod_file(p);
        if (m) return strcpy(buf, m);
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

// The same for a DLL of the cabinet's that hooks more of the game's file calls (ezusb_c33.c)
const char *hk_map_path(const char *p, char *buf) { return mapped(p, buf); }

// beatmania IIDX 9th style to GOLD keep their charts' times in frames, in the chart files
// (sd_data\NNNN\NNNN.1: twelve charts, an offset and a length each, each chart a list of 8-byte
// events, an int32 time first, 0x7fffffff its end). A high frame rate's mod ([[rate]] chart_times,
// HK_PC_CHART_TIMES = k) has the game run k times as fast while a chart plays and read every time
// as k times as many frames: a copy made in the save folder (charts-xK, beside pc.log) and opened
// instead (made again only when the chart is newer). The longest chart of any of them (8,637 frames)
// still fits the 16 bits they keep a note's time in, doubled
static int is_chart_file(const char *p) {
    const size_t n = p ? strlen(p) : 0;
    if (n < 3 || p[n - 2] != '.' || p[n - 1] != '1') return 0;
    for (const char *q = p; *q; q++)
        if (!_strnicmp(q, "sd_data", 7)) return 1;   // the songs' folder's (settings.bin.1 is 9th style's saves)
    return 0;
}
static int g_chart_times = 1;
static const char *chart_times_file(const char *p, const char *real, DWORD acc, char *buf) {
    if (g_chart_times < 2 || !p || !real || (acc & GENERIC_WRITE)) return NULL;
    if (!is_chart_file(p)) return NULL;
    const size_t n = strlen(p);
    const char *base = p + n;
    while (base > p && base[-1] != '\\' && base[-1] != '/') base--;
    const char *log = getenv("HK_PC_LOG"), *slash = log ? strrchr(log, '\\') : NULL;
    if (!slash || (size_t)(slash - log) + strlen(base) + 32 >= MAX_PATH) return NULL;
    uint32_t from = 2166136261u;   // which file it's a copy of (a mod's chart, the game's): in its name
    for (const char *q = real; *q; q++) from = (from ^ (uint8_t)*q) * 16777619u;
    snprintf(buf, MAX_PATH, "%.*s\\charts-x%d\\%08x-%s", (int)(slash - log), log, g_chart_times, (unsigned)from, base);
    WIN32_FILE_ATTRIBUTE_DATA was, copy;   // a copy made before, as long and newer than the chart: as it is
    if (GetFileAttributesExA(real, GetFileExInfoStandard, &was) && GetFileAttributesExA(buf, GetFileExInfoStandard, &copy) &&
        was.nFileSizeLow == copy.nFileSizeLow && !was.nFileSizeHigh && !copy.nFileSizeHigh &&
        CompareFileTime(&copy.ftLastWriteTime, &was.ftLastWriteTime) >= 0)
        return buf;
    HANDLE h = CreateFileA(real, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    const DWORD size = GetFileSize(h, NULL);
    uint8_t *d = size >= 96 && size < (16u << 20) ? malloc(size) : NULL;
    DWORD got = 0;
    const BOOL ok = d && ReadFile(h, d, size, &got, NULL) && got == size;
    CloseHandle(h);
    const char *why = !ok ? "can't read it" : NULL;
    int events = 0;
    for (int k = 0; !why && k < 12; k++) {   // the twelve charts' places: all inside, whole events
        uint32_t off, len;
        memcpy(&off, d + k * 8, 4), memcpy(&len, d + k * 8 + 4, 4);
        if (len && (off < 96 || len % 8 || off > size || len > size - off)) why = "not a chart file";
    }
    for (int k = 0; !why && k < 12; k++) {
        uint32_t off, len;
        memcpy(&off, d + k * 8, 4), memcpy(&len, d + k * 8 + 4, 4);
        for (uint32_t i = off; i < off + len; i += 8) {
            int32_t t;
            memcpy(&t, d + i, 4);
            if (t == 0x7fffffff) break;   // the chart's end
            if (t < 0 || t > 0x7fffffff / g_chart_times) { why = "a time out of range"; break; }
            t *= g_chart_times, events++;
            memcpy(d + i, &t, 4);
        }
    }
    if (!why) {
        char dir[MAX_PATH];
        snprintf(dir, sizeof dir, "%.*s\\charts-x%d", (int)(slash - log), log, g_chart_times);
        CreateDirectoryA(dir, NULL);
        h = CreateFileA(buf, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        DWORD put = 0;
        if (h == INVALID_HANDLE_VALUE || !WriteFile(h, d, size, &put, NULL) || put != size) why = "can't write its copy";
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    free(d);
    static int said;
    if (why) hk_log("hiraku-pc: mods: %s left as it is (%s)", p, why);
    else if (said++ < 3) hk_log("hiraku-pc: mods: %s read in frames %d times shorter (%d events)", p, g_chart_times, events);
    return why ? NULL : buf;
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
    char c[MAX_PATH];
    const char *chart = chart_times_file(p, to, acc, c);
    const HANDLE h = CreateFileA(chart ? chart : to, acc, share, sa, cd, fl, t);
    static int log = -1;
    if (log < 0) log = getenv("HK_PC_FILELOG") != NULL;
    if (log && h == INVALID_HANDLE_VALUE) hk_log("hiraku-pc: can't open %s (%s): %lu", p ? p : "(null)", to ? to : "(null)", GetLastError());
    return h;
}
static BOOL WINAPI my_CreateDirectoryA(LPCSTR p, LPSECURITY_ATTRIBUTES sa) {
    char b[MAX_PATH];
    return CreateDirectoryA(mapped(p, b), sa);
}
// A folder's listing with the mods' files (FindFirstFileA with a wildcard, over a folder of
// g_folders): the game's own entries, a replaced file's with the mod's file's size and times (none
// for a folder only the mods have: its "." and ".." then), and each new name the pattern takes, in
// a list of HIRAKU's that FindNextFileA and FindClose know by its handle
static struct listing { struct listing *next; int n, at; WIN32_FIND_DATAA e[]; } *g_listings;

// Windows' wildcards, any case: '*' any letters, '?' one ("*.*" is everything)
static int wild(const char *pat, const char *name) {
    if (!strcmp(pat, "*.*")) pat = "*";
    for (; *pat; pat++, name++) {
        if (*pat == '*') {
            for (const char *n = name;; n++) {
                if (wild(pat + 1, n)) return 1;
                if (!*n) return 0;
            }
        }
        if (!*name) return 0;
        if (*pat != '?' && (*pat | (*pat >= 'A' && *pat <= 'Z' ? 0x20 : 0)) != (*name | (*name >= 'A' && *name <= 'Z' ? 0x20 : 0))) return 0;
    }
    return !*name;
}

static struct listing *listing_add(struct listing *l, int *cap, const WIN32_FIND_DATAA *e) {
    if (l->n == *cap) {
        *cap *= 2;
        l = (struct listing *)realloc(l, sizeof *l + sizeof *l->e * (size_t)*cap);
    }
    l->e[l->n++] = *e;
    return l;
}

static HANDLE listing(const char *p, int folder, const char *pat, LPWIN32_FIND_DATAA d) {
    int cap = 64;
    struct listing *l = (struct listing *)malloc(sizeof *l + sizeof *l->e * (size_t)cap);
    l->n = l->at = 0;
    char b[MAX_PATH];
    WIN32_FIND_DATAA e;
    HANDLE h = FindFirstFileA(mapped(p, b), &e);   // the game's
    if (h != INVALID_HANDLE_VALUE) {
        do l = listing_add(l, &cap, &e);
        while (FindNextFileA(h, &e));
        FindClose(h);
    }
    const int first = g_folders[folder].first;
    if (!l->n && first >= 0 && g_files[first].kind != FILE_REPLACED) {   // a folder only the mods have: its own "." and "..", as a mod's folder has them
        char in[MAX_PATH];
        const char *mod = g_files[first].mod, *cut = strrchr(mod, '\\');
        if (cut && (size_t)(cut - mod) + 3 < sizeof in) {
            snprintf(in, sizeof in, "%.*s\\*", (int)(cut - mod), mod);
            if ((h = FindFirstFileA(in, &e)) != INVALID_HANDLE_VALUE) {
                do
                    if ((!strcmp(e.cFileName, ".") || !strcmp(e.cFileName, "..")) && wild(pat, e.cFileName)) l = listing_add(l, &cap, &e);
                while (FindNextFileA(h, &e));
                FindClose(h);
            }
        }
    }
    const int game = l->n;
    for (int f = first; f >= 0; f = g_files[f].next) {
        if (!wild(pat, g_files[f].name) || (h = FindFirstFileA(g_files[f].mod, &e)) == INVALID_HANDLE_VALUE) continue;
        FindClose(h);
        if (g_files[f].kind == FILE_REPLACED) {   // the game's entry, with the mod's file's size and times
            for (int i = 0; i < game; i++)
                if (!_stricmp(l->e[i].cFileName, g_files[f].name)) {
                    l->e[i].nFileSizeHigh = e.nFileSizeHigh, l->e[i].nFileSizeLow = e.nFileSizeLow;
                    l->e[i].ftCreationTime = e.ftCreationTime, l->e[i].ftLastAccessTime = e.ftLastAccessTime;
                    l->e[i].ftLastWriteTime = e.ftLastWriteTime;
                    break;
                }
            continue;
        }
        snprintf(e.cFileName, sizeof e.cFileName, "%s", g_files[f].name);   // (new: none of the game's has its name)
        e.cAlternateFileName[0] = 0;
        l = listing_add(l, &cap, &e);
    }
    if (!l->n) {
        free(l);
        SetLastError(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    *d = l->e[l->at++];
    EnterCriticalSection(&g_listings_lock);
    l->next = g_listings, g_listings = l;
    LeaveCriticalSection(&g_listings_lock);
    return (HANDLE)l;
}

// h as a listing of HIRAKU's (taken out of the list when `take`), or NULL: Windows' own
static struct listing *listing_of(HANDLE h, int take) {
    if (!g_listings) return NULL;
    EnterCriticalSection(&g_listings_lock);
    struct listing **at = &g_listings;
    while (*at && (HANDLE)*at != h) at = &(*at)->next;
    struct listing *l = *at;
    if (l && take) *at = l->next;
    LeaveCriticalSection(&g_listings_lock);
    return l;
}

static HANDLE WINAPI my_FindFirstFileA(LPCSTR p, LPWIN32_FIND_DATAA d) {
    char b[MAX_PATH];
    if (g_nfolders && p) {   // a wildcard in a folder with a mod's files
        const char *slash = strrchr(p, '\\'), *fwd = strrchr(p, '/');
        const char *leaf = fwd > slash ? fwd + 1 : slash ? slash + 1 : p;
        char key[MAX_PATH];
        if (strpbrk(leaf, "*?") && leaf > p && (size_t)(leaf - p) < sizeof key) {
            memcpy(b, p, (size_t)(leaf - p)), b[leaf - p] = 0;
            const int folder = hk_path_key(b, key, sizeof key) > 0 ? folder_at(key, key_hash(key)) : -1;
            if (folder >= 0) return listing(p, folder, leaf, d);
        }
    }
    return FindFirstFileA(mapped(p, b), d);
}
static BOOL WINAPI my_FindNextFileA(HANDLE h, LPWIN32_FIND_DATAA d) {
    struct listing *l = listing_of(h, 0);
    if (!l) return FindNextFileA(h, d);
    if (l->at >= l->n) {
        SetLastError(ERROR_NO_MORE_FILES);
        return FALSE;
    }
    *d = l->e[l->at++];
    return TRUE;
}
static BOOL WINAPI my_FindClose(HANDLE h) {
    struct listing *l = listing_of(h, 1);
    if (!l) return FindClose(h);
    free(l);
    return TRUE;
}
static DWORD WINAPI my_GetFileAttributesA(LPCSTR p) {
    char b[MAX_PATH];
    return GetFileAttributesA(mapped(p, b));
}
static BOOL WINAPI my_GetFileAttributesExA(LPCSTR p, GET_FILEEX_INFO_LEVELS level, LPVOID info) {
    char b[MAX_PATH];
    return GetFileAttributesExA(mapped(p, b), level, info);
}
// The calls that take a mod's files only (HK_PC_MAP isn't theirs): a name given in UTF-16 (the
// libraries in the game's folder: D3DX's), ...
static HANDLE WINAPI my_CreateFileW(LPCWSTR p, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD cd, DWORD fl, HANDLE t) {
    char a[MAX_PATH];
    WCHAR w[MAX_PATH];
    const char *m = p && WideCharToMultiByte(CP_ACP, 0, p, -1, a, sizeof a, NULL, NULL) > 0 ? mod_file(a) : NULL;
    if (m && MultiByteToWideChar(CP_ACP, 0, m, -1, w, MAX_PATH) > 0) p = w;
    return CreateFileW(p, acc, share, sa, cd, fl, t);
}
// ... and a name the game turns into UTF-16 for DirectShow, which opens the movies itself
// (IGraphBuilder::RenderFile: IIDX's "..\data\movie\NNNN.4"): the mod's file's name instead.
// Only a whole name with a separator in it is looked for
static int WINAPI my_MultiByteToWideChar(UINT page, DWORD fl, LPCCH s, int n, LPWSTR w, int wn) {
    if (s && n == -1 && (page == CP_ACP || page == CP_THREAD_ACP) && (strchr(s, '\\') || strchr(s, '/')) && strlen(s) < MAX_PATH) {
        const char *m = mod_file(s);
        if (m) {
            const int r = MultiByteToWideChar(page, fl, m, -1, w, wn);
            if (r > 0) return r;   // (too long for the game's buffer: the game's own name)
        }
    }
    return MultiByteToWideChar(page, fl, s, n, w, wn);
}
// mmioOpenA (a game's sounds): a name as the other calls take it (a memory file's "name" is none)
static HMMIO WINAPI my_mmioOpenA(LPSTR p, LPMMIOINFO info, DWORD flags) {
    typedef HMMIO(WINAPI * mmioOpenA_t)(LPSTR, LPMMIOINFO, DWORD);
    static mmioOpenA_t real;
    if (!real) real = (mmioOpenA_t)(void *)GetProcAddress(LoadLibraryA("winmm.dll"), "mmioOpenA");
    char b[MAX_PATH];
    return real(p && !(info && info->pchBuffer) ? (LPSTR)mapped(p, b) : p, info, flags);
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

// rate: 1 tied to a [[rate]] (in place only while it's on), 2 also only between the cabinet's ticks,
// 3 a float of one cabinet frame's time made the rate's picture's (its replace made as the rate comes on)
// line: the patch's place in hiraku-pc's list (its patch_done), whichever lines were read
// mod_at: the address as the mod gave it (the program's own, at its preferred base: imagebase.h)
static struct patch { uint8_t *at, *original, *replace; size_t n; int once, done, rate, line; uint32_t mod_at; } *g_patches;
static int g_npatches;

// The program (the last 32-bit PC pop'n's: its DLL) where Windows put it: the mods' addresses, the
// [[rate]]s' words and the debug tools' addresses are its own at its preferred base, moved with it
static struct hk_image g_img;
static void program_image(void) {
    static int done;
    if (done) return;
    done = 1;
    HMODULE m = (HMODULE)hk_program();
    hk_image_open(&g_img, m, (uintptr_t)m);
}
static uintptr_t moved(uint32_t addr) { return (uintptr_t)hk_image_move(&g_img, addr); }
static uint32_t unmoved(uintptr_t addr) { return (uint32_t)hk_image_unmove(&g_img, addr); }

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

// A line's next word, cut out in place (a patch's bytes are as long as the mod made them: no
// buffer of a fixed size to fit); NULL at the line's end
static char *word(char **at) {
    char *w = *at + strspn(*at, " \t");
    if (!*w) return NULL;
    char *end = w + strcspn(w, " \t");
    if (*end) *end++ = 0;
    *at = end;
    return w;
}

static void read_patches(void) {
    char *t = read_text(getenv("HK_PC_PATCHES"));
    int line_no = -1, unread = 0, moves = 0, fixed = 0;
    program_image();
    for (char *line = t ? strtok(t, "\r\n") : NULL; line && line_no < 255; line = strtok(NULL, "\r\n")) {
        line_no++;
        // "ADDRESS ONCE ORIGINAL REPLACE [rate|ticks]"
        char *at = line;
        const char *addr = word(&at), *once = word(&at), *orig = word(&at), *repl = word(&at), *kind = word(&at);
        if (!repl) { unread++; continue; }
        if (!kind) kind = "";
        struct patch p = {(uint8_t *)(uintptr_t)strtoul(addr, NULL, 16), NULL, NULL, 0, atoi(once), 0,
                          !strcmp(kind, "rate") ? 1 : !strcmp(kind, "ticks") ? 2 : !strcmp(kind, "picture") ? 3 : 0, line_no, 0};
        const int n = unhex(repl, &p.replace);
        if (n <= 0 || (strcmp(orig, "-") && unhex(orig, &p.original) != n)) { unread++; continue; }
        if (p.rate && (!p.original || p.once)) continue;   // (mods.cpp refuses these)
        if (p.rate == 3 && n != 4) continue;
        p.n = (size_t)n;
        // where the program is now; its bytes at the fields it relocated as relocated
        p.mod_at = (uint32_t)(uintptr_t)p.at;
        p.at = (uint8_t *)moved(p.mod_at);
        if ((uintptr_t)p.at != p.mod_at) {
            moves++;
            fixed += hk_image_fix_patch(&g_img, p.mod_at - (uint32_t)g_img.preferred, p.n, p.original, p.rate == 3 ? NULL : p.replace) > 0;
        }
        g_patches = realloc(g_patches, sizeof *g_patches * (size_t)(g_npatches + 1));
        g_patches[g_npatches++] = p;
    }
    if (g_img.real != g_img.preferred) {
        char path[MAX_PATH] = "?";
        GetModuleFileNameA((HMODULE)hk_program(), path, sizeof path);
        const char *name = strrchr(path, '\\');
        hk_log("hiraku-pc: mods: %s at %08lx, not its own base %08lx: %d of the %d patches moved with it (%d with relocated bytes),"
               " the [[rate]]s' words too", name ? name + 1 : path, (unsigned long)g_img.real, (unsigned long)g_img.preferred, moves,
               g_npatches, fixed);
    } else if (g_img.size && hk_program() != (void *)GetModuleHandleA(NULL)) {
        hk_log("hiraku-pc: mods: the program's DLL at its own base %08lx", (unsigned long)g_img.real);
    }
    if (g_npatches) hk_log("hiraku-pc: mods: %d memory patches", g_npatches);
    if (unread) hk_log("hiraku-pc: mods: %d of the patches' lines couldn't be read: left out", unread);
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
static uint32_t g_rate_milli, g_cab_milli = 60000;   // the rate's and the cabinet's own, in mHz
static void rate_frame(void) {
    static double owed;
    const uint32_t hz = hk_link()->game_hz_milli;
    g_rate_on = hz > 60500;
    if (!g_rate_on) {
        owed = 0, g_tick = 1;
        if (hz > 30000) g_cab_milli = hz;
        return;
    }
    g_rate_milli = hz;
    // a whole multiple of the cabinet's rate (a mod's hz = "x2": a PC IIDX's 59.935 or 60.035 Hz
    // twice): every that many frames exactly, whatever the cabinet's own rate
    const uint32_t k = (hz + 30000) / 60000;
    if (k >= 2 && hz >= k * 59800 && hz <= k * 60200) {
        static uint32_t n;
        g_tick = ++n % k == 0;
        return;
    }
    owed += 59940.0 / hz;
    g_tick = owed >= 1 - 1e-9;
    if (g_tick) owed -= 1;
}

static void apply_patches(void) {
    for (int i = 0; i < g_npatches; i++) {
        struct patch *p = &g_patches[i];
        if (p->rate == 3 && g_rate_on) {   // a cabinet frame's time: the rate's picture's
            float f;
            memcpy(&f, p->original, 4);
            f = (float)(f * g_cab_milli / g_rate_milli);
            if (memcmp(&f, p->replace, 4)) {   // a new rate: the last one's value out first
                if (present(p->at, 4) && !memcmp(p->at, p->replace, 4) && memcmp(p->replace, p->original, 4)) put(p->at, p->original, 4);
                memcpy(p->replace, &f, 4);
            }
        }
        if (p->rate && (!g_rate_on || (p->rate == 2 && g_tick))) {   // the original back, if the patch is there
            if (present(p->at, p->n) && !memcmp(p->at, p->replace, p->n)) put(p->at, p->original, p->n);
            continue;
        }
        if (p->done || !present(p->at, p->n) || !memcmp(p->at, p->replace, p->n)) continue;   // gone, not there, in place
        if (p->original && memcmp(p->at, p->original, p->n)) continue;   // not loaded (yet): wait
        put(p->at, p->replace, p->n);
        if (!hk_link()->patch_done[p->line] && (uintptr_t)p->at == p->mod_at)
            hk_log("hiraku-pc: mods: patch at %p (%u bytes) in place", (void *)p->at, (unsigned)p->n);
        else if (!hk_link()->patch_done[p->line])
            hk_log("hiraku-pc: mods: patch at %08lx (the mod's; %p in this run, %u bytes) in place", (unsigned long)p->mod_at, (void *)p->at,
                   (unsigned)p->n);
        hk_link()->patch_done[p->line] = 1;
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
    if (hk_picture_more) hk_picture_more((uint32_t *)((char *)l + HK_PC_PIXELS), &w, &h);
    l->frame_w = w, l->frame_h = h;
}
void (*hk_picture_more)(uint32_t *pixels, unsigned *w, unsigned *h);

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
// it between hiraku-pc starting a frame and the game reading its controls; this one lasts as long
// under Wine and, on Windows, to the timer's next tick: half a millisecond (hk_cabinet_start asks)
static void nap(void) { hk_win_nap(2000); }

// Until hiraku-pc has shown the picture and its pacing lets the next frame start (or a quarter of
// a second, if it has stopped answering)
void (*hk_between)(void);

static void wait_for_go(void) {
    struct hk_pc_link *l = hk_link();
    const uint32_t seq = l->frame_seq;
    const DWORD t0 = GetTickCount();
    DWORD drawn = t0 - 2;   // when the last in-between picture was drawn (none yet)
    for (int spins = 0; l->frame_go != seq; spins++) {
        const DWORD now = GetTickCount();
        if (now - t0 > 250) break;
        // an in-between picture asked for (smooth motion): drawn at once
        if (hk_between && l->between_req != l->between_done) {
            hk_between();
            drawn = GetTickCount();
            spins = 0;
            continue;
        }
        // Looked for without a pause only while something is likely to come: right after the
        // frame, and for a millisecond or two after an in-between picture (a frame's are asked
        // for one after the other). Then in naps: with smooth motion on, this loop used to keep a
        // processor busy for the whole wait, most of every frame (RED in a stage: the game's
        // thread at 99 % of a core, 21 % now; an in-between picture 0.39 ms from 0.35)
        if (spins < 200) YieldProcessor();
        else if (spins < 400 || now - drawn < 2) SwitchToThread();
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
            program_image();   // (the program's own address: where it is in this run)
            w[nw].ptr = moved(p), w[nw].size = z, w[nw].direct = direct, nw++, on = 1;
            hk_log("hiraku-pc: debug: watching the buffers at %08x (%08lx in this run, %x bytes) into %s", p, (unsigned long)w[nw - 1].ptr, z,
                   w[nw - 1].dir);
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
            hk_log("hiraku-pc: debug: %08x holds %p", (unsigned)unmoved(w[i].ptr), (const void *)now);
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
            // (written to: a section of code too, where the program is one (IIDX EMPRESS unpacked))
            if (!(sec->Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
            char f[MAX_PATH + 48];
            // (named by the program's own address, at its preferred base: imagebase.h)
            snprintf(f, sizeof f, "%s\\dump_f%u_%08x.bin", dir, (unsigned)frame, unmoved((uintptr_t)(base + sec->VirtualAddress)));
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
        hk_log("hiraku-pc: debug: the program's globals dumped at picture %u (named by its own addresses; it is at %p in this run)",
               (unsigned)frame, (void *)base);
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
            hk_log("hiraku-pc: debug: %08lx (%08lx in this run) run: eax %08lx ebx %08lx ecx %08lx edx %08lx esi %08lx edi %08lx ebp %08lx esp %08lx;"
                   " stack %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx",
                   (unsigned long)unmoved(dr[which]), (unsigned long)dr[which], (unsigned long)c->Eax, (unsigned long)c->Ebx,
                   (unsigned long)c->Ecx, (unsigned long)c->Edx,
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
        hk_log("hiraku-pc: debug: %08lx written before %08lx (%08lx, %08lx in this run; thread %lu, now %08x)", (unsigned long)unmoved(dr[which]),
               (unsigned long)unmoved(at), (unsigned long)dr[which], (unsigned long)at, GetCurrentThreadId(),
               *(const uint32_t *)(uintptr_t)dr[which]);
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
            program_image();   // (the program's own address: where it is in this run)
            addr[naddr++] = (uint32_t)moved((uint32_t)strtoul(p, &end, 16));
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
    hk_log("hiraku-pc: debug: watching %d addresses from %08x (%08x in this run; %d threads, and those started later)", naddr,
           unmoved(addr[0]), addr[0], threads);
}

static const volatile float *g_chart_frame;   // IIDX DJ TROOPERS, EMPRESS: their charts' frame (chart_frame)
static volatile uint32_t *g_check_result;     // IIDX GOLD: its monitor check's result (chart_frame)
int hk_chart_scale = 1;   // a mod's charts counted in frames this many times shorter (chart_frame)

// Each Present, Direct3D 9's or 8's (d3d8.c): the mods' patches and the debug watch first, before
// the next frame runs
void hk_present_begin(void) {
    // the words a mod's [[rate]] looks at, for hiraku-pc (link.h, v9)
    struct hk_pc_link *l = hk_link();
    for (uint32_t i = 0; i < HK_PC_WATCHES; i++) {
        const uint8_t *w = (const uint8_t *)moved(l->watch_addr[i]);   // (the program's own address: where it is now)
        if (w) l->watch_val[i] = present(w, 4) ? *(const uint32_t *)w : 0;
    }
    rate_frame();
    if (g_chart_frame) {
        const float ms = *g_chart_frame * (float)hk_chart_scale;
        l->chart_frame_ns = ms > 15.0f && ms < 18.0f ? (uint32_t)(ms * 1e6f + 0.5f) : 0;
    } else if (g_check_result) {   // GOLD: its check's result as the same two frames (VGA/DVI, S-VIDEO)
        const uint32_t r = *g_check_result;
        l->chart_frame_ns = r == 0 ? 16652800u : r == 1 ? 16680600u : 0;
    }
    if (g_npatches) apply_patches();
    dbg_records();
    dbg_watch();
    dbg_dumps();
}

// A picture the game has drawn, in HIRAKU's window (its pixels in the link if `captured`): counted
// with its frame's sound, then the game waits for hiraku-pc's next frame
static void profile_frame(void);

// The CPU time this process has used, for hiraku-pc's performance overlay (link.h cpu_ms): once a second
static void cpu_time(struct hk_pc_link *l) {
    static DWORD last;
    const DWORD now = GetTickCount();
    FILETIME c, e, k, u;
    if (now - last < 1000) return;
    last = now;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return;
    l->cpu_ms = (uint32_t)((((uint64_t)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((uint64_t)u.dwHighDateTime << 32 | u.dwLowDateTime)) / 10000);
    l->cpu_at_ms = now ? now : 1;
}

void hk_picture_drawn(int captured) {
    struct hk_pc_link *l = hk_link();
    profile_frame();
    cpu_time(l);
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
// HIRAKU's Present, Reset and CreateTexture in the device's table now; 1 if one had to be put back
static int keep_device_hooks(void) {
    void **vt = *(void ***)g_dev;   // the device's table now (it may be another)
    int again = 0;
    if (vt[17] != (void *)my_Present) {
        uintptr_t at;
        const char *m = module_of(vt[17], &at);
        hk_log("hiraku-pc: the game's Present was replaced (by %s+%lx): HIRAKU's put back, that one called by it", m, (unsigned long)at);
        g_Present = (Present_t)vt[17];
        patch_slot(&vt[17], (void *)my_Present);
        again = 1;
    }
    if (vt[16] != (void *)my_Reset) {
        g_Reset = (Reset_t)vt[16];
        patch_slot(&vt[16], (void *)my_Reset);
        again = 1;
    }
    // the 16-bit render targets made as A8R8G8B8 (IIDX GOLD makes 240 in A1R5G5B5 as it starts,
    // which an NVIDIA card refuses: the game then never makes its state block and faults on it)
    if (vt[23] != (void *)my_CreateTexture) {
        static int said;
        if (!said++) {
            uintptr_t at;
            const char *m = module_of(vt[23], &at);
            hk_log("hiraku-pc: the game's CreateTexture was replaced too (by %s+%lx): HIRAKU's put back", m, (unsigned long)at);
        }
        g_CreateTexture = (CreateTexture_t)vt[23];
        patch_slot(&vt[23], (void *)my_CreateTexture);
        again = 1;
    }
    return again;
}
// Every 2 ms for the first 10 seconds (games make their textures as they start, right after the
// device), every 100 ms to the first minute, then every half second (a Reset may do it again)
static DWORD WINAPI present_watch(LPVOID unused) {
    (void)unused;
    for (int i = 0;; i++) {
        Sleep(i < 5000 ? 2 : i < 5500 ? 100 : 500);
        keep_device_hooks();
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
        was = patch_slot(&vt[23], (void *)my_CreateTexture);
        if (was) g_CreateTexture = (CreateTexture_t)was;
        hk_d3d9_hooks(*dev);
        if (!g_dev && host_shows()) {
            g_dev = *dev;
            IDirect3DDevice9_AddRef(g_dev);   // (the watcher's)
            // Windows' Direct3D 9 may put its own table in place as the device is first used: a call
            // that changes nothing, and the hooks looked at again before the game goes on
            IDirect3DDevice9_TestCooperativeLevel(g_dev);
            keep_device_hooks();
            CloseHandle(CreateThread(NULL, 0, present_watch, NULL, 0, NULL));
        }
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
    if (!_stricmp(dll, "ole32.dll")) return host_shows() ? hk_effector_hook(fn) : NULL;   // effector.c
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
        {"kernel32.dll", "FindNextFileA", (void *)my_FindNextFileA},
        {"kernel32.dll", "FindClose", (void *)my_FindClose},
        {"kernel32.dll", "GetFileAttributesA", (void *)my_GetFileAttributesA},
        {"kernel32.dll", "GetFileAttributesExA", (void *)my_GetFileAttributesExA},
        {"kernel32.dll", "CreateFileW", (void *)my_CreateFileW},
        {"kernel32.dll", "MultiByteToWideChar", (void *)my_MultiByteToWideChar},
        {"winmm.dll", "mmioOpenA", (void *)my_mmioOpenA},
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
            // the mods' alone (a game without mods: as it was)
            if ((hooks[i].to == (void *)my_FindNextFileA || hooks[i].to == (void *)my_FindClose) && !g_nfolders) return NULL;
            if ((hooks[i].to == (void *)my_GetFileAttributesExA || hooks[i].to == (void *)my_CreateFileW ||
                 hooks[i].to == (void *)my_MultiByteToWideChar || hooks[i].to == (void *)my_mmioOpenA) && !g_nfiles)
                return NULL;
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
        {"ws2_32.dll", 51, "gethostbyaddr"},
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
    if (bytes > sizeof mods) bytes = sizeof mods;   // (the bytes all the modules need: more than the array's when it is full)
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

// The monitor check of beatmania IIDX GOLD (all but its first program), DJ TROOPERS and EMPRESS: with
// the operator's output mode AUTO, the start-up times 3000 of its frames (timeGetTime: cmp eax,3000;
// jne; call edi; sub eax,[T0]; cmp eax,50004) and keeps 1 when they took longer (a 59.94 Hz monitor:
// S-VIDEO), else 0 (VGA, or GOLD's DVI: 60.05 Hz). VGA or S-VIDEO set gives that at once. The result
// picks the frame DJ TROOPERS and EMPRESS count their charts in (chart_frame, below); GOLD shortens
// every chart's times by 59.94/60.05 when it is 1. The setting's reader and the result's setter are
// found where the operator menu leaves (call G; cmp eax,1; jne; push 0; jmp; call G; cmp eax,2;
// jne; push 1; call SET), each perhaps through a jump: G = movsx eax,byte [S]; ret, SET = mov eax,
// [esp+4]; mov [R],eax; ret. 1 with both, -1 with the measurement but not them, 0 without a check
static uint8_t *code_target(uint8_t *call) {
    uint8_t *to = call + 5 + *(const int32_t *)(call + 1);
    if (present(to, 5) && to[0] == 0xe9) to += 5 + *(const int32_t *)(to + 1);
    return present(to, 10) ? to : NULL;
}
int hk_monitor_check(uint8_t **setting, volatile uint32_t **result) {
    static const uint8_t measure[20] = {0x3d, 0xb8, 0x0b, 0, 0, 0x75, 0, 0xff, 0xd7, 0x2b, 0x05, 0, 0, 0, 0, 0x3d, 0x54, 0xc3, 0, 0};
    static const uint8_t measure_m[20] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0, 0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0xff};
    static const uint8_t leave[25] = {0xe8, 0, 0, 0, 0, 0x83, 0xf8, 0x01, 0x75, 0x04, 0x6a, 0x00, 0xeb, 0,
                                      0xe8, 0, 0, 0, 0, 0x83, 0xf8, 0x02, 0x75, 0x0a, 0x6a};
    static int state = 2;   // (2: not looked yet)
    static uint8_t *s;
    static volatile uint32_t *r;
    if (state == 2) {
        state = 0;
        char *base = (char *)hk_program();
        IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
        IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
        for (WORD k = 0; k < nt->FileHeader.NumberOfSections; k++, sec++) {
            if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            uint8_t *code = (uint8_t *)base + sec->VirtualAddress;
            for (size_t i = 0; i + 32 <= sec->Misc.VirtualSize; i++) {
                size_t j = 0;
                if (!state) {
                    while (j < 20 && (code[i + j] & measure_m[j]) == measure[j]) j++;
                    if (j == 20) state = -1;
                }
                for (j = 0; j < 25 && (code[i + j] == leave[j] || j == 13 || (j >= 1 && j <= 4) || (j >= 15 && j <= 18)); j++) {}
                if (j < 25 || code[i + 25] != 0x01 || code[i + 26] != 0xe8) continue;
                uint8_t *g = code_target(code + i), *g2 = code_target(code + i + 14), *set = code_target(code + i + 26);
                if (!g || g != g2 || !set || g[0] != 0x0f || g[1] != 0xbe || g[2] != 0x05 || g[7] != 0xc3) continue;
                if (memcmp(set, "\x8b\x44\x24\x04\xa3", 5) || set[9] != 0xc3) continue;
                s = (uint8_t *)(uintptr_t)*(const uint32_t *)(g + 3);
                r = (volatile uint32_t *)(uintptr_t)*(const uint32_t *)(set + 5);
            }
        }
        if (state && s && r) state = 1;
    }
    if (setting) *setting = state > 0 ? s : NULL;
    if (result) *result = state > 0 ? r : NULL;
    return state;
}

// beatmania IIDX DJ TROOPERS and EMPRESS take their charts' times (ms) into frames as they read them,
// at a frame the monitor check's result picks (above: the output mode, or the measurement): 16.6528
// ms (VGA, 60.05 Hz) or 16.6806 ms (S-VIDEO, 59.95 Hz), set into one float just before (mov ds:X,
// 0x418538ea or 0x418571cd). Found by those two stores, read each picture for hiraku-pc (link.h,
// v13), which runs the game at the rate that keeps its music with the notes (g_chart_frame, above).
// GOLD has no such frame: its check's result itself is read (g_check_result) and handed over as the
// same two frames, hiraku-pc running GOLD at its charts' rate times 59.94/60.05 when it is 1
static void chart_frame(void) {
    if (hk_monitor_check(NULL, NULL) > 0) {   // (taken back below for DJ TROOPERS and EMPRESS)
        hk_monitor_check(NULL, &g_check_result);
        if (!present((const uint8_t *)g_check_result, 4)) g_check_result = NULL;
    }
    char *base = (char *)hk_program();
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    static const uint8_t a[4] = {0xea, 0x38, 0x85, 0x41}, b[4] = {0xcd, 0x71, 0x85, 0x41};
    for (WORD k = 0; k < nt->FileHeader.NumberOfSections; k++, sec++) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const uint8_t *code = (const uint8_t *)base + sec->VirtualAddress;
        const size_t size = sec->Misc.VirtualSize;
        for (size_t i = 0; i + 10 <= size; i++) {
            if (code[i] != 0xc7 || code[i + 1] != 0x05 || memcmp(code + i + 6, a, 4)) continue;
            for (size_t j = i + 10; j + 10 <= size && j < i + 64; j++)
                if (code[j] == 0xc7 && code[j + 1] == 0x05 && !memcmp(code + j + 2, code + i + 2, 4) && !memcmp(code + j + 6, b, 4)) {
                    g_chart_frame = (const volatile float *)(uintptr_t)*(const uint32_t *)(code + i + 2);
                    g_check_result = NULL;
                    hk_log("hiraku-pc: the charts' frame (the monitor check's) at %p", (const void *)g_chart_frame);
                    // a mod that counts the charts in shorter frames (a high frame rate's: the frame
                    // halved, the game run at twice the rate while a chart plays): by how much, so
                    // that hiraku-pc still hears the check's frame and the judging takes the lanes'
                    // distances back to the cabinet's frames (press.c)
                    for (int q = 0; q < g_npatches; q++) {
                        const struct patch *p = &g_patches[q];
                        if (p->n != 4 || p->at != code + i + 6) continue;
                        float was, now;
                        memcpy(&was, a, 4), memcpy(&now, p->replace, 4);
                        const int k = now > 0.5f ? (int)(was / now + 0.5f) : 0;
                        if (k >= 2 && k <= 8) {
                            hk_chart_scale = k;
                            hk_log("hiraku-pc: mods: the charts counted in frames %d times shorter", k);
                        }
                    }
                    return;
                }
        }
    }
    if (g_check_result) hk_log("hiraku-pc: the monitor check's result at %p (GOLD's: hiraku-pc follows it)", (const void *)g_check_result);
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
    // The timer at its finest from the start, and kept though no window of the game's shows
    // (shell/wintimer.h): the waits here and the game's own (its Sleep(1) between frames, its
    // sound's thread) would else end on 15.6 ms ticks until the game asked, if it does
    const int full = hk_win_full_speed();
    const unsigned long timer = hk_win_timer(5000);
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
    chart_frame();
    const char *ct = getenv("HK_PC_CHART_TIMES");   // (chart_times_file)
    if (ct && atoi(ct) >= 2 && atoi(ct) <= 8 && hk_chart_scale <= 1) {
        hk_chart_scale = g_chart_times = atoi(ct);
        hk_log("hiraku-pc: mods: the charts' files read in frames %d times shorter", g_chart_times);
    }
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
    hk_log("hiraku-pc: the game's timer: %.1f ms%s", timer / 1e4,
           full == 2 ? ", kept whatever its windows show" : full ? ", its speed kept whatever its windows show" : "");
}
