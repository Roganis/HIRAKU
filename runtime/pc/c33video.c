// SPDX-License-Identifier: GPL-3.0-or-later
// The C33 cabinet's video board's picture (runtime/pc/c33video.h)
#include "c33video.h"

#include <string.h>

static uint32_t colour(const uint8_t *palette, unsigned i) {
    const uint8_t *p = palette + 4 * (i & 255);
    return (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
}

void c33_video_draw(const struct c33_video *v, uint32_t *out, int pitch) {
    for (int y = 0; y < C33_SCREEN_H; y++) memset(out + (size_t)y * pitch, 0, C33_SCREEN_W * 4);
    if (v->control & C33_BLANK) return;
    const int base = v->control & C33_BANK ? C33_LAYERS : 0;
    int drawn = 0;
    for (int layer = C33_LAYERS - 1; layer >= 0; layer--) {   // (layer 0 over the others)
        if (v->attr[layer] == 0x0f) continue;
        const uint8_t *page = v->vram + (size_t)(base + layer) * C33_PAGE_SIZE;
        for (int cy = 0; cy < C33_SCREEN_H / 8; cy++)
            for (int cx = 0; cx < C33_SCREEN_W / 8; cx++) {
                const uint8_t *cell = page + (cy * 64 + cx) * 3;
                const uint32_t tile = cell[0] | (uint32_t)cell[1] << 8;
                const unsigned row = C33_CELL_ROW(cell[2]) * 16;
                const int fx = cell[2] & C33_CELL_FLIP_X ? 7 : 0, fy = cell[2] & C33_CELL_FLIP_Y ? 7 : 0;
                const uint8_t *t = (size_t)tile * 32 + 32 <= C33_FLASH_SIZE ? v->flash + (size_t)tile * 32 : NULL;
                for (int y = 0; y < 8; y++) {
                    uint32_t *o = out + (size_t)(cy * 8 + y) * pitch + cx * 8;
                    const int ty = y ^ fy;   // (7 - y when turned)
                    for (int x = 0; x < 8; x++) {
                        const int tx = x ^ fx;
                        const unsigned pen = t ? (t[ty * 4 + tx / 2] >> (tx & 1 ? 4 : 0)) & 15 : 0;
                        if (pen || !drawn) o[x] = colour(v->palette, row + pen);
                    }
                }
            }
        drawn++;
    }
}

// HIRAKU's own small letters for the keys' labels: 5 x 7, a row's bit 4 its left pixel
static const struct {
    char c;
    uint8_t rows[7];
} k_glyphs[] = {
    {'0', {14, 17, 19, 21, 25, 17, 14}}, {'1', {4, 12, 4, 4, 4, 4, 14}},     {'2', {14, 17, 1, 2, 4, 8, 31}},
    {'3', {31, 2, 4, 2, 1, 17, 14}},     {'4', {2, 6, 10, 18, 31, 2, 2}},    {'5', {31, 16, 30, 1, 1, 17, 14}},
    {'6', {6, 8, 16, 30, 17, 17, 14}},   {'7', {31, 1, 2, 4, 8, 8, 8}},      {'8', {14, 17, 17, 14, 17, 17, 14}},
    {'9', {14, 17, 17, 15, 1, 2, 12}},   {'A', {14, 17, 17, 31, 17, 17, 17}}, {'B', {30, 17, 17, 30, 17, 17, 30}},
    {'C', {14, 17, 16, 16, 16, 17, 14}}, {'D', {28, 18, 17, 17, 17, 18, 28}}, {'E', {31, 16, 16, 30, 16, 16, 31}},
    {'F', {31, 16, 16, 30, 16, 16, 16}}, {'G', {14, 17, 16, 23, 17, 17, 15}}, {'H', {17, 17, 17, 31, 17, 17, 17}},
    {'I', {14, 4, 4, 4, 4, 4, 14}},      {'J', {7, 2, 2, 2, 2, 18, 12}},     {'K', {17, 18, 20, 24, 20, 18, 17}},
    {'L', {16, 16, 16, 16, 16, 16, 31}}, {'M', {17, 27, 21, 21, 17, 17, 17}}, {'N', {17, 17, 25, 21, 19, 17, 17}},
    {'O', {14, 17, 17, 17, 17, 17, 14}}, {'P', {30, 17, 17, 30, 16, 16, 16}}, {'Q', {14, 17, 17, 17, 21, 18, 13}},
    {'R', {30, 17, 17, 30, 20, 18, 17}}, {'S', {15, 16, 16, 14, 1, 1, 30}},   {'T', {31, 4, 4, 4, 4, 4, 4}},
    {'U', {17, 17, 17, 17, 17, 17, 14}}, {'V', {17, 17, 17, 17, 17, 10, 4}},  {'W', {17, 17, 17, 21, 21, 21, 10}},
    {'X', {17, 17, 10, 4, 10, 17, 17}},  {'Y', {17, 17, 10, 4, 4, 4, 4}},     {'Z', {31, 1, 2, 4, 8, 16, 31}},
    {'.', {0, 0, 0, 0, 0, 12, 12}},      {'-', {0, 0, 0, 31, 0, 0, 0}},      {'_', {0, 0, 0, 0, 0, 0, 31}},
    {'<', {2, 4, 8, 16, 8, 4, 2}},       {'>', {8, 4, 2, 1, 2, 4, 8}},
};

static const uint8_t *glyph(char c) {
    for (size_t i = 0; i < sizeof k_glyphs / sizeof *k_glyphs; i++)
        if (k_glyphs[i].c == c) return k_glyphs[i].rows;
    return NULL;
}

static void fill(uint32_t *out, int pitch, int w, int h, int x0, int y0, int x1, int y1, uint32_t c) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) out[(size_t)y * pitch + x] = c;
}

// text centred on (cx, cy), each of the glyphs' pixels k x k
static void text(uint32_t *out, int pitch, int w, int h, int cx, int cy, const char *s, int k, uint32_t c) {
    const int n = (int)strlen(s), tw = n * 6 * k - k;
    int x = cx - tw / 2;
    const int y = cy - 7 * k / 2;
    for (; *s; s++, x += 6 * k) {
        const uint8_t *g = glyph(*s);
        for (int r = 0; g && r < 7; r++)
            for (int b = 0; b < 5; b++)
                if (g[r] >> (4 - b) & 1) fill(out, pitch, w, h, x + b * k, y + r * k, x + b * k + k, y + r * k + k, c);
    }
}

void c33_panel_draw(uint32_t *out, int pitch, int w, int h, int px, int units, const uint32_t *screen, int screen_x,
                    int screen_y, const struct c33_key_place *keys, int nkeys, int touched, int tx, int ty) {
#define P(u) ((u) * px / units)
    fill(out, pitch, w, h, 0, 0, w, h, 0x303438);   // the panel
    // the small display, 1:1, with a frame round it
    const int sx = P(screen_x), sy = P(screen_y);
    fill(out, pitch, w, h, sx - 4, sy - 4, sx + C33_SCREEN_W + 4, sy + C33_SCREEN_H + 4, 0x101010);
    for (int y = 0; y < C33_SCREEN_H; y++)
        for (int x = 0; x < C33_SCREEN_W; x++)
            if (sx + x >= 0 && sx + x < w && sy + y >= 0 && sy + y < h)
                out[(size_t)(sy + y) * pitch + sx + x] = screen[(size_t)y * C33_SCREEN_W + x];
    for (int i = 0; i < nkeys; i++) {
        const struct c33_key_place *k = &keys[i];
        if (k->w <= 0 || k->h <= 0) continue;
        const int x0 = P(k->x) + 2, y0 = P(k->y) + 2, x1 = P(k->x + k->w) - 2, y1 = P(k->y + k->h) - 2;
        const uint32_t face = i == touched ? 0xffd040 : 0xe8e8e0;
        fill(out, pitch, w, h, x0, y0, x1, y1, 0x101010);
        fill(out, pitch, w, h, x0 + 2, y0 + 2, x1 - 2, y1 - 2, face);
        char label[8] = {k->c, 0};
        if (k->c == '[') strcpy(label, "DEL");
        else if (k->c == ']') strcpy(label, "ENTER");
        const int kk = (x1 - x0) >= 6 * 2 * (int)strlen(label) + 4 && (y1 - y0) >= 7 * 2 + 4 ? 2 : 1;
        text(out, pitch, w, h, (x0 + x1) / 2, (y0 + y1) / 2, label, kk, 0x101010);
    }
    if (tx >= 0) {   // where it's touched
        const int x = P(tx), y = P(ty);
        fill(out, pitch, w, h, x - 4, y - 4, x + 5, y + 5, 0xff2020);
    }
#undef P
}
