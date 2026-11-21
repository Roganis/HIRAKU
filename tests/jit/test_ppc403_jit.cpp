// SPDX-License-Identifier: GPL-3.0-or-later
// The executor's made code (runtime/core/exec_jit.cpp, runtime/jit/ppc403_stencils.c) against
// the interpreter of steps (exec_run.inc), on made-up programs: no game. Each program is a few
// functions of random instructions (every kind the executor has a step for: arithmetic with and
// without its record and overflow forms, rotates, compares, the condition register, the special
// registers kept in hk_ctx and those the runtime keeps (the time base among them), loads and
// stores of every width and form into RAM, its mirror, past RAM's end and into a made-up device,
// load and store multiple and string, forward branches of every BO shape, loops counted by the
// count register, by a register with a poll on the way back, or left by a branch out, calls,
// calls through the count register, tail calls, jump tables, conditional returns), loaded into
// the executor with a timer whose events write RAM, mod hooks on some instructions, and one
// program with a function a patch changed. Each run is done twice from the same state, by the
// interpreter of steps and by made code, and everything compared: the registers, the budget and
// virtual time, all of RAM, every device access with the time it came at, every event and hook
// with the registers it saw.
//
//   test_ppc403_jit <scratch folder> [programs] [seed]      exit 0 when all hold
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "core/exec.h"
#include "core/hooks.h"
#include "core/runtime.h"
#include "core/sched.h"
#include "core/cpu403.h"
#include "jit/ppc403_layout.h"
#include "ppc/decode.h"
#include "ppc/spec.h"

using namespace hk;

namespace {

constexpr uint32_t BASE = 0x80100000;      // the program (physical 0x100000)
constexpr uint32_t DATA = 0x00200000;      // r2: what random loads and stores work on
constexpr uint32_t STACK = 0x00300000;     // r1
constexpr uint32_t TABLES = 0x00006000;    // jump tables (reached with rA = 0)
constexpr uint32_t DEVICE = 0x70000000;    // r13: not RAM
constexpr uint32_t RETURN = 0x80100ff0;    // where the first call returns to (nothing runs there)
constexpr size_t COMPARED = 4u << 20;      // RAM compared every run (all of it every 8th)

// ---- a small assembler ------------------------------------------------------------------------
uint32_t D(unsigned op, unsigned rt, unsigned ra, uint32_t imm) { return op << 26 | (rt & 31) << 21 | (ra & 31) << 16 | (imm & 0xffff); }
uint32_t X(unsigned xo, unsigned rt, unsigned ra, unsigned rb, unsigned rc = 0, unsigned oe = 0) {
    return 31u << 26 | (rt & 31) << 21 | (ra & 31) << 16 | (rb & 31) << 11 | oe << 10 | xo << 1 | rc;
}
uint32_t M(unsigned op, unsigned rs, unsigned ra, unsigned sh, unsigned mb, unsigned me, unsigned rc) {
    return op << 26 | rs << 21 | ra << 16 | sh << 11 | mb << 6 | me << 1 | rc;
}
uint32_t XL(unsigned xo, unsigned bt, unsigned ba, unsigned bb) { return 19u << 26 | bt << 21 | ba << 16 | bb << 11 | xo << 1; }
uint32_t mfspr(unsigned rt, unsigned spr) { return X(339, rt, spr & 31, spr >> 5); }
uint32_t mtspr(unsigned spr, unsigned rs) { return X(467, rs, spr & 31, spr >> 5); }
constexpr uint32_t BLR = 0x4e800020, BCTR = 0x4e800420, BCTRL = 0x4e800421, MFLR0 = 0x7c0802a6, MTLR0 = 0x7c0803a6;

struct Asm {
    std::vector<uint32_t> w;
    std::vector<int64_t> at;                       // a label's word, -1 until placed
    struct Fix { size_t word; int label; bool b; };
    std::vector<Fix> fix;
    int label() { at.push_back(-1); return (int)at.size() - 1; }
    void place(int l) { at[(size_t)l] = (int64_t)w.size(); }
    void put(uint32_t x) { w.push_back(x); }
    void bc(unsigned bo, unsigned bi, int l) { fix.push_back({w.size(), l, false}); put(16u << 26 | bo << 21 | bi << 16); }
    void b(int l) { fix.push_back({w.size(), l, true}); put(18u << 26); }
    uint32_t addr(size_t word, uint32_t base) const { return base + 4 * (uint32_t)word; }
    void resolve() {
        for (const Fix &f : fix) {
            const int32_t d = (int32_t)(4 * (at[(size_t)f.label] - (int64_t)f.word));
            w[f.word] |= f.b ? (uint32_t)d & 0x3fffffcu : (uint32_t)d & 0xfffcu;
        }
    }
};

// ---- what the made-up machine saw -----------------------------------------------------------
struct Seen {
    std::vector<uint64_t> log;   // device accesses, events, hooks: what and when
};
Seen g_seen;
uint32_t g_device_count = 0;

uint32_t device_read(uint32_t phys, int size) {
    const uint32_t v = (phys * 2654435761u) ^ (g_device_count++ * 40503u);
    g_seen.log.push_back((uint64_t)phys << 32 | (uint32_t)size << 24 | 1);
    g_seen.log.push_back(now());
    return size == 1 ? v & 0xff : size == 2 ? v & 0xffff : v;
}
void device_write(uint32_t phys, uint32_t value, int size) {
    g_seen.log.push_back((uint64_t)phys << 32 | (uint32_t)size << 24 | 2);
    g_seen.log.push_back((uint64_t)value << 32 | (uint32_t)now());
}

uint32_t g_period = 100;
uint32_t g_events = 0;
Timer *g_timer = nullptr;
void on_event() {
    g_events++;
    g_seen.log.push_back(0xe000000000000000ull | now());
    const uint32_t v = __builtin_bswap32(g_events * 0x01010101u);
    memcpy(hk_ram + DATA + 0x40, &v, 4);   // (a word the programs read)
    g_timer->again(g_period);
}

void hook_fn(void *ctx, uint32_t *regs) {
    const uint32_t k = (uint32_t)(uintptr_t)ctx;
    g_seen.log.push_back(0xd000000000000000ull | (uint64_t)k << 32 | regs[3]);
    regs[5] ^= 0x9e3779b9u * (k + 1);
    regs[32] ^= 0x00100000u;   // (cr: cr3's lt)
    hk_ram[DATA + (k & 0xff)] ^= 0x5a;
}

// ---- random programs --------------------------------------------------------------------------
struct Gen {
    std::mt19937 &r;
    Asm &a;
    int funcs, me;                 // functions, and which one this is (it calls only later ones)
    const std::vector<uint32_t> &entries;
    std::vector<std::pair<size_t, std::vector<int>>> &tables;   // a bctr's word and its cases
    bool in_loop = false;

    unsigned pick(unsigned n) { return (unsigned)(r() % n); }
    bool chance(unsigned pct) { return pick(100) < pct; }
    // a destination: never r1 (stack), r2 (data), r12 (data's mirror), r13 (device); in a loop not
    // r11 (its counter)
    unsigned dst() {
        for (;;) {
            const unsigned d = pick(32);
            if (d == 1 || d == 2 || d == 12 || d == 13 || (in_loop && d == 11)) continue;
            return d;
        }
    }
    unsigned src() { return pick(32); }
    uint32_t simm() { return chance(30) ? pick(16) - 8 : r() & 0xffff; }

    // an index in r11's place for the indexed forms: rS masked to the data area (any alignment)
    unsigned index_reg() {
        const unsigned x = in_loop ? 10 : 11;
        a.put(M(21, src(), x, pick(32), 16, 31, 0));   // rlwinm x, rS, n, 16, 31: 0 to 0xffff
        return x;
    }

    void arith() {
        static const unsigned xo3[] = {266, 10, 138, 40, 8, 136, 235, 491, 459};   // add addc adde subf subfc subfe mullw divw divwu
        static const unsigned xo2[] = {202, 234, 200, 232, 104};                   // addze addme subfze subfme neg
        static const unsigned hi[] = {75, 11};                                     // mulhw mulhwu
        static const unsigned logic[] = {28, 60, 444, 412, 316, 476, 124, 284, 24, 536, 792};   // and andc or orc xor nand nor eqv slw srw sraw
        static const unsigned one[] = {954, 922, 26};                              // extsb extsh cntlzw
        const unsigned rc = chance(25), oe = chance(10);
        switch (pick(17)) {
        case 0: a.put(X(xo3[pick(9)], dst(), src(), src(), rc, oe)); break;
        case 1: a.put(X(xo2[pick(5)], dst(), src(), 0, rc, oe)); break;
        case 2: a.put(X(hi[pick(2)], dst(), src(), src(), rc)); break;
        case 3: { const unsigned s = src(); a.put(X(logic[pick(11)], s, dst(), chance(30) ? s : src(), rc)); break; }   // (or rA, rS, rS: mr)
        case 4: a.put(X(one[pick(3)], src(), dst(), 0, rc)); break;
        case 5: a.put(X(824, src(), dst(), pick(32), rc)); break;                                 // srawi
        case 6: a.put(D(14 + pick(2), dst(), chance(30) ? 0 : src(), simm())); break;             // addi addis (li lis)
        case 7: a.put(D(12 + pick(2), dst(), src(), simm())); break;                              // addic addic.
        case 8: a.put(D(pick(2) ? 8 : 7, dst(), src(), simm())); break;                           // subfic mulli
        case 9: a.put(D(24 + pick(6), src(), dst(), r() & 0xffff)); break;                        // ori oris xori xoris andi. andis.
        case 10: a.put(M(21, src(), dst(), pick(32), pick(32), pick(32), rc)); break;             // rlwinm
        case 11: a.put(M(20, src(), dst(), pick(32), pick(32), pick(32), rc)); break;             // rlwimi
        case 12: a.put(M(23, src(), dst(), src(), pick(32), pick(32), rc)); break;                // rlwnm
        case 13: a.put(D(11 - pick(2), pick(8) << 2, src(), simm())); break;                      // cmpi cmpli (crfd)
        case 14: a.put(X(pick(2) ? 32 : 0, pick(8) << 2, src(), src())); break;                   // cmp cmpl
        case 15: {   // the condition register
            static const unsigned cr[] = {257, 129, 289, 225, 33, 449, 417, 193};
            switch (pick(5)) {
            case 0: a.put(XL(cr[pick(8)], pick(32), pick(32), pick(32))); break;
            case 1: a.put(XL(0, pick(8) << 2, pick(8) << 2, 0)); break;                           // mcrf
            case 2: a.put(X(512, pick(8) << 2, 0, 0)); break;                                     // mcrxr
            case 3: a.put(X(19, dst(), 0, 0)); break;                                             // mfcr
            default: a.put(31u << 26 | src() << 21 | pick(256) << 12 | 144 << 1); break;          // mtcrf
            }
            break;
        }
        default: {   // special registers
            static const unsigned word[] = {1, 26, 27, 990, 991, 272, 273, 274, 275};
            static const unsigned kept[] = {287, 0x3dd, 0x3dc, 0x3d8, 0x3c};   // PVR, the time base, TSR, an unnamed one
            switch (pick(9)) {
            case 0: case 1: a.put(mfspr(dst(), word[pick(9)])); break;
            case 2: case 3: a.put(mtspr(word[pick(9)], src())); break;
            case 4: a.put(mfspr(dst(), kept[pick(5)])); break;
            case 5: a.put(mtspr(pick(2) ? 0x3d8 : 0x3c, src())); break;
            case 6: a.put(X(83, dst(), 0, 0)); break;                                             // mfmsr
            case 7: a.put(mfspr(dst(), pick(2) ? 8 : 9)); break;                                  // mflr mfctr
            default: a.put(D(3, 0, src(), simm())); break;                                        // twi 0: never traps
            }
            break;
        }
        }
    }

    void memory() {
        // loads lwz lwzu lbz lbzu lhz lhzu lha lhau; stores stw stwu stb stbu sth sthu
        static const unsigned ld[] = {32, 34, 40, 42}, st[] = {36, 38, 44};
        static const unsigned ldx[] = {23, 87, 279, 343, 790, 534}, stx[] = {151, 215, 407, 918, 662};
        const bool store = chance(45);
        switch (pick(11)) {
        case 9: case 0: case 1: case 2: {   // d(r2), d(r12) (the mirror), d(r13) (the device)
            const unsigned base = chance(70) ? 2 : chance(50) ? 12 : 13;
            const uint32_t d = chance(80) ? (r() & 0x7fff) : (uint32_t)-(int32_t)(r() & 0x7fff);
            a.put(store ? D(st[pick(3)], src(), base, d) : D(ld[pick(4)], dst(), base, d));
            break;
        }
        case 3: {   // update forms, from r11 (or r10 in a loop) set first
            const unsigned x = in_loop ? 10 : 11;
            a.put(D(14, x, chance(80) ? 2 : 13, r() & 0x3fff));
            unsigned d = dst();
            a.put(store ? D(st[pick(3)] + 1, src(), x, r() & 0xfff) : D(ld[pick(4)] + 1, d == x ? 3 : d, x, r() & 0xfff));
            break;
        }
        case 4: {   // indexed forms (byte-reversed among them), from r2 or 0
            const unsigned x = index_reg();
            const unsigned ra = chance(15) ? 0 : 2;
            if (ra == 0) a.put(X(266, x, x, 2));   // (add x, x, r2: the same place, from 0)
            a.put(store ? X(stx[pick(5)], src(), ra, x) : X(ldx[pick(6)], dst(), ra, x));
            break;
        }
        case 5: {   // indexed update forms
            const unsigned x = index_reg();
            const unsigned y = x == 11 ? 10 : 9;
            if (in_loop && y == 11) break;
            a.put(D(14, y, 2, r() & 0x3fff));
            static const unsigned lu[] = {55, 119, 311, 375}, su[] = {183, 247, 439};
            a.put(store ? X(su[pick(3)], src(), y, x) : X(lu[pick(4)], 3, y, x));
            break;
        }
        case 6: {   // 0 + d: low RAM (loads only: the jump tables are there) or past 0x7fff8000 (the device)
            if (store) a.put(D(st[pick(3)], src(), 0, 0x8000 | (r() & 0x7fff)));
            else a.put(D(ld[pick(4)], dst(), 0, r() & 0xffff));
            break;
        }
        case 7: case 8: {   // RAM's last bytes: an access that is partly past it goes to the device
            const unsigned x = in_loop ? 10 : 11;
            a.put(D(15, x, 0, chance(50) ? 0x0200 : 0x8200));                 // lis x, 0x0200 (or its mirror)
            const uint32_t d = (uint32_t)-(int32_t)pick(5);                   // -4 .. 0
            a.put(store ? D(st[pick(3)], src(), x, d) : D(ld[pick(4)], dst(), x, d));
            break;
        }
        default: {   // multiple and string, dcbz, lwarx / stwcx.
            switch (pick(5)) {
            case 0: a.put(D(46, 14 + pick(18), 2, r() & 0x7ff0 & ~3u)); break;               // lmw r14+
            case 1: a.put(D(47, 1 + pick(31), chance(85) ? 2 : 13, r() & 0x7ff0)); break;    // stmw
            case 2: a.put(X(store ? 725 : 597, 20, 2, pick(32))); break;                       // stswi lswi r20 (to r27)
            case 3: { const unsigned x = index_reg(); a.put(X(1014, 0, 2, x)); break; }          // dcbz
            default: {
                const unsigned x = index_reg();
                a.put(X(20, dst(), 2, x));                                                     // lwarx
                if (chance(70)) a.put(X(150, src(), 2, x, 1));                                 // stwcx.
                break;
            }
            }
            break;
        }
        }
    }

    // a straight piece: n of instructions and accesses, maybe a forward branch over some
    void piece(int n) {
        for (int k = 0; k < n; k++) {
            const unsigned what = pick(100);
            if (what < 55) arith();
            else if (what < 90) memory();
            else if (what < 97 && k + 2 < n) {   // a forward branch over the next few
                static const unsigned bo[] = {12, 4, 20, 16, 18, 0, 2, 8, 10};   // (the count register's only outside loops)
                const unsigned o = in_loop ? bo[pick(3)] : bo[pick(9)];
                const int l = a.label();
                a.bc(o, chance(50) ? pick(4) : pick(32), l);   // (cr0's bits the most, as programs do)
                const int skip = 1 + (int)pick(3);
                for (int q = 0; q < skip; q++) arith();
                a.place(l);
            } else {
                a.put(0x60000000);   // nop
            }
        }
    }

    void loop() {
        in_loop = true;
        const unsigned kind = pick(3);
        const int top = a.label(), out = a.label();
        if (kind == 0) {        // counted: mtctr, bdnz
            a.put(D(14, 11, 0, 1 + pick(6)));
            a.put(mtspr(9, 11));
            a.place(top);
            piece(2 + (int)pick(6));
            a.bc(16, 0, top);
        } else if (kind == 1) { // by r11: addic. r11, r11, -1; bne top (a poll on the way back)
            a.put(D(14, 11, 0, 1 + pick(6)));
            a.place(top);
            piece(2 + (int)pick(6));
            a.put(D(13, 11, 11, (uint32_t)-1));
            a.bc(4, 2, top);
        } else {                // left by a branch out; b top
            a.put(D(14, 11, 0, 1 + pick(6)));
            a.place(top);
            a.put(D(13, 11, 11, (uint32_t)-1));
            a.bc(12, 2, out);
            piece(1 + (int)pick(5));
            a.b(top);
        }
        a.place(out);
        in_loop = false;
    }

    void call() {
        if (me + 1 >= funcs) return;
        const uint32_t t = entries[(size_t)(me + 1 + (int)pick((unsigned)(funcs - me - 1)))];
        if (chance(70)) {   // bl
            const int32_t d = (int32_t)(t - a.addr(a.w.size(), entries[(size_t)me]));
            a.put(18u << 26 | ((uint32_t)d & 0x3fffffcu) | 1);
        } else {            // through the count register
            a.put(D(15, 11, 0, t >> 16));
            a.put(D(24, 11, 11, t & 0xffff));
            a.put(mtspr(9, 11));
            a.put(BCTRL);
        }
    }

    // a jump table: one of 2 to 4 cases by r3's low bits, each its own piece, then on together
    void table(uint32_t table_at) {
        const unsigned n = 2 + pick(3);
        std::vector<int> cases;
        for (unsigned k = 0; k < n; k++) cases.push_back(a.label());
        const int join = a.label();
        a.put(M(21, src(), 11, 2, 28, 29, 0));        // rlwinm r11, rS, 2, 28, 29: 0, 4, 8, 12
        a.put(D(14, 11, 11, table_at & 0x7fff));      // addi r11, r11, table
        a.put(D(32, 11, 11, 0));                      // lwz r11, 0(r11)
        a.put(mtspr(9, 11));
        tables.push_back({a.w.size(), cases});
        a.put(BCTR);
        for (unsigned k = 0; k < n; k++) {
            a.place(cases[k]);
            piece(1 + (int)pick(3));
            a.b(join);
        }
        a.place(join);
    }
};

struct Program {
    std::vector<uint32_t> words;
    std::string spec;
    std::vector<uint32_t> entries;
    std::vector<uint32_t> table_words;   // (address, value) pairs for low RAM
    std::vector<uint32_t> insns;         // every instruction's address (for hooks)
};

Program make_program(std::mt19937 &r, int funcs, bool nopoll_some) {
    Program p;
    // functions laid out one after another; their entries are known only once each is made, and a
    // call goes to a later one: made last to first
    std::vector<Asm> bodies((size_t)funcs);
    std::vector<std::vector<std::pair<size_t, std::vector<int>>>> tables((size_t)funcs);
    std::vector<uint32_t> sizes((size_t)funcs);
    p.entries.assign((size_t)funcs, 0);
    // sizes first need the entries: lay out with a fixed room each (4 KiB)
    for (int f = 0; f < funcs; f++) p.entries[(size_t)f] = BASE + 0x1000u * (uint32_t)f;
    uint32_t table_at = TABLES;
    std::string spec = "base 80100000\n";
    for (int f = funcs - 1; f >= 0; f--) {
        Asm &a = bodies[(size_t)f];
        Gen g{r, a, funcs, f, p.entries, tables[(size_t)f]};
        a.put(MFLR0);
        a.put(D(36, 0, 1, 4));                // stw r0, 4(r1)
        a.put(D(37, 1, 1, (uint32_t)-32));    // stwu r1, -32(r1)
        const int parts = 3 + (int)g.pick(8);
        for (int k = 0; k < parts; k++) {
            const unsigned what = g.pick(10);
            if (what < 4) g.piece(3 + (int)g.pick(14));
            else if (what < 6) g.loop();
            else if (what < 8) g.call();
            else { g.table(table_at); table_at += 16; }
        }
        a.put(D(14, 1, 1, 32));               // addi r1, r1, 32
        a.put(D(32, 0, 1, 4));                // lwz r0, 4(r1)
        a.put(MTLR0);
        const unsigned end = g.pick(4);
        if (end == 0 && f + 1 < funcs) {      // a tail call to a later function
            const uint32_t t = p.entries[(size_t)f + 1 + g.pick((unsigned)(funcs - f - 1))];
            const int32_t d = (int32_t)(t - (p.entries[(size_t)f] + 4 * (uint32_t)a.w.size()));
            a.put(18u << 26 | ((uint32_t)d & 0x3fffffcu));
        } else if (end == 1) {                // a conditional return first
            a.put(19u << 26 | 12u << 21 | g.pick(32) << 16 | 16 << 1);
            a.put(BLR);
        } else {
            a.put(BLR);
        }
        a.resolve();
        if (a.w.size() * 4 > 0x1000) fprintf(stderr, "test: a function too long\n"), exit(2);
        sizes[(size_t)f] = (uint32_t)a.w.size() * 4;
    }
    // the jump tables' words: each table's cases in order (the case addresses), 4 slots
    uint32_t t_at = TABLES;
    for (int f = funcs - 1; f >= 0; f--) {
        for (auto &[word, cases] : tables[(size_t)f]) {
            for (size_t k = 0; k < 4; k++) {
                const int l = cases[k % cases.size()];
                p.table_words.push_back(t_at + 4 * (uint32_t)k);
                p.table_words.push_back(p.entries[(size_t)f] + 4 * (uint32_t)bodies[(size_t)f].at[(size_t)l]);
            }
            t_at += 16;
        }
    }
    p.words.assign((size_t)funcs * 0x400, 0x60000000);
    for (int f = 0; f < funcs; f++) {
        const Asm &a = bodies[(size_t)f];
        for (size_t k = 0; k < a.w.size(); k++) {
            p.words[(size_t)f * 0x400 + k] = a.w[k];
            p.insns.push_back(p.entries[(size_t)f] + 4 * (uint32_t)k);
        }
        char buf[96];
        snprintf(buf, sizeof buf, "func %x%s\nrange %x %x\n", p.entries[(size_t)f], nopoll_some && (r() & 3) == 0 ? " nopoll" : "",
                 p.entries[(size_t)f], p.entries[(size_t)f] + sizes[(size_t)f]);
        spec += buf;
        for (auto &[word, cases] : tables[(size_t)f]) {
            snprintf(buf, sizeof buf, "jt %x", p.entries[(size_t)f] + 4 * (uint32_t)word);
            spec += buf;
            std::set<uint32_t> ts;
            for (int l : cases) ts.insert(p.entries[(size_t)f] + 4 * (uint32_t)a.at[(size_t)l]);
            for (uint32_t t : ts) snprintf(buf, sizeof buf, " %x", t), spec += buf;
            spec += "\n";
        }
        spec += "end\n";
    }
    p.spec = spec;
    return p;
}

// ---- one run ----------------------------------------------------------------------------------
struct State {
    hk_ctx c{};
    Cpu403 cpu;   // the special and device control registers the runtime keeps
    int32_t budget = 0;
    uint64_t time = 0;
    std::vector<uint8_t> ram;
    std::vector<uint64_t> log;
};

void set_up(const Program &p, uint32_t seed) {
    std::mt19937 r(seed);
    memset(hk_ram, 0, 0x400000);
    memset(hk_ram + HK_RAM_SIZE - 0x1000, 0, 0x1000);   // (RAM's last bytes: the accesses across its end)
    for (size_t k = 0; k < p.words.size(); k++) {
        const uint32_t be = __builtin_bswap32(p.words[k]);
        memcpy(hk_ram + (BASE & 0x7fffffff) + 4 * k, &be, 4);
    }
    for (size_t k = 0; k + 1 < p.table_words.size(); k += 2) {
        const uint32_t be = __builtin_bswap32(p.table_words[k + 1]);
        memcpy(hk_ram + p.table_words[k], &be, 4);
    }
    for (uint32_t a = DATA - 0x8000; a < DATA + 0x10100; a++) hk_ram[a] = (uint8_t)r();
    g_cpu = Cpu403{};
    cpu403::reset();
    sched_reset();
    g_seen = {};
    g_device_count = 0;
    g_events = 0;
    g_period = 20 + (uint32_t)(r() % 400);
    g_timer->in(g_period);
}

hk_ctx registers(uint32_t seed) {
    std::mt19937 r(seed ^ 0x51ed270bu);
    hk_ctx c{};
    for (int k = 0; k < 32; k++) c.r[k] = r();
    c.r[1] = STACK;
    c.r[2] = DATA;
    c.r[12] = 0x80000000u | DATA;
    c.r[13] = DEVICE;
    c.cr = r();
    c.xer = r() & 0xe000007fu;
    c.ctr = r();
    c.lr = RETURN;
    c.msr = 0;
    c.srr0 = r(), c.srr1 = r(), c.srr2 = r(), c.srr3 = r();
    for (uint32_t &s : c.sprg) s = r();
    return c;
}

State run(const Program &p, uint32_t entry, uint32_t seed, bool made, bool all_ram) {
    set_up(p, seed);
    State s;
    s.c = registers(seed);
    exec::interpret_only(!made);
    exec::call(&s.c, entry);
    exec::interpret_only(false);
    s.budget = hk_budget;
    s.time = now();
    s.cpu = g_cpu;
    s.ram.assign(hk_ram, hk_ram + (all_ram ? HK_RAM_SIZE : COMPARED));
    s.log = g_seen.log;
    return s;
}

int g_failed = 0;
void check(bool ok, const std::string &what) {
    if (!ok || getenv("HK_TEST_VERBOSE")) printf("%s%s\n", ok ? "ok    " : "FAIL  ", what.c_str());
    g_failed += !ok;
}

// the stencils' op list is the instruction set's (src/ppc/decode.h), names and fields
bool lists_agree() {
    std::set<std::string> all, ours;
#define S0(n) all.insert(#n);
#define S1(n, a) all.insert(#n " " #a);
#define S2(n, a, b) all.insert(#n " " #a " " #b);
#define S3(n, a, b, c) all.insert(#n " " #a " " #b " " #c);
#define S4(n, a, b, c, d) all.insert(#n " " #a " " #b " " #c " " #d);
#define S5(n, a, b, c, d, e) all.insert(#n " " #a " " #b " " #c " " #d " " #e);
#define S6(n, a, b, c, d, e, f) all.insert(#n " " #a " " #b " " #c " " #d " " #e " " #f);
    HK_OPLIST(S0, S1, S2, S3, S4, S5, S6)
#undef S0
#undef S1
#undef S2
#undef S3
#undef S4
#undef S5
#undef S6
#define S0(n) ours.insert(#n);
#define S1(n, a) ours.insert(#n " " #a);
#define S2(n, a, b) ours.insert(#n " " #a " " #b);
#define S3(n, a, b, c) ours.insert(#n " " #a " " #b " " #c);
#define S4(n, a, b, c, d) ours.insert(#n " " #a " " #b " " #c " " #d);
#define S5(n, a, b, c, d, e) ours.insert(#n " " #a " " #b " " #c " " #d " " #e);
#define S6(n, a, b, c, d, e, f) ours.insert(#n " " #a " " #b " " #c " " #d " " #e " " #f);
    HK_P4_OPS(S0, S1, S2, S3, S4, S5, S6)
    bool ok = true;
    for (const std::string &s : ours)
        if (!all.count(s)) printf("      not in HK_OPLIST: %s\n", s.c_str()), ok = false;
    return ok;
}

}  // namespace

int main(int argc, char **argv) {
    const std::string dir = argc > 1 ? argv[1] : "ppc403-jit-test";
    const int programs = argc > 2 ? atoi(argv[2]) : 40;
    const uint32_t seed0 = argc > 3 ? (uint32_t)strtoul(argv[3], nullptr, 0) : 403;
    std::filesystem::create_directories(dir);
    const std::string image = dir + "/program.bin";

    check(lists_agree(), "the stencils' instructions are HK_OPLIST's, names and fields");
    ram_init();
    mmio_set_handler({device_read, device_write});
    Timer timer("test", on_event);
    g_timer = &timer;
    if (!exec::made_available()) {
        printf("skipped: no made code here (no stencils for this processor, or HK_JIT=0 / HK_403_JIT=0)\n");
        return 0;
    }

    uint64_t runs = 0, steps_compared = 0, events = 0, device = 0, hooked = 0;
    for (int n = 0; n < programs; n++) {
        std::mt19937 r(seed0 + 7919u * (uint32_t)n);
        const int funcs = 2 + (int)(r() % 5);
        const Program p = make_program(r, funcs, true);
        {
            std::ofstream f(image, std::ios::binary);
            for (uint32_t w : p.words) {
                const uint32_t be = __builtin_bswap32(w);
                f.write(reinterpret_cast<const char *>(&be), 4);
            }
        }
        bool decodes = true;
        for (uint32_t w : p.words) {
            const Op op = decode(w, 0).op;
            decodes &= op != Op::invalid && op != Op::unimpl;
        }
        if (!decodes) {
            check(false, "program " + std::to_string(n) + ": every word decodes");
            continue;
        }
        // a few hooks (one program in three)
        hooks::clear();
        if (n % 3 == 1)
            for (int k = 0; k < 4; k++) {
                std::string err;
                hooks::add(p.insns[r() % p.insns.size()], hook_fn, reinterpret_cast<void *>((uintptr_t)k), "test", err);
            }
        if (const char *d = getenv("HK_TEST_DUMP"); d && atoi(d) == n) {   // (a failing program, to read)
            printf("%s", p.spec.c_str());
            for (size_t k = 0; k < p.words.size(); k++)
                if (p.words[k] != 0x60000000u || (k && p.words[k - 1] != 0x60000000u))
                    printf("  %08x  %08x  %s\n", BASE + 4 * (uint32_t)k, p.words[k], disasm(decode(p.words[k], BASE + 4 * (uint32_t)k)).c_str());
        }
        const std::string e = exec::load_text(p.spec, image);
        if (!e.empty()) {
            check(false, "program " + std::to_string(n) + " loads: " + e);
            continue;
        }
        check(exec::made_count() == exec::count(), "program " + std::to_string(n) + ": every function made (" +
                                                       std::to_string(exec::made_count()) + " of " + std::to_string(exec::count()) + ")");
        // the last program: its second function changed by a patch (interpreted from then on)
        if (n == programs - 1 && funcs > 2) patch_functions(*exec::program(), p.entries[1], p.entries[1] + 4);
        for (int k = 0; k < 16; k++) {
            const uint32_t s = r();
            const uint32_t entry = p.entries[(size_t)(k % funcs)];
            const bool all = runs % 8 == 0;
            const State a = run(p, entry, s, false, all), b = run(p, entry, s, true, all);
            runs++;
            const std::string what = "program " + std::to_string(n) + " run " + std::to_string(k) + " (" + std::to_string(funcs) +
                                     " functions, entry " + std::to_string(entry - BASE) + ")";
            if (memcmp(&a.c, &b.c, offsetof(hk_ctx, reserve) + 4) != 0) {
                check(false, what + ": the registers");
                for (size_t q = 0; q <= offsetof(hk_ctx, reserve) / 4; q++) {
                    const uint32_t x = reinterpret_cast<const uint32_t *>(&a.c)[q], y = reinterpret_cast<const uint32_t *>(&b.c)[q];
                    if (x != y) printf("      word %zu: steps %08x, made %08x\n", q, x, y);
                }
                continue;
            }
            if (a.budget != b.budget || a.time != b.time) {
                check(false, what + ": the budget " + std::to_string(a.budget) + " / " + std::to_string(b.budget) + ", time " +
                                 std::to_string(a.time) + " / " + std::to_string(b.time));
                continue;
            }
            if (memcmp(&a.cpu, &b.cpu, sizeof a.cpu) != 0) {
                check(false, what + ": the special registers the runtime keeps");
                continue;
            }
            if (a.ram != b.ram) {
                size_t at = 0;
                while (a.ram[at] == b.ram[at]) at++;
                char buf[96];
                snprintf(buf, sizeof buf, ": RAM (first at %zx: %02x / %02x)", at, a.ram[at], b.ram[at]);
                check(false, what + buf);
                continue;
            }
            if (a.log != b.log) {
                check(false, what + ": what the device, the events and the hooks saw (" + std::to_string(a.log.size()) + " / " +
                                 std::to_string(b.log.size()) + " entries)");
                continue;
            }
            check(a.c.pc == RETURN, what + ": back at the caller");
            steps_compared += (uint64_t)(a.time);
            for (uint64_t v : a.log) {
                events += (v >> 60) == 0xe;
                hooked += (v >> 60) == 0xd;
                device += (v >> 60) < 0xd && (v & 0xff) != 0;
            }
        }
    }
    printf("%llu runs of %d programs compared (%llu cycles of the machine's time, %llu events, %llu hook runs, ~%llu device accesses); "
           "made code %zu KiB\n",
           (unsigned long long)runs, programs, (unsigned long long)steps_compared, (unsigned long long)events, (unsigned long long)hooked,
           (unsigned long long)device, exec::made_bytes() >> 10);
    printf("%s\n", g_failed ? "FAILED" : "all ok");
    return g_failed ? 1 : 0;
}
