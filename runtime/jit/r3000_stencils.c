// SPDX-License-Identifier: GPL-3.0-or-later
// The stencils of the PlayStation's R3000 (the System 573, Twinkle): a block of a game's code is
// made of these at run time (runtime/sys573/mipsjit.cpp), instruction by instruction, as
// tools/mipsgen.py makes a development build's blocks in C++ from a recording. What each must
// equal is MAME's interpreter (psx.cpp) as mipsgen.py writes it out.
//
// s is the psxcpu_device. Kept in registers: base, the timeslice's cycles left when the block
// was entered; dv, MAME's m_delayv (a load on its way, or a branch's target in its delay slot);
// t, the value an instruction has worked out; a, an address, then a memory access's status (0;
// 1: stop after this instruction; 2: a bus error); f, a branch's condition, then where a jump
// by register goes. The registers themselves stay in the device.
// HK_GOTO(0) leaves the straight path: to the instruction's way out, kept after the block.
#define HK_ARGS_DECL char *s, int64_t base, uint32_t dv, uint32_t t, uint32_t a, uint32_t f
#define HK_ARGS s, base, dv, t, a, f
#include "stencil.h"

#include "r3000_layout.h"

typedef uint32_t u32;
typedef int32_t s32;
typedef uint8_t u8;

#define R(h) HK_AT(u32, h)
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#define IMM(h) ((u32)HK_IMM(h))

// the calls out (mipsjit.cpp), their addresses in the device's m_hk_jit
typedef void (HK_ABI *bail_fn)(char *s, u32 pc, u32 dr, u32 dv, s32 icount);
typedef uint64_t (HK_ABI *rd_fn)(char *s, u32 a, s32 icount);            // the value | status << 32
typedef uint64_t (HK_ABI *rdm_fn)(char *s, u32 a, u32 v, s32 icount);    // lwl, lwr: v with the bytes read
typedef u32 (HK_ABI *wr_fn)(char *s, u32 a, u32 v, s32 icount);          // the status
typedef void (HK_ABI *upd_fn)(char *s);
typedef u32 (HK_ABI *gte_rd_fn)(char *s, u32 pc, u32 reg);
typedef void (HK_ABI *gte_wr_fn)(char *s, u32 pc, u32 reg, u32 v);

// ---- an instruction's value: into t, or straight into its register (w_) where nothing is on
// its way through the pipeline. Holes: 0, 1 the operands (a register's offset; a number); 2 the
// register written ----
#define OP2(name, expr) \
    HK_STENCIL(t_##name) { const u32 x = R(H0), y = R(H1); t = (expr); HK_NEXT; } \
    HK_STENCIL(w_##name) { const u32 x = R(H0), y = R(H1); R(H2) = (expr); HK_NEXT; }
OP2(addu, x + y)
OP2(subu, x - y)
OP2(and, x & y)
OP2(or, x | y)
OP2(xor, x ^ y)
OP2(nor, ~(x | y))
OP2(slt, (u32)((s32)x < (s32)y))
OP2(sltu, (u32)(x < y))
OP2(sllv, y << (x & 31))                  // x: rs, the count; y: rt
OP2(srlv, y >> (x & 31))
OP2(srav, (u32)((s32)y >> (x & 31)))
#define OPI(name, expr) \
    HK_STENCIL(t_##name) { const u32 x = R(H0), i = IMM(H1); t = (expr); HK_NEXT; } \
    HK_STENCIL(w_##name) { const u32 x = R(H0), i = IMM(H1); R(H2) = (expr); HK_NEXT; }
OPI(addiu, x + i)
OPI(slti, (u32)((s32)x < (s32)i))
OPI(sltiu, (u32)(x < i))
OPI(andi, x & i)
OPI(ori, x | i)
OPI(xori, x ^ i)
OPI(sll, x << i)                          // i: the count, 0 to 31
OPI(srl, x >> i)
OPI(sra, (u32)((s32)x >> i))
HK_STENCIL(t_const) { t = IMM(H0); HK_NEXT; }
HK_STENCIL(w_const) { R(H1) = IMM(H0); HK_NEXT; }
HK_STENCIL(t_reg) { t = R(H0); HK_NEXT; }
// add, addi, sub: an overflow is the interpreter's (target 0)
HK_STENCIL(t_add_ov) {
    const u32 x = R(H0), y = R(H1), r = x + y;
    if (UNLIKELY((s32)(~(x ^ y) & (x ^ r)) < 0)) HK_GOTO(0);
    t = r;
    HK_NEXT;
}
HK_STENCIL(t_addi_ov) {
    const u32 x = R(H0), y = IMM(H1), r = x + y;
    if (UNLIKELY((s32)(~(x ^ y) & (x ^ r)) < 0)) HK_GOTO(0);
    t = r;
    HK_NEXT;
}
HK_STENCIL(t_sub_ov) {
    const u32 x = R(H0), y = R(H1), r = x - y;
    if (UNLIKELY((s32)((x ^ y) & (x ^ r)) < 0)) HK_GOTO(0);
    t = r;
    HK_NEXT;
}

// ---- the multiplier (MAME works a product out when HI or LO is first read) ----
// mfhi, mflo: 0 m_multiplier_operation, 1 m_hi or m_lo. Target 0: a product still to work out
// (hilo_update, kept out of line: its call, 0, then back to this stencil, target 0)
HK_STENCIL(t_hilo) {
    if (UNLIKELY(HK_AT(s32, H0) != 0)) HK_GOTO(0);
    t = R(H1);
    HK_NEXT;
}
HK_STENCIL(hilo_update) { HK_FN(upd_fn, H0)(s); HK_GOTO(0); }
// mthi, mtlo: 0 m_multiplier_operation, 1 m_hi or m_lo, 2 rs
HK_STENCIL(mt_hilo) { HK_AT(s32, H0) = 0; R(H1) = R(H2); HK_NEXT; }
// mult, multu (and div, divu, which clear HI): 0 m_multiplier_operation, 1, 2 its operands, 3 m_lo,
// 4 rs, 5 rt, 6 the operation, 7 m_hi
HK_STENCIL(mult) {
    const u32 x = R(H4), y = R(H5);
    HK_AT(s32, H0) = HK_IMM(H6);
    R(H1) = x, R(H2) = y, R(H3) = x;
    HK_NEXT;
}
HK_STENCIL(div) {
    const u32 x = R(H4), y = R(H5);
    HK_AT(s32, H0) = HK_IMM(H6);
    R(H1) = x, R(H2) = y, R(H3) = x, R(H7) = 0;
    HK_NEXT;
}

// ---- the pipeline ----
// a load on its way lands: 0 its register
HK_STENCIL(commit) { R(H0) = dv; dv = 0; HK_NEXT; }
// ... at a block's first instruction, where which register (if any) is the CPU's to say:
// 0 m_delayr, 1 m_r[0]
HK_STENCIL(commit_entry) {
    const u32 dr = R(H0);
    if (dr) {
        *(u32 *)(s + HK_DISP(H1) + 4 * (intptr_t)dr) = dv;
        dv = 0;
    }
    HK_NEXT;
}
// ... before a load into register 2 (hole): one still on its way to the same register is dropped
HK_STENCIL(commit_entry_load) {
    const u32 dr = R(H0);
    if (dr == IMM(H2)) dv = 0;
    else if (dr) {
        *(u32 *)(s + HK_DISP(H1) + 4 * (intptr_t)dr) = dv;
        dv = 0;
    }
    HK_NEXT;
}
HK_STENCIL(wr) { R(H0) = t; HK_NEXT; }
HK_STENCIL(dv_t) { dv = t; HK_NEXT; }
HK_STENCIL(dv_zero) { dv = 0; HK_NEXT; }
HK_STENCIL(dv_const) { dv = IMM(H0); HK_NEXT; }
// a register as the pipeline has it (lwl, lwr): the load on its way if it is to this register.
// At a block's first instruction: 0 m_delayr, 1 the register's number, 2 its offset
HK_STENCIL(t_pipe_entry) { t = R(H0) == IMM(H1) ? dv : R(H2); HK_NEXT; }
HK_STENCIL(t_dv) { t = dv; HK_NEXT; }

// ---- branches: the condition in f, then the target (hole 0) in dv if taken ----
HK_STENCIL(c_eq) { f = R(H0) == R(H1); HK_NEXT; }
HK_STENCIL(c_ne) { f = R(H0) != R(H1); HK_NEXT; }
HK_STENCIL(c_lez) { const u32 x = R(H0), y = R(H1); f = (s32)x < 0 || x == y; HK_NEXT; }    // (MAME compares with rt too)
HK_STENCIL(c_gtz) { const u32 x = R(H0), y = R(H1); f = (s32)x >= 0 && x != y; HK_NEXT; }
HK_STENCIL(c_ltz) { f = (s32)R(H0) < 0; HK_NEXT; }
HK_STENCIL(c_gez) { f = (s32)R(H0) >= 0; HK_NEXT; }
HK_STENCIL(dv_br) { dv = f ? IMM(H0) : 0; HK_NEXT; }
// a jump by register, in its delay slot: where it goes, kept in f
HK_STENCIL(f_dv) { f = dv; dv = 0; HK_NEXT; }

// ---- ways off the straight path ----
HK_STENCIL(if_f) { if (f) HK_GOTO(0); HK_NEXT; }
HK_STENCIL(if_a) { if (UNLIKELY(a)) HK_GOTO(0); HK_NEXT; }
// the timeslice has no room for the next stretch: 0 the instructions run so far plus the stretch's
HK_STENCIL(room) { if (UNLIKELY(base < (int64_t)HK_IMM(H0))) HK_GOTO(0); HK_NEXT; }
HK_STENCIL(align3) { if (UNLIKELY(a & 3)) HK_GOTO(0); HK_NEXT; }
// the GTE unusable (SR's CU2 clear): 0 m_cp0r[12]
HK_STENCIL(cu2) { if (UNLIKELY(!(R(H0) & (1u << 30)))) HK_GOTO(0); HK_NEXT; }

// The block ends: MAME's state as the interpreter would have left it. Holes: 0 m_pc, 1 m_delayr,
// 2 m_delayv, 3 m_icount, 4 m_hook_pc; 5 the next pc, 6 m_delayr's value, 7 the instructions
// run, 8 the last instruction's pc
#define LEAVE(PC, DR, IC) { \
    const u32 pc_ = (PC), dr_ = (DR); \
    const s32 ic_ = (IC); \
    R(H0) = pc_, R(H1) = dr_, R(H2) = dv, HK_AT(s32, H3) = ic_, R(H4) = IMM(H8); \
    return 0; }
HK_STENCIL(leave) LEAVE(IMM(H5), IMM(H6), (s32)base - HK_IMM(H7))
HK_STENCIL(leave_f) LEAVE(f, IMM(H6), (s32)base - HK_IMM(H7))                      // on at f
HK_STENCIL(leave_brtk) LEAVE(IMM(H5), f ? 32u : 33u, (s32)base - HK_IMM(H7))       // before a conditional branch's delay slot
// ... after an instruction during which a device cut the timeslice short: MAME's own count, less this one
HK_STENCIL(leave_st) LEAVE(IMM(H5), IMM(H6), HK_AT(s32, H3) - 1)
HK_STENCIL(leave_st_f) LEAVE(f, IMM(H6), HK_AT(s32, H3) - 1)
HK_STENCIL(leave_st_tk) LEAVE(f ? IMM(H5) : IMM(H7), IMM(H6), HK_AT(s32, H3) - 1)  // 5 taken, 7 not

// A branch back to the block's own start (target 0), taken while the timeslice has room for
// the first stretch again and no checked code was written since the block was entered: 0 the
// instructions run, 1 the first stretch, 2 the pointer to the epoch, 3 the epoch at entry,
// 4 m_delayr, 5 its value now. Else on to the leave that follows, which counts none run.
HK_STENCIL(loop) {
    base -= HK_IMM(H0);
    if (base >= (int64_t)HK_IMM(H1) && *HK_AT(const uint64_t *, H2) == HK_AT(uint64_t, H3)) {
        R(H4) = IMM(H5);
        HK_GOTO(0);
    }
    HK_NEXT;
}

// The instruction at pc goes to the interpreter, from the state before it, and the block ends:
// 0 the call, 1 pc, 2 m_delayr's value (bail_entry: its offset), 3 the instructions before it
HK_STENCIL(bail) { HK_FN(bail_fn, H0)(s, IMM(H1), IMM(H2), dv, (s32)base - HK_IMM(H3)); return 0; }
HK_STENCIL(bail_entry) { HK_FN(bail_fn, H0)(s, IMM(H1), R(H2), dv, (s32)base - HK_IMM(H3)); return 0; }
HK_STENCIL(bail_tk) { HK_FN(bail_fn, H0)(s, IMM(H1), f ? 32u : 33u, dv, (s32)base - HK_IMM(H3)); return 0; }

// ---- memory: a page of RAM or ROM read and written in place; anything else by the access's
// slow half, kept out of line (the scratchpad, else MAME's handlers by a call), which comes back
// to the stencil after the access ----
HK_STENCIL(addr) { a = R(H0) + IMM(H1); HK_NEXT; }
// loads: 0 the page table. Target 0: the interpreter's (an address not aligned); 1: the slow half
#define LOAD(name, align, type, conv) \
    HK_STENCIL(name) { \
        if (align && UNLIKELY(a & align)) HK_GOTO(0); \
        const u8 *p = HK_AT(const struct hk_r3000_page *, H0)[a >> HK_R3000_PAGE_BITS].direct_r; \
        if (UNLIKELY(p == 0)) HK_GOTO(1); \
        type v; \
        __builtin_memcpy(&v, p + (a & 0xffff), sizeof v); \
        t = conv v, a = 0; \
        HK_NEXT; \
    } \
    /* 0 the scratchpad, 1 the call, 2 the instructions before this one. Target 0: the */ \
    /* interpreter's (a bus error); 1: back after the load */ \
    HK_STENCIL(name##_slow) { \
        const u8 *sp = HK_AT(const u8 *, H0); \
        if ((a & ~HK_R3000_SP_MASK) == HK_R3000_SP_BASE && sp) { \
            type v; \
            __builtin_memcpy(&v, sp + (a & HK_R3000_SP_MASK), sizeof v); \
            t = conv v, a = 0; \
            HK_GOTO(1); \
        } \
        const uint64_t r = HK_FN(rd_fn, H1)(s, a, (s32)base - HK_IMM(H2)); \
        t = conv (type)r, a = (u32)(r >> 32); \
        if (UNLIKELY(a == 2)) HK_GOTO(0); \
        HK_GOTO(1); \
    }
LOAD(ld32, 3, u32, (u32))
LOAD(ld16u, 1, uint16_t, (u32))
LOAD(ld16s, 1, int16_t, (u32)(s32))
LOAD(ld8u, 0, u8, (u32))
LOAD(ld8s, 0, int8_t, (u32)(s32))
// stores: 0 the page table, 1 the register stored (st32t: t is). The slow half: 0 the scratchpad,
// 1 the call, 2 the instructions before this one, 3 the register stored. Targets as the loads'
#define STORE(name, align, type, value, hpages, value_slow) \
    HK_STENCIL(name) { \
        if (align && UNLIKELY(a & align)) HK_GOTO(0); \
        u8 *p = HK_AT(const struct hk_r3000_page *, hpages)[a >> HK_R3000_PAGE_BITS].direct_w; \
        if (UNLIKELY(p == 0)) HK_GOTO(1); \
        const type v = (type)(value); \
        __builtin_memcpy(p + (a & 0xffff), &v, sizeof v); \
        a = 0; \
        HK_NEXT; \
    } \
    HK_STENCIL(name##_slow) { \
        const u32 w = (value_slow); \
        u8 *sp = HK_AT(u8 *, H0); \
        if ((a & ~HK_R3000_SP_MASK) == HK_R3000_SP_BASE && sp) { \
            const type v = (type)w; \
            __builtin_memcpy(sp + (a & HK_R3000_SP_MASK), &v, sizeof v); \
            a = 0; \
            HK_GOTO(1); \
        } \
        a = HK_FN(wr_fn, H1)(s, a, w, (s32)base - HK_IMM(H2)); \
        if (UNLIKELY(a == 2)) HK_GOTO(0); \
        HK_GOTO(1); \
    }
STORE(st32, 3, u32, R(H1), H0, R(H3))
STORE(st32t, 3, u32, t, H0, t)
STORE(st16, 1, uint16_t, R(H1), H0, R(H3))
STORE(st8, 0, u8, R(H1), H0, R(H3))
// lwl, lwr (t: the register as the pipeline has it) and swl, swr, all by their calls: 0 the call,
// 1 the instructions before this one, 2 the register stored
HK_STENCIL(ld_lr) {
    const uint64_t r = HK_FN(rdm_fn, H0)(s, a, t, (s32)base - HK_IMM(H1));
    t = (u32)r, a = (u32)(r >> 32);
    if (UNLIKELY(a == 2)) HK_GOTO(0);
    HK_NEXT;
}
HK_STENCIL(st_lr) {
    a = HK_FN(wr_fn, H0)(s, a, R(H2), (s32)base - HK_IMM(H1));
    if (UNLIKELY(a == 2)) HK_GOTO(0);
    HK_NEXT;
}

// ---- the GTE (COP2), by MAME's own: 0 the call, 1 the pc (for its log), 2 the register or the
// function, 3 the register moved ----
HK_STENCIL(t_gte) { t = HK_FN(gte_rd_fn, H0)(s, IMM(H1), IMM(H2)); HK_NEXT; }
HK_STENCIL(gte_wr) { HK_FN(gte_wr_fn, H0)(s, IMM(H1), IMM(H2), R(H3)); HK_NEXT; }
HK_STENCIL(gte_wr_t) { HK_FN(gte_wr_fn, H0)(s, IMM(H1), IMM(H2), t); HK_NEXT; }
HK_STENCIL(gte_op) { HK_FN(gte_wr_fn, H0)(s, IMM(H1), IMM(H2), 0); HK_NEXT; }
