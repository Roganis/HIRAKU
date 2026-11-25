// SPDX-License-Identifier: GPL-3.0-or-later
// The Twinkle SPU board's interrupt acknowledges (runtime/twinkle/spuctrl.h), driven the way the
// board's program writes its control register:
//   - a bit taken down acknowledges its interrupt; a bit set, or one that was down already, nothing
//   - a write that takes two bits down acknowledges both (the disk's interrupt arriving inside the
//     tick's handler: the case that left the disk's interrupt up for good, "E730:HDD READ ERROR")
//   - MAME's rule (first_only) acknowledges the lower one alone
//   - the register's other bits change nothing
#include <cstdio>

#include "twinkle/spuctrl.h"

using namespace hk::twinkle;

namespace {
int g_fail = 0;
#define CHECK_EQ(a, b)                                                           \
    do {                                                                         \
        const unsigned va = (unsigned)(a), vb = (unsigned)(b);                   \
        if (va != vb) { fprintf(stderr, "FAIL %s:%d: %s = 0x%x, not 0x%x\n", __FILE__, __LINE__, #a, va, vb); g_fail++; } \
    } while (0)

constexpr uint16_t TICK = 0x0080, DISK = 0x0400;
constexpr unsigned ACK1 = 1, ACK2 = 2, ACK4 = 4, ACK6 = 8;   // spu_ctrl_acks' bits, by SPU_IRQ_LEVEL

// The lines of a 68000 with the board's four interrupts, as the board's glue drives them
struct Lines {
    uint16_t ctrl = 0;
    bool up[7] = {};
    bool first_only = false;
    void write(uint16_t v) {
        const unsigned acks = spu_ctrl_acks(ctrl, v, first_only);
        for (int k = 0; k < 4; k++)
            if (acks >> k & 1) up[SPU_IRQ_LEVEL[k]] = false;
        ctrl = v;
    }
};

// The program's handlers (863a05, 0x2e0e and 0x2eda): the register's copy in RAM, a handler's own
// bit taken out of the copy, then the copy written. `disk_inside`: the disk's interrupt arrives
// between the tick handler's two instructions, and its handler runs first (level 6 over level 1).
void tick_with_disk(Lines &l, bool disk_inside) {
    uint16_t copy = l.ctrl;
    l.up[1] = true;                  // the 60 Hz tick
    copy &= ~TICK;                   // 0x2e18: andi.w #-0x81,(copy)
    if (disk_inside) {
        l.up[6] = true;              // the read's end
        copy &= ~DISK;               // 0x2ee4: andi.w #-0x401,(copy)
        l.write(copy);               // 0x2eee: both bits down in one write
    }
    l.write(copy);                   // 0x2e22
    if (!disk_inside) {
        l.up[6] = true;              // the read's end, after the tick's acknowledge
        copy &= ~DISK;
        l.write(copy);
    }
}
}  // namespace

int main() {
    // one bit at a time
    CHECK_EQ(spu_ctrl_acks(0x0580, 0x0500), ACK1);
    CHECK_EQ(spu_ctrl_acks(0x0580, 0x0180), ACK6);
    CHECK_EQ(spu_ctrl_acks(0x0780, 0x0680), ACK2);
    CHECK_EQ(spu_ctrl_acks(0x0780, 0x0580), ACK4);
    // a bit set, a bit down already, the same value again: nothing
    CHECK_EQ(spu_ctrl_acks(0x0100, 0x0580), 0);
    CHECK_EQ(spu_ctrl_acks(0x0100, 0x0100), 0);
    CHECK_EQ(spu_ctrl_acks(0x0000, 0x0000), 0);
    // the bits outside 7 to 10 are not acknowledges
    CHECK_EQ(spu_ctrl_acks(0xf87f, 0x0000), 0);
    CHECK_EQ(spu_ctrl_acks(0xffff, 0x0000), ACK1 | ACK2 | ACK4 | ACK6);
    // two down in one write: both, and MAME's rule the lower alone
    CHECK_EQ(spu_ctrl_acks(0x0580, 0x0100), ACK1 | ACK6);
    CHECK_EQ(spu_ctrl_acks(0x0580, 0x0100, true), ACK1);
    CHECK_EQ(spu_ctrl_acks(0x0780, 0x0180, true), ACK4);
    CHECK_EQ(spu_ctrl_acks(0x0780, 0x0180), ACK4 | ACK6);

    // the program's handlers: whenever the disk's interrupt arrives, both lines end down
    for (int inside = 0; inside < 2; inside++) {
        Lines l;
        l.ctrl = TICK | 0x0100 | DISK;
        tick_with_disk(l, inside != 0);
        CHECK_EQ(l.up[1], 0);
        CHECK_EQ(l.up[6], 0);
        CHECK_EQ(l.ctrl, 0x0100);
    }
    // ... and with MAME's rule the disk's stays up when it arrived inside the tick's handler (every
    // later write of the handler is 0x0100 over 0x0100: nothing falls, the 68000 never leaves it)
    {
        Lines l;
        l.first_only = true;
        l.ctrl = TICK | 0x0100 | DISK;
        tick_with_disk(l, true);
        CHECK_EQ(l.up[1], 0);
        CHECK_EQ(l.up[6], 1);
        l.write(0x0100);
        CHECK_EQ(l.up[6], 1);
    }

    if (g_fail) { fprintf(stderr, "%d checks failed\n", g_fail); return 1; }
    printf("spu control register: acknowledges ok\n");
    return 0;
}
