// SPDX-License-Identifier: GPL-3.0-or-later
// The executor's recorder (exec_steps.h, record()): the dispatcher of exec.cpp once more, over
// accessors that note every byte of RAM a run reads before writing it and every byte it writes,
// and over the runtime's services wrapped to notice what a remembered run cannot hold: a device,
// the clock, the scheduler, code that isn't the executor's.
//
// The instructions' semantics are include/hiraku/ppc.h's, as everywhere: HK_MEM_HOOKS has that
// header leave hk_r8...hk_w32 to this file, and the services it calls are renamed to the wrappers
// below. So this must be the first thing to include it.
//
// What the recorder itself knows of the instruction set is where a general register can be named
// (an instruction's three five-bit fields, src/ppc/decode.h's rt, ra, rb; the load and store
// multiple and string instructions, which go on from rt), and that stw, lwz, stmw and lmw move a
// register's whole word to or from RAM with one hk_w32 or hk_r32 each, in register order. From
// that it keeps apart the words a run only moves (exec_steps.h, Copy): a register or a word of
// RAM whose value the run found is "as found" until an instruction other than those four names
// it, or its bytes are read or written in any other way, which makes it one the run depends on.
// HK_EXEC_MEMO=check (exec.cpp) runs every remembered run again and compares.
#define HK_MEM_HOOKS 1
#define hk_mfspr hk_rec_mfspr
#define hk_mtspr hk_rec_mtspr
#define hk_mfdcr hk_rec_mfdcr
#define hk_mtdcr hk_rec_mtdcr
#define hk_mtmsr hk_rec_mtmsr
#include "hiraku/ppc.h"
#undef hk_mfspr
#undef hk_mtspr
#undef hk_mfdcr
#undef hk_mtdcr
#undef hk_mtmsr

extern "C" {
uint32_t hk_mfspr(hk_ctx *c, uint32_t spr);
void hk_mtspr(hk_ctx *c, uint32_t spr, uint32_t v);
uint32_t hk_mfdcr(hk_ctx *c, uint32_t dcr);
void hk_mtdcr(hk_ctx *c, uint32_t dcr, uint32_t v);
void hk_mtmsr(hk_ctx *c, uint32_t v);
}

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <unordered_map>
#include <vector>

#include "exec_names.h"
#include "exec_steps.h"
#include "runtime.h"

namespace hk::exec {
namespace {

constexpr size_t MAX_TOUCHED = 1 << 16;   // bytes of RAM a run may touch and still be remembered
constexpr size_t MAX_FOUND = 2048;        // words it may hold as found

struct Touched { uint32_t at; uint8_t was; };
constexpr uint8_t READ = 1, WRITTEN = 2, MOVED = 4;   // MOVED: a byte of a word stored as found (and WRITTEN)

// a word as the run found it: a general register's, or four bytes of RAM (no byte of them touched
// before). `used`: the run depends on what it held, so that is compared when the run is looked up.
struct Found {
    bool ram, used;
    uint32_t at;        // the register's number, or the physical address
    uint8_t was[4];     // RAM: the bytes
};

struct Recorder {
    bool on = false;
    bool outside = false;            // the run did something that can't be remembered
    uint8_t *flag = nullptr;         // per byte of RAM: READ (before any write of the run's), WRITTEN, MOVED
    std::vector<Touched> touched;    // the bytes flagged, each with what it held when first read
    std::vector<Found> found;        // [0, 32): the general registers
    uint16_t holds[32] = {};         // per general register: 1 + the Found it still holds, 0: a value the run made
    std::unordered_map<uint32_t, uint16_t> moved;   // words of RAM that hold a Found (1 +), by address
    int moving = 0;                  // the instruction under way moves words: 1 loads, 2 stores
    int reg = 0, left = 0;           // ... the register of its next word, and the words to come
} g;

inline void use(uint16_t held) {
    if (held) g.found[held - 1].used = true;
}
// a register named by an instruction that isn't a plain move: what it held decides the run from here
inline void use_reg(unsigned r) {
    use(g.holds[r]);
    g.holds[r] = 0;
}

// a byte of a word stored as found is read or written on its own: the word is one the run depends on
void use_moved(uint32_t byte, bool drop) {
    for (uint32_t at = byte >= 3 ? byte - 3 : 0; at <= byte; at++) {
        auto it = g.moved.find(at);
        if (it == g.moved.end()) continue;
        use(it->second);
        if (drop) {   // written over in part: no longer a whole word of it
            for (uint32_t k = 0; k < 4; k++) g.flag[at + k] &= (uint8_t)~MOVED;
            g.moved.erase(it);
        }
    }
}

inline void note_read(uint32_t p, uint32_t n) {
    if (g.outside) return;
    for (uint32_t k = 0; k < n; k++) {
        uint8_t &f = g.flag[p + k];
        if (__builtin_expect(f & MOVED, 0)) use_moved(p + k, false);
        if (f) continue;
        f = READ;
        g.touched.push_back({p + k, hk_ram[p + k]});
    }
    if (g.touched.size() > MAX_TOUCHED) g.outside = true;
}

inline void note_write(uint32_t p, uint32_t n) {
    if (g.outside) return;
    for (uint32_t k = 0; k < n; k++) {
        uint8_t &f = g.flag[p + k];
        if (__builtin_expect(f & MOVED, 0)) use_moved(p + k, true);
        if (!f) g.touched.push_back({p + k, 0});
        f |= WRITTEN;
    }
    if (g.touched.size() > MAX_TOUCHED) g.outside = true;
}

// lwz, lmw: the word at p into the next register
inline void load_word(uint32_t p) {
    const int r = g.reg++ & 31;
    if (--g.left <= 0) g.moving = 0;
    g.holds[r] = 0;
    if (g.outside) return;
    const uint8_t *f = g.flag + p;
    if (!(f[0] | f[1] | f[2] | f[3])) {   // RAM the run hasn't touched: the word as found, not read yet
        if (g.found.size() >= MAX_FOUND) { g.outside = true; return; }
        Found w{true, false, p, {}};
        memcpy(w.was, hk_ram + p, 4);
        g.found.push_back(w);
        g.holds[r] = (uint16_t)g.found.size();
        return;
    }
    if (f[0] & MOVED)   // a word the run stored as found, whole
        if (auto it = g.moved.find(p); it != g.moved.end()) {
            g.holds[r] = it->second;
            return;
        }
    note_read(p, 4);
}

// stw, stmw: the next register's word to p
inline void store_word(const hk_ctx *c, uint32_t p, uint32_t v) {
    const int r = g.reg++ & 31;
    if (--g.left <= 0) g.moving = 0;
    if (c->r[r] != v) g.outside = true;   // (not the register's word: not the move this takes it for)
    if (g.outside) return;
    uint16_t held = g.holds[r];
    if (held && g.found[held - 1].used) held = g.holds[r] = 0;
    if (!held) { note_write(p, 4); return; }
    for (uint32_t k = 0; k < 4; k++)   // (words stored as found that this one covers in part)
        if (g.flag[p + k] & MOVED) use_moved(p + k, true);
    for (uint32_t k = 0; k < 4; k++) {
        uint8_t &f = g.flag[p + k];
        if (!f) g.touched.push_back({p + k, 0});
        f |= WRITTEN | MOVED;
    }
    g.moved[p] = held;
    if (g.touched.size() > MAX_TOUCHED) g.outside = true;
}

// before each step: which registers the instruction names (exec_names.h), and whether it only
// moves words. A register it reads is one the run depends on, if it still holds what the run
// found; one it sets holds, from here, a value the run made.
inline void step(const Step *st) {
    g.moving = 0;
    const Op op = op_of(st->k);
    if (op == Op::invalid) return;   // control: the condition, link and count registers, never "as found"
    switch (op) {
    case Op::lwz: use_reg(st->ra); g.moving = 1, g.reg = st->rt, g.left = 1; return;
    case Op::lmw: use_reg(st->ra); g.moving = 1, g.reg = st->rt, g.left = 32 - st->rt; return;
    case Op::stw: use_reg(st->ra); g.moving = 2, g.reg = st->rt, g.left = 1; return;
    case Op::stmw: use_reg(st->ra); g.moving = 2, g.reg = st->rt, g.left = 32 - st->rt; return;
    default: break;
    }
    if (many(op)) {
        for (unsigned r = 0; r < 32; r++) use_reg(r);
        return;
    }
    const unsigned named = names(op), set = sets(op);
    if (named & ~set & F_rt) use_reg(st->rt);
    if (named & ~set & F_ra) use_reg(st->ra);
    if (named & ~set & F_rb) use_reg(st->rb);
    if (set & F_rt) g.holds[st->rt] = 0;
    if (set & F_ra) g.holds[st->ra] = 0;
}

void run_rec(hk_ctx *c, const Code *k);

// a call or jump to an address worked out as the program runs: the executor's function there by
// the recorder too, anything else (dispatch.cpp's call_guest: a function a mod changed, the
// game binary's, the interpreter's) as it is, unrecorded
void dyn(hk_ctx *c, uint32_t target, bool jump) {
    if (!patched(target) && !lookup(target))
        if (const Code *k = find(target)) {
            run_rec(c, k);
            return;
        }
    g.outside = true;
    if (jump) hk_jump(c, target);
    else hk_call_dyn(c, target);
}

}  // namespace
}  // namespace hk::exec

// ppc.h's accessors (the same lines), noting RAM and marking a device's access
static inline uint32_t hk_r8(hk_ctx *c, uint32_t ea) {
    uint32_t p = HK_PHYS(ea);
    if (__builtin_expect(p < HK_RAM_SIZE, 1)) { hk::exec::note_read(p, 1); return hk_ram[p]; }
    hk::exec::g.outside = true;
    return hk_mmio_read(c, p, 1) & 0xff;
}
static inline uint32_t hk_r16(hk_ctx *c, uint32_t ea) {
    uint32_t p = HK_PHYS(ea);
    if (__builtin_expect(p <= HK_RAM_SIZE - 2, 1)) { hk::exec::note_read(p, 2); uint16_t v; memcpy(&v, hk_ram + p, 2); return hk_bswap16(v); }
    hk::exec::g.outside = true;
    return hk_mmio_read(c, p, 2) & 0xffff;
}
static inline uint32_t hk_r32(hk_ctx *c, uint32_t ea) {
    uint32_t p = HK_PHYS(ea);
    if (__builtin_expect(p <= HK_RAM_SIZE - 4, 1)) {
        if (hk::exec::g.moving == 1) hk::exec::load_word(p);
        else hk::exec::note_read(p, 4);
        uint32_t v;
        memcpy(&v, hk_ram + p, 4);
        return hk_bswap32(v);
    }
    hk::exec::g.outside = true;
    return hk_mmio_read(c, p, 4);
}
static inline void hk_w8(hk_ctx *c, uint32_t ea, uint32_t v) {
    uint32_t p = HK_PHYS(ea);
    if (__builtin_expect(p < HK_RAM_SIZE, 1)) { hk::exec::note_write(p, 1); hk_ram[p] = (uint8_t)v; return; }
    hk::exec::g.outside = true;
    hk_mmio_write(c, p, v & 0xff, 1);
}
static inline void hk_w16(hk_ctx *c, uint32_t ea, uint32_t v) {
    uint32_t p = HK_PHYS(ea);
    if (__builtin_expect(p <= HK_RAM_SIZE - 2, 1)) { hk::exec::note_write(p, 2); uint16_t s = hk_bswap16((uint16_t)v); memcpy(hk_ram + p, &s, 2); return; }
    hk::exec::g.outside = true;
    hk_mmio_write(c, p, v & 0xffff, 2);
}
static inline void hk_w32(hk_ctx *c, uint32_t ea, uint32_t v) {
    uint32_t p = HK_PHYS(ea);
    if (__builtin_expect(p <= HK_RAM_SIZE - 4, 1)) {
        if (hk::exec::g.moving == 2) hk::exec::store_word(c, p, v);
        else hk::exec::note_write(p, 4);
        uint32_t s = hk_bswap32(v);
        memcpy(hk_ram + p, &s, 4);
        return;
    }
    hk::exec::g.outside = true;
    hk_mmio_write(c, p, v, 4);
}

// The services ppc.h's semantics call (state.cpp). A remembered run holds registers and RAM: the
// special registers kept in hk_ctx are those; any other (the time base, the timers, a register
// that is the runtime's) and every device control register isn't. Setting the machine state
// register with interrupts enabled asks the runtime whether one is waiting.
extern "C" {

uint32_t hk_rec_mfspr(hk_ctx *c, uint32_t n) {
    using namespace hk::spr;
    switch (n) {
    case SRR0: case SRR1: case SRR2: case SRR3: case SPRG0: case SPRG1: case SPRG2: case SPRG3: break;
    default: hk::exec::g.outside = true; break;
    }
    return hk_mfspr(c, n);
}

void hk_rec_mtspr(hk_ctx *c, uint32_t n, uint32_t v) {
    using namespace hk::spr;
    switch (n) {
    case SRR0: case SRR1: case SRR2: case SRR3: case SPRG0: case SPRG1: case SPRG2: case SPRG3: break;
    default: hk::exec::g.outside = true; break;
    }
    hk_mtspr(c, n, v);
}

uint32_t hk_rec_mfdcr(hk_ctx *c, uint32_t n) {
    hk::exec::g.outside = true;
    return hk_mfdcr(c, n);
}

void hk_rec_mtdcr(hk_ctx *c, uint32_t n, uint32_t v) {
    hk::exec::g.outside = true;
    hk_mtdcr(c, n, v);
}

void hk_rec_mtmsr(hk_ctx *c, uint32_t v) {
    if (v & hk::msr::EE) hk::exec::g.outside = true;
    hk_mtmsr(c, v);
}

}  // extern "C"

namespace hk::exec {
namespace {

#define HK_RUN run_rec
#define HK_RUN_STEP(st) step(st)
#define HK_RUN_OUTSIDE() (g.outside = true)
#define HK_RUN_CALL_DYN(c, t) dyn((c), (t), false)
#define HK_RUN_JUMP(c, t) dyn((c), (t), true)
#include "exec_run.inc"
#undef HK_RUN
#undef HK_RUN_STEP
#undef HK_RUN_OUTSIDE
#undef HK_RUN_CALL_DYN
#undef HK_RUN_JUMP

// the bytes flagged `which` (and not `but`), in address order, as stretches: theirs appended to data
template <typename From>
void spans(uint8_t which, uint8_t but, std::vector<Span> &out, std::vector<uint8_t> &data, From from) {
    for (const Touched &t : g.touched) {
        const uint8_t f = g.flag[t.at];
        if (!(f & which) || (f & but)) continue;
        if (out.empty() || out.back().at + out.back().n != t.at) out.push_back({t.at, 0, (uint32_t)data.size()});
        out.back().n++;
        data.push_back(from(t));
    }
}

// what the run did, from the notes
bool finish(hk_ctx *c, Memo &m) {
    memcpy(&m.out, c, CTX_BYTES);
    for (unsigned r = 0; r < 32; r++)
        if (g.found[r].used) m.used |= 1u << r;
    // words stored as found whose value turned out to matter after all: plain written bytes
    for (auto it = g.moved.begin(); it != g.moved.end();) {
        if (!g.found[it->second - 1].used) { ++it; continue; }
        for (uint32_t k = 0; k < 4; k++) g.flag[it->first + k] &= (uint8_t)~MOVED;
        it = g.moved.erase(it);
    }
    std::sort(g.touched.begin(), g.touched.end(), [](const Touched &a, const Touched &b) { return a.at < b.at; });
    spans(READ, 0, m.reads, m.rdata, [](const Touched &t) { return t.was; });
    for (size_t n = 32; n < g.found.size(); n++) {   // RAM held as found, then depended on: read
        const Found &w = g.found[n];
        if (!w.used) continue;
        m.reads.push_back({w.at, 4, (uint32_t)m.rdata.size()});
        m.rdata.insert(m.rdata.end(), w.was, w.was + 4);
    }
    spans(WRITTEN, MOVED, m.writes, m.wdata, [](const Touched &t) { return hk_ram[t.at]; });
    for (const auto &[at, held] : g.moved) {
        const Found &w = g.found[held - 1];
        m.copies.push_back({w.at, at, w.ram, true});
    }
    for (unsigned r = 0; r < 32; r++) {
        const uint16_t held = g.holds[r];
        if (!held || g.found[held - 1].used) continue;
        const Found &w = g.found[held - 1];
        m.copies.push_back({w.at, r, w.ram, false});
    }
    return m.copies.size() <= MAX_COPIES;
}

}  // namespace

bool recording() { return g.on; }

bool record(hk_ctx *c, const Code *k, Memo &m) {
    if (!g.flag) g.flag = static_cast<uint8_t *>(calloc(HK_RAM_SIZE, 1));
    g.on = true;
    g.outside = g.flag == nullptr;
    g.touched.clear();
    g.moved.clear();
    g.found.clear();
    for (unsigned r = 0; r < 32; r++) {
        g.found.push_back({false, false, r, {}});
        g.holds[r] = (uint16_t)(r + 1);
    }
    g.moving = 0;
    memcpy(&m.in, c, CTX_BYTES);
    const int32_t budget = hk_budget;
    run_rec(c, k);
    g.on = false;
    m.cycles = budget - hk_budget;   // (nothing but the steps' charges moved it, or the run is outside)
    const bool ok = !g.outside && finish(c, m);
    if (g.flag)
        for (const Touched &t : g.touched) g.flag[t.at] = 0;
    return ok;
}

}  // namespace hk::exec
