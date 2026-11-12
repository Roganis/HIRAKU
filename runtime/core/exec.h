// SPDX-License-Identifier: GPL-3.0-or-later
// The executor: a Firebeat program's functions run without a compiler (PLAN.md section 27,
// FB-3a), for a release's hiraku-firebeat, which has no game's code in it. At start it reads
// the game's recompile spec (recomp.txt) and program image and turns each function into steps,
// decision for decision as hiraku-recomp turns it into C (recomp/main.cpp): the same op_* calls
// with the same operands, the same cycle charges (HK_TICK) at the same places, the same polls,
// calls, returns, jump tables and fall-throughs. So a run is the native build's run, frame for
// frame, only slower.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "hiraku/ppc.h"

namespace hk::spec { struct Program; }

namespace hk::exec {

// The game's functions from `spec` (analyze/prep.py's recomp.txt) over `image` (its program.bin).
// "" when ready, else what is wrong.
std::string load(const std::string &spec, const std::string &image);
// ... from the spec's text (src/ppc/analyze.h's, made at start)
std::string load_text(const std::string &spec_text, const std::string &image);
bool loaded();
size_t count();                          // functions, secondary entries included

bool has(uint32_t addr);                 // a function (or secondary entry) starts at addr
// the functions loaded (their ranges: core/patched.cpp), nullptr before load()
const spec::Program *program();
bool call(hk_ctx *c, uint32_t addr);     // run it, as a recompiled function is run; false: none there
// ... an exception's handler: run, or what its run did the last time it started from this state
// done in its place (exec.cpp, "runs remembered"). False: none there, or not this way (call())
bool call_memo(hk_ctx *c, uint32_t addr);

}  // namespace hk::exec
