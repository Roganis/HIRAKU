// SPDX-License-Identifier: GPL-3.0-or-later
// The SPU board's up/down volume control, as the sound program drives it through the high byte of
// its register at 0x230000 (MAME takes the register for interrupt acknowledges only, bits 0-3).
// Two channels, 64 steps each (0..63). A step is a pulse on the channel's clock bit; the direction
// bit set in the same write means down. The sound program counts the steps itself, starts each
// channel at the top (63, after 64 steps up), and sets the beatmania III games' DEMO VOLUME with it
// in the attract (command 6: 0/21/42/63), back to 63 when the attract is left.
//
// The chip itself is not known (no part named for the board), so neither is its curve: the gain
// here is counter / 63 as a plain amplitude factor, 0 silent and 63 exactly 1. An assumption.
#pragma once

#include <cstdint>

namespace hk::spu {

struct VolumeChip {
    static constexpr int TOP = 63;
    static constexpr uint16_t CLOCK[2] = {0x1000, 0x0100};
    static constexpr uint16_t DOWN[2] = {0x8000, 0x0800};

    int level[2] = {TOP, TOP};
    bool clock[2] = {false, false};

    void reset() { *this = VolumeChip{}; }

    // a write to 0x230000; only its high byte reaches the chip. True when a level changed.
    bool write(uint16_t v, uint16_t mask) {
        if (!(mask & 0xff00)) return false;
        bool changed = false;
        for (int ch = 0; ch < 2; ch++) {
            const bool c = (v & CLOCK[ch]) != 0;
            if (c && !clock[ch]) {   // a step on the clock's rising edge
                const int n = (v & DOWN[ch]) ? level[ch] - 1 : level[ch] + 1;
                if (n >= 0 && n <= TOP) level[ch] = n, changed = true;
            }
            clock[ch] = c;
        }
        return changed;
    }

    bool full() const { return level[0] == TOP && level[1] == TOP; }
    float gain(int ch) const { return level[ch] == TOP ? 1.0f : (float)level[ch] / (float)TOP; }
};

}  // namespace hk::spu
