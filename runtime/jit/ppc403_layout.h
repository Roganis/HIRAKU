/* SPDX-License-Identifier: GPL-3.0-or-later */
/* What the PowerPC 403's stencils (ppc403_stencils.c, freestanding C) and their builder
   (runtime/core/exec_jit.cpp) must agree on: the made function's own block, the calls out of made
   code, the ways out of it, and which of the instruction set's op_* (include/hiraku/ppc.h) have a
   stencil of their own. */
#ifndef HK_PPC403_LAYOUT_H
#define HK_PPC403_LAYOUT_H

#include <stdint.h>

/* the calls out of made code (exec_jit.cpp), each the System V way on every x86-64 system:
   STEP  void (char *ctx, char *made, uint32_t step): the step's own work, as the interpreter of
         steps does it (a call, a system call, a mod's hooks, a poll that found the budget spent,
         an instruction with no stencil of its own)
   RDn   uint32_t (char *ctx, uint32_t ea): hk_r8/16/32, for what is not main RAM
   WRn   void (char *ctx, uint32_t ea, uint32_t v): hk_w8/16/32
   CALL  void (char *ctx, char *made, uint32_t step): a call step's (exec_run.inc's call)
   MFSPR uint32_t (char *ctx, uint32_t spr): hk_mfspr; MTSPR void (char *ctx, uint32_t spr, uint32_t v): hk_mtspr */
enum { HK_P4_STEP, HK_P4_RD8, HK_P4_RD16, HK_P4_RD32, HK_P4_WR8, HK_P4_WR16, HK_P4_WR32, HK_P4_CALL, HK_P4_MFSPR, HK_P4_MTSPR,
       HK_P4_FNS };

/* a made function's block, its code's second value: where the budget and RAM are, the calls out,
   and (for the calls out) the executor's own Code */
struct hk_p4_made {
    int32_t *budget;        /* &hk_budget: written before every call out and every way out, read after */
    unsigned char **ram;    /* &hk_ram */
    void *fn[HK_P4_FNS];
    const void *code;       /* the hk::exec::Code made */
};

/* how made code ends: the value returned, the step (its index) << 8 | one of these. The step's
   cycles are charged; the interpreter of steps does the rest of it (exec_jit.cpp, run_made) */
enum {
    HK_P4_RETURNED,   /* a return, rfi or rfci: c->pc is set (step 0) */
    HK_P4_JUMP,       /* the step's hk_jump */
    HK_P4_TAIL,       /* tail and call_up: the callee's steps go on in place of these */
    HK_P4_TABLE,      /* a jump table, or a jump by the count register */
    HK_P4_UNIMPL,     /* hk_unimplemented */
    HK_P4_BAD         /* past the last step: never */
};

#define HK_P4_RAM_SIZE 0x02000000u   /* = HK_RAM_SIZE */

/* The instructions given stencils made from their op_* (ppc.h) as they are, in src/ppc/decode.h's
   HK_OPLIST form (the same names, fields in the same order: tests/jit/test_ppc403_jit.cpp checks):
   those that neither touch memory nor call out of the instruction set. The rest (the loads and
   stores have stencils of their own; mfspr, mtmsr, tw and the like call the runtime) go through
   HK_P4_STEP. */
#define HK_P4_OPS(A0, A1, A2, A3, A4, A5, A6) \
    A5(add, rt, ra, rb, oe, rc)      A5(addc, rt, ra, rb, oe, rc)    A5(adde, rt, ra, rb, oe, rc) \
    A4(addme, rt, ra, oe, rc)        A4(addze, rt, ra, oe, rc)       A5(subf, rt, ra, rb, oe, rc) \
    A5(subfc, rt, ra, rb, oe, rc)    A5(subfe, rt, ra, rb, oe, rc)   A4(subfme, rt, ra, oe, rc) \
    A4(subfze, rt, ra, oe, rc)       A4(neg, rt, ra, oe, rc) \
    A3(addi, rt, ra, simm)           A3(addis, rt, ra, simm)         A4(addic, rt, ra, simm, rc) \
    A3(subfic, rt, ra, simm)         A3(mulli, rt, ra, simm)         A5(mullw, rt, ra, rb, oe, rc) \
    A4(mulhw, rt, ra, rb, rc)        A4(mulhwu, rt, ra, rb, rc)      A5(divw, rt, ra, rb, oe, rc) \
    A5(divwu, rt, ra, rb, oe, rc) \
    A4(and_, ra, rt, rb, rc)         A4(andc, ra, rt, rb, rc)        A4(or_, ra, rt, rb, rc) \
    A4(orc, ra, rt, rb, rc)          A4(xor_, ra, rt, rb, rc)        A4(nand, ra, rt, rb, rc) \
    A4(nor, ra, rt, rb, rc)          A4(eqv, ra, rt, rb, rc) \
    A3(ori, ra, rt, uimm)            A3(oris, ra, rt, uimm)          A3(xori, ra, rt, uimm) \
    A3(xoris, ra, rt, uimm)          A3(andi_, ra, rt, uimm)         A3(andis_, ra, rt, uimm) \
    A3(extsb, ra, rt, rc)            A3(extsh, ra, rt, rc)           A3(cntlzw, ra, rt, rc) \
    A4(slw, ra, rt, rb, rc)          A4(srw, ra, rt, rb, rc)         A4(sraw, ra, rt, rb, rc) \
    A4(srawi, ra, rt, sh, rc) \
    A6(rlwinm, ra, rt, sh, mb, me, rc) A6(rlwnm, ra, rt, rb, mb, me, rc) A6(rlwimi, ra, rt, sh, mb, me, rc) \
    A3(cmp, crfd, ra, rb)            A3(cmpl, crfd, ra, rb)          A3(cmpi, crfd, ra, simm) \
    A3(cmpli, crfd, ra, uimm) \
    A3(crand, rt, ra, rb)            A3(crandc, rt, ra, rb)          A3(creqv, rt, ra, rb) \
    A3(crnand, rt, ra, rb)           A3(crnor, rt, ra, rb)           A3(cror, rt, ra, rb) \
    A3(crorc, rt, ra, rb)            A3(crxor, rt, ra, rb) \
    A2(mcrf, crfd, crfs)             A1(mcrxr, crfd)                 A1(mfcr, rt) \
    A2(mtcrf, crm, rt)               A1(mfmsr, rt)

#endif
