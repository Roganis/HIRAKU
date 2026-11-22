// SPDX-License-Identifier: GPL-3.0-or-later
// PowerPC 403GCX instruction decoder, shared by the recompiler, the
// interpreter and the disassembler.
#pragma once

#include <cstdint>
#include <string>

namespace hk {

// Every ordinary (non-control-flow) instruction, with the arguments its op_*
// function in include/hiraku/ppc.h takes after the context, as Insn fields.
// The interpreter and the emitter are both generated from this one list, so
// they cannot disagree about operand order.
//   An(name, fields...)  -> op_name(c, i.field...)
#define HK_OPLIST(A0, A1, A2, A3, A4, A5, A6) \
    A5(add, rt, ra, rb, oe, rc)      A5(addc, rt, ra, rb, oe, rc)    A5(adde, rt, ra, rb, oe, rc) \
    A4(addme, rt, ra, oe, rc)        A4(addze, rt, ra, oe, rc)       A5(subf, rt, ra, rb, oe, rc) \
    A5(subfc, rt, ra, rb, oe, rc)    A5(subfe, rt, ra, rb, oe, rc)   A4(subfme, rt, ra, oe, rc) \
    A4(subfze, rt, ra, oe, rc)       A4(neg, rt, ra, oe, rc) \
    A3(addi, rt, ra, simm)           A3(addis, rt, ra, simm)         A4(addic, rt, ra, simm, rc) \
    A3(subfic, rt, ra, simm)         A3(mulli, rt, ra, simm)         A5(mullw, rt, ra, rb, oe, rc) \
    A4(mulhw, rt, ra, rb, rc)        A4(mulhwu, rt, ra, rb, rc)      A5(divw, rt, ra, rb, oe, rc) \
    A5(divwu, rt, ra, rb, oe, rc) \
    A4(and_, ra, rt, rb, rc)          A4(andc, ra, rt, rb, rc)        A4(or_, ra, rt, rb, rc) \
    A4(orc, ra, rt, rb, rc)          A4(xor_, ra, rt, rb, rc)         A4(nand, ra, rt, rb, rc) \
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
    A2(mtcrf, crm, rt) \
    A2(mfspr, rt, spr)               A2(mtspr, spr, rt)              A1(mfmsr, rt) \
    A1(mtmsr, rt)                    A1(wrtee, rt)                   A1(wrteei, e) \
    A2(mfdcr, rt, dcr)               A2(mtdcr, dcr, rt) \
    A3(lbz, rt, ra, simm)   A3(lbzu, rt, ra, simm)   A3(lbzx, rt, ra, rb)   A3(lbzux, rt, ra, rb) \
    A3(lhz, rt, ra, simm)   A3(lhzu, rt, ra, simm)   A3(lhzx, rt, ra, rb)   A3(lhzux, rt, ra, rb) \
    A3(lha, rt, ra, simm)   A3(lhau, rt, ra, simm)   A3(lhax, rt, ra, rb)   A3(lhaux, rt, ra, rb) \
    A3(lwz, rt, ra, simm)   A3(lwzu, rt, ra, simm)   A3(lwzx, rt, ra, rb)   A3(lwzux, rt, ra, rb) \
    A3(stb, rt, ra, simm)   A3(stbu, rt, ra, simm)   A3(stbx, rt, ra, rb)   A3(stbux, rt, ra, rb) \
    A3(sth, rt, ra, simm)   A3(sthu, rt, ra, simm)   A3(sthx, rt, ra, rb)   A3(sthux, rt, ra, rb) \
    A3(stw, rt, ra, simm)   A3(stwu, rt, ra, simm)   A3(stwx, rt, ra, rb)   A3(stwux, rt, ra, rb) \
    A3(lhbrx, rt, ra, rb)   A3(lwbrx, rt, ra, rb)    A3(sthbrx, rt, ra, rb) A3(stwbrx, rt, ra, rb) \
    A3(lmw, rt, ra, simm)   A3(stmw, rt, ra, simm) \
    A3(lswi, rt, ra, nb)    A3(stswi, rt, ra, nb)    A3(lswx, rt, ra, rb)   A3(stswx, rt, ra, rb) \
    A3(lwarx, rt, ra, rb)   A3(stwcx_, rt, ra, rb)   A2(dcbz, ra, rb) \
    A4(tw, to, ra, rb, addr) A4(twi, to, ra, simm, addr)

enum class Op : uint16_t {
    invalid,
#define HK_OPNAME(n, ...) n,
    HK_OPLIST(HK_OPNAME, HK_OPNAME, HK_OPNAME, HK_OPNAME, HK_OPNAME, HK_OPNAME, HK_OPNAME)
#undef HK_OPNAME
    // control flow and system, handled by the interpreter/emitter themselves
    b, bc, bclr, bcctr, sc, rfi, rfci,
    nop,          // sync, isync, eieio, cache hints/invalidates (no architectural effect here)
    unimpl,       // decodable but deliberately unsupported (FPU, dcread, icread, ...)
};

struct Insn {
    uint32_t word = 0, addr = 0;
    Op op = Op::invalid;
    uint8_t rt = 0, ra = 0, rb = 0;     // instruction bits 6-10, 11-15, 16-20
    uint8_t crfd = 0, crfs = 0;
    uint8_t sh = 0, mb = 0, me = 0, nb = 0, crm = 0, to = 0, e = 0;
    uint8_t bo = 0, bi = 0;
    uint8_t oe = 0, rc = 0, lk = 0, aa = 0;
    int32_t simm = 0;                   // sign-extended 16-bit immediate / displacement
    uint32_t uimm = 0;                  // zero-extended 16-bit immediate
    uint32_t spr = 0, dcr = 0;
    uint32_t target = 0;                // b/bc: absolute target address

    bool is_branch() const { return op == Op::b || op == Op::bc || op == Op::bclr || op == Op::bcctr; }
    bool always() const { return (bo & 0x14) == 0x14; }  // bc/bclr/bcctr unconditional
};

Insn decode(uint32_t word, uint32_t addr);
std::string disasm(const Insn &i);
const char *op_name(Op op);

}  // namespace hk
