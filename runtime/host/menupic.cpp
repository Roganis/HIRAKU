// SPDX-License-Identifier: GPL-3.0-or-later
#include "menupic.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

#include "shell/sys.h"

namespace hk::host::menupic {

namespace {

void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24); }

}  // namespace

bool save(const std::string &path, const uint32_t *rgb, int w, int h, double aspect) {
    if (!rgb || w <= 0 || h <= 0 || path.empty()) return false;
    const int th = kLines;
    const int tw = std::clamp((int)std::lround(th * (aspect > 0 ? aspect : (double)w / h)), th / 2, th * 3);
    const int row = (tw * 3 + 3) & ~3;   // a BMP's rows are whole words
    std::vector<uint8_t> out(54 + (size_t)row * th);
    std::vector<uint32_t> shades(4096, 0);   // how many pixels of each colour, 4 bits a part
    for (int ty = 0; ty < th; ty++) {
        const int y0 = (int)((int64_t)ty * h / th), y1 = std::max(y0 + 1, (int)((int64_t)(ty + 1) * h / th));
        uint8_t *line = &out[54 + (size_t)row * (th - 1 - ty)];   // the last line first
        for (int tx = 0; tx < tw; tx++) {
            const int x0 = (int)((int64_t)tx * w / tw), x1 = std::max(x0 + 1, (int)((int64_t)(tx + 1) * w / tw));
            uint32_t r = 0, g = 0, b = 0;   // the mean of the frame's pixels this one covers
            for (int y = y0; y < y1; y++)
                for (int x = x0; x < x1; x++) {
                    const uint32_t p = rgb[(size_t)y * w + x];
                    r += p >> 16 & 0xff, g += p >> 8 & 0xff, b += p & 0xff;
                }
            const uint32_t n = (uint32_t)(y1 - y0) * (uint32_t)(x1 - x0);
            line[tx * 3] = (uint8_t)(b / n), line[tx * 3 + 1] = (uint8_t)(g / n), line[tx * 3 + 2] = (uint8_t)(r / n);
            shades[(r / n >> 4) << 8 | (g / n >> 4) << 4 | (b / n >> 4)]++;
        }
    }
    // A blank screen, or a screen of text (a start-up check, an error, a test menu: nearly all of
    // it a handful of flat colours) isn't a picture of the game. Of the 86 title screens measured
    // the plainest needs 11 colours for 97 % of its pixels; the error screens 2 to 4.
    std::sort(shades.begin(), shades.end(), std::greater<uint32_t>());
    uint32_t covered = 0;
    int colours = 0;
    while (covered < (uint32_t)tw * th / 100 * 97) covered += shades[(size_t)colours++];
    if (colours < kColours) return false;
    out[0] = 'B', out[1] = 'M';
    put32(&out[2], (uint32_t)out.size());
    put32(&out[10], 54);
    put32(&out[14], 40);
    put32(&out[18], (uint32_t)tw);
    put32(&out[22], (uint32_t)th);
    out[26] = 1, out[28] = 24;
    put32(&out[34], (uint32_t)row * th);
    put32(&out[38], 2835), put32(&out[42], 2835);   // 72 dots an inch
    if (const size_t slash = path.find_last_of("/\\"); slash != std::string::npos && slash > 0) sys::make_dirs(path.substr(0, slash));
    return sys::write_file(path, out.data(), out.size());
}

}  // namespace hk::host::menupic
