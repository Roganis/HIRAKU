// SPDX-License-Identifier: GPL-3.0-or-later
// The Twinkle SPU board's control register (the 68000's 0x230000), bits 7 to 10: the enables of
// its interrupts 1 (the 60 Hz tick), 2, 4 (the main board's mailbox) and 6 (the hard disk). A bit
// written 0 after 1 acknowledges its interrupt: the line goes down.
//
// Each bit acknowledges its own interrupt, whatever the other bits do in the same write. MAME's
// driver looks at the bits in turn and stops at the first that fell, so a write that takes two
// enables down acknowledges the lower interrupt only. The board's program makes such writes: its
// handlers keep a copy of the register in RAM, take their bit out of the copy and then write the
// copy. When the disk's interrupt arrives between those two instructions of the tick's handler,
// the disk's handler writes a value with both bits down. With MAME's rule the disk's interrupt
// then stays up for good, the 68000 never leaves its handler, and the game stops ten seconds later
// with "E730:HDD READ ERROR" (now and then: it takes a disk read ending within a few tens of the
// 266,667 cycles between two ticks, and a song's load is some 200 reads). `first_only` is MAME's
// rule (HK_TW_ONE_ACK=1).
#pragma once
#include <cstdint>

namespace hk::twinkle {

inline constexpr int SPU_IRQ_LEVEL[4] = {1, 2, 4, 6};   // the 68000's level of control bits 7, 8, 9, 10

// The interrupts a write of `now` over `before` acknowledges: bit k for SPU_IRQ_LEVEL[k]
constexpr unsigned spu_ctrl_acks(uint16_t before, uint16_t now, bool first_only = false) {
    unsigned fell = ((unsigned)(before & ~now) >> 7) & 0xf;
    if (first_only) fell &= ~fell + 1;   // the lowest one alone
    return fell;
}

}  // namespace hk::twinkle
