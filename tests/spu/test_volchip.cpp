// SPDX-License-Identifier: GPL-3.0-or-later
// The SPU board's up/down volume control (runtime/spu/volchip.h) driven the way the sound program
// drives it (a pulse = the clock bit on, then off; the direction bit in both writes for a step down):
//   - 63 steps down from the top reach 0 and stay there; steps up stop at 63
//   - a write without the clock bit, or to the low byte only, moves nothing
//   - the two channels are independent
//   - the gain at 63 is exactly 1, at 0 exactly 0
#include <cstdio>

#include "spu/volchip.h"

using namespace hk::spu;

namespace {
int g_fail = 0;
#define CHECK_EQ(a, b)                                                           \
    do {                                                                         \
        const double va = (double)(a), vb = (double)(b);                         \
        if (va != vb) { fprintf(stderr, "FAIL %s:%d: %s = %g, not %g\n", __FILE__, __LINE__, #a, va, vb); g_fail++; } \
    } while (0)

constexpr uint16_t BASE = 0x000b;   // interrupt acknowledges in the low byte, as the program ORs in

void step(VolumeChip &c, int ch, bool down) {
    const uint16_t dir = down ? VolumeChip::DOWN[ch] : 0;
    c.write(VolumeChip::CLOCK[ch] | dir | BASE, 0xffff);
    c.write(dir | BASE, 0xffff);
}
}  // namespace

int main() {
    VolumeChip c;
    CHECK_EQ(c.level[0], 63);
    CHECK_EQ(c.level[1], 63);
    CHECK_EQ(c.gain(0), 1.0f);
    CHECK_EQ(c.full(), true);

    for (int k = 0; k < 63; k++) step(c, 0, true);
    CHECK_EQ(c.level[0], 0);
    CHECK_EQ(c.gain(0), 0.0f);
    CHECK_EQ(c.level[1], 63);   // the other channel untouched
    CHECK_EQ(c.gain(1), 1.0f);
    for (int k = 0; k < 5; k++) step(c, 0, true);
    CHECK_EQ(c.level[0], 0);    // stays at 0

    for (int k = 0; k < 21; k++) step(c, 0, false);
    CHECK_EQ(c.level[0], 21);
    CHECK_EQ(c.gain(0), 21.0f / 63.0f);
    for (int k = 0; k < 64; k++) step(c, 0, false);
    CHECK_EQ(c.level[0], 63);   // stops at the top
    CHECK_EQ(c.gain(0), 1.0f);

    for (int k = 0; k < 10; k++) step(c, 0, true);
    for (int k = 0; k < 3; k++) step(c, 1, true);
    CHECK_EQ(c.level[0], 53);
    CHECK_EQ(c.level[1], 60);

    // no clock bit: the direction bits and the low byte alone move nothing
    c.write(VolumeChip::DOWN[0] | VolumeChip::DOWN[1] | BASE, 0xffff);
    c.write(BASE, 0xffff);
    CHECK_EQ(c.level[0], 53);
    CHECK_EQ(c.level[1], 60);
    // a clock bit in a low-byte-only write does not reach the chip
    c.write(VolumeChip::CLOCK[0] | VolumeChip::CLOCK[1], 0x00ff);
    c.write(0, 0x00ff);
    CHECK_EQ(c.level[0], 53);
    CHECK_EQ(c.level[1], 60);
    // a clock held on is one step, not one a write
    c.write(VolumeChip::CLOCK[0] | VolumeChip::DOWN[0], 0xffff);
    c.write(VolumeChip::CLOCK[0] | VolumeChip::DOWN[0], 0xffff);
    c.write(VolumeChip::DOWN[0], 0xffff);
    CHECK_EQ(c.level[0], 52);

    c.reset();
    CHECK_EQ(c.level[0], 63);
    CHECK_EQ(c.level[1], 63);

    if (g_fail) { fprintf(stderr, "%d checks failed\n", g_fail); return 1; }
    printf("volchip: all checks passed\n");
    return 0;
}
