// SPDX-License-Identifier: GPL-3.0-or-later
// A game's program where Windows put it: a mod's addresses and bytes moved with it (imagebase.h)
#include "imagebase.h"

#include <string.h>

static uint32_t u16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t u32(const uint8_t *p) { return u16(p) | u16(p + 2) << 16; }

int hk_image_open(struct hk_image *im, const void *map, uint64_t real) {
    memset(im, 0, sizeof *im);
    const uint8_t *b = (const uint8_t *)map;
    if (!b || b[0] != 'M' || b[1] != 'Z') return 0;
    const uint8_t *pe = b + u32(b + 0x3c), *opt = pe + 24;
    if (memcmp(pe, "PE\0\0", 4)) return 0;
    const uint32_t magic = u16(opt);
    uint64_t base;
    const uint8_t *dirs;
    uint32_t ndirs;
    if (magic == 0x10b) base = u32(opt + 28), ndirs = u32(opt + 92), dirs = opt + 96;
    else if (magic == 0x20b) base = u32(opt + 24) | (uint64_t)u32(opt + 28) << 32, ndirs = u32(opt + 108), dirs = opt + 112;
    else return 0;
    im->map = b, im->preferred = base, im->real = real, im->size = u32(opt + 56);
    if (ndirs > 5 && !(u16(pe + 22) & 1)) {   // IMAGE_DIRECTORY_ENTRY_BASERELOC; not RELOCS_STRIPPED
        im->relocs = u32(dirs + 5 * 8), im->relocs_size = u32(dirs + 5 * 8 + 4);
        if (!im->relocs || im->relocs >= im->size || im->relocs_size > im->size - im->relocs) im->relocs = im->relocs_size = 0;
    }
    return 1;
}

uint64_t hk_image_move(const struct hk_image *im, uint64_t addr) {
    return im->size && addr >= im->preferred && addr - im->preferred < im->size ? addr - im->preferred + im->real : addr;
}

uint64_t hk_image_unmove(const struct hk_image *im, uint64_t addr) {
    return im->size && addr >= im->real && addr - im->real < im->size ? addr - im->real + im->preferred : addr;
}

// One relocated field (w bytes at RVA f) that the patch's range [rva, rva + n) covers, partly or
// whole: its value at the preferred base, the patch's bytes where they cover it (the rest the
// mapped image's, un-relocated), relocated as the loader would (original: always; replace: only an
// address in the image), its bytes in the range written back
static void fix_field(const struct hk_image *im, uint32_t f, int w, uint32_t rva, size_t n, uint8_t *bytes, int always) {
    const uint64_t delta = im->real - im->preferred;
    uint64_t v = 0;
    int outside = 0;
    for (int k = 0; k < w; k++) outside |= f + (uint32_t)k < rva || f + (uint32_t)k - rva >= n;
    if (outside) {   // the mapped field less the move, the patch's bytes over it below
        for (int k = w; k-- > 0;) v = v << 8 | im->map[f + (uint32_t)k];
        v -= delta;
    }
    for (int k = 0; k < w; k++)
        if (f + (uint32_t)k >= rva && f + (uint32_t)k - rva < n)
            v = (v & ~((uint64_t)0xff << 8 * k)) | (uint64_t)bytes[f + (uint32_t)k - rva] << 8 * k;
    if (w == 4) v &= 0xffffffffu;
    if (!always && !(v >= im->preferred && v - im->preferred < im->size)) return;   // not an address: as the mod wrote it
    v += delta;
    for (int k = 0; k < w; k++)
        if (f + (uint32_t)k >= rva && f + (uint32_t)k - rva < n) bytes[f + (uint32_t)k - rva] = (uint8_t)(v >> 8 * k);
}

int hk_image_fix_patch(const struct hk_image *im, uint32_t rva, size_t n, uint8_t *original, uint8_t *replace) {
    if (!im->relocs || im->real == im->preferred || !n) return 0;
    int fields = 0;
    for (uint32_t at = im->relocs, end = im->relocs + im->relocs_size; at + 8 <= end;) {
        const uint32_t page = u32(im->map + at), block = u32(im->map + at + 4);
        if (block < 8 || block > end - at) break;
        // a field in this block starts in [page, page + 0x1000) and is at most 8 bytes long
        if (page < rva + n && page + 0x1000 + 8 > rva)
            for (uint32_t e = at + 8; e + 2 <= at + block; e += 2) {
                const uint32_t entry = u16(im->map + e), type = entry >> 12, f = page + (entry & 0xfff);
                const int w = type == 3 ? 4 : type == 10 ? 8 : 0;   // HIGHLOW, DIR64 (ABSOLUTE: padding)
                if (!w || f + (uint32_t)w <= rva || f >= rva + n || f + (uint32_t)w > im->size) continue;
                if (original) fix_field(im, f, w, rva, n, original, 1);
                if (replace) fix_field(im, f, w, rva, n, replace, 0);
                fields++;
            }
        at += block;
    }
    return fields;
}
