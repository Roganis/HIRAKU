/* SPDX-License-Identifier: GPL-3.0-or-later */
/* What the H8S's stencils (h8_stencils.c, freestanding C) and their builder
   (runtime/dev/xt446_h8jit.cpp, which checks each against the real thing) must agree on. */
#ifndef HK_H8_LAYOUT_H
#define HK_H8_LAYOUT_H

#include <stdint.h>

/* a page of the shim's address space, as address_space::page_t begins: its memory for reading
   and for writing, null where the page is not plain memory */
struct hk_h8_page {
    const unsigned char *direct_r;
    unsigned char *direct_w;
    unsigned short did_r, did_w;
    void *ridx, *widx;
};
#define HK_H8_PAGE_BITS 8   /* a 24-bit space: 256-byte pages */

/* What made code works on: the CPU's registers, copied from the device as the dispatcher
   enters made code and back as it leaves, and where the code stopped. */
struct hk_h8_state {
    uint16_t R[16];        /* MAME's m_R: R0-R7, then E0-E7 */
    uint8_t CCR, EXR;
    uint8_t pad0[2];
    int32_t ic;            /* m_icount, at a stop */
    uint32_t npc;          /* a stop before an instruction: its address ... */
    uint32_t ir0;          /* ... and its first word */
    uint32_t pc;           /* a stop after a jump: m_PC (the next instruction's address + 2) */
    uint32_t ppc;          /* m_PPC: the last instruction's address */
    const struct hk_h8_page *pages;   /* the CPU's page table */
    /* ... as the made code reads it: per page, its memory for reading / writing or null */
    const unsigned char *const *rd;
    unsigned char *const *wr;
    /* made code by address: l1[a >> 8] is null or a page's 128 entries (a & 0xff) >> 1, each
       null or the code that starts there */
    void *const *const *l1;
    /* made code a jump to a known place goes to, by the jump's slot (null: not made yet) ... */
    void *const *chain;
    int32_t slot;          /* ... and the slot of a stop for want of it, else -1 */
};

/* what made code returns */
#define HK_H8_STOP_BEFORE 1   /* before the instruction at npc (first word ir0): the interpreter's */
#define HK_H8_STOP_JUMP 2     /* after a jump to pc - 2, no made code there (or it can't be chained) */

#endif
