// SPDX-License-Identifier: GPL-3.0-or-later
// R3000 blocks made into the host's code at run time (PLAN.md section 45): what a release has in
// place of the blocks a development build compiles from a recording (mips.h, tools/mipsgen.py).
// A block is made of the stencils of runtime/jit/r3000_stencils.c, instruction by instruction, by
// the rules mipsgen.py writes C++ by, so it does what MAME's interpreter does for those
// instructions, in the same order, with the same results, and ends at the same points. Which
// blocks are made, when they may run and how written code is noticed is mips.cpp's, the same as
// for the built-in ones.
//
//   HK_573_JIT=0 (or HK_JIT=0)   none made: built-in blocks and the interpreter only
#pragma once

#include <cstddef>
#include <cstdint>

class psxcpu_device;

namespace hk::k573::mips::jit {

// stencils for this processor in this build, and not turned off
bool available();
// the calls the made code uses, into the device
void init(psxcpu_device &d);
// the CPU's page table and the scratchpad (null where it is not plain memory) as they are now
void map_changed(psxcpu_device &d, uint8_t *scratchpad);

struct Made {
    void *entry;       // for enter()
    uint32_t len;      // its first stretch: the timeslice must have room for it
    uint32_t nwords;   // how many of the words it was made from (the code to check before a run)
};
// The block for the code at pc, words[0..n) as the recorder forms a block. False: none (no
// memory, or nothing worth making).
bool make(psxcpu_device &d, uint32_t pc, const uint32_t *words, int n, Made &out);
// run a made block: as a built-in block's function is called. epoch: hk_psx::epoch now.
void enter(psxcpu_device &d, void *entry, uint64_t epoch);

size_t code_bytes();   // executable memory used

}  // namespace hk::k573::mips::jit
