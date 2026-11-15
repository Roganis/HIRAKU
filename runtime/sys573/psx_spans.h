// SPDX-License-Identifier: GPL-3.0-or-later
// The PlayStation GPU's textured rows (MAME's psx.cpp, TEXTUREFILL): the ones a game draws most,
// each with exactly the pixels of the pixel-by-pixel loop it stands for.
//
// MAME's loop serves every primitive: for each pixel it steps u and v, finds the texture's row
// again, and asks whether the texture is interleaved, windowed, which depth it has, whether the
// shade is neutral, whether the mask bit is tested and set. Counted over a stage, three quarters
// to nine tenths of the pixels a System 573 or Twinkle game draws are in rows where none of that
// changes along the row: a sprite's, or a polygon's whose texture is not turned (v the same for
// the whole row), no texture window, no interleave, no mask test. For those the texels are bytes
// (or nibbles, or words) read one after another along one row of VRAM.
//
// Exactness: the pixels are written in MAME's order, each texel and palette entry read when its
// pixel is drawn, so a row that draws over its own texture or palette gives what MAME's gives.
// u is kept as MAME keeps it, a fixed-point number whose top bits are the texel: 16.16 for a
// polygon (n_u.d, stepping n_du), 8.24 for a sprite (its u is a byte that wraps at 256).
// tests/sys573/test_psx_spans.cpp holds each loop against MAME's expressions, and
// HK_GPU_SPANS=check draws every such row of a running game both ways and compares.
#pragma once

#include <bit>
#include <cstdint>

namespace hk::psxspan {

static_assert(std::endian::native == std::endian::little, "texels are read as bytes of VRAM's words");

// The three shade tables' rows for one colour: what MAME reads at [ level << 8 | shade ].
struct Shade {
    uint16_t r[32], g[32], b[32];
    uint16_t of(uint16_t c) const { return (uint16_t)(r[c & 31] | g[(c >> 5) & 31] | b[(c >> 10) & 31] | (c & 0x8000)); }
};

// What MAME's SEMITRANSPARENT pixel reads, as the routine was set up (TRANSPARENCYSETUP): the
// front colour's weight, the three tables that give the picture's colour its place in the index,
// and the three that add or subtract.
struct Blend {
    const uint16_t *f, *redb, *greenb, *blueb, *redtrans, *greentrans, *bluetrans;
};

// The texel at u of a row: depth 0 = 4 bits, 1 = 8 bits (through the palette), 2 = 15 bits.
template <int DEPTH>
inline uint16_t texel(const uint16_t *row, uint32_t u, const uint16_t *clut) {
    if (DEPTH == 0) return clut[(reinterpret_cast<const uint8_t *>(row)[u >> 1] >> ((u & 1) << 2)) & 15];
    if (DEPTH == 1) return clut[reinterpret_cast<const uint8_t *>(row)[u]];
    return row[u];
}

// n pixels at d; texel = u >> SHIFT of `row` (the texture's row: p_p_vram[ n_ty + v ] + n_tx),
// u stepping du; a texel of 0 is not drawn; `stp` (0 or 0x8000) is set in what is drawn.
// Unshaded: the texel as it is.
template <int DEPTH, int SHIFT>
inline void plain(uint16_t *d, int n, const uint16_t *row, uint32_t u, uint32_t du, const uint16_t *clut, uint16_t stp) {
    for (int i = 0; i < n; i++, u += du) {
        const uint16_t c = texel<DEPTH>(row, u >> SHIFT, clut);
        if (c) d[i] = c | stp;
    }
}

// Shaded by one colour.
template <int DEPTH, int SHIFT>
inline void shaded(uint16_t *d, int n, const uint16_t *row, uint32_t u, uint32_t du, const uint16_t *clut, uint16_t stp,
                   const Shade &sh) {
    for (int i = 0; i < n; i++, u += du) {
        const uint16_t c = texel<DEPTH>(row, u >> SHIFT, clut);
        if (c) d[i] = sh.of(c) | stp;
    }
}

// Semi-transparency on: a texel with bit 15 is mixed with the picture, another is drawn as above
// (but without its bit 15, which is 0). `sh` null: unshaded.
template <int DEPTH, int SHIFT>
inline void mixed(uint16_t *d, int n, const uint16_t *row, uint32_t u, uint32_t du, const uint16_t *clut, uint16_t stp,
                  const Shade *sh, const Blend &bl, unsigned r, unsigned g, unsigned b) {
    for (int i = 0; i < n; i++, u += du) {
        const uint16_t c = texel<DEPTH>(row, u >> SHIFT, clut);
        if (!c) continue;
        if (c & 0x8000) {
            const uint16_t back = d[i];
            d[i] = (uint16_t)(bl.redtrans[bl.f[((c & 31) << 8) | r] | bl.redb[back]] |
                              bl.greentrans[bl.f[(((c >> 5) & 31) << 8) | g] | bl.greenb[back]] |
                              bl.bluetrans[bl.f[(((c >> 10) & 31) << 8) | b] | bl.blueb[back]] | 0x8000 | stp);
        } else
            d[i] = (sh ? sh->of(c) : c) | stp;
    }
}

// The row, by the texture's depth (n_tp: 0, 1 or 2).
template <int SHIFT>
inline void row_plain(int tp, uint16_t *d, int n, const uint16_t *row, uint32_t u, uint32_t du, const uint16_t *clut, uint16_t stp) {
    if (tp == 0) plain<0, SHIFT>(d, n, row, u, du, clut, stp);
    else if (tp == 1) plain<1, SHIFT>(d, n, row, u, du, clut, stp);
    else plain<2, SHIFT>(d, n, row, u, du, clut, stp);
}
template <int SHIFT>
inline void row_shaded(int tp, uint16_t *d, int n, const uint16_t *row, uint32_t u, uint32_t du, const uint16_t *clut, uint16_t stp,
                       const Shade &sh) {
    if (tp == 0) shaded<0, SHIFT>(d, n, row, u, du, clut, stp, sh);
    else if (tp == 1) shaded<1, SHIFT>(d, n, row, u, du, clut, stp, sh);
    else shaded<2, SHIFT>(d, n, row, u, du, clut, stp, sh);
}
template <int SHIFT>
inline void row_mixed(int tp, uint16_t *d, int n, const uint16_t *row, uint32_t u, uint32_t du, const uint16_t *clut, uint16_t stp,
                      const Shade *sh, const Blend &bl, unsigned r, unsigned g, unsigned b) {
    if (tp == 0) mixed<0, SHIFT>(d, n, row, u, du, clut, stp, sh, bl, r, g, b);
    else if (tp == 1) mixed<1, SHIFT>(d, n, row, u, du, clut, stp, sh, bl, r, g, b);
    else mixed<2, SHIFT>(d, n, row, u, du, clut, stp, sh, bl, r, g, b);
}

}  // namespace hk::psxspan
