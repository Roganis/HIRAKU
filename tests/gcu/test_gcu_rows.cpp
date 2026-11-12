// SPDX-License-Identifier: GPL-3.0-or-later
// The GCU's rows (runtime/dev/k057714_rows.h: eight pixels at a time where the host can) against
// the pixel-by-pixel loops they stand for, on runs drawn at random: every length and parity,
// source and destination apart, near and overlapping, with and without transparency, every blend
// weight. The whole buffer is compared after each.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "dev/k057714_rows.h"

using namespace hk::gcu_rows;

int main() {
    uint64_t rng = 0x9e3779b97f4a7c15ull;
    auto rnd = [&] {
        rng ^= rng << 13, rng ^= rng >> 7, rng ^= rng << 17;
        return (uint32_t)(rng >> 16);
    };
    constexpr int SIZE = 4096, N = 3000000;
    std::vector<uint16_t> a(SIZE), b(SIZE);
    uint64_t wrong[3] = {}, overlapping = 0;
    for (int t = 0; t < N; t++) {
        for (int q = 0; q < SIZE; q += 1 + (int)(rnd() % 3)) a[q] = (uint16_t)rnd();
        if (t % 64 == 0)
            for (uint16_t &x : a) x = (uint16_t)rnd();
        b = a;
        const int n = 1 + (int)(rnd() % (rnd() % 4 ? 40 : 300));
        const uint32_t s = 16 + rnd() % 3400;
        // the destination: anywhere, or within a few pixels of the source
        uint32_t d = rnd() % 3 ? 16 + rnd() % 3400 : s + rnd() % 80 - (s > 100 ? 40 : 0);
        d = std::min<uint32_t>(std::max<uint32_t>(d, 16), 3416);
        const bool trans = rnd() % 4 != 0;
        const uint16_t want = rnd() & 1 ? 0x8000 : 0;
        const uint32_t a1 = rnd() % 32, a2 = rnd() % 32;
        if (s <= d + n + 1 && d <= s + n + 1) overlapping++;
        const int which = t % 3;
        if (which == 0) {
            blit_row(a.data(), s, d, n, trans, want);
            for (int k = 0; k < n; k++) {
                const uint16_t p = b[(s + k) ^ 1u];
                if (trans && (p & 0x8000) != want) continue;
                b[(d + k) ^ 1u] = p;
            }
        } else if (which == 1) {
            blend_row(a.data(), s, d, n, trans, want, a1, a2);
            for (int k = 0; k < n; k++) {   // (K057714::draw_object's loop, blend modes 1 and 2)
                const uint16_t p = b[(s + k) ^ 1u];
                if (trans && (p & 0x8000) != want) continue;
                const uint16_t o = b[(d + k) ^ 1u];
                uint32_t sr = (o >> 10) & 0x1f, sg = (o >> 5) & 0x1f, sb = o & 0x1f;
                const uint32_t r = (p >> 10) & 0x1f, g = (p >> 5) & 0x1f, bl = p & 0x1f;
                sr = std::min((sr * a2 + r * a1) >> 4, 0x1fu);
                sg = std::min((sg * a2 + g * a1) >> 4, 0x1fu);
                sb = std::min((sb * a2 + bl * a1) >> 4, 0x1fu);
                b[(d + k) ^ 1u] = (uint16_t)((sr << 10) | (sg << 5) | sb | (p & 0x8000));
            }
        } else {
            const uint16_t color[4] = {(uint16_t)rnd(), (uint16_t)rnd(), (uint16_t)rnd(), (uint16_t)rnd()};
            fill_row(a.data(), d, n, color);
            for (int k = 0; k < n; k++) b[(d + k) ^ 1u] = color[(d + k) & 3];
        }
        if (a != b) wrong[which]++, a = b;
    }
    printf("test_gcu_rows: %d runs (%llu with source and destination overlapping), wrong: %llu copied, %llu blended, %llu filled\n", N,
           (unsigned long long)overlapping, (unsigned long long)wrong[0], (unsigned long long)wrong[1], (unsigned long long)wrong[2]);
    return wrong[0] || wrong[1] || wrong[2] ? 1 : 0;
}
