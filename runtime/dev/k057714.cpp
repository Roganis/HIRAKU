// license:BSD-3-Clause
// copyright-holders:Ville Linde
//
// Konami 0000057714 "GCU" 2D graphics chip, ported from MAME's k057714.cpp.
// Modified for HIRAKU: no MAME device framework; pixels are addressed through
// pix()/set_pix() instead of an endian-swizzled uint16_t view; output is
// composed into a 32-bit RGB buffer.
#include "k057714.h"
#include "k057714_rows.h"
#include "hiraku/subpixel_mix.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#if defined(__SSE2__)
#include <emmintrin.h>
#elif defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace hk {

K057714::K057714() : m_vram(new uint32_t[VRAM_SIZE / 4]) {
    m_scroll.follow = hk::ScrollGuess::follow_env(true);   // in-between pictures: notes along the scroll's line
    reset();
}

// VRAM is an array of 32-bit words; 16-bit pixel n is the high half of word
// n/2 when n is even (the chip is big-endian). Viewed as 16-bit values on a
// little-endian host that is index n ^ 1 - MAME's NATIVE_ENDIAN_VALUE_LE_BE(1,0).
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "the VRAM pixel view assumes a little-endian host");
#define V16 (reinterpret_cast<uint16_t *>(m_vram.get()))
#define VIDX(n) (((n) ^ 1u) & 0xffffffu)
inline uint16_t K057714::pix(uint32_t n) const { return V16[VIDX(n)]; }
inline void K057714::set_pix(uint32_t n, uint16_t v) { V16[VIDX(n)] = v; }

using namespace gcu_rows;   // blit_row, blend_row, fill_row (k057714_rows.h)

void K057714::reset() {
    // beatmania III relies on the 640x480 default (it never programs the CRTC)
    m_display_h_visarea = 640; m_display_h_frontporch = 16; m_display_h_backporch = 48; m_display_h_syncpulse = 96;
    m_display_v_visarea = 480; m_display_v_frontporch = 10; m_display_v_backporch = 33; m_display_v_syncpulse = 2;
    m_pixclock = 25'175'000.0;
    m_vram_read_addr = 0;
    m_command_fifo0_ptr = m_command_fifo1_ptr = 0;
    m_vram_fifo0_addr = m_vram_fifo1_addr = 0;
    m_fb_origin_x = m_fb_origin_y = 0;
    m_reg_6c = 0;
    for (auto &f : m_frame) f = Frame{0, 0, 0, 0, 0, (uint16_t)((16 << 7) | (16 << 2))};
    memset(m_vram.get(), 0, VRAM_SIZE);
}

int K057714::htotal() const { return m_display_h_visarea + m_display_h_frontporch + m_display_h_backporch + m_display_h_syncpulse; }
int K057714::vtotal() const { return m_display_v_visarea + m_display_v_frontporch + m_display_v_backporch + m_display_v_syncpulse; }

uint32_t K057714::read(uint32_t reg) {
    if (reg >= 0x80 && reg < 0x100) return m_vram[(m_vram_read_addr + (reg >> 2) - 0x20) & (VRAM_SIZE / 4 - 1)];
    if (reg == 0x78) return 0xffff0005;   // GCU status (constant in MAME)
    return 0xffffffff;
}

void K057714::write(uint32_t reg, uint32_t data, uint32_t mem_mask) {
    const bool hi = mem_mask & 0xffff0000u, lo = mem_mask & 0x0000ffffu;
    switch (reg) {
    case 0x00:
        if (hi) m_display_h_visarea = ((data >> 16) & 0xffff) + 1;
        if (lo) { m_display_h_frontporch = ((data >> 8) & 0xff) + 1; m_display_h_backporch = (data & 0xff) + 1; }
        break;
    case 0x04:
        if (hi) m_display_v_visarea = ((data >> 16) & 0xffff) + 1;
        if (lo) { m_display_v_frontporch = ((data >> 8) & 0xff) + 1; m_display_v_backporch = (data & 0xff) + 1; }
        break;
    case 0x08:
        if (hi) { m_display_h_syncpulse = ((data >> 24) & 0xff) + 1; m_display_v_syncpulse = ((data >> 16) & 0xff) + 1; }
        break;
    case 0x10:
        // IRQ clear/enable: games write the bit off then on in response to the interrupt
        if (hi) {
            uint16_t d = data >> 16;
            if (!(d & 1) && irq) irq(false);
            m_irqctrl = d;
        }
        if (lo) m_layer_select = data & 0xffff;
        break;
    case 0x14:
        if (hi) m_frame[0].alpha = (data >> 16) & 0xffff;
        if (lo) m_frame[1].alpha = data & 0xffff;
        break;
    case 0x18:
        if (hi) m_frame[2].alpha = (data >> 16) & 0xffff;
        if (lo) m_frame[3].alpha = data & 0xffff;
        break;
    case 0x1c:
        if ((data >> 16) == 1) { m_ext_fifo_count = 0; m_ext_fifo_line = 0; }
        break;
    case 0x20: case 0x24: case 0x28: case 0x2c: {
        Frame &f = m_frame[(reg - 0x20) / 4];
        if (hi) f.y = (data >> 16) & 0xffff;
        if (lo) f.x = data & 0xffff;
        break;
    }
    case 0x30: case 0x34: case 0x38: case 0x3c: {
        Frame &f = m_frame[(reg - 0x30) / 4];
        if (hi) f.height = (data >> 16) & 0xffff;
        if (lo) f.width = data & 0xffff;
        break;
    }
    case 0x40: case 0x44: case 0x48: case 0x4c:
        m_frame[(reg - 0x40) / 4].base = data;
        break;
    case 0x54:
        if (hi) m_ext_fifo_num_lines = data >> 16;
        if (lo) m_ext_fifo_width = data & 0xffff;
        break;
    case 0x58: m_ext_fifo_addr = data & 0xffffff; break;
    case 0x5c: m_vram_read_addr = (data & 0xffffff) / 2; break;
    case 0x60: m_vram_fifo0_addr = (data & 0xffffff) / 2; break;
    case 0x64: m_vram_fifo1_addr = (data & 0xffffff) / 2; break;
    case 0x68:
        if (hi) m_vram_fifo0_mode = data >> 16;
        if (lo) m_vram_fifo1_mode = data & 0xffff;
        break;
    case 0x70:
        if (m_vram_fifo0_mode & 0x100) {
            m_command_fifo0[m_command_fifo0_ptr++] = data;
            if (m_command_fifo0_ptr >= 4) { execute_command(m_command_fifo0); m_command_fifo0_ptr = 0; }
        } else {
            m_vram[m_vram_fifo0_addr++ & (VRAM_SIZE / 4 - 1)] = data;
        }
        break;
    case 0x74:
        if (m_vram_fifo1_mode & 0x100) {
            m_command_fifo1[m_command_fifo1_ptr++] = data;
            if (m_command_fifo1_ptr >= 4) { execute_command(m_command_fifo1); m_command_fifo1_ptr = 0; }
        } else {
            m_vram[m_vram_fifo1_addr++ & (VRAM_SIZE / 4 - 1)] = data;
        }
        break;
    case 0x6c:
        if (lo) m_reg_6c = data & 0xffff;
        break;
    default:
        break;
    }
}

void K057714::vblank(bool state) {
    if (state && m_keep && !m_drawing.cmds.empty()) {   // a new frame (in-between pictures)
        m_shown_before = std::move(m_shown);
        m_shown = std::move(m_drawing);
        m_drawing = Drawn{};
        m_drawing.origin_x = m_fb_origin_x, m_drawing.origin_y = m_fb_origin_y;
        m_mark++;
    }
    if (state && (m_irqctrl & 1) && irq) irq(true);
}

void K057714::draw_frame(int frame, uint16_t *bitmap, int bw, int bh, bool inverse_trans, const uint8_t *rows) {
    const Frame &f = m_frame[frame];
    if (f.height == 0 || f.width == 0) return;
    int height = f.height + 1, width = f.width + 1;
    int alpha = f.alpha, blend_mode = alpha & 3;
    int alpha1 = (alpha >> 7) & 0x1f;   // beatmania III uses this for blend mode 1
    int alpha2 = (alpha >> 2) & 0x1f;   // pop'n music has alpha 1 and 2 the same for blend mode 1
    if (blend_mode == 2 && alpha1) alpha1 = (alpha2 * 16) / alpha1;
    const uint16_t trans_value = inverse_trans ? 0x8000 : 0x0000;
    const int max_y = bh - 1, max_x = bw - 1;
    if (f.y + height > max_y) height = max_y - f.y;
    if (f.x + width > max_x) width = max_x - f.x;
    // channel scale table: same (c * alpha1) >> 4 clamped to 0x1f as MAME, per 5-bit value
    uint16_t lut_r[32], lut_g[32], lut_b[32];
    for (int v = 0; v < 32; v++) {
        int sc = std::min((v * alpha1) >> 4, 0x1f);
        lut_r[v] = (uint16_t)(sc << 10);
        lut_g[v] = (uint16_t)(sc << 5);
        lut_b[v] = (uint16_t)sc;
    }
    const uint16_t *v = V16;
    const bool identity = alpha1 == 16;   // full brightness: colours pass unchanged
    for (int j = 0; j <= height; j++) {
        int y = j + f.y;
        if (y < 0 || y >= bh || (rows && !rows[y])) continue;
        uint16_t *d = bitmap + y * bw + f.x;
        uint32_t base = f.base + (uint32_t)j * 1024;
        int iend = std::min(width, bw - 1 - f.x);
        if (iend >= 0 && base + (uint32_t)iend < VRAM_SIZE_HALF - 1) {
            // no wrap: read the pixels in pairs (pixel n is index n ^ 1)
            const uint32_t b0 = base & ~1u;
            const int off = (int)(base & 1), n = iend + 1;
            const uint16_t *__restrict row = v + b0;
            if (identity) {
                int i = 0;
#if defined(__SSE2__) || defined(__ARM_NEON)
                // From an even pixel on, 8 pixels at a time: swap each pair of
                // 16-bit lanes back into pixel order, keep the opaque ones.
                if (off && n > 0) {
                    uint16_t p = row[0];   // pixel base (odd) is index base ^ 1 = b0
                    if ((p & 0x8000) != trans_value) d[0] = (uint16_t)(p & 0x7fff);
                    i = 1;
                }
#endif
#if defined(__SSE2__)
                const __m128i low15 = _mm_set1_epi16(0x7fff);
                for (; i + 8 <= n; i += 8) {
                    // pixel base + i is even: pixels base+i .. base+i+7 are the 8 halfwords from there, pairs swapped
                    __m128i x = _mm_loadu_si128(reinterpret_cast<const __m128i *>(row + off + i));
                    x = _mm_shufflehi_epi16(_mm_shufflelo_epi16(x, 0xb1), 0xb1);
                    __m128i opaque = _mm_srai_epi16(x, 15);   // bit 15 set
                    if (trans_value) opaque = _mm_xor_si128(opaque, _mm_set1_epi16(-1));
                    __m128i old = _mm_loadu_si128(reinterpret_cast<const __m128i *>(d + i));
                    __m128i res = _mm_or_si128(_mm_and_si128(opaque, _mm_and_si128(x, low15)), _mm_andnot_si128(opaque, old));
                    _mm_storeu_si128(reinterpret_cast<__m128i *>(d + i), res);
                }
#elif defined(__ARM_NEON)
                const uint16x8_t low15 = vdupq_n_u16(0x7fff);
                for (; i + 8 <= n; i += 8) {
                    const uint16x8_t x = vrev32q_u16(vld1q_u16(row + off + i));
                    uint16x8_t opaque = vreinterpretq_u16_s16(vshrq_n_s16(vreinterpretq_s16_u16(x), 15));
                    if (trans_value) opaque = vmvnq_u16(opaque);
                    vst1q_u16(d + i, vbslq_u16(opaque, vandq_u16(x, low15), vld1q_u16(d + i)));
                }
#endif
                for (; i < n; i++) {
                    uint16_t p = row[(uint32_t)(i + off) ^ 1u];
                    if ((p & 0x8000) != trans_value) d[i] = (uint16_t)(p & 0x7fff);
                }
            } else {
                for (int i = 0; i < n; i++) {
                    uint16_t p = row[(uint32_t)(i + off) ^ 1u];
                    if ((p & 0x8000) != trans_value)
                        d[i] = (uint16_t)(lut_r[(p >> 10) & 0x1f] | lut_g[(p >> 5) & 0x1f] | lut_b[p & 0x1f]);
                }
            }
            continue;
        }
        for (int i = 0; i <= iend; i++) {
            uint16_t p = v[VIDX(base + (uint32_t)i)];
            if ((p & 0x8000) == trans_value) continue;
            d[i] = identity ? (uint16_t)(p & 0x7fff) : (uint16_t)(lut_r[(p >> 10) & 0x1f] | lut_g[(p >> 5) & 0x1f] | lut_b[p & 0x1f]);
        }
    }
}

void K057714::draw(uint32_t *out, int w, int h, const uint8_t *rows) {
    static thread_local std::unique_ptr<uint16_t[]> bmp;
    static thread_local int bmp_size = 0;
    if (bmp_size < w * h) { bmp.reset(new uint16_t[w * h]); bmp_size = w * h; }
    if (!rows) {
        std::fill(bmp.get(), bmp.get() + w * h, 0);
    } else {
        for (int y = 0; y < h; y++)
            if (rows[y]) std::fill(bmp.get() + (size_t)y * w, bmp.get() + (size_t)(y + 1) * w, 0);
    }
    // most likely wrong, inverse transparency is only used by kbm;
    // beatmania III sets reg 6c to 0xfff but doesn't use inverse transparency
    bool inverse_trans = (m_reg_6c & 0xf) != 0 && m_reg_6c != 0xfff;
    draw_frame((m_layer_select >> 8) & 3, bmp.get(), w, h, inverse_trans, rows);
    draw_frame((m_layer_select >> 10) & 3, bmp.get(), w, h, inverse_trans, rows);
    draw_frame((m_layer_select >> 12) & 3, bmp.get(), w, h, inverse_trans, rows);
    draw_frame((m_layer_select >> 14) & 3, bmp.get(), w, h, inverse_trans, rows);
    // RGB555 -> xRGB8888, the same pal5bit expansion as MAME (as arithmetic, which vectorises)
    for (int y = 0; y < h; y++) {
        if (rows && !rows[y]) continue;
        const uint16_t *src = bmp.get() + (size_t)y * w;
        uint32_t *o = out + (size_t)y * w;
        for (int k = 0; k < w; k++) {
            uint32_t p = src[k], r = (p >> 10) & 0x1f, g = (p >> 5) & 0x1f, b = p & 0x1f;
            o[k] = 0xff000000u | ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (b << 3 | b >> 2);
        }
    }
}

void K057714::draw_object(const uint32_t *cmd) {
    uint32_t address_x = cmd[0] & 0x3ff;
    uint32_t address_y = (cmd[0] >> 10) & 0x3fff;
    bool relative_coords = cmd[0] & 0x10000000;
    int x = cmd[1] & 0x3ff;
    int y = (cmd[1] >> 10) & 0x3fff;
    bool xflip = cmd[1] & 0x04000000, yflip = cmd[1] & 0x08000000;
    int blend_mode = (cmd[1] >> 28) & 3;
    bool trans_enable = cmd[1] & 0xc0000000;
    uint16_t trans_value = (cmd[1] & 0x80000000) ? 0x0000 : 0x8000;
    int width = (cmd[2] & 0x1ff) + 1;
    int xscale = ((cmd[2] >> 10) & 0x7ff) * (((cmd[2] >> 10) & 0x800) ? -1 : 1);
    int alpha1_1 = (cmd[2] >> 22) & 0x1f, alpha1_2 = (cmd[2] >> 27) & 0x1f;
    int height = (cmd[3] & 0x3ff) + 1;
    int yscale = ((cmd[3] >> 10) & 0x7ff) * (((cmd[3] >> 10) & 0x800) ? -1 : 1);
    int alpha2_1 = (cmd[3] >> 22) & 0x1f, alpha2_2 = (cmd[3] >> 27) & 0x1f;
    if (xscale == 0 || yscale == 0) return;
    if (xflip && (uint32_t)(4 - ((width - 1) % 4)) <= (address_x % 4)) address_x -= 4;   // pop'n music 8 @ 0x800b30d0
    if (yflip) y -= (((height * 64) - 1) / yscale) - (((height - 1) * 64) / yscale);    // pop'n music 8 @ 0x800b3140
    if (relative_coords) { x += m_fb_origin_x; y += m_fb_origin_y; }
    uint32_t address = (address_y << 10) | address_x;
    int orig_height = height;
    width = (((width * 65536) / xscale) * 64) / 65536;
    height = (((height * 65536) / yscale) * 64) / 65536;
    if (height <= 0 || width <= 0) return;
    touch(y, y + height, x, x + width);   // + 1 row: a row running past x 1023 goes on into the next
    // (a blended object shows what's under it: whoever drew that keeps the pixel, its fraction with it)
    if (m_owning && !blend_mode) own_object(address, x, y, width, height, orig_height, xscale, yscale, xflip, yflip, trans_enable, trans_value);
    if (xscale == 64 && yscale == 64 && !xflip && !yflip && !blend_mode) {
        // the common case, same result as the general loop below
        uint16_t *v = V16;
        for (int j = 0; j < height; j++) {
            if (row_out(j + y)) continue;   // in-between pictures: a row not redrawn
            uint32_t src = address + (uint32_t)j * 1024, dst = (uint32_t)((j + y) * 1024 + x);
            if (src + (uint32_t)width <= VRAM_SIZE_HALF && dst + (uint32_t)width <= VRAM_SIZE_HALF) {
                blit_row(v, src, dst, width, trans_enable, trans_value);
                continue;
            }
            for (int i = 0; i < width; i++, src++, dst++) {
                uint16_t p = v[VIDX(src)];
                if (trans_enable && (p & 0x8000) != trans_value) continue;
                if (dst < VRAM_SIZE_HALF) v[dst ^ 1u] = p;
            }
        }
        return;
    }
    int v = 0, xinc = xflip ? -1 : 1;
    uint16_t *vram = V16;
    for (int j = 0; j < height; j++) {
        if (row_out(j + y)) {   // in-between pictures: a row not redrawn
            v += yscale;
            continue;
        }
        uint32_t fbaddr = ((j + y) * 1024) + x;
        int index = yflip ? address + ((orig_height - 1 - (v >> 6)) * 1024) : address + ((v >> 6) * 1024);
        if (xflip) fbaddr += width - 1;
        // A row that stays inside VRAM on both sides (scales are positive here)
        // needs neither the wrap mask nor the bounds check: same loop without them.
        const uint32_t lo = xflip ? fbaddr - (uint32_t)(width - 1) : fbaddr;
        const int64_t last_src = (int64_t)index + (((int64_t)(width - 1) * xscale) >> 6);
        if (index >= 0 && last_src <= 0xffffff && lo <= fbaddr && (uint64_t)lo + (uint64_t)width <= VRAM_SIZE_HALF) {
            // the two commonest of these rows, with exactly the result of the loop below
            if (xscale == 64 && !xflip && (blend_mode == 1 || blend_mode == 2)) {   // blended, not scaled across
                blend_row(vram, (uint32_t)index, fbaddr, width, trans_enable, trans_value,
                          blend_mode == 1 ? alpha1_2 : alpha1_1, blend_mode == 1 ? alpha2_2 : alpha2_1);
                v += yscale;
                continue;
            }
            if (!blend_mode) {   // scaled or flipped, not blended: a pixel kept or left, without a branch
                const uint16_t all = trans_enable ? 0 : 0xffff;
                int u = 0;
                for (int i = 0; i < width; i++, fbaddr += xinc, u += xscale) {
                    const uint16_t p = vram[(uint32_t)(index + (u >> 6)) ^ 1u];
                    uint16_t &o = vram[fbaddr ^ 1u];
                    const uint16_t keep = (uint16_t)(all | (uint16_t)-(uint16_t)((p & 0x8000) == trans_value));
                    o = (uint16_t)((p & keep) | (o & ~keep));
                }
                v += yscale;
                continue;
            }
            int u = 0;
            for (int i = 0; i < width; i++, fbaddr += xinc, u += xscale) {
                uint16_t p = vram[(uint32_t)(index + (u >> 6)) ^ 1u];
                if (trans_enable && (p & 0x8000) != trans_value) continue;
                if (blend_mode) {
                    uint16_t s = vram[fbaddr ^ 1u];
                    uint32_t sr = (s >> 10) & 0x1f, sg = (s >> 5) & 0x1f, sb = s & 0x1f;
                    uint32_t r = (p >> 10) & 0x1f, g = (p >> 5) & 0x1f, b = p & 0x1f;
                    const uint32_t a2 = blend_mode == 1 ? alpha2_2 : alpha2_1, a1 = blend_mode == 1 ? alpha1_2 : alpha1_1;
                    if (blend_mode <= 2) {
                        sr = std::min((sr * a2 + r * a1) >> 4, 0x1fu);
                        sg = std::min((sg * a2 + g * a1) >> 4, 0x1fu);
                        sb = std::min((sb * a2 + b * a1) >> 4, 0x1fu);
                    }
                    p = (uint16_t)((sr << 10) | (sg << 5) | sb | (p & 0x8000));
                }
                vram[fbaddr ^ 1u] = p;
            }
            v += yscale;
            continue;
        }
        int u = 0;
        for (int i = 0; i < width; i++) {
            uint16_t p = pix((index + (u >> 6)) & 0xffffff);
            bool draw = !trans_enable || ((p & 0x8000) == trans_value);
            if (fbaddr < VRAM_SIZE_HALF && draw) {
                if (blend_mode) {
                    uint16_t s = pix(fbaddr);
                    uint32_t sr = (s >> 10) & 0x1f, sg = (s >> 5) & 0x1f, sb = s & 0x1f;
                    uint32_t r = (p >> 10) & 0x1f, g = (p >> 5) & 0x1f, b = p & 0x1f;
                    if (blend_mode == 1) {
                        sr = ((sr * alpha2_2) + (r * alpha1_2)) >> 4;
                        sg = ((sg * alpha2_2) + (g * alpha1_2)) >> 4;
                        sb = ((sb * alpha2_2) + (b * alpha1_2)) >> 4;
                    } else if (blend_mode == 2) {   // Keyboardmania glow effects
                        sr = ((sr * alpha2_1) + (r * alpha1_1)) >> 4;
                        sg = ((sg * alpha2_1) + (g * alpha1_1)) >> 4;
                        sb = ((sb * alpha2_1) + (b * alpha1_1)) >> 4;
                    }
                    sr = std::min(sr, 0x1fu); sg = std::min(sg, 0x1fu); sb = std::min(sb, 0x1fu);
                    p = (uint16_t)((sr << 10) | (sg << 5) | sb | (p & 0x8000));
                }
                set_pix(fbaddr, p);
            }
            fbaddr += xinc;
            u += xscale;
        }
        v += yscale;
    }
}

void K057714::fill_rect(const uint32_t *cmd) {
    int x = cmd[1] & 0x3ff, y = (cmd[1] >> 10) & 0x3fff;
    int width = (cmd[0] & 0x3ff) + 1, height = ((cmd[0] >> 10) & 0x3ff) + 1;
    if (cmd[0] & 0x10000000) { x += m_fb_origin_x; y += m_fb_origin_y; }
    uint16_t color[4] = {(uint16_t)(cmd[2] >> 16), (uint16_t)cmd[2], (uint16_t)(cmd[3] >> 16), (uint16_t)cmd[3]};
    touch(y, y + height, x, x + width);
    for (int j = y; j < y + height; j++) {
        if (row_out(j)) continue;   // in-between pictures: a row not redrawn
        if (m_owning) {
            const uint32_t a = (uint32_t)(j * 1024 + x) & 0xffffffu, b = (a + (uint32_t)width - 1) & 0xffffffu;
            if ((a >> 10) == (b >> 10) && b >= a) {
                if (int32_t *o = own_at(a)) std::fill(o, o + width, -2);
            } else {
                for (int i = x; i < x + width; i++)
                    if (int32_t *o = own_at((uint32_t)(j * 1024 + i) & 0xffffffu)) *o = -2;
            }
        }
        const uint32_t d = (uint32_t)(j * 1024 + x);
        if (d + (uint32_t)width <= VRAM_SIZE_HALF) {
            fill_row(V16, d, width, color);   // rows start at even pixels: d & 3 == x & 3
            continue;
        }
        for (int i = x; i < x + width; i++) set_pix((uint32_t)(j * 1024 + i), color[i & 3]);
    }
}

void K057714::draw_character(const uint32_t *cmd) {
    int x = cmd[1] & 0x3ff, y = (cmd[1] >> 10) & 0x3fff;
    uint32_t address = cmd[0] & 0xffffff;
    if (cmd[0] & 0x10000000) { x += m_fb_origin_x; y += m_fb_origin_y; }
    bool double_height = cmd[1] & 0x01000000, trans_enable = cmd[1] & 0x40000000;
    uint16_t color[4] = {(uint16_t)(cmd[2] >> 16), (uint16_t)cmd[2], (uint16_t)(cmd[3] >> 16), (uint16_t)cmd[3]};
    int height = double_height ? 16 : 8;
    touch(y, y + height, x, x + 8);
    for (int j = 0; j < height; j++) {
        if (row_out(y + j)) {   // in-between pictures: a row not redrawn
            address += 4;
            continue;
        }
        uint32_t fbaddr = (y + j) * 1024;
        uint16_t line = pix(address);
        address += 4;
        for (int i = 0; i < 8; i++) {
            int p = (line >> ((7 - i) * 2)) & 3;
            if (!trans_enable || (color[p] & 0x8000)) {
                set_pix(fbaddr + x + i, color[p]);
                if (m_owning)
                    if (int32_t *o = own_at((fbaddr + x + i) & 0xffffffu)) *o = -2;
            }
        }
    }
}

void K057714::fb_config(const uint32_t *cmd) {
    m_fb_origin_x = cmd[2] & 0x3ff;
    m_fb_origin_y = cmd[3] & 0x3fff;
}

// A list's command 1 runs another list and comes back. MAME sets no limit to how deep that goes:
// a list that runs itself (bad video RAM) never comes back there, and takes the stack with it.
// Here a list LIST_DEPTH deep runs no further one (in the regress runs no game's list runs another at all).
// A command is four words: one that starts in video RAM's last three isn't run (MAME reads past
// the end for the rest of it).
void K057714::execute_display_list(uint32_t addr, int depth) {
    static constexpr int LIST_DEPTH = 256;
    bool end = false;
    int counter = 0;
    addr /= 2;
    while (!end && counter < 0x1000 && addr + 4 <= VRAM_SIZE / 4) {
        const uint32_t *cmd = &m_vram[addr];
        addr += 4;
        switch ((cmd[0] >> 29) & 7) {
        case 0: break;
        case 1:
            if (depth + 1 < LIST_DEPTH) execute_display_list(cmd[0] & 0xffffff, depth + 1);
            break;
        case 2: end = true; break;
        default: run(cmd); break;   // 3-7
        }
        counter++;
    }
}

void K057714::execute_command(const uint32_t *cmd) {
    switch ((cmd[0] >> 29) & 7) {
    case 0: case 2: break;
    case 1: execute_display_list(cmd[0] & 0xffffff); break;
    default: run(cmd); break;
    }
}

void K057714::run(const uint32_t *cmd) {
    const bool keep = m_keep && !m_replaying;
    if (keep) m_drawing.cmds.push_back({cmd[0], cmd[1], cmd[2], cmd[3]});
    m_cmd_y0 = 1 << 30, m_cmd_y1 = -1;
    switch ((cmd[0] >> 29) & 7) {
    case 3: fb_config(cmd); break;
    case 4: fill_rect(cmd); break;
    case 5: draw_object(cmd); break;
    default: draw_character(cmd); break;   // 6, 7
    }
    if (keep) m_drawing.cmd_rows.emplace_back(m_cmd_y0, m_cmd_y1);
}

// ---- HIRAKU: in-between pictures --------------------------------------------------------
// A frame's drawing goes into rows of VRAM (1024 pixels, 512 words each). Before a command draws
// on a row for the first time in a frame, the row is kept as it was; in a replay, as it is now, to
// be put back after.
static constexpr int ROWS = (int)(K057714::VRAM_SIZE_HALF / 1024);

void K057714::touch(int y0, int y1, int x0, int x1) {
    if (!m_keep) return;
    if (m_row_mark.empty()) m_row_mark.assign(2 * ROWS, 0);
    m_cmd_y0 = std::min(m_cmd_y0, std::max(y0, 0)), m_cmd_y1 = std::max(m_cmd_y1, y1);
    if (!m_replaying) {   // the frame's columns (a row running past 1023 goes on at the next's start)
        if (x1 >= 1024 || x0 < 0) x0 = 0, x1 = 1023;
        m_drawing.x0 = std::min(m_drawing.x0, x0), m_drawing.x1 = std::max(m_drawing.x1, x1);
    }
    y0 = std::max(y0, 0);
    y1 = std::min(y1, y0 + ROWS - 1);
    for (int y = y0; y <= y1; y++) {
        const uint32_t r = (uint32_t)y & (ROWS - 1);
        const uint32_t *row = &m_vram[(size_t)r * 512];
        if (m_replaying) {   // the replay's span of it
            if (row_out((int)r) || m_row_mark[ROWS + r] == m_replay_mark) continue;
            m_row_mark[ROWS + r] = m_replay_mark;
            m_restore_rows.push_back(r);
            m_restore.insert(m_restore.end(), row + m_span0, row + m_span1);
        } else {
            if (m_row_mark[r] == m_mark) continue;
            m_row_mark[r] = m_mark;
            m_drawing.rows.push_back(r);
            m_drawing.before.insert(m_drawing.before.end(), row, row + 512);
        }
    }
}

// Between two pixels: which command draws each pixel of the rows drawn again, before it draws them
// (as the loops of draw_object() go: a copy reads its source's pixels, and passes on who drew them)
void K057714::own_object(uint32_t address, int x, int y, int width, int height, int orig_height, int xscale, int yscale,
                         bool xflip, bool yflip, bool trans_enable, uint16_t trans_value) {
    const int32_t mine = m_own_cmd;
    const bool frac = mine >= 0 && (size_t)mine < m_own_frac.size() && m_own_frac[(size_t)mine];
    const int xinc = xflip ? -1 : 1;
    const uint16_t *vram = V16;
    int v = 0;
    for (int j = 0; j < height; j++, v += yscale) {
        if (row_out(j + y)) continue;
        uint32_t fbaddr = (uint32_t)((j + y) * 1024 + x);
        const int index = yflip ? (int)address + ((orig_height - 1 - (v >> 6)) * 1024) : (int)address + ((v >> 6) * 1024);
        if (xflip) fbaddr += (uint32_t)(width - 1);
        // the row's pixels: on one VRAM row each side, as a rule (else pixel by pixel)
        const uint32_t d0 = fbaddr, d1 = fbaddr + (uint32_t)((width - 1) * xinc);
        const int64_t s0 = index, s1 = (int64_t)index + (((int64_t)(width - 1) * xscale) >> 6);
        if (xscale > 0 && d0 < VRAM_SIZE_HALF && d1 < VRAM_SIZE_HALF && (d0 >> 10) == (d1 >> 10) && s0 >= 0 &&
            s1 <= 0xffffff && (s0 >> 10) == (s1 >> 10)) {
            int32_t *drow = own_at(d0 & ~1023u);
            if (!drow) continue;   // a row not drawn again: nobody asks who drew it
            const int32_t *srow = frac ? nullptr : own_at((uint32_t)s0 & ~1023u);
            const uint32_t sb = (uint32_t)s0 & ~1023u;
            if (!srow && xscale == 64 && !xflip) {   // the common case: its own pixels, branch-free
                int32_t *o = drow + (d0 & 1023u);
                if (!trans_enable) {
                    std::fill(o, o + width, mine);
                    continue;
                }
                const uint32_t s = (uint32_t)s0;
                for (int i = 0; i < width; i++) {
                    const int32_t keep = -(int32_t)((vram[(s + (uint32_t)i) ^ 1u] & 0x8000) == trans_value);
                    o[i] = (mine & keep) | (o[i] & ~keep);
                }
                continue;
            }
            int u = 0;
            uint32_t d = d0 & 1023u;
            for (int i = 0; i < width; i++, d += (uint32_t)xinc, u += xscale) {
                const uint32_t src = (uint32_t)(index + (u >> 6));
                if (trans_enable && (vram[src ^ 1u] & 0x8000) != trans_value) continue;
                int32_t who = mine;
                if (srow && srow[src - sb] >= 0) who = srow[src - sb];
                drow[d] = who;
            }
            continue;
        }
        int u = 0;
        for (int i = 0; i < width; i++, fbaddr += (uint32_t)xinc, u += xscale) {
            if (fbaddr >= VRAM_SIZE_HALF) continue;
            const uint32_t src = (uint32_t)(index + (u >> 6)) & 0xffffffu;
            if (trans_enable && (pix(src) & 0x8000) != trans_value) continue;
            int32_t *o = own_at(fbaddr);
            if (!o) continue;
            int32_t who = mine;
            if (!frac)
                if (const int32_t *so = own_at(src); so && *so >= 0) who = *so;
            *o = who;
        }
    }
}

// Who drew each pixel of the picture draw() composes (the top layer's pixel shown wins). Only rows
// drawn again: elsewhere both pictures are the same, and nobody asks
void K057714::own_screen(std::vector<int32_t> &out, int w, int h) {
    out.assign((size_t)w * h, -1);
    const bool inverse_trans = (m_reg_6c & 0xf) != 0 && m_reg_6c != 0xfff;
    const uint16_t trans_value = inverse_trans ? 0x8000 : 0x0000;
    for (int sh = 8; sh <= 14; sh += 2) {
        const Frame &f = m_frame[(m_layer_select >> sh) & 3];
        if (f.height == 0 || f.width == 0) continue;
        int height = f.height + 1, width = f.width + 1;
        if (f.y + height > h - 1) height = h - 1 - f.y;
        if (f.x + width > w - 1) width = w - 1 - f.x;
        const int iend = std::min(width, w - 1 - f.x);
        for (int j = 0; j <= height; j++) {
            const int y = j + f.y;
            if (y < 0 || y >= h || iend < 0) continue;
            const uint32_t base = f.base + (uint32_t)j * 1024;
            const uint32_t first = base & 0xffffffu, last = (base + (uint32_t)iend) & 0xffffffu;
            if ((first >> 10) == (last >> 10) && last >= first && f.x >= 0) {   // one VRAM row: branch-free
                const int32_t *o = own_at(first);
                if (!o) continue;
                const uint16_t *v = V16;
                int32_t *d = &out[(size_t)y * w + f.x];
                for (int i = 0; i <= iend; i++) {
                    const int32_t shown = -(int32_t)((v[(first + (uint32_t)i) ^ 1u] & 0x8000) != trans_value);
                    d[i] = (o[i] & shown) | (d[i] & ~shown);
                }
                continue;
            }
            for (int i = 0; i <= iend; i++) {
                const int x = f.x + i;
                if (x < 0 || x >= w) continue;
                const uint32_t px = (base + (uint32_t)i) & 0xffffffu;
                if ((pix(px) & 0x8000) == trans_value) continue;
                const int32_t *o = own_at(px);
                out[(size_t)y * w + x] = o ? *o : -1;
            }
        }
    }
}

bool K057714::draw_between(uint32_t *out, int width, int height, double t) {
    if (!m_keep) {   // from now on
        m_keep = true;
        m_mark++;
        m_drawing = Drawn{};
        m_drawing.origin_x = m_fb_origin_x, m_drawing.origin_y = m_fb_origin_y;
        return false;
    }
    if (m_shown.cmds.empty() || m_shown_before.cmds.empty()) return false;
    // The frame on the screen: the one whose frame buffer the first layer shows (a frame's buffer:
    // where its own fb_config, its first command, puts the origin). It's the last one drawn, as a
    // rule; a game that drew a frame ahead would show the one before it, its objects then going
    // towards where the next one has them.
    auto buffer_shown = [&](const Drawn &d) {
        int x = d.origin_x, y = d.origin_y;
        for (const Cmd &c : d.cmds)
            if (((c[0] >> 29) & 7) == 3) {
                x = c[2] & 0x3ff, y = c[3] & 0x3fff;
                break;
            }
        return m_frame[0].base == (uint32_t)(y * 1024 + x);
    };
    const bool ahead = buffer_shown(m_shown_before) && !buffer_shown(m_shown);
    const Drawn &shown = ahead ? m_shown_before : m_shown;
    int shown_y = shown.origin_y;   // its buffer's origin row
    for (const Cmd &c : shown.cmds)
        if (((c[0] >> 29) & 7) == 3) {
            shown_y = c[3] & 0x3fff;
            break;
        }
    const std::vector<Cmd> &other = ahead ? m_shown.cmds : m_shown_before.cmds;
    // Where each object reaches the screen: some games draw objects into a strip of VRAM that a later
    // object copies onto the screen, the strip elsewhere from frame to frame (pop'n music 8's notes:
    // in a strip at x 608 or 611 by turns, copied onto the lanes), so where they are in VRAM moves
    // sideways while the notes only fall. An object inside a later one's source rectangle is placed
    // through it (and its scale noted, to turn a move on the screen back into one in the strip)
    // Its place is its middle: pop'n music 8's notes also squash and stretch to the beat about their
    // middle, so their left edge moves while they don't
    struct Placed { int x, y, xs, ys; bool copied, rel; int via; };   // on the screen; the copy's scale, origin, index
    auto middle = [](const Cmd &c, int &x, int &y) {
        const int xs = (c[2] >> 10) & 0x7ff, ys = (c[3] >> 10) & 0x7ff;
        const int w = (int)(c[2] & 0x1ff) + 1, h = (int)(c[3] & 0x3ff) + 1;
        x = int(c[1] & 0x3ff) + (xs && !(xs & 0x800) ? w * 64 / xs : w) / 2;
        y = int((c[1] >> 10) & 0x3fff) + (ys && !(ys & 0x800) ? h * 64 / ys : h) / 2;
    };
    auto place = [&](const std::vector<Cmd> &cs) {
        std::vector<Placed> p(cs.size());
        for (size_t k = 0; k < cs.size(); k++) {
            p[k] = {0, 0, 64, 64, false, false, -1};
            middle(cs[k], p[k].x, p[k].y);
        }
        for (size_t j = 0; j < cs.size(); j++) {
            const Cmd &b = cs[j];
            if (((b[0] >> 29) & 7) != 5) continue;
            const int ax = b[0] & 0x3ff, ay = (b[0] >> 10) & 0x3fff, w = (b[2] & 0x1ff) + 1, h = (b[3] & 0x3ff) + 1;
            const int xs = (b[2] >> 10) & 0x7ff, ys = (b[3] >> 10) & 0x7ff;
            if (xs == 0 || ys == 0 || (xs & 0x800) || (ys & 0x800)) continue;   // flipped: not followed
            const int bx = b[1] & 0x3ff, by = (b[1] >> 10) & 0x3fff;
            for (size_t k = 0; k < j; k++) {
                if (p[k].copied || ((cs[k][0] >> 29) & 7) != 5) continue;
                const int x = p[k].x, y = p[k].y;   // its middle, inside the copy's source
                if (x < ax || x >= ax + w || y < ay || y >= ay + h) continue;
                p[k] = {bx + (x - ax) * 64 / xs, by + (y - ay) * 64 / ys, xs, ys, true, (b[0] & 0x10000000) != 0, (int)j};
            }
        }
        return p;
    };
    const std::vector<Placed> here = place(shown.cmds), there = place(other);
    // Each object drawn is matched with one of the frame before's that looks the same (below), the
    // scroll voted first (hiraku/scroll_guess.h, pair: what is fast may be further than 16 pixels
    // on), and carried on: what scrolls with the frame's other moving objects along the scroll's
    // steady line, the rest along its own (a note or two alone at hi-speed), else by its last move
    std::vector<Cmd> cmds = shown.cmds;
    std::vector<std::pair<int, int>> moves(cmds.size(), {0, 0});
    std::vector<hk::ScrollGuess::Candidate> cands;
    for (size_t k = 0; k < cmds.size(); k++) {
        const Cmd &c = cmds[k];
        if (((c[0] >> 29) & 7) != 5) continue;
        const int x = here[k].x, y = here[k].y;
        for (size_t kb = 0; kb < other.size(); kb++) {
            const Cmd &b = other[kb];
            if (((b[0] >> 29) & 7) != 5 || (b[1] & 0xff000000) != (c[1] & 0xff000000)) continue;
            // the same picture or height (a note going off the lane gets shorter, an animated one
            // changes picture), whatever its width and horizontal scale (pop'n music 8's notes
            // squash and stretch to the beat as they fall, a pixel narrower or wider, and change
            // picture too: at x4 one in a few frames did both, unmatched, and stood still for a frame)
            if (b[0] != c[0] && b[3] != c[3]) continue;
            const int bx = there[kb].x, by = there[kb].y;
            if (std::abs(x - bx) > hk::ScrollGuess::REACH || std::abs(y - by) > hk::ScrollGuess::REACH) continue;
            int dx = ahead ? bx - x : x - bx, dy = ahead ? by - y : y - by;
            // the same picture from the same edge, only longer or shorter on that side: resized, not
            // moved (its middle moves). beatmania III's lane covers (TRANSLUCENT) are strips blended
            // over each other, the top one from row 0 186 and 178 rows tall by turns: carried on 4
            // rows a frame, it went over the next strip, and those rows showed darker (issue 15)
            if (b[0] == c[0] && !here[k].copied && !there[kb].copied) {
                if ((b[1] & 0x3ff) == (c[1] & 0x3ff)) dx = 0;
                if (((b[1] >> 10) & 0x3fff) == ((c[1] >> 10) & 0x3fff)) dy = 0;
            }
            // one edge and its picture's edge moved together, the other edge staying, the picture the
            // same the other way (unscaled): the same pixels in the same places, a side cut off or shown
            // more, not moved. beatmania III's life gauge: from x 240 and 192 wide to x 244 and 188,
            // its picture from x 672 to 676; carried on by its middle, it shook a pixel or two each way
            // (issue 15)
            if (!here[k].copied && !there[kb].copied && (b[0] & 0x10000000) == (c[0] & 0x10000000)) {
                const int cax = c[0] & 0x3ff, cay = (c[0] >> 10) & 0x3fff, bax = b[0] & 0x3ff, bay = (b[0] >> 10) & 0x3fff;
                const int cx = c[1] & 0x3ff, cy = (c[1] >> 10) & 0x3fff, bx = b[1] & 0x3ff, by = (b[1] >> 10) & 0x3fff;
                const int cw = (c[2] & 0x1ff) + 1, ch = (c[3] & 0x3ff) + 1, bw = (b[2] & 0x1ff) + 1, bh = (b[3] & 0x3ff) + 1;
                const bool unscaled = ((c[2] >> 10) & 0xfff) == 64 && ((c[3] >> 10) & 0xfff) == 64 && ((b[2] >> 10) & 0xfff) == 64 &&
                                      ((b[3] >> 10) & 0xfff) == 64;
                if (unscaled && cay == bay && cx - cax == bx - bax && cx + cw == bx + bw) dx = 0;
                if (unscaled && cax == bax && cy - cay == by - bay && cy + ch == by + bh) dy = 0;
            }
            // a blended object only moves as the same picture: one only as tall as another isn't it
            // moved. beatmania III's lanes on SEPARATE fade in strips of their own pictures, 15 or 16
            // rows apart, the stack 8 rows on by turns; each paired with the frame before's next
            // strip, all were carried 8 rows, went over each other, and those rows showed darker
            if (((c[1] >> 28) & 3) && b[0] != c[0]) dx = dy = 0;
            cands.push_back({(int)k, (int)kb, dx, dy, (b[0] != c[0] ? 8 : 0) + (b[3] != c[3] ? 8 : 0) + (b[2] != c[2] ? 4 : 0), true});
        }
    }
    const uint64_t id = ahead ? m_mark - 1 : m_mark;
    const std::vector<int> paired = m_scroll.pair(id, (int)cmds.size(), cands);
    std::vector<int> from(cmds.size(), -1);   // each object's own track: the frame before's, as numbered (not when ahead)
    for (size_t k = 0; k < cmds.size(); k++)
        if (paired[k] >= 0) {
            moves[k] = {cands[(size_t)paired[k]].dx, cands[(size_t)paired[k]].dy};
            if (!ahead) from[k] = cands[(size_t)paired[k]].from;
        }
    m_scroll.frame(id, moves, from);
    bool moved = false;
    if (m_clip_row.empty()) m_clip_row.assign(ROWS, 0);
    std::fill(m_clip_row.begin(), m_clip_row.end(), 0);
    auto mark = [&](int y0, int y1) {
        for (int y = std::max(y0, 0); y <= y1 && y - y0 < ROWS; y++) m_clip_row[(uint32_t)y & (ROWS - 1)] = 1;
    };
    // the row a relative command's place counts from: the frame buffer's origin as it drew (a frame
    // may set it more than once: beatmania III's song select sets row 0 then 480, or 1984 then 2464; its
    // list's copies went by the first: their strips' rows weren't drawn again with them, and a moved
    // row showed the next one's title, issue 15)
    auto origin_y = [&](size_t j) {
        const Cmd &b = shown.cmds[j];
        if (!(b[0] & 0x10000000)) return 0;
        if (j < shown.cmd_rows.size() && shown.cmd_rows[j].first <= shown.cmd_rows[j].second)
            return shown.cmd_rows[j].first - int((b[1] >> 10) & 0x3fff);
        return shown_y;
    };
    // object k of the frame, (ox, oy) on on the screen, into `c`: whether it moved (and the rows it
    // was drawn on and the rows it goes to marked: the only ones drawn again, their objects' other
    // rows staying as the frame has them)
    auto put = [&](size_t k, Cmd &c, int ox, int oy) {
        const Cmd &c0 = shown.cmds[k];
        const int x = c0[1] & 0x3ff, y = (c0[1] >> 10) & 0x3fff;
        // in VRAM through the copy's scale
        const int sx = here[k].xs == 64 ? ox : (int)std::lround(ox * here[k].xs / 64.0);
        const int sy = here[k].ys == 64 ? oy : (int)std::lround(oy * here[k].ys / 64.0);
        const int nx = x + sx, ny = y + sy;
        if ((nx == x && ny == y) || nx < 0 || nx > 0x3ff || ny < 0 || ny > 0x3fff) {
            c[1] = c0[1];   // where the frame has it (a pixel further on from a place a pixel back)
            return false;
        }
        c[1] = (c0[1] & 0xff000000u) | (uint32_t)ny << 10 | (uint32_t)nx;
        if (k < shown.cmd_rows.size()) {
            const auto [y0, y1] = shown.cmd_rows[k];
            mark(y0, y1);
            mark(y0 + (ny - y), y1 + (ny - y));
        }
        if (here[k].copied) {   // drawn through a copy: where it shows, before and after (the copy's
            // place relative to the frame buffer's origin, as most are: matched without it, as both
            // buffers' frames are, but drawn there)
            const int h = int((c0[3] & 0x3ff) + 1) * 64 / here[k].ys;
            const int at = here[k].y - h / 2 + (here[k].rel ? origin_y((size_t)here[k].via) : 0);
            mark(at - 2, at + h + 2);
            mark(at + oy - 2, at + oy + h + 2);
        }
        return true;
    };
    // Between two pixels (HK_BETWEEN_SUBPIXEL=0: whole pixels, as before), as DJ Main draws them
    // (hiraku/subpixel_mix.h): each moving object at the whole pixel below its place along its move's
    // main axis (the other rounded), the frame drawn again with each one a fraction on a pixel
    // further, and the two mixed pixel by pixel by the fraction of the object that drew it (a copy
    // passing on its source's: pop'n music 8's notes, drawn into a strip and copied onto the lanes)
    static const bool subpixel = !std::getenv("HK_BETWEEN_SUBPIXEL") || std::strcmp(std::getenv("HK_BETWEEN_SUBPIXEL"), "0") != 0;
    struct Place { int ox = 0, oy = 0, axis = -1, q = 0; };   // q: the fraction on along `axis`, in 16ths
    std::vector<Place> places(cmds.size());
    bool frac = false;
    // (an object of the scroll's line goes along it, a frame where the game missed its update for
    // it, standing still, too: ScrollGuess::on_line)
    std::vector<double> ex(cmds.size(), 0.0), ey(cmds.size(), 0.0);
    std::vector<int> axes(cmds.size(), 1);
    for (size_t k = 0; k < cmds.size(); k++) {
        if (((cmds[k][0] >> 29) & 7) != 5) continue;
        m_scroll.offset_exact(moves[k].first, moves[k].second, t, ex[k], ey[k], (int)k);
        axes[k] = std::abs(moves[k].second) >= std::abs(moves[k].first) ? 1 : 0;
        if (double lx, ly; m_scroll.follow) m_scroll.on_line((int)k, t, lx, ly, axes[k]);
    }
    for (size_t k = 0; k < cmds.size(); k++) {
        if (((cmds[k][0] >> 29) & 7) != 5) continue;
        double rx = ex[k], ry = ey[k];
        int axis = axes[k];
        // drawn through a copy that moves too: its move on the screen has the copy's in it, and the
        // copy carries it that far already; in the strip, only the rest. beatmania III's song list:
        // each title drawn into a strip that stays put, the strip's copy scrolling: moved in the strip
        // too, the titles went twice as far as their rows (issue 15)
        if (const int j = here[k].via; here[k].copied && j >= 0 && paired[k] >= 0 && paired[(size_t)j] >= 0) {
            rx -= ex[(size_t)j], ry -= ey[(size_t)j];
            if (std::abs(rx) < 1.0 / 32) rx = 0;
            if (std::abs(ry) < 1.0 / 32) ry = 0;
        }
        Place &pl = places[k];
        pl.ox = (int)std::lround(rx), pl.oy = (int)std::lround(ry);
        if (subpixel) {
            const double r = axis ? ry : rx, lo = std::floor(r);
            int q = (int)std::lround((r - lo) * 16), whole = (int)lo;
            if (q == 16) whole++, q = 0;
            (axis ? pl.oy : pl.ox) = whole;
            if (q) pl.axis = axis, pl.q = q, frac = true;
        }
        if (put(k, cmds[k], pl.ox, pl.oy)) moved = true;
    }
    std::vector<Cmd> further;   // each object a fraction on, a pixel further
    if (frac) {
        further = cmds;
        for (size_t k = 0; k < cmds.size(); k++)
            if (const Place &pl = places[k]; pl.axis >= 0) put(k, further[k], pl.ox + (pl.axis ? 0 : 1), pl.oy + (pl.axis ? 1 : 0));
    }
    if (!moved && !frac) return false;   // nothing on yet: the frame as it is
    // A copy's two sides drawn again together: a screen row it writes drawn again needs the strip
    // row it reads drawn again (else it copies the strip as the frame left it: the object at its
    // old place, a slice of it under the moved one), and a strip row drawn again needs the screen
    // row it goes to
    {
        std::vector<bool> is_copy(shown.cmds.size(), false);
        for (const Placed &p : here)
            if (p.via >= 0) is_copy[(size_t)p.via] = true;
        auto marked = [&](int y) { return y >= 0 && m_clip_row[(uint32_t)y & (ROWS - 1)] != 0; };
        // until nothing changes: in a buffer at row 0 the strip's rows are also screen rows (pop'n
        // music 8: the strip's rows 0-316 go to rows 29-345), each row drawn again another's source,
        // a chain 29 rows a link (two passes left its last links' sources as they were: notes split)
        for (bool more = true; more;) {
            more = false;
            for (size_t j = 0; j < shown.cmds.size(); j++) {
                if (!is_copy[j]) continue;
                const Cmd &b = shown.cmds[j];
                const int ay = (b[0] >> 10) & 0x3fff, h = (b[3] & 0x3ff) + 1, ys = (b[3] >> 10) & 0x7ff;
                const int by = int((b[1] >> 10) & 0x3fff) + origin_y(j);
                const int dh = h * 64 / ys;
                for (int r = 0; r < dh; r++) {
                    const int dst = by + r, src = ay + r * ys / 64;
                    if (marked(dst) != marked(src)) mark(dst, dst), mark(src, src), more = true;
                }
            }
        }
    }
    // The rows back as they were before the frame was drawn, the frame drawn again, composed, and
    // VRAM and the frame buffer origin put back: of each row, the columns the frame drew on and as
    // far each side as what moves is carried on (at most ScrollGuess::REACH a frame)
    m_span0 = std::max(0, shown.x0 - hk::ScrollGuess::REACH) / 2, m_span1 = std::min(1024, shown.x1 + hk::ScrollGuess::REACH + 2) / 2;
    if (m_span1 <= m_span0) m_span0 = 0, m_span1 = 512;
    const size_t n = (size_t)(m_span1 - m_span0);
    // the commands that draw on a row drawn again (where they were, or where they go), or set the
    // frame buffer's origin
    std::vector<int> sum(ROWS + 1, 0);
    for (int r = 0; r < ROWS; r++) sum[r + 1] = sum[r] + m_clip_row[r];
    auto any = [&](int y0, int y1) {
        if (y1 < y0) return false;
        if (y1 - y0 >= ROWS) return true;
        const int a = y0 & (ROWS - 1), b = y1 & (ROWS - 1);
        return a <= b ? sum[b + 1] - sum[a] > 0 : sum[ROWS] - sum[a] + sum[b + 1] > 0;
    };
    // the screen rows that show a row drawn again: the only ones an in-between picture composes,
    // on the frame's own picture (composed once a frame, VRAM as the frame left it)
    static std::vector<uint8_t> screen_rows;
    screen_rows.assign((size_t)height, 0);
    for (int sh = 8; sh <= 14; sh += 2) {
        const Frame &f = m_frame[(m_layer_select >> sh) & 3];
        if (f.height == 0 || f.width == 0) continue;
        for (int y = std::max(0, (int)f.y); y < height && y - f.y <= f.height + 1; y++) {
            const uint32_t a = f.base + (uint32_t)(y - f.y) * 1024, b = a + (uint32_t)f.width + 1;
            if (m_clip_row[(a >> 10) & (ROWS - 1)] || m_clip_row[(b >> 10) & (ROWS - 1)]) screen_rows[(size_t)y] = 1;
        }
    }
    {
        Composed &c = m_composed;
        const bool same = c.mark == m_mark && c.drawn == m_drawing.cmds.size() && c.layer_select == m_layer_select &&
                          c.reg_6c == m_reg_6c && c.w == width && c.h == height &&
                          std::memcmp(c.frame, m_frame, sizeof m_frame) == 0;
        if (!same) {
            c.px.resize((size_t)width * height);
            draw(c.px.data(), width, height);
            c.mark = m_mark, c.drawn = m_drawing.cmds.size(), c.layer_select = m_layer_select, c.reg_6c = m_reg_6c;
            c.w = width, c.h = height;
            std::memcpy(c.frame, m_frame, sizeof m_frame);
        }
    }
    if (frac) {   // who draws what: a row of m_own for each row drawn again
        m_own_slot.assign(ROWS, -1);
        int slots = 0;
        for (int r = 0; r < ROWS; r++)
            if (m_clip_row[r]) m_own_slot[r] = slots++;
        m_own.resize((size_t)slots * 1024);
        m_own_frac.assign(cmds.size(), 0);
        for (size_t k = 0; k < cmds.size(); k++) m_own_frac[k] = places[k].axis >= 0;
    }
    auto replay = [&](const std::vector<Cmd> &cs, uint32_t *dst, std::vector<int32_t> *own, const uint8_t *rows) {
        m_replaying = true;
        m_replay_mark++;
        m_restore_rows.clear();
        m_restore.clear();
        m_clip = true;
        for (size_t i = 0; i < shown.rows.size(); i++) {
            const uint32_t r = shown.rows[i];
            if (row_out((int)r)) continue;
            touch((int)r, (int)r, 0, 0);
            std::memcpy(&m_vram[(size_t)r * 512 + m_span0], &shown.before[i * 512 + m_span0], n * 4);
        }
        if (own) {
            std::fill(m_own.begin(), m_own.end(), -1);
            m_owning = true;
        }
        const int ox = m_fb_origin_x, oy = m_fb_origin_y;
        m_fb_origin_x = shown.origin_x, m_fb_origin_y = shown.origin_y;
        for (size_t k = 0; k < cs.size(); k++) {
            const Cmd &c = cs[k];
            if (k < shown.cmd_rows.size() && ((c[0] >> 29) & 7) != 3) {
                const auto [y0, y1] = shown.cmd_rows[k];
                const int dy = (int)((c[1] >> 10) & 0x3fff) - (int)((shown.cmds[k][1] >> 10) & 0x3fff);
                if (!any(y0, y1) && !any(y0 + dy, y1 + dy)) continue;
            }
            m_own_cmd = (int)k;
            run(c.data());
        }
        m_owning = false;
        m_own_cmd = -1;
        std::memcpy(dst, m_composed.px.data(), (size_t)width * height * 4);
        draw(dst, width, height, rows);
        if (own) own_screen(*own, width, height);
        m_fb_origin_x = ox, m_fb_origin_y = oy;
        m_clip = false;
        for (size_t i = 0; i < m_restore_rows.size(); i++)
            std::memcpy(&m_vram[(size_t)m_restore_rows[i] * 512 + m_span0], &m_restore[i * n], n * 4);
        m_replaying = false;
    };
    if (!frac) {
        replay(cmds, out, nullptr, screen_rows.data());
        return true;
    }
    static std::vector<int32_t> own0, own1;
    static std::vector<uint32_t> on;
    on.resize((size_t)width * height);
    replay(cmds, out, &own0, screen_rows.data());
    replay(further, on.data(), &own1, screen_rows.data());
    // the fraction of the object that drew the pixel a pixel further on, else of the one that left it
    auto q_of = [&](int32_t o) { return o >= 0 && (size_t)o < places.size() && places[(size_t)o].axis >= 0 ? places[(size_t)o].q : 0; };
    for (int y = 0; y < height; y++) {
        if (!screen_rows[(size_t)y]) continue;
        const size_t row = (size_t)y * width;
        hk::subpixel_mix(out + row, on.data() + row, (size_t)width, [&](size_t k) {
            const int q = q_of(own1[row + k]);
            return q ? q : q_of(own0[row + k]);
        });
    }
    return true;
}

}  // namespace hk
