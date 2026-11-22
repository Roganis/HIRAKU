// SPDX-License-Identifier: GPL-3.0-or-later
#include "uibox.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <charconv>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <mutex>

#include "lang/fit.h"
#include "lang/lang.h"
#include "menu/font.h"

namespace hk::host::ui {

namespace {

std::once_flag g_once;
bool g_styled = false;
double g_scale = 1;
hk::ui::Look g_look;
hk::menu::Font g_font;
std::vector<uint8_t> g_logo;   // how light each pixel is
int g_logo_w = 0, g_logo_h = 0;

void read_logo(const std::string &path) {
    g_logo.clear(), g_logo_w = g_logo_h = 0;
    SDL_Surface *file = path.empty() ? nullptr : SDL_LoadBMP(path.c_str());
    SDL_Surface *s = file ? SDL_ConvertSurface(file, SDL_PIXELFORMAT_ARGB8888) : nullptr;
    if (s && s->w > 0 && s->h > 0 && s->w <= 4096 && s->h <= 1024) {
        g_logo_w = s->w, g_logo_h = s->h;
        g_logo.resize((size_t)s->w * s->h);
        for (int y = 0; y < s->h; y++)
            for (int x = 0; x < s->w; x++) {
                const uint32_t p = ((const uint32_t *)((const uint8_t *)s->pixels + (size_t)y * s->pitch))[x];
                g_logo[(size_t)y * s->w + x] = (uint8_t)(((p >> 16 & 255) * 2 + (p >> 8 & 255) * 5 + (p & 255)) / 8);
            }
    }
    if (s) SDL_DestroySurface(s);
    if (file) SDL_DestroySurface(file);
}

bool take(const std::string &look, const std::string &font, const std::string &logo, double scale) {
    g_styled = false;
    g_scale = 1;
    g_look = hk::ui::Look();
    g_font = hk::menu::Font();
    std::string why;
    if (look.empty() || font.empty()) return false;
    if (!g_look.parse(look, why) || !g_font.load(font, why)) {
        fprintf(stderr, "boxes: %s: SDL's 8x8 font instead\n", why.c_str());
        g_look = hk::ui::Look();
        g_font = hk::menu::Font();
        return false;
    }
    read_logo(logo);
    g_scale = std::clamp(scale, 1.0, 8.0);
    g_styled = true;
    return true;
}

void from_environment() {
    const char *look = getenv("HK_UI_LOOK"), *font = getenv("HK_UI_FONT"), *logo = getenv("HK_UI_LOGO"), *scale = getenv("HK_UI_SCALE");
    double k = 1;
    if (scale) std::from_chars(scale, scale + strlen(scale), k);   // (whatever the locale: "1.333")
    take(look ? look : "", font ? font : "", logo ? logo : "", k);
}

// "a character" as UTF-8, for the font's icons
std::string utf8(uint32_t c) {
    std::string s;
    if (c < 0x80) s += (char)c;
    else if (c < 0x800) s += (char)(0xc0 | c >> 6), s += (char)(0x80 | (c & 63));
    else if (c < 0x10000) s += (char)(0xe0 | c >> 12), s += (char)(0x80 | (c >> 6 & 63)), s += (char)(0x80 | (c & 63));
    else s += (char)(0xf0 | c >> 18), s += (char)(0x80 | (c >> 12 & 63)), s += (char)(0x80 | (c >> 6 & 63)), s += (char)(0x80 | (c & 63));
    return s;
}

// Drawing into a box's picture: shapes with smoothed edges, laid over what is there (straight alpha)
struct Paint {
    Image &im;
    void put(int x, int y, uint32_t rgb, int a) {
        if (x < 0 || y < 0 || x >= im.w || y >= im.h || a <= 0) return;
        uint32_t &p = im.px[(size_t)y * im.w + x];
        const int da = (int)(p >> 24), oa = a + da * (255 - a) / 255;
        if (oa <= 0) return;
        uint32_t out = (uint32_t)oa << 24;
        for (int sh = 0; sh < 24; sh += 8) {
            const int s = (int)(rgb >> sh & 255), d = (int)(p >> sh & 255);
            out |= (uint32_t)std::clamp((s * a + d * da * (255 - a) / 255) / oa, 0, 255) << sh;
        }
        p = out;
    }
    // how much of the pixel at (xx, yy) a rounded rectangle covers
    static double cover(double x, double y, double w, double h, double r, int xx, int yy) {
        r = std::min({r, w / 2, h / 2});
        const double px = xx + 0.5, py = yy + 0.5;
        const double cx = std::clamp(px, x + r, x + w - r), cy = std::clamp(py, y + r, y + h - r);
        // inside the corners' circles, and the straight edges cut at the pixel
        double c = std::clamp(r - std::hypot(px - cx, py - cy) + 0.5, 0.0, 1.0);
        c *= std::clamp(std::min(px + 0.5 - x, x + w - (px - 0.5)), 0.0, 1.0);
        c *= std::clamp(std::min(py + 0.5 - y, y + h - (py - 0.5)), 0.0, 1.0);
        return c;
    }
    void round_rect(double x, double y, double w, double h, double r, uint32_t rgb, int a = 255) {
        const int xa = std::max(0, (int)std::floor(x)), ya = std::max(0, (int)std::floor(y));
        const int xb = std::min(im.w, (int)std::ceil(x + w)), yb = std::min(im.h, (int)std::ceil(y + h));
        for (int yy = ya; yy < yb; yy++)
            for (int xx = xa; xx < xb; xx++)
                if (const double c = cover(x, y, w, h, r, xx, yy); c > 0) put(xx, yy, rgb, (int)std::lround(a * c));
    }
    void text(int x, int y, int size, const std::string &t, uint32_t rgb) {
        g_font.draw(im.px.data(), im.w, im.h, 0, 0, im.w, im.h, x, y, size, t, rgb, 255);
    }
    void icon(int x, int y, int size, Icon i, uint32_t rgb) { text(x, y, size, utf8((uint32_t)i), rgb); }
};

int width(int size, const std::string &t) { return g_font.width(size, t); }

// the box itself: its ground, a line round it, its corners round
void ground(Paint &p, double s) {
    const hk::ui::Look &L = g_look;
    p.round_rect(0, 0, p.im.w, p.im.h, 12.0 * s, L.line);
    p.round_rect(s, s, p.im.w - 2 * s, p.im.h - 2 * s, 11.0 * s, L.ground);
}

void start(Image &out, int w, int h) {
    out.w = w, out.h = h, out.scale = g_scale;
    out.px.assign((size_t)w * h, 0);
}

}  // namespace

const char *icon_name(Icon i) {
    static const char *names[kIcons] = {"information",  "alert-circle", "download", "camera",    "volume-off",
                                        "card-account-details", "floppy", "dialpad", "fast-forward", "view-list"};
    const uint32_t k = (uint32_t)i - (uint32_t)Icon::info;
    return i == Icon::none || k >= (uint32_t)kIcons ? "" : names[k];
}

bool styled() {
    std::call_once(g_once, from_environment);
    return g_styled;
}
double scale() { return styled() ? g_scale : 1; }
const hk::menu::Font *font() { return styled() ? &g_font : nullptr; }

std::string said(const char *context, const std::string &text, bool keep) {
    return styled() ? hk::lang::shown(context, text, keep) : hk::lang::english(text);
}

int max_line_width() { return (int)std::lround(300 * scale()); }

std::vector<std::string> wrap(const std::string &text, int size) {
    if (!styled()) return {hk::lang::unmarked(text)};
    return hk::lang::wrap(text, max_line_width(), [size](uint32_t c) { return g_font.width(size, utf8(c)); });
}
const hk::ui::Look &look() {
    styled();
    return g_look;
}

uint32_t icon_colour(Icon i) {
    // the six: the look's, or the system's look's (KDE's Breeze colours)
    static const uint32_t plain[6] = {0x2980b9, 0x27ae60, 0xf1b62b, 0xf67400, 0xda4453, 0x9b59b6};
    const std::vector<uint32_t> &six = g_look.stripe;
    auto k = [&](int n) { return six.size() == 6 ? six[(size_t)n] : plain[n]; };
    switch (i) {
    case Icon::alert: return k(4);
    case Icon::update: return k(5);
    case Icon::picture: return k(1);
    case Icon::sound: return k(3);
    case Icon::card:
    case Icon::disk: return k(0);
    case Icon::dialpad:
    case Icon::fast:
    case Icon::menu: return g_look.point;
    default: return k(2);
    }
}

void notice_box(Image &out, Icon icon, const std::vector<std::string> &lines) {
    styled();
    const double s = g_scale;
    const auto u = [s](double n) { return (int)std::lround(n * s); };   // a 360-line screen's pixels, here
    const int pad = u(12), edge = u(4), icon_w = g_font.line(2), gap = u(12);
    const int title = g_font.line(1), small = g_font.line(0);
    // each line given a paragraph, broken to the box's width: the title's lines in the title's size
    std::vector<std::string> broken;
    size_t titles = 0;
    for (size_t k = 0; k < lines.size(); k++)
        for (const std::string &l : wrap(lines[k], k ? 0 : 1)) {
            broken.push_back(l);
            if (!k) titles++;
        }
    if (lines.size() && !titles) broken.insert(broken.begin(), ""), titles = 1;
    std::vector<std::string> said = std::move(broken);
    int text_w = 0;
    for (size_t k = 0; k < said.size(); k++) text_w = std::max(text_w, width(k < titles ? 1 : 0, said[k]));
    const int text_h = (int)titles * title + (int)(said.size() - titles) * small;
    const int w = edge + pad + icon_w + gap + text_w + pad + u(4), h = std::max(text_h, icon_w) + 2 * pad;
    start(out, w, h);
    Paint p{out};
    const uint32_t c = icon_colour(icon);
    // the edge in the icon's colour: the box, its left side
    p.round_rect(0, 0, w, h, 12.0 * s, c);
    p.round_rect(edge - s, 0, w - edge + s, h, 12.0 * s, g_look.line);
    p.round_rect(edge, s, w - edge - s, h - u(2), 11.0 * s, g_look.ground);
    const int x = edge + pad;
    if (icon != Icon::none) p.icon(x, (h - icon_w) / 2, 2, icon, c);
    int y = (h - text_h) / 2;
    const int tx = x + icon_w + gap;
    for (size_t k = 0; k < said.size(); k++) {
        const bool head = k < titles;
        p.text(tx, y, head ? 1 : 0, said[k], head ? g_look.ink : hk::ui::mix(g_look.ink, g_look.dim, 90));
        y += head ? title : small;
    }
}

void bar_box(Image &out, bool logo, Icon heading_icon, const std::string &heading, Icon line_icon, const std::string &line,
             double done, const std::string &foot) {
    styled();
    const double s = g_scale;
    const auto u = [s](double n) { return (int)std::lround(n * s); };
    const int pad = u(16), big = g_font.line(2), small = g_font.line(0), gap = u(12);
    const bool has_logo = logo && g_logo_w > 0;
    const int lead = has_logo ? g_logo_w + u(8) : heading_icon != Icon::none ? big : 0;
    const int line_lead = line_icon != Icon::none ? small + u(8) : 0;
    const int inner = std::max({u(300) - 2 * pad, lead + gap + width(2, heading), line_lead + width(0, line), width(0, foot)});
    const int bar_h = u(10);
    const int head_h = std::max(big, has_logo ? g_logo_h + u(8) : 0);
    const int h = pad + head_h + u(8) + small + u(12) + bar_h + (foot.empty() ? 0 : u(8) + small) + pad;
    const int w = inner + 2 * pad;
    start(out, w, h);
    Paint p{out};
    ground(p, s);
    int y = pad;
    // the heading, after HIRAKU's name (white on black) or an icon
    int x = pad;
    if (has_logo) {
        const int ly = y + (head_h - g_logo_h - u(8)) / 2;
        p.round_rect(x, ly, g_logo_w + u(8), g_logo_h + u(8), 2.0 * s, 0);
        for (int yy = 0; yy < g_logo_h; yy++)
            for (int xx = 0; xx < g_logo_w; xx++) p.put(x + u(4) + xx, ly + u(4) + yy, 0xffffff, g_logo[(size_t)yy * g_logo_w + xx]);
        x += lead + gap;
    } else if (heading_icon != Icon::none) {
        p.icon(x, y + (head_h - big) / 2, 2, heading_icon, icon_colour(heading_icon));
        x += lead + gap;
    }
    p.text(x, y + (head_h - big) / 2, 2, heading, g_look.ink);
    y += head_h + u(8);
    // the line, after its icon
    x = pad;
    if (line_icon != Icon::none) {
        p.icon(x, y, 0, line_icon, icon_colour(line_icon));
        x += line_lead;
    }
    p.text(x, y, 0, line, hk::ui::mix(g_look.ink, g_look.dim, 60));
    y += small + u(12);
    // the bar: the six colours side by side, filled from the left (or the bar's colour)
    const int bw = w - 2 * pad, fill = (int)std::lround(bw * std::clamp(done, 0.0, 1.0));
    p.round_rect(pad, y, bw, bar_h, bar_h / 2.0, g_look.line);
    const std::vector<uint32_t> &six = g_look.stripe;
    for (int yy = y; yy < y + bar_h; yy++)
        for (int xx = pad; xx < pad + fill; xx++) {
            const double c = Paint::cover(pad, y, bw, bar_h, bar_h / 2.0, xx, yy);
            const uint32_t rgb = six.empty() ? g_look.bar : six[std::min(six.size() - 1, (size_t)((xx - pad) * (int)six.size() / bw))];
            if (c > 0) p.put(xx, yy, rgb, (int)std::lround(255 * c));
        }
    y += bar_h;
    if (!foot.empty()) p.text(pad, y + u(8), 0, foot, g_look.dim);
}

bool set_for_test(const std::string &look, const std::string &font, const std::string &logo, double scale) {
    std::call_once(g_once, [] {});
    return take(look, font, logo, scale);
}

}  // namespace hk::host::ui
