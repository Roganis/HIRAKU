// SPDX-License-Identifier: GPL-3.0-or-later
// The GCU's in-between pictures (K057714::draw_between, --smooth-motion) on a made-up game drawn
// through the GCU's own registers: two frame buffers by turns, a note falling 2 rows a frame, and
// beside it two translucent strips the way beatmania III draws its lane covers (issue 15): the top
// one from row 0, 20 and 24 rows tall by turns, the next one under it another picture each frame. The
// note is carried on; the strips stay as the frame has them (resized from their top edge, not
// moved: carried on, the top one went over the next one and those rows were blended twice).
#include <cstdint>
#include <cstdio>
#include <vector>

#include "dev/k057714.h"

namespace {

constexpr int W = 64, H = 64;
constexpr uint32_t TEX_Y = 3000, BUF_Y[2] = {1024, 1536};

void cmd(hk::K057714 &g, uint32_t c0, uint32_t c1, uint32_t c2, uint32_t c3) {
    for (uint32_t w : {c0, c1, c2, c3}) g.write(0x70, w, 0xffffffff);
}

// pixels into VRAM through FIFO 0's data mode (two a word), then back to commands
void fill_vram(hk::K057714 &g, uint32_t x, uint32_t y, int w, int h, uint16_t colour) {
    g.write(0x68, 0, 0xffff0000);
    for (int j = 0; j < h; j++) {
        g.write(0x60, (y + j) * 1024 + x, 0xffffffff);
        for (int i = 0; i < w; i += 2) g.write(0x70, (uint32_t)colour << 16 | colour, 0xffffffff);
    }
    g.write(0x68, 0x100u << 16, 0xffff0000);
}

// an object from (sx, TEX_Y), relative to the frame buffer's origin; blended: half the picture
// under it (blend mode 1, weights 8 and 8)
void object(hk::K057714 &g, uint32_t sx, int x, int y, int w, int h, bool blend) {
    const uint32_t a = blend ? 8u << 27 : 0;
    cmd(g, 0xb0000000u | TEX_Y << 10 | sx, (blend ? 1u << 28 : 0) | (uint32_t)y << 10 | (uint32_t)x,
        a | 64u << 10 | (uint32_t)(w - 1), a | 64u << 10 | (uint32_t)(h - 1));
}

}  // namespace

int main() {
    hk::K057714 g;
    g.reset();
    g.write(0x00, (W - 1u) << 16, 0xffff0000);
    g.write(0x04, (H - 1u) << 16, 0xffff0000);
    g.write(0x10, 0, 0x0000ffff);                 // the four layers: frame 0
    g.write(0x14, 16u << 7 << 16, 0xffff0000);    // frame 0 at full brightness
    g.write(0x20, 0, 0xffffffff);
    g.write(0x30, (H - 1u) << 16 | (W - 1u), 0xffffffff);
    fill_vram(g, 0, TEX_Y, 32, 32, 0x8000);       // the strips' pictures: black (two places)
    fill_vram(g, 128, TEX_Y, 64, 32, 0x8000);
    fill_vram(g, 64, TEX_Y, 4, 4, 0xfc00);        // the note: red

    std::vector<uint32_t> frame(W * H), between(W * H);
    int checked = 0, strip_wrong = 0, note_moved = 0;
    for (int n = 0; n < 40; n++) {
        // the vblank: the frame drawn last goes on show
        g.vblank(true);
        g.vblank(false);
        if (n > 0) g.write(0x40, BUF_Y[(n - 1) & 1] * 1024, 0xffffffff);
        if (n > 2) {
            g.draw(frame.data(), W, H);
            if (g.draw_between(between.data(), W, H, 0.5)) {
                checked++;
                for (int y = 0; y < H; y++)
                    for (int x = 0; x < 32; x++) strip_wrong += frame[y * W + x] != between[y * W + x];
                for (int y = 0; y < H; y++)
                    for (int x = 40; x < 44; x++) note_moved += frame[y * W + x] != between[y * W + x];
            } else {
                g.draw_between(between.data(), W, H, 0.5);   // (the first call starts the keeping)
            }
        }
        // frame n into its buffer: cleared white, the strips, the note
        cmd(g, 0x60000000, 0, 0, BUF_Y[n & 1]);
        cmd(g, 0x90000000u | (H - 1u) << 10 | (W - 1u), 0, 0xffffffff, 0xffffffff);
        const int top = n & 1 ? 24 : 20;
        object(g, 0, 0, 0, 32, top, true);
        object(g, n & 1 ? 160 : 128, 0, top, 32, n & 1 ? 7 : 8, true);   // (another picture and height by turns, as the game's)
        object(g, 64, 40, 2 * n % 48, 4, 4, false);
    }
    printf("test_gcu_between: %d in-between pictures, %d strip pixels not as the frame has them, %d note pixels moved\n",
           checked, strip_wrong, note_moved);
    return checked > 20 && strip_wrong == 0 && note_moved > 0 ? 0 : 1;
}
