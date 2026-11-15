// SPDX-License-Identifier: GPL-3.0-or-later
// A waiting loop's turn, from the bus cycles it made: is it one that does the same again?
//
// A sound board's processor spends its time going round a main loop that looks at its own RAM for
// something to do (a command from the main board, a voice to move on) and mostly finds nothing. A
// turn that found nothing leaves the processor exactly where it was, and memory too: the same
// turn follows, and the next, until something outside changes what the turn reads. The processor's
// core counts such turns instead of running them (loop_counter.h; m68000_loops.h for MAME's
// 68000); this is the part that knows no processor, so that a test can hold it to its word.
//
// The core notes every bus cycle of one turn, from an instruction to the same instruction with
// the processor's whole state the same. `analyse` then says whether the turn stands for those
// that follow, and what must hold for that:
//   - every cycle was memory's own (a device read can answer differently each time, a device
//     write does something);
//   - what the turn wrote, it left as it found it (the return addresses a call pushes are the
//     same every turn; a counter moved on is not);
//   - `reads`: the bytes it read before writing them. While they hold what they held, the turn
//     does what it did;
//   - `finals`: the bytes it wrote, as it left them. A counted turn must leave them so, whatever
//     was there (something else may have used the same stack since).
// Bytes are told by bus words and masks, as a 16-bit big-endian bus has them: a word at an even
// address, mask 0xff00 for its even byte, 0x00ff for its odd one.
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace hk::turns {

struct Access {
    uint32_t addr;     // the bus word's address (even)
    uint16_t data;     // read: what was read; write: what is written
    uint16_t before;   // write: the word as it was
    uint16_t mask;
    bool write;
    uint8_t plain;     // bit 0: memory's own cycle; bit 1: memory that can be written
};
struct Word {
    uint32_t addr;
    uint16_t value, mask;
    friend bool operator==(const Word &, const Word &) = default;
};
struct Turn {
    std::vector<Word> reads, finals;
};

inline bool analyse(const std::vector<Access> &log, Turn &out) {
    struct Byte {
        uint32_t addr;
        uint8_t first, now;   // at the turn's start; now
        bool written, read_first;
    };
    std::vector<Byte> bytes;
    bytes.reserve(log.size() * 2);
    // (a turn is a few hundred cycles: the bytes are found by a search of what is kept sorted)
    auto at = [&](uint32_t a) {
        return std::lower_bound(bytes.begin(), bytes.end(), a, [](const Byte &b, uint32_t x) { return b.addr < x; });
    };
    for (const Access &x : log) {
        if (!(x.plain & 1)) return false;
        if (x.write && !(x.plain & 2)) return false;   // a write to ROM: a device's for all this knows
        if (!x.write && !(x.plain & 2)) continue;      // ROM: reads what it read
        for (int half = 0; half < 2; half++) {
            const uint16_t m = half ? 0x00ff : 0xff00;
            if (!(x.mask & m)) continue;
            if ((x.mask & m) != m) return false;       // part of a byte: not a 68000's
            const uint32_t a = (x.addr & ~1u) + (uint32_t)half;
            const uint8_t v = (uint8_t)(half ? x.data : x.data >> 8), was = (uint8_t)(half ? x.before : x.before >> 8);
            auto it = at(a);
            if (it == bytes.end() || it->addr != a) {
                bytes.insert(it, x.write ? Byte{a, was, v, true, false} : Byte{a, v, v, false, true});
                continue;
            }
            if (x.write) it->now = v, it->written = true;
            else if (it->now != v) return false;       // not what was last there: something else writes it
        }
    }
    out.reads.clear();
    out.finals.clear();
    auto put = [](std::vector<Word> &to, const Byte &b) {
        const uint32_t w = b.addr & ~1u;
        const uint16_t m = b.addr & 1 ? 0x00ff : 0xff00, v = (uint16_t)(b.addr & 1 ? b.now : b.now << 8);
        if (!to.empty() && to.back().addr == w) to.back().value |= v, to.back().mask |= m;
        else to.push_back({w, v, m});
    };
    for (const Byte &b : bytes) {
        if (b.written && b.now != b.first) return false;   // left changed: the next turn is another
        if (b.read_first) put(out.reads, b);
        if (b.written) put(out.finals, b);
    }
    return true;
}

}  // namespace hk::turns
