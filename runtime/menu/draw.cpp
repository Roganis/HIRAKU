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
#include <map>
#include <vector>

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

// A game's picture as it is drawn: its file scaled to the list's height, or to its place's width if
// it is wider than that (Keyboardmania's two screens). The last few looked at are kept; a file that
// can't be read is no picture.
struct Picture {
    int w = 0, h = 0;
    std::vector<uint32_t> px;
};
std::map<std::string, Picture> g_pictures;
const Picture *picture(const std::string &path, int box_h, int max_w) {
    if (path.empty()) return nullptr;
    const std::string key = std::to_string(box_h) + "x" + std::to_string(max_w) + " " + path;
    if (const auto it = g_pictures.find(key); it != g_pictures.end()) return it->second.w ? &it->second : nullptr;
    if (g_pictures.size() >= 12) g_pictures.clear();
    Picture &p = g_pictures[key];
    SDL_Surface *file = SDL_LoadBMP(path.c_str());
    SDL_Surface *s = file ? SDL_ConvertSurface(file, SDL_PIXELFORMAT_ARGB8888) : nullptr;
    if (s && s->w > 1 && s->h > 1 && s->w <= 4096 && s->h <= 4096) {
        const int full_w = std::max(2, (int)std::lround((double)s->w * box_h / s->h));
        p.w = std::min(full_w, max_w), p.h = full_w > max_w ? std::max(2, (int)std::lround((double)s->h * max_w / s->w)) : box_h;
        p.px.resize((size_t)p.w * p.h);
        const auto at = [&](int x, int y) { return ((const uint32_t *)((const uint8_t *)s->pixels + (size_t)y * s->pitch))[x]; };
        for (int y = 0; y < p.h; y++) {   // between the file's pixels, by 256ths
            const int fy = std::min((int)((int64_t)y * (s->h - 1) * 256 / std::max(1, p.h - 1)), (s->h - 1) * 256 - 1);
            const int y0 = fy >> 8, wy = fy & 255;
            for (int x = 0; x < p.w; x++) {
                const int fx = std::min((int)((int64_t)x * (s->w - 1) * 256 / std::max(1, p.w - 1)), (s->w - 1) * 256 - 1);
                const int x0 = fx >> 8, wx = fx & 255;
                const uint32_t a = at(x0, y0), b = at(x0 + 1, y0), c = at(x0, y0 + 1), d = at(x0 + 1, y0 + 1);
                uint32_t out = 0;
                for (int sh = 0; sh < 24; sh += 8) {
                    const int top = (int)(a >> sh & 255) * (256 - wx) + (int)(b >> sh & 255) * wx;
                    const int bottom = (int)(c >> sh & 255) * (256 - wx) + (int)(d >> sh & 255) * wx;
                    out |= (uint32_t)((top * (256 - wy) + bottom * wy) >> 16) << sh;
                }
                p.px[(size_t)y * p.w + x] = out;
            }
        }
    }
    if (s) SDL_DestroySurface(s);
    if (file) SDL_DestroySurface(file);
    return p.w ? &p : nullptr;
}

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

    // the chosen game's picture behind the list, at its right: it fades away towards the titles'
    // side and comes up after a move
    const int bar_x = mx - 8, bar_end = w - (mx - 8);
    const Picture *pic = picture(sel->picture, rows * row_h, (bar_end - bar_x) * 6 / 10);
    int solid_to = bar_end, clear_from = bar_end;   // the bar's amber: whole up to the first, gone from the second
    if (pic) {
        const int left = bar_end - pic->w, ramp = std::max(2, pic->w * 45 / 100);
        const uint64_t since = frame - s.moved_at();
        const int top = since < 8 ? 175 * ((int)since + 1) / 9 : 175;
        const int up = list_y + (rows * row_h - pic->h) / 2;
        for (int y = 0; y < pic->h; y++)
            for (int x = 0; x < pic->w; x++) c.put(left + x, up + y, pic->px[(size_t)y * pic->w + x] & 0xffffff, top * std::min(x, ramp) / ramp);
        solid_to = left + ramp / 2, clear_from = left + ramp;
    }

    // the list: the chosen row on the bar, the others dimmer the further they are. Over a picture
    // the bar thins out to two lines, so that the middle of a title screen (its name) shows whole
    const int n = (int)shelf.games.size();
    const bool flash = started && started_for / 3 % 2 == 0;
    const uint32_t bar = flash ? 0xffffff : kAmber;
    c.fill(bar_x, bar_y, solid_to - bar_x, row_h, bar);
    for (int x = solid_to; x < clear_from; x++) c.fill(x, bar_y, 1, row_h, bar, 255 * (clear_from - x) / (clear_from - solid_to));
    c.fill(solid_to, bar_y, bar_end - solid_to, 2, bar, 170);
    c.fill(solid_to, bar_y + row_h - 2, bar_end - solid_to, 2, bar, 170);
    const int bar_text_to = (solid_to + clear_from) / 2;   // dark on the amber up to here, light after
    c.clip(0, list_y, w, list_y + rows * row_h);
    for (int i = 0; i < n; i++) {
        const double off = i - s.shown_row();
        const int y = bar_y + (int)std::lround(off * row_h) + 4;
        if (y + 16 <= list_y || y >= list_y + rows * row_h) continue;
        const Game &g = shelf.games[(size_t)i];
        const bool on_bar = i == s.row();
        const int a = on_bar ? 255 : std::max(60, 235 - (int)(std::fabs(off) * 34));
        const int detail_w = width(g.detail, 1), title_x = mx + 8;
        const bool detail = !pic || on_bar;   // the year and the system: every row's, or over a picture the chosen game's alone
        const int room = w - 2 * (mx + 8) - (detail && detail_w ? detail_w + 16 : 0);
        const int over = width(g.title, 2) - room;
        Canvas row = c;
        row.clip(title_x, std::max(c.y0, y - 4), title_x + room, std::min(c.y1, y + 20));
        if (on_bar) {
            const int tx = title_x - roll(over, frame - s.moved_at());
            Canvas on = row, past = row;
            on.x1 = std::min(row.x1, bar_text_to), past.x0 = std::max(row.x0, bar_text_to);
            on.text(tx, y, g.title, 2, kDark);
            past.text(tx + 2, y + 2, g.title, 2, kDark, 190);
            past.text(tx, y, g.title, 2, kText);
        } else if (over > 0) {   // cut short, with dots
            const size_t fit = (size_t)std::max(0, room / 16 - 2);
            if (pic) row.text(title_x + 2, y + 2, g.title.substr(0, fit) + "..", 2, kDark, a * 3 / 4);   // (a shadow, over a picture)
            row.text(title_x, y, g.title.substr(0, fit) + "..", 2, kText, a);
        } else {
            if (pic) row.text(title_x + 2, y + 2, g.title, 2, kDark, a * 3 / 4);
            row.text(title_x, y, g.title, 2, kText, a);
        }
        if (pic && on_bar) c.text(w - mx - 8 - detail_w + 1, y + 5, g.detail, 1, kDark, 220);
        if (detail) c.text(w - mx - 8 - detail_w, y + 4, g.detail, 1, pic ? kText : on_bar ? kDark : kDim, a);
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
