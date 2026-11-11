// SPDX-License-Identifier: GPL-3.0-or-later
// Waiting loops, counted instead of run (runtime/sys573/mips.h).
//
// A PlayStation game spends most of a frame waiting for the next vertical blank: the library's
// VSync() counts a timeout down on the stack and compares a counter in RAM that the interrupt
// handler moves on, a dozen instructions a turn, some forty thousand turns a frame. MAME's CPU
// runs alone for a timeslice (to the next timer), and nothing but the CPU changes its registers
// or main RAM inside one: so a turn of such a loop that neither reads a device nor leaves the
// loop does the same again, turn after turn, until the timeslice ends. This finds those loops as
// the interpreter runs them, works out from the code what a turn does (each register and each
// stored word moves by a fixed step; every comparison keeps its result for a known number of
// turns), lets the interpreter run two turns to check that against, and then takes the rest of
// the timeslice's whole turns at once: the registers and the stored words as the last turn
// leaves them, the cycles off m_icount. The state after is the interpreter's own, cycle for
// cycle (tools/regress.py checks), with or without recompiled blocks.
//
// Nothing here knows a game: a loop is taken only if every instruction on its path is one the
// model below does (loads and whole-word stores at fixed RAM addresses, adds, logic on values
// that don't change, comparisons, branches, calls), and anything else stays the interpreter's.
#include "sys573/mips.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "sys573/idle_model.h"
#include "sys573/mips_ops.h"

namespace hk::k573::mips {
namespace {

using X = hk_psx;

constexpr int MAX_PATH = 48;     // the longest loop taken, in instructions
constexpr int MAX_CELLS = 8;     // ... and the most words it stores to
constexpr int MIN_SLICE = 256;   // a shorter timeslice isn't worth looking at

struct Step { uint32_t pc, op; };
struct Loop {
    int len = 0;
    Step path[MAX_PATH];   // from the head: a turn's instructions in the order they run
    uint32_t fails = 0;    // turns the model couldn't take, in a row
    bool bad = false;      // an instruction the model doesn't do, or it was wrong once: left alone
};

// by pc, direct-mapped: the loop an instruction is on the path of, or that none was found from it
struct Slot {
    uint32_t pc = 1;   // (never a pc: not word-aligned)
    int32_t loop = -1;
    uint32_t wait = 0;   // entries to let pass before looking again
};
constexpr uint32_t SLOTS = 4096;
constexpr size_t MAX_LOOPS = 8192;   // more than this found (hours of play: the same loops met again
                                     // after their slots went to others): all forgotten, found anew

struct Stats {
    uint64_t entries = 0, traces = 0, found = 0, attempts = 0, skips = 0, skipped = 0, no_model = 0, short_k = 0, wrong = 0;
    uint64_t short_slices = 0, short_cycles = 0, cycles = 0;
};

struct State {
    std::vector<Slot> slots;
    std::vector<std::unique_ptr<Loop>> loops;
    Stats st;
    bool stats_on = false;
} g;

Slot &slot(uint32_t pc) { return g.slots[(pc >> 2) & (SLOTS - 1)]; }

// the host's memory behind a guest address, when its whole page is memory (main RAM, the ROM): not
// the scratchpad, which shares its page with devices and comes and goes with the cache's settings
const uint8_t *rd_ptr(const X::page_t *pg, uint32_t a) {
    const uint8_t *p = pg[a >> X::PAGE_BITS].direct_r;
    return p ? p + (a & X::PAGE_MASK) : nullptr;
}
uint8_t *wr_ptr(const X::page_t *pg, uint32_t a) {
    uint8_t *p = pg[a >> X::PAGE_BITS].direct_w;
    return p ? p + (a & X::PAGE_MASK) : nullptr;
}

struct Cell {
    uint8_t *p;        // the word in RAM (the host's address: every mirror of it is the same word)
    uint32_t m0;       // what it holds now
    uint32_t d;        // its step a turn, as assumed at the loop's head
    AV now;            // as the turn's stores leave it
    bool stored;
};

struct Model {
    uint32_t r0[32], dr[32];   // the registers now, and each one's step a turn
    int ncell = 0;
    Cell cell[MAX_CELLS];
    uint64_t turns = 0;        // turns from now that follow the path (every comparison as on turn 0)
};

// One turn of the loop from the head, by the model: the registers and stored words as functions of
// the turn's number, given each one's assumed step (m.dr, cell.d). MAME's pipeline as execute_one()
// has it: an instruction reads the registers as they are, a load's value arrives after the next
// instruction (dropped if that one loads the same register), a branch takes effect after its
// delay slot. False: an instruction or a shape the model doesn't do.
bool turn(psxcpu_device &d, const Loop &lp, Model &m, AV end[32]) {
    const X::page_t *pg = X::pages(d);
    AV r[32];
    for (int i = 0; i < 32; i++) r[i] = {m.r0[i], m.dr[i]};
    for (int i = 0; i < m.ncell; i++) m.cell[i].stored = false, m.cell[i].now = {m.cell[i].m0, m.cell[i].d};
    uint8_t *plain[MAX_PATH];   // words read as they are in RAM (not stored to before, this turn)
    bool plain_part[MAX_PATH];  // ... a byte or a half of it
    int nplain = 0;
    uint32_t pend_r = 0;        // a load on its way
    AV pend_v = {0, 0};
    bool in_slot = false;
    uint32_t target = 0;
    uint64_t turns = FOREVER;
    auto commit = [&] { if (pend_r) r[pend_r] = pend_v, pend_r = 0; };
    auto alu = [&](uint32_t reg, AV v) { commit(); if (reg) r[reg] = v; };
    auto load = [&](uint32_t reg, AV v) {
        if (pend_r == reg) pend_r = 0; else commit();
        pend_r = reg, pend_v = v;
    };
    auto fixed = [](AV a) { return a.s == 0; };
    for (int j = 0; j < lp.len; j++) {
        const uint32_t pc = lp.path[j].pc, op = lp.path[j].op;
        const uint32_t next = lp.path[j + 1 < lp.len ? j + 1 : 0].pc;
        const uint32_t rs = (op >> 21) & 31, rt = (op >> 16) & 31, rd = (op >> 11) & 31, sh = (op >> 6) & 31;
        const uint32_t imm = op & 0xffff, simm = (uint32_t)(int32_t)(int16_t)imm;
        const AV a = r[rs], b = r[rt];
        bool branch = false, taken = false;
        uint32_t to = 0, link = 0;
        switch (op >> 26) {
        case 0:
            switch (op & 63) {
            case 0: alu(rd, {b.v << sh, b.s << sh}); break;                                  // sll
            case 2: if (!fixed(b)) return false; alu(rd, {b.v >> sh, 0}); break;             // srl
            case 3: if (!fixed(b)) return false; alu(rd, {(uint32_t)((int32_t)b.v >> sh), 0}); break;   // sra
            case 4: if (!fixed(a) || !fixed(b)) return false; alu(rd, {b.v << (a.v & 31), 0}); break;   // sllv
            case 6: if (!fixed(a) || !fixed(b)) return false; alu(rd, {b.v >> (a.v & 31), 0}); break;   // srlv
            case 7: if (!fixed(a) || !fixed(b)) return false; alu(rd, {(uint32_t)((int32_t)b.v >> (a.v & 31)), 0}); break;
            case 8: if (!fixed(a)) return false; branch = taken = true, to = a.v; break;     // jr
            case 9: if (!fixed(a)) return false; branch = taken = true, to = a.v, link = rd; break;   // jalr
            case 0x20: {                                                                      // add
                if (!fixed(a) || !fixed(b)) return false;
                const uint32_t v = a.v + b.v;
                if ((int32_t)(~(a.v ^ b.v) & (a.v ^ v)) < 0) return false;   // overflow: an exception
                alu(rd, {v, 0});
                break;
            }
            case 0x21: alu(rd, {a.v + b.v, a.s + b.s}); break;                               // addu
            case 0x22: {                                                                      // sub
                if (!fixed(a) || !fixed(b)) return false;
                const uint32_t v = a.v - b.v;
                if ((int32_t)((a.v ^ b.v) & (a.v ^ v)) < 0) return false;
                alu(rd, {v, 0});
                break;
            }
            case 0x23: alu(rd, {a.v - b.v, a.s - b.s}); break;                               // subu
            case 0x24: if (!fixed(a) || !fixed(b)) return false; alu(rd, {a.v & b.v, 0}); break;
            case 0x25:                                                                        // or (a move, with r0)
                if (fixed(b) && b.v == 0) alu(rd, a);
                else if (fixed(a) && a.v == 0) alu(rd, b);
                else if (fixed(a) && fixed(b)) alu(rd, {a.v | b.v, 0});
                else return false;
                break;
            case 0x26: if (!fixed(a) || !fixed(b)) return false; alu(rd, {a.v ^ b.v, 0}); break;
            case 0x27: if (!fixed(a) || !fixed(b)) return false; alu(rd, {~(a.v | b.v), 0}); break;
            case 0x2a: turns = std::min(turns, stable_lt(a, b, true)); alu(rd, {(uint32_t)((int32_t)a.v < (int32_t)b.v), 0}); break;
            case 0x2b: turns = std::min(turns, stable_lt(a, b, false)); alu(rd, {(uint32_t)(a.v < b.v), 0}); break;
            default: return false;   // the multiplier, syscall, break
            }
            break;
        case 1:   // bltz, bgez (not the linking ones)
            if (rt > 1) return false;
            turns = std::min(turns, stable_lt(a, {0, 0}, true));
            branch = true, taken = ((int32_t)a.v < 0) == (rt == 0), to = pc + 4 + (simm << 2);
            break;
        case 2: branch = taken = true, to = ((pc + 4) & 0xf0000000) + ((op & 0x03ffffff) << 2); break;             // j
        case 3: branch = taken = true, to = ((pc + 4) & 0xf0000000) + ((op & 0x03ffffff) << 2), link = 31; break;   // jal
        case 4: case 5:   // beq, bne
            turns = std::min(turns, stable_eq(a, b));
            branch = true, taken = (a.v == b.v) == ((op >> 26) == 4), to = pc + 4 + (simm << 2);
            break;
        case 6: case 7:   // blez, bgtz
            if (rt != 0) return false;
            turns = std::min(turns, stable_lt({0, 0}, a, true));
            branch = true, taken = ((int32_t)a.v > 0) == ((op >> 26) == 7), to = pc + 4 + (simm << 2);
            break;
        case 8: {   // addi
            if (!fixed(a)) return false;
            const uint32_t v = a.v + simm;
            if ((int32_t)(~(a.v ^ simm) & (a.v ^ v)) < 0) return false;
            alu(rt, {v, 0});
            break;
        }
        case 9: alu(rt, {a.v + simm, a.s}); break;                                           // addiu
        case 10: turns = std::min(turns, stable_lt(a, {simm, 0}, true)); alu(rt, {(uint32_t)((int32_t)a.v < (int32_t)simm), 0}); break;
        case 11: turns = std::min(turns, stable_lt(a, {simm, 0}, false)); alu(rt, {(uint32_t)(a.v < simm), 0}); break;
        case 12: if (!fixed(a)) return false; alu(rt, {a.v & imm, 0}); break;
        case 13: if (imm == 0) alu(rt, a); else if (fixed(a)) alu(rt, {a.v | imm, 0}); else return false; break;
        case 14: if (!fixed(a)) return false; alu(rt, {a.v ^ imm, 0}); break;
        case 15: alu(rt, {imm << 16, 0}); break;                                             // lui
        case 32: case 33: case 35: case 36: case 37: {   // lb, lh, lw, lbu, lhu
            if (!fixed(a)) return false;
            const uint32_t at = a.v + simm, kind = op >> 26;
            const uint32_t align = kind == 35 ? 3 : (kind == 33 || kind == 37) ? 1 : 0;
            if (at & align) return false;
            const uint8_t *p = rd_ptr(pg, at);
            if (!p) return false;   // a device: read by the interpreter
            uint8_t *word = const_cast<uint8_t *>(p) - (at & 3);
            Cell *c = nullptr;
            for (int i = 0; i < m.ncell; i++)
                if (m.cell[i].p == word) c = &m.cell[i];
            AV v;
            if (kind == 35) {
                if (c) v = c->now;
                else { memcpy(&v.v, p, 4); v.s = 0; }
            } else {
                if (c) return false;   // part of a word the loop stores to
                if (kind == 32) v = {(uint32_t)(int32_t)(int8_t)*p, 0};
                else if (kind == 36) v = {*p, 0};
                else { uint16_t h; memcpy(&h, p, 2); v = {kind == 33 ? (uint32_t)(int32_t)(int16_t)h : h, 0}; }
            }
            if (!c) plain[nplain] = word, plain_part[nplain++] = kind != 35;
            load(rt, v);
            break;
        }
        case 43: {   // sw
            if (!fixed(a)) return false;
            const uint32_t at = a.v + simm;
            if (at & 3) return false;
            uint8_t *p = wr_ptr(pg, at);
            if (!p || p != rd_ptr(pg, at)) return false;   // not RAM
            for (int i = 0; i < lp.len; i++)
                if (rd_ptr(pg, lp.path[i].pc) == p) return false;   // the loop's own code
            for (int i = 0; i < nplain; i++)
                if (plain[i] == p && plain_part[i]) return false;
            Cell *c = nullptr;
            for (int i = 0; i < m.ncell; i++)
                if (m.cell[i].p == p) c = &m.cell[i];
            if (!c) {
                if (m.ncell == MAX_CELLS) return false;
                c = &m.cell[m.ncell++];
                c->p = p;
                memcpy(&c->m0, p, 4);
                c->d = 0;
            }
            c->now = b;
            c->stored = true;
            commit();
            break;
        }
        default:
            return false;   // the coprocessors, partial loads and stores, bytes and halves stored
        }
        if (branch) {
            if (in_slot) return false;   // a branch in a delay slot
            commit();
            if (link) r[link] = {pc + 8, 0};
            // the path the loop was seen to take: the delay slot, then where this goes on turn 0
            if (j + 1 >= lp.len || lp.path[j + 1].pc != pc + 4) return false;
            target = taken ? to : pc + 8;
            if (lp.path[j + 2 < lp.len ? j + 2 : 0].pc != target) return false;
            in_slot = true;
        } else if (in_slot) {
            in_slot = false;   // (where it goes was checked at the branch)
        } else if (next != pc + 4) {
            return false;   // the path leaves the code some other way (an exception)
        }
    }
    if (pend_r || in_slot) return false;
    for (int i = 0; i < 32; i++) end[i] = r[i];
    m.turns = turns;
    return true;
}

// The loop from here: each register's and each stored word's step a turn, found by running the
// model with a guess (first: nothing moves) until a turn leaves everything a step on from where
// it started, with the same steps.
bool model(psxcpu_device &d, const Loop &lp, Model &m) {
    for (int i = 0; i < 32; i++) m.r0[i] = X::r(d, i), m.dr[i] = 0;
    m.ncell = 0;
    for (int pass = 0; pass < 4; pass++) {
        AV end[32];
        if (!turn(d, lp, m, end)) return false;
        bool same = true;
        for (int i = 1; i < 32; i++) {
            const uint32_t step = end[i].v - m.r0[i];
            if (step != m.dr[i] || end[i].s != m.dr[i]) same = false;
            m.dr[i] = step;
        }
        for (int i = 0; i < m.ncell; i++) {
            Cell &c = m.cell[i];
            if (!c.stored) return false;   // (a word stored to on one pass and not the next: not a steady loop)
            const uint32_t step = c.now.v - c.m0;
            if (step != c.d || c.now.s != c.d) same = false;
            c.d = step;
        }
        if (same) return true;
    }
    return false;
}

// the registers and the stored words are as the model has them after n turns
bool matches(psxcpu_device &d, const Model &m, uint32_t n) {
    for (int i = 1; i < 32; i++)
        if (X::r(d, i) != m.r0[i] + n * m.dr[i]) return false;
    for (int i = 0; i < m.ncell; i++) {
        uint32_t v;
        memcpy(&v, m.cell[i].p, 4);
        if (v != m.cell[i].m0 + n * m.cell[i].d) return false;
    }
    return true;
}

bool code_there(psxcpu_device &d, const Loop &lp) {
    const X::page_t *pg = X::pages(d);
    for (int i = 0; i < lp.len; i++) {
        const uint8_t *p = rd_ptr(pg, lp.path[i].pc);
        uint32_t w;
        if (!p) return false;
        memcpy(&w, p, 4);
        if (w != lp.path[i].op) return false;
    }
    return true;
}

void forget(const Loop &lp) {
    for (int i = 0; i < lp.len; i++)
        if (Slot &s = slot(lp.path[i].pc); s.pc == lp.path[i].pc) s = Slot{};
}

// The CPU is on a known loop's path: the interpreter to the loop's head, the model from there,
// two turns by the interpreter against it, the timeslice's other whole turns at once.
void attempt(psxcpu_device &d, Loop &lp) {
    if (!code_there(d, lp)) { forget(lp); return; }
    const uint32_t head = lp.path[0].pc;
    for (int i = 0; i < lp.len && (X::pc(d) != head || X::delayr(d) != 0) && X::icount(d) > 0; i++) X::step(d);
    if (X::icount(d) < 4 * lp.len || X::pc(d) != head || X::delayr(d) != 0 || !X::fast_mode(d)) return;
    if (g.stats_on) g.st.attempts++;
    Model m;
    if (!model(d, lp, m)) {
        if (g.stats_on) g.st.no_model++;
        if (++lp.fails >= 64) lp.bad = true;   // not a loop the model does: no more tries
        return;
    }
    lp.fails = 0;
    if (m.turns < 8) {   // about to leave (the wait is over): the interpreter's
        if (g.stats_on) g.st.short_k++;
        return;
    }
    // two turns by the interpreter, each instruction where the path has it, the state as the model says
    for (uint32_t t = 1; t <= 2; t++) {
        for (int j = 0; j < lp.len; j++) {
            if (X::pc(d) != lp.path[j].pc || X::icount(d) <= 0) {
                if (X::icount(d) > 0) lp.bad = true, g.st.wrong++;
                return;
            }
            X::step(d);
        }
        if (X::pc(d) != head || X::delayr(d) != 0 || !X::fast_mode(d) || !matches(d, m, t)) {
            lp.bad = true, g.st.wrong++;
            return;
        }
    }
    if (X::icount(d) <= 0) return;
    // the rest: turns 2 to n + 1, every one before the first whose comparisons may differ
    const uint64_t n = std::min<uint64_t>(m.turns - 3, (uint64_t)(X::icount(d) / lp.len));
    if (n == 0) return;
    const uint32_t total = 2 + (uint32_t)n;
    for (int i = 1; i < 32; i++) X::r(d, i) = m.r0[i] + total * m.dr[i];
    for (int i = 0; i < m.ncell; i++) {
        const uint32_t v = m.cell[i].m0 + total * m.cell[i].d;
        memcpy(m.cell[i].p, &v, 4);
    }
    X::icount_ref(d) -= (int)(n * (uint64_t)lp.len);
    if (g.stats_on) g.st.skips++, g.st.skipped += n * (uint64_t)lp.len;
}

// From a pc no loop is known for: the interpreter runs on, watched, until it comes back to an
// instruction it ran before (outside a delay slot, no load on its way): a loop, its path the
// instructions between.
void discover(psxcpu_device &d, Slot &from) {
    if (g.stats_on) g.st.traces++;
    const uint32_t entry = X::pc(d);
    Step tr[MAX_PATH];
    bool clean[MAX_PATH];
    int n = 0;
    uint32_t last = 0;
    const X::page_t *pg = X::pages(d);
    while (n < MAX_PATH && X::icount(d) > 0 && X::fast_mode(d)) {
        const uint32_t pc = X::pc(d);
        const bool c = X::delayr(d) == 0;
        if (c && n && pc <= last)   // a jump back: to an instruction of this trace?
            for (int i = 0; i < n; i++)
                if (tr[i].pc == pc && clean[i]) {
                    const int len = n - i;
                    // the loop, if it is known already (come to from outside its path): else a new one
                    int32_t id = -1;
                    if (const Slot &h = slot(pc); h.pc == pc && h.loop >= 0)
                        if (const Loop &k = *g.loops[(size_t)h.loop];
                            k.len == len && memcmp(k.path, &tr[i], sizeof(Step) * (size_t)len) == 0)
                            id = h.loop;
                    if (id < 0) {
                        auto lp = std::make_unique<Loop>();
                        lp->len = len;
                        memcpy(lp->path, &tr[i], sizeof(Step) * (size_t)len);
                        id = (int32_t)g.loops.size();
                        g.loops.push_back(std::move(lp));
                        if (g.stats_on) g.st.found++;
                    }
                    Loop &lp = *g.loops[(size_t)id];
                    for (int k = 0; k < len; k++) slot(lp.path[k].pc) = Slot{lp.path[k].pc, id, 0};
                    if (!lp.bad) attempt(d, lp);
                    return;
                }
        const uint8_t *p = (pc & 3) ? nullptr : rd_ptr(pg, pc);
        if (!p) break;
        tr[n].pc = pc;
        memcpy(&tr[n].op, p, 4);
        clean[n++] = c;
        last = pc;
        X::step(d);
    }
    from = Slot{entry, -1, 255};   // none from here: the next 255 entries at this pc pass
}

// MAME's execute_run() starts a timeslice. True: the timeslice is used up (execute_run returns).
bool idle(psxcpu_device &d) {
    if (X::icount(d) < MIN_SLICE || !X::fast_mode(d)) {
        if (g.stats_on) g.st.short_slices++, g.st.short_cycles += (uint64_t)std::max(X::icount(d), 0);
        return false;
    }
    if (g.stats_on) g.st.entries++, g.st.cycles += (uint64_t)X::icount(d);
    if (g.loops.size() > MAX_LOOPS) {
        g.loops.clear();
        g.slots.assign(SLOTS, Slot{});
    }
    Slot &s = slot(X::pc(d));
    if (s.pc != X::pc(d)) {
        discover(d, s);
    } else if (s.loop < 0) {
        if (s.wait == 0) discover(d, s);
        else s.wait--;
    } else if (Loop &lp = *g.loops[(size_t)s.loop]; !lp.bad) {
        attempt(d, lp);
    } else if (++lp.fails >= 65536) {   // a long while on: the code there may be another by now
        forget(lp);
    }
    return X::icount(d) <= 0;
}

}  // namespace

void idle_init(psxcpu_device &cpu) {
    if (const char *e = getenv("HK_573_IDLE"); e && strcmp(e, "0") == 0) return;
    g.stats_on = getenv("HK_573_MIPS_STATS") != nullptr;
    g.slots.assign(SLOTS, Slot{});
    cpu.m_hk_idle = idle;
}

std::string idle_report() {
    if (!g.stats_on || g.slots.empty()) return {};
    char b[600];
    snprintf(b, sizeof b,
             "mips: waiting loops: %" PRIu64 " timeslices looked at (%" PRIu64 " cycles; %" PRIu64 " shorter ones passed, %" PRIu64
             " cycles), %" PRIu64 " traced, %zu loops found; %" PRIu64 " tries, %" PRIu64 " took %" PRIu64
             " instructions at once; left to the interpreter: %" PRIu64 " not the model's, %" PRIu64 " about to end, %" PRIu64
             " where the model was wrong",
             g.st.entries, g.st.cycles, g.st.short_slices, g.st.short_cycles, g.st.traces, g.loops.size(), g.st.attempts,
             g.st.skips, g.st.skipped, g.st.no_model, g.st.short_k, g.st.wrong);
    return b;
}

}  // namespace hk::k573::mips
