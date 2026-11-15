// SPDX-License-Identifier: GPL-3.0-or-later
// The PlayStation GPU's textured rows (runtime/sys573/psx_spans.h) against the pixel-by-pixel
// loop of MAME's psx.cpp they stand for (TEXTUREFILL with no texture window, no interleave and no
// mask test: the texel expressions and SHADEDPIXEL / TRANSPARENTPIXEL as written there), on rows
// drawn at random in a small VRAM: every depth, a polygon's u (16.16, any step, wrapping) and a
// sprite's (a byte, stepping 1 or -1, wrapping at 256), plain, shaded and semi-transparent, the
// mask bit set or not, and rows that draw over their own texture or palette. The whole VRAM is
// compared after each. `--bench` times the two instead.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "sys573/psx_spans.h"

using namespace hk::psxspan;

namespace {

uint64_t g_rng = 0x9e3779b97f4a7c15ull;
uint32_t rnd() {
    g_rng ^= g_rng << 13, g_rng ^= g_rng >> 7, g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 16);
}

// MAME's tables, by their shapes: shade[ level << 8 | shade ]; f[ level << 8 | shade ] and
// b[ pixel ] make an index into trans[]
struct Tables {
    std::vector<uint16_t> redshade, greenshade, blueshade, f, redb, greenb, blueb, redtrans, greentrans, bluetrans;
    Tables()
        : redshade(32 * 256), greenshade(32 * 256), blueshade(32 * 256), f(32 * 256), redb(65536), greenb(65536),
          blueb(65536), redtrans(1024), greentrans(1024), bluetrans(1024) {
        for (auto *t : {&redshade, &greenshade, &blueshade, &redtrans, &greentrans, &bluetrans})
            for (uint16_t &x : *t) x = (uint16_t)(rnd() & 0x7fff);
        for (uint16_t &x : f) x = (uint16_t)((rnd() & 31) << 5);
        for (auto *t : {&redb, &greenb, &blueb})
            for (uint16_t &x : *t) x = (uint16_t)(rnd() & 31);
    }
};

struct Row {
    int kind;        // 1 plain, 2 shaded, 3 semi-transparent
    bool neutral;    // (kind 3) the shade is 0x80 in all three
    int tp;          // 0, 1, 2
    bool sprite;     // u a byte stepping du (1 or -1); else 16.16 stepping du
    uint32_t u;      // sprite: the byte; polygon: n_u.d
    int32_t du;
    int n;
    uint16_t *d;
    const uint16_t *row, *clut;
    bool draw_stp;
    unsigned r, g, b;
};

// psx.cpp's loop
void mame(const Row &w, const Tables &t) {
    uint16_t *p_vram = w.d;
    const uint16_t *p_clut = w.clut;
    uint32_t n_u_d = w.u;
    uint8_t n_u8 = (uint8_t)w.u;
    int n_distance = w.n;
    while (n_distance > 0) {
        const unsigned TXU = w.sprite ? n_u8 : (uint16_t)(n_u_d >> 16);
        uint16_t n_bgr;
        if (w.tp == 0) n_bgr = p_clut[(*(w.row + (TXU >> 2)) >> ((TXU & 0x03) << 2)) & 0x0f];
        else if (w.tp == 1) n_bgr = p_clut[(*(w.row + (TXU >> 1)) >> ((TXU & 0x01) << 3)) & 0xff];
        else n_bgr = *(w.row + TXU);
        auto write = [&](unsigned p) { *p_vram = (uint16_t)(w.draw_stp ? p | 0x8000 : p); };
        const unsigned rl = (n_bgr & 0x1f) << 8, gl = ((n_bgr >> 5) & 0x1f) << 8, bl = ((n_bgr >> 10) & 0x1f) << 8;
        if (n_bgr != 0) {
            if (w.kind == 3 && (n_bgr & 0x8000) != 0)
                write(t.redtrans[t.f[rl | w.r] | t.redb[*p_vram]] | t.greentrans[t.f[gl | w.g] | t.greenb[*p_vram]] |
                      t.bluetrans[t.f[bl | w.b] | t.blueb[*p_vram]] | 0x8000);
            else if (w.kind == 1 || (w.kind == 3 && w.neutral))
                write(n_bgr);
            else if (w.kind == 2)
                write(t.redshade[rl | w.r] | t.greenshade[gl | w.g] | t.blueshade[bl | w.b] | (n_bgr & 0x8000));
            else
                write(t.redshade[rl | w.r] | t.greenshade[gl | w.g] | t.blueshade[bl | w.b]);
        }
        p_vram++;
        if (w.sprite) n_u8 = (uint8_t)(n_u8 + w.du);
        else n_u_d += (uint32_t)w.du;
        n_distance--;
    }
}

// HIRAKU's, called as psx.cpp's HK_SPANSETUP and HK_SPANFILL call it
// (a routine sets its shade up once, for all its rows: `made` is that, for the timing)
void hiraku(const Row &w, const Tables &t, const Shade *made = nullptr) {
    Shade own;
    const bool shade = w.kind == 2 || (w.kind == 3 && !w.neutral);
    if (shade && !made)
        for (int l = 0; l < 32; l++) {
            own.r[l] = t.redshade[(l << 8) | w.r];
            own.g[l] = t.greenshade[(l << 8) | w.g];
            own.b[l] = t.blueshade[(l << 8) | w.b];
        }
    const Shade &sh = made ? *made : own;
    const Blend bl = {t.f.data(), t.redb.data(), t.greenb.data(), t.blueb.data(), t.redtrans.data(), t.greentrans.data(), t.bluetrans.data()};
    const uint16_t stp = w.draw_stp ? 0x8000 : 0;
    if (w.sprite) {
        const uint32_t u = w.u << 24, du = (uint32_t)w.du << 24;
        if (w.kind == 1) row_plain<24>(w.tp, w.d, w.n, w.row, u, du, w.clut, stp);
        else if (w.kind == 2) row_shaded<24>(w.tp, w.d, w.n, w.row, u, du, w.clut, stp, sh);
        else row_mixed<24>(w.tp, w.d, w.n, w.row, u, du, w.clut, stp, shade ? &sh : nullptr, bl, w.r, w.g, w.b);
    } else {
        const uint32_t u = w.u, du = (uint32_t)w.du;
        if (w.kind == 1) row_plain<16>(w.tp, w.d, w.n, w.row, u, du, w.clut, stp);
        else if (w.kind == 2) row_shaded<16>(w.tp, w.d, w.n, w.row, u, du, w.clut, stp, sh);
        else row_mixed<16>(w.tp, w.d, w.n, w.row, u, du, w.clut, stp, shade ? &sh : nullptr, bl, w.r, w.g, w.b);
    }
}

// the VRAM rows are drawn in: a few short rows, so that a row often meets its texture or palette;
// what follows them is there for a polygon's u, which reaches 65535 texels along
constexpr int ROWS = 6, WIDTH = 512, AFTER = 0x10000 + 16;

Row random_row(uint16_t *vram) {
    Row w;
    w.kind = 1 + (int)(rnd() % 3);
    w.neutral = rnd() & 1;
    w.tp = (int)(rnd() % 3);
    w.sprite = rnd() & 1;
    w.n = 1 + (int)(rnd() % (rnd() % 4 ? 40 : 300));
    if (w.sprite) {
        w.u = rnd() & 255;
        w.du = rnd() % 5 ? 1 : -1;
    } else {
        w.u = (rnd() % 8 ? rnd() & 0x00ffffff : rnd() << 4);
        const uint32_t k = rnd() % 8;
        w.du = k < 4 ? 0x10000 : k == 4 ? -0x10000 : k == 5 ? (int32_t)(rnd() & 0x3ffff) : k == 6 ? -(int32_t)(rnd() & 0x3ffff) : 0;
    }
    w.d = vram + (rnd() % ROWS) * WIDTH + rnd() % (WIDTH - w.n);
    w.row = vram + (rnd() % ROWS) * WIDTH + rnd() % WIDTH;
    w.clut = vram + (rnd() % ROWS) * WIDTH + rnd() % (WIDTH - 256);
    w.draw_stp = rnd() % 4 == 0;
    w.r = w.neutral && w.kind == 3 ? 0x80 : rnd() & 255;
    w.g = w.neutral && w.kind == 3 ? 0x80 : rnd() & 255;
    w.b = w.neutral && w.kind == 3 ? 0x80 : rnd() & 255;
    return w;
}

int bench() {
    Tables t;
    std::vector<uint16_t> v(1024 * 512 + AFTER);
    // texels as a sprite has them: runs of one value, a third of them the transparent one
    for (size_t i = 0; i < v.size();) {
        const uint16_t x = (uint16_t)(rnd() % 3 ? rnd() | 0x0101 : 0);
        for (size_t run = 1 + rnd() % 24; run && i < v.size(); run--) v[i++] = x;
    }
    // the palette (and what follows it): colours, entry 0 transparent, half of them semi-transparent
    for (size_t i = 500 * 1024; i < 500 * 1024 + 256; i++) v[i] = (uint16_t)(i % 256 ? rnd() | 1 : 0);
    struct Case { const char *name; int kind, tp; bool sprite; int n; };
    const Case cases[] = {{"sprite, 8 bits, plain, 128", 1, 1, true, 128},  {"sprite, 4 bits, plain, 16", 1, 0, true, 16},
                          {"sprite, 4 bits, plain, 128", 1, 0, true, 128},  {"polygon, 8 bits, plain, 64", 1, 1, false, 64},
                          {"polygon, 8 bits, shaded, 64", 2, 1, false, 64}, {"polygon, 8 bits, mixed, 64", 3, 1, false, 64},
                          {"polygon, 4 bits, mixed, 64", 3, 0, false, 64}};
    for (const Case &c : cases) {
        double ns[2];
        for (int which = 0; which < 2; which++) {
            const auto t0 = std::chrono::steady_clock::now();
            uint64_t pixels = 0;
            for (int rep = 0; rep < 40; rep++)
                for (int y = 0; y < 256; y++) {
                    Row w{c.kind, false, c.tp, c.sprite, c.sprite ? 0u : 0u, c.sprite ? 1 : 0x10000, c.n,
                          v.data() + (256 + y) * 1024 + 100, v.data() + y * 1024 + 300, v.data() + 500 * 1024,
                          false, 0x70, 0x90, 0x80};
                    Shade sh;
                    for (int l = 0; l < 32; l++) {
                        sh.r[l] = t.redshade[(l << 8) | w.r];
                        sh.g[l] = t.greenshade[(l << 8) | w.g];
                        sh.b[l] = t.blueshade[(l << 8) | w.b];
                    }
                    for (int x = 0; x + c.n <= 768; x += c.n) {
                        w.d = v.data() + (256 + y) * 1024 + x;
                        which ? hiraku(w, t, &sh) : mame(w, t);
                        pixels += (uint64_t)c.n;
                    }
                }
            ns[which] = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / (double)pixels;
        }
        printf("%-30s  the loop as in psx.cpp %.2f ns a pixel, the row %.2f\n", c.name, ns[0], ns[1]);
    }
    return 0;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--bench")) return bench();
    const int N = argc > 1 ? atoi(argv[1]) : 600000;
    Tables t;
    std::vector<uint16_t> a(ROWS * WIDTH + AFTER), b;
    for (uint16_t &x : a) x = (uint16_t)rnd();
    uint64_t wrong = 0, meets = 0, by_kind[4] = {};
    for (int i = 0; i < N; i++) {
        // fresh pixels here and there; many zeros, which are not drawn
        for (int q = 0; q < ROWS * WIDTH; q += 1 + (int)(rnd() % 7)) a[(size_t)q] = (uint16_t)(rnd() % 3 ? rnd() : 0);
        b = a;
        Row w = random_row(a.data());
        mame(w, t);
        const uint16_t *lo = w.d, *hi = w.d + w.n;
        if ((w.clut < hi && w.clut + 256 > lo) || (w.row < hi && w.row + 256 > lo)) meets++;
        w.d = b.data() + (w.d - a.data());
        w.row = b.data() + (w.row - a.data());
        w.clut = b.data() + (w.clut - a.data());
        hiraku(w, t);
        by_kind[w.kind]++;
        if (a != b) {
            if (wrong++ < 5)
                fprintf(stderr, "row %d differs: kind %d depth %d %s u %08x step %d, %d pixels\n", i, w.kind, w.tp,
                        w.sprite ? "sprite" : "polygon", (unsigned)w.u, (int)w.du, w.n);
            a = b;
        }
    }
    printf("%d rows (%llu plain, %llu shaded, %llu semi-transparent; %llu met their own texture or palette): %llu differ\n", N,
           (unsigned long long)by_kind[1], (unsigned long long)by_kind[2], (unsigned long long)by_kind[3],
           (unsigned long long)meets, (unsigned long long)wrong);
    return wrong ? 1 : 0;
}
