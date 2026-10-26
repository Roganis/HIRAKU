// SPDX-License-Identifier: GPL-3.0-or-later
// hiraku-pc32: a 32-bit x86 instruction decoded for its length and fields (x86.c)
#pragma once

struct x86_insn {
    unsigned char len;       // the instruction's bytes, prefixes included
    unsigned char escape;    // 0F
    unsigned char opcode;    // after the prefixes (and 0F)
    unsigned char modrm, reg;   // a ModRM byte; its reg field
    signed char base;        // the memory operand's base register (0 eax .. 4 esp, 5 ebp ..), -1 none
    unsigned char disp_at, disp_size;   // the address or displacement: where, its size (0 none)
    unsigned char imm_at, imm_size;     // the immediate: where, its size (0 none)
    unsigned char relative;  // the immediate is a jump's or call's offset
};
// The instruction at code (its length), or 0 for one it doesn't know
int x86_decode(const unsigned char *code, struct x86_insn *in);
