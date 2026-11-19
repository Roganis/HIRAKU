// SPDX-License-Identifier: GPL-3.0-or-later
// Lamps hung on a security cassette's shift registers (MAME's serial lamps, ksys573.cpp:
// serial_lamp_reset, serial_lamp_data and each game's lamp clock): the cassette's d5 clocks a bit
// of d7 in at its rising edge, d6 high starts a word afresh. Step Champ's word is eight bits.
#pragma once

namespace hk::k573 {

struct SerialLamps {
    int bits = 0, shift = 0, data = 0, clk = 0;

    void reset(int state) {
        if (state) bits = shift = 0;
    }
    void set_data(int state) { data = state; }
    // The clock line: true when this edge completed a word of `length` bits, then in `word` (its
    // first bit the highest)
    bool clock(int state, int length, int &word) {
        bool done = false;
        if (state && !clk) {
            bits++;
            shift = (shift << 1) | data;
            if (bits == length) {
                word = shift;
                bits = shift = 0;
                done = true;
            }
        }
        clk = state;
        return done;
    }
};

}  // namespace hk::k573
