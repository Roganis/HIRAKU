// SPDX-License-Identifier: GPL-3.0-or-later
// Guest hooks (docs/MODS.md, "Hooks, settings and actions"): a mod's function run before an
// instruction of the program, wherever that instruction runs: the executor's steps (exec.cpp: a
// probe step put in front of the instruction's own when the functions are built) and the
// interpreter (interp.cpp: a function a mod's patch changed, a call the analysis missed,
// --interp-only). The table is filled before exec::load(), which then refuses an address in none
// of the program's functions (a hook there would never run). Several hooks at one address run in
// the order they were added.
//
// With no hook added, nothing changes: the executor builds no probe step and the interpreter
// looks at one flag. A run of the executor that passes a hook is never remembered (exec.cpp,
// "runs remembered"): the hook runs every time.
#pragma once

#include <cstdint>
#include <string>

#include "hiraku/ppc.h"

namespace hk::hooks {

// A hook: ctx as given to add(); regs the machine's registers as 32-bit words, numbered as
// Core::mod_registers() names them (the PowerPC: r0-r31, cr, xer, lr, ctr: hk_ctx's first 36
// words). It may change them and RAM; the instruction then runs with what it left.
using Fn = void (*)(void *ctx, uint32_t *regs);

// A hook at the instruction at `at` (the program's own address); `label` names it in a message
// (a mod's file and line). False, with why in err, after the program's functions are loaded.
bool add(uint32_t at, Fn fn, void *ctx, const std::string &label, std::string &err);
void clear();   // (the tests: another program)

extern bool g_any;   // a hook was added (the interpreter's one look)

// For exec.cpp: the hooks at `at` as one number (a probe step's), ~0u for none; run them; and,
// once the program's functions are built, a message for each address no probe was built for
uint32_t slot(uint32_t at);
void run(hk_ctx *c, uint32_t slot);
void placed(uint32_t slot);
std::string unplaced();
void loaded(bool yes);   // the executor's functions are being built (false) / built (add() is too late then)

// For the interpreter: the hooks at `at`, if any
void run_at(hk_ctx *c, uint32_t at);

}  // namespace hk::hooks
