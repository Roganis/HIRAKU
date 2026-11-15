// SPDX-License-Identifier: GPL-3.0-or-later
// What a stencil source is written with (runtime/jit/jit.h has the method). A stencil is a small
// C function of one fixed shape, compiled once when HIRAKU is built; a program for the player's
// machine is made at run time by copying stencils one after another and filling their holes.
//
//   HK_STENCIL(name) { ...; HK_NEXT; }
//
// s is the state the code works on, p, x and y three values kept in processor registers from one
// stencil to the next (what each means is the user's: the MEG keeps its accumulator in p).
// A hole is a value unknown until run time: HK_DISP(Hn) an offset from s, HK_IMM(Hn) a small
// number. To the compiler a hole is the address of a symbol nobody defines, so it leaves a
// relocation where the value goes, and tools/stencilgen.py turns the object file's functions and
// relocations into tables. HK_NEXT runs the stencil copied after this one: a jump the copying
// drops where it is the stencil's last instruction.
//
// Freestanding C, integers only: no calls but through HK_FN (a function pointer kept in the
// state), no switch (its table would be data outside the function), no statics.
#pragma once
#include <stdint.h>

typedef int64_t (*hk_stencil_fn)(char *s, int64_t p, int64_t x, int64_t y);
int64_t HK_CONTINUE(char *s, int64_t p, int64_t x, int64_t y);   // never defined: the next stencil

extern char H0[], H1[], H2[], H3[];   // never defined: the holes

#define HK_STENCIL(name) int64_t name(char *s, int64_t p, int64_t x, int64_t y)
#define HK_NEXT __attribute__((musttail)) return HK_CONTINUE(s, p, x, y)
// ... where x and y hold nothing the next stencil wants (after a call they need not be kept)
#define HK_NEXT_PLAIN __attribute__((musttail)) return HK_CONTINUE(s, p, 0, 0)

#define HK_DISP(h) ((intptr_t)(h))
#define HK_IMM(h) ((int32_t)(intptr_t)(h))
#define HK_AT(type, h) (*(type *)(s + HK_DISP(h)))
// a function whose address the state holds at offset h, called the System V way on every x86-64
// system (the stencils are compiled once for all of them) and the platform's way elsewhere
#if defined(__x86_64__)
#define HK_ABI __attribute__((sysv_abi))
#else
#define HK_ABI
#endif
#define HK_FN(type, h) (*(type *)(s + HK_DISP(h)))   // type: a pointer to an HK_ABI function
