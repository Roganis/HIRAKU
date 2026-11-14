// SPDX-License-Identifier: GPL-3.0-or-later
// runtime/menu/logo.anim read (its layout: tools/logoanim.py)
#include "logo.h"

#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

extern const unsigned char hk_logo_anim[];   // (cmake/embed.cmake)
extern const unsigned long hk_logo_anim_size;

namespace hk::menu::logo {

namespace {

struct Logo {
    bool tried = false, ok = false;
    int w = 0, h = 0;
    std::vector<int> order[2];           // each frame's picture
    std::vector<uint8_t> pictures;       // one after the other
    int still[2] = {0, 0};
};
Logo g;

bool read(const uint8_t *p, size_t n) {
    size_t at = 0;
    const auto u16 = [&](uint32_t &v) {
        if (at + 2 > n) return false;
        v = (uint32_t)p[at] | (uint32_t)p[at + 1] << 8, at += 2;
        return true;
    };
    const auto u32 = [&](uint32_t &v) {
        uint32_t lo, hi;
        if (!u16(lo) || !u16(hi)) return false;
        v = lo | hi << 16;
        return true;
    };
    uint32_t w, h, anims, count, packed, raw;
    if (n < 8 || memcmp(p, "HKLOGO1", 8) != 0) return false;
    at = 8;
    if (!u16(w) || !u16(h) || !u16(anims) || anims != 2 || !w || !h) return false;
    for (int a = 0; a < 2; a++) {
        uint32_t frames;
        if (!u16(frames) || !frames) return false;
        for (uint32_t i = 0; i < frames; i++) {
            uint32_t k;
            if (!u16(k)) return false;
            g.order[a].push_back((int)k);
        }
    }
    if (!u16(count) || !u32(packed) || !u32(raw) || !count || at + packed > n) return false;
    const size_t one = (size_t)w * h * 2;
    if (raw != one * count) return false;
    for (const std::vector<int> &o : g.order)
        for (int k : o)
            if (k >= (int)count) return false;
    g.pictures.resize(raw);
    uLongf got = raw;
    if (uncompress(g.pictures.data(), &got, p + at, packed) != Z_OK || got != raw) return false;
    for (size_t i = one; i < raw; i++) g.pictures[i] = (uint8_t)(g.pictures[i] + g.pictures[i - one]);   // each from the one before
    g.w = (int)w, g.h = (int)h;
    for (int a = 0; a < 2; a++) {
        const std::vector<int> &o = g.order[a];
        int s = (int)o.size() - 1;
        while (s > 0 && o[(size_t)s - 1] == o.back()) s--;
        g.still[a] = s;
    }
    return true;
}

const Logo &get() {
    if (!g.tried) {
        g.tried = true;
        g.ok = read(hk_logo_anim, hk_logo_anim_size);
        if (!g.ok) {
            fprintf(stderr, "menu: the logo's animation couldn't be read\n");
            g = Logo();
            g.tried = true;
        }
    }
    return g;
}

}  // namespace

bool ready() { return get().ok; }
int width() { return get().w; }
int height() { return get().h; }
int frames(Anim a) { return ready() ? (int)g.order[a].size() : 0; }
int still_from(Anim a) { return ready() ? g.still[a] : 0; }

const uint8_t *frame(Anim a, int n) {
    if (!ready()) return nullptr;
    const std::vector<int> &o = g.order[a];
    const int k = o[(size_t)std::clamp(n, 0, (int)o.size() - 1)];
    return g.pictures.data() + (size_t)k * g.w * g.h * 2;
}

}  // namespace hk::menu::logo
