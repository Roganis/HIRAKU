// SPDX-License-Identifier: GPL-3.0-or-later
// hiraku-pc32 (PLAN.md section 35): a 32-bit x86 instruction's length and where its address or
// number fields are, for tables.c, which finds the code that reads the game's tables by the
// addresses and counts the instructions carry (as popnhax's database patcher does with a
// disassembler). The instructions a C compiler emits: the one- and two-byte opcodes, x87, MMX and
// SSE to SSE4 (0F 38 and 0F 3A), no 16-bit addressing.
#include "x86.h"

#include <string.h>

// one-byte opcodes: M = a ModRM byte, then the immediate's size: B 1, W 2, Z 4 (2 with 66), P a
// far pointer (6), X: a prefix or an escape (handled before the table), G: by the ModRM's reg
// (F6/F7: test takes an immediate), R: a relative jump's offset (B or Z)
enum { M = 0x10, B = 1, W = 2, Z = 4, P = 6, X = 0x20, G = 0x40, R = 0x80 };
static const unsigned char ONE[256] = {
    // 00
    M, M, M, M, B, Z, 0, 0, M, M, M, M, B, Z, 0, X,
    M, M, M, M, B, Z, 0, 0, M, M, M, M, B, Z, 0, 0,
    M, M, M, M, B, Z, X, 0, M, M, M, M, B, Z, X, 0,
    M, M, M, M, B, Z, X, 0, M, M, M, M, B, Z, X, 0,
    // 40
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, M, M, X, X, X, X, Z, M | Z, B, M | B, 0, 0, 0, 0,
    R | B, R | B, R | B, R | B, R | B, R | B, R | B, R | B, R | B, R | B, R | B, R | B, R | B, R | B, R | B, R | B,
    // 80
    M | B, M | Z, M | B, M | B, M, M, M, M, M, M, M, M, M, M, M, M,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, P, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, B, Z, 0, 0, 0, 0, 0, 0,   // A0-A3: moffs, handled below
    B, B, B, B, B, B, B, B, Z, Z, Z, Z, Z, Z, Z, Z,
    // C0
    M | B, M | B, W, 0, M, M, M | B, M | Z, 0, 0, W, 0, 0, B, 0, 0,   // C8: enter, handled below
    M, M, M, M, B, B, 0, 0, M, M, M, M, M, M, M, M,
    R | B, R | B, R | B, R | B, B, B, B, B, R | Z, R | Z, P, R | B, 0, 0, 0, 0,
    X, 0, X, X, 0, 0, M | G, M | G, 0, 0, 0, 0, 0, 0, M, M,
};
// two-byte opcodes (0F xx), the same marks (38 and 3A are escapes to three-byte opcodes)
static const unsigned char TWO[256] = {
    // 00
    M, M, M, M, 0, 0, 0, 0, 0, 0, 0, 0, 0, M, 0, M | B,
    M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M,
    M, M, M, M, 0, 0, 0, 0, M, M, M, M, M, M, M, M,
    0, 0, 0, 0, 0, 0, 0, 0, X, 0, X, 0, 0, 0, 0, 0,
    // 40
    M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M,
    M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M,
    M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M,
    M | B, M | B, M | B, M | B, M, M, M, 0, M, M, 0, 0, M, M, M, M,
    // 80
    R | Z, R | Z, R | Z, R | Z, R | Z, R | Z, R | Z, R | Z, R | Z, R | Z, R | Z, R | Z, R | Z, R | Z, R | Z, R | Z,
    M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M,
    0, 0, 0, M, M | B, M, 0, 0, 0, 0, 0, M, M | B, M, M, M,
    M, M, M, M, M, M, M, M, M, M, M | B, M, M, M, M, M,
    // C0
    M, M, M | B, M, M | B, M | B, M | B, M, 0, 0, 0, 0, 0, 0, 0, 0,
    M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M,
    M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M,
    M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M,
};

int x86_decode(const unsigned char *code, struct x86_insn *in) {
    memset(in, 0, sizeof *in);
    in->base = -1;
    const unsigned char *p = code;
    int opsize16 = 0, addr16 = 0;
    for (int n = 0;; n++) {   // prefixes
        if (n == 15) return 0;
        const unsigned char c = *p;
        if (c == 0x66) opsize16 = 1;
        else if (c == 0x67) addr16 = 1;
        else if (c != 0xf0 && c != 0xf2 && c != 0xf3 && c != 0x2e && c != 0x36 && c != 0x3e && c != 0x26 && c != 0x64 && c != 0x65)
            break;
        p++;
    }
    if (addr16) return 0;
    unsigned op = *p++, flags;
    if (op == 0x0f) {
        op = *p++;
        if (op == 0x38) p++, flags = M;
        else if (op == 0x3a) p++, flags = M | B;
        else flags = TWO[op];
        in->escape = 1;
    } else {
        flags = ONE[op];
        if (flags == X) return 0;   // a prefix out of place, or 0F handled above
    }
    in->opcode = (unsigned char)op;
    unsigned imm = flags & 0xf;
    if (imm == Z && opsize16 && !(flags & R)) imm = W;
    if (!in->escape && op >= 0xa0 && op <= 0xa3) {   // mov al/eax <-> [address]
        in->disp_at = (unsigned char)(p - code), in->disp_size = 4;
        p += 4;
    }
    if (!in->escape && op == 0xc8) imm = 3;   // enter: a word and a byte
    if (flags & M) {
        const unsigned char modrm = *p++;
        const unsigned mod = modrm >> 6, rm = modrm & 7;
        in->modrm = 1, in->reg = (modrm >> 3) & 7;
        if (mod != 3) {
            unsigned base = rm, disp = mod == 1 ? 1 : mod == 2 ? 4 : 0;
            if (rm == 4) {   // SIB
                const unsigned char sib = *p++;
                base = sib & 7;
                if (base == 5 && mod == 0) base = 8, disp = 4;   // [index*scale + address]
            } else if (rm == 5 && mod == 0) {
                base = 8, disp = 4;   // [address]
            }
            in->base = base == 8 ? -1 : (signed char)base;
            if (disp) in->disp_at = (unsigned char)(p - code), in->disp_size = (unsigned char)disp;
            p += disp;
        }
        if (flags & G) imm = in->reg > 1 ? 0 : op == 0xf6 ? B : opsize16 ? W : Z;   // F6/F7: only test has an immediate
    }
    if (imm) {
        in->imm_at = (unsigned char)(p - code), in->imm_size = (unsigned char)imm;
        in->relative = (flags & R) != 0;
        p += imm;
    }
    in->len = (unsigned char)(p - code);
    return in->len;
}
