// SPDX-License-Identifier: GPL-3.0-or-later
// The mods' folders over the game's data (PLAN.md section 35), as ifs_layeredfs lays them out:
//   - a file at <mod>/a/b is the game's data/a/b, new files too (AVS2 opens and looks for files with
//     libavs-win32's CreateFileA and FindFirstFileA: those take the mod's when it has one);
//   - inside a pack: the game reads a pack through a mount (avs_fs_mount, imagefs: a song's banner
//     pack data/tex/system/kc_diff.ifs at /ifs_image_kc, the sounds' at /ifs_sd_mp, each texture
//     pack at /afpN/data/...), then its members by path (/ifs_image_kc/kc_0001.ifs). A file in the
//     mod's folder named after the pack with _ifs (tex/system/kc_diff_ifs/kc_0001.ifs) is that
//     member: the game's open and stat of it (from popn22.dll, and from the texture library through
//     its AVS2 shim, _ibavs-win32.dll) go to the mod's file, through a mount of the mod's folder
//     (/hiraku_modN);
//   - a pack inside a pack (a character's pictures, /afp5/ifs_image_hariai/ha_mimi_28a.ifs mounted
//     from the member /ifs_image_hariai/ha_mimi_28a.ifs) is mounted from the mod's file the same way,
//     and its own members looked for in <pack>_ifs/<member's name>_ifs/.
// Not yet: pictures as PNGs (<pack>_ifs/tex/name.png: a texture pack keeps its pictures converted,
// under their names' MD5, listed in its tex/texturelist.xml).
#include "pc32.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_mods[16][MAX_PATH];
static int g_nmods;
static pc32_mount_fn r_mount;
static pc32_open_fn r_open;
static pc32_stat_fn r_stat;
static HANDLE(WINAPI *r_CreateFileA)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static HANDLE(WINAPI *r_FindFirstFileA)(LPCSTR, LPWIN32_FIND_DATAA);

static int is_file(const char *path) {
    const DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
static void backslashes(char *p) {
    for (; *p; p++)
        if (*p == '/') *p = '\\';
}
static void said(const char *what, const char *from, const char *to) {
    static int n;
    if (n++ < 300) pc32_log("mods: %s %s from %s", what, from, to);
}

// --- whole files: a path AVS2 opens (relative to its root, the run folder: data\sd\...) in a mod
static int in_mod_folder(const char *path, char *out, size_t size) {
    if (!g_nmods || !path || strpbrk(path, "*?")) return 0;
    const char *rest = path;
    if (!strncmp(rest, "./", 2) || !strncmp(rest, ".\\", 2)) rest += 2;
    if (_strnicmp(rest, "data", 4) || (rest[4] != '/' && rest[4] != '\\')) return 0;
    rest += 5;
    for (int i = 0; i < g_nmods; i++) {
        snprintf(out, size, "%s\\%s", g_mods[i], rest);
        backslashes(out);
        if (is_file(out)) return 1;
    }
    return 0;
}
static HANDLE WINAPI CreateFileA_(LPCSTR path, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    char moved[MAX_PATH * 2];
    if (!(acc & GENERIC_WRITE) && in_mod_folder(path, moved, sizeof moved)) {
        said("file", path, moved);
        return r_CreateFileA(moved, acc, share, sa, disp, flags, tmpl);
    }
    return r_CreateFileA(path, acc, share, sa, disp, flags, tmpl);
}
static HANDLE WINAPI FindFirstFileA_(LPCSTR path, LPWIN32_FIND_DATAA data) {
    char moved[MAX_PATH * 2];
    return r_FindFirstFileA(in_mod_folder(path, moved, sizeof moved) ? moved : path, data);
}

// --- inside packs: each mount of a pack, and the folder name a mod gives its members
static struct {
    char mountpoint[MAX_PATH], folder[MAX_PATH];
} g_packs[256];
static int g_npacks;
static int g_mounted[16];   // the mod's folder mounted at /hiraku_modN

// The pack a path is in (the longest mount point it starts with), and the member's path in it
static int pack_of(const char *path, const char **member) {
    int best = -1;
    size_t best_n = 0;
    for (int i = 0; i < g_npacks; i++) {
        const size_t n = strlen(g_packs[i].mountpoint);
        if (n > best_n && !strncmp(path, g_packs[i].mountpoint, n) && path[n] == '/') best = i, best_n = n;
    }
    if (best >= 0) *member = path + best_n + 1;
    return best;
}
// A member's file in a mod: its AVS2 path (/hiraku_modN/...) in out, or 0
static int member_in_mod(int pack, const char *member, char *out, size_t size) {
    if (!g_nmods || strstr(member, "..")) return 0;
    for (int i = 0; i < g_nmods; i++) {
        char host[MAX_PATH * 2];
        snprintf(host, sizeof host, "%s\\%s\\%s", g_mods[i], g_packs[pack].folder, member);
        backslashes(host);
        if (!is_file(host)) continue;
        if (!g_mounted[i]) {
            char mp[32];
            snprintf(mp, sizeof mp, "/hiraku_mod%d", i);
            g_mounted[i] = 1;
            if (r_mount(mp, g_mods[i], "fs", NULL) < 0) pc32_log("mods: %s couldn't be mounted at %s", g_mods[i], mp);
        }
        snprintf(out, size, "/hiraku_mod%d/%s/%s", i, g_packs[pack].folder, member);
        return 1;
    }
    return 0;
}
// a pack's file name made its members' folder name: x.ifs -> x_ifs
static void folder_of(const char *file, char *out, size_t size) {
    snprintf(out, size, "%s", file);
    const size_t n = strlen(out);
    if (n > 4 && !_stricmp(out + n - 4, ".ifs")) out[n - 4] = '_';
}

const char *pc32_files_mount(const char *mountpoint, const char *source, const char *fstype, char *buf, size_t n) {
    if (!mountpoint || !source || !fstype || _stricmp(fstype, "imagefs")) return source;
    char folder[MAX_PATH];
    const char *member = NULL;
    const int outer = source[0] == '/' ? pack_of(source, &member) : -1;
    const char *from = source;
    if (!_strnicmp(source, "/data/", 6)) {
        folder_of(source + 6, folder, sizeof folder);
    } else if (outer >= 0) {   // a pack inside a pack
        char inner[MAX_PATH];
        folder_of(member, inner, sizeof inner);
        snprintf(folder, sizeof folder, "%s/%s", g_packs[outer].folder, inner);
        if (member_in_mod(outer, member, buf, n)) {
            said("pack", source, buf);
            from = buf;
        }
    } else {
        return source;
    }
    int i = 0;   // the mount point again (the texture library mounts its packs as it needs them): its entry
    while (i < g_npacks && strcmp(g_packs[i].mountpoint, mountpoint)) i++;
    if (i == g_npacks) {
        if (g_npacks == (int)(sizeof g_packs / sizeof *g_packs)) return from;
        g_npacks++;
    }
    snprintf(g_packs[i].mountpoint, sizeof g_packs[i].mountpoint, "%s", mountpoint);
    snprintf(g_packs[i].folder, sizeof g_packs[i].folder, "%s", folder);
    return from;
}

static int open_(const char *path, int mode, int perm) {
    char moved[MAX_PATH * 2];
    const char *member;
    const int pack = path ? pack_of(path, &member) : -1;
    if (pack >= 0 && member_in_mod(pack, member, moved, sizeof moved)) {
        said("member", path, moved);
        path = moved;
    }
    const int r = r_open(path, mode, perm);
    if (pc32_trace()) pc32_log("trace: open %s -> %d", path ? path : "?", r);
    return r;
}
static int stat_(const char *path, void *st) {
    char moved[MAX_PATH * 2];
    const char *member;
    const int pack = path ? pack_of(path, &member) : -1;
    if (pack >= 0 && member_in_mod(pack, member, moved, sizeof moved)) path = moved;
    return r_stat(path, st);
}

void pc32_files_install(HMODULE core, HMODULE game, pc32_mount_fn mount, pc32_open_fn open, pc32_stat_fn stat) {
    r_mount = mount, r_open = open, r_stat = stat;
    for (const char *d = getenv("HK_PC32_DATA"); d && *d && g_nmods < (int)(sizeof g_mods / sizeof *g_mods);) {
        const char *end = strchr(d, ';');
        snprintf(g_mods[g_nmods], MAX_PATH, "%.*s", (int)(end ? end - d : (int)strlen(d)), d);
        backslashes(g_mods[g_nmods]);
        if (g_mods[g_nmods][0]) pc32_log("mods: %s over the game's data", g_mods[g_nmods]), g_nmods++;
        d = end ? end + 1 : NULL;
    }
    if (!g_nmods && !pc32_trace()) return;
    if (g_nmods) {
        r_CreateFileA = (void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "CreateFileA");
        r_FindFirstFileA = (void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "FindFirstFileA");
        if (!pc32_hook_target(core, (void *)r_CreateFileA, (void *)CreateFileA_) ||
            !pc32_hook_target(core, (void *)r_FindFirstFileA, (void *)FindFirstFileA_))
            pc32_log("mods: AVS2 doesn't open files as expected: whole files not from the mods");
    }
    HMODULE shim = GetModuleHandleA("_ibavs-win32.dll");
    HMODULE where[] = {game, shim};
    for (int k = 0; k < 2; k++) {
        if (!where[k]) continue;
        pc32_hook_target(where[k], (void *)r_open, (void *)open_);
        pc32_hook_target(where[k], (void *)r_stat, (void *)stat_);
    }
}
