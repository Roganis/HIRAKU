// SPDX-License-Identifier: GPL-3.0-or-later
// The game menu's picture: the shelf across the top, its games down the middle with the chosen one
// on a bar that stays put (the list moves under it), and what this cabinet's controls do at the
// bottom. Drawn by hand in SDL's 8x8 font, two and three times its size: plain, and readable on a
// cabinet's tube from where the player stands. Everything moves by the frame count alone, so a
// scripted run's pictures are the same every time.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "menu.h"

namespace hk::menu {

namespace {

constexpr uint32_t kAmber = 0xffb830, kText = 0xe8ecf4, kDim = 0x8088a4, kDark = 0x10121c;

// SDL's 8x8 font, a bit a pixel: read once from what its renderer draws (as host/statsview.cpp's)
uint8_t g_font[96][8];
bool g_font_read = false;
void read_font() {
    g_font_read = true;
    SDL_Surface *s = SDL_CreateSurface(96 * 8, 8, SDL_PIXELFORMAT_ARGB8888);
    SDL_Renderer *r = s ? SDL_CreateSoftwareRenderer(s) : nullptr;
    if (r) {
        SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
        SDL_RenderClear(r);
        SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
        for (int c = 33; c < 127; c++) {
            const char t[2] = {(char)c, 0};
            SDL_RenderDebugText(r, (float)((c - 32) * 8), 0, t);
        }
        SDL_RenderPresent(r);
        for (int c = 0; c < 96; c++)
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++)
                    if (((const uint32_t *)((const uint8_t *)s->pixels + (size_t)y * s->pitch))[c * 8 + x] & 0xffffff) g_font[c][y] |= (uint8_t)(1 << x);
        SDL_DestroyRenderer(r);
    }
    if (s) SDL_DestroySurface(s);
}

// The frame being drawn, and the part of it drawing is kept to
struct Canvas {
    uint32_t *px;
    int w, h;
    int x0, y0, x1, y1;
    void clip(int cx0, int cy0, int cx1, int cy1) { x0 = std::max(cx0, 0), y0 = std::max(cy0, 0), x1 = std::min(cx1, w), y1 = std::min(cy1, h); }
    void unclip() { x0 = y0 = 0, x1 = w, y1 = h; }
    void put(int x, int y, uint32_t rgb, int a) {
        uint32_t &p = px[(size_t)y * w + x];
        if (a >= 255) {
            p = 0xff000000u | rgb;
            return;
        }
        const uint32_t rb = ((rgb & 0xff00ff) * (uint32_t)a + (p & 0xff00ff) * (uint32_t)(255 - a)) >> 8 & 0xff00ff;
        const uint32_t g = ((rgb & 0x00ff00) * (uint32_t)a + (p & 0x00ff00) * (uint32_t)(255 - a)) >> 8 & 0x00ff00;
        p = 0xff000000u | rb | g;
    }
    void fill(int x, int y, int fw, int fh, uint32_t rgb, int a = 255) {
        const int xa = std::max(x, x0), ya = std::max(y, y0), xb = std::min(x + fw, x1), yb = std::min(y + fh, y1);
        for (int yy = ya; yy < yb; yy++)
            for (int xx = xa; xx < xb; xx++) put(xx, yy, rgb, a);
    }
    // text at k times the font's size; its width is 8 k a character
    void text(int x, int y, const std::string &t, int k, uint32_t rgb, int a = 255) {
        for (size_t i = 0; i < t.size(); i++, x += 8 * k) {
            const unsigned c = (unsigned char)t[i];
            if (c <= 32 || c >= 127 || x + 8 * k <= x0 || x >= x1) continue;
            for (int gy = 0; gy < 8; gy++)
                for (int gx = 0; gx < 8; gx++)
                    if (g_font[c - 32][gy] >> gx & 1) fill(x + gx * k, y + gy * k, k, k, rgb, a);
        }
    }
};

int width(const std::string &t, int k) { return (int)t.size() * 8 * k; }

// a title too long for its place rolls: a wait, across, a wait, back to the start
int roll(int over, uint64_t since) {
    if (over <= 0) return 0;
    const uint64_t wait = 60, across = (uint64_t)over / 2 + 1, cycle = wait + across + wait;
    const uint64_t t = since % cycle;
    return t < wait ? 0 : t < wait + across ? (int)(t - wait) * 2 : over;
}

}  // namespace

void draw(const State &s, const std::string &family, uint64_t frame, uint32_t *px, int w, int h) {
    if (!g_font_read) read_font();
    Canvas c{px, w, h, 0, 0, w, h};
    // the ground: night blue, a little lighter towards the bottom
    for (int y = 0; y < h; y++) {
        const uint32_t t = (uint32_t)y * 255 / (uint32_t)std::max(1, h - 1);
        const uint32_t rgb = (10 + t * 10 / 255) << 16 | (13 + t * 13 / 255) << 8 | (28 + t * 28 / 255);
        std::fill(px + (size_t)y * w, px + (size_t)(y + 1) * w, 0xff000000u | rgb);
    }
    const int mx = 24;   // the side margins (a tube's overscan)
    c.text(mx, 16, "HIRAKU", 3, kAmber);
    c.text(w - mx - width("GAME MENU", 2), 20, "GAME MENU", 2, kText);
    c.fill(mx, 48, w - 2 * mx, 2, kAmber);

    const List &list = s.list();
    const int list_y = 112, row_h = 24, rows = 11, bar_y = list_y + rows / 2 * row_h;
    const Game *sel = s.selected();
    if (!sel) {
        const char *none = "No game is ready to play";
        c.text((w - width(none, 2)) / 2, bar_y + 4, none, 2, kText);
        const char *how = "Add your games in HIRAKU's window first";
        c.text((w - width(how, 1)) / 2, bar_y + 32, how, 1, kDim);
        return;
    }
    const Shelf &shelf = list.shelves[(size_t)s.shelf()];
    const bool started = s.chosen() != nullptr;
    const uint64_t started_for = started ? frame - s.chosen_at() : 0;

    // the shelf, between its arrows: it slides in from the side it was called from
    {
        const int k = width(shelf.name, 3) <= w - 2 * mx - 96 ? 3 : 2;
        const uint64_t t = frame - s.shelf_at();
        const int slide = s.shelf_dir() && t < 8 ? s.shelf_dir() * (int)(8 - t) * 10 : 0;
        const int a = s.shelf_dir() && t < 8 ? 95 + (int)t * 20 : 255;
        const int y = 64 + (3 - k) * 4;
        c.clip(mx + 32, 0, w - mx - 32, h);
        c.text((w - width(shelf.name, k)) / 2 + slide, y, shelf.name, k, kText, a);
        c.unclip();
        if (list.shelves.size() > 1) {
            const bool blink = frame / 30 % 2 == 0;
            c.text(mx, 64, "<", 3, blink ? kAmber : kDim);
            c.text(w - mx - 24, 64, ">", 3, blink ? kAmber : kDim);
            // a mark for each shelf, this one's lit
            const int n = (int)list.shelves.size(), step = 14, x0 = (w - (n * step - 6)) / 2;
            for (int i = 0; i < n; i++) c.fill(x0 + i * step, 96, 8, 4, i == s.shelf() ? kAmber : 0x3a4060);
        }
    }

    // the list: the chosen row on the bar, the others dimmer the further they are
    const int n = (int)shelf.games.size();
    const bool flash = started && started_for / 3 % 2 == 0;
    c.fill(mx - 8, bar_y, w - 2 * (mx - 8), row_h, flash ? 0xffffff : kAmber);
    c.clip(0, list_y, w, list_y + rows * row_h);
    for (int i = 0; i < n; i++) {
        const double off = i - s.shown_row();
        const int y = bar_y + (int)std::lround(off * row_h) + 4;
        if (y + 16 <= list_y || y >= list_y + rows * row_h) continue;
        const Game &g = shelf.games[(size_t)i];
        const bool on_bar = i == s.row();
        const int a = on_bar ? 255 : std::max(60, 235 - (int)(std::fabs(off) * 34));
        const int detail_w = width(g.detail, 1), title_x = mx + 8;
        const int room = w - 2 * (mx + 8) - (detail_w ? detail_w + 16 : 0);
        const int over = width(g.title, 2) - room;
        Canvas row = c;
        row.clip(title_x, std::max(c.y0, y - 4), title_x + room, std::min(c.y1, y + 20));
        if (on_bar) row.text(title_x - roll(over, frame - s.moved_at()), y, g.title, 2, kDark);
        else if (over > 0) {   // cut short, with dots
            const size_t fit = (size_t)std::max(0, room / 16 - 2);
            row.text(title_x, y, g.title.substr(0, fit) + "..", 2, kText, a);
        } else row.text(title_x, y, g.title, 2, kText, a);
        c.text(w - mx - 8 - detail_w, y + 4, g.detail, 1, on_bar ? kDark : kDim, a);
    }
    c.unclip();
    {   // where the bar is in the shelf
        char at[32];
        snprintf(at, sizeof at, "%d / %d", s.row() + 1, n);
        c.text(w - mx - width(at, 1), 100, at, 1, kDim);
    }

    // what this cabinet's controls do; once a game is started, that it is
    const int legend_y = list_y + rows * row_h + 12;
    c.fill(mx, legend_y - 6, w - 2 * mx, 1, 0x3a4060);
    if (started) {
        const std::string line = "Starting " + sel->title;
        const int k = width(line, 2) <= w - 2 * mx ? 2 : 1;
        c.text((w - width(line, k)) / 2, legend_y + 28, line, k, kText);
        return;
    }
    const std::vector<Hint> hints = legend(family);
    size_t longest = 0, does = 0;
    for (const Hint &hint : hints) longest = std::max(longest, hint.control.size()), does = std::max(does, hint.does.size());
    const int x = std::max(mx, (w - (int)(longest + 2 + does) * 16) / 2);
    for (size_t i = 0; i < hints.size(); i++) {
        const int y = legend_y + 4 + (int)i * 24;
        c.text(x, y, hints[i].control, 2, kAmber);
        c.text(x + (int)(longest + 2) * 16, y, hints[i].does, 2, kText);
    }
}

}  // namespace hk::menu
