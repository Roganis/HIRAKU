// SPDX-License-Identifier: GPL-3.0-or-later
// What the executor's recorder takes for granted about the instructions (runtime/core/
// exec_names.h, exec_rec.cpp), against the semantics themselves (include/hiraku/ppc.h), with
// registers and operands drawn at random:
//   - an instruction's result does not depend on a general register it doesn't name (names(),
//     many()), and it leaves that register alone;
//   - nor on the register sets() says it only sets;
//   - stw, lwz, stmw and lmw move whole words between RAM and the registers from rt on, in order,
//     and change nothing else.
#include <cstdio>
#include <cstring>
#include <vector>

#include "core/exec_names.h"
#include "core/exec_steps.h"
#include "core/runtime.h"

// (no recompiled game: an empty function table)
extern "C" {
const hk_func_entry hk_func_table[1] = {{0, nullptr}};
const unsigned hk_func_count = 0;
}

using namespace hk;
using namespace hk::exec;

namespace {

uint64_t g_rng = 0x9e3779b97f4a7c15ull;
uint32_t rnd() {
    g_rng ^= g_rng << 13, g_rng ^= g_rng >> 7, g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 16);
}
uint32_t value() {   // edges, and anywhere
    static const uint32_t edge[] = {0, 1, 2, 31, 32, 0x7fff, 0x8000, 0xffff, 0x7fffffff, 0x80000000, 0xffffffff, 0xfffffffe};
    const uint32_t r = rnd();
    return r % 3 == 0 ? edge[rnd() % 12] : rnd();
}

void run_op(hk_ctx *c, const Insn &i) {
    switch (i.op) {
#define HK_I0(n) case Op::n: op_##n(c); break;
#define HK_I1(n, a) case Op::n: op_##n(c, i.a); break;
#define HK_I2(n, a, b) case Op::n: op_##n(c, i.a, i.b); break;
#define HK_I3(n, a, b, d) case Op::n: op_##n(c, i.a, i.b, i.d); break;
#define HK_I4(n, a, b, d, e) case Op::n: op_##n(c, i.a, i.b, i.d, i.e); break;
#define HK_I5(n, a, b, d, e, f) case Op::n: op_##n(c, i.a, i.b, i.d, i.e, i.f); break;
#define HK_I6(n, a, b, d, e, f, g) case Op::n: op_##n(c, i.a, i.b, i.d, i.e, i.f, i.g); break;
        HK_OPLIST(HK_I0, HK_I1, HK_I2, HK_I3, HK_I4, HK_I5, HK_I6)
#undef HK_I0
#undef HK_I1
#undef HK_I2
#undef HK_I3
#undef HK_I4
#undef HK_I5
#undef HK_I6
    default: break;
    }
}

const Op ALL[] = {
#define HK_N(n, ...) Op::n,
    HK_OPLIST(HK_N, HK_N, HK_N, HK_N, HK_N, HK_N, HK_N)
#undef HK_N
};

// the instructions that reach RAM: their registers are kept where every address they make is
// inside the scratch zone
bool reaches_ram(Op op) {
    const char *n = op_name(op);
    return n[0] == 'l' || (n[0] == 's' && n[1] == 't') || op == Op::dcbz;
}

constexpr uint32_t ZONE = 0x4400;   // RAM the instructions may reach: [0, ZONE)
std::vector<uint8_t> g_zone0, g_zone1;

// an instruction with operands at random, and registers to run it on
void draw(Op op, Insn &i, hk_ctx &c) {
    static const uint32_t sprs[] = {1, 8, 9, spr::SRR0, spr::SRR1, spr::SRR2, spr::SRR3, spr::SPRG0, spr::SPRG1, spr::SPRG2,
                                    spr::SPRG3, spr::ESR, spr::DEAR, spr::EVPR, spr::PVR};
    i = Insn{};
    i.op = op;
    i.rt = rnd() & 31, i.ra = rnd() & 31, i.rb = rnd() & 31;
    i.crfd = rnd() & 7, i.crfs = rnd() & 7;
    i.sh = rnd() & 31, i.mb = rnd() & 31, i.me = rnd() & 31, i.nb = rnd() & 31, i.crm = rnd() & 255;
    i.to = 0;   // (a trap ends the program)
    i.e = rnd() & 1, i.oe = rnd() & 1, i.rc = rnd() & 1;
    i.simm = reaches_ram(op) ? (int32_t)(rnd() % 0x800) : (int32_t)(int16_t)value();   // (rA can be "0")
    i.uimm = value() & 0xffff;
    i.spr = sprs[rnd() % (sizeof sprs / sizeof *sprs)];
    i.dcr = rnd() & 1 ? dcr::EXIER : dcr::IOCR;
    c = hk_ctx{};
    for (uint32_t &r : c.r) r = reaches_ram(op) ? 0x400 + rnd() % 0x1800 : value();
    c.cr = rnd(), c.xer = rnd() & 0xe000007fu, c.lr = rnd(), c.ctr = rnd();
    c.srr0 = rnd(), c.srr1 = rnd(), c.srr2 = rnd(), c.srr3 = rnd();
    for (uint32_t &s : c.sprg) s = rnd();
    c.reserve = rnd() & 1 ? 0 : (rnd() | 1);
}

int g_failed = 0;
void fail(const char *what, const Insn &i, const char *detail) {
    if (g_failed++ < 20)
        printf("%s: %s (rt %u ra %u rb %u simm %d): %s\n", what, op_name(i.op), i.rt, i.ra, i.rb, i.simm, detail);
}

bool same_but(const hk_ctx &a, const hk_ctx &b, int reg) {
    for (int r = 0; r < 32; r++)
        if (r != reg && a.r[r] != b.r[r]) return false;
    return memcmp(reinterpret_cast<const char *>(&a) + CTX_GPRS, reinterpret_cast<const char *>(&b) + CTX_GPRS, CTX_BYTES - CTX_GPRS) == 0;
}

// run it on a, then on b from the same RAM: true when the two leave the same RAM
bool run_both(const Insn &i, hk_ctx &a, hk_ctx &b) {
    memcpy(hk_ram, g_zone0.data(), ZONE);
    run_op(&a, i);
    memcpy(g_zone1.data(), hk_ram, ZONE);
    memcpy(hk_ram, g_zone0.data(), ZONE);
    run_op(&b, i);
    return memcmp(g_zone1.data(), hk_ram, ZONE) == 0;
}

uint32_t word_at(uint32_t ea) {
    uint32_t v;
    memcpy(&v, hk_ram + ea, 4);
    return hk_bswap32(v);
}

}  // namespace

int main() {
    ram_init();
    mmio_set_handler({[](uint32_t p, int) -> uint32_t { fprintf(stderr, "unexpected MMIO read %08x\n", p); return 0; },
                      [](uint32_t p, uint32_t, int) { fprintf(stderr, "unexpected MMIO write %08x\n", p); }});
    g_zone0.resize(ZONE), g_zone1.resize(ZONE);
    for (uint8_t &b : g_zone0) b = (uint8_t)rnd();
    uint64_t unnamed = 0, set_only = 0, moves = 0;

    for (Op op : ALL) {
        const unsigned named = names(op), set = sets(op);
        if (set & ~named) fail("sets() names a field names() doesn't", Insn{0, 0, op}, "");
        for (int trial = 0; trial < 3000; trial++) {
            Insn i;
            hk_ctx c0;
            draw(op, i, c0);
            const bool string = op == Op::lswi || op == Op::lswx || op == Op::stswi || op == Op::stswx;

            // a register it doesn't name
            if (!string) {
                const int x = (int)(rnd() & 31);
                bool is_named = ((named & F_rt) && x == i.rt) || ((named & F_ra) && x == i.ra) || ((named & F_rb) && x == i.rb);
                if (many(op) && x >= i.rt) is_named = true;
                if (!is_named) {
                    hk_ctx a = c0, b = c0;
                    b.r[x] = reaches_ram(op) ? 0x400 + rnd() % 0x1800 : value();
                    const bool ram = run_both(i, a, b);
                    unnamed++;
                    if (!ram || !same_but(a, b, x) || a.r[x] != c0.r[x]) fail("a register not named", i, "the result depends on it, or it changed");
                }
            }

            // the register it only sets
            if (set) {
                const int d = set & F_rt ? i.rt : i.ra;
                const unsigned read = named & ~set;
                const bool also_read = ((read & F_rt) && d == i.rt) || ((read & F_ra) && d == i.ra) || ((read & F_rb) && d == i.rb);
                if (!also_read) {
                    hk_ctx a = c0, b = c0;
                    b.r[d] = value();
                    const bool ram = run_both(i, a, b);
                    set_only++;
                    if (!ram || memcmp(&a, &b, CTX_BYTES) != 0) fail("the register only set", i, "the result depends on what it held");
                }
            }

            // the moves
            if (op == Op::stw || op == Op::stmw || op == Op::lwz || op == Op::lmw) {
                const uint32_t ea = (i.ra ? c0.r[i.ra] : 0) + (uint32_t)i.simm;
                const int first = i.rt, count = op == Op::stw || op == Op::lwz ? 1 : 32 - i.rt;
                hk_ctx a = c0;
                memcpy(hk_ram, g_zone0.data(), ZONE);
                run_op(&a, i);
                moves++;
                bool ok = true;
                if (op == Op::stw || op == Op::stmw) {
                    ok = memcmp(&a, &c0, CTX_BYTES) == 0;
                    for (int k = 0; k < count; k++) ok = ok && word_at(ea + 4 * k) == c0.r[first + k];
                    memcpy(g_zone1.data(), g_zone0.data(), ZONE);   // ... and no other byte of RAM
                    memcpy(g_zone1.data() + ea, hk_ram + ea, 4 * (size_t)count);
                    ok = ok && memcmp(g_zone1.data(), hk_ram, ZONE) == 0;
                } else {
                    ok = memcmp(hk_ram, g_zone0.data(), ZONE) == 0;
                    hk_ctx want = c0;
                    for (int k = 0; k < count; k++) want.r[first + k] = word_at(ea + 4 * k);
                    ok = ok && memcmp(&a, &want, CTX_BYTES) == 0;
                }
                if (!ok) fail("a move", i, "not whole words between RAM and the registers from rt on, or something else changed");
            }
        }
    }
    printf("test_exec_names: %llu runs with a register not named changed, %llu with the register only set, %llu moves: %d failed\n",
           (unsigned long long)unnamed, (unsigned long long)set_only, (unsigned long long)moves, g_failed);
    return g_failed ? 1 : 0;
}
