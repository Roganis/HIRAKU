// license:BSD-3-Clause
// copyright-holders:Ville Linde
//
// Konami 0000057714 "GCU" 2D graphics chip, ported from MAME's k057714.cpp.
// Modified for HIRAKU: no MAME device framework; pixels are addressed through
// pix()/set_pix() instead of an endian-swizzled uint16_t view; output is
// composed into a 32-bit RGB buffer.
#include "k057714.h"
#include "k057714_rows.h"

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

K057714::K057714() : m_vram(new uint32_t[VRAM_SIZE / 4]) { reset(); }

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

void K057714::draw_frame(int frame, uint16_t *bitmap, int bw, int bh, bool inverse_trans) {
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
        if (y < 0 || y >= bh) continue;
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

void K057714::draw(uint32_t *out, int w, int h) {
    static thread_local std::unique_ptr<uint16_t[]> bmp;
    static thread_local int bmp_size = 0;
    if (bmp_size < w * h) { bmp.reset(new uint16_t[w * h]); bmp_size = w * h; }
    std::fill(bmp.get(), bmp.get() + w * h, 0);
    // most likely wrong, inverse transparency is only used by kbm;
    // beatmania III sets reg 6c to 0xfff but doesn't use inverse transparency
    bool inverse_trans = (m_reg_6c & 0xf) != 0 && m_reg_6c != 0xfff;
    draw_frame((m_layer_select >> 8) & 3, bmp.get(), w, h, inverse_trans);
    draw_frame((m_layer_select >> 10) & 3, bmp.get(), w, h, inverse_trans);
    draw_frame((m_layer_select >> 12) & 3, bmp.get(), w, h, inverse_trans);
    draw_frame((m_layer_select >> 14) & 3, bmp.get(), w, h, inverse_trans);
    // RGB555 -> xRGB8888, the same pal5bit expansion as MAME (as arithmetic, which vectorises)
    const uint16_t *src = bmp.get();
    for (int k = 0; k < w * h; k++) {
        uint32_t p = src[k], r = (p >> 10) & 0x1f, g = (p >> 5) & 0x1f, b = p & 0x1f;
        out[k] = 0xff000000u | ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (b << 3 | b >> 2);
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
            if (!trans_enable || (color[p] & 0x8000)) set_pix(fbaddr + x + i, color[p]);
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
            const int dx = ahead ? bx - x : x - bx, dy = ahead ? by - y : y - by;
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
    for (size_t k = 0; k < cmds.size(); k++) {
        Cmd &c = cmds[k];
        if (((c[0] >> 29) & 7) != 5) continue;
        const int x = c[1] & 0x3ff, y = (c[1] >> 10) & 0x3fff;
        int ox, oy;   // on the screen; in VRAM through the copy's scale
        m_scroll.offset(moves[k].first, moves[k].second, t, ox, oy, (int)k);
        const int sx = here[k].xs == 64 ? ox : (int)std::lround(ox * here[k].xs / 64.0);
        const int sy = here[k].ys == 64 ? oy : (int)std::lround(oy * here[k].ys / 64.0);
        const int nx = x + sx, ny = y + sy;
        if ((nx == x && ny == y) || nx < 0 || nx > 0x3ff || ny < 0 || ny > 0x3fff) continue;
        c[1] = (c[1] & ~0xffffffu) | (c[1] & 0xff000000u) | (uint32_t)ny << 10 | (uint32_t)nx;
        moved = true;
        // the rows it was drawn on and the rows it goes to: the only ones drawn again (their objects'
        // other rows stay as the frame has them)
        if (k < shown.cmd_rows.size()) {
            const auto [y0, y1] = shown.cmd_rows[k];
            mark(y0, y1);
            mark(y0 + (ny - y), y1 + (ny - y));
        }
        if (here[k].copied) {   // drawn through a copy: where it shows, before and after (the copy's
            // place relative to the frame buffer's origin, as most are: matched without it, as both
            // buffers' frames are, but drawn there)
            const int h = int((c[3] & 0x3ff) + 1) * 64 / here[k].ys, at = here[k].y - h / 2 + (here[k].rel ? shown_y : 0);
            mark(at - 2, at + h + 2);
            mark(at + oy - 2, at + oy + h + 2);
        }
    }
    if (!moved) return false;   // nothing on by a whole pixel yet: the frame as it is
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
                const int by = int((b[1] >> 10) & 0x3fff) + ((b[0] & 0x10000000) ? shown_y : 0);
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
    m_replaying = true;
    m_replay_mark++;
    m_restore_rows.clear();
    m_restore.clear();
    m_span0 = std::max(0, shown.x0 - hk::ScrollGuess::REACH) / 2, m_span1 = std::min(1024, shown.x1 + hk::ScrollGuess::REACH + 2) / 2;
    if (m_span1 <= m_span0) m_span0 = 0, m_span1 = 512;
    const size_t n = (size_t)(m_span1 - m_span0);
    m_clip = true;
    for (size_t i = 0; i < shown.rows.size(); i++) {
        const uint32_t r = shown.rows[i];
        if (row_out((int)r)) continue;
        touch((int)r, (int)r, 0, 0);
        std::memcpy(&m_vram[(size_t)r * 512 + m_span0], &shown.before[i * 512 + m_span0], n * 4);
    }
    const int ox = m_fb_origin_x, oy = m_fb_origin_y;
    m_fb_origin_x = shown.origin_x, m_fb_origin_y = shown.origin_y;
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
    for (size_t k = 0; k < cmds.size(); k++) {
        const Cmd &c = cmds[k];
        if (k < shown.cmd_rows.size() && ((c[0] >> 29) & 7) != 3) {
            const auto [y0, y1] = shown.cmd_rows[k];
            const int dy = (int)((c[1] >> 10) & 0x3fff) - (int)((shown.cmds[k][1] >> 10) & 0x3fff);
            if (!any(y0, y1) && !any(y0 + dy, y1 + dy)) continue;
        }
        run(c.data());
    }
    draw(out, width, height);
    m_fb_origin_x = ox, m_fb_origin_y = oy;
    m_clip = false;
    for (size_t i = 0; i < m_restore_rows.size(); i++)
        std::memcpy(&m_vram[(size_t)m_restore_rows[i] * 512 + m_span0], &m_restore[i * n], n * 4);
    m_replaying = false;
    return true;
}

}  // namespace hk
