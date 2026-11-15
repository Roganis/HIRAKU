// SPDX-License-Identifier: GPL-3.0-or-later
// MAME's 68000 (Twinkle's sound board): turns of its waiting loop counted instead of run.
//
// The board's program goes round one main loop some 130 times a frame, about 115 instructions
// and 1600 cycles a turn, looking in its own RAM for something to do; MAME's core, exact to the
// bus cycle, took a third of a Twinkle frame's work over it. dev/loop_counter.h says how the turns
// that find nothing are counted, exactly; MAME's core tells it of each instruction at the loop's
// head and, while a turn is learned, of every bus cycle (m68000.h, the lines marked HIRAKU).
//
// HK_68K_LOOPS=0: every turn run. =check: the turns that would be counted are run, and the
// processor and memory compared with what counting them would have left. HK_68K_STATS=1: a line
// at exit.
#pragma once

#include <cstdint>
#include <vector>

class m68000_device;

namespace hk::m68000_loops {

void attach(m68000_device &cpu);   // once, after the machine has started
void frame(m68000_device &cpu);    // once a frame: looks for the loop again when the known one is not met any more

// for tests/dev/test_m68000_loops.cpp: everything MAME keeps of the processor between two
// instructions but the cycles left in its timeslice, and the turns counted so far
void state(m68000_device &cpu, std::vector<char> &to);
uint64_t counted(m68000_device &cpu);

}  // namespace hk::m68000_loops
