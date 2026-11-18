// SPDX-License-Identifier: GPL-3.0-or-later
// A turntable on a controller's axis, read a value at a time: how far it turned since the value
// before, in 65536ths of a turn (a wrap of the axis is a turn, either way round). The game's controls
// (host/input.cpp) and the launcher's dial beside a turntable's binding (launcher/controlsdialog.cpp)
// read it alike.
//
// SDL gives an 8-bit axis (0-255: an IIDX controller's turntable) as v * 257 - 32768, so each step is
// 257 units but the one across the wrap, 255 to 0, which is 1 (32767 to -32768). Read as the 16 bits
// it says, a turn's total was right but that one step came to a 64th of a count instead of 4: a slow
// scratch through it lost a step of the 2 the game takes one way as a scratch. So while every value
// seen is of that form the axis is read as 8 bits, a 256th of a turn a step; the first that isn't
// makes it 16 bits for good, read as before from the value before (no jump at the change). A 16-bit
// controller taken for 8 bits for its first values turns 0.4 % short meanwhile.
#pragma once

#include <cstdint>

namespace hk::host {

struct AxisTurn {
    bool seen = false;   // a value read (the first is where the axis is, not a movement)
    bool wide = false;   // a value not of the 8-bit form seen: 16 bits for good
    int16_t last = 0;

    int turn(int16_t v) {
        const bool was = seen;
        const int from = last;
        seen = true, last = v;
        if (!wide && (v + 32768) % 257 != 0) wide = true;
        if (!was) return 0;
        if (!wide) {
            int d = (v + 32768) / 257 - (from + 32768) / 257;
            if (d > 128) d -= 256;
            if (d < -128) d += 256;
            return d * 256;
        }
        int d = v - from;
        if (d > 32768) d -= 65536;
        if (d < -32768) d += 65536;
        return d;
    }
};

}  // namespace hk::host
