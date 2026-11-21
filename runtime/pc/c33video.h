// SPDX-License-Identifier: GPL-3.0-or-later
// The C33 cabinet's video board (Konami's ee'MALL 1st Avenue station, runtime/pc/ezusb_c33.c): its
// picture from what the program wrote to it, as a plain function of the board's state. Plain C, no
// Windows: the DLL draws with it, and tests/pc/test_c33video.c checks it on made-up boards.
//
// The board, as the program's calls (UsbVideo.dll) and its pictures (video.img) show it:
// - Its flash holds 8x8 tiles of 4 bits a pixel, 32 bytes each (four bytes a row, the left pixel in
//   each byte's low half), numbered from the flash's start.
// - Its memory holds 11 pages of 64 x 32 cells, 3 bytes each: the tile's number (16 bits, low byte
//   first) and an attribute byte: its high four bits choose one of the palette's 16 rows of 16
//   colours (C33_CELL_ROW; the title: 0x10-0xb0, clean with its palette only that way), bit 3 turns
//   the tile left to right and bit 2 upside down (C33_CELL_FLIP_X/_Y: the title's few cells with
//   them, its clouds' and buildings' edges, and the test screen's long-vowel mark, a dash drawn
//   with a turned tile, come out whole only that way; bits 0 and 1 were never seen set).
// - The screen shows 40 x 30 cells (320 x 240 pixels) from a page's top-left corner. Of the 11 pages,
//   five are on the screen at a time as layers, 0-4 or, with the bank bit (usbVramBank), 5-9; the
//   program draws into the other five and then turns the bank. Layer n is shown unless the
//   attribute register's byte n (usbAttrSet) is 0x0f; the program shows one layer at a time, but
//   draws its title before it first sets that register (all 0: every layer shown). Layer 0 goes over
//   the others, its colour 0 letting the next show through (the title, on page 0 over what page 1
//   still holds, comes out clean only that way round).
// - The palette is 256 entries of 4 bytes as the program hands them over (blue, green, red,
//   unused, as in its .pal files); the board's display-off bit blanks the screen.
// - The scale, scroll and other registers: the program sets the scale to 0x80 (1:1) for every layer
//   and never scrolls; they're kept, not drawn.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define C33_SCREEN_W 320
#define C33_SCREEN_H 240
#define C33_FLASH_SIZE 0x90000u         // 36 blocks of 16 KiB
#define C33_PAGE_SIZE 0x1800u           // 64 x 32 cells, 3 bytes each
#define C33_PAGES 11
#define C33_LAYERS 5
#define C33_CELL_ROW(attr) (((attr) >> 4) & 15)
#define C33_CELL_FLIP_X 0x08u
#define C33_CELL_FLIP_Y 0x04u

// The board's control byte (usbVramBank, usbPaletteBank, usbDisplayOnOff)
#define C33_BANK 1u        // the layers on screen are pages 5-9 (else 0-4)
#define C33_PALBANK 2u     // (kept; the program never sets it)
#define C33_BLANK 4u       // the screen blank

struct c33_video {
    const uint8_t *flash;     // C33_FLASH_SIZE bytes
    const uint8_t *vram;      // C33_PAGES * C33_PAGE_SIZE bytes
    const uint8_t *palette;   // 256 x 4 bytes
    const uint8_t *attr;      // the attribute register's 32 bytes
    unsigned control;         // C33_BANK, C33_BLANK...
};

// The screen as 0x00RRGGBB, C33_SCREEN_W x C33_SCREEN_H, rows `pitch` pixels apart
void c33_video_draw(const struct c33_video *v, uint32_t *out, int pitch);

// The touch panel's keys, as the program has them (TouchPanel.dll's table, read at run time): the
// key's character and its place in the panel's space (228 x 300 units)
struct c33_key_place {
    char c;
    int x, y, w, h;
};
// The touch panel drawn upright as the player sees it: `w` x `h` pixels at `out` (rows `pitch`
// apart), `px` pixels to `units` units; the small display's picture (`screen`, C33_SCREEN_W x
// C33_SCREEN_H) at (screen_x, screen_y) units, at 1:1; each key a box with its character (DEL for
// '[', ENTER for ']', arrows for '<' and '>'), lit while `touched` is (its index, -1 none); a dot where
// the panel is touched (tx, ty units; tx < 0: nowhere)
void c33_panel_draw(uint32_t *out, int pitch, int w, int h, int px, int units, const uint32_t *screen, int screen_x,
                    int screen_y, const struct c33_key_place *keys, int nkeys, int touched, int tx, int ty);

#ifdef __cplusplus
}
#endif
