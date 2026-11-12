// license:BSD-3-Clause
// copyright-holders:Ville Linde
//
// The GCU's rows (k057714.cpp): a run of pixels copied, blended or filled at once, each with
// exactly the result of the pixel-by-pixel loop it stands for. Apart from the chip so that
// tests/gcu/test_gcu_rows.cpp can hold them against those loops.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#if defined(__SSE2__)
#include <emmintrin.h>
#elif defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace hk::gcu_rows {

// Each covers a run of pixels that neither wraps
// around VRAM nor needs clipping (the callers check), and gives exactly the
// result of the per-pixel loop it replaces.
//
// Pixel n lives at 16-bit index n ^ 1. For an even n and an even count, pixels
// n..n+count-1 occupy indices n..n+count-1 in some order; two such runs with
// the same parity therefore map index for index, and can be processed as
// plain arrays.

// Copy pixels s.. to d.. (n of them), skipping those whose bit 15 is not `want`
// when `trans` is set.
inline void blit_row(uint16_t *v, uint32_t s, uint32_t d, int n, bool trans, uint16_t want) {
    auto one = [&](uint32_t si, uint32_t di) {
        uint16_t p = v[si ^ 1u];
        if (!trans || (p & 0x8000) == want) v[di ^ 1u] = p;
    };
    // Source and destination overlapping (the copy order matters): pixel by
    // pixel, in the original order.
    if (s <= d + n + 1 && d <= s + n + 1) {
        for (int k = 0; k < n; k++) one(s + k, d + k);
        return;
    }
    int k = 0;
    if ((s ^ d) & 1) {
        // Opposite parity. Align the destination: then destination pixels D, D+1
        // (indices D+1, D) come from source pixels S, S+1 (indices S-1, S+2).
        if (d & 1) one(s, d), k = 1;
        const int m = (n - k) & ~1;
        const uint16_t *__restrict src = v + s + k;
        uint16_t *__restrict dst = v + d + k;
        int i = 0;
        // Eight pixels at a time: the destination's pairs from src[i + 2], src[i - 1], src[i + 4],
        // src[i + 1]... (two loads, the even lanes of each), kept where bit 15 is `want`. The
        // second load reaches src[i + 9]: only while the pairs after these read that far anyway.
#if defined(__SSE2__)
        {
            const __m128i top = _mm_set1_epi16((short)0x8000), wanted = _mm_set1_epi16((short)want), even = _mm_set1_epi32(0xffff);
            for (; i + 10 <= m; i += 8) {
                const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i *>(src + i - 1));
                const __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i *>(src + i + 2));
                const __m128i x = _mm_or_si128(_mm_and_si128(b, even), _mm_slli_epi32(a, 16));
                __m128i keep = _mm_cmpeq_epi16(_mm_and_si128(x, top), wanted);
                if (!trans) keep = _mm_set1_epi16(-1);
                const __m128i old = _mm_loadu_si128(reinterpret_cast<const __m128i *>(dst + i));
                _mm_storeu_si128(reinterpret_cast<__m128i *>(dst + i), _mm_or_si128(_mm_and_si128(keep, x), _mm_andnot_si128(keep, old)));
            }
        }
#elif defined(__ARM_NEON)
        {
            const uint16x8_t top = vdupq_n_u16(0x8000), wanted = vdupq_n_u16(want);
            for (; i + 10 <= m; i += 8) {
                const uint32x4_t a = vreinterpretq_u32_u16(vld1q_u16(src + i - 1)), b = vreinterpretq_u32_u16(vld1q_u16(src + i + 2));
                const uint16x8_t x = vreinterpretq_u16_u32(vorrq_u32(vandq_u32(b, vdupq_n_u32(0xffff)), vshlq_n_u32(a, 16)));
                uint16x8_t keep = vceqq_u16(vandq_u16(x, top), wanted);
                if (!trans) keep = vdupq_n_u16(0xffff);
                vst1q_u16(dst + i, vbslq_u16(keep, x, vld1q_u16(dst + i)));
            }
        }
#endif
        for (; i < m; i += 2) {
            uint16_t p0 = src[i - 1], p1 = src[i + 2];
            if (!trans || (p0 & 0x8000) == want) dst[i + 1] = p0;
            if (!trans || (p1 & 0x8000) == want) dst[i] = p1;
        }
        if (k + m < n) one(s + k + m, d + k + m);
        return;
    }
    if (s & 1) one(s, d), k = 1;
    const int m = (n - k) & ~1;
    const uint16_t *__restrict src = v + s + k;
    uint16_t *__restrict dst = v + d + k;
    if (!trans) {
        memcpy(dst, src, (size_t)m * 2);
    } else {
        int i = 0;
        // (eight pixels at a time, kept where bit 15 is `want`: the compiler makes a branch a pixel of it)
#if defined(__SSE2__)
        {
            const __m128i top = _mm_set1_epi16((short)0x8000), wanted = _mm_set1_epi16((short)want);
            for (; i + 8 <= m; i += 8) {
                const __m128i x = _mm_loadu_si128(reinterpret_cast<const __m128i *>(src + i));
                const __m128i keep = _mm_cmpeq_epi16(_mm_and_si128(x, top), wanted);
                const __m128i old = _mm_loadu_si128(reinterpret_cast<const __m128i *>(dst + i));
                _mm_storeu_si128(reinterpret_cast<__m128i *>(dst + i), _mm_or_si128(_mm_and_si128(keep, x), _mm_andnot_si128(keep, old)));
            }
        }
#elif defined(__ARM_NEON)
        {
            const uint16x8_t top = vdupq_n_u16(0x8000), wanted = vdupq_n_u16(want);
            for (; i + 8 <= m; i += 8) {
                const uint16x8_t x = vld1q_u16(src + i);
                vst1q_u16(dst + i, vbslq_u16(vceqq_u16(vandq_u16(x, top), wanted), x, vld1q_u16(dst + i)));
            }
        }
#endif
        for (; i < m; i++) {
            uint16_t p = src[i];
            dst[i] = (p & 0x8000) == want ? p : dst[i];
        }
    }
    if (k + m < n) one(s + k + m, d + k + m);
}

// Blend pixels s.. over pixels d.. (n of them), as draw_object's blend modes 1 and 2 do a pixel: of
// those kept (all, or with `trans` those whose bit 15 is `want`), each channel becomes
// min((old * a2 + new * a1) >> 4, 31), and bit 15 is the new pixel's.
inline void blend_row(uint16_t *v, uint32_t s, uint32_t d, int n, bool trans, uint16_t want, uint32_t a1, uint32_t a2) {
    auto one = [&](uint32_t si, uint32_t di) {
        const uint16_t p = v[si ^ 1u];
        if (trans && (p & 0x8000) != want) return;
        const uint16_t o = v[di ^ 1u];
        const uint32_t r = std::min((((o >> 10) & 0x1fu) * a2 + ((p >> 10) & 0x1fu) * a1) >> 4, 0x1fu);
        const uint32_t g = std::min((((o >> 5) & 0x1fu) * a2 + ((p >> 5) & 0x1fu) * a1) >> 4, 0x1fu);
        const uint32_t b = std::min(((o & 0x1fu) * a2 + (p & 0x1fu) * a1) >> 4, 0x1fu);
        v[di ^ 1u] = (uint16_t)((r << 10) | (g << 5) | b | (p & 0x8000));
    };
    // Source and destination overlapping (the order matters): pixel by pixel, in the original order.
    if (s <= d + n + 1 && d <= s + n + 1) {
        for (int k = 0; k < n; k++) one(s + k, d + k);
        return;
    }
    // From an even destination pixel on, eight at a time. Destination index i holds pixel i ^ 1; its
    // source is index i of the source where the two start at the same parity, else (as in
    // blit_row) src[i + 2] for an even i and src[i - 1] for an odd one.
    int k = 0;
    if (d & 1) one(s, d), k = 1;
    const int m = (n - k) & ~1;
    const bool opposite = (s ^ d) & 1;
    const uint16_t *__restrict src = v + s + k;
    uint16_t *__restrict dst = v + d + k;
    int i = 0;
#if defined(__SSE2__)
    {
        const __m128i top = _mm_set1_epi16((short)0x8000), wanted = _mm_set1_epi16((short)want), five = _mm_set1_epi16(0x1f);
        const __m128i va1 = _mm_set1_epi16((short)a1), va2 = _mm_set1_epi16((short)a2), even = _mm_set1_epi32(0xffff);
        for (; i + (opposite ? 10 : 8) <= m; i += 8) {
            __m128i x;
            if (opposite) {
                const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i *>(src + i - 1));
                const __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i *>(src + i + 2));
                x = _mm_or_si128(_mm_and_si128(b, even), _mm_slli_epi32(a, 16));
            } else {
                x = _mm_loadu_si128(reinterpret_cast<const __m128i *>(src + i));
            }
            const __m128i old = _mm_loadu_si128(reinterpret_cast<const __m128i *>(dst + i));
            auto channel = [&](int shift) {   // (sums up to 31 * 31 * 2: they fit a lane)
                const __m128i o = _mm_and_si128(_mm_srl_epi16(old, _mm_cvtsi32_si128(shift)), five);
                const __m128i c = _mm_and_si128(_mm_srl_epi16(x, _mm_cvtsi32_si128(shift)), five);
                const __m128i sum = _mm_add_epi16(_mm_mullo_epi16(o, va2), _mm_mullo_epi16(c, va1));
                return _mm_sll_epi16(_mm_min_epi16(_mm_srli_epi16(sum, 4), five), _mm_cvtsi32_si128(shift));
            };
            const __m128i res = _mm_or_si128(_mm_or_si128(channel(10), channel(5)), _mm_or_si128(channel(0), _mm_and_si128(x, top)));
            __m128i keep = _mm_cmpeq_epi16(_mm_and_si128(x, top), wanted);
            if (!trans) keep = _mm_set1_epi16(-1);
            _mm_storeu_si128(reinterpret_cast<__m128i *>(dst + i), _mm_or_si128(_mm_and_si128(keep, res), _mm_andnot_si128(keep, old)));
        }
    }
#elif defined(__ARM_NEON)
    {
        const uint16x8_t top = vdupq_n_u16(0x8000), wanted = vdupq_n_u16(want), five = vdupq_n_u16(0x1f);
        const uint16x8_t va1 = vdupq_n_u16((uint16_t)a1), va2 = vdupq_n_u16((uint16_t)a2);
        for (; i + (opposite ? 10 : 8) <= m; i += 8) {
            uint16x8_t x;
            if (opposite) {
                const uint32x4_t a = vreinterpretq_u32_u16(vld1q_u16(src + i - 1)), b = vreinterpretq_u32_u16(vld1q_u16(src + i + 2));
                x = vreinterpretq_u16_u32(vorrq_u32(vandq_u32(b, vdupq_n_u32(0xffff)), vshlq_n_u32(a, 16)));
            } else {
                x = vld1q_u16(src + i);
            }
            const uint16x8_t old = vld1q_u16(dst + i);
            auto channel = [&](int shift) {
                const int16x8_t down = vdupq_n_s16((int16_t)-shift), up = vdupq_n_s16((int16_t)shift);
                const uint16x8_t o = vandq_u16(vshlq_u16(old, down), five), c = vandq_u16(vshlq_u16(x, down), five);
                const uint16x8_t sum = vaddq_u16(vmulq_u16(o, va2), vmulq_u16(c, va1));
                return vshlq_u16(vminq_u16(vshrq_n_u16(sum, 4), five), up);
            };
            const uint16x8_t res = vorrq_u16(vorrq_u16(channel(10), channel(5)), vorrq_u16(channel(0), vandq_u16(x, top)));
            uint16x8_t keep = vceqq_u16(vandq_u16(x, top), wanted);
            if (!trans) keep = vdupq_n_u16(0xffff);
            vst1q_u16(dst + i, vbslq_u16(keep, res, old));
        }
    }
#endif
    for (; i < m; i++) one(s + k + i, d + k + i);
    if (k + m < n) one(s + k + m, d + k + m);
}

// Fill pixels d.. (n of them) with the 4-pixel pattern color[pixel & 3].
inline void fill_row(uint16_t *v, uint32_t d, int n, const uint16_t color[4]) {
    int k = 0;
    if (d & 1) v[d ^ 1u] = color[d & 3], k = 1;
    const int m = (n - k) & ~1;
    // index i holds pixel i ^ 1
    const uint16_t pat[4] = {color[1], color[0], color[3], color[2]};
    uint16_t *dst = v + d + k;
    const uint32_t i0 = d + k;
    // four pixels at a time: the pattern from where the row starts in it
    uint16_t four[4];
    for (uint32_t q = 0; q < 4; q++) four[q] = pat[(i0 + q) & 3];
    int i = 0;
    for (; i + 4 <= m; i += 4) memcpy(dst + i, four, 8);
    for (; i < m; i++) dst[i] = pat[(i0 + i) & 3];
    if (k + m < n) v[(d + k + m) ^ 1u] = color[(d + k + m) & 3];
}

}  // namespace hk::gcu_rows
