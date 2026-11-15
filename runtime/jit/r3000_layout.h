/* SPDX-License-Identifier: GPL-3.0-or-later */
/* What the R3000's stencils (r3000_stencils.c, freestanding C) and their builder
   (runtime/sys573/mipsjit.cpp, which checks each against the real thing) must agree on. */
#ifndef HK_R3000_LAYOUT_H
#define HK_R3000_LAYOUT_H

/* a page of the shim's address space, as address_space::page_t begins: its memory for reading
   and for writing, null where the page is not plain memory */
struct hk_r3000_page {
    unsigned char *direct_r, *direct_w;
    unsigned short did_r, did_w;
    void *ridx, *widx;
};
#define HK_R3000_PAGE_BITS 16

/* the scratchpad: 1 KiB of memory on a page shared with I/O */
#define HK_R3000_SP_BASE 0x1f800000u
#define HK_R3000_SP_MASK 0x3ffu

/* psxcpu_device::m_hk_jit: pointers the made code works from. The first hold data, the rest
   the functions it calls (declared in r3000_stencils.c, defined in mipsjit.cpp). */
enum {
    HK_R3K_PAGES,        /* const hk_r3000_page *: the CPU's page table */
    HK_R3K_SP,           /* unsigned char *: the scratchpad where it is memory both ways, else null */
    HK_R3K_EPOCH,        /* const uint64_t *: moves on when checked code may have changed */
    HK_R3K_EP0,          /* (a uint64_t) the epoch as the running block was entered */
    HK_R3K_BAIL,
    HK_R3K_RD8, HK_R3K_RD16, HK_R3K_RD32,
    HK_R3K_WR8, HK_R3K_WR16, HK_R3K_WR32,
    HK_R3K_LWL, HK_R3K_LWR, HK_R3K_SWL, HK_R3K_SWR,
    HK_R3K_MULUPD,
    HK_R3K_MFC2, HK_R3K_CFC2, HK_R3K_MTC2, HK_R3K_CTC2, HK_R3K_COP2,
    HK_R3K_SLOTS
};

#endif
