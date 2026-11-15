// SPDX-License-Identifier: GPL-3.0-or-later
// The stencils of the SWP30's MEG (Keyboardmania's XT446 card): one step of an effect program is
// a handful of these, chosen by the step's fields (runtime/jit/megjit.cpp builds a program from
// them; third_party/mame's swp30.cpp step_decoded() is the step they must equal, line for line).
// s is the meg_state, p the accumulator (27.15), x the multiplier's product, then the ALU's
// result, y the ALU's second operand.
#include "stencil.h"

// ... where x and y hold nothing the next stencil wants (after a call they need not be kept)
#define HK_NEXT_PLAIN __attribute__((musttail)) return HK_CONTINUE(s, p, 0, 0)

typedef int64_t s64;
typedef int32_t s32;
typedef uint32_t u32;
typedef int16_t s16;
typedef uint8_t u8;

// the calls out, their addresses in the state (megjit.cpp): the machine's random number, an LFO's
// value, a step's access to the delay memory
typedef u32 (HK_ABI *rand_fn)(char *s);
typedef u32 (HK_ABI *lfo_fn)(char *s, s32 lfo);
typedef void (HK_ABI *mem_fn)(char *s, s32 pc, s32 d2);

// ---- moves between the state's fields ------------------------------------------------
HK_STENCIL(copy32) { HK_AT(s32, H0) = HK_AT(s32, H1); HK_NEXT; }
HK_STENCIL(copy16) { HK_AT(s16, H0) = HK_AT(s16, H1); HK_NEXT; }
HK_STENCIL(set8) { HK_AT(u8, H0) = (u8)HK_IMM(H1); HK_NEXT; }

// ---- what the steps 3 and 2 before left in the delay slots, where they are not known --------
// (the program's first steps: the slots are the previous sample's)
// a register write: slot H0 names the register (0: none) of the array at H1, H2 its value
HK_STENCIL(commit_reg) {
    const u8 r = HK_AT(u8, H0);
    if (r) *(s32 *)(s + HK_DISP(H1) + 4 * (intptr_t)r) = HK_AT(s32, H2);
    HK_NEXT;
}
// if the flag at H0 is set, H1 = H2
HK_STENCIL(commit_flag) { if (HK_AT(u8, H0)) HK_AT(s32, H1) = HK_AT(s32, H2); HK_NEXT; }
// ... and the flag cleared (a memory read or write on its way)
HK_STENCIL(commit_flag_clear) {
    if (HK_AT(u8, H0)) {
        HK_AT(s32, H1) = HK_AT(s32, H2);
        HK_AT(u8, H0) = 0;
    }
    HK_NEXT;
}

// ---- the multiplier: x -------------------------------------------------------------------
static inline __attribute__((always_inline)) s64 m1_expand(s16 v) {
    if (v < 0) return 0;
    const u32 e = (u32)v >> 12;
    v = (s16)(0x1000 | (v & 0xfff));
    return (s16)((e == 5) ? v : (e < 5) ? (v >> (5 - e)) : (v << (e - 5)));
}
HK_STENCIL(m_zero) { x = 0; HK_NEXT; }
HK_STENCIL(m_mul) { x = (s64)HK_AT(s16, H0) * (s64)HK_AT(s32, H1); HK_NEXT; }
HK_STENCIL(m_mul_exp) { x = m1_expand(HK_AT(s16, H0)) * (s64)HK_AT(s32, H1); HK_NEXT; }
HK_STENCIL(m_m1) { x = (s64)HK_AT(s16, H0) << 23; HK_NEXT; }
HK_STENCIL(m_m1_exp) { x = m1_expand(HK_AT(s16, H0)) << 23; HK_NEXT; }
HK_STENCIL(m_m2) { x = (s64)HK_AT(s32, H0) << 15; HK_NEXT; }

// ---- the ALU's second operand: y, and the operation: x --------------------------------------
HK_STENCIL(a_reg) { y = (s64)HK_AT(s32, H0) << 15; HK_NEXT; }
HK_STENCIL(a_psar) { y = p >> 15; HK_NEXT; }
HK_STENCIL(op_add) { x = x + y; HK_NEXT; }
HK_STENCIL(op_sub) { x = x - y; HK_NEXT; }
HK_STENCIL(op_addabs) { x = x + (y < 0 ? -y : y); HK_NEXT; }
HK_STENCIL(op_and) { x = x & y; HK_NEXT; }
// ... with the accumulator itself (the commonest)
HK_STENCIL(op_add_p) { x = x + p; HK_NEXT; }
HK_STENCIL(op_sub_p) { x = x - p; HK_NEXT; }
HK_STENCIL(op_addabs_p) { x = x + (p < 0 ? -p : p); HK_NEXT; }
HK_STENCIL(op_and_p) { x = x & p; HK_NEXT; }

HK_STENCIL(shl1) { x = (s64)((uint64_t)x << 1); HK_NEXT; }
HK_STENCIL(shl2) { x = (s64)((uint64_t)x << 2); HK_NEXT; }
HK_STENCIL(shl4) { x = (s64)((uint64_t)x << 4); HK_NEXT; }

// ---- the result saturated: p ----------------------------------------------------------------
HK_STENCIL(sat_sext) { p = (s64)((uint64_t)x << 22) >> 22; HK_NEXT; }
HK_STENCIL(sat_clamp) { p = x < -0x4000000000 ? -0x4000000000 : x > 0x3fffffffff ? 0x3fffffffff : x; HK_NEXT; }
HK_STENCIL(sat_clamp0) { p = x < 0 ? 0 : x > 0x3fffffffff ? 0x3fffffffff : x; HK_NEXT; }
HK_STENCIL(sat_abs) {
    const s64 a = x < 0 ? (s64)(0 - (uint64_t)x) : x;
    p = a < 0x3fffffffff ? a : 0x3fffffffff;
    HK_NEXT;
}

// ---- values kept for a register, the memory, the index, the T registers ---------------------
static inline __attribute__((always_inline)) s32 pack24(s64 v) {
    s64 q = v / 32768;
    if (q == 0x800000) q = 0x7fffff;
    else if (q == -0x800001) q = -0x800000;
    return (s32)((u32)q << 8) >> 8;
}
HK_STENCIL(st_pack) { HK_AT(s32, H0) = pack24(p); HK_NEXT; }
// ... with the dither: the machine's random number, by the call at H1
HK_STENCIL(st_pack_rand) {
    const u32 r = HK_FN(rand_fn, H1)(s);
    HK_AT(s32, H0) = pack24(p + (r & 0x07e0));
    HK_NEXT_PLAIN;
}
// a random number, 24 bits signed
HK_STENCIL(st_rand24) {
    u32 v = HK_FN(rand_fn, H1)(s) & 0xffffff;
    if (v & 0x00800000) v |= 0xff000000;
    HK_AT(u32, H0) = v;
    HK_NEXT_PLAIN;
}
// an LFO's value: the call at H1, the LFO H2
HK_STENCIL(st_lfo) {
    HK_AT(u32, H0) = HK_FN(lfo_fn, H1)(s, HK_IMM(H2));
    HK_NEXT_PLAIN;
}
HK_STENCIL(st_memw) { HK_AT(s32, H0) = (s32)(p / 32768); HK_NEXT; }
HK_STENCIL(st_idx) { HK_AT(s32, H0) = (s32)(p >> 23); HK_NEXT; }
HK_STENCIL(st_tval) {
    const s64 v = p >> 23;
    HK_AT(s16, H0) = (s16)(v < -0x8000 ? -0x8000 : v > 0x7fff ? 0x7fff : v);
    HK_NEXT;
}
HK_STENCIL(st_tval_idx) { HK_AT(s16, H0) = (s16)((p >> 8) & 0x7fff); HK_NEXT; }

// ---- the delay memory: the call at H0 does the step H1's access (its delay slot H2) ---------
HK_STENCIL(mem_op) {
    HK_FN(mem_fn, H0)(s, HK_IMM(H1), HK_IMM(H2));
    HK_NEXT_PLAIN;
}

// ---- the program's end ----------------------------------------------------------------------
HK_STENCIL(done) { return p; }
