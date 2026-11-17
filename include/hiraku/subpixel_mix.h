// HIRAKU: in-between pictures drawn between two pixels (--smooth-motion). A core draws the picture
// twice: once with each moving object at the whole pixel below its place along its move's main
// axis, once with each such object a pixel further on; where the two differ, each pixel is mixed by
// the fraction (16ths) of the object that drew it there, in linear light. DJ Main and Firebeat
// (runtime/djmain/board.cpp, runtime/dev/k057714.cpp); docs/SMOOTH.md.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace hk {

// `base` (xRGB8888, mixed in place) and `further`, n pixels; q(k): the fraction, 0..16, of what
// drew pixel k (0: left as `base` has it)
template <class Q>
void subpixel_mix(uint32_t *base, const uint32_t *further, size_t n, Q &&q) {
    // sRGB <-> linear light (16 bits)
    static uint16_t to_lin[256];
    static uint8_t to_srgb[4096];
    static const bool tables = [] {
        for (int v = 0; v < 256; v++) {
            const double c = v / 255.0;
            to_lin[v] = (uint16_t)std::lround(65535.0 * (c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4)));
        }
        for (int v = 0; v < 4096; v++) {
            const double l = v / 4095.0;
            to_srgb[v] = (uint8_t)std::lround(255.0 * (l <= 0.0031308 ? l * 12.92 : 1.055 * std::pow(l, 1 / 2.4) - 0.055));
        }
        return true;
    }();
    (void)tables;
    for (size_t k = 0; k < n; k++) {
        const uint32_t p0 = base[k], p1 = further[k];
        if (p0 == p1) continue;
        const int f = q(k);
        if (f <= 0) continue;
        const uint32_t a = (uint32_t)f * 4096 / 16, b = 4096 - a;   // the further picture's share, of 4096
        uint32_t mixed = 0xff000000u & p0;
        for (int sh = 0; sh <= 16; sh += 8) {
            const uint32_t l = (to_lin[p0 >> sh & 0xff] * b + to_lin[p1 >> sh & 0xff] * a) >> 12;   // 16 bits
            mixed |= (uint32_t)to_srgb[l >> 4] << sh;
        }
        base[k] = mixed;
    }
}

}  // namespace hk
