// SPDX-License-Identifier: GPL-3.0-or-later
// A waiting loop's turn judged from its bus cycles (runtime/dev/loop_turns.h): which turns stand
// for those that follow, and what is kept of them.
#include <cstdio>
#include <vector>

#include "dev/loop_turns.h"

using namespace hk::turns;

namespace {
int g_failed = 0;
void expect(bool ok, const char *what) {
    if (!ok) {
        printf("test_loop_turns: FAILED: %s\n", what);
        g_failed++;
    }
}
constexpr uint8_t ROM = 1, RAM = 3, DEVICE = 0;
Access rd(uint32_t a, uint16_t v, uint16_t mask = 0xffff, uint8_t plain = RAM) { return {a, (uint16_t)(v & mask), 0, mask, false, plain}; }
Access wr(uint32_t a, uint16_t v, uint16_t before, uint16_t mask = 0xffff, uint8_t plain = RAM) { return {a, v, before, mask, true, plain}; }
}  // namespace

int main() {
    Turn t;
    // reads alone: a turn to count, held to what it read; ROM is not kept
    expect(analyse({rd(0x1000, 0x1234), rd(0x400, 0x4e75, 0xffff, ROM), rd(0x2000, 0)}, t), "reads alone are a turn to count");
    expect(t.reads == std::vector<Word>{{0x1000, 0x1234, 0xffff}, {0x2000, 0, 0xffff}} && t.finals.empty(), "its reads are kept, ROM's are not");

    // a call's return address: written, read back, the same every turn
    expect(analyse({wr(0x7ffc, 0x0000, 0x0000), wr(0x7ffe, 0x0412, 0x0412), rd(0x7ffc, 0x0000), rd(0x7ffe, 0x0412)}, t),
           "a word written as it was is a turn to count");
    expect(t.reads.empty() && t.finals == std::vector<Word>{{0x7ffc, 0x0000, 0xffff}, {0x7ffe, 0x0412, 0xffff}},
           "what was written first is not held to what it held, and is kept as left");

    // a scratch word written twice, back to what it was
    expect(analyse({rd(0x3000, 7), wr(0x3000, 9, 7), rd(0x3000, 9), wr(0x3000, 7, 9)}, t), "a word changed and put back is a turn to count");
    expect(t.reads == std::vector<Word>{{0x3000, 7, 0xffff}} && t.finals == std::vector<Word>{{0x3000, 7, 0xffff}}, "read first: held; written: kept");

    // a counter moved on: the next turn is another
    expect(!analyse({rd(0x3000, 7), wr(0x3000, 8, 7)}, t), "a word left changed is not a turn to count");
    expect(!analyse({wr(0x3000, 8, 7)}, t), "... nor one written without being read");

    // a device
    expect(!analyse({rd(0x1000, 1), rd(0x200000, 0xff, 0xffff, DEVICE)}, t), "a device read is not a turn to count");
    expect(!analyse({wr(0x220000, 1, 0, 0xffff, DEVICE)}, t), "a device write is not a turn to count");
    expect(!analyse({wr(0x400, 1, 1, 0xffff, ROM)}, t), "a write to ROM is not a turn to count");

    // bytes: the even one is the word's high half
    expect(analyse({rd(0x1000, 0xab00, 0xff00), rd(0x1000, 0x00cd, 0x00ff), rd(0x1002, 0x0011, 0x00ff)}, t), "byte reads are a turn to count");
    expect(t.reads == std::vector<Word>{{0x1000, 0xabcd, 0xffff}, {0x1002, 0x0011, 0x00ff}}, "two bytes of a word are held as one, a lone byte by its mask");
    expect(analyse({wr(0x1000, 0x5500, 0x5577, 0xff00), rd(0x1000, 0x5577)}, t), "a byte written as it was, the word read after");
    expect(t.reads == std::vector<Word>{{0x1000, 0x0077, 0x00ff}} && t.finals == std::vector<Word>{{0x1000, 0x5500, 0xff00}},
           "of that word the byte not written is held, the written one kept");
    expect(!analyse({wr(0x1000, 0x5500, 0x4477, 0xff00)}, t), "a byte left changed is not a turn to count");

    // a read that does not see what the turn last put there: something else writes it
    expect(!analyse({wr(0x1000, 5, 5), rd(0x1000, 6)}, t), "a read of another value than the one written is not a turn to count");

    if (!g_failed) printf("test_loop_turns: turns judged as they should be\n");
    return g_failed ? 1 : 0;
}
