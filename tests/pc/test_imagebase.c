// SPDX-License-Identifier: GPL-3.0-or-later
// A mod's addresses and bytes in a game's DLL loaded away from its base (runtime/pc/imagebase.h),
// on made-up images: a PE32 and a PE32+ header, a base relocation table, the image "loaded" by
// applying it as Windows' loader would, then patches written at the preferred base checked against it
#include "imagebase.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, failed;
#define CHECK(c)                                                         \
    do {                                                                 \
        checks++;                                                        \
        if (!(c)) failed++, fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); \
    } while (0)

enum { SIZE = 0x4000, RELOCS = 0x3000 };

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v), put16(p + 2, v >> 16); }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v), put32(p + 4, (uint32_t)(v >> 32)); }

// A PE image's headers (wide: PE32+), its relocation table at RELOCS: one block for page 0x1000 with
// `n` entries
static void make(uint8_t *img, int wide, uint64_t base, const uint16_t *entries, int n, int stripped) {
    memset(img, 0, SIZE);
    img[0] = 'M', img[1] = 'Z';
    put32(img + 0x3c, 0x80);
    uint8_t *pe = img + 0x80, *opt = pe + 24;
    memcpy(pe, "PE\0\0", 4);
    put16(pe + 22, 0x2000 | (stripped ? 1 : 0));   // a DLL
    put16(opt, wide ? 0x20b : 0x10b);
    if (wide) put64(opt + 24, base), put32(opt + 108, 16);
    else put32(opt + 28, (uint32_t)base), put32(opt + 92, 16);
    put32(opt + 56, SIZE);
    uint8_t *dir = opt + (wide ? 112 : 96) + 5 * 8;
    const uint32_t block = 8 + 2 * (uint32_t)((n + 1) & ~1);   // (padded to 4 bytes with an ABSOLUTE entry)
    put32(dir, RELOCS), put32(dir + 4, block);
    put32(img + RELOCS, 0x1000), put32(img + RELOCS + 4, block);
    for (int i = 0; i < n; i++) put16(img + RELOCS + 8 + 2 * i, entries[i]);
}

// What Windows' loader does to it at `real`
static void load(uint8_t *img, uint64_t base, uint64_t real) {
    const uint32_t block = img[RELOCS + 4] | img[RELOCS + 5] << 8;
    for (uint32_t e = RELOCS + 8; e < RELOCS + block; e += 2) {
        const uint32_t entry = img[e] | img[e + 1] << 8, f = 0x1000 + (entry & 0xfff);
        if (entry >> 12 == 3) {
            uint32_t v = img[f] | img[f + 1] << 8 | img[f + 2] << 16 | (uint32_t)img[f + 3] << 24;
            put32(img + f, v + (uint32_t)(real - base));
        } else if (entry >> 12 == 10) {
            uint64_t v = 0;
            for (int k = 8; k-- > 0;) v = v << 8 | img[f + k];
            put64(img + f, v + (real - base));
        }
    }
}

static void pe32(void) {
    const uint64_t base = 0x10000000, real = 0x07b30000;
    static uint8_t file[SIZE], map[SIZE];
    // fields: an `add [0x10001ffc], ebx` at 0x1100 (its operand at 0x1102); a pointer at 0x1200 to
    // something outside the image (0x12ff0000: the loader moves it all the same); one at 0x1ffe
    // across the page's end
    const uint16_t entries[] = {0x3000 | 0x102, 0x3000 | 0x200, 0x3000 | 0xffe};
    make(file, 0, base, entries, 3, 0);
    const uint8_t add[6] = {0x01, 0x1d, 0xfc, 0x1f, 0x00, 0x10};
    memcpy(file + 0x1100, add, 6);
    put32(file + 0x1200, 0x12ff0000);
    put32(file + 0x1ffe, 0x10003000);
    memcpy(map, file, SIZE);
    load(map, base, real);

    struct hk_image im;
    CHECK(hk_image_open(&im, map, real));
    CHECK(im.preferred == base && im.size == SIZE && im.relocs == RELOCS);
    // addresses: in the image moved, outside as they are, and back
    CHECK(hk_image_move(&im, 0x10001100) == 0x07b31100);
    CHECK(hk_image_move(&im, 0x10003fff) == 0x07b33fff);
    CHECK(hk_image_move(&im, 0x10004000) == 0x10004000);
    CHECK(hk_image_move(&im, 0x0fffffff) == 0x0fffffff);
    CHECK(hk_image_move(&im, 0x00400000) == 0x00400000);
    CHECK(hk_image_unmove(&im, 0x07b31100) == 0x10001100);
    CHECK(hk_image_unmove(&im, 0x10001100) == 0x10001100);

    // the whole instruction: its original as loaded; a nop over it as written; a replace that
    // keeps the operand (add [x], 0 -> add [x], 0 with another opcode byte) moved
    uint8_t o[6], r[6];
    memcpy(o, add, 6);
    static const uint8_t nop[6] = {0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00};
    memcpy(r, nop, 6);
    CHECK(hk_image_fix_patch(&im, 0x1100, 6, o, r) == 1);
    CHECK(!memcmp(o, map + 0x1100, 6));
    CHECK(!memcmp(r, nop, 6));
    static const uint8_t keep[6] = {0x29, 0x1d, 0xfc, 0x1f, 0x00, 0x10};   // sub [0x10001ffc], ebx
    memcpy(r, keep, 6);
    CHECK(hk_image_fix_patch(&im, 0x1100, 6, NULL, r) == 1);
    CHECK(r[0] == 0x29 && r[1] == 0x1d && !memcmp(r + 2, map + 0x1102, 4));
    // a replace's address elsewhere in the image (not the one it replaces): moved too
    put32(r + 2, 0x10002000);
    hk_image_fix_patch(&im, 0x1100, 6, NULL, r);
    CHECK((r[2] | r[3] << 8 | r[4] << 16 | (uint32_t)r[5] << 24) == 0x07b32000);
    // ... an address outside the image: as written
    put32(r + 2, 0x20000000);
    hk_image_fix_patch(&im, 0x1100, 6, NULL, r);
    CHECK((r[2] | r[3] << 8 | r[4] << 16 | (uint32_t)r[5] << 24) == 0x20000000);

    // a range with no field: nothing
    memcpy(o, file + 0x1104, 2);
    CHECK(hk_image_fix_patch(&im, 0x1106, 2, o, NULL) == 0);

    // part of a field: its last two bytes, then its last byte alone, the first two (unmoved by a
    // move of whole 64 KB), across the page's end
    uint8_t p[4];
    memcpy(p, file + 0x1104, 2);
    CHECK(hk_image_fix_patch(&im, 0x1104, 2, p, NULL) == 1);
    CHECK(!memcmp(p, map + 0x1104, 2));
    p[0] = file[0x1203];
    CHECK(hk_image_fix_patch(&im, 0x1203, 1, p, NULL) == 1);   // (0x12ff0000 - 0x084d0000: a carry into it)
    CHECK(p[0] == map[0x1203]);
    memcpy(p, file + 0x1200, 2);
    hk_image_fix_patch(&im, 0x1200, 2, p, NULL);
    CHECK(!memcmp(p, map + 0x1200, 2));
    memcpy(p, file + 0x1fff, 4);
    CHECK(hk_image_fix_patch(&im, 0x1fff, 4, p, NULL) == 1);
    CHECK(!memcmp(p, map + 0x1fff, 4));
    // two fields in one range
    uint8_t big[0x110];
    memcpy(big, file + 0x1100, sizeof big);
    CHECK(hk_image_fix_patch(&im, 0x1100, sizeof big, big, NULL) == 2);
    CHECK(!memcmp(big, map + 0x1100, sizeof big));

    // at its own base: nothing moves
    memcpy(map, file, SIZE);
    CHECK(hk_image_open(&im, map, base));
    CHECK(hk_image_move(&im, 0x10001100) == 0x10001100);
    memcpy(o, add, 6);
    CHECK(hk_image_fix_patch(&im, 0x1100, 6, o, NULL) == 0 && !memcmp(o, add, 6));
}

static void pe32plus(void) {
    const uint64_t base = 0x180000000ull, real = 0x6d53510000ull;
    static uint8_t file[SIZE], map[SIZE];
    const uint16_t entries[] = {0xa000 | 0x008};   // a vtable's pointer at 0x1008
    make(file, 1, base, entries, 1, 0);
    put64(file + 0x1008, 0x180002340ull);
    memcpy(map, file, SIZE);
    load(map, base, real);
    struct hk_image im;
    CHECK(hk_image_open(&im, map, real));
    CHECK(im.preferred == base);
    CHECK(hk_image_move(&im, 0x180001008ull) == 0x6d53511008ull);
    CHECK(hk_image_move(&im, 0x5e150) == 0x5e150);   // (an offset, as the 64-bit mods' module patches give)
    uint8_t o[8], r[8];
    memcpy(o, file + 0x1008, 8);
    put64(r, 0x180003000ull);
    CHECK(hk_image_fix_patch(&im, 0x1008, 8, o, r) == 1);
    CHECK(!memcmp(o, map + 0x1008, 8));
    uint64_t v = 0;
    for (int k = 8; k-- > 0;) v = v << 8 | r[k];
    CHECK(v == 0x6d53513000ull);
    // half of it
    memcpy(o, file + 0x100c, 4);
    CHECK(hk_image_fix_patch(&im, 0x100c, 4, o, NULL) == 1);
    CHECK(!memcmp(o, map + 0x100c, 4));
}

static void exe(void) {
    // an EXE at its own base, its relocations stripped (pop'n 15-18, the PC IIDX games)
    static uint8_t img[SIZE];
    make(img, 0, 0x400000, NULL, 0, 1);
    struct hk_image im;
    CHECK(hk_image_open(&im, img, 0x400000));
    CHECK(im.relocs == 0);
    CHECK(hk_image_move(&im, 0x401234) == 0x401234);
    uint8_t o[4] = {1, 2, 3, 4};
    CHECK(hk_image_fix_patch(&im, 0x1234, 4, o, o) == 0 && o[0] == 1 && o[3] == 4);
    // not an image: nothing moves
    CHECK(!hk_image_open(&im, (const uint8_t *)"not an image", 0x1000));
    CHECK(hk_image_move(&im, 0x401234) == 0x401234);
}

int main(void) {
    pe32();
    pe32plus();
    exe();
    printf("imagebase: %d of %d checks passed\n", checks - failed, checks);
    return failed ? 1 : 0;
}
