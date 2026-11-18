// SPDX-License-Identifier: GPL-3.0-or-later
// The PPC403 interpreter: the fallback for code the static pass did not
// cover, the engine for the assembly glue during bring-up, and the second
// opinion in shadow mode. Semantics come from include/hiraku/ppc.h, exactly
// as in recompiled code.
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <memory>
#include <unordered_map>

#include "../../src/ppc/cycles.h"
#include "../../src/ppc/decode.h"
#include "cpu403.h"
#include "hooks.h"
#include "runtime.h"
#include "sched.h"

namespace hk {

InterpOptions g_interp;
InterpStats g_stats;

namespace {

int g_shadow_depth = 0;                                // >0 while interpreting a shadowed call
std::unordered_map<uint32_t, unsigned> g_shadow_count; // checks done per function
std::jmp_buf g_shadow_escape;                          // native side hit a syscall: abandon the check
bool g_shadow_native = false;

uint32_t fetch(hk_ctx *c, uint32_t pc) {
    uint32_t p = HK_PHYS(pc);
    if (p > HK_RAM_SIZE - 4) fatal(c, "instruction fetch outside RAM at %08x", pc);
    uint32_t w;
    memcpy(&w, hk_ram + p, 4);
    return hk_bswap32(w);
}

void exec_ordinary(hk_ctx *c, const Insn &i) {
    switch (i.op) {
#define HK_I0(n) case Op::n: op_##n(c); break;
#define HK_I1(n, a) case Op::n: op_##n(c, i.a); break;
#define HK_I2(n, a, b) case Op::n: op_##n(c, i.a, i.b); break;
#define HK_I3(n, a, b, d) case Op::n: op_##n(c, i.a, i.b, i.d); break;
#define HK_I4(n, a, b, d, e) case Op::n: op_##n(c, i.a, i.b, i.d, i.e); break;
#define HK_I5(n, a, b, d, e, f) case Op::n: op_##n(c, i.a, i.b, i.d, i.e, i.f); break;
#define HK_I6(n, a, b, d, e, f, g) case Op::n: op_##n(c, i.a, i.b, i.d, i.e, i.f, i.g); break;
        HK_OPLIST(HK_I0, HK_I1, HK_I2, HK_I3, HK_I4, HK_I5, HK_I6)
#undef HK_I0
#undef HK_I1
#undef HK_I2
#undef HK_I3
#undef HK_I4
#undef HK_I5
#undef HK_I6
    default:
        fatal(c, "interp: no semantics for %s (%08x) at %08x", op_name(i.op), i.word, i.addr);
    }
}

// Shadow check: run the native function and the interpreter on the same
// starting state and compare registers and all of RAM. A call that enters the
// kernel (sc, or an exception while interpreting) is not a pure function of
// that state - other threads may run - so such checks are rolled back and
// counted as "impure" instead. The interpreted result always stands.
bool shadow_call(hk_ctx *c, hk_func fn, uint32_t target, uint32_t ret) {
    unsigned &n = g_shadow_count[target];
    if (n >= g_interp.shadow_checks || g_shadow_depth > 0) return false;
    n++;
    static std::unique_ptr<uint8_t[]> ram0(new uint8_t[HK_RAM_SIZE]), ram1(new uint8_t[HK_RAM_SIZE]);
    static hk_ctx c0, cn;
    static Cpu403 cpu0;
    c0 = *c;
    memcpy(ram0.get(), hk_ram, HK_RAM_SIZE);
    cpu0 = g_cpu;
    auto restore = [&]() {
        *c = c0;
        memcpy(hk_ram, ram0.get(), HK_RAM_SIZE);
        g_cpu = cpu0;
    };

    // native
    if (setjmp(g_shadow_escape) == 0) {
        g_shadow_native = true;
        c->lr = ret;
        fn(c);
        g_shadow_native = false;
    } else {
        g_shadow_native = false;
        restore();
        g_stats.shadow_impure++;
        return false;               // caller interprets the call normally
    }
    cn = *c;
    memcpy(ram1.get(), hk_ram, HK_RAM_SIZE);

    // interpreted, from the same state
    restore();
    c->lr = ret;
    c->pc = target;
    uint64_t exc0 = g_stats.exceptions;
    g_shadow_depth++;
    Stop s = interp_run(c, ret, c0.r[1], 50'000'000);
    g_shadow_depth--;
    if (s != Stop::returned) {          // e.g. spinning on a device that does not exist yet
        restore();
        g_stats.shadow_impure++;
        return false;
    }
    if (g_stats.exceptions != exc0) {   // the kernel ran in between: not comparable
        g_stats.shadow_impure++;
        return true;
    }

    bool ok = memcmp(cn.r, c->r, sizeof c->r) == 0 && cn.cr == c->cr && cn.xer == c->xer &&
              cn.ctr == c->ctr && cn.lr == c->lr && cn.msr == c->msr;
    size_t bad_at = SIZE_MAX;
    if (memcmp(ram1.get(), hk_ram, HK_RAM_SIZE) != 0) {
        ok = false;
        for (size_t k = 0; k < HK_RAM_SIZE; k++)
            if (ram1[k] != hk_ram[k]) { bad_at = k; break; }
    }
    if (ok) {
        g_stats.shadow_ok++;
    } else {
        g_stats.shadow_bad++;
        fprintf(stderr, "shadow MISMATCH in f_%08x (call %u)\n", target, n);
        for (int r = 0; r < 32; r++)
            if (cn.r[r] != c->r[r]) fprintf(stderr, "  r%d native %08x interp %08x\n", r, cn.r[r], c->r[r]);
        if (cn.cr != c->cr) fprintf(stderr, "  cr  native %08x interp %08x\n", cn.cr, c->cr);
        if (cn.xer != c->xer) fprintf(stderr, "  xer native %08x interp %08x\n", cn.xer, c->xer);
        if (cn.ctr != c->ctr) fprintf(stderr, "  ctr native %08x interp %08x\n", cn.ctr, c->ctr);
        if (bad_at != SIZE_MAX)
            fprintf(stderr, "  RAM first differs at %08zx: native %02x interp %02x\n", bad_at, ram1[bad_at], hk_ram[bad_at]);
    }
    return true;
}

}  // namespace

void take_exception(hk_ctx *c, uint32_t vector, uint32_t return_pc) {
    if (vector == 0x100 || vector == 0x1020) {      // critical class: SRR2/3
        c->srr2 = return_pc;
        c->srr3 = c->msr;
        c->msr &= ~(msr::WE | msr::CE | msr::EE | msr::PR | msr::DE | msr::IR | msr::DR);
    } else {
        c->srr0 = return_pc;
        c->srr1 = c->msr;
        c->msr &= ~(msr::WE | msr::EE | msr::PR | msr::IR | msr::DR);
    }
    c->pc = (g_cpu.sprs[spr::EVPR] & 0xffff0000u) | vector;
}

bool interp_step(hk_ctx *c) {
    if (hk_budget <= 0) {
        if (shadow_native_active()) hk_budget = 1 << 20;
        else sched_sync();
    }
    // interrupts are taken between instructions (not while a shadow check interprets)
    if ((c->msr & msr::EE) && g_shadow_depth == 0 && cpu403::irq_pending()) {
        uint32_t v = cpu403::pending_vector();
        if (g_interp.lle) take_exception(c, v, c->pc);
        else glue_exception(c, v, c->pc);
        return false;               // no instruction executed (and pc may be unchanged)
    }
    const uint32_t pc = c->pc;
    // a mod's hooks (core/hooks.h), before the instruction as in the executor: here, past the
    // interrupt above (which leaves this instruction for later, its hooks with it)
    if (__builtin_expect(hooks::g_any, 0)) hooks::run_at(c, pc);
    const Insn i = decode(fetch(c, pc), pc);
    // time: what MAME charges this instruction (src/ppc/cycles.h)
    {
        const uint32_t pp = HK_PHYS(pc - 4);
        uint32_t prev = 0;
        bool has_prev = pc >= 4 && pp <= HK_RAM_SIZE - 4;
        if (has_prev) {
            memcpy(&prev, hk_ram + pp, 4);
            prev = hk_bswap32(prev);
            has_prev = !ppc::ends_sequence(prev);
        }
        hk_budget -= ppc::cycles(i.word, pc, has_prev, prev);
    }
    if (g_interp.trace) fprintf(stderr, "%08x: %08x %s\n", pc, i.word, disasm(i).c_str());
    g_stats.insns++;
    uint32_t next = pc + 4;

    switch (i.op) {
    case Op::b:
        if (i.lk) {
            c->lr = next;
            if (g_shadow_depth == 0 && i.target != next) {
                if (has_guest(i.target)) {   // recompiled, or the executor's (call_guest)
                    hk_func fn = lookup(i.target);
                    if (fn && g_interp.shadow_checks && shadow_call(c, fn, i.target, next)) {
                        c->pc = next;       // checked; the interpreted result stands
                        return true;
                    }
                    if (g_interp.call_native) {
                        g_stats.native_calls++;
                        call_guest(c, i.target);
                        if (c->pc != next) fatal(c, "native %08x returned to %08x, expected %08x", i.target, c->pc, next);
                        c->pc = next;
                        return true;
                    }
                }
            }
        }
        next = i.target;
        break;
    case Op::bc: {
        bool ok = hk_bc_ok(c, i.bo, i.bi);
        if (i.lk) c->lr = pc + 4;
        if (ok) next = i.target;
        break;
    }
    case Op::bclr: {
        uint32_t t = c->lr & ~3u;
        bool ok = hk_bc_ok(c, i.bo, i.bi);
        if (i.lk) c->lr = pc + 4;
        if (ok) next = t;
        break;
    }
    case Op::bcctr: {
        bool ok = hk_bc_ok(c, i.bo | 4, i.bi);     // bcctr never decrements CTR
        if (i.lk) c->lr = pc + 4;
        if (ok) next = c->ctr & ~3u;
        break;
    }
    case Op::sc:
        if (g_interp.lle || !(c->msr & msr::PR)) take_exception(c, 0xc00, pc + 4);
        else glue_exception(c, 0xc00, pc + 4);   // user code on a fiber: the kernel may switch threads
        if (g_interp.lle || !(c->msr & msr::PR)) g_stats.exceptions++;
        return true;
    case Op::rfi:
        c->msr = c->srr1;
        c->pc = c->srr0 & ~3u;
        if (cpu403::irq_pending()) sched_kick();
        return true;
    case Op::rfci:
        c->msr = c->srr3;
        c->pc = c->srr2 & ~3u;
        return true;
    case Op::nop:
        break;
    case Op::unimpl:
    case Op::invalid:
        hk_unimplemented(c, pc, i.word);
        break;
    default:
        exec_ordinary(c, i);
        break;
    }
    c->pc = next;
    return true;
}

Stop interp_run(hk_ctx *c, uint32_t stop_pc, uint32_t stop_sp, uint64_t max_steps) {
    for (uint64_t n = 0; n < max_steps; n++) {
        if (c->pc == stop_pc && c->r[1] == stop_sp) return Stop::returned;
        uint32_t pc = c->pc;
        if (interp_step(c) && c->pc == pc) return Stop::halted;   // `b .`: a hang (fatal vectors end this way)
    }
    return Stop::max_steps;
}

bool shadow_native_active() { return g_shadow_native; }
bool shadow_interp_active() { return g_shadow_depth > 0; }
[[noreturn]] void shadow_abandon() { longjmp(g_shadow_escape, 1); }

}  // namespace hk
