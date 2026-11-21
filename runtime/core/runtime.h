// SPDX-License-Identifier: GPL-3.0-or-later
// HIRAKU runtime core: internal C++ interface.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "hiraku/recomp.h"

namespace hk {

// ---------------------------------------------------------------------------
// PPC403GCX state that is not in hk_ctx (privileged SPRs, DCRs, time base)
// ---------------------------------------------------------------------------
namespace spr {
constexpr uint32_t XER = 1, LR = 8, CTR = 9, SRR0 = 26, SRR1 = 27;
constexpr uint32_t SPRG0 = 272, SPRG1 = 273, SPRG2 = 274, SPRG3 = 275, PVR = 287;
constexpr uint32_t ESR = 0x3d4, DEAR = 0x3d5, EVPR = 0x3d6, TSR = 0x3d8, TCR = 0x3da, PIT = 0x3db;
constexpr uint32_t TBHI = 0x3dc, TBLO = 0x3dd, SRR2 = 0x3de, SRR3 = 0x3df;
constexpr uint32_t TBHU = 0x3cc, TBLU = 0x3cd;   // user-mode read-only aliases of the time base
}  // namespace spr

namespace dcr {
constexpr uint32_t EXISR = 0x40, EXIER = 0x42, IOCR = 0xa0;
}

namespace msr {
constexpr uint32_t WE = 0x40000, CE = 0x20000, EE = 0x8000, PR = 0x4000, ME = 0x1000, DE = 0x200, IR = 0x20, DR = 0x10;
}

constexpr uint32_t PVR_403GCX = 0x00201400;

struct Cpu403 {
    uint32_t sprs[1024] = {};   // privileged SPRs not mirrored in hk_ctx
    uint32_t dcrs[1024] = {};
    int64_t tb_offset = 0;      // time base = virtual time (sched::now) + offset
};
extern Cpu403 g_cpu;

// ---------------------------------------------------------------------------
// Memory and the MMIO bus
// ---------------------------------------------------------------------------
void ram_init();                                   // allocate and zero hk_ram
bool ram_load(const std::string &path, uint32_t phys);

struct MmioHandler {
    std::function<uint32_t(uint32_t phys, int size)> read;
    std::function<void(uint32_t phys, uint32_t value, int size)> write;
};
// Install the handler for all non-RAM accesses (devices arrive in M2; M1 uses a logger).
void mmio_set_handler(MmioHandler h);

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------
hk_func lookup(uint32_t guest_addr);               // nullptr if not recompiled
// A function at the address, recompiled or the executor's (core/exec.h: a release's
// hiraku-firebeat): has_guest says whether there is one, call_guest runs it (false: none)
bool has_guest(uint32_t guest_addr);
bool call_guest(hk_ctx *c, uint32_t guest_addr);
// Functions a mod's code patch changed run interpreted from then on (core/patched.cpp): the
// program's functions overlapping [lo, hi) marked, and whether an entry is one of theirs; the
// entries marked so far (it only goes up: a lifted patch's function stays marked)
namespace spec { struct Program; }
void patch_functions(const spec::Program &p, uint32_t lo, uint32_t hi);
bool patched(uint32_t entry);
uint32_t patched_count();

// ---------------------------------------------------------------------------
// Interpreter
// ---------------------------------------------------------------------------
struct InterpOptions {
    bool call_native = true;      // `bl` to a recompiled function runs the native version
    bool lle = false;             // exceptions handled purely in the interpreter (no fibers): --interp-only
    unsigned shadow_checks = 0;   // per function: run native AND interpreted, compare (0 = off)
    bool trace = false;           // print every instruction
};
extern InterpOptions g_interp;

enum class Stop { returned, max_steps, halted };

struct InterpStats {
    uint64_t insns = 0, native_calls = 0, exceptions = 0;
    uint64_t shadow_ok = 0, shadow_bad = 0, shadow_impure = 0;
    uint64_t threads_started = 0, thread_switches = 0, native_vectors = 0;
};
extern InterpStats g_stats;

// Interpret from c->pc until c->pc == stop_pc with r1 == stop_sp (a function
// returning), max_steps instructions, or a halt (hang loop / fatal vector).
Stop interp_run(hk_ctx *c, uint32_t stop_pc, uint32_t stop_sp, uint64_t max_steps);
bool interp_step(hk_ctx *c);                 // false: an interrupt was taken instead of an instruction
void take_exception(hk_ctx *c, uint32_t vector, uint32_t return_pc);
bool shadow_native_active();                 // native half of a shadow check is running
bool shadow_interp_active();                 // interpreted half of a shadow check is running

// Exception glue and guest threads (glue.cpp)
void glue_init(hk_ctx *c);
void glue_exception(hk_ctx *c, uint32_t vector, uint32_t ret);
[[noreturn]] void shadow_abandon();          // ...and just hit something impure (sc)

[[noreturn]] void fatal(hk_ctx *c, const char *fmt, ...);
extern std::function<void()> g_on_fatal;   // e.g. print run statistics before exiting
void dump_ctx(const hk_ctx *c, FILE *f);

}  // namespace hk
