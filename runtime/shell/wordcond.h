// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// A condition on a word of the game's memory, looked at once a frame: a mod's [[rate]] (host/mods.cpp,
// docs/MODS.md) and a game's [smooth] (shell/smoothgate.h, docs/SMOOTH.md). It holds while the word
// at `address` changes from frame to frame (two frames in a row changed: on; two still: off), and,
// with `gated`, only while the word at `gate` holds `value` (off at once when it doesn't).
#include <cstdint>
#include <cstring>

namespace hk {

struct WordCondition {
    uint32_t address = 0, last = 0;
    int changed = 0, still = 0;   // frames in a row the word changed, didn't
    bool always = false;          // no while_changing: from the first frame on
    bool gated = false;           // while_equal: and only while this word holds `value`
    uint32_t gate = 0, value = 0;

    // One frame: whether it holds, `on` whether it held so far. read(address) gives the word's 4
    // bytes (nullptr: not there). `known` false when the word at `address` couldn't be read (nothing
    // counted; `on` given back)
    template <class Read> bool step(bool on, Read read, bool &known) {
        known = true;
        if (always) {
            changed = 2, still = 0;
        } else {
            const uint8_t *m = read(address);
            if (!m) {
                known = false;
                return on;
            }
            uint32_t v;
            std::memcpy(&v, m, 4);
            if (v != last) changed++, still = 0;
            else still++, changed = 0;
            last = v;
        }
        // two frames in a row either way, so a word that changes once (a load) doesn't count; and
        // with while_equal, only while that word holds the value (off at once when it doesn't)
        bool in_state = true;
        if (gated) {
            const uint8_t *g = read(gate);
            uint32_t gv = 0;
            if (g) std::memcpy(&gv, g, 4);
            in_state = g && gv == value;
        }
        return in_state && (on ? still < 2 : changed >= 2);
    }
};

}  // namespace hk
