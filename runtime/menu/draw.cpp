// SPDX-License-Identifier: GPL-3.0-or-later
// The game menu's picture: the shelf across the top, its games down the middle with the chosen one
// on a bar that stays put (the list moves under it), and what this cabinet's controls do at the
// bottom. Drawn by hand, in the launcher's fonts or SDL's 8x8 font two and three times its size:
// plain, and readable on a cabinet's tube from where the player stands. In amber on night blue, or
// with the launcher's look (--look): its colours, the shelf's name on a pill of its series' colour,
// the six colours under the top, a bar that fades across, a dot of each control's colour.
// Everything moves by the frame count alone, so a scripted run's pictures are the same every time.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

#include "font.h"
#include "lang/lang.h"
#include "logo.h"
#include "menu.h"

namespace hk::menu {

namespace {

constexpr uint32_t kDark = 0x10121c;   // text on the amber bar, and shadows

Look g_look;            // the colours (--look; the defaults: amber on night blue)
bool g_styled = false;  // ... and the launcher's look's shapes with them

// The launcher's fonts (--font), where it gave them: the text's sizes 1, 2 and 3 below are its
// small, row and heading sizes
Font g_real;

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
    // a rectangle with round corners (radius r), its edges smoothed
    void round_rect(int x, int y, int fw, int fh, int r, uint32_t rgb, int a = 255) {
        r = std::min({r, fw / 2, fh / 2});
        const int xa = std::max(x, x0), ya = std::max(y, y0), xb = std::min(x + fw, x1), yb = std::min(y + fh, y1);
        for (int yy = ya; yy < yb; yy++)
            for (int xx = xa; xx < xb; xx++) {
                const double px = xx + 0.5, py = yy + 0.5;
                const double cx = std::clamp(px, (double)x + r, (double)x + fw - r), cy = std::clamp(py, (double)y + r, (double)y + fh - r);
                const double d = std::hypot(px - cx, py - cy), cover = std::clamp(r - d + 0.5, 0.0, 1.0);
                if (cover > 0) put(xx, yy, rgb, (int)(a * cover));
            }
    }
    void disc(int cx, int cy, int r, uint32_t rgb, int a = 255) { round_rect(cx - r, cy - r, 2 * r, 2 * r, r, rgb, a); }
    // text at size k (1 small, 2 a row's, 3 a heading's), in a box 8 k high from y: the launcher's
    // fonts, centred in that box; or the 8x8 font k times its size, 8 k a character
    void text(int x, int y, const std::string &given, int k, uint32_t rgb, int a = 255) {
        if (g_real.loaded()) {
            g_real.draw(px, w, h, x0, y0, x1, y1, x, y + 4 * k - g_real.line(k - 1) / 2, k - 1, given, rgb, a);
            return;
        }
        const std::string t = ascii_only(given);
        for (size_t i = 0; i < t.size(); i++, x += 8 * k) {
            const unsigned c = (unsigned char)t[i];
            if (c <= 32 || c >= 127 || x + 8 * k <= x0 || x >= x1) continue;
            for (int gy = 0; gy < 8; gy++)
                for (int gx = 0; gx < 8; gx++)
                    if (g_font[c - 32][gy] >> gx & 1) fill(x + gx * k, y + gy * k, k, k, rgb, a);
        }
    }
};

int width(const std::string &t, int k) { return g_real.loaded() ? g_real.width(k - 1, t) : (int)ascii_only(t).size() * 8 * k; }

// a text cut short with dots, if it is wider than its room
std::string cut(const std::string &given, int k, int room) {
    if (width(given, k) <= room) return given;
    const std::string t = g_real.loaded() ? given : ascii_only(given);
    size_t fits = 0, over = utf8_points(t).size();   // the most characters that fit, by halves
    while (fits + 1 < over) {
        const size_t mid = (fits + over) / 2;
        (width(utf8_first(t, mid) + "..", k) <= room ? fits : over) = mid;
    }
    return utf8_first(t, fits) + "..";
}

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

std::string g_logo;   // --logo's file

}  // namespace

std::string ascii_only(const std::string &s) {
    std::string out;
    for (const char ch : s) {
        const unsigned char c = (unsigned char)ch;
        if (c >= 32 && c < 127) out += ch;
        else if (c >= 0xc0) out += '?';   // (a UTF-8 character's first byte: its others add nothing)
    }
    return out;
}

bool set_font(const std::string &path, std::string &error) {
    if (path.empty()) {
        g_real = Font();
        return true;
    }
    Font f;
    if (!f.load(path, error)) return false;
    g_real = std::move(f);
    return true;
}

void set_logo(const std::string &path) { g_logo = path; }

void set_look(const Look &l) {
    g_look = l;
    g_styled = true;
}

void own_look() {
    g_look = Look();
    g_styled = false;
}

namespace {

using ui::mix;

// the colour a control is called by (pop'n's "Blue buttons"), from the look's six; else the
// i'th of them (or the bar's)
uint32_t control_colour(const std::string &control, int i) {
    const std::vector<uint32_t> &six = g_look.stripe;
    const auto has = [&](const char *word) { return control.find(word) != std::string::npos; };
    const auto pick = [&](int k, uint32_t plain) { return six.size() == 6 ? six[(size_t)k] : plain; };
    if (has("Blue")) return pick(0, 0x2f7de1);
    if (has("Green")) return pick(1, 0x27ae60);
    if (has("Yellow")) return pick(2, 0xf1c40f);
    if (has("Red")) return pick(4, 0xda4453);
    if (has("White")) return 0xf4f4f4;
    if (has("Black")) return 0x2a2a30;
    return six.size() == 6 ? six[(size_t)(i * 2 % 6)] : g_look.bar;
}

// the menu under the logo
void draw_menu(const State &s, const std::string &family, uint64_t frame, uint32_t *px, int w, int h) {
    Canvas c{px, w, h, 0, 0, w, h};
    const Look &L = g_look;
    const bool day = light(L.ground);
    // the ground: night blue, a little lighter towards the bottom (the look's: its ground to its foot)
    for (int y = 0; y < h; y++) {
        const int t = y * 255 / std::max(1, h - 1);
        uint32_t rgb = 0;
        for (int sh = 0; sh < 24; sh += 8) {
            const int from = (int)(L.ground >> sh & 255), to = (int)(L.foot >> sh & 255);
            rgb |= (uint32_t)(from + t * (to - from) / 255) << sh;
        }
        std::fill(px + (size_t)y * w, px + (size_t)(y + 1) * w, 0xff000000u | rgb);
    }
    const int mx = 24;   // the side margins (a tube's overscan)
    // HIRAKU's name: its logo, once it has come up here (draw); without one, the launcher's picture
    // of it (white on black: how light a pixel is, is how much of it is drawn), else in letters
    if (logo::ready()) {
        // (drawn last, over the rest)
    } else if (const Picture *logo = picture(g_logo, 30, w / 2)) {
        if (day) c.fill(mx - 4, 5, logo->w + 8, logo->h + 8, 0);   // (white letters: on black, on a light ground)
        for (int y = 0; y < logo->h; y++)
            for (int x = 0; x < logo->w; x++) {
                const uint32_t p = logo->px[(size_t)y * logo->w + x];
                c.put(mx + x, 9 + y, day ? 0xffffff : L.ink, (int)((p >> 16 & 0xff) * 2 + (p >> 8 & 0xff) * 5 + (p & 0xff)) / 8);
            }
    } else c.text(mx, 16, "HIRAKU", 3, L.point);
    const char *title = lang::tr("menu", "GAME MENU");
    c.text(w - mx - width(title, 2), 20, title, 2, L.ink);
    if (g_styled && !L.stripe.empty()) {   // the six, side by side
        const int n = (int)L.stripe.size();
        for (int i = 0; i < n; i++) {
            const int a = mx + (w - 2 * mx) * i / n, b = mx + (w - 2 * mx) * (i + 1) / n;
            c.fill(a, 47, b - a, 3, L.stripe[(size_t)i]);
        }
    } else if (g_styled) c.fill(mx, 48, w - 2 * mx, 1, L.line);
    else c.fill(mx, 48, w - 2 * mx, 2, L.bar);

    const List &list = s.list();
    const int list_y = 112, row_h = 24, rows = 11, bar_y = list_y + rows / 2 * row_h;
    const Game *sel = s.selected();
    if (!sel) {
        const char *none = lang::tr("menu", "No game is ready to play");
        c.text((w - width(none, 2)) / 2, bar_y + 4, none, 2, L.ink);
        const char *how = lang::tr("menu", "Add your games in HIRAKU's window first");
        c.text((w - width(how, 1)) / 2, bar_y + 32, how, 1, L.dim);
        return;
    }
    const Shelf &shelf = list.shelves[(size_t)s.shelf()];
    const bool started = s.chosen() != nullptr;
    const uint64_t started_for = started ? frame - s.chosen_at() : 0;
    // the shelf's colour (its series'), and what is written on it
    const uint32_t shelf_c = shelf.has_colour ? shelf.colour : L.bar;
    const uint32_t on_shelf = ((shelf_c >> 16 & 255) * 2 + (shelf_c >> 8 & 255) * 5 + (shelf_c & 255)) / 8 > 175 ? 0x1c1c24 : 0xffffff;

    // the shelf, between its arrows: it slides in from the side it was called from
    {
        const int k = width(shelf.name, 3) <= w - 2 * mx - 96 ? 3 : 2;
        const uint64_t t = frame - s.shelf_at();
        const int slide = s.shelf_dir() && t < 8 ? s.shelf_dir() * (int)(8 - t) * 10 : 0;
        const int a = s.shelf_dir() && t < 8 ? 95 + (int)t * 20 : 255;
        const int y = 64 + (3 - k) * 4;
        c.clip(mx + 32, 0, w - mx - 32, h);
        if (g_styled) {   // on a pill of its colour
            const int tw = width(shelf.name, k), pw = tw + 10 * k + 10, ph = 8 * k + 10;
            c.round_rect((w - pw) / 2 + slide, y - 5, pw, ph, std::min(L.round, ph / 2), shelf_c, a);
            c.text((w - tw) / 2 + slide, y, shelf.name, k, on_shelf, a);
        } else c.text((w - width(shelf.name, k)) / 2 + slide, y, shelf.name, k, L.ink, a);
        c.unclip();
        if (list.shelves.size() > 1) {
            const bool blink = frame / 30 % 2 == 0;
            c.text(mx, 64, "<", 3, blink ? L.point : L.dim);
            c.text(w - mx - 24, 64, ">", 3, blink ? L.point : L.dim);
            // a mark for each shelf, this one's lit
            const int n = (int)list.shelves.size(), step = 14, x0 = (w - (n * step - 6)) / 2;
            for (int i = 0; i < n; i++) {
                const int my = g_real.loaded() ? 100 : 96;
                if (g_styled) c.round_rect(x0 + i * step, my, 8, 4, 2, i == s.shelf() ? shelf_c : L.line);
                else c.fill(x0 + i * step, my, 8, 4, i == s.shelf() ? L.bar : L.line);
            }
        }
    }

    // the chosen game's picture behind the list, at its right: it fades away towards the titles'
    // side and comes up after a move
    const int bar_x = mx - 8, bar_end = w - (mx - 8);
    const Picture *pic = picture(sel->picture, rows * row_h, (bar_end - bar_x) * 6 / 10);
    int solid_to = bar_end, clear_from = bar_end;   // the bar's amber: whole up to the first, gone from the second
    // with the launcher's look, a game without a picture has a stand-in: its shelf's colour,
    // coming up from the titles' side, a light circle in its corner
    const bool stand_in = !pic && g_styled && shelf.has_colour;
    if (stand_in) {
        const int pw = (bar_end - bar_x) * 48 / 100, left = bar_end - pw, ph = rows * row_h, ramp = pw / 2;
        const uint64_t since = frame - s.moved_at();
        const int top = since < 8 ? 205 * ((int)since + 1) / 9 : 205;
        for (int x = 0; x < pw; x++) c.fill(left + x, list_y, 1, ph, shelf_c, top * std::min(x, ramp) / ramp);
        Canvas in = c;
        in.clip(left, list_y, bar_end, list_y + ph);
        in.disc(left + pw * 87 / 100, list_y + ph / 5, ph * 42 / 100, 0xffffff, top / 6);
    }
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
    const uint32_t bar = flash ? 0xffffff : L.bar;
    int bar_text_to = (solid_to + clear_from) / 2;   // dark on the amber up to here, light after
    if (g_styled) {   // the look's: its colour fading across, a white edge at its start; white on it
        const int bw = bar_end - bar_x;
        for (int x = 0; x < bw; x++) c.fill(bar_x + x, bar_y, 1, row_h, bar, 255 - 225 * x / bw);
        c.round_rect(bar_x - 2, bar_y, 4, row_h, 2, 0xffffff);
        bar_text_to = bar_x;
    } else {
        c.fill(bar_x, bar_y, solid_to - bar_x, row_h, bar);
        for (int x = solid_to; x < clear_from; x++) c.fill(x, bar_y, 1, row_h, bar, 255 * (clear_from - x) / (clear_from - solid_to));
        c.fill(solid_to, bar_y, bar_end - solid_to, 2, bar, 170);
        c.fill(solid_to, bar_y + row_h - 2, bar_end - solid_to, 2, bar, 170);
    }
    const bool over_picture = pic || stand_in;   // (the rows' years and systems: the chosen game's alone)
    const bool shadowed = pic && !day;           // the titles: light with a shadow over a picture on a dark ground
    c.clip(0, list_y, w, list_y + rows * row_h);
    for (int i = 0; i < n; i++) {
        const double off = i - s.shown_row();
        const int y = bar_y + (int)std::lround(off * row_h) + 4;
        if (y + 16 <= list_y || y >= list_y + rows * row_h) continue;
        const Game &g = shelf.games[(size_t)i];
        const bool on_bar = i == s.row();
        const int a = on_bar ? 255 : std::max(60, 235 - (int)(std::fabs(off) * 34));
        const int detail_w = width(g.detail, 1), title_x = mx + 8;
        const bool detail = !over_picture || on_bar;   // the year and the system: every row's, or over a picture the chosen game's alone
        const int room = w - 2 * (mx + 8) - (detail && detail_w ? detail_w + 16 : 0);
        const int over = width(g.title, 2) - room;
        Canvas row = c;
        row.clip(title_x, std::max(c.y0, y - 4), title_x + room, std::min(c.y1, y + 20));
        if (on_bar) {
            const int tx = title_x - roll(over, frame - s.moved_at());
            Canvas on = row, past = row;
            on.x1 = std::min(row.x1, bar_text_to), past.x0 = std::max(row.x0, bar_text_to);
            on.text(tx, y, g.title, 2, kDark);
            past.text(tx + 2, y + 2, g.title, 2, kDark, g_styled ? 90 : 190);
            past.text(tx, y, g.title, 2, g_styled ? 0xffffff : L.ink);
        } else {   // cut short with dots, if it is too long
            const std::string shown = over > 0 ? cut(g.title, 2, room) : g.title;
            if (shadowed) row.text(title_x + 2, y + 2, shown, 2, kDark, a * 3 / 4);
            row.text(title_x, y, shown, 2, L.ink, a);
        }
        if (over_picture && on_bar) c.text(w - mx - 8 - detail_w + 1, y + 5, g.detail, 1, kDark, 220);
        if (detail)
            c.text(w - mx - 8 - detail_w, y + 4, g.detail, 1,
                   over_picture ? (day ? 0xffffff : L.ink) : on_bar ? (g_styled ? L.ink : kDark) : L.dim, a);
    }
    c.unclip();
    {   // where the bar is in the shelf
        char at[32];
        snprintf(at, sizeof at, "%d / %d", s.row() + 1, n);
        c.text(w - mx - width(at, 1), 100, at, 1, L.dim);
    }

    // what this cabinet's controls do; once a game is started, that it is
    const int legend_y = list_y + rows * row_h + 12;
    c.fill(mx, legend_y - 6, w - 2 * mx, 1, L.line);
    if (started) {
        // TR: %1 is the game's title, as the game writes it
        const std::string line = lang::arg(lang::tr("menu", "Starting %1"), {sel->title});
        const int k = width(line, 2) <= w - 2 * mx ? 2 : 1;
        c.text((w - width(line, k)) / 2, legend_y + 28, line, k, L.ink);
        return;
    }
    if (!s.notice().empty()) {   // said first: a box in the controls' place, until the first move
        const size_t bar_at = s.notice().find('|');
        const std::string first = cut(s.notice().substr(0, bar_at), 2, w - 2 * mx - 32);
        const std::string second = bar_at == std::string::npos ? "" : cut(s.notice().substr(bar_at + 1), 1, w - 2 * mx - 32);
        c.fill(mx, legend_y, w - 2 * mx, 72, L.point);
        c.fill(mx + 2, legend_y + 2, w - 2 * mx - 4, 68, g_styled ? L.foot : kDark);
        c.text((w - width(first, 2)) / 2, legend_y + (second.empty() ? 28 : 16), first, 2, L.ink);
        c.text((w - width(second, 1)) / 2, legend_y + 44, second, 1, day ? L.ink : L.point);
        return;
    }
    // in the player's language (nav.cpp's texts, context "menu"); the colour from the English name
    const std::vector<Hint> hints = legend(family);
    std::vector<Hint> said;
    for (const Hint &hint : hints) said.push_back({lang::tr("menu", hint.control.c_str()), lang::tr("menu", hint.does.c_str())});
    int longest = 0, does = 0;
    for (const Hint &hint : said) longest = std::max(longest, width(hint.control, 2)), does = std::max(does, width(hint.does, 2));
    const int dot = g_styled ? 22 : 0;   // the look's: a dot of the control's colour before it
    // a language that says it longer: the controls keep what they need (up to two thirds), what they
    // do is cut short with dots to the room left, not drawn off the screen's side
    longest = std::min(longest, (w - 2 * mx - dot) * 2 / 3);
    const int gap = width("mm", 2), x = std::max(mx + dot, (w - (dot + longest + gap + does)) / 2 + dot);
    const int room = w - mx - (x + longest + gap);
    for (size_t i = 0; i < said.size(); i++) {
        const int y = legend_y + 4 + (int)i * 24;
        const std::string control = cut(said[i].control, 2, longest), done = cut(said[i].does, 2, room);
        if (g_styled) {
            c.disc(x - 15, y + 8, 7, control_colour(hints[i].control, (int)i));
            c.text(x, y, control, 2, L.ink);
            c.text(x + longest + gap, y, done, 2, mix(L.ink, L.dim, 80));
        } else {
            c.text(x, y, control, 2, L.bar);
            c.text(x + longest + gap, y, done, 2, L.ink);
        }
    }
}

// A frame of the logo, scaled down to its place: each pixel the mean of the logo's it covers
// (bilinear would drop its thin lines when small), weighed by how opaque each is
void draw_logo(Canvas &c, const LogoAt &at) {
    const uint8_t *src = logo::frame((logo::Anim)at.anim, at.frame);
    const int lw = logo::width(), lh = logo::height();
    if (!src || at.w < 1 || at.h < 1) return;
    for (int y = 0; y < at.h; y++) {
        const int py = at.y + y;
        if (py < c.y0 || py >= c.y1) continue;
        const int sy0 = y * lh / at.h, sy1 = std::min(lh, std::max(sy0 + 1, (y + 1) * lh / at.h));
        for (int x = 0; x < at.w; x++) {
            const int px = at.x + x;
            if (px < c.x0 || px >= c.x1) continue;
            const int sx0 = x * lw / at.w, sx1 = std::min(lw, std::max(sx0 + 1, (x + 1) * lw / at.w));
            uint32_t light = 0, opaque = 0;
            for (int sy = sy0; sy < sy1; sy++)
                for (int sx = sx0; sx < sx1; sx++) {
                    const uint8_t *p = src + ((size_t)sy * lw + sx) * 2;
                    light += (uint32_t)p[0] * p[1], opaque += p[1];
                }
            if (!opaque) continue;
            const uint32_t l = light / opaque, a = opaque / (uint32_t)((sy1 - sy0) * (sx1 - sx0));
            if (a) c.put(px, py, l << 16 | l << 8 | l, (int)a);
        }
    }
}

// the frame the logo starts closing at: down in the middle again, its opening done
uint64_t closing_from(const State &s) {
    return std::max<uint64_t>(s.chosen_at() + kLogoGlide, (uint64_t)logo::still_from(logo::opening));
}

}  // namespace

LogoAt logo_at(const State &s, uint64_t frame, int w, int h) {
    LogoAt at;
    if (!logo::ready()) return at;
    const double lw = logo::width(), lh = logo::height();
    // in the middle, and up in the corner: as high as the top's text, where HIRAKU's name was
    const double mid_x = (w - lw) / 2, mid_y = (h - lh) / 2, top_h = 40, top_w = lw * top_h / lh, top_x = 24, top_y = 4;
    const uint64_t opening = (uint64_t)logo::frames(logo::opening), dock = std::min(s.dock_at(), opening);
    const auto ease = [](double t) {
        t = std::clamp(t, 0.0, 1.0);
        return t * t * (3 - 2 * t);
    };
    const auto up = [&](uint64_t f) { return f < dock ? 0.0 : ease((double)(f - dock) / kLogoGlide); };   // 0 the middle, 1 the corner
    // the black behind it: whole while it opens, gone quickly as it goes up
    const auto dark = [&](uint64_t f) { return f < dock ? 255 : 255 - (int)std::min<uint64_t>(255, (f - dock) * 255 / kLogoFade); };
    double p = up(frame);
    at.dark = dark(frame);
    at.anim = logo::opening, at.frame = (int)std::min(frame, opening - 1);
    if (s.chosen() && frame >= s.chosen_at()) {
        p = up(s.chosen_at()) * (1 - ease((double)(frame - s.chosen_at()) / kLogoGlide));
        // ... and back as it comes down, whole by the time it is in the middle
        const uint64_t c = s.chosen_at(), from_c = kLogoGlide - kLogoFade;
        at.dark = std::max(dark(std::min(frame, c)), frame < c + from_c ? 0 : (int)std::min<uint64_t>(255, (frame - c - from_c) * 255 / kLogoFade));
        if (const uint64_t from = closing_from(s); frame >= from) {
            if (frame - from >= (uint64_t)logo::frames(logo::closing)) {
                at.anim = -1;   // closed: the black alone
                return at;
            }
            at.anim = logo::closing, at.frame = (int)(frame - from);
        }
    }
    at.w = (int)std::lround(lw + (top_w - lw) * p), at.h = (int)std::lround(lh + (top_h - lh) * p);
    at.x = (int)std::lround(mid_x + (top_x - mid_x) * p), at.y = (int)std::lround(mid_y + (top_y - mid_y) * p);
    return at;
}

uint64_t logo_closed(const State &s) { return logo::ready() ? closing_from(s) + (uint64_t)logo::frames(logo::closing) : 0; }

void draw(const State &s, const std::string &family, uint64_t frame, uint32_t *px, int w, int h) {
    if (!g_font_read) read_font();
    draw_menu(s, family, frame, px, w, h);
    const LogoAt at = logo_at(s, frame, w, h);
    Canvas c{px, w, h, 0, 0, w, h};
    if (at.dark) c.fill(0, 0, w, h, 0, at.dark);
    if (at.anim >= 0 && light(g_look.ground)) c.fill(at.x, at.y, at.w, at.h, 0);   // (its white lines: on black, on a light ground)
    if (at.anim >= 0) draw_logo(c, at);
}

}  // namespace hk::menu
