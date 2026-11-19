// SPDX-License-Identifier: GPL-3.0-or-later
// The System 573's registers against its connectors' pins (shell/wired573.h's bits), for a real
// cabinet on them (board.h's wired_*): which pin each bit of the I/O boards' output registers
// drives, and which bits of the input registers each switch pulls low. From the board's
// documentation (psx-spx, "Konami System 573": the analog and digital I/O boards' registers, the
// ASIC's), which MAME's output numbers agree with (tests/usb573 holds one against the other).
#pragma once

#include <cstdint>

#include "shell/wired573.h"

namespace hk::k573::pins {

// The analog I/O board (GX700-PWB(F)): a write of `data` to bank 0-3's latch (A-D, 0x1f640080 +
// 8 * bank). `mask`: the bank's pins; the return: those of them it grounds (a bit at 0 grounds its
// output). Bits 0-7 of banks A to C are outputs 1, 3, 5, 7, 6, 4, 2, 0; bits 0-3 of bank D are D3 to D0.
inline uint64_t analog_bank(int bank, uint8_t data, uint64_t &mask) {
    static const int pin[8] = {1, 3, 5, 7, 6, 4, 2, 0};
    uint64_t bits = 0;
    if (bank < 3) {
        mask = 0xffull << (bank * 8);
        for (int b = 0; b < 8; b++)
            if (!(data >> b & 1)) bits |= 1ull << (bank * 8 + pin[b]);
    } else {
        mask = 0xfull << 24;
        for (int b = 0; b < 4; b++)
            if (!(data >> b & 1)) bits |= wired573::out_d(3 - b);
    }
    return bits;
}

// The digital I/O board (GX894-PWB(B)A), by MAME's k573dio output number: 4 * register + i, the
// registers in its order (A0-A3, A4-A7, B0-B3, B4-B7, C0-C3, C4-C7, none, D0-D3) and i counting
// their bits 12-15 as 0, 2, 3, 1. 0: no pin.
inline uint64_t digital_pin(unsigned number) {
    static const int first[8] = {0, 4, 8, 12, 16, 20, -1, 24};
    static const int bit[4] = {0, 2, 3, 1};
    if (number >= 32 || first[number / 4] < 0) return 0;
    return 1ull << (first[number / 4] + bit[number % 4]);
}

// The ASIC's input registers as MAME's ports have them (IN1 0x1f400004, IN2 0x1f400008, IN3
// 0x1f40000c, 32 bits each): the bits a cabinet's closed switches pull low
struct Inputs {
    uint32_t in1 = 0, in2 = 0, in3 = 0;
};
inline Inputs inputs(uint32_t sw) {
    using namespace wired573;
    Inputs i;
    i.in2 = sw & 0xffff;   // the switches' bits 0-15 are IN2's: the joysticks, buttons 1-3, the starts
    i.in3 = (sw & P1_B4 ? 0x100u : 0) | (sw & P1_B5 ? 0x200u : 0) | (sw & TEST ? 0x400u : 0) | (sw & P1_B6 ? 0x800u : 0) |
            (sw & P2_B4 ? 0x1000000u : 0) | (sw & P2_B5 ? 0x2000000u : 0) | (sw & P2_B6 ? 0x8000000u : 0);
    i.in1 = (sw & COIN1 ? 0x1000000u : 0) | (sw & COIN2 ? 0x2000000u : 0) | (sw & SERVICE ? 0x10000000u : 0);
    return i;
}

}  // namespace hk::k573::pins
