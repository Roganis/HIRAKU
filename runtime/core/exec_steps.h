// SPDX-License-Identifier: GPL-3.0-or-later
// The executor's own (exec.h): a function as steps, and what a run of one did, kept to do again
// without running it (exec.cpp, "runs remembered"). For exec.cpp and exec_rec.cpp only.
#pragma once

#include <cstddef>   // offsetof
#include <cstdint>
#include <utility>
#include <vector>

#include "hiraku/recomp.h"
#include "ppc/decode.h"

namespace hk::exec {

// What a step does. The first ones are the emitter's lines of control flow (recomp/main.cpp);
// from K_op on, an ordinary instruction's op_* (three kinds each, see kind()).
enum K : uint16_t {
    K_bad,        // (never built)
    K_tick,       // HK_TICK(a)
    K_poll,       // HK_POLL(c, a, ...)
    K_go,         // goto: on at step a
    K_bc,         // if (!hk_bc_ok(c, bo, bi)) on at step a: the branch's "then" steps follow
    K_tail,       // f(c); return;  (a tail call: the callee's steps replace these)
    K_jump,       // hk_jump(c, a); return;
    K_call,       // HK_CALL(c, f, a)
    K_call_up,    // c->lr = a; f(c); return;  (a helper that returns to our caller)
    K_call_dyn,   // HK_CALL_DYN(c, b, a)
    K_set_lr,     // c->lr = a;
    K_bclrl,      // { t = c->lr & ~3; c->lr = a; if (hk_bc_ok) HK_CALL_IND(c, t, a); }
    K_ret,        // HK_RET(c)
    K_ret_if,     // if (hk_bc_ok) HK_RET(c)
    K_bcctrl,     // { t = c->ctr & ~3; c->lr = a; [if (hk_bc_ok)] HK_CALL_IND(c, t, a); }
    K_table,      // [if (hk_bc_ok)] switch (c->ctr & ~3) { the jump table's cases; default: hk_jump; return }
    K_jump_ctr,   // [if (hk_bc_ok)] { hk_jump(c, c->ctr & ~3); return; }
    K_sc,         // c->pc = a; hk_syscall(c);
    K_rfi,        // c->msr = c->srr1; c->pc = c->srr0 & ~3; return;
    K_rfci,       // c->msr = c->srr3; c->pc = c->srr2 & ~3; return;
    K_unimpl,     // hk_unimplemented(c, a, b); return;
    // A branch inside the function, its lines in one step (what the emitter writes for it: the
    // condition, a poll where it goes backwards, the goto)
    K_gop,        // HK_POLL(c, b, ...); goto: on at step a
    K_br,         // if (hk_bc_ok(c, bo, bi)) goto: on at step a
    K_brp,        // if (hk_bc_ok(c, bo, bi)) { HK_POLL(c, b, ...); goto: on at step a }
    // The commonest shapes of a step above or of an instruction, with what is fixed in them given
    // to the same hk_bc_ok or op_* as a constant (the compiler folds it): done the same, sooner.
    K_bc_true,    // K_bc where the branch is taken on a condition bit set (BO 011xx)
    K_bc_false,   // ... clear (BO 001xx)
    K_bc_dnz,     // ... on the count register, decremented, not zero (BO 1x00x)
    K_br_true, K_br_false, K_br_dnz,      // K_br, K_brp the same three ways
    K_brp_true, K_brp_false, K_brp_dnz,
    K_cmpi0,      // cmpi, cmpli, cmp, cmpl into condition field 0
    K_cmpli0,
    K_cmp0,
    K_cmpl0,
    K_mflr,       // mfspr, mtspr of the link and count registers
    K_mfctr,
    K_mtlr,
    K_mtctr,
    K_op = 64,    // kind(op, shape)
};

// An ordinary instruction's step: its op_* with the instruction's fields (shape 0), or, where it
// is neither a record nor an overflow form, with rc and oe 0 and rA known not to be register 0
// (shape 1) or to be it (shape 2), which is "0" to a load, a store or an addi.
constexpr int SHAPES = 3;
constexpr uint16_t kind(Op op, int shape) { return (uint16_t)(K_op + SHAPES * (int)op + shape); }
constexpr size_t KINDS = K_op + SHAPES * ((size_t)Op::unimpl + 1);

struct Code;

// 32 bytes, two to a cache line: the handler first, then what it reads
struct Step {
    const void *h = nullptr;   // run()'s handler for k (exec.cpp's; the recorder's dispatcher goes by k)
    union X {
        const Code *fn;                                           // tail, call, call_up: the callee
        struct F { uint8_t mb, me, nb, crm, to, e, oe, rc; } f;   // an instruction's fields
        constexpr X() : fn(nullptr) {}
    } x;
    uint32_t a = 0;            // go, bc, br: the step to go on at; calls, sc, set_lr: the return address; tick: the cycles;
                               // an instruction's spr or dcr number (tw, twi: its address)
    union Y {
        uint32_t b;                                         // call_dyn's target, unimpl's word, table's index, a poll's address
        struct F { uint8_t crfd, crfs; int16_t imm; } f;    // an instruction's fields: imm is simm, and uimm's 16 bits
        constexpr Y() : b(0) {}
    } y;
    uint16_t k = K_bad;
    uint16_t tick = 0;         // HK_TICK folded in: charged when the step is reached (see fold())
    uint8_t rt = 0, ra = 0, rb = 0;   // an instruction's register fields; a branch's bo, bi, and "always"
                                      // (bcctr's: no condition is evaluated, the emitter writes no `if`)
    uint8_t sh = 0;
};
static_assert(sizeof(Step) <= 32, "the executor's step: two to a cache line");

struct Code {
    uint32_t entry = 0;   // where it's entered (a function's entry, or a secondary one)
    std::vector<Step> steps;
    // table steps' cases, by step: (value, step to go to, or ~0u: hk_jump(c, value))
    std::vector<std::vector<std::pair<uint32_t, uint32_t>>> tables;
};

// the instruction a step of kind k does: Op::invalid for control flow
constexpr Op op_of(uint16_t k) {
    switch (k) {
    case K_cmpi0: return Op::cmpi;
    case K_cmpli0: return Op::cmpli;
    case K_cmp0: return Op::cmp;
    case K_cmpl0: return Op::cmpl;
    case K_mflr: case K_mfctr: return Op::mfspr;
    case K_mtlr: case K_mtctr: return Op::mtspr;
    default: return k >= K_op ? (Op)((k - K_op) / SHAPES) : Op::invalid;
    }
}

const Code *find(uint32_t addr);        // the function entered at addr, or null
void run(hk_ctx *c, const Code *k);     // a recompiled function's body, run (exec.cpp)

// ---------------------------------------------------------------------------------------------
// A run remembered. What a run of a function does is decided by the registers it starts with and
// by what it reads; one that reads only main RAM (no device, no clock) and lets no time-keeping
// in (no poll that finds the budget spent, no interrupt) does the same again whenever it starts
// from the same registers and finds the same bytes where it read. So: the registers before and
// after, the bytes read (as they were before the run wrote any of them), the bytes written (as
// the run left them) and the cycles it took.
//
// Not every word a run touches decides what it does. An exception's handler stores the thread's
// registers in its frame and loads the next thread's from another, whatever they hold: words that
// are only moved, whole, between a general register and RAM (stw, lwz, stmw, lmw) and are never
// an operand of anything else are kept as copies, from where to where, and what they hold is
// neither compared nor remembered.
// ---------------------------------------------------------------------------------------------
struct Span { uint32_t at, n, data; };   // n bytes of RAM from physical address `at`; theirs at data[]

// a word moved as it is: from a general register as the run found it, or from RAM as the run
// found it, to a general register or to RAM as the run leaves them
struct Copy {
    uint32_t from, to;         // a register's number, or a physical address
    bool from_ram, to_ram;
};

constexpr size_t CTX_BYTES = offsetof(hk_ctx, reserve) + sizeof(uint32_t);   // the registers (not the runtime's fiber)
constexpr size_t CTX_GPRS = offsetof(hk_ctx, cr);                            // ... the general ones come first
constexpr size_t MAX_COPIES = 160;

struct Memo {
    hk_ctx in{}, out{};
    uint32_t used = 0;          // the general registers whose value going in decides the run (a bit each)
    int32_t cycles = 0;
    std::vector<Span> reads, writes;
    std::vector<uint8_t> rdata, wdata;
    std::vector<Copy> copies;
};

// Run k as run() does, noting what it reads and writes (exec_rec.cpp: the same steps over
// accessors that keep a list). True: m is what it did, and it is a run that can be remembered.
// False: it wasn't one (the run itself is as right either way).
bool record(hk_ctx *c, const Code *k, Memo &m);
bool recording();   // inside record()

}  // namespace hk::exec
