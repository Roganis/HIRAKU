// SPDX-License-Identifier: GPL-3.0-or-later
// The C33 cabinet's small display (runtime/pc/c33video.h) drawn from a made-up board: nothing of
// the game's. Two tiles of our own in the flash, a palette of our own, cells placed on the pages.
#include "c33video.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t flash[C33_FLASH_SIZE], vram[C33_PAGES * C33_PAGE_SIZE], palette[1024], attr[32];
static uint32_t out[C33_SCREEN_W * C33_SCREEN_H];
static int fails;

static void check(int ok, const char *what) {
    if (!ok) fprintf(stderr, "c33video: %s\n", what), fails++;
}
static void cell(int page, int cx, int cy, unsigned tile, unsigned at) {
    uint8_t *c = vram + page * C33_PAGE_SIZE + (cy * 64 + cx) * 3;
    c[0] = (uint8_t)tile, c[1] = (uint8_t)(tile >> 8), c[2] = (uint8_t)at;
}
static void colour(int i, uint8_t r, uint8_t g, uint8_t b) {   // (blue, green, red, 0 in the program's order)
    palette[i * 4] = b, palette[i * 4 + 1] = g, palette[i * 4 + 2] = r;
}
static uint32_t at(int x, int y) { return out[y * C33_SCREEN_W + x]; }

int main(void) {
    // tile 1: pen 1 in its left column, pen 2 elsewhere (the left pixel in each byte's low half);
    // tile 0x123 (past 256: the number's high byte counts): pen 3 in its top row, 0 elsewhere
    for (int y = 0; y < 8; y++)
        for (int b = 0; b < 4; b++) flash[32 + y * 4 + b] = b == 0 ? 0x21 : 0x22;
    for (int b = 0; b < 4; b++) flash[0x123 * 32 + b] = 0x33;
    colour(0, 0, 0, 0), colour(1, 255, 0, 0), colour(2, 0, 255, 0), colour(3, 0, 0, 255);
    colour(16 + 0, 9, 9, 9), colour(16 + 1, 255, 255, 0), colour(16 + 2, 0, 255, 255);
    colour(5 * 16 + 3, 200, 100, 50);
    struct c33_video v = {flash, vram, palette, attr, 0};

    // layer 0 (page 0) shown, the others hidden as the program does it (0x0f)
    memset(attr, 0x0f, 5), attr[0] = 0;
    cell(0, 0, 0, 1, 0);       // palette row 0
    cell(0, 1, 0, 1, 0x10);    // row 1 (the attribute's high half)
    cell(0, 39, 29, 0x123, 0x50); // the screen's last cell, row 5
    cell(0, 40, 0, 1, 0);      // past the screen's 40 columns: not shown
    cell(0, 3, 0, 1, 0x13);    // row 1; the low bits 0 and 1 (never seen set) change nothing
    cell(0, 5, 0, 1, 0x08);    // turned left to right: pen 1 in the right column
    cell(0, 6, 0, 0x123, 0x04); // upside down: pen 3 in the bottom row
    cell(0, 7, 0, 1, 0x1c);    // both, row 1
    c33_video_draw(&v, out, C33_SCREEN_W);
    check(at(0, 0) == 0xff0000 && at(1, 0) == 0x00ff00 && at(7, 7) == 0x00ff00, "tile 1 in palette row 0");
    check(at(8, 0) == 0xffff00 && at(9, 3) == 0x00ffff, "tile 1 in palette row 1");
    check(at(312, 232) == 0xc86432 && at(319, 232) == 0xc86432 && at(312, 233) == 0, "tile 0x123, row 5, at the screen's end");
    check(at(16, 0) == 0, "an empty cell: tile 0, pen 0");
    check(at(24, 0) == 0xffff00 && at(25, 0) == 0x00ffff, "the attribute's low bits 0 and 1 change nothing");
    check(at(47, 0) == 0xff0000 && at(40, 0) == 0x00ff00 && at(47, 7) == 0xff0000, "bit 3: the tile turned left to right");
    check(at(48, 7) == 0x0000ff && at(55, 7) == 0x0000ff && at(48, 0) == 0, "bit 2: the tile upside down");
    check(at(63, 7) == 0xffff00 && at(56, 0) == 0x00ffff, "bits 3 and 2 together, with a row");

    // the bank: pages 5-9 on the screen
    cell(5, 2, 2, 1, 0x10);
    v.control = C33_BANK;
    c33_video_draw(&v, out, C33_SCREEN_W);
    check(at(0, 0) == 0 && at(16, 16) == 0xffff00, "the bank turned: page 5 shown, page 0 not");

    // two layers shown: layer 0 (page 5, tile 0x123) over layer 1 (page 6, tile 1), its pen 0
    // letting layer 1 through
    attr[1] = 0;
    cell(5, 2, 2, 0x123, 0);
    cell(6, 2, 2, 1, 0x10);
    c33_video_draw(&v, out, C33_SCREEN_W);
    check(at(16, 16) == 0x0000ff && at(16, 17) == 0xffff00, "layer 0 over layer 1, its pen 0 clear");
    attr[0] = 0x0f;
    c33_video_draw(&v, out, C33_SCREEN_W);
    check(at(16, 16) == 0xffff00 && at(17, 16) == 0x00ffff, "layer 0 hidden: layer 1 alone");
    attr[1] = 0x0f, attr[0] = 0;
    c33_video_draw(&v, out, C33_SCREEN_W);
    check(at(16, 16) == 0x0000ff && at(16, 17) == 0, "layer 1 hidden: layer 0 alone, its pen 0 drawn");

    // blank
    v.control = C33_BANK | C33_BLANK;
    out[0] = 1;
    c33_video_draw(&v, out, C33_SCREEN_W);
    check(at(16, 16) == 0 && at(0, 0) == 0, "the screen blank");

    // a pitch wider than the screen: the rest of each row left alone
    static uint32_t wide[C33_SCREEN_H * 400];
    for (size_t i = 0; i < sizeof wide / 4; i++) wide[i] = 7;
    v.control = 0, attr[0] = 0, attr[1] = 0x0f;
    c33_video_draw(&v, wide, 400);
    check(wide[0] == 0xff0000 && wide[320] == 7 && wide[400 + 8] == 0xffff00, "a wider pitch");

    // the touch panel: 2 pixels a unit, the screen at (10, 5) units, two keys of our own
    static uint32_t panel[400 * 700];
    static uint32_t scr[C33_SCREEN_W * C33_SCREEN_H];
    scr[0] = 0x123456, scr[C33_SCREEN_W * C33_SCREEN_H - 1] = 0x654321;
    const struct c33_key_place keys[2] = {{'A', 10, 200, 20, 20}, {']', 40, 200, 60, 20}};
    c33_panel_draw(panel, 400, 400, 700, 2, 1, scr, 10, 5, keys, 2, 1, 50, 210);
    check(panel[10 * 400 + 20] == 0x123456 && panel[(10 + 239) * 400 + 20 + 319] == 0x654321, "the screen at 1:1 on the panel");
    check(panel[(400 + 6) * 400 + 20 + 6] == 0xe8e8e0, "a key's face");
    check(panel[(400 + 6) * 400 + 80 + 6] == 0xffd040, "the touched key lit");
    check(panel[420 * 400 + 100] == 0xff2020, "the touch's dot");
    int ink = 0;
    for (int y = 402; y < 438; y++)
        for (int x = 22; x < 58; x++) ink += panel[y * 400 + x] == 0x101010;
    check(ink > 8 + 4 * 36, "the key's letter drawn (and its edge)");

    if (!fails) printf("c33video: all good\n");
    return fails ? 1 : 0;
}
