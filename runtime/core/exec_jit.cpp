// SPDX-License-Identifier: GPL-3.0-or-later
// The executor's functions made into the host's code (PLAN.md section 45; runtime/jit/jit.h has
// the method). A release has no game's code in it, so a Firebeat game's functions are steps
// (exec.cpp) that a threaded interpreter runs (exec_run.inc), a jump through a table at every
// step. Here each function's steps are put down as stencils (runtime/jit/ppc403_stencils.c) one
// after another, in the steps' order, with what the step holds (registers, numbers, the cycles
// folded into it) as the stencils' holes: the jumps between steps are gone, and a branch inside
// the function is a jump straight to its target's stencils.
//
// What made code does is the interpreter of steps' doing, step for step, and it charges the same
// cycles at the same steps, so polls (and with them interrupts and the threads' yields) come at
// the same instructions. What it does not do itself it hands back: the steps that leave the
// function (a tail call, a jump, a jump table) return to run_made below, which does them as
// exec_run.inc does; the steps that call out (a call, a system call, a mod's hooks, a poll that
// found the budget spent, an instruction with no stencil) call fn_step, which does that one step;
// main RAM is read and written in place, anything else through the runtime's accessors, out of
// line. The interpreter of steps stays: for an ARM build, for HK_JIT=0 or HK_403_JIT=0, for the
// remembered runs' recorder (exec_rec.cpp) and as the reference HK_403_JIT=check compares with.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "hiraku/recomp.h"
#include "jit/jit.h"
#include "jit/ppc403_layout.h"
#include "ppc/decode.h"
#include "exec_names.h"
#include "exec_steps.h"
#include "hooks.h"
#include "runtime.h"

#ifdef HK_PPC403_STENCILS
#include "hk_ppc403_stencils.inc"
#endif

namespace hk::exec {

namespace {

enum class Mode { off, on, check };

struct Settings {
    Mode mode = Mode::on;
    bool stats = false;
    Settings() {
        const char *e = getenv("HK_403_JIT");
        if (e && !strcmp(e, "0")) mode = Mode::off;
        else if (e && !strcmp(e, "check")) mode = Mode::check;
        if (!jit::wanted()) mode = Mode::off;
        stats = getenv("HK_403_STATS") != nullptr;
    }
} g_set;

struct Stats {
    uint64_t made = 0, steps = 0, bytes = 0, failed = 0;
    double make_ms = 0;
    uint64_t steps_out = 0, leaves = 0, checked = 0, impure = 0;
    uint64_t out_by[KINDS] = {};
    uint64_t spr_to[1024] = {}, spr_from[1024] = {};   // mtspr, mfspr out of made code, by register
    ~Stats();
} g_st;

}  // namespace

bool g_jit_off = false;   // (the tests' switch: every call to the interpreter of steps)

// a function's made code (exec_steps.h, Code::made)
struct Made {
    hk_p4_made blk{};             // the code's second value (ppc403_layout.h)
    const unsigned char *code = nullptr;
    std::vector<uint32_t> at;     // each step's place in the code: where it is entered
    uint64_t calls = 0;           // (check mode: which calls are compared)
    bool checkable = false;       // (check mode: no step of it or of what it calls can leave the thread)
};

namespace {

std::vector<std::unique_ptr<Made>> g_made;

// ---- the calls out of made code -------------------------------------------------------------

// an ordinary instruction, as exec_run.inc's handler without a shape does it (the step's fields)
void one_op(hk_ctx *c, const Step &st) {
    switch (op_of(st.k)) {
#define HK_FV_rt st.rt
#define HK_FV_ra st.ra
#define HK_FV_rb st.rb
#define HK_FV_sh st.sh
#define HK_FV_mb st.x.f.mb
#define HK_FV_me st.x.f.me
#define HK_FV_nb st.x.f.nb
#define HK_FV_crm st.x.f.crm
#define HK_FV_to st.x.f.to
#define HK_FV_e st.x.f.e
#define HK_FV_oe st.x.f.oe
#define HK_FV_rc st.x.f.rc
#define HK_FV_crfd st.y.f.crfd
#define HK_FV_crfs st.y.f.crfs
#define HK_FV_simm ((int32_t)st.y.f.imm)
#define HK_FV_uimm ((uint32_t)(uint16_t)st.y.f.imm)
#define HK_FV_spr st.a
#define HK_FV_dcr st.a
#define HK_FV_addr st.a
#define HK_C0(n) case Op::n: op_##n(c); return;
#define HK_C1(n, a) case Op::n: op_##n(c, HK_FV_##a); return;
#define HK_C2(n, a, b) case Op::n: op_##n(c, HK_FV_##a, HK_FV_##b); return;
#define HK_C3(n, a, b, d) case Op::n: op_##n(c, HK_FV_##a, HK_FV_##b, HK_FV_##d); return;
#define HK_C4(n, a, b, d, e) case Op::n: op_##n(c, HK_FV_##a, HK_FV_##b, HK_FV_##d, HK_FV_##e); return;
#define HK_C5(n, a, b, d, e, f) case Op::n: op_##n(c, HK_FV_##a, HK_FV_##b, HK_FV_##d, HK_FV_##e, HK_FV_##f); return;
#define HK_C6(n, a, b, d, e, f, g) case Op::n: op_##n(c, HK_FV_##a, HK_FV_##b, HK_FV_##d, HK_FV_##e, HK_FV_##f, HK_FV_##g); return;
        HK_OPLIST(HK_C0, HK_C1, HK_C2, HK_C3, HK_C4, HK_C5, HK_C6)
    default:
        fatal(c, "made code: step kind %d has no instruction", (int)st.k);
    }
}

const Code &code_of(char *g) { return *static_cast<const Code *>(reinterpret_cast<hk_p4_made *>(g)->code); }

// a step's own work (HK_P4_STEP): what exec_run.inc's handler for it does after charging its cycles
void HK_JIT_ABI fn_step(char *s, char *g, uint32_t idx) {
    hk_ctx *c = reinterpret_cast<hk_ctx *>(s);
    const Step &st = code_of(g).steps[idx];
    if (__builtin_expect(g_set.stats, 0)) {
        g_st.steps_out++, g_st.out_by[st.k]++;
        if (op_of(st.k) == Op::mtspr) g_st.spr_to[st.a & 1023]++;
        if (op_of(st.k) == Op::mfspr) g_st.spr_from[st.a & 1023]++;
    }
    switch (st.k) {
    case K_poll:   // (made code found the budget spent: hk_budget <= 0)
        hk_poll(c, st.a);
        return;
    case K_gop: case K_brp: case K_brp_true: case K_brp_false: case K_brp_dnz:
        hk_poll(c, st.y.b);
        return;
    case K_probe:
        hooks::run(c, st.y.b);
        return;
    case K_call:
        c->lr = st.a;
        run(c, st.x.fn);
        if (__builtin_expect(c->pc != st.a, 0)) hk_bad_return(c, st.a);
        return;
    case K_call_dyn:
        c->lr = st.a;
        hk_call_dyn(c, st.y.b);
        if (__builtin_expect(c->pc != st.a, 0)) hk_bad_return(c, st.a);
        return;
    case K_bclrl: {
        const uint32_t t = c->lr & ~3u;
        c->lr = st.a;
        if (hk_bc_ok(c, st.rt, st.ra)) {
            hk_call_dyn(c, t);
            if (__builtin_expect(c->pc != st.a, 0)) hk_bad_return(c, st.a);
        }
        return;
    }
    case K_bcctrl: {
        const uint32_t t = c->ctr & ~3u;
        c->lr = st.a;
        if (st.rb || hk_bc_ok(c, st.rt, st.ra)) {
            hk_call_dyn(c, t);
            if (__builtin_expect(c->pc != st.a, 0)) hk_bad_return(c, st.a);
        }
        return;
    }
    case K_sc:
        c->pc = st.a;
        hk_syscall(c);
        return;
    default:
        one_op(c, st);
        return;
    }
}

// a call (K_call), as exec_run.inc's: straight to the callee's made code where nothing stands between
void HK_JIT_ABI fn_call(char *s, char *g, uint32_t idx);
uint32_t HK_JIT_ABI fn_mfspr(char *s, uint32_t spr) { return hk_mfspr(reinterpret_cast<hk_ctx *>(s), spr); }
void HK_JIT_ABI fn_mtspr(char *s, uint32_t spr, uint32_t v) { hk_mtspr(reinterpret_cast<hk_ctx *>(s), spr, v); }

uint32_t HK_JIT_ABI fn_rd8(char *s, uint32_t ea) { return hk_r8(reinterpret_cast<hk_ctx *>(s), ea); }
uint32_t HK_JIT_ABI fn_rd16(char *s, uint32_t ea) { return hk_r16(reinterpret_cast<hk_ctx *>(s), ea); }
uint32_t HK_JIT_ABI fn_rd32(char *s, uint32_t ea) { return hk_r32(reinterpret_cast<hk_ctx *>(s), ea); }
void HK_JIT_ABI fn_wr8(char *s, uint32_t ea, uint32_t v) { hk_w8(reinterpret_cast<hk_ctx *>(s), ea, v); }
void HK_JIT_ABI fn_wr16(char *s, uint32_t ea, uint32_t v) { hk_w16(reinterpret_cast<hk_ctx *>(s), ea, v); }
void HK_JIT_ABI fn_wr32(char *s, uint32_t ea, uint32_t v) { hk_w32(reinterpret_cast<hk_ctx *>(s), ea, v); }

Stats::~Stats() {
    if (!g_set.stats || !made) return;
    fprintf(stderr, "executor's made code: %llu functions (%llu steps, %llu KiB, made in %.1f ms; %llu not made); %llu steps done out of it, "
                    "%llu ways out; %llu runs compared (%llu not comparable)\n",
            (unsigned long long)made, (unsigned long long)steps, (unsigned long long)(bytes >> 10), make_ms, (unsigned long long)failed,
            (unsigned long long)steps_out, (unsigned long long)leaves, (unsigned long long)checked, (unsigned long long)impure);
    std::vector<std::pair<uint64_t, size_t>> v;
    for (size_t k = 0; k < KINDS; k++)
        if (out_by[k]) v.emplace_back(out_by[k], k);
    std::sort(v.rbegin(), v.rend());
    for (size_t n = 0; n < v.size() && n < 16; n++) {
        const uint16_t k = (uint16_t)v[n].second;
        const Op op = op_of(k);
        fprintf(stderr, "  %12llu  %s\n", (unsigned long long)v[n].first,
                op != Op::invalid ? op_name(op) : k == K_call ? "(call)" : k == K_sc ? "(sc)" : k == K_probe ? "(hooks)" :
                k == K_call_dyn || k == K_bclrl || k == K_bcctrl ? "(call by address)" : "(poll)");
    }
    for (size_t n = 0; n < 1024; n++) {
        if (spr_to[n]) fprintf(stderr, "  %12llu  mtspr %zu\n", (unsigned long long)spr_to[n], n);
        if (spr_from[n]) fprintf(stderr, "  %12llu  mfspr %zu\n", (unsigned long long)spr_from[n], n);
    }
}

#ifdef HK_PPC403_STENCILS
using namespace hk::jit::ppc403;

// what made code is entered as: the registers, its block, hk_ram, an address, hk_budget, a spare
using Entry = int64_t (HK_JIT_ABI *)(char *s, char *g, unsigned char *ram, uint32_t t, int32_t b, uint32_t u);

int64_t imm(uint32_t v) { return (int64_t)(int32_t)v; }   // a number as a hole takes it
int64_t reg(unsigned r) { return (int64_t)(offsetof(hk_ctx, r) + 4 * r); }
int64_t cr_mask(unsigned bi) { return imm(1u << (31 - (bi & 31))); }
// a special register that is a word of hk_ctx, as hk_mfspr and hk_mtspr have it (the XER as
// op_mfspr, op_mtspr do): its offset; -1 for any other
int64_t word_spr(uint32_t spr) {
    switch (spr) {
    case spr::XER: return offsetof(hk_ctx, xer);
    case spr::SRR0: return offsetof(hk_ctx, srr0);
    case spr::SRR1: return offsetof(hk_ctx, srr1);
    case spr::SRR2: return offsetof(hk_ctx, srr2);
    case spr::SRR3: return offsetof(hk_ctx, srr3);
    case spr::SPRG0: case spr::SPRG1: case spr::SPRG2: case spr::SPRG3: return (int64_t)(offsetof(hk_ctx, sprg) + 4 * (spr - spr::SPRG0));
    default: return -1;
    }
}

#ifndef _WIN32
// perf's file of names for code made at run time (/tmp/perf-<pid>.map), with HK_403_PERFMAP=1
FILE *perf_map() {
    if (!getenv("HK_403_PERFMAP")) return nullptr;
    return fopen(("/tmp/perf-" + std::to_string(getpid()) + ".map").c_str(), "w");
}
#endif

jit::Arena &arena() {
    static auto &a = *new jit::Arena((size_t)256 << 20);   // (never freed: a fiber may be in it as the process ends)
    return a;
}

// One function's steps as stencils
struct Builder {
    jit::Code &c;
    const Code &k;
    std::vector<int> label;     // each step's, made when something goes there
    struct Stub { int at; int st; int64_t h0; int back; };
    std::vector<Stub> stubs;    // out of line, after the steps
    Builder(jit::Code &code, const Code &fn) : c(code), k(fn), label(fn.steps.size() + 1, -1) {}

    int at(uint32_t step) {
        if (step >= label.size()) return -1;   // (never: Code::ok() then fails)
        if (label[step] < 0) label[step] = c.label();
        return label[step];
    }
    void leave(size_t i, int kind) { c.put(ST_leave, imm((uint32_t)(i << 8 | (unsigned)kind))); }
    // the stencil, then its slow half (or poll) out of line, coming back after it
    void with_stub(int fast, std::initializer_list<int64_t> holes, int slow, int64_t h0) {
        const int out = c.label(), back = c.label();
        c.put(fast, holes, {out});
        c.bind(back);
        stubs.push_back({out, slow, h0, back});
    }

    // a branch's condition, its shape (exec.cpp, shape()): any, on a bit set, clear, the count
    void branch(const Step &st, int any, int on, int off, int dnz, std::initializer_list<int> targets) {
        switch (st.k) {
        case K_bc_true: case K_br_true: case K_brp_true: c.put(on, {cr_mask(st.ra)}, targets); break;
        case K_bc_false: case K_br_false: case K_brp_false: c.put(off, {cr_mask(st.ra)}, targets); break;
        case K_bc_dnz: case K_br_dnz: case K_brp_dnz: c.put(dnz, {}, targets); break;
        default: c.put(any, {st.rt, st.ra}, targets); break;
        }
    }

    void load_store(const Step &st, Op op) {
        // the address: rA (or 0) + d or rB; the update forms use rA's register even when it's 0
        bool upd = false, x = false;
        int fast = -1, slow = -1;
        switch (op) {
#define HK_LS(base, f, s) \
        case Op::base: fast = f, slow = s; break; \
        case Op::base##u: fast = f, slow = s, upd = true; break; \
        case Op::base##x: fast = f, slow = s, x = true; break; \
        case Op::base##ux: fast = f, slow = s, upd = x = true; break;
        HK_LS(lbz, ST_ld8, ST_ld8_slow)
        HK_LS(lhz, ST_ld16, ST_ld16_slow)
        HK_LS(lha, ST_lda16, ST_lda16_slow)
        HK_LS(lwz, ST_ld32, ST_ld32_slow)
        HK_LS(stb, ST_st8, ST_st8_slow)
        HK_LS(sth, ST_st16, ST_st16_slow)
        HK_LS(stw, ST_st32, ST_st32_slow)
#undef HK_LS
        default: break;
        }
        if (x) {
            if (st.ra || upd) c.put(ST_ea_x, {reg(st.ra), reg(st.rb)}, {});
            else c.put(ST_ea_xz, {reg(st.rb)}, {});
        } else {
            if (st.ra || upd) c.put(ST_ea_d, {reg(st.ra), (int64_t)(int32_t)st.y.f.imm}, {});
            else c.put(ST_ea_dz, {(int64_t)(int32_t)st.y.f.imm}, {});
        }
        with_stub(fast, {reg(st.rt)}, slow, reg(st.rt));
        if (upd) c.put(ST_upd, {reg(st.ra)}, {});
    }

    // an ordinary instruction: its own stencil, or its op_* with the fields as holes, or fn_step
    void ordinary(size_t i, const Step &st) {
        const Op op = op_of(st.k);
        switch (st.k) {
        case K_cmpi0: c.put(ST_cmpi0, {reg(st.ra), (int64_t)(int32_t)st.y.f.imm}, {}); return;
        case K_cmpli0: c.put(ST_cmpli0, {reg(st.ra), (int64_t)(uint16_t)st.y.f.imm}, {}); return;
        case K_cmp0: c.put(ST_cmp0, {reg(st.ra), reg(st.rb)}, {}); return;
        case K_cmpl0: c.put(ST_cmpl0, {reg(st.ra), reg(st.rb)}, {}); return;
        case K_mflr: c.put(ST_mflr, {reg(st.rt)}, {}); return;
        case K_mfctr: c.put(ST_mfctr, {reg(st.rt)}, {}); return;
        case K_mtlr: c.put(ST_mtlr, {reg(st.rt)}, {}); return;
        case K_mtctr: c.put(ST_mtctr, {reg(st.rt)}, {}); return;
        default: break;
        }
        const int shape = (st.k - K_op) % SHAPES;   // (exec_steps.h, kind(): 0 a record or overflow form)
        const bool plain = st.k >= K_op && shape != 0;
        const int64_t simm = (int32_t)st.y.f.imm, uimm = (uint16_t)st.y.f.imm;
        if ((op == Op::mfspr || op == Op::mtspr) && st.a != spr::LR && st.a != spr::CTR) {   // (those two: K_mflr ...)
            const int64_t w = word_spr(st.a);
            if (op == Op::mfspr) c.put(w >= 0 ? ST_from_word : ST_mfspr, {w >= 0 ? reg(st.rt) : st.a, w >= 0 ? w : reg(st.rt)}, {});
            else if (st.a == spr::XER) c.put(ST_mtxer, {reg(st.rt)}, {});
            else if (w >= 0) c.put(ST_to_word, {w, reg(st.rt)}, {});
            else c.put(ST_mtspr, {st.a, reg(st.rt)}, {});
            return;
        }
        switch (op) {
        case Op::addi:
        case Op::addis: {
            const int64_t v = imm(op == Op::addis ? (uint32_t)simm << 16 : (uint32_t)simm);
            if (st.ra) c.put(ST_add_imm, {reg(st.rt), reg(st.ra), v}, {});
            else c.put(ST_load_imm, {reg(st.rt), v}, {});
            return;
        }
        case Op::ori: case Op::oris: case Op::xori: case Op::xoris: case Op::andi_: case Op::andis_: {
            const bool high = op == Op::oris || op == Op::xoris || op == Op::andis_;
            const int which = op == Op::ori || op == Op::oris ? ST_or_imm : op == Op::xori || op == Op::xoris ? ST_xor_imm : ST_and_imm_rc;
            c.put(which, {reg(st.ra), reg(st.rt), imm((uint32_t)uimm << (high ? 16 : 0))}, {});
            return;
        }
        case Op::or_:
            if (!plain) break;
            if (st.rt == st.rb) c.put(ST_move, {reg(st.ra), reg(st.rt)}, {});
            else c.put(ST_or, {reg(st.ra), reg(st.rt), reg(st.rb)}, {});
            return;
        case Op::and_:
            if (!plain) break;
            c.put(ST_and, {reg(st.ra), reg(st.rt), reg(st.rb)}, {});
            return;
        case Op::add:
        case Op::subf:
            if (!plain) break;
            c.put(op == Op::add ? ST_add : ST_subf, {reg(st.rt), reg(st.ra), reg(st.rb)}, {});
            return;
        case Op::rlwinm:
            if (st.x.f.oe) break;   // (never: rlwinm has no overflow form)
            c.put(st.x.f.rc ? ST_rlwinm_rc : ST_rlwinm, {reg(st.ra), reg(st.rt), st.sh & 31, imm(hk_mask(st.x.f.mb, st.x.f.me))}, {});
            return;
        case Op::extsb:
        case Op::extsh:
            if (!plain) break;
            c.put(op == Op::extsb ? ST_extsb : ST_extsh, {reg(st.ra), reg(st.rt)}, {});
            return;
        case Op::cmpi: c.put(ST_cmpi, {reg(st.ra), simm, st.y.f.crfd}, {}); return;
        case Op::cmpli: c.put(ST_cmpli, {reg(st.ra), uimm, st.y.f.crfd}, {}); return;
        case Op::lmw:
        case Op::stmw:
            if (st.ra) c.put(ST_ea_d, {reg(st.ra), simm}, {});
            else c.put(ST_ea_dz, {simm}, {});
            with_stub(op == Op::lmw ? ST_lmw : ST_stmw, {st.rt}, ST_step_out, imm((uint32_t)i));
            return;
        default: break;
        }
        switch (op) {
        case Op::lbz: case Op::lbzu: case Op::lbzx: case Op::lbzux:
        case Op::lhz: case Op::lhzu: case Op::lhzx: case Op::lhzux:
        case Op::lha: case Op::lhau: case Op::lhax: case Op::lhaux:
        case Op::lwz: case Op::lwzu: case Op::lwzx: case Op::lwzux:
        case Op::stb: case Op::stbu: case Op::stbx: case Op::stbux:
        case Op::sth: case Op::sthu: case Op::sthx: case Op::sthux:
        case Op::stw: case Op::stwu: case Op::stwx: case Op::stwux:
            load_store(st, op);
            return;
        default: break;
        }
        const int which = shape == 1 ? 2 : shape == 2 ? 3 : st.x.f.rc && !st.x.f.oe ? 1 : 0;   // L R P Z
        switch (op) {
#define HK_FH_rt (int64_t)st.rt
#define HK_FH_ra (int64_t)st.ra
#define HK_FH_rb (int64_t)st.rb
#define HK_FH_sh (int64_t)st.sh
#define HK_FH_mb (int64_t)st.x.f.mb
#define HK_FH_me (int64_t)st.x.f.me
#define HK_FH_crm (int64_t)st.x.f.crm
#define HK_FH_oe (int64_t)st.x.f.oe
#define HK_FH_rc (int64_t)st.x.f.rc
#define HK_FH_crfd (int64_t)st.y.f.crfd
#define HK_FH_crfs (int64_t)st.y.f.crfs
#define HK_FH_simm (int64_t)(int32_t)st.y.f.imm
#define HK_FH_uimm (int64_t)(uint16_t)st.y.f.imm
#define HK_P0(n) case Op::n: c.put(ST_L_##n); return;
#define HK_PUT(n, ...) case Op::n: { static const int s4[] = {ST_L_##n, ST_R_##n, ST_P_##n, ST_Z_##n}; c.put(s4[which], {__VA_ARGS__}, {}); return; }
#define HK_P1(n, a) HK_PUT(n, HK_FH_##a)
#define HK_P2(n, a, b) HK_PUT(n, HK_FH_##a, HK_FH_##b)
#define HK_P3(n, a, b, d) HK_PUT(n, HK_FH_##a, HK_FH_##b, HK_FH_##d)
#define HK_P4(n, a, b, d, e) HK_PUT(n, HK_FH_##a, HK_FH_##b, HK_FH_##d, HK_FH_##e)
#define HK_P5(n, a, b, d, e, f) HK_PUT(n, HK_FH_##a, HK_FH_##b, HK_FH_##d, HK_FH_##e, HK_FH_##f)
#define HK_P6(n, a, b, d, e, f, g) HK_PUT(n, HK_FH_##a, HK_FH_##b, HK_FH_##d, HK_FH_##e, HK_FH_##f, HK_FH_##g)
        HK_P4_OPS(HK_P0, HK_P1, HK_P2, HK_P3, HK_P4, HK_P5, HK_P6)
        default: break;
        }
        c.put(ST_step, imm((uint32_t)i));   // no stencil of its own: fn_step
    }

    bool build() {
        const std::vector<Step> &v = k.steps;
        std::vector<uint32_t> places(v.size() + 1);
        for (size_t i = 0; i < v.size(); i++) {
            const Step &st = v[i];
            places[i] = (uint32_t)c.size();
            c.bind(at((uint32_t)i));
            if (st.tick) c.put(ST_tick, st.tick);
            switch (st.k) {
            case K_tick: c.put(ST_tick, st.a); break;
            case K_poll: with_stub(ST_poll, {}, ST_step_out, imm((uint32_t)i)); break;
            case K_call: c.put(ST_call, imm((uint32_t)i)); break;
            case K_probe: case K_call_dyn: case K_bclrl: case K_bcctrl: case K_sc:
                c.put(ST_step, imm((uint32_t)i));
                break;
            case K_go: c.put(ST_go, {}, {at(st.a)}); break;
            case K_gop: {
                const int out = c.label();
                c.put(ST_gop, {}, {at(st.a), out});
                stubs.push_back({out, ST_step_out, imm((uint32_t)i), at(st.a)});
                break;
            }
            case K_bc: case K_bc_true: case K_bc_false: case K_bc_dnz:
                branch(st, ST_bc_any, ST_bc_true, ST_bc_false, ST_bc_dnz, {at(st.a)});
                break;
            case K_br: case K_br_true: case K_br_false: case K_br_dnz:
                branch(st, ST_br_any, ST_br_true, ST_br_false, ST_br_dnz, {at(st.a)});
                break;
            case K_brp: case K_brp_true: case K_brp_false: case K_brp_dnz: {
                const int out = c.label();
                branch(st, ST_brp_any, ST_brp_true, ST_brp_false, ST_brp_dnz, {at(st.a), out});
                stubs.push_back({out, ST_step_out, imm((uint32_t)i), at(st.a)});
                break;
            }
            case K_tail: case K_call_up: leave(i, HK_P4_TAIL); break;
            case K_jump: leave(i, HK_P4_JUMP); break;
            case K_set_lr: c.put(ST_set_lr, imm(st.a)); break;
            case K_ret: c.put(ST_ret); break;
            case K_ret_if: c.put(ST_ret_if, st.rt, st.ra); break;
            case K_table: case K_jump_ctr: leave(i, HK_P4_TABLE); break;
            case K_rfi: c.put(ST_rfi); break;
            case K_rfci: c.put(ST_rfci); break;
            case K_unimpl: leave(i, HK_P4_UNIMPL); break;
            default:
                if (st.k >= K_cmpi0 && st.k <= K_mtctr) ordinary(i, st);
                else if (st.k >= K_op && op_of(st.k) != Op::invalid) ordinary(i, st);
                else leave(i, HK_P4_BAD);
                break;
            }
        }
        places[v.size()] = (uint32_t)c.size();
        c.bind(at((uint32_t)v.size()));
        leave(v.size(), HK_P4_BAD);   // (past the last step: never)
        for (const Stub &s : stubs) {
            c.bind(s.at);
            c.put(s.st, {s.h0}, {s.back});
        }
        places.swap(where);
        return c.ok();
    }
    std::vector<uint32_t> where;
};

#endif

}  // namespace

// every function's code made, once the steps are settled (exec.cpp, prepare())
void jit_make(const std::vector<std::unique_ptr<Code>> &codes) {
#ifdef HK_PPC403_STENCILS
    if (g_set.mode == Mode::off) return;
    const auto t0 = std::chrono::steady_clock::now();
    static jit::Code code(SET);
    for (const auto &k : codes) {
        code.clear();
        Builder b(code, *k);
        void *mem = b.build() ? arena().add(code.bytes()) : nullptr;
        if (!mem) {
            g_st.failed++;
            continue;
        }
        auto m = std::make_unique<Made>();
        m->blk.budget = &hk_budget;
        m->blk.ram = &hk_ram;
        m->blk.fn[HK_P4_STEP] = reinterpret_cast<void *>(&fn_step);
        m->blk.fn[HK_P4_RD8] = reinterpret_cast<void *>(&fn_rd8);
        m->blk.fn[HK_P4_RD16] = reinterpret_cast<void *>(&fn_rd16);
        m->blk.fn[HK_P4_RD32] = reinterpret_cast<void *>(&fn_rd32);
        m->blk.fn[HK_P4_WR8] = reinterpret_cast<void *>(&fn_wr8);
        m->blk.fn[HK_P4_WR16] = reinterpret_cast<void *>(&fn_wr16);
        m->blk.fn[HK_P4_WR32] = reinterpret_cast<void *>(&fn_wr32);
        m->blk.fn[HK_P4_CALL] = reinterpret_cast<void *>(&fn_call);
        m->blk.fn[HK_P4_MFSPR] = reinterpret_cast<void *>(&fn_mfspr);
        m->blk.fn[HK_P4_MTSPR] = reinterpret_cast<void *>(&fn_mtspr);
        m->blk.code = k.get();
        m->code = static_cast<const unsigned char *>(mem);
        m->at = std::move(b.where);
        g_st.made++, g_st.steps += k->steps.size(), g_st.bytes += code.size();
        k->made = m.get();
#ifndef _WIN32
        if (static FILE *map = perf_map(); map)   // (HK_403_PERFMAP=1: perf report names made code)
            fprintf(map, "%llx %zx p403_%08x\n", (unsigned long long)(uintptr_t)mem, code.size(), k->entry), fflush(map);
#endif
        g_made.push_back(std::move(m));
    }
    g_st.make_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (g_set.mode == Mode::check) {   // which can be checked: none of their steps, nor their callees', leaves the thread
        auto own = [](const Code &k) {
            for (const Step &st : k.steps)
                switch (st.k) {
                case K_sc: case K_call_dyn: case K_bclrl: case K_bcctrl: case K_jump: case K_jump_ctr: case K_table:
                case K_probe: case K_unimpl: case K_bad:
                    return false;
                default:
                    break;
                }
            return true;
        };
        for (const auto &k : codes)
            if (k->made) k->made->checkable = own(*k);
        for (bool changed = true; changed;) {
            changed = false;
            for (const auto &k : codes) {
                if (!k->made || !k->made->checkable) continue;
                for (const Step &st : k->steps)
                    if ((st.k == K_call || st.k == K_tail || st.k == K_call_up) && (!st.x.fn->made || !st.x.fn->made->checkable)) {
                        k->made->checkable = false;
                        changed = true;
                        break;
                    }
            }
        }
    }
#else
    (void)codes;
#endif
}

void jit_forget() { g_made.clear(); }

namespace {

// A function's made code run, from step `at` on, and what it hands back done as exec_run.inc does
HK_JIT_ENTERS void run_made(hk_ctx *c, const Code *k) {
#ifdef HK_PPC403_STENCILS
    uint32_t at = 0;
    for (;;) {
        Made &m = *k->made;
        const auto fn = reinterpret_cast<Entry>(const_cast<unsigned char *>(m.code) + m.at[at]);
        const int64_t r = fn(reinterpret_cast<char *>(c), reinterpret_cast<char *>(&m.blk), hk_ram, 0, hk_budget, 0);
        const int kind = (int)(r & 0xff);
        if (kind == HK_P4_RETURNED) return;
        const uint32_t idx = (uint32_t)r >> 8;
        const Step &st = k->steps[idx];
        if (__builtin_expect(g_set.stats, 0)) g_st.leaves++;
        switch (kind) {
        case HK_P4_JUMP:
            hk_jump(c, st.a);
            return;
        case HK_P4_TAIL:
            if (st.k == K_call_up) c->lr = st.a;
            k = st.x.fn;
            if (__builtin_expect(hk_patched_any, 0) && patched(k->entry)) {   // changed by a mod: interpreted
                hk_interp_function(c, k->entry);
                return;
            }
            if (!k->made) {
                run(c, k);
                return;
            }
            at = 0;
            continue;
        case HK_P4_TABLE:
            if (st.rb || hk_bc_ok(c, st.rt, st.ra)) {
                const uint32_t v = c->ctr & ~3u;
                if (st.k == K_table) {
                    const auto &cases = k->tables[st.y.b];
                    auto it = std::lower_bound(cases.begin(), cases.end(), std::make_pair(v, 0u));
                    if (it != cases.end() && it->first == v && it->second != ~0u) {
                        at = it->second;
                        continue;
                    }
                }
                hk_jump(c, v);
                return;
            }
            at = idx + 1;
            continue;
        case HK_P4_UNIMPL:
            hk_unimplemented(c, st.a, st.y.b);
            return;
        default:
            fatal(c, "the executor's made code for %08x left at step %u (%d)", k->entry, idx, kind);
        }
    }
#else
    (void)c, (void)k;
#endif
}

void HK_JIT_ABI fn_call(char *s, char *g, uint32_t idx) {
    hk_ctx *c = reinterpret_cast<hk_ctx *>(s);
    const Step &st = code_of(g).steps[idx];
    c->lr = st.a;
    const Code *k = st.x.fn;
    // (run()'s own tests, in its order: a patched function, then made code, the tests' switch)
    if (__builtin_expect(hk_patched_any, 0) || !k->made || g_jit_off || g_set.mode != Mode::on) run(c, k);
    else run_made(c, k);
    if (__builtin_expect(c->pc != st.a, 0)) hk_bad_return(c, st.a);
}

// HK_403_JIT=check: a call run by the interpreter of steps (exec_rec.cpp's recorder, which says
// whether the run touched anything but RAM and registers), then from the same state by the made
// code, and the two compared: registers, the budget, all of RAM. A run that touched a device,
// the clock or the scheduler can't be run twice: its interpreted run stands. Each function's
// first calls and one in 4096 after are compared (all of RAM is copied three times a call).
// Only a function that cannot give the processor to another of the game's threads on the way
// (no system call, call by address or jump out in it or in what it calls: checkable) and with a
// budget a short run's polls don't use up: the recorder should not be left running in a thread
// that waits (if a long run's poll does take an interrupt, the other threads' calls go unchecked
// and their handlers unremembered until this one is back; nothing else changes).
bool g_in_check = false;
constexpr int32_t CHECK_BUDGET = 1 << 14;

void run_checked(hk_ctx *c, const Code *k) {
    const uint64_t n = k->made->calls++;
    if (g_in_check || recording() || !k->made->checkable || hk_budget < CHECK_BUDGET || (n >= 4 && n % 4096 != 0)) {
        run_made(c, k);
        return;
    }
    static std::vector<uint8_t> before(HK_RAM_SIZE), after(HK_RAM_SIZE);
    const hk_ctx c0 = *c;
    const int32_t b0 = hk_budget;
    memcpy(before.data(), hk_ram, HK_RAM_SIZE);
    Memo m;
    if (!record(c, k, m)) {
        g_st.impure++;
        return;   // (the interpreter's run stands)
    }
    const hk_ctx want = *c;
    const int32_t b1 = hk_budget;
    memcpy(after.data(), hk_ram, HK_RAM_SIZE);
    *c = c0;
    hk_budget = b0;
    memcpy(hk_ram, before.data(), HK_RAM_SIZE);
    g_in_check = true;
    run_made(c, k);
    g_in_check = false;
    g_st.checked++;
    const char *bad = nullptr;
    if (memcmp(&want, c, CTX_BYTES) != 0) bad = "the registers differ";
    else if (hk_budget != b1) bad = "the cycles differ";
    else if (memcmp(after.data(), hk_ram, HK_RAM_SIZE) != 0) bad = "RAM differs";
    if (!bad) return;
    fprintf(stderr, "executor: the made code of %08x did not do what its steps do: %s (budget: steps %d, made %d)\n", k->entry, bad,
            b1, hk_budget);
    const uint32_t *a = reinterpret_cast<const uint32_t *>(&want), *b = reinterpret_cast<const uint32_t *>(c);
    for (size_t q = 0; q < CTX_BYTES / 4; q++)
        if (a[q] != b[q]) fprintf(stderr, "  register word %zu: steps %08x, made %08x\n", q, a[q], b[q]);
    int shown = 0;
    for (size_t p = 0; p < HK_RAM_SIZE && shown < 8; p++)
        if (after[p] != hk_ram[p]) fprintf(stderr, "  RAM %08zx: steps %02x, made %02x\n", p, after[p], hk_ram[p]), shown++;
    fatal(c, "HK_403_JIT=check failed");
}

}  // namespace

void jit_enter(hk_ctx *c, const Code *k) {
    if (g_set.mode == Mode::check) run_checked(c, k);
    else run_made(c, k);
}

void interpret_only(bool on) { g_jit_off = on; }
size_t made_count() { return g_made.size(); }
size_t made_bytes() {
#ifdef HK_PPC403_STENCILS
    return g_set.mode == Mode::off ? 0 : arena().used();
#else
    return 0;
#endif
}
bool made_available() {
#ifdef HK_PPC403_STENCILS
    return g_set.mode != Mode::off;
#else
    return false;
#endif
}

}  // namespace hk::exec
