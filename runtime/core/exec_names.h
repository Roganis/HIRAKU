// SPDX-License-Identifier: GPL-3.0-or-later
// Which general registers an ordinary instruction names, for the executor's recorder
// (exec_rec.cpp), which must know when a register's value stops being "as the run found it".
//
// names(): which of the three register fields (src/ppc/decode.h's rt, ra, rb) the instruction's
// op_* is given (and whether it is given rc and oe), read off the one list of them (HK_OPLIST);
// it can touch no other general register (but the load and store multiple and string
// instructions, which go on from rt).
// sets(): of those, the field whose register the instruction sets without reading it: the one
// thing here written by hand. tests/core/test_exec_names.cpp checks both against the semantics
// (include/hiraku/ppc.h) with registers changed at random.
#pragma once

#include "ppc/decode.h"

namespace hk::exec {

enum : unsigned {
    F_rt = 1, F_ra = 2, F_rb = 4,
    F_rc = 8, F_oe = 16,   // (not registers: whether it has a record form, an overflow form: exec.cpp)
    // (the other fields)
    F_simm = 0, F_uimm = 0, F_sh = 0, F_mb = 0, F_me = 0, F_crfd = 0, F_crfs = 0, F_crm = 0,
    F_spr = 0, F_dcr = 0, F_nb = 0, F_to = 0, F_addr = 0, F_e = 0,
};

// (The condition register's logic names bits by the same fields: taken for registers here, which
// only makes a run depend on more than it does.)
constexpr unsigned names(Op op) {
    switch (op) {
#define HK_M0(n) case Op::n: return 0;
#define HK_M1(n, a) case Op::n: return F_##a;
#define HK_M2(n, a, b) case Op::n: return F_##a | F_##b;
#define HK_M3(n, a, b, d) case Op::n: return F_##a | F_##b | F_##d;
#define HK_M4(n, a, b, d, e) case Op::n: return F_##a | F_##b | F_##d | F_##e;
#define HK_M5(n, a, b, d, e, f) case Op::n: return F_##a | F_##b | F_##d | F_##e | F_##f;
#define HK_M6(n, a, b, d, e, f, g) case Op::n: return F_##a | F_##b | F_##d | F_##e | F_##f | F_##g;
        HK_OPLIST(HK_M0, HK_M1, HK_M2, HK_M3, HK_M4, HK_M5, HK_M6)
#undef HK_M0
#undef HK_M1
#undef HK_M2
#undef HK_M3
#undef HK_M4
#undef HK_M5
#undef HK_M6
    default: return F_rt | F_ra | F_rb;
    }
}

// the load and store multiple and string instructions: registers from rt on, whatever the fields say
constexpr bool many(Op op) {
    return op == Op::lmw || op == Op::stmw || op == Op::lswi || op == Op::lswx || op == Op::stswi || op == Op::stswx;
}

constexpr unsigned sets(Op op) {
    switch (op) {
    // rD = f(rA, rB, immediate)
    case Op::add: case Op::addc: case Op::adde: case Op::addme: case Op::addze:
    case Op::subf: case Op::subfc: case Op::subfe: case Op::subfme: case Op::subfze: case Op::neg:
    case Op::addi: case Op::addis: case Op::addic: case Op::subfic:
    case Op::mulli: case Op::mullw: case Op::mulhw: case Op::mulhwu: case Op::divw: case Op::divwu:
    // rD = a special register
    case Op::mfcr: case Op::mfspr: case Op::mfmsr: case Op::mfdcr:
    // rD = memory
    case Op::lbz: case Op::lbzu: case Op::lbzx: case Op::lbzux: case Op::lhz: case Op::lhzu: case Op::lhzx: case Op::lhzux:
    case Op::lha: case Op::lhau: case Op::lhax: case Op::lhaux: case Op::lwz: case Op::lwzu: case Op::lwzx: case Op::lwzux:
    case Op::lhbrx: case Op::lwbrx: case Op::lwarx:
        return F_rt;
    // rA = f(rS, rB, immediate)
    case Op::and_: case Op::andc: case Op::or_: case Op::orc: case Op::xor_: case Op::nand: case Op::nor: case Op::eqv:
    case Op::ori: case Op::oris: case Op::xori: case Op::xoris: case Op::andi_: case Op::andis_:
    case Op::extsb: case Op::extsh: case Op::cntlzw:
    case Op::slw: case Op::srw: case Op::sraw: case Op::srawi: case Op::rlwinm: case Op::rlwnm:
        return F_ra;
    default:
        return 0;
    }
}

}  // namespace hk::exec
