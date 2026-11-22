// SPDX-License-Identifier: GPL-3.0-or-later
#include "font.h"

#include <cstring>
#include <fstream>
#include <sstream>

namespace hk::menu {

std::vector<uint32_t> utf8_points(const std::string &text) {
    std::vector<uint32_t> out;
    const size_t n = text.size();
    for (size_t i = 0; i < n;) {
        const unsigned char c = (unsigned char)text[i];
        int more = c < 0x80 ? 0 : (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : -1;
        uint32_t p = more == 0 ? c : more == 1 ? c & 0x1fu : more == 2 ? c & 0x0fu : c & 0x07u;
        if (more > 0 && i + (size_t)more >= n) more = -1;   // cut short
        for (int k = 1; more > 0 && k <= more; k++) {
            const unsigned char d = (unsigned char)text[i + (size_t)k];
            if ((d & 0xc0) != 0x80) {
                more = -1;
                break;
            }
            p = p << 6 | (d & 0x3fu);
        }
        if (more < 0) {
            out.push_back(0xfffd);
            i++;
        } else {
            out.push_back(p);
            i += (size_t)more + 1;
        }
    }
    return out;
}

std::string utf8_first(const std::string &text, size_t count) {
    size_t i = 0;
    for (size_t k = 0; k < count && i < text.size(); k++) {
        const unsigned char c = (unsigned char)text[i];
        size_t len = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 1;
        for (size_t j = 1; j < len; j++)
            if (i + j >= text.size() || ((unsigned char)text[i + j] & 0xc0) != 0x80) {
                len = 1;
                break;
            }
        i += len;
    }
    return text.substr(0, i);
}

bool Font::parse(const uint8_t *d, size_t n, std::string &error) {
    loaded_ = false;
    for (Size &s : sizes_) s = Size{};
    ink_.clear();
    size_t at = 0;
    const auto u32 = [&](uint32_t &v) {
        if (n - at < 4) return false;
        v = (uint32_t)d[at] | (uint32_t)d[at + 1] << 8 | (uint32_t)d[at + 2] << 16 | (uint32_t)d[at + 3] << 24;
        at += 4;
        return true;
    };
    const auto u16 = [&](uint32_t &v) {
        if (n - at < 2) return false;
        v = (uint32_t)d[at] | (uint32_t)d[at + 1] << 8;
        at += 2;
        return true;
    };
    const auto bad = [&](const char *why) {
        error = why;
        for (Size &s : sizes_) s = Size{};
        count_ = 0;
        return false;
    };
    if (n < 8 || memcmp(d, "HKMF1\0\0\0", 8) != 0) return bad("not a menu font (its first bytes aren't HKMF1)");
    at = 8;
    uint32_t count = 0;
    if (!u32(count) || count < 3 || count > (uint32_t)kSizes) return bad("not three or four sizes");
    struct Raw {
        int size;
        uint32_t c;
        Glyph g;
    };
    std::vector<Raw> raw;
    for (int s = 0; s < (int)count; s++) {
        uint32_t line = 0, glyphs = 0;
        if (!u32(line) || !u32(glyphs) || line == 0 || line > 256 || glyphs > 65536) return bad("a size's line height or glyph count is out of range");
        if ((n - at) / 18 < glyphs) return bad("cut short in a size's glyphs");
        sizes_[s].line = (int)line;
        for (uint32_t k = 0; k < glyphs; k++) {
            uint32_t c = 0, adv = 0, left = 0, top = 0, gw = 0, gh = 0, where = 0;
            u32(c), u16(adv), u16(left), u16(top), u16(gw), u16(gh), u32(where);
            if (gw > 512 || gh > 512) return bad("a glyph larger than 512 pixels");
            raw.push_back({s, c, {(int16_t)adv, (int16_t)left, (int16_t)top, (int)gw, (int)gh, where}});
        }
    }
    uint32_t bytes = 0;
    if (!u32(bytes) || n - at < bytes) return bad("cut short in the glyphs' pixels");
    for (const Raw &r : raw) {
        if ((uint64_t)r.g.at + (uint64_t)r.g.w * (uint64_t)r.g.h > bytes) return bad("a glyph's pixels are outside the file");
        sizes_[r.size].glyphs[r.c] = r.g;
    }
    ink_.assign(d + at, d + at + bytes);
    count_ = (int)count;
    loaded_ = true;
    return true;
}

bool Font::load(const std::string &path, std::string &error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot read " + path;
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string all = ss.str();
    if (!parse((const uint8_t *)all.data(), all.size(), error)) {
        error = path + ": " + error;
        return false;
    }
    return true;
}

// a character's glyph: its own, else a question mark's, else none
const Font::Glyph *Font::glyph(int size, uint32_t c) const {
    const auto &g = sizes_[size].glyphs;
    auto it = g.find(c);
    if (it == g.end()) it = g.find('?');
    return it == g.end() ? nullptr : &it->second;
}

int Font::width(int size, const std::string &text) const {
    if (size < 0 || size >= count_) return 0;
    int w = 0;
    for (const uint32_t c : utf8_points(text))
        if (const Glyph *g = glyph(size, c)) w += g->advance;
    return w;
}

void Font::draw(uint32_t *px, int w, int h, int x0, int y0, int x1, int y1, int x, int y, int size, const std::string &text, uint32_t rgb,
                int alpha) const {
    if (size < 0 || size >= count_ || alpha <= 0) return;
    x0 = x0 < 0 ? 0 : x0, y0 = y0 < 0 ? 0 : y0, x1 = x1 > w ? w : x1, y1 = y1 > h ? h : y1;
    for (const uint32_t c : utf8_points(text)) {
        const Glyph *g = glyph(size, c);
        if (!g) continue;
        const int gx = x + g->left, gy = y + g->top;
        if (gx < x1 && gx + g->w > x0 && gy < y1 && gy + g->h > y0)
            for (int row = 0; row < g->h; row++) {
                const int py = gy + row;
                if (py < y0 || py >= y1) continue;
                const uint8_t *ink = &ink_[g->at + (size_t)row * (size_t)g->w];
                for (int col = 0; col < g->w; col++) {
                    const int pxx = gx + col;
                    if (pxx < x0 || pxx >= x1 || !ink[col]) continue;
                    const uint32_t a = (uint32_t)ink[col] * (uint32_t)alpha / 255;
                    uint32_t &p = px[(size_t)py * (size_t)w + (size_t)pxx];
                    const uint32_t rb = ((rgb & 0xff00ff) * a + (p & 0xff00ff) * (255 - a)) >> 8 & 0xff00ff;
                    const uint32_t gg = ((rgb & 0x00ff00) * a + (p & 0x00ff00) * (255 - a)) >> 8 & 0x00ff00;
                    p = 0xff000000u | rb | gg;
                }
            }
        x += g->advance;
    }
}

}  // namespace hk::menu
