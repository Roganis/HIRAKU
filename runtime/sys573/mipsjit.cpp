// SPDX-License-Identifier: GPL-3.0-or-later
// R3000 blocks made at run time (mipsjit.h): tools/mipsgen.py's Block, putting stencils where
// that writes C++. Keep the two alike: every decision here (what the pipeline holds at each
// instruction, where a block ends, what goes to the interpreter) is one mipsgen.py makes, and the
// regress cases hold both to MAME's interpreter.
#include "sys573/mipsjit.h"

#include <cstdlib>
#include <cstring>
#include <functional>
#include <vector>

#include "jit/jit.h"
#include "jit/r3000_layout.h"
#include "sys573/mips_ops.h"

#ifdef HK_R3000_STENCILS
#include "hk_r3000_stencils.inc"
#endif

namespace hk::k573::mips::jit {

#ifdef HK_R3000_STENCILS
namespace {

using X = hk_psx;
using D = psxcpu_device;
using namespace hk::jit::r3000;

static_assert(sizeof(hk_r3000_page) == sizeof(X::page_t) && offsetof(hk_r3000_page, direct_r) == offsetof(X::page_t, direct_r) &&
              offsetof(hk_r3000_page, direct_w) == offsetof(X::page_t, direct_w), "the stencils' page is not the shim's");
static_assert(HK_R3000_PAGE_BITS == X::PAGE_BITS && HK_R3000_SP_BASE == X::SP_BASE && HK_R3000_SP_MASK == X::SP_MASK);
static_assert(HK_R3K_SLOTS <= X::JIT_SLOTS, "psxcpu_device::m_hk_jit is too small");

// what a made block is entered as (r3000_stencils.c): the device; the timeslice's cycles left;
// m_delayv; three values of its own
using Entry = int64_t (HK_JIT_ABI *)(char *s, int64_t base, uint32_t dv, uint32_t t, uint32_t a, uint32_t f);

D &dev(char *s) { return *reinterpret_cast<D *>(s); }

// ---- the calls out of made code ----
void HK_JIT_ABI fn_bail(char *s, uint32_t pc, uint32_t dr, uint32_t dv, int32_t ic) { X::bail(dev(s), pc, dr, dv, ic); }
uint64_t packed(const X::Slow &r, uint32_t mask) { return (uint64_t)(r.v & mask) | (uint64_t)(uint32_t)r.st << 32; }
uint64_t HK_JIT_ABI fn_rd8(char *s, uint32_t a, int32_t ic) { return packed(X::rd8_slow(dev(s), a, ic), 0xff); }
uint64_t HK_JIT_ABI fn_rd16(char *s, uint32_t a, int32_t ic) { return packed(X::rd16_slow(dev(s), a, ic), 0xffff); }
uint64_t HK_JIT_ABI fn_rd32(char *s, uint32_t a, int32_t ic) { return packed(X::rd32_slow(dev(s), a, ic), 0xffffffffu); }
// (sb, sh: MAME's writeword_masked of the byte or half's lanes)
uint32_t HK_JIT_ABI fn_wr8(char *s, uint32_t a, uint32_t v, int32_t ic) {
    return (uint32_t)X::wr32m_slow(dev(s), a, v << (8 * (a & 3)), 0xffu << (8 * (a & 3)), ic);
}
uint32_t HK_JIT_ABI fn_wr16(char *s, uint32_t a, uint32_t v, int32_t ic) {
    return (uint32_t)X::wr32m_slow(dev(s), a, v << (8 * (a & 2)), 0xffffu << (8 * (a & 2)), ic);
}
uint32_t HK_JIT_ABI fn_wr32(char *s, uint32_t a, uint32_t v, int32_t ic) { return (uint32_t)X::wr32_slow(dev(s), a, v, ic); }
// lwl, lwr: v is the register as the pipeline has it; swl, swr: v the register stored
uint64_t HK_JIT_ABI fn_lwl(char *s, uint32_t a, uint32_t v, int32_t ic) {
    D &d = dev(s);
    const X::page_t *pg = X::pages(d);
    int st = 0;
    const uint32_t lt = a & 3;
    a &= ~3u;
    switch (lt) {
    case 0: v = (v & 0x00ffffffu) | (X::rd32m(d, pg, a, 0x000000ffu, ic, st) << 24); break;
    case 1: v = (v & 0x0000ffffu) | (X::rd32m(d, pg, a, 0x0000ffffu, ic, st) << 16); break;
    case 2: v = (v & 0x000000ffu) | (X::rd32m(d, pg, a, 0x00ffffffu, ic, st) << 8); break;
    default: v = X::rd32(d, pg, a, ic, st); break;
    }
    return (uint64_t)v | (uint64_t)(uint32_t)st << 32;
}
uint64_t HK_JIT_ABI fn_lwr(char *s, uint32_t a, uint32_t v, int32_t ic) {
    D &d = dev(s);
    const X::page_t *pg = X::pages(d);
    int st = 0;
    switch (a & 3) {
    case 0: v = X::rd32(d, pg, a, ic, st); break;
    case 1: v = (v & 0xff000000u) | (X::rd32m(d, pg, a, 0xffffff00u, ic, st) >> 8); break;
    case 2: v = (v & 0xffff0000u) | (X::rd32m(d, pg, a, 0xffff0000u, ic, st) >> 16); break;
    default: v = (v & 0xffffff00u) | (X::rd32m(d, pg, a, 0xff000000u, ic, st) >> 24); break;
    }
    return (uint64_t)v | (uint64_t)(uint32_t)st << 32;
}
uint32_t HK_JIT_ABI fn_swl(char *s, uint32_t a, uint32_t v, int32_t ic) {
    D &d = dev(s);
    const X::page_t *pg = X::pages(d);
    int st = 0;
    const uint32_t b = a & ~3u;
    switch (a & 3) {
    case 0: X::wr32m(d, pg, b, v >> 24, 0x000000ffu, ic, st); break;
    case 1: X::wr32m(d, pg, b, v >> 16, 0x0000ffffu, ic, st); break;
    case 2: X::wr32m(d, pg, b, v >> 8, 0x00ffffffu, ic, st); break;
    default: X::wr32(d, pg, b, v, ic, st); break;
    }
    return (uint32_t)st;
}
uint32_t HK_JIT_ABI fn_swr(char *s, uint32_t a, uint32_t v, int32_t ic) {
    D &d = dev(s);
    const X::page_t *pg = X::pages(d);
    int st = 0;
    switch (a & 3) {
    case 0: X::wr32(d, pg, a, v, ic, st); break;
    case 1: X::wr32m(d, pg, a, v << 8, 0xffffff00u, ic, st); break;
    case 2: X::wr32m(d, pg, a, v << 16, 0xffff0000u, ic, st); break;
    default: X::wr32m(d, pg, a, v << 24, 0xff000000u, ic, st); break;
    }
    return (uint32_t)st;
}
void HK_JIT_ABI fn_mulupd(char *s) { (void)X::hi(dev(s)); }
uint32_t HK_JIT_ABI fn_mfc2(char *s, uint32_t pc, uint32_t reg) { return X::mfc2(dev(s), pc, (int)reg); }
uint32_t HK_JIT_ABI fn_cfc2(char *s, uint32_t pc, uint32_t reg) { return X::cfc2(dev(s), pc, (int)reg); }
void HK_JIT_ABI fn_mtc2(char *s, uint32_t pc, uint32_t reg, uint32_t v) { X::mtc2(dev(s), pc, (int)reg, v); }
void HK_JIT_ABI fn_ctc2(char *s, uint32_t pc, uint32_t reg, uint32_t v) { X::ctc2(dev(s), pc, (int)reg, v); }
void HK_JIT_ABI fn_cop2(char *s, uint32_t pc, uint32_t fun, uint32_t) { X::cop2(dev(s), pc, fun); }

bool is_branch(uint32_t w) {
    const uint32_t op = w >> 26;
    return (op >= 1 && op <= 7) || (op == 0 && ((w & 63) == 8 || (w & 63) == 9));
}
bool special_ok(uint32_t fn) {
    switch (fn) {
    case 0: case 2: case 3: case 4: case 6: case 7: case 8: case 9: case 16: case 17: case 18: case 19:
    case 24: case 25: case 26: case 27: case 32: case 33: case 34: case 35: case 36: case 37:
    case 38: case 39: case 42: case 43:
        return true;
    default:
        return false;
    }
}
bool cop0_ri(uint32_t rd) { return rd == 0 || rd == 1 || rd == 2 || rd == 4 || rd == 10; }   // reserved instruction

// a number as a hole takes it: 32 bits, whichever way the instruction extends them
int64_t imm(uint32_t v) { return (int64_t)(int32_t)v; }

hk::jit::Arena &arena() {
    static auto &a = *new hk::jit::Arena((size_t)192 << 20);   // (never freed: the CPU may be in it as the process ends)
    return a;
}

// One block (mipsgen.py's Block). MAME's pipeline at each instruction boundary, known here:
// pend: the register a load is still on its way to (1..31), 0 for none, or ENTRY at the first
//       instruction: whatever the CPU was doing when the block started (m_delayr, read then);
// br:   inside a branch's delay slot: none, TK (taken is decided at run time, in f) or ALWAYS.
struct Block {
    enum { ENTRY = -1 };
    enum Br { NONE, TK, ALWAYS };

    hk::jit::Code &c;
    const X::Offsets o;
    const uint32_t pc0;
    const uint32_t *const words;
    const int nwords;

    int pend = ENTRY;
    Br br = NONE;
    bool jconst = false;       // an unconditional jump's target is a constant (jtarget), else it is in f
    uint32_t jtarget = 0;
    int n = 0;                 // instructions compiled
    bool done = false;         // the delay slot has run
    bool terminal = false;     // this instruction always goes to the interpreter
    bool has_target = false;   // a conditional branch's target
    uint32_t target = 0;
    bool last_was_conditional = false;
    int len0 = 0;              // the first stretch
    int start = -1;            // the block's first instruction (a loop's way back)
    std::vector<std::function<void()>> later;   // ways off the straight path, put after it

    Block(hk::jit::Code &code, D &d, uint32_t pc, const uint32_t *w, int nw) : c(code), o(X::offsets(d)), pc0(pc), words(w), nwords(nw) {}

    int64_t R(uint32_t i) const { return o.r + 4 * (int64_t)i; }
    int64_t slot(int i) const { return o.jit + 8 * (int64_t)i; }

    // MAME's m_delayr now, where it is a constant (after the first advance, outside a delay slot)
    uint32_t dr_const() const { return br == ALWAYS ? 32u : (uint32_t)pend; }

    // the interpreter takes the instruction at pc (the k-th): put now
    void put_bail(int k, uint32_t pc) {
        if (br == TK) c.put(ST_bail_tk, {slot(HK_R3K_BAIL), imm(pc), 0, k}, {});
        else if (br == NONE && pend == ENTRY) c.put(ST_bail_entry, {slot(HK_R3K_BAIL), imm(pc), o.delayr, k}, {});
        else c.put(ST_bail, {slot(HK_R3K_BAIL), imm(pc), dr_const(), k}, {});
    }
    // ... a label to go to for it, the code put after the straight path
    int bail_at(int k, uint32_t pc) {
        const int l = c.label();
        const Br b = br;
        const int p = pend;
        later.push_back([this, l, b, p, k, pc] {
            c.bind(l);
            const Br b0 = br;
            const int p0 = pend;
            br = b, pend = p;
            put_bail(k, pc);
            br = b0, pend = p0;
        });
        return l;
    }

    // MAME's advance_pc(): commit a pending load, or leave the branch delay slot
    void advance() {
        if (br != NONE) {
            if (br == ALWAYS && !jconst) c.put(ST_f_dv);   // where a jump by register goes
            else c.put(ST_dv_zero);
            last_was_conditional = br == TK;
            br = NONE;
            done = true;
        } else if (pend == ENTRY) {
            c.put(ST_commit_entry, {o.delayr, o.r}, {});
            pend = 0;
        } else if (pend != 0) {
            c.put(ST_commit, {R((uint32_t)pend)}, {});
            pend = 0;
        }
    }
    // nothing is on its way: an instruction's result can go straight to its register
    bool settled() const { return br == NONE && pend == 0; }

    // MAME's load(): t (already worked out) lands as the pipeline moves on
    void load(uint32_t rd) {
        advance();
        if (rd) c.put(ST_wr, {R(rd)}, {});
    }
    // a value of two operands into rd: straight there where the pipeline allows
    void op(int st_t, int st_w, int64_t h0, int64_t h1, uint32_t rd) {
        if (!rd) {   // (nothing to work out)
            advance();
        } else if (settled()) {
            c.put(st_w, {h0, h1, R(rd)}, {});
        } else {
            c.put(st_t, {h0, h1}, {});
            load(rd);
        }
    }
    // MAME's delayed_load(): t waits a slot. A second load into the register still in the slot
    // drops the first.
    void delayed_load(uint32_t rt) {
        if (br == NONE && pend == ENTRY) {
            c.put(ST_commit_entry_load, {o.delayr, o.r, rt}, {});
        } else if (br == NONE && (uint32_t)pend == rt) {
            // (dropped: dv is set just below)
        } else {
            advance();
        }
        pend = (int)rt;
        c.put(ST_dv_t);
    }
    // MAME's get_register_from_pipeline(rt) into t
    void pipe(uint32_t rt) {
        if (br != NONE) c.put(ST_t_reg, {R(rt)}, {});
        else if (pend == ENTRY) c.put(ST_t_pipe_entry, {o.delayr, rt, R(rt)}, {});
        else if ((uint32_t)pend == rt) c.put(ST_t_dv);
        else c.put(ST_t_reg, {R(rt)}, {});
    }

    // instructions from i to the end of its stretch: through the next branch's delay slot
    int stretch(int i) const {
        for (int j = i; j < nwords; j++)
            if (is_branch(words[j])) return (j + 2 < nwords ? j + 2 : nwords) - i;
        return nwords - i;
    }

    void put_leave(int st, uint32_t pc, uint32_t dr, int executed, uint32_t last, uint32_t other = 0) {
        // (holes 5 to 8: the next pc, m_delayr, the instructions run, the last pc; leave_st_tk
        // has the not-taken pc in 7)
        c.put(st, {o.pc, o.delayr, o.delayv, o.icount, o.hook_pc, imm(pc), dr, st == ST_leave_st_tk ? imm(other) : (int64_t)executed, imm(last)}, {});
    }
    // the block goes on at the constant target after `executed` instructions: round again when
    // the target is its own start and the timeslice has room (and no checked code was written),
    // else the end
    void jump(uint32_t to, int executed, uint32_t last, uint32_t dr) {
        if (to == pc0) {
            c.put(ST_loop, {executed, len0, slot(HK_R3K_EPOCH), slot(HK_R3K_EP0), o.delayr, dr}, {start});
            put_leave(ST_leave, to, dr, 0, last);
        } else {
            put_leave(ST_leave, to, dr, executed, last);
        }
    }

    bool emit() {
        uint32_t pc = pc0;
        len0 = stretch(0);
        start = c.label();
        c.bind(start);
        int k = 0;
        while (k < nwords) {
            const uint32_t w = words[k];
            if (done) break;
            if (br != NONE && is_branch(w)) break;   // a branch in a delay slot: left to the interpreter
            n = k + 1;
            terminal = false;
            const Br in_slot = br;
            const bool slow = insn(k, pc, w);
            if (terminal) return finish();
            if (slow) {   // a device cut the timeslice short: stop after this instruction, as MAME does
                const int l = c.label();
                c.put(ST_if_a, {}, {l});
                const bool dn = done, jc = jconst;
                const uint32_t jt = jtarget, tg = target, dr = (uint32_t)pend;
                later.push_back([this, l, dn, in_slot, jc, jt, tg, dr, pc] {
                    c.bind(l);
                    if (!dn) put_leave(ST_leave_st, pc + 4, dr, 0, pc);
                    else if (in_slot == TK) put_leave(ST_leave_st_tk, tg, dr, 0, pc, pc + 4);
                    else if (jc) put_leave(ST_leave_st, jt, dr, 0, pc);
                    else put_leave(ST_leave_st_f, 0, dr, 0, pc);
                });
            }
            if (done && in_slot == TK && k + 1 < nwords) {
                // past a conditional branch: taken, the block goes there; not taken, it runs on
                const uint32_t dr = (uint32_t)pend;
                const int lt = c.label();
                c.put(ST_if_f, {}, {lt});
                const uint32_t tg = target;
                later.push_back([this, lt, tg, k, pc, dr] {
                    c.bind(lt);
                    jump(tg, k + 1, pc, dr);
                });
                const int lr = c.label();
                c.put(ST_room, {k + 1 + stretch(k + 1)}, {lr});
                later.push_back([this, lr, k, pc, dr] {
                    c.bind(lr);
                    put_leave(ST_leave, pc + 4, dr, k + 1, pc);
                });
                done = false;
            }
            k++;
            pc += 4;
        }
        const uint32_t last = pc0 + 4 * (uint32_t)(n - 1);
        if (br == ALWAYS) {   // stopped before a delay slot: the interpreter runs it
            put_leave(ST_leave, last + 4, 32, n, last);
        } else if (br == TK) {
            put_leave(ST_leave_brtk, last + 4, 0, n, last);
        } else if (!done) {   // ran off the end of its words: on at the next instruction
            jump(last + 4, n, last, (uint32_t)pend);
        } else if (has_target && last_was_conditional) {
            const uint32_t dr = (uint32_t)pend;
            const int lt = c.label();
            c.put(ST_if_f, {}, {lt});
            const uint32_t tg = target;
            const int nn = n;
            later.push_back([this, lt, tg, nn, last, dr] {
                c.bind(lt);
                jump(tg, nn, last, dr);
            });
            jump(last + 4, n, last, dr);
        } else if (jconst) {
            jump(jtarget, n, last, (uint32_t)pend);
        } else {
            put_leave(ST_leave_f, 0, (uint32_t)pend, n, last);
        }
        return finish();
    }

    bool finish() {
        for (size_t i = 0; i < later.size(); i++) later[i]();   // (one may add another)
        return c.ok();
    }

    // MAME's branch() by register (jr, jalr)
    void branch_reg(uint32_t rs) {
        c.put(ST_t_reg, {R(rs)}, {});
        advance();
        c.put(ST_dv_t);
        br = ALWAYS;
        jconst = false;
    }
    void branch_const(uint32_t to) {
        advance();
        c.put(ST_dv_const, {imm(to)}, {});
        br = ALWAYS;
        jconst = true;
        jtarget = to;
    }
    // MAME's conditional_branch(): the condition (already in f), then the target
    void branch_cond(uint32_t to) {
        advance();
        c.put(ST_dv_br, {imm(to)}, {});
        br = TK;
        has_target = true;
        target = to;
    }

    // a load or a store's access with its slow half: st the stencil, slow its out-of-line half
    void access(int st, std::initializer_list<int64_t> holes, int slow, std::initializer_list<int64_t> slow_holes, int k, uint32_t pc) {
        const int lb = bail_at(k, pc), ls = c.label(), back = c.label();
        c.put(st, holes, {lb, ls});
        c.bind(back);
        const std::vector<int64_t> sh(slow_holes);
        later.push_back([this, ls, lb, back, slow, sh] {
            c.bind(ls);
            switch (sh.size()) {
            case 3: c.put(slow, {sh[0], sh[1], sh[2]}, {lb, back}); break;
            default: c.put(slow, {sh[0], sh[1], sh[2], sh[3]}, {lb, back}); break;
            }
        });
    }

    // One instruction. True if it may have gone through MAME's handlers.
    bool insn(int k, uint32_t pc, uint32_t w) {
        const uint32_t opc = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31, rd = (w >> 11) & 31;
        const uint32_t sa = (w >> 6) & 31, fn = w & 63, uimm = w & 0xffff;
        const uint32_t simm = (uint32_t)(int32_t)(int16_t)uimm;
        const auto bail = [&] {
            put_bail(k, pc);
            terminal = true;
        };

        if (opc == 0) {
            if (!special_ok(fn)) { bail(); return false; }   // reserved, syscall, break
            switch (fn) {
            case 0: op(ST_t_sll, ST_w_sll, R(rt), sa, rd); break;
            case 2: op(ST_t_srl, ST_w_srl, R(rt), sa, rd); break;
            case 3: op(ST_t_sra, ST_w_sra, R(rt), sa, rd); break;
            case 4: op(ST_t_sllv, ST_w_sllv, R(rs), R(rt), rd); break;
            case 6: op(ST_t_srlv, ST_w_srlv, R(rs), R(rt), rd); break;
            case 7: op(ST_t_srav, ST_w_srav, R(rs), R(rt), rd); break;
            case 8: branch_reg(rs); break;                    // jr
            case 9:                                           // jalr
                branch_reg(rs);
                if (rd) c.put(ST_w_const, {imm(pc + 8), R(rd)}, {});
                break;
            case 16: case 18: {                               // mfhi, mflo
                const int again = c.label(), upd = c.label();
                c.bind(again);
                c.put(ST_t_hilo, {o.mop, fn == 16 ? o.hi : o.lo}, {upd});
                later.push_back([this, upd, again] {
                    c.bind(upd);
                    c.put(ST_hilo_update, {slot(HK_R3K_MULUPD)}, {again});
                });
                load(rd);
                break;
            }
            case 17: case 19:                                 // mthi, mtlo
                c.put(ST_mt_hilo, {o.mop, fn == 17 ? o.hi : o.lo, R(rs)}, {});
                advance();
                break;
            case 24: case 25: case 26: case 27:               // mult, multu, div, divu
                c.put(fn >= 26 ? ST_div : ST_mult, {o.mop, o.mop1, o.mop2, o.lo, R(rs), R(rt), fn - 23, o.hi}, {});
                advance();
                break;
            case 32:                                          // add: overflow raises
                c.put(ST_t_add_ov, {R(rs), R(rt)}, {bail_at(k, pc)});
                load(rd);
                break;
            case 33: op(ST_t_addu, ST_w_addu, R(rs), R(rt), rd); break;
            case 34:                                          // sub: overflow raises
                c.put(ST_t_sub_ov, {R(rs), R(rt)}, {bail_at(k, pc)});
                load(rd);
                break;
            case 35: op(ST_t_subu, ST_w_subu, R(rs), R(rt), rd); break;
            case 36: op(ST_t_and, ST_w_and, R(rs), R(rt), rd); break;
            case 37: op(ST_t_or, ST_w_or, R(rs), R(rt), rd); break;
            case 38: op(ST_t_xor, ST_w_xor, R(rs), R(rt), rd); break;
            case 39: op(ST_t_nor, ST_w_nor, R(rs), R(rt), rd); break;
            case 42: op(ST_t_slt, ST_w_slt, R(rs), R(rt), rd); break;
            case 43: op(ST_t_sltu, ST_w_sltu, R(rs), R(rt), rd); break;
            }
            return false;
        }

        if (opc == 1) {   // REGIMM: bit 0 of rt picks bgez over bltz; rt 16/17 also link
            const bool ge = rt & 1;
            c.put(ge ? ST_c_gez : ST_c_ltz, {R(rs)}, {});
            branch_cond(pc + 4 + (simm << 2));
            if (rt == (ge ? 17u : 16u)) c.put(ST_w_const, {imm(pc + 8), R(31)}, {});
            return false;
        }
        if (opc == 2 || opc == 3) {
            branch_const(((pc + 4) & 0xf0000000u) + ((w & 0x3ffffffu) << 2));
            if (opc == 3) c.put(ST_w_const, {imm(pc + 8), R(31)}, {});
            return false;
        }
        if (opc >= 4 && opc <= 7) {   // MAME's blez/bgtz compare with rt too
            static constexpr int conds[4] = {ST_c_eq, ST_c_ne, ST_c_lez, ST_c_gtz};
            c.put(conds[opc - 4], {R(rs), R(rt)}, {});
            branch_cond(pc + 4 + (simm << 2));
            return false;
        }
        if (opc == 8) {   // addi: overflow raises
            c.put(ST_t_addi_ov, {R(rs), imm(simm)}, {bail_at(k, pc)});
            load(rt);
            return false;
        }
        switch (opc) {
        case 9: op(ST_t_addiu, ST_w_addiu, R(rs), imm(simm), rt); return false;
        case 10: op(ST_t_slti, ST_w_slti, R(rs), imm(simm), rt); return false;
        case 11: op(ST_t_sltiu, ST_w_sltiu, R(rs), imm(simm), rt); return false;
        case 12: op(ST_t_andi, ST_w_andi, R(rs), uimm, rt); return false;
        case 13: op(ST_t_ori, ST_w_ori, R(rs), uimm, rt); return false;
        case 14: op(ST_t_xori, ST_w_xori, R(rs), uimm, rt); return false;
        case 15:   // lui
            if (!rt) advance();
            else if (settled()) c.put(ST_w_const, {imm(uimm << 16), R(rt)}, {});
            else {
                c.put(ST_t_const, {imm(uimm << 16)}, {});
                load(rt);
            }
            return false;
        default: break;
        }

        if (opc == 16) {   // COP0 (kernel mode: usable): mfc0 here, the rest in the interpreter
            if (rs == 0 && !cop0_ri(rd)) {
                if (rd < 16) {
                    c.put(ST_t_reg, {o.cp0r + 4 * (int64_t)rd}, {});
                    delayed_load(rt);
                } else {
                    advance();
                }
            } else {
                bail();
            }
            return false;
        }
        if (opc == 18) {   // COP2, the GTE
            if (rs == 8 || rs == 12) { bail(); return false; }
            c.put(ST_cu2, {o.cp0r + 4 * 12}, {bail_at(k, pc)});
            if (rs == 0 || rs == 2) {
                c.put(ST_t_gte, {slot(rs == 0 ? HK_R3K_MFC2 : HK_R3K_CFC2), imm(pc), rd}, {});
                delayed_load(rt);
            } else if (rs == 4 || rs == 6) {
                c.put(ST_gte_wr, {slot(rs == 4 ? HK_R3K_MTC2 : HK_R3K_CTC2), imm(pc), rd, R(rt)}, {});
                advance();
            } else {
                if ((w >> 25) & 1) c.put(ST_gte_op, {slot(HK_R3K_COP2), imm(pc), w & 0x1ffffffu}, {});
                advance();
            }
            return false;
        }

        const int64_t pages = slot(HK_R3K_PAGES), sp = slot(HK_R3K_SP);
        switch (opc) {
        case 32: case 33: case 35: case 36: case 37: {   // lb lh lw lbu lhu
            static constexpr struct { int st, slow, fn; } how[6] = {
                {ST_ld8s, ST_ld8s_slow, HK_R3K_RD8}, {ST_ld16s, ST_ld16s_slow, HK_R3K_RD16}, {0, 0, 0},
                {ST_ld32, ST_ld32_slow, HK_R3K_RD32}, {ST_ld8u, ST_ld8u_slow, HK_R3K_RD8}, {ST_ld16u, ST_ld16u_slow, HK_R3K_RD16}};
            const auto &h = how[opc - 32];
            c.put(ST_addr, {R(rs), imm(simm)}, {});
            access(h.st, {pages}, h.slow, {sp, slot(h.fn), k}, k, pc);
            delayed_load(rt);
            return true;
        }
        case 34: case 38:   // lwl, lwr
            c.put(ST_addr, {R(rs), imm(simm)}, {});
            pipe(rt);
            c.put(ST_ld_lr, {slot(opc == 34 ? HK_R3K_LWL : HK_R3K_LWR), k}, {bail_at(k, pc)});
            delayed_load(rt);
            return true;
        case 40: case 41: case 43: {   // sb sh sw
            const int st = opc == 40 ? ST_st8 : opc == 41 ? ST_st16 : ST_st32;
            const int slow = opc == 40 ? ST_st8_slow : opc == 41 ? ST_st16_slow : ST_st32_slow;
            const int fnn = opc == 40 ? HK_R3K_WR8 : opc == 41 ? HK_R3K_WR16 : HK_R3K_WR32;
            c.put(ST_addr, {R(rs), imm(simm)}, {});
            access(st, {pages, R(rt)}, slow, {sp, slot(fnn), k, R(rt)}, k, pc);
            advance();
            return true;
        }
        case 42: case 46:   // swl, swr
            c.put(ST_addr, {R(rs), imm(simm)}, {});
            c.put(ST_st_lr, {slot(opc == 42 ? HK_R3K_SWL : HK_R3K_SWR), k, R(rt)}, {bail_at(k, pc)});
            advance();
            return true;
        case 50:   // lwc2: the GTE register is set after the pipeline moves on
            c.put(ST_cu2, {o.cp0r + 4 * 12}, {bail_at(k, pc)});
            c.put(ST_addr, {R(rs), imm(simm)}, {});
            access(ST_ld32, {pages}, ST_ld32_slow, {sp, slot(HK_R3K_RD32), k}, k, pc);
            advance();
            c.put(ST_gte_wr_t, {slot(HK_R3K_MTC2), imm(pc + 4), rt}, {});
            return true;
        case 58:   // swc2
            c.put(ST_cu2, {o.cp0r + 4 * 12}, {bail_at(k, pc)});
            c.put(ST_addr, {R(rs), imm(simm)}, {});
            c.put(ST_align3, {}, {bail_at(k, pc)});
            c.put(ST_t_gte, {slot(HK_R3K_MFC2), imm(pc), rt}, {});
            access(ST_st32t, {pages}, ST_st32t_slow, {sp, slot(HK_R3K_WR32), k}, k, pc);
            advance();
            return true;
        default:
            bail();   // COP1, COP3, LWC/SWC 0, 1, 3, reserved
            return false;
        }
    }
};

}  // namespace

bool available() {
    static const bool on = [] {
        const char *e = getenv("HK_573_JIT");
        return hk::jit::wanted() && !(e && strcmp(e, "0") == 0);
    }();
    return on;
}

void init(psxcpu_device &d) {
    void **t = d.m_hk_jit;
    t[HK_R3K_EPOCH] = &X::epoch;
    t[HK_R3K_BAIL] = reinterpret_cast<void *>(&fn_bail);
    t[HK_R3K_RD8] = reinterpret_cast<void *>(&fn_rd8);
    t[HK_R3K_RD16] = reinterpret_cast<void *>(&fn_rd16);
    t[HK_R3K_RD32] = reinterpret_cast<void *>(&fn_rd32);
    t[HK_R3K_WR8] = reinterpret_cast<void *>(&fn_wr8);
    t[HK_R3K_WR16] = reinterpret_cast<void *>(&fn_wr16);
    t[HK_R3K_WR32] = reinterpret_cast<void *>(&fn_wr32);
    t[HK_R3K_LWL] = reinterpret_cast<void *>(&fn_lwl);
    t[HK_R3K_LWR] = reinterpret_cast<void *>(&fn_lwr);
    t[HK_R3K_SWL] = reinterpret_cast<void *>(&fn_swl);
    t[HK_R3K_SWR] = reinterpret_cast<void *>(&fn_swr);
    t[HK_R3K_MULUPD] = reinterpret_cast<void *>(&fn_mulupd);
    t[HK_R3K_MFC2] = reinterpret_cast<void *>(&fn_mfc2);
    t[HK_R3K_CFC2] = reinterpret_cast<void *>(&fn_cfc2);
    t[HK_R3K_MTC2] = reinterpret_cast<void *>(&fn_mtc2);
    t[HK_R3K_CTC2] = reinterpret_cast<void *>(&fn_ctc2);
    t[HK_R3K_COP2] = reinterpret_cast<void *>(&fn_cop2);
}

void map_changed(psxcpu_device &d, uint8_t *scratchpad) {
    d.m_hk_jit[HK_R3K_PAGES] = const_cast<void *>(static_cast<const void *>(X::pages(d)));
    d.m_hk_jit[HK_R3K_SP] = scratchpad;
}

bool make(psxcpu_device &d, uint32_t pc, const uint32_t *words, int n, Made &out) {
    static hk::jit::Code code(SET);
    code.clear();
    Block b(code, d, pc, words, n);
    if (!b.emit() || b.n == 0) return false;
    void *mem = arena().add(code.bytes());
    if (!mem) return false;
    out.entry = mem;
    out.len = (uint32_t)(b.len0 < b.n ? b.len0 : b.n);
    out.nwords = (uint32_t)b.n;
    return true;
}

HK_JIT_ENTERS void enter(psxcpu_device &d, void *entry, uint64_t epoch) {
    memcpy(&d.m_hk_jit[HK_R3K_EP0], &epoch, sizeof epoch);
    reinterpret_cast<Entry>(entry)(reinterpret_cast<char *>(&d), X::icount(d), X::delayv(d), 0, 0, 0);
}

size_t code_bytes() { return arena().used(); }

#else   // no stencils for this processor: nothing is made

bool available() { return false; }
void init(psxcpu_device &) {}
void map_changed(psxcpu_device &, uint8_t *) {}
bool make(psxcpu_device &, uint32_t, const uint32_t *, int, Made &) { return false; }
void enter(psxcpu_device &, void *, uint64_t) {}
size_t code_bytes() { return 0; }

#endif

}  // namespace hk::k573::mips::jit
