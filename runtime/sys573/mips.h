// SPDX-License-Identifier: GPL-3.0-or-later
// Recompiled R3000 code for the System 573 (PLAN.md section 19, 573-4).
//
// MAME's PS1 CPU takes one cycle per instruction, so a straight run of instructions can be
// compiled ahead of time without changing when anything happens. A recording run (the
// interpreter alone) notes where blocks start and the code each block was, keyed by its words,
// since the game's code lives in RAM, loaded from the CD or the flash. tools/mipsgen.py turns
// the recording into C++ that is built into the game's binary. At run time a block runs in place
// of the interpreter when MAME's CPU is about to execute the same words at the same address, the
// timeslice has room for all of it, and the CPU is in the plain kernel mode the blocks assume.
// Everything else is MAME's interpreter, so a run with blocks is the same, cycle for cycle, as
// a run without (tools/regress.py checks).
//
// Environment:
//   HK_573_RECORD=FILE   interpret, and add the blocks this run executes to FILE (counts are
//                        summed over runs; tools/mipsgen.py reads it)
//   HK_573_BLOCKS=0      interpret everything, even with recompiled blocks built in
//   HK_573_MIPS_STATS=1  print what the blocks covered at exit
//   HK_573_IDLE=0        run waiting loops turn by turn (below)
#pragma once

#include <string>

class psxcpu_device;

namespace hk::k573::mips {

void init(psxcpu_device &cpu);
void finish();            // writes the recording (HK_573_RECORD)
std::string report();     // "" unless HK_573_MIPS_STATS is set

// Waiting loops (runtime/sys573/idle.cpp): a loop that only waits (the library's VSync(): a
// timeout counted down, a counter in RAM compared) is found as the interpreter runs it, and the
// rest of a timeslice's turns of it are counted instead of run, to the same state. For every
// game, with or without recorded blocks; HK_573_IDLE=0 turns it off. init() starts it.
void idle_init(psxcpu_device &cpu);
std::string idle_report();   // "" unless HK_573_MIPS_STATS is set

}  // namespace hk::k573::mips
