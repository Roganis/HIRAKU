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
static int (*r_avs_fs_mount)(const char *mountpoint, const char *source, const char *fstype, void *data);

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
static HANDLE(WINAPI *r_CreateFileW)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static HANDLE(WINAPI *r_CreateFileA)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static HANDLE(WINAPI *r_FindFirstFileA)(LPCSTR, LPWIN32_FIND_DATAA);
static HANDLE(WINAPI *r_FindFirstFileExW)(LPCWSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD);

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
    if (!(acc & GENERIC_WRITE) && overlaid(path, moved, sizeof moved)) {
        said(moved);
        return r_CreateFileA(moved, acc, share, sa, disp, flags, tmpl);
    }
    return r_CreateFileA(path, acc, share, sa, disp, flags, tmpl);
}
// a wide path's layer file, wide (0: none)
static int overlaid_w(LPCWSTR path, WCHAR *out, int size) {
    char narrow[MAX_PATH * 2], moved[MAX_PATH * 2];
    return g_nlayers && path && WideCharToMultiByte(CP_UTF8, 0, path, -1, narrow, sizeof narrow, NULL, NULL) &&
           overlaid(narrow, moved, sizeof moved) && MultiByteToWideChar(CP_UTF8, 0, moved, -1, out, size) && (said(moved), 1);
}
static HANDLE WINAPI CreateFileW_(LPCWSTR path, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    WCHAR wide[MAX_PATH * 2];
    if (!(acc & GENERIC_WRITE) && overlaid_w(path, wide, MAX_PATH * 2)) return r_CreateFileW(wide, acc, share, sa, disp, flags, tmpl);
    return r_CreateFileW(path, acc, share, sa, disp, flags, tmpl);
}
// AVS2's look at one file (its size, that it's there): the layer's (a folder's listing stays the game's)
static HANDLE WINAPI FindFirstFileA_(LPCSTR path, LPWIN32_FIND_DATAA data) {
    char moved[MAX_PATH * 2];
    return r_FindFirstFileA(overlaid(path, moved, sizeof moved) ? moved : path, data);
}
static HANDLE WINAPI FindFirstFileExW_(LPCWSTR path, FINDEX_INFO_LEVELS level, LPVOID data, FINDEX_SEARCH_OPS op, LPVOID filter, DWORD flags) {
    WCHAR wide[MAX_PATH * 2];
    return r_FindFirstFileExW(overlaid_w(path, wide, MAX_PATH * 2) ? wide : path, level, data, op, filter, flags);
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
        return r_avs_open(m, mode, perm);
    }
    return r_avs_open(path, mode, perm);
}
static int avs_stat_(const char *path, void *st) {
    char moved[MAX_PATH * 2];
    const char *m = in_mods(path, moved, sizeof moved);
    return r_avs_stat(m ? m : path, st);
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
    if (g_nlayers && avs) {
        r_CreateFileW = pc64_import(avs, "kernel32.dll", "CreateFileW", (void *)CreateFileW_);
        r_CreateFileA = pc64_import(avs, "kernel32.dll", "CreateFileA", (void *)CreateFileA_);
        r_FindFirstFileA = pc64_import(avs, "kernel32.dll", "FindFirstFileA", (void *)FindFirstFileA_);
        r_FindFirstFileExW = pc64_import(avs, "kernel32.dll", "FindFirstFileExW", (void *)FindFirstFileExW_);
    }
    if (g_nlayers > g_first_mod_layer) {
        r_avs_open = pc64_import_ordinal(game, "avs2-core.dll", 79, (void *)avs_open_);
        r_avs_stat = pc64_import_ordinal(game, "avs2-core.dll", 100, (void *)avs_stat_);
        if (!r_avs_open || !r_avs_stat) pc64_log("overlay: the game's AVS2 open and stat not found: no pictures from the mods' packs");
        r_set_texture = pc64_import_ordinal(game, "afp-core.dll", 137, (void *)set_texture_);
    }
    if (!r_avs_fs_mount) pc64_log("the game doesn't mount its data through AVS2 (ordinal 76)");
}
