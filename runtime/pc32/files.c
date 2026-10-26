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
//   - pictures as PNGs: a texture pack keeps each picture as a member of its own, tex/<the MD5 of its
//     name>, its pixels BGRA (argb8888rev; the size its tex/texturelist.xml gives), packed (avslz: the
//     sizes unpacked and packed, big endian, then Konami's LZ) when the list says so. A mod's
//     <pack>_ifs/tex/<name>.png (or <pack>_ifs/<name>.png) is that member: decoded (Windows' WIC),
//     written as the game's own (packed when it is: as literals), kept in the save folder's
//     dev\hiraku_tex, and read from there (/hiraku_tex). A picture the pack hasn't comes with the mod's
//     own tex/texturelist.xml listing it (a member as any other), packed as that list says.
//     Not yet: dxt5 pictures, and pictures added without a list of the mod's.
#define COBJMACROS
#include "pc32.h"

#include <wincodec.h>
#include <wincrypt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_mods[16][MAX_PATH];
static int g_nmods;
static pc32_mount_fn r_mount;
static pc32_open_fn r_open;
static pc32_stat_fn r_stat;
static pc32_read_fn r_read;
static pc32_close_fn r_close;
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

// --- pictures as PNGs
static int g_tex_mounted;
static char g_tex_dir[MAX_PATH];   // dev\hiraku_tex, in full

// the MD5 of a picture's name, as 32 lowercase hex digits (CryptoAPI)
static int md5_hex(const char *name, char out[33]) {
    HCRYPTPROV prov;
    HCRYPTHASH h;
    BYTE sum[16];
    DWORD n = sizeof sum;
    if (!CryptAcquireContextA(&prov, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) return 0;
    const int ok = CryptCreateHash(prov, CALG_MD5, 0, 0, &h) && CryptHashData(h, (const BYTE *)name, (DWORD)strlen(name), 0) &&
                   CryptGetHashParam(h, HP_HASHVAL, sum, &n, 0);
    if (ok) CryptDestroyHash(h);
    CryptReleaseContext(prov, 0);
    for (int i = 0; ok && i < 16; i++) snprintf(out + 2 * i, 3, "%02x", sum[i]);
    return ok;
}
// a mod's PNG for the member tex/<md5> of a pack: its path (out), or 0. The mod's pictures of each
// pack are named once, the first time one of the pack's members is asked for
static struct {
    char folder[MAX_PATH];
    int n;
    struct { char md5[33]; char png[MAX_PATH]; } *pics;
} g_pngs[64];
static int g_npngs;
static void add_pngs(int k, const char *dir) {
    char pattern[MAX_PATH * 2];
    snprintf(pattern, sizeof pattern, "%s\\*.png", dir);
    WIN32_FIND_DATAA d;
    HANDLE h = FindFirstFileA(pattern, &d);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char name[MAX_PATH];
        snprintf(name, sizeof name, "%s", d.cFileName);
        name[strlen(name) - 4] = 0;   // .png
        void *more = realloc(g_pngs[k].pics, sizeof *g_pngs[k].pics * (size_t)(g_pngs[k].n + 1));
        if (!more) break;
        g_pngs[k].pics = more;
        if (!md5_hex(name, g_pngs[k].pics[g_pngs[k].n].md5)) continue;
        snprintf(g_pngs[k].pics[g_pngs[k].n].png, MAX_PATH, "%s\\%s", dir, d.cFileName);
        g_pngs[k].n++;
    } while (FindNextFileA(h, &d));
    FindClose(h);
}
static const char *png_for(int pack, const char *md5) {
    int k = 0;
    while (k < g_npngs && strcmp(g_pngs[k].folder, g_packs[pack].folder)) k++;
    if (k == g_npngs) {
        if (g_npngs == (int)(sizeof g_pngs / sizeof *g_pngs)) return NULL;
        g_npngs++;
        snprintf(g_pngs[k].folder, MAX_PATH, "%s", g_packs[pack].folder);
        for (int i = g_nmods - 1; i >= 0; i--) {   // the first mod's last: found first below
            char dir[MAX_PATH * 2];
            snprintf(dir, sizeof dir, "%s\\%s\\tex", g_mods[i], g_packs[pack].folder);
            backslashes(dir);
            add_pngs(k, dir);
            snprintf(dir, sizeof dir, "%s\\%s", g_mods[i], g_packs[pack].folder);
            backslashes(dir);
            add_pngs(k, dir);
        }
    }
    for (int i = g_pngs[k].n - 1; i >= 0; i--)
        if (!strcmp(g_pngs[k].pics[i].md5, md5)) return g_pngs[k].pics[i].png;
    return NULL;
}
// a PNG's pixels, BGRA (Windows' WIC decodes it), and its size: malloc'd, NULL when it can't be read
static BYTE *decode_png(const char *path, unsigned *w, unsigned *h) {
    static int com;
    if (!com) com = SUCCEEDED(CoInitializeEx(NULL, COINIT_MULTITHREADED)) ? 1 : -1;
    IWICImagingFactory *f = NULL;
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *conv = NULL;
    WCHAR wide[MAX_PATH * 2];
    BYTE *px = NULL;
    MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH * 2);
    if (SUCCEEDED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&f)) &&
        SUCCEEDED(IWICImagingFactory_CreateDecoderFromFilename(f, wide, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(IWICBitmapDecoder_GetFrame(dec, 0, &frame)) && SUCCEEDED(IWICImagingFactory_CreateFormatConverter(f, &conv)) &&
        SUCCEEDED(IWICFormatConverter_Initialize(conv, (IWICBitmapSource *)frame, &GUID_WICPixelFormat32bppBGRA,
                                                 WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(IWICFormatConverter_GetSize(conv, w, h)) && *w && *h && *w <= 8192 && *h <= 8192 &&
        (px = malloc((size_t)*w * *h * 4)) &&
        FAILED(IWICFormatConverter_CopyPixels(conv, NULL, *w * 4, *w * *h * 4, px)))
        free(px), px = NULL;
    if (conv) IWICFormatConverter_Release(conv);
    if (frame) IWICBitmapFrameDecode_Release(frame);
    if (dec) IWICBitmapDecoder_Release(dec);
    if (f) IWICImagingFactory_Release(f);
    return px;
}
// the game's own member's first bytes (n of them): how many it has (0: no such member)
static int member_head(const char *path, BYTE *head, int n) {
    const int fd = r_open(path, 1, 0444);
    if (fd < 0) return 0;
    const int got = r_read(fd, head, (unsigned)n);
    r_close(fd);
    return got > 0 ? got : 0;
}
// does the mod's own tex/texturelist.xml for the pack say avslz? (-1: it has none)
static int list_packs(int pack) {
    char path[MAX_PATH * 2], buf[512];
    for (int i = 0; i < g_nmods; i++) {
        snprintf(path, sizeof path, "%s\\%s\\tex\\texturelist.xml", g_mods[i], g_packs[pack].folder);
        backslashes(path);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        const size_t n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[n] = 0;
        return strstr(buf, "avslz") != NULL;
    }
    return -1;
}
// The member tex/<md5> from a mod's PNG: its AVS2 path (/hiraku_tex/...) in out, or 0
static int picture_in_mod(int pack, const char *path, const char *member, char *out, size_t size) {
    if (!g_nmods || strncmp(member, "tex/", 4) || strlen(member) != 36 || strspn(member + 4, "0123456789abcdef") != 32) return 0;
    const char *png = png_for(pack, member + 4);
    if (!png) return 0;
    char name[MAX_PATH], file[MAX_PATH * 2];
    snprintf(name, sizeof name, "%s_%s", g_packs[pack].folder, member + 4);
    for (char *c = name; *c; c++)
        if (*c == '/' || *c == '\\') *c = '_';
    snprintf(file, sizeof file, "%s\\%s", g_tex_dir, name);
    if (!is_file(file)) {   // made once a run (a PNG changed between runs is made again: the folder starts empty)
        unsigned w, h;
        BYTE head[8];
        const int had = member_head(path, head, 8);
        BYTE *px = decode_png(png, &w, &h);
        if (!px) {
            pc32_log("mods: %s couldn't be read as a PNG", png);
            return 0;
        }
        const uint32_t raw = w * h * 4, said_raw = had == 8 ? (uint32_t)head[0] << 24 | head[1] << 16 | head[2] << 8 | head[3] : 0;
        // packed or not: as the mod's own list of the pack says when it brings one (its pictures' sizes
        // its own); else as the game's member is, which then must be this size, BGRA (packed: it starts
        // with its unpacked size)
        const int list = list_packs(pack);
        const int packed = list >= 0 ? list : said_raw == raw;
        if (list < 0 && said_raw != raw) {
            pc32_log("mods: %s: the game's picture isn't BGRA %ux%u, packed (%s): left as the game's", png, w, h,
                     !had ? "not in the pack, and no list of the mod's" : said_raw == w * h ? "dxt5" : "another size or format");
            free(px);
            return 0;
        }
        FILE *f = fopen(file, "wb");
        if (!f) {
            free(px);
            return 0;
        }
        if (packed) {   // avslz: the sizes, then the pixels as literals (a flag byte of 8 per 8), the end (a 0 bit, 00 00)
            const uint32_t n_lit = raw, n_out = raw + (raw + 7) / 8 + (raw % 8 ? 2 : 3);
            const BYTE sizes[8] = {raw >> 24, raw >> 16, raw >> 8, raw, n_out >> 24, n_out >> 16, n_out >> 8, n_out};
            fwrite(sizes, 1, 8, f);
            for (uint32_t i = 0; i < n_lit; i += 8) {
                const uint32_t k = n_lit - i < 8 ? n_lit - i : 8;
                fputc(k == 8 ? 0xff : (1 << k) - 1, f);
                fwrite(px + i, 1, k, f);
            }
            if (raw % 8 == 0) fputc(0, f);
            fputc(0, f), fputc(0, f);
        } else {
            fwrite(px, 1, raw, f);
        }
        fclose(f);
        free(px);
        pc32_log("mods: picture %s from %s (%ux%u%s)", path, png, w, h, packed ? ", packed" : "");
    }
    if (!g_tex_mounted) g_tex_mounted = r_mount("/hiraku_tex", g_tex_dir, "fs", NULL) >= 0 ? 1 : -1;
    if (g_tex_mounted < 0) return 0;
    snprintf(out, size, "/hiraku_tex/%s", name);
    return 1;
}

static int open_(const char *path, int mode, int perm) {
    char moved[MAX_PATH * 2];
    const char *member;
    const int pack = path ? pack_of(path, &member) : -1;
    if (pack >= 0 && picture_in_mod(pack, path, member, moved, sizeof moved)) {
        path = moved;
    } else if (pack >= 0 && member_in_mod(pack, member, moved, sizeof moved)) {
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
    if (pack >= 0 && (picture_in_mod(pack, path, member, moved, sizeof moved) || member_in_mod(pack, member, moved, sizeof moved)))
        path = moved;
    return r_stat(path, st);
}

void pc32_files_install(HMODULE core, HMODULE game, const struct pc32_avs_files *avs) {
    r_mount = avs->mount, r_open = avs->open, r_stat = avs->stat, r_read = avs->read, r_close = avs->close;
    for (const char *d = getenv("HK_PC32_DATA"); d && *d && g_nmods < (int)(sizeof g_mods / sizeof *g_mods);) {
        const char *end = strchr(d, ';');
        snprintf(g_mods[g_nmods], MAX_PATH, "%.*s", (int)(end ? end - d : (int)strlen(d)), d);
        backslashes(g_mods[g_nmods]);
        if (g_mods[g_nmods][0]) pc32_log("mods: %s over the game's data", g_mods[g_nmods]), g_nmods++;
        d = end ? end + 1 : NULL;
    }
    if (!g_nmods && !pc32_trace()) return;
    if (g_nmods) {   // the pictures made from PNGs: made again each run
        CreateDirectoryA("dev\\hiraku_tex", NULL);
        GetFullPathNameA("dev\\hiraku_tex", sizeof g_tex_dir, g_tex_dir, NULL);
        char pattern[MAX_PATH * 2];
        snprintf(pattern, sizeof pattern, "%s\\*", g_tex_dir);
        WIN32_FIND_DATAA d;
        HANDLE h = FindFirstFileA(pattern, &d);
        for (int more = h != INVALID_HANDLE_VALUE; more; more = FindNextFileA(h, &d)) {
            char old[MAX_PATH * 2];
            snprintf(old, sizeof old, "%s\\%s", g_tex_dir, d.cFileName);
            if (!(d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileA(old);
        }
        if (h != INVALID_HANDLE_VALUE) FindClose(h);
    }
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
