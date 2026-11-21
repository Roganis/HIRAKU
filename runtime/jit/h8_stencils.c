// SPDX-License-Identifier: GPL-3.0-or-later
// The stencils of the H8S/2655 (the XT446 card's processor, Keyboardmania): a block of the card's
// program is made of these at run time (runtime/dev/xt446_h8jit.cpp), instruction by instruction.
// Each instruction's stencil is its own statements from MAME's h8.lst, written out by
// tools/h8jitgen.py into hk_h8_ops.h; this file gives them what MAME's device gives them.
//
// s is a struct hk_h8_state (h8_layout.h): the registers, copied from the device and back by the
// dispatcher. Kept in processor registers: ic, MAME's m_icount; bc, its m_bcount (the cycle count
// at which an on-chip event is due, or 0); a, after a jump the new m_PC; f, the flags (CCR).
//
// The cycles: an instruction is run here only when MAME would run it whole, with no stop between
// its bus cycles: at least its most cycles left before bc (every memory access is one cycle on
// this chip, as internal() counts the others). Otherwise, and when an access is anything but
// RAM or ROM (on-chip registers, the SWP30, nothing mapped), the instruction is left untouched
// (what it changed undone) and the code stops before it (HK_GOTO(0): its way out, after the
// block), for MAME's interpreter. So within made code no peripheral is touched and no interrupt
// can become due: what MAME checks after each instruction (prefetch_done) cannot change.
#define HK_ARGS_DECL char *s, int64_t ic, int64_t bc, uint32_t a, uint32_t f
#define HK_ARGS s, ic, bc, a, f
#include "stencil.h"

#include "h8_layout.h"

#define true 1
#define false 0
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef uint16_t __attribute__((may_alias)) u16a;
// MAME's function-style casts
#define u8(x) ((u8)(x))
#define u16(x) ((u16)(x))
#define u32(x) ((u32)(x))
#define u64(x) ((u64)(x))
#define s8(x) ((s8)(x))
#define s16(x) ((s16)(x))
#define s32(x) ((s32)(x))

#define S ((struct hk_h8_state *)s)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

enum { F_I = 0x80, F_UI = 0x40, F_H = 0x20, F_U = 0x10, F_N = 0x08, F_Z = 0x04, F_V = 0x02, F_C = 0x01 };
enum { EXR_T = 0x80, EXR_NC = 0x78, EXR_I = 0x07 };

// MAME's register access (h8.h, h8h.h)
static inline __attribute__((unused)) void r8_w(struct hk_h8_state *st, int reg, u8 val) {
    if (reg & 8) st->R[reg & 7] = (st->R[reg & 7] & 0xff00) | val;
    else st->R[reg & 7] = (st->R[reg & 7] & 0xff) | (val << 8);
}
static inline __attribute__((unused)) u8 r8_r(struct hk_h8_state *st, int reg) {
    return (reg & 8) ? (u8)st->R[reg & 7] : (u8)(st->R[reg & 7] >> 8);
}
static inline __attribute__((unused)) void r16_w(struct hk_h8_state *st, int reg, u16 val) { st->R[reg & 0xf] = val; }
static inline __attribute__((unused)) u16 r16_r(struct hk_h8_state *st, int reg) { return st->R[reg & 0xf]; }
static inline __attribute__((unused)) void r32_w(struct hk_h8_state *st, int reg, u32 val) { st->R[reg & 7] = (u16)val; st->R[(reg & 7) | 8] = (u16)(val >> 16); }
static inline __attribute__((unused)) u32 r32_r(struct hk_h8_state *st, int reg) { return st->R[reg & 7] | ((u32)st->R[(reg & 7) | 8] << 16); }
// ... a register the instruction's words name: its place in R a hole (worked out by the builder)
#define R8(h) (*(u8 *)(s + HK_DISP(h)))
#define R16(h) (*(u16a *)(s + HK_DISP(h)))
#define R32(h) ((u32)*(u16a *)(s + HK_DISP(h)) | ((u32)*(u16a *)(s + HK_DISP(h) + 16) << 16))
#define R8W(h, v) (R8(h) = (u8)(v))
#define R16W(h, v) (R16(h) = (u16)(v))
#define R32W(h, v) ({ const u32 v_ = (v); *(u16a *)(s + HK_DISP(h)) = (u16)v_; *(u16a *)(s + HK_DISP(h) + 16) = (u16)(v_ >> 16); })

// Memory: the space's pages (24 bits, 256 bytes a page, 16-bit big-endian bus words kept as host
// words). Each access is one cycle (MAME's read8 and the others on a chip with EXR). A page that
// isn't plain memory both ways needed: `goto slow`, the instruction undone (its registers put
// back; words it wrote already are written again, the same, when the interpreter does it).
#define PGR(adr) (S->rd[((adr) & 0xffffffu) >> HK_H8_PAGE_BITS])
#define PGW(adr) (S->wr[((adr) & 0xffffffu) >> HK_H8_PAGE_BITS])
#define WORD(p, adr) ((u16a *)((p) + ((adr) & 0xfeu)))
#define RD8(adr) ({ const u32 a_ = (adr); const u8 *p_ = PGR(a_); if (UNLIKELY(!p_)) goto slow; ic -= 1; \
                    const u16 w_ = *WORD(p_, a_); (u8)((a_ & 1) ? w_ : w_ >> 8); })
#define RD16(adr) ({ const u32 a_ = (adr); const u8 *p_ = PGR(a_); if (UNLIKELY(!p_)) goto slow; ic -= 1; \
                     (u16)*WORD(p_, a_); })
// an instruction fetch: its word is not needed here (the instruction's words were read when the
// block was made, the next one's are), only that it is memory
#define FETCH(adr) do { if (UNLIKELY(!PGR(adr))) goto slow; ic -= 1; } while (0)
#define WR8(adr, v) do { const u32 a_ = (adr); u8 *p_ = PGW(a_); if (UNLIKELY(!p_)) goto slow; ic -= 1; \
                         u16a *w_ = WORD(p_, a_); const u8 v_ = (u8)(v); \
                         *w_ = (a_ & 1) ? (u16)((*w_ & 0xff00) | v_) : (u16)((*w_ & 0xff) | (v_ << 8)); } while (0)
#define WR16(adr, v) do { const u32 a_ = (adr); u8 *p_ = PGW(a_); if (UNLIKELY(!p_)) goto slow; ic -= 1; \
                          *WORD(p_, a_) = (u16)(v); } while (0)

#include "hk_h8_ops.h"

// ---- a block's ends ----

// The next instruction is at a - 2 (a is MAME's m_PC after the jump's prefetch): its made code if
// there is any and a carries no bits above the address, else back to the dispatcher. H0: the
// last instruction's address, MAME's m_PPC.
HK_STENCIL(x_jump) {
    S->ppc = (u32)HK_IMM(H0);
    const u32 npc = (a - 2) & 0xffffffu;
    if (npc + 2 == a) {
        void *const *pg = S->l1[npc >> HK_H8_PAGE_BITS];
        if (pg) {
            int64_t (*fn)(HK_ARGS_DECL) = (int64_t (*)(HK_ARGS_DECL))pg[(npc & 0xff) >> 1];
            if (fn) __attribute__((musttail)) return fn(HK_ARGS);
        }
    }
    S->pc = a;
    S->ic = (int32_t)ic;
    S->CCR = (u8)f;
    return HK_H8_STOP_JUMP;
}
// the block's last instruction falls through to H1 (no jump): a as a jump there would leave it
HK_STENCIL(x_next) {
    a = (u32)HK_IMM(H1) + 2;
    S->ppc = (u32)HK_IMM(H0);
    const u32 npc = (u32)HK_IMM(H1);
    void *const *pg = S->l1[npc >> HK_H8_PAGE_BITS];
    if (pg) {
        int64_t (*fn)(HK_ARGS_DECL) = (int64_t (*)(HK_ARGS_DECL))pg[(npc & 0xff) >> 1];
        if (fn) __attribute__((musttail)) return fn(HK_ARGS);
    }
    S->pc = a;
    S->ic = (int32_t)ic;
    S->CCR = (u8)f;
    return HK_H8_STOP_JUMP;
}
// A jump to a place its words give (H0: its address, m_PPC; H1: its slot in the chain table,
// filled with the code there once there is some)
HK_STENCIL(x_chain) {
    S->ppc = (u32)HK_IMM(H0);
    int64_t (*fn)(HK_ARGS_DECL) = (int64_t (*)(HK_ARGS_DECL))S->chain[HK_IMM(H1)];
    if (fn) __attribute__((musttail)) return fn(HK_ARGS);
    S->slot = HK_IMM(H1);
    S->pc = a;
    S->ic = (int32_t)ic;
    S->CCR = (u8)f;
    return HK_H8_STOP_JUMP;
}
// Stop before an instruction (H0 its address, H1 its first word), H2 the one before it (m_PPC)
HK_STENCIL(x_before) {
    S->npc = (u32)HK_IMM(H0);
    S->ir0 = (u32)HK_IMM(H1);
    S->ppc = (u32)HK_IMM(H2);
    S->ic = (int32_t)ic;
    S->CCR = (u8)f;
    return HK_H8_STOP_BEFORE;
}
// ... the block's first, or one jumped to: m_PPC as it was
HK_STENCIL(x_before_first) {
    S->npc = (u32)HK_IMM(H0);
    S->ir0 = (u32)HK_IMM(H1);
    S->ic = (int32_t)ic;
    S->CCR = (u8)f;
    return HK_H8_STOP_BEFORE;
}
// a jump back (or on) to an instruction of the same block: H0 the jump's address (m_PPC)
HK_STENCIL(x_loop) {
    S->ppc = (u32)HK_IMM(H0);
    HK_GOTO(0);
}
// on the straight path into an instruction others jump to: the one before it, m_PPC
HK_STENCIL(x_ppc) {
    S->ppc = (u32)HK_IMM(H0);
    HK_NEXT;
}
