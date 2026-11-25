// SPDX-License-Identifier: GPL-3.0-or-later
// Where the game's files are (PLAN.md section 32). With HK_PC64_SAVE the loader runs in that folder
// (HIRAKU's save folder for the game): AVS2's root, the cabinet's dev/nvram and dev/raw, a copy of
// prop/ are there, and the game's folder is only read. The game mounts its data itself
// (avs_fs_mount, avs2-core's ordinal 76) from folders under the root (./plain_data/sd, ...): those
// mounts are pointed at the game's folder.
#include "pc64.h"

#include <stdlib.h>
#include <string.h>

static char g_game[MAX_PATH];   // the game's folder, when it isn't AVS2's root
static int g_game_fallback;     // a read the run folder doesn't have falls back to the game's own
                                // folder (only read; writes never go there). This game opens its
                                // data straight from its folder (e.g. .\data\font\0000.bin) as well
                                // as through AVS2, so in the save folder those opens miss
static int (*r_avs_fs_mount)(const char *mountpoint, const char *source, const char *fstype, void *data);

// the access and disposition bits that can change or delete: the fallback to the game's folder
// (which must stay read-only) is allowed only for a pure read of an existing file
#define HK_WRITE_BITS (GENERIC_WRITE | GENERIC_ALL | MAXIMUM_ALLOWED | DELETE | WRITE_DAC | WRITE_OWNER | \
                       FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES)
static int read_only_open(DWORD acc, DWORD disp, DWORD flags) {
    return disp == OPEN_EXISTING && !(acc & HK_WRITE_BITS) && !(flags & FILE_FLAG_DELETE_ON_CLOSE);
}

// the game's own folder's file for a relative `path` (0: none — absolute paths, listings and any
// ".." component are left alone)
static int game_path(const char *path, char *out, size_t size) {
    if (!g_game_fallback || !path || strpbrk(path, "*?")) return 0;
    const char *rest = path;
    if (!strncmp(rest, "./", 2) || !strncmp(rest, ".\\", 2)) rest += 2;
    if (strchr(rest, ':') || rest[0] == '\\' || rest[0] == '/') return 0;
    for (const char *p = rest;;) {
        const char *sep = strpbrk(p, "\\/");
        const size_t n = sep ? (size_t)(sep - p) : strlen(p);
        if (n == 2 && p[0] == '.' && p[1] == '.') return 0;
        if (!sep) break;
        p = sep + 1;
    }
    snprintf(out, size, "%s\\%s", g_game, rest);
    for (char *c = out; *c; c++)
        if (*c == '/') *c = '\\';
    return 1;
}



static int avs_fs_mount_(const char *mountpoint, const char *source, const char *fstype, void *data) {
    char moved[MAX_PATH * 2];
    if (g_game[0] && source && fstype && !strcmp(fstype, "fs")) {   // a folder (not ramfs's names, imagefs's files)
        const char *rest = !strncmp(source, "./", 2) ? source + 2 : source;
        if (strncmp(rest, "dev/", 4) && !strchr(rest, ':') && rest[0] != '/' && rest[0] != '\\') {
            snprintf(moved, sizeof moved, "%s\\%s", g_game, rest);
            for (char *c = moved; *c; c++)
                if (*c == '/') *c = '\\';
            source = moved;
        }
    }
    const int r = r_avs_fs_mount(mountpoint, source, fstype, data);
    if (source == moved) pc64_log("%s: %s", mountpoint ? mountpoint : "?", source);
    return r;
}

// Folders laid over the game's files, as AVS2 opens them (CreateFileW/A in avs2-core) and looks for
// them (FindFirstFileA/ExW): a file in one at a path is the game's file there, a file the game
// hasn't too; the first folder that has it wins.
//   - HK_PC64_OVERLAY=DIR: over the game's folder (DIR\plain_data\tex\system29\x.ifs for the
//     game's plain_data\tex\system29\x.ifs: the single-screen layout);
//   - HK_PC64_DATA=DIR;DIR...: each over the game's data folder (plain_data, or data): the mods'
//     folders (DIR\sd\x\y.ifs for the game's data\sd\x\y.ifs).
struct layer {
    char dir[MAX_PATH];
    char under[16];   // the game's folder it stands for, under the game's ("": the game's own)
};
static struct layer g_layers[16];
static int g_nlayers;
static int g_file_log;   // HK_PC64_FILE_LOG: the file opens and attribute queries that fail, logged
static HANDLE(WINAPI *r_CreateFileW)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static HANDLE(WINAPI *r_CreateFileA)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static HANDLE(WINAPI *r_FindFirstFileA)(LPCSTR, LPWIN32_FIND_DATAA);
static HANDLE(WINAPI *r_FindFirstFileExA)(LPCSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD);
static HANDLE(WINAPI *r_FindFirstFileExW)(LPCWSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD);
static DWORD(WINAPI *r_GetFileAttributesA)(LPCSTR);
static DWORD(WINAPI *r_GetFileAttributesW)(LPCWSTR);
static BOOL(WINAPI *r_GetFileAttributesExW)(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID);

// HK_PC64_FILE_LOG: every open that can change or delete something, and where it landed
static void write_log(const char *path, HANDLE h, DWORD acc, DWORD disp, DWORD flags) {
    if (!g_file_log) return;
    if (!(acc & HK_WRITE_BITS) && disp == OPEN_EXISTING && !(flags & FILE_FLAG_DELETE_ON_CLOSE)) return;
    char final[MAX_PATH * 2] = "";
    if (h != INVALID_HANDLE_VALUE) GetFinalPathNameByHandleA(h, final, sizeof final, FILE_NAME_NORMALIZED);
    pc64_log("file: write-open acc %08lx disp %lu flags %08lx: \"%s\" -> %s", (unsigned long)acc, (unsigned long)disp,
             (unsigned long)flags, path, h != INVALID_HANDLE_VALUE ? final : "FAILED");
}

// a failed open or attribute query: the path as it was given, the mapped one if a layer had it, the error
static void file_fail(const char *op, const char *path, const char *moved, DWORD err) {
    if (!g_file_log) return;
    if (moved) pc64_log("file: %s failed (err %lu): \"%s\" -> \"%s\"", op, err, path, moved);
    else pc64_log("file: %s failed (err %lu): \"%s\"", op, err, path);
}
static void narrow(LPCWSTR w, char *out, int size) {
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, size, NULL, NULL)) snprintf(out, size, "?");
}

// The layers' file for `path` (a path under the game's folder, or relative to it), if one has it
static int overlaid(const char *path, char *out, size_t size) {
    if (!g_nlayers || !path || strpbrk(path, "*?")) return 0;
    const char *rest = NULL;
    const size_t n = strlen(g_game);
    if (n && !_strnicmp(path, g_game, n) && (path[n] == '\\' || path[n] == '/')) rest = path + n + 1;
    else if (!strchr(path, ':') && path[0] != '\\' && path[0] != '/') rest = !strncmp(path, "./", 2) || !strncmp(path, ".\\", 2) ? path + 2 : path;
    if (!rest) return 0;
    for (int i = 0; i < g_nlayers; i++) {
        const char *sub = rest;
        const size_t u = strlen(g_layers[i].under);
        if (u) {
            if (_strnicmp(rest, g_layers[i].under, u) || (rest[u] != '\\' && rest[u] != '/')) continue;
            sub = rest + u + 1;
        }
        snprintf(out, size, "%s\\%s", g_layers[i].dir, sub);
        for (char *c = out; *c; c++)
            if (*c == '/') *c = '\\';
        const DWORD a = GetFileAttributesA(out);
        if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) return 1;
    }
    return 0;
}
static void said(const char *moved) {
    static int n;
    if (n++ < 500) pc64_log("overlay: %s", moved);
}
static HANDLE WINAPI CreateFileA_(LPCSTR path, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    char moved[MAX_PATH * 2];
    const int layered = !(acc & GENERIC_WRITE) && overlaid(path, moved, sizeof moved);
    if (layered) said(moved);
    HANDLE h = r_CreateFileA(layered ? moved : path, acc, share, sa, disp, flags, tmpl);
    const DWORD e = GetLastError();
    write_log(path, h, acc, disp, flags);
    if (h == INVALID_HANDLE_VALUE && read_only_open(acc, disp, flags) && (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND)) {
        char gp[MAX_PATH * 2];
        if (game_path(path, gp, sizeof gp)) h = r_CreateFileA(gp, acc, share, sa, disp, flags, tmpl);
        if (h != INVALID_HANDLE_VALUE && g_file_log) pc64_log("file: open \"%s\": not in the run folder, from the game's: \"%s\"", path, gp);
    }
    if (h == INVALID_HANDLE_VALUE) {
        SetLastError(e);   // the game sees the first attempt's error, not the fallback's
        file_fail("open", path, layered ? moved : NULL, e);
    }
    return h;
}
// a wide path's layer file, wide (0: none)
static int overlaid_w(LPCWSTR path, WCHAR *out, int size) {
    char narrow_[MAX_PATH * 2], moved[MAX_PATH * 2];
    return g_nlayers && path && WideCharToMultiByte(CP_UTF8, 0, path, -1, narrow_, sizeof narrow_, NULL, NULL) &&
           overlaid(narrow_, moved, sizeof moved) && MultiByteToWideChar(CP_UTF8, 0, moved, -1, out, size) && (said(moved), 1);
}
static HANDLE WINAPI CreateFileW_(LPCWSTR path, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    WCHAR wide[MAX_PATH * 2];
    const int layered = !(acc & GENERIC_WRITE) && overlaid_w(path, wide, MAX_PATH * 2);
    HANDLE h = r_CreateFileW(layered ? wide : path, acc, share, sa, disp, flags, tmpl);
    const DWORD e = GetLastError();
    char np[MAX_PATH * 2];
    narrow(path, np, sizeof np);
    write_log(np, h, acc, disp, flags);
    if (h == INVALID_HANDLE_VALUE && read_only_open(acc, disp, flags) && (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND)) {
        char gp[MAX_PATH * 2];
        if (game_path(np, gp, sizeof gp)) {
            WCHAR wgp[MAX_PATH * 2];
            if (MultiByteToWideChar(CP_UTF8, 0, gp, -1, wgp, MAX_PATH * 2)) h = r_CreateFileW(wgp, acc, share, sa, disp, flags, tmpl);
            if (h != INVALID_HANDLE_VALUE && g_file_log) pc64_log("file: open \"%s\": not in the run folder, from the game's: \"%s\"", np, gp);
        }
    }
    if (h == INVALID_HANDLE_VALUE) {
        SetLastError(e);
        char nm[MAX_PATH * 2];
        file_fail("open", np, layered ? (narrow(wide, nm, sizeof nm), nm) : NULL, e);
    }
    return h;
}
// AVS2's look at one file (its size, that it's there): the layer's (a folder's listing stays the game's)
static HANDLE WINAPI FindFirstFileA_(LPCSTR path, LPWIN32_FIND_DATAA data) {
    char moved[MAX_PATH * 2];
    const int layered = overlaid(path, moved, sizeof moved);
    HANDLE h = r_FindFirstFileA(layered ? moved : path, data);
    const DWORD e = GetLastError();
    if (h == INVALID_HANDLE_VALUE && (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND)) {
        char gp[MAX_PATH * 2];
        if (game_path(path, gp, sizeof gp)) h = r_FindFirstFileA(gp, data);
        if (h != INVALID_HANDLE_VALUE && g_file_log) pc64_log("file: find \"%s\": not in the run folder, from the game's: \"%s\"", path, gp);
    }
    if (h == INVALID_HANDLE_VALUE) {
        SetLastError(e);
        file_fail("find", path, layered ? moved : NULL, e);
    }
    return h;
}
static HANDLE WINAPI FindFirstFileExW_(LPCWSTR path, FINDEX_INFO_LEVELS level, LPVOID data, FINDEX_SEARCH_OPS op, LPVOID filter, DWORD flags) {
    WCHAR wide[MAX_PATH * 2];
    const int layered = overlaid_w(path, wide, MAX_PATH * 2);
    HANDLE h = r_FindFirstFileExW(layered ? wide : path, level, data, op, filter, flags);
    const DWORD e = GetLastError();
    char np[MAX_PATH * 2];
    narrow(path, np, sizeof np);
    if (h == INVALID_HANDLE_VALUE && (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND)) {
        char gp[MAX_PATH * 2];
        if (game_path(np, gp, sizeof gp)) {
            WCHAR wgp[MAX_PATH * 2];
            if (MultiByteToWideChar(CP_UTF8, 0, gp, -1, wgp, MAX_PATH * 2)) h = r_FindFirstFileExW(wgp, level, data, op, filter, flags);
            if (h != INVALID_HANDLE_VALUE && g_file_log) pc64_log("file: find \"%s\": not in the run folder, from the game's: \"%s\"", np, gp);
        }
    }
    if (h == INVALID_HANDLE_VALUE) {
        SetLastError(e);
        char nm[MAX_PATH * 2];
        file_fail("find", np, layered ? (narrow(wide, nm, sizeof nm), nm) : NULL, e);
    }
    return h;
}
static HANDLE WINAPI FindFirstFileExA_(LPCSTR path, FINDEX_INFO_LEVELS level, LPVOID data, FINDEX_SEARCH_OPS op, LPVOID filter, DWORD flags) {
    char moved[MAX_PATH * 2];
    const int layered = overlaid(path, moved, sizeof moved);
    HANDLE h = r_FindFirstFileExA(layered ? moved : path, level, data, op, filter, flags);
    const DWORD e = GetLastError();
    if (h == INVALID_HANDLE_VALUE && (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND)) {
        char gp[MAX_PATH * 2];
        if (game_path(path, gp, sizeof gp)) h = r_FindFirstFileExA(gp, level, data, op, filter, flags);
        if (h != INVALID_HANDLE_VALUE && g_file_log) pc64_log("file: find \"%s\": not in the run folder, from the game's: \"%s\"", path, gp);
    }
    if (h == INVALID_HANDLE_VALUE) {
        SetLastError(e);
        file_fail("find", path, layered ? moved : NULL, e);
    }
    return h;
}
static DWORD WINAPI GetFileAttributesA_(LPCSTR path) {
    DWORD a = r_GetFileAttributesA(path);
    DWORD e = GetLastError();
    if (a == INVALID_FILE_ATTRIBUTES && (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND)) {
        char gp[MAX_PATH * 2];
        if (game_path(path, gp, sizeof gp)) a = r_GetFileAttributesA(gp);
        if (a != INVALID_FILE_ATTRIBUTES && g_file_log) pc64_log("file: attributes \"%s\": not in the run folder, from the game's: \"%s\"", path, gp);
    }
    if (a == INVALID_FILE_ATTRIBUTES) {
        SetLastError(e);
        file_fail("attributes", path, NULL, e);
    }
    return a;
}
static DWORD WINAPI GetFileAttributesW_(LPCWSTR path) {
    DWORD a = r_GetFileAttributesW(path);
    DWORD e = GetLastError();
    if (a == INVALID_FILE_ATTRIBUTES && (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND)) {
        char np[MAX_PATH * 2], gp[MAX_PATH * 2];
        narrow(path, np, sizeof np);
        if (game_path(np, gp, sizeof gp)) a = r_GetFileAttributesA(gp);
        if (a != INVALID_FILE_ATTRIBUTES && g_file_log) pc64_log("file: attributes \"%s\": not in the run folder, from the game's: \"%s\"", np, gp);
    }
    if (a == INVALID_FILE_ATTRIBUTES) {
        SetLastError(e);
        char np[MAX_PATH * 2];
        narrow(path, np, sizeof np);
        file_fail("attributes", np, NULL, e);
    }
    return a;
}
static BOOL WINAPI GetFileAttributesExW_(LPCWSTR path, GET_FILEEX_INFO_LEVELS level, LPVOID data) {
    BOOL r = r_GetFileAttributesExW(path, level, data);
    DWORD e = GetLastError();
    if (!r && (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND)) {
        char np[MAX_PATH * 2], gp[MAX_PATH * 2];
        narrow(path, np, sizeof np);
        if (game_path(np, gp, sizeof gp)) {
            WCHAR wgp[MAX_PATH * 2];
            if (MultiByteToWideChar(CP_UTF8, 0, gp, -1, wgp, MAX_PATH * 2)) r = r_GetFileAttributesExW(wgp, level, data);
            if (r && g_file_log) pc64_log("file: attributes-ex \"%s\": not in the run folder, from the game's: \"%s\"", np, gp);
        }
    }
    if (!r) {
        SetLastError(e);
        char np[MAX_PATH * 2];
        narrow(path, np, sizeof np);
        file_fail("attributes-ex", np, NULL, e);
    }
    return r;
}

static void add_layer(const char *dir, const char *under) {
    if (g_nlayers >= (int)(sizeof g_layers / sizeof g_layers[0]) || !*dir) return;
    snprintf(g_layers[g_nlayers].dir, sizeof g_layers[0].dir, "%s", dir);
    snprintf(g_layers[g_nlayers].under, sizeof g_layers[0].under, "%s", under);
    g_nlayers++;
    pc64_log("overlay: %s over the game's %s", dir, *under ? under : "folder");
}

// The game's data folder, under its folder: plain_data (or data)
const char *pc64_data_folder(void) {
    char p[MAX_PATH * 2];
    snprintf(p, sizeof p, "%s%splain_data", g_game, g_game[0] ? "\\" : "");
    const DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) ? "plain_data" : "data";
}
const char *pc64_game_folder(void) { return g_game; }

// --- Inside the game's packs: the pictures it reads one by one from a pack it has mounted (a song's
// banner from /ifs_image_kc, its background from /ifs_image_bg, ...), each itself a small pack. A
// mod's folder named after the pack and _ifs, as ifs_layeredfs has them (tex/system/kc_diff_ifs/
// kc_0001.ifs), gives one the game's pack doesn't have, or replaces one: the game's AVS2 open and
// stat (avs2-core's ordinals 79 and 100) are pointed at the mod's file, through a mount of each
// mod's folder (/hiraku_modN). The packs' names differ between the games the mods were made for
// and this one (kc_diff, kc_jka_diff, kc_jka): each mount point has the names a mod may use.
static const struct { const char *mount, *folders[4]; } PACKS[] = {
    {"/ifs_image_kc", {"tex/system/kc_diff_ifs", "tex/system/kc_jka_diff_ifs", "tex/system/kc_jka_ifs", NULL}},
    {"/ifs_image_bg", {"tex/system/bg_diff_ifs", "tex/system/bg_jka_ifs", NULL}},
    {"/ifs_image_hariai", {"tex/system/ha_merge_ifs", "tex/system/ha_merge_jka_ifs", NULL}},
    {"/ifs_image_chara_name", {"tex/system/chara_name_diff_ifs", "tex/system/chara_name_new_diff_ifs", "tex/system/chara_name_jka_ifs", NULL}},
};
static int (*r_avs_open)(const char *path, int mode, int perm);
static int (*r_avs_stat)(const char *path, void *st);
static int g_mods_mounted;
static int g_first_mod_layer;   // g_layers' first mod (after the game's folder's overlay)

static void mount_mods(void) {
    if (g_mods_mounted) return;
    g_mods_mounted = 1;
    for (int i = g_first_mod_layer; i < g_nlayers; i++) {
        char mp[32];
        snprintf(mp, sizeof mp, "/hiraku_mod%d", i - g_first_mod_layer);
        if (r_avs_fs_mount(mp, g_layers[i].dir, "fs", NULL) < 0) pc64_log("overlay: %s couldn't be mounted at %s", g_layers[i].dir, mp);
    }
}
// the mod's AVS path for the game's `path` (NULL: none)
static const char *in_mods(const char *path, char *out, size_t size) {
    if (!path || g_first_mod_layer >= g_nlayers || !r_avs_fs_mount) return NULL;
    const char *p = path;
    if (!strncmp(p, "/afp", 4)) {   // the picture loader's own mount of the pack (/afp27/ifs_image_kc/...)
        p += 4;
        while (*p >= '0' && *p <= '9') p++;
    }
    for (size_t k = 0; k < sizeof PACKS / sizeof PACKS[0]; k++) {
        const size_t n = strlen(PACKS[k].mount);
        if (strncmp(p, PACKS[k].mount, n) || p[n] != '/' || strstr(p, "..")) continue;
        const char *member = p + n + 1;
        for (int i = g_first_mod_layer; i < g_nlayers; i++)
            for (int f = 0; PACKS[k].folders[f]; f++) {
                char host[MAX_PATH * 2];
                snprintf(host, sizeof host, "%s\\%s\\%s", g_layers[i].dir, PACKS[k].folders[f], member);
                for (char *c = host; *c; c++)
                    if (*c == '/') *c = '\\';
                const DWORD a = GetFileAttributesA(host);
                if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) continue;
                mount_mods();
                snprintf(out, size, "/hiraku_mod%d/%s/%s", i - g_first_mod_layer, PACKS[k].folders[f], member);
                return out;
            }
        return NULL;
    }
    return NULL;
}
static int avs_open_(const char *path, int mode, int perm) {
    char moved[MAX_PATH * 2];
    const char *m = in_mods(path, moved, sizeof moved);
    if (m) {
        static int n;
        if (n++ < 200) pc64_log("overlay: %s from %s", path, m);
    }
    const int r = r_avs_open(m ? m : path, mode, perm);
    if (r < 0) file_fail("avs open", path, NULL, (DWORD)r);
    return r;
}
static int avs_stat_(const char *path, void *st) {
    char moved[MAX_PATH * 2];
    const char *m = in_mods(path, moved, sizeof moved);
    const int r = r_avs_stat(m ? m : path, st);
    if (r < 0) file_fail("avs stat", path, NULL, (DWORD)r);
    return r;
}

// A look at one of the game's data files (AVS2's stat, ordinal 100), answered from the first answer
// kept. A 64-bit PC IIDX's music select asks, for every song bar and every frame, whether the song's
// movie thumbnail is there (/data/graphic/movie_thumbnail/..., most songs have none). Under Wine each
// look is an open that fails in the run folder, the game folder's open (above), the file's and its
// volume's information and a close: calls to the wine server, two thirds of the frame (2026-11-24),
// where a cabinet's own disk answers in microseconds. The game's data doesn't change while it runs
// (only read: the run folder never has it), so a path under /data/ gets its first answer again: the
// result, and the stat record (0x28 bytes, as AVS2 clears it) when there is one. Other paths, long
// ones and a look while another thread is in the table go to AVS2 as before.
// HK_PC64_DATA_STAT=0: every look goes to AVS2.
#define STAT_SLOTS 4096
#define STAT_SIZE 0x28
// (its answer is a byte, "there or not", in a register left otherwise as it was: kept whole)
static long long (*r_data_stat)(const char *path, void *st);
static struct { unsigned h; long long r; char path[100]; BYTE st[STAT_SIZE]; } g_stat_kept[STAT_SLOTS];
static volatile LONG g_stat_lock;
static volatile LONG64 g_stat_calls, g_stat_kept_answers;
static long long data_stat_(const char *path, void *st) {
    if (!path || strncmp(path, "/data/", 6) != 0 || !st) return r_data_stat(path, st);
    const LONG64 n = InterlockedIncrement64(&g_stat_calls);
    if (n == 1 << 16 || n == 1 << 20 || n == 1 << 24)
        pc64_log("data stat: %lld looks at the game's data, %lld answered from those kept", (long long)n, (long long)g_stat_kept_answers);
    size_t len = 0;
    unsigned h = 2166136261u;
    for (; path[len] && len < sizeof g_stat_kept[0].path; len++) h = (h ^ (BYTE)path[len]) * 16777619u;
    if (len >= sizeof g_stat_kept[0].path || InterlockedCompareExchange(&g_stat_lock, 1, 0) != 0) return r_data_stat(path, st);
    long long r;
    const unsigned i = h % STAT_SLOTS;
    if (g_stat_kept[i].path[0] && g_stat_kept[i].h == h && !strcmp(g_stat_kept[i].path, path)) {
        r = g_stat_kept[i].r;
        if ((BYTE)r) memcpy(st, g_stat_kept[i].st, STAT_SIZE);
        g_stat_kept_answers++;
    } else {
        r = r_data_stat(path, st);
        g_stat_kept[i].h = h, g_stat_kept[i].r = r;
        memcpy(g_stat_kept[i].path, path, len + 1);
        if ((BYTE)r) memcpy(g_stat_kept[i].st, st, STAT_SIZE);
        static int probed;   // once: how much of the record AVS2 writes (more than kept would be a wrong answer)
        if ((BYTE)r && !probed++) {
            BYTE big[0x100];
            memset(big, 0xcc, sizeof big);
            r_data_stat(path, big);
            int last = 0;
            for (int k = 0; k < (int)sizeof big; k++)
                if (big[k] != 0xcc) last = k + 1;
            pc64_log("data stat: AVS2 writes %#x bytes of the stat record (%#x kept)", last, STAT_SIZE);
        }
    }
    InterlockedExchange(&g_stat_lock, 0);
    return r;
}
static void data_stat_install(HMODULE game) {
    const char *v = getenv("HK_PC64_DATA_STAT");
    if (!game || !pc64_game_is_iidx() || (v && *v == '0')) return;
    HMODULE avs = GetModuleHandleA("avs2-core.dll");
    void *stat = avs ? (void *)GetProcAddress(avs, "XCgsqzn0000063") : NULL;   // AVS2's stat (2.17)
    void *o = stat ? pc64_import_ordinal(game, "avs2-core.dll", 100, (void *)data_stat_) : NULL;
    if (!o) {
        pc64_log("data stat: AVS2's stat not found in the game's imports: every look goes to AVS2");
        return;
    }
    if (o != stat && o != (void *)avs_stat_) {   // not AVS2's stat (another AVS2): put back
        pc64_import_ordinal(game, "avs2-core.dll", 100, o);
        pc64_log("data stat: the game's import 100 is not AVS2's stat: every look goes to AVS2");
        return;
    }
    r_data_stat = (long long (*)(const char *, void *))o;   // AVS2's, or the mods' look above (it chains to AVS2's)
    pc64_log("data stat: looks at the game's data (/data/...) answered from the first answer kept (HK_PC64_DATA_STAT=0: off)");
}

// An older game's banner (a 246x60 picture; this game's are 488x118) is set on its frame's clip at its
// own size: a quarter of the frame, the frame's "Loading" around it. A texture of that size is
// stretched to its clip instead (afp-core's texture call, ordinal 137, its fit flag). The texture:
// its atlas's width and height (+4, +8), its corners there as fractions (+12: u0, u1, v0, v1)
typedef int (*set_texture_fn)(int clip, const void *texture, char fit);
static set_texture_fn r_set_texture;
static int set_texture_(int clip, const void *texture, char fit) {
    if (texture && !fit) {
        uint32_t aw, ah;
        float uv[4];
        memcpy(&aw, (const char *)texture + 4, 4), memcpy(&ah, (const char *)texture + 8, 4), memcpy(uv, (const char *)texture + 12, 16);
        const float w = (uv[1] - uv[0]) * (float)aw, h = (uv[3] - uv[2]) * (float)ah;
        if (w > 230 && w < 260 && h > 54 && h < 66) fit = 1;
    }
    return r_set_texture(clip, texture, fit);
}

void pc64_files_install(HMODULE game, const char *game_dir) {
    if (game_dir) snprintf(g_game, sizeof g_game, "%s", game_dir);
    const char *fl = getenv("HK_PC64_FILE_LOG");
    g_file_log = fl && *fl && *fl != '0';
    g_game_fallback = g_game[0] && pc64_game_is_iidx();
    r_avs_fs_mount = pc64_import_ordinal(game, "avs2-core.dll", 76, (void *)avs_fs_mount_);
    const char *overlay = getenv("HK_PC64_OVERLAY"), *data = getenv("HK_PC64_DATA");
    if (!g_game[0] && ((overlay && *overlay) || (data && *data))) GetCurrentDirectoryA(sizeof g_game, g_game);
    if (overlay && *overlay) add_layer(overlay, "");
    g_first_mod_layer = g_nlayers;
    for (const char *d = data; d && *d;) {
        const char *end = strchr(d, ';');
        char dir[MAX_PATH];
        snprintf(dir, sizeof dir, "%.*s", (int)(end ? end - d : (int)strlen(d)), d);
        add_layer(dir, pc64_data_folder());
        d = end ? end + 1 : NULL;
    }
    HMODULE avs = GetModuleHandleA("avs2-core.dll");
    // HK_PC64_FILE_LOG: the game DLL's own file calls hooked too, to log them; the game's-folder
    // fallback needs them as well (first: they give the real targets when AVS2 doesn't import the
    // same), then AVS2's, so its internal opens are logged as well
    if ((g_file_log || g_game_fallback) && game) {
        void *o;
        if ((o = pc64_import(game, "kernel32.dll", "CreateFileW", (void *)CreateFileW_))) r_CreateFileW = o;
        if ((o = pc64_import(game, "kernel32.dll", "CreateFileA", (void *)CreateFileA_))) r_CreateFileA = o;
        if ((o = pc64_import(game, "kernel32.dll", "FindFirstFileA", (void *)FindFirstFileA_))) r_FindFirstFileA = o;
        if ((o = pc64_import(game, "kernel32.dll", "FindFirstFileExA", (void *)FindFirstFileExA_))) r_FindFirstFileExA = o;
        if ((o = pc64_import(game, "kernel32.dll", "FindFirstFileExW", (void *)FindFirstFileExW_))) r_FindFirstFileExW = o;
        if ((o = pc64_import(game, "kernel32.dll", "GetFileAttributesA", (void *)GetFileAttributesA_))) r_GetFileAttributesA = o;
        if ((o = pc64_import(game, "kernel32.dll", "GetFileAttributesW", (void *)GetFileAttributesW_))) r_GetFileAttributesW = o;
        if ((o = pc64_import(game, "kernel32.dll", "GetFileAttributesExW", (void *)GetFileAttributesExW_))) r_GetFileAttributesExW = o;
    }
    if ((g_nlayers || g_file_log || g_game_fallback) && avs) {
        void *o;
        if ((o = pc64_import(avs, "kernel32.dll", "CreateFileW", (void *)CreateFileW_)) && !r_CreateFileW) r_CreateFileW = o;
        if ((o = pc64_import(avs, "kernel32.dll", "CreateFileA", (void *)CreateFileA_)) && !r_CreateFileA) r_CreateFileA = o;
        if ((o = pc64_import(avs, "kernel32.dll", "FindFirstFileA", (void *)FindFirstFileA_)) && !r_FindFirstFileA) r_FindFirstFileA = o;
        if ((o = pc64_import(avs, "kernel32.dll", "FindFirstFileExA", (void *)FindFirstFileExA_)) && !r_FindFirstFileExA) r_FindFirstFileExA = o;
        if ((o = pc64_import(avs, "kernel32.dll", "FindFirstFileExW", (void *)FindFirstFileExW_)) && !r_FindFirstFileExW) r_FindFirstFileExW = o;
        if (g_file_log || g_game_fallback) {
            if ((o = pc64_import(avs, "kernel32.dll", "GetFileAttributesA", (void *)GetFileAttributesA_)) && !r_GetFileAttributesA) r_GetFileAttributesA = o;
            if ((o = pc64_import(avs, "kernel32.dll", "GetFileAttributesW", (void *)GetFileAttributesW_)) && !r_GetFileAttributesW) r_GetFileAttributesW = o;
            if ((o = pc64_import(avs, "kernel32.dll", "GetFileAttributesExW", (void *)GetFileAttributesExW_)) && !r_GetFileAttributesExW) r_GetFileAttributesExW = o;
        }
    }
    if (g_nlayers > g_first_mod_layer || g_file_log) {
        r_avs_open = pc64_import_ordinal(game, "avs2-core.dll", 79, (void *)avs_open_);
        r_avs_stat = pc64_import_ordinal(game, "avs2-core.dll", 100, (void *)avs_stat_);
        if (!r_avs_open || !r_avs_stat) pc64_log("overlay: the game's AVS2 open and stat not found: no pictures from the mods' packs");
        if (g_nlayers > g_first_mod_layer) r_set_texture = pc64_import_ordinal(game, "afp-core.dll", 137, (void *)set_texture_);
    }
    if (!r_avs_fs_mount) pc64_log("the game doesn't mount its data through AVS2 (ordinal 76)");
    data_stat_install(game);   // (after the mods' look: it goes through it the first time)
}
