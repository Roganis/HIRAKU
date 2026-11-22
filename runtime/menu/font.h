// SPDX-License-Identifier: GPL-3.0-or-later
// The game menu's text in real fonts: the launcher draws each character the menu will need with
// its own fonts (launcher/menufont.h: every alphabet the PC's fonts have) and hands them over as a
// file, --font; the menu sets its text from them. Without one the menu keeps SDL's 8x8 font, which
// has ASCII only.
//
// The file, little-endian:
//     "HKMF1\0\0\0"
//     u32 sizes (3: small, a list's row, a heading; a game's boxes' file has a 4th, the performance
//         overlay's, host/statsview.h)
//     each size: u32 line height, u32 glyphs, then a glyph each:
//         u32 character (Unicode), i16 advance, i16 left, i16 top, u16 width, u16 height, u32 at
//     u32 bytes, then that many: every glyph's pixels, a byte each (how much of it is ink), rows
//     from the top; `at` is a glyph's first. left and top place them from the pen and the line's top.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace hk::menu {

// A text's characters (UTF-8; a byte that isn't is U+FFFD)
std::vector<uint32_t> utf8_points(const std::string &text);
// ... and back, the first n of them
std::string utf8_first(const std::string &text, size_t n);

class Font {
public:
    static constexpr int kSizes = 4;   // (at most: a file has 3 or 4)
    bool parse(const uint8_t *data, size_t n, std::string &error);
    bool load(const std::string &path, std::string &error);
    bool loaded() const { return loaded_; }
    int line(int size) const { return has(size) ? sizes_[size].line : 0; }
    bool has(int size) const { return loaded_ && size >= 0 && size < count_; }
    int width(int size, const std::string &text) const;
    // Into px (w x h, 0xffRRGGBB), within the box x0..x1, y0..y1: the text, its pen at x and its
    // line's top at y, in rgb at alpha (0..255)
    void draw(uint32_t *px, int w, int h, int x0, int y0, int x1, int y1, int x, int y, int size, const std::string &text, uint32_t rgb,
              int alpha) const;

private:
    struct Glyph {
        int advance = 0, left = 0, top = 0, w = 0, h = 0;
        uint32_t at = 0;
    };
    struct Size {
        int line = 0;
        std::unordered_map<uint32_t, Glyph> glyphs;
    };
    const Glyph *glyph(int size, uint32_t c) const;
    Size sizes_[kSizes];
    int count_ = 0;
    std::vector<uint8_t> ink_;
    bool loaded_ = false;
};

}  // namespace hk::menu
