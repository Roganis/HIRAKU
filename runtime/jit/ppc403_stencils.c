// SPDX-License-Identifier: GPL-3.0-or-later
// The stencils of Firebeat's PowerPC 403: a function of the program, as the executor's steps
// (runtime/core/exec.cpp), is made of these at run time (runtime/core/exec_jit.cpp), step by step.
// What each must equal is the interpreter of steps (runtime/core/exec_run.inc), which is the
// instruction set's own C (include/hiraku/ppc.h): the instructions are that header's op_*,
// called with their fields as holes; the rest is exec_run.inc's control flow, line for line.
//
// s is the guest's registers (hk_ctx); g the made function's block (ppc403_layout.h). Kept in
// registers: ram, hk_ram; t, an address on its way from one stencil to the next (or a rotation's
// count: x86's shifts count in the register t is passed in); b, hk_budget (written back before
// every call out and every way out, read again after a call); u, spare.
// HK_GOTO(0): a branch's target, or a memory access's slow half (kept after the function's steps)
// and back; HK_GOTO(1): a branch's poll, out of line.
#define HK_ARGS_DECL char *s, char *g, unsigned char *ram, uint32_t t, int32_t b, uint32_t u
#define HK_ARGS s, g, ram, t, b, u
#include "stencil.h"

#include "ppc403_layout.h"

// the instruction set's semantics, the one definition of them (memory accesses are this file's:
// HK_MEM_HOOKS leaves them out, and no op_* that uses one has a stencil)
#define HK_MEM_HOOKS 1
#include "../../include/hiraku/ppc.h"
static inline uint32_t hk_r8(hk_ctx *c, uint32_t ea) { (void)c, (void)ea; __builtin_trap(); }
static inline uint32_t hk_r16(hk_ctx *c, uint32_t ea) { (void)c, (void)ea; __builtin_trap(); }
static inline uint32_t hk_r32(hk_ctx *c, uint32_t ea) { (void)c, (void)ea; __builtin_trap(); }
static inline void hk_w8(hk_ctx *c, uint32_t ea, uint32_t v) { (void)c, (void)ea, (void)v; __builtin_trap(); }
static inline void hk_w16(hk_ctx *c, uint32_t ea, uint32_t v) { (void)c, (void)ea, (void)v; __builtin_trap(); }
static inline void hk_w32(hk_ctx *c, uint32_t ea, uint32_t v) { (void)c, (void)ea, (void)v; __builtin_trap(); }

typedef uint32_t u32;
typedef int32_t s32;

#define C ((hk_ctx *)s)
#define M ((struct hk_p4_made *)g)
#define R(h) HK_AT(u32, h)                 // a general register, the hole its offset in hk_ctx
#define IMM(h) HK_IMM(h)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#define LIKELY(x) __builtin_expect(!!(x), 1)

typedef void (HK_ABI *step_fn)(char *s, char *g, u32 step);
typedef u32 (HK_ABI *rd_fn)(char *s, u32 ea);
typedef void (HK_ABI *wr_fn)(char *s, u32 ea, u32 v);

// the budget as the runtime keeps it, before a call out or a way out; and what a call left
#define SYNC (*M->budget = b)
#define RELOAD (b = *M->budget, ram = *M->ram)

// ---- cycles: what fold() put on a step, charged when the step is reached ----
HK_STENCIL(tick) { b = (s32)((u32)b - (u32)IMM(H0)); HK_NEXT; }

// ---- the step's own work, out of the made code (HK_P4_STEP); hole 0 the step ----
HK_STENCIL(step) {
    SYNC;
    ((step_fn)M->fn[HK_P4_STEP])(s, g, (u32)IMM(H0));
    RELOAD;
    t = 0, u = 0;
    HK_NEXT;
}
// a call step (K_call): hole 0 the step
HK_STENCIL(call) {
    SYNC;
    ((step_fn)M->fn[HK_P4_CALL])(s, g, (u32)IMM(H0));
    RELOAD;
    t = 0, u = 0;
    HK_NEXT;
}
// mfspr, mtspr of a register the runtime keeps (hk_mfspr, hk_mtspr): hole 0 the register's
// number, 1 the general register's offset
typedef u32 (HK_ABI *mfspr_fn)(char *s, u32 spr);
typedef void (HK_ABI *mtspr_fn)(char *s, u32 spr, u32 v);
HK_STENCIL(mfspr) {
    SYNC;
    const u32 v = ((mfspr_fn)M->fn[HK_P4_MFSPR])(s, (u32)IMM(H0));
    RELOAD;
    R(H1) = v;
    t = 0, u = 0;
    HK_NEXT;
}
HK_STENCIL(mtspr) {
    SYNC;
    ((mtspr_fn)M->fn[HK_P4_MTSPR])(s, (u32)IMM(H0), R(H1));
    RELOAD;
    t = 0, u = 0;
    HK_NEXT;
}
// ... out of line, then on at target 0 (a poll's: after the poll, or the branch's target)
HK_STENCIL(step_out) {
    SYNC;
    ((step_fn)M->fn[HK_P4_STEP])(s, g, (u32)IMM(H0));
    RELOAD;
    t = 0, u = 0;
    HK_GOTO(0);
}
// a way out: hole 0 the value returned (the step << 8 | an HK_P4_ kind)
HK_STENCIL(leave) {
    SYNC;
    return (int64_t)IMM(H0);
}

// ---- control flow (exec_run.inc): a branch's bo, bi; a condition on one bit is given its mask,
// 1 << (31 - bi) ----
HK_STENCIL(poll) {   // HK_POLL_AT: target 0 the poll (step_out), which comes back here
    if (UNLIKELY(b <= 0)) HK_GOTO(0);
    HK_NEXT;
}
HK_STENCIL(go) { HK_GOTO(0); }
HK_STENCIL(gop) {   // a poll (target 1, out of line, then on at the target), then the goto
    if (UNLIKELY(b <= 0)) HK_GOTO(1);
    HK_GOTO(0);
}
#define COND_any hk_bc_ok(C, (unsigned)IMM(H0), (unsigned)IMM(H1))
#define COND_true (C->cr & (u32)IMM(H0))
#define COND_false (!(C->cr & (u32)IMM(H0)))
#define COND_dnz (--C->ctr != 0)
#define BRANCHES(name) \
    HK_STENCIL(bc_##name) { if (!(COND_##name)) HK_GOTO(0); HK_NEXT; } \
    HK_STENCIL(br_##name) { if (COND_##name) HK_GOTO(0); HK_NEXT; } \
    HK_STENCIL(brp_##name) { \
        if (COND_##name) { \
            if (UNLIKELY(b <= 0)) HK_GOTO(1); \
            HK_GOTO(0); \
        } \
        HK_NEXT; \
    }
BRANCHES(any)
BRANCHES(true)
BRANCHES(false)
BRANCHES(dnz)
HK_STENCIL(set_lr) { C->lr = (u32)IMM(H0); HK_NEXT; }
HK_STENCIL(ret) {
    C->pc = C->lr & ~3u;
    SYNC;
    return HK_P4_RETURNED;
}
HK_STENCIL(ret_if) {
    if (COND_any) {
        C->pc = C->lr & ~3u;
        SYNC;
        return HK_P4_RETURNED;
    }
    HK_NEXT;
}
HK_STENCIL(rfi) {
    C->msr = C->srr1;
    C->pc = C->srr0 & ~3u;
    SYNC;
    return HK_P4_RETURNED;
}
HK_STENCIL(rfci) {
    C->msr = C->srr3;
    C->pc = C->srr2 & ~3u;
    SYNC;
    return HK_P4_RETURNED;
}

// ---- the commonest instructions' own shapes (exec_run.inc's cmpi0 ... mtctr): registers by
// their offsets ----
HK_STENCIL(cmpi0) { hk_setcrf(C, 0, hk_cmps(C, (s32)R(H0), IMM(H1))); HK_NEXT; }
HK_STENCIL(cmpli0) { hk_setcrf(C, 0, hk_cmpu(C, R(H0), (u32)IMM(H1))); HK_NEXT; }
HK_STENCIL(cmp0) { hk_setcrf(C, 0, hk_cmps(C, (s32)R(H0), (s32)R(H1))); HK_NEXT; }
HK_STENCIL(cmpl0) { hk_setcrf(C, 0, hk_cmpu(C, R(H0), R(H1))); HK_NEXT; }
HK_STENCIL(mflr) { R(H0) = C->lr; HK_NEXT; }
HK_STENCIL(mfctr) { R(H0) = C->ctr; HK_NEXT; }
HK_STENCIL(mtlr) { C->lr = R(H0); HK_NEXT; }
HK_STENCIL(mtctr) { C->ctr = R(H0); HK_NEXT; }

// ---- loads and stores: the address into t, then the access; main RAM in place, anything else
// through the runtime's accessors, out of line (target 0, then back at target 0) ----
HK_STENCIL(ea_d) { t = R(H0) + (u32)IMM(H1); HK_NEXT; }   // rA + d (rA's register even when it is 0: the update forms)
HK_STENCIL(ea_dz) { t = (u32)IMM(H0); HK_NEXT; }          // 0 + d
HK_STENCIL(ea_x) { t = R(H0) + R(H1); HK_NEXT; }          // rA + rB
HK_STENCIL(ea_xz) { t = R(H0); HK_NEXT; }                 // 0 + rB
HK_STENCIL(upd) { R(H0) = t; HK_NEXT; }                   // the update forms: rA = the address
#define PHYS(ea) ((ea) & 0x7fffffffu)
HK_STENCIL(ld8) {
    const u32 p = PHYS(t);
    if (LIKELY(p < HK_P4_RAM_SIZE)) {
        R(H0) = ram[p];
        HK_NEXT;
    }
    HK_GOTO(0);
}
HK_STENCIL(ld16) {
    const u32 p = PHYS(t);
    if (LIKELY(p <= HK_P4_RAM_SIZE - 2)) {
        uint16_t v;
        __builtin_memcpy(&v, ram + p, 2);
        R(H0) = __builtin_bswap16(v);
        HK_NEXT;
    }
    HK_GOTO(0);
}
HK_STENCIL(lda16) {
    const u32 p = PHYS(t);
    if (LIKELY(p <= HK_P4_RAM_SIZE - 2)) {
        uint16_t v;
        __builtin_memcpy(&v, ram + p, 2);
        R(H0) = (u32)(s32)(int16_t)__builtin_bswap16(v);
        HK_NEXT;
    }
    HK_GOTO(0);
}
HK_STENCIL(ld32) {
    const u32 p = PHYS(t);
    if (LIKELY(p <= HK_P4_RAM_SIZE - 4)) {
        u32 v;
        __builtin_memcpy(&v, ram + p, 4);
        R(H0) = __builtin_bswap32(v);
        HK_NEXT;
    }
    HK_GOTO(0);
}
#define LD_SLOW(name, which, conv) \
    HK_STENCIL(name##_slow) { \
        SYNC; \
        const u32 v = ((rd_fn)M->fn[which])(s, t); \
        RELOAD; \
        R(H0) = conv(v); \
        HK_GOTO(0); \
    }
#define AS_IS(v) (v)
#define HALF_SIGNED(v) ((u32)(s32)(int16_t)(v))
LD_SLOW(ld8, HK_P4_RD8, AS_IS)
LD_SLOW(ld16, HK_P4_RD16, AS_IS)
LD_SLOW(lda16, HK_P4_RD16, HALF_SIGNED)
LD_SLOW(ld32, HK_P4_RD32, AS_IS)
HK_STENCIL(st8) {
    const u32 p = PHYS(t);
    if (LIKELY(p < HK_P4_RAM_SIZE)) {
        ram[p] = (unsigned char)R(H0);
        HK_NEXT;
    }
    HK_GOTO(0);
}
HK_STENCIL(st16) {
    const u32 p = PHYS(t);
    if (LIKELY(p <= HK_P4_RAM_SIZE - 2)) {
        const uint16_t v = __builtin_bswap16((uint16_t)R(H0));
        __builtin_memcpy(ram + p, &v, 2);
        HK_NEXT;
    }
    HK_GOTO(0);
}
HK_STENCIL(st32) {
    const u32 p = PHYS(t);
    if (LIKELY(p <= HK_P4_RAM_SIZE - 4)) {
        const u32 v = __builtin_bswap32(R(H0));
        __builtin_memcpy(ram + p, &v, 4);
        HK_NEXT;
    }
    HK_GOTO(0);
}
#define ST_SLOW(name, which) \
    HK_STENCIL(name##_slow) { \
        SYNC; \
        ((wr_fn)M->fn[which])(s, t, R(H0)); \
        RELOAD; \
        HK_GOTO(0); \
    }
ST_SLOW(st8, HK_P4_WR8)
ST_SLOW(st16, HK_P4_WR16)
ST_SLOW(st32, HK_P4_WR32)

// ---- the commonest instructions with their registers as offsets (the generic stencils below
// index hk_ctx by a register's number, an instruction more each): what their op_* does with the
// fields fixed. A number shifted or a mask worked out from the fields is the builder's ----
HK_STENCIL(add_imm) { R(H0) = R(H1) + (u32)IMM(H2); HK_NEXT; }    // addi, addis (rA not 0)
HK_STENCIL(load_imm) { R(H0) = (u32)IMM(H1); HK_NEXT; }           // addi, addis (rA 0)
HK_STENCIL(or_imm) { R(H0) = R(H1) | (u32)IMM(H2); HK_NEXT; }     // ori, oris
HK_STENCIL(xor_imm) { R(H0) = R(H1) ^ (u32)IMM(H2); HK_NEXT; }    // xori, xoris
HK_STENCIL(and_imm_rc) { const u32 r = R(H1) & (u32)IMM(H2); R(H0) = r; hk_cr0(C, r); HK_NEXT; }   // andi., andis.
HK_STENCIL(move) { R(H0) = R(H1); HK_NEXT; }                      // or rA, rS, rS
HK_STENCIL(or) { R(H0) = R(H1) | R(H2); HK_NEXT; }
HK_STENCIL(and) { R(H0) = R(H1) & R(H2); HK_NEXT; }
HK_STENCIL(add) { R(H0) = R(H1) + R(H2); HK_NEXT; }
HK_STENCIL(subf) { R(H0) = R(H2) - R(H1); HK_NEXT; }              // rB - rA
// rlwinm: hole 2 the rotation (0 to 31: hk_rotl's), 3 the mask (MASK(mb, me))
HK_STENCIL(rlwinm) {
    t = (u32)IMM(H2);
    R(H0) = __builtin_rotateleft32(R(H1), t) & (u32)IMM(H3);
    HK_NEXT;
}
HK_STENCIL(rlwinm_rc) {
    t = (u32)IMM(H2);
    const u32 r = __builtin_rotateleft32(R(H1), t) & (u32)IMM(H3);
    R(H0) = r;
    hk_cr0(C, r);
    HK_NEXT;
}
HK_STENCIL(extsb) { R(H0) = (u32)(s32)(int8_t)R(H1); HK_NEXT; }
HK_STENCIL(extsh) { R(H0) = (u32)(s32)(int16_t)R(H1); HK_NEXT; }
HK_STENCIL(cmpi) { hk_setcrf(C, IMM(H2), hk_cmps(C, (s32)R(H0), IMM(H1))); HK_NEXT; }   // into field hole 2
HK_STENCIL(cmpli) { hk_setcrf(C, IMM(H2), hk_cmpu(C, R(H0), (u32)IMM(H1))); HK_NEXT; }
// a special register that is a word of hk_ctx (SRR0-3, SPRG0-3, and the XER read): moved as
// hk_mfspr and hk_mtspr move it; the XER written keeps its bits
HK_STENCIL(from_word) { R(H0) = HK_AT(u32, H1); HK_NEXT; }
HK_STENCIL(to_word) { HK_AT(u32, H0) = R(H1); HK_NEXT; }
HK_STENCIL(mtxer) { C->xer = R(H0) & 0xe000007fu; HK_NEXT; }
// lmw, stmw: from register hole 0 (its number) on, wholly in main RAM; else out of line (the
// instruction by fn_step, then back)
HK_STENCIL(lmw) {
    const u32 p = PHYS(t), n = 32 - (u32)IMM(H0);
    if (LIKELY(p <= HK_P4_RAM_SIZE - 4 * n)) {
        for (u32 r = (u32)IMM(H0), q = p; r < 32; r++, q += 4) {
            u32 v;
            __builtin_memcpy(&v, ram + q, 4);
            C->r[r] = __builtin_bswap32(v);
        }
        HK_NEXT;
    }
    HK_GOTO(0);
}
HK_STENCIL(stmw) {
    const u32 p = PHYS(t), n = 32 - (u32)IMM(H0);
    if (LIKELY(p <= HK_P4_RAM_SIZE - 4 * n)) {
        for (u32 r = (u32)IMM(H0), q = p; r < 32; r++, q += 4) {
            const u32 v = __builtin_bswap32(C->r[r]);
            __builtin_memcpy(ram + q, &v, 4);
        }
        HK_NEXT;
    }
    HK_GOTO(0);
}

// ---- every other instruction without a call out: its op_* with its fields as holes, in order
// (ppc403_layout.h, HK_P4_OPS), in four shapes: L the fields as they are; R a record form (rc 1,
// oe 0); P neither record nor overflow form, rA not register 0; Z ... rA register 0 ----
// (a field's bounds told to the compiler: a register's number then indexes hk_ctx as it is)
#define BELOW(i, n) ({ const int v_ = IMM(H##i); __builtin_assume((unsigned)v_ < (n)); v_; })
#define F_rt(i) BELOW(i, 32)
#define F_ra(i) BELOW(i, 32)
#define F_rb(i) BELOW(i, 32)
#define F_sh(i) (unsigned)BELOW(i, 32)
#define F_mb(i) (unsigned)BELOW(i, 32)
#define F_me(i) (unsigned)BELOW(i, 32)
#define F_crm(i) (unsigned)BELOW(i, 256)
#define F_oe(i) BELOW(i, 2)
#define F_rc(i) BELOW(i, 2)
#define F_crfd(i) BELOW(i, 8)
#define F_crfs(i) BELOW(i, 8)
#define F_simm(i) IMM(H##i)
#define F_uimm(i) (u32)IMM(H##i)
#define R_rt F_rt
#define R_ra F_ra
#define R_rb F_rb
#define R_sh F_sh
#define R_mb F_mb
#define R_me F_me
#define R_crm F_crm
#define R_oe(i) 0
#define R_rc(i) 1
#define R_crfd F_crfd
#define R_crfs F_crfs
#define R_simm F_simm
#define R_uimm F_uimm
#define P_rt F_rt
#define P_ra(i) ({ const int ra_ = BELOW(i, 32); __builtin_assume(ra_ != 0); ra_; })
#define P_rb F_rb
#define P_sh F_sh
#define P_mb F_mb
#define P_me F_me
#define P_crm F_crm
#define P_oe(i) 0
#define P_rc(i) 0
#define P_crfd F_crfd
#define P_crfs F_crfs
#define P_simm F_simm
#define P_uimm F_uimm
#define Z_rt F_rt
#define Z_ra(i) 0
#define Z_rb F_rb
#define Z_sh F_sh
#define Z_mb F_mb
#define Z_me F_me
#define Z_crm F_crm
#define Z_oe(i) 0
#define Z_rc(i) 0
#define Z_crfd F_crfd
#define Z_crfs F_crfs
#define Z_simm F_simm
#define Z_uimm F_uimm
// (the shape's prefix pasted onto each field: one macro per arity)
#define OP1(n, a) \
    HK_STENCIL(L_##n) { op_##n(C, F_##a(0)); HK_NEXT; } \
    HK_STENCIL(R_##n) { op_##n(C, R_##a(0)); HK_NEXT; } \
    HK_STENCIL(P_##n) { op_##n(C, P_##a(0)); HK_NEXT; } \
    HK_STENCIL(Z_##n) { op_##n(C, Z_##a(0)); HK_NEXT; }
#define OP2(n, a, b2) \
    HK_STENCIL(L_##n) { op_##n(C, F_##a(0), F_##b2(1)); HK_NEXT; } \
    HK_STENCIL(R_##n) { op_##n(C, R_##a(0), R_##b2(1)); HK_NEXT; } \
    HK_STENCIL(P_##n) { op_##n(C, P_##a(0), P_##b2(1)); HK_NEXT; } \
    HK_STENCIL(Z_##n) { op_##n(C, Z_##a(0), Z_##b2(1)); HK_NEXT; }
#define OP3(n, a, b2, c3) \
    HK_STENCIL(L_##n) { op_##n(C, F_##a(0), F_##b2(1), F_##c3(2)); HK_NEXT; } \
    HK_STENCIL(R_##n) { op_##n(C, R_##a(0), R_##b2(1), R_##c3(2)); HK_NEXT; } \
    HK_STENCIL(P_##n) { op_##n(C, P_##a(0), P_##b2(1), P_##c3(2)); HK_NEXT; } \
    HK_STENCIL(Z_##n) { op_##n(C, Z_##a(0), Z_##b2(1), Z_##c3(2)); HK_NEXT; }
#define OP4(n, a, b2, c3, d4) \
    HK_STENCIL(L_##n) { op_##n(C, F_##a(0), F_##b2(1), F_##c3(2), F_##d4(3)); HK_NEXT; } \
    HK_STENCIL(R_##n) { op_##n(C, R_##a(0), R_##b2(1), R_##c3(2), R_##d4(3)); HK_NEXT; } \
    HK_STENCIL(P_##n) { op_##n(C, P_##a(0), P_##b2(1), P_##c3(2), P_##d4(3)); HK_NEXT; } \
    HK_STENCIL(Z_##n) { op_##n(C, Z_##a(0), Z_##b2(1), Z_##c3(2), Z_##d4(3)); HK_NEXT; }
#define OP5(n, a, b2, c3, d4, e5) \
    HK_STENCIL(L_##n) { op_##n(C, F_##a(0), F_##b2(1), F_##c3(2), F_##d4(3), F_##e5(4)); HK_NEXT; } \
    HK_STENCIL(R_##n) { op_##n(C, R_##a(0), R_##b2(1), R_##c3(2), R_##d4(3), R_##e5(4)); HK_NEXT; } \
    HK_STENCIL(P_##n) { op_##n(C, P_##a(0), P_##b2(1), P_##c3(2), P_##d4(3), P_##e5(4)); HK_NEXT; } \
    HK_STENCIL(Z_##n) { op_##n(C, Z_##a(0), Z_##b2(1), Z_##c3(2), Z_##d4(3), Z_##e5(4)); HK_NEXT; }
#define OP6(n, a, b2, c3, d4, e5, f6) \
    HK_STENCIL(L_##n) { op_##n(C, F_##a(0), F_##b2(1), F_##c3(2), F_##d4(3), F_##e5(4), F_##f6(5)); HK_NEXT; } \
    HK_STENCIL(R_##n) { op_##n(C, R_##a(0), R_##b2(1), R_##c3(2), R_##d4(3), R_##e5(4), R_##f6(5)); HK_NEXT; } \
    HK_STENCIL(P_##n) { op_##n(C, P_##a(0), P_##b2(1), P_##c3(2), P_##d4(3), P_##e5(4), P_##f6(5)); HK_NEXT; } \
    HK_STENCIL(Z_##n) { op_##n(C, Z_##a(0), Z_##b2(1), Z_##c3(2), Z_##d4(3), Z_##e5(4), Z_##f6(5)); HK_NEXT; }
#define OP0(n) HK_STENCIL(L_##n) { op_##n(C); HK_NEXT; }
HK_P4_OPS(OP0, OP1, OP2, OP3, OP4, OP5, OP6)
