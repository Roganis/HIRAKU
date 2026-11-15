// SPDX-License-Identifier: GPL-3.0-or-later
// R3000 blocks made at run time (runtime/sys573/mipsjit.cpp, the stencils of
// runtime/jit/r3000_stencils.c) against MAME's interpreter, with no game: two bare PlayStation
// CPUs with 4 MiB of RAM each, one interpreting, one with HIRAKU's dispatcher, blocks and waiting
// loops (hk::k573::mips::init), run the same random programs side by side. Every 1/2000 s the
// two must have the same registers, pipeline, HI and LO, COP0, cycle count and memory.
//
// A program is a ring of small functions of random instructions of every kind a block compiles
// (and a few it hands to the interpreter): arithmetic, shifts, the multiplier, loads and stores
// of every width and alignment into RAM, the scratchpad and the root counters (whose values tell
// the time: a cycle off shows), branches with their delay slots, bounded loops back to a block's
// start, calls, the GTE, an overflow or a system call now and then (the exception handler starts
// the ring again), and stores into another function's code (the write guard). A build with no
// stencils for its processor has nothing to compare and says so.
//
//   test_r3000_jit [PROGRAMS [FIRST_SEED [STEPS]]]     (48 programs, 120 steps each: a second or two)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "sys573/mips.h"
#include "sys573/mips_ops.h"
#include "sys573/mipsjit.h"
#include "machine/ram.h"

namespace {

using X = hk_psx;

class bare_ps1 : public device_t {
public:
    bare_ps1(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock = 0)
        : device_t(mconfig, TYPE, tag, owner, clock), m_maincpu(*this, "maincpu"), m_ram(*this, "ram") {}
    static const device_creator<bare_ps1> TYPE;
    required_device<psxcpu_device> m_maincpu;
    required_device<ram_device> m_ram;

    void device_add_mconfig(machine_config &config) override {
        CXD8530CQ(config, m_maincpu, 67.7376_MHz_XTAL);
        m_maincpu->set_ram(m_ram);
        RAM(config, m_ram).set_bits(32).set_default_size("4M").set_default_value(0);
    }
    void device_start() override {}
};
const device_creator<bare_ps1> bare_ps1::TYPE("bare_ps1", "a PlayStation CPU and its RAM");

uint64_t g_rs;
uint32_t rnd() {   // xorshift64*
    g_rs ^= g_rs >> 12, g_rs ^= g_rs << 25, g_rs ^= g_rs >> 27;
    return (uint32_t)((g_rs * 0x2545f4914f6cdd1dULL) >> 32);
}
uint32_t pick(uint32_t n) { return rnd() % n; }

// ---- instruction words ----
uint32_t ins_r(uint32_t fn, uint32_t rs, uint32_t rt, uint32_t rd, uint32_t sa = 0) { return rs << 21 | rt << 16 | rd << 11 | sa << 6 | fn; }
uint32_t ins_i(uint32_t op, uint32_t rs, uint32_t rt, uint32_t imm) { return op << 26 | rs << 21 | rt << 16 | (imm & 0xffff); }
uint32_t ins_j(uint32_t op, uint32_t addr) { return op << 26 | ((addr >> 2) & 0x3ffffff); }
constexpr uint32_t NOP = 0;

// registers: values in 1-7, 16-19 and 21-23; 8-11 point into the data, 12 at the scratchpad, 13
// at the root counters, 14 at another function's code, 20 holds an instruction word to store
// there; 24 counts loops, 25 is the leaf function, 26 the boot's and the exception handler's,
// 27 the next function (kept in memory at NEXT for the handler), 31 the link
constexpr uint32_t DATA = 0x800c0000u, LEAF = 0x80000200u, START = 0x80000100u, NEXT = 0x300;
uint32_t value_reg() { static const uint32_t r[] = {1, 2, 3, 4, 5, 6, 7, 16, 17, 18, 19, 21, 22, 23}; return r[pick(14)]; }
uint32_t any_reg() { return pick(8) == 0 ? pick(32) : value_reg(); }
uint32_t dest_reg() { return pick(24) == 0 ? 0 : value_reg(); }

// one random instruction that is not a branch
uint32_t plain(uint32_t pc, uint32_t other_code) {
    const uint32_t k = pick(1000);
    if (k < 300) {   // register arithmetic
        static const uint32_t fns[] = {33, 35, 36, 37, 38, 39, 42, 43, 4, 6, 7};
        return ins_r(fns[pick(11)], any_reg(), any_reg(), dest_reg());
    }
    if (k < 380) { static const uint32_t fns[] = {0, 2, 3}; return ins_r(fns[pick(3)], 0, any_reg(), dest_reg(), pick(32)); }
    if (k < 520) { static const uint32_t ops[] = {9, 10, 11, 12, 13, 14}; return ins_i(ops[pick(6)], any_reg(), dest_reg(), rnd()); }
    if (k < 550) return ins_i(15, 0, dest_reg(), rnd());                                 // lui
    if (k < 553) return ins_r(pick(2) ? 32 : 34, value_reg(), value_reg(), dest_reg());  // add, sub: may overflow
    if (k < 556) return ins_i(8, value_reg(), dest_reg(), rnd());                        // addi
    if (k < 560) return ins_i(8, 0, dest_reg(), rnd());                                  // (addi from zero: never does)
    if (k < 600) return ins_r(24 + pick(4), any_reg(), any_reg(), 0);                    // mult, multu, div, divu
    if (k < 630) return ins_r(pick(2) ? 16 : 18, 0, 0, dest_reg());                      // mfhi, mflo
    if (k < 640) return ins_r(pick(2) ? 17 : 19, any_reg(), 0, 0);                       // mthi, mtlo
    if (k < 800) {   // loads and stores into the data
        static const uint32_t ops[] = {32, 36, 33, 37, 35, 40, 41, 43, 34, 38, 42, 46};
        const uint32_t op = ops[pick(12)];
        const uint32_t base = 8 + pick(4);
        uint32_t off = pick(0x400);
        const bool whole = pick(400) != 0;   // (seldom: an address the instruction can't take)
        if (whole && (op == 33 || op == 37 || op == 41)) off &= ~1u;
        if (whole && (op == 35 || op == 43)) off &= ~3u;
        return ins_i(op, base, op >= 40 ? any_reg() : dest_reg(), off);
    }
    if (k < 830) {   // the scratchpad
        static const uint32_t ops[] = {35, 43, 36, 40, 37, 41};
        const uint32_t op = ops[pick(6)];
        return ins_i(op, 12, op >= 40 ? any_reg() : dest_reg(), pick(0x100) * 4);
    }
    if (k < 850) return ins_i(pick(2) ? 37 : 35, 13, dest_reg(), 16 * pick(3) + 4 * pick(3));   // a root counter's count, mode or target
    if (k < 856) return ins_i(43, 14, 20, 0xffff);                                       // an instruction word into other code (where: set at the end)
    if (k < 890) return 16u << 26 | 0 << 21 | dest_reg() << 16 | (12 + pick(4)) << 11;   // mfc0 SR, CAUSE, EPC, PRID
    if (k < 893) return 16u << 26 | 4 << 21 | value_reg() << 16 | 3 << 11;               // mtc0 BPC: the interpreter's
    if (k < 920) return 18u << 26 | (pick(2) ? 0u : 2u) << 21 | dest_reg() << 16 | pick(32) << 11;      // mfc2, cfc2
    if (k < 945) return 18u << 26 | (pick(2) ? 4u : 6u) << 21 | any_reg() << 16 | pick(32) << 11;       // mtc2, ctc2
    if (k < 960) { static const uint32_t fun[] = {0x0180001, 0x1400006, 0x0780010, 0x0a00428, 0x190003d}; return 18u << 26 | 1u << 25 | fun[pick(5)]; }
    if (k < 975) return ins_i(pick(2) ? 50 : 58, 8 + pick(4), pick(32), pick(0x100) * 4);  // lwc2, swc2
    if (k < 976) return pick(2) ? 12 : 13;                                               // syscall, break
    (void)pc, (void)other_code;
    return NOP;
}

// a program: a ring of FUNCS functions of LEN instructions each, from base
constexpr int FUNCS = 48, LEN = 40;
void program(uint32_t *ram, uint32_t base) {
    const auto at = [ram](uint32_t a) -> uint32_t & { return ram[(a & 0x3fffff) / 4]; };
    // where a reset goes: on to the program; an exception: on to the function after the one it was in
    at(START) = ins_j(2, base), at(START + 4) = NOP;
    at(0x80000080u) = ins_i(35, 0, 26, NEXT), at(0x80000084u) = NOP;                 // lw r26, NEXT(r0)
    at(0x80000088u) = ins_r(8, 26, 0, 0), at(0x8000008cu) = 16u << 26 | 1u << 25 | 16;   // jr r26; rfe
    at(LEAF) = ins_r(33, 2, 3, 2), at(LEAF + 4) = ins_r(8, 31, 0, 0), at(LEAF + 8) = ins_i(9, 4, 4, 1);   // addu; jr ra; addiu (its slot)
    uint32_t pc = base;
    // set-up: the GTE usable, the pointers, some values
    const auto li = [&](uint32_t r, uint32_t v) {
        at(pc) = ins_i(15, 0, r, v >> 16), pc += 4;
        at(pc) = ins_i(13, r, r, v), pc += 4;
    };
    li(1, 0x40000000u);
    at(pc) = 16u << 26 | 4 << 21 | 1 << 16 | 12 << 11, pc += 4;   // mtc0 r1, SR: CU2
    // the scratchpad switched on, as a BIOS does (the cache control register: RAM, DS)
    at(pc) = ins_i(15, 0, 1, 0xfffe), pc += 4;
    at(pc) = ins_i(13, 0, 2, 0x88), pc += 4;
    at(pc) = ins_i(43, 1, 2, 0x130), pc += 4;
    for (uint32_t r = 1; r < 24; r++)
        if (r < 8 || r >= 16) li(r, rnd());
    for (uint32_t r = 8; r < 12; r++) li(r, DATA + (r - 8) * 0x1000 + 4 * pick(64));
    li(12, 0x1f800000u), li(13, 0x1f801100u), li(25, LEAF);
    li(20, 0x24210001u);   // "addiu r1, r1, 1"
    const uint32_t ring = pc;
    // (stores into code replace only instructions made by plain(): a loop that lost its count's
    // step would never end)
    std::vector<uint32_t> plains[FUNCS], stores[FUNCS];
    const auto put_plain = [&](int f, uint32_t other) {
        at(pc) = plain(pc, other);
        (at(pc) == ins_i(43, 14, 20, 0xffff) ? stores : plains)[f].push_back(pc);
        pc += 4;
    };
    for (int f = 0; f < FUNCS; f++) {
        const uint32_t start = ring + 4 * (uint32_t)(f * LEN), next = ring + 4 * (uint32_t)(((f + 1) % FUNCS) * LEN);
        const uint32_t other = ring + 4 * (uint32_t)(((f + FUNCS / 2) % FUNCS) * LEN);
        pc = start;
        li(14, other);                                  // where this function's stores into code go
        li(27, next);                                   // where an exception goes on
        at(pc) = ins_i(43, 0, 27, NEXT), pc += 4;
        at(pc) = ins_i(9, 0, 24, 2 + pick(6)), pc += 4;   // the loop count
        const uint32_t end = start + 4 * (LEN - 2);
        while (pc < end) {
            const uint32_t k = pick(100);
            const uint32_t room = (end - pc) / 4;
            if (k < 12 && room >= 4) {   // a branch forwards, its delay slot (seldom another branch: the interpreter's)
                static const uint32_t ops[] = {4, 5, 6, 7};
                const uint32_t dist = 1 + pick(room - 2 < 6 ? room - 2 : 6);
                const uint32_t which = pick(8);
                if (which < 4) at(pc) = ins_i(ops[which], any_reg(), any_reg(), dist);
                else at(pc) = ins_i(1, any_reg(), which == 4 ? 0 : which == 5 ? 1 : which == 6 ? 16 : 17, dist);   // bltz, bgez, bltzal, bgezal
                pc += 4;
                if (pick(40) == 0) at(pc) = ins_i(4, 0, 0, 1), pc += 4;
                else put_plain(f, other);
            } else if (k < 17 && pc - start >= 4 * 10 && room >= 4) {   // a loop back, while the count lasts
                at(pc) = ins_i(9, 24, 24, 0xffff), pc += 4;
                const uint32_t back = 2 + pick((pc - start) / 4 - 8 < 12 ? (pc - start) / 4 - 8 : 12);
                at(pc) = ins_i(7, 24, 0, (uint32_t)-(int32_t)back), pc += 4;   // bgtz r24
                put_plain(f, other);
            } else if (k < 20 && room >= 2) {   // a call of the leaf function, by address or by register
                at(pc) = pick(2) ? ins_j(3, LEAF) : ins_r(9, 25, 0, 31), pc += 4;
                put_plain(f, other);
            } else {
                put_plain(f, other);
            }
        }
        while (pc < start + 4 * (LEN - 2)) at(pc) = NOP, pc += 4;
        at(pc) = ins_j(2, next), pc += 4;
        put_plain(f, other);
    }
    for (int f = 0; f < FUNCS; f++) {
        const int o = (f + FUNCS / 2) % FUNCS;
        const uint32_t other = ring + 4 * (uint32_t)(o * LEN);
        for (uint32_t a : stores[f]) at(a) = plains[o].empty() ? NOP : ins_i(43, 14, 20, plains[o][pick((uint32_t)plains[o].size())] - other);
    }
}

struct Machine {
    running_machine m;
    bare_ps1 *dev = nullptr;
    psxcpu_device &cpu() { return *dev->m_maincpu; }
    uint32_t *ram() { return reinterpret_cast<uint32_t *>(dev->m_ram->pointer()); }
    void build() {
        m.m_exact = true;
        // the ROM a reset starts in: on to RAM, where the program's first jump is written
        std::vector<u8> rom(0x80000, 0);
        const uint32_t boot[] = {ins_i(15, 0, 26, START >> 16), ins_i(13, 26, 26, START), ins_r(8, 26, 0, 0), NOP};
        memcpy(rom.data(), boot, sizeof boot);
        m.add_region(":maincpu:rom", std::move(rom));
        dev = &bare_ps1::TYPE(m.config(), "");
        m.start();
    }
};

// what differs between the two CPUs, "" if nothing
std::string differ(Machine &a, Machine &b, bool memory) {
    psxcpu_device &x = a.cpu(), &y = b.cpu();
    char t[160];
    for (uint32_t i = 0; i < 32; i++)
        if (X::r(x, i) != X::r(y, i)) {
            snprintf(t, sizeof t, "r%u: %08x made, %08x interpreted", i, X::r(x, i), X::r(y, i));
            return t;
        }
    if (X::pc(x) != X::pc(y)) { snprintf(t, sizeof t, "pc: %08x made, %08x interpreted", X::pc(x), X::pc(y)); return t; }
    if (X::delayr(x) != X::delayr(y) || X::delayv(x) != X::delayv(y)) return "the pipeline";
    if (X::hi(x) != X::hi(y) || X::lo(x) != X::lo(y)) return "HI or LO";
    for (int i = 0; i < 16; i++)
        if (X::cp0(x, i) != X::cp0(y, i)) { snprintf(t, sizeof t, "COP0 register %d", i); return t; }
    if (static_cast<cpu_device &>(x).total_cycles() != static_cast<cpu_device &>(y).total_cycles()) return "the cycles run";
    if (memory && memcmp(a.ram(), b.ram(), a.dev->m_ram->size()) != 0) {
        const uint32_t *p = a.ram(), *q = b.ram();
        for (uint32_t i = 0; i < a.dev->m_ram->size() / 4; i++)
            if (p[i] != q[i]) { snprintf(t, sizeof t, "memory at %08x: %08x made, %08x interpreted", 0x80000000u + 4 * i, p[i], q[i]); return t; }
    }
    return "";
}

}  // namespace

int main(int argc, char **argv) {
    const int programs = argc > 1 ? atoi(argv[1]) : 48;
    const unsigned first = argc > 2 ? (unsigned)strtoul(argv[2], nullptr, 0) : 1;
    const int steps = argc > 3 ? atoi(argv[3]) : 120;
    if (!hk::k573::mips::jit::available()) {
        printf("test_r3000_jit: skip: no R3000 stencils in this build (or turned off)\n");
        return 0;
    }
    // (the report at the end says how much ran as made blocks)
#ifdef _WIN32
    _putenv_s("HK_573_MIPS_STATS", "1");
#else
    setenv("HK_573_MIPS_STATS", "1", 1);
#endif
    auto made = std::make_unique<Machine>(), interpreted = std::make_unique<Machine>();
    made->build();
    interpreted->build();
    hk::k573::mips::init(made->cpu());
    uint64_t step = 0;
    for (int p = 0; p < programs; p++) {
        const unsigned seed = first + (unsigned)p;
        // each program at a place of its own (a place keeps only a few versions of its code),
        // written into both RAMs; the data random
        const uint32_t base = 0x80010000u + 0x8000u * (uint32_t)(p % 16);
        for (Machine *mc : {made.get(), interpreted.get()}) {
            g_rs = seed * 0x9e3779b97f4a7c15ULL + 1;
            uint32_t *ram = mc->ram();
            for (uint32_t a = DATA; a < DATA + 0x8000; a += 4) ram[(a & 0x3fffff) / 4] = rnd();
            program(ram, base);
            mc->m.reset();
        }
        for (int s = 0; s < steps; s++) {
            step++;
            const attotime t = attotime::from_ticks(step, 2000u);
            made->m.run_until(t);
            interpreted->m.run_until(t);
            const std::string d = differ(*made, *interpreted, s % 8 == 7 || s == steps - 1);
            if (!d.empty()) {
                printf("test_r3000_jit: FAILED: program %d (seed %u), step %d: %s\n%s\n", p, seed, s, d.c_str(), hk::k573::mips::report().c_str());
                return 1;
            }
        }
    }
    const std::string rep = hk::k573::mips::report();
    printf("%.*s\n", (int)rep.find('\n'), rep.c_str());
    printf("test_r3000_jit: %d programs, %d steps each: made blocks and the interpreter agree\n", programs, steps);
    return 0;
}
