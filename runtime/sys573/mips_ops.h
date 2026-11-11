// SPDX-License-Identifier: GPL-3.0-or-later
// What recompiled R3000 blocks (tools/mipsgen.py, runtime/sys573/mips.cpp) use to act on MAME's
// psxcpu_device: its registers, its pipeline (m_delayr/m_delayv, the load and branch delay
// slots), its multiplier and GTE, and the memory map through the shim's page table.
//
// A block runs MAME's instructions in MAME's order with MAME's results, so a run with blocks is
// the same, cycle for cycle, as one without (PLAN.md section 19, 573-4). It runs only in the
// common case the dispatcher checks for (kernel mode, caches not isolated, no hardware
// breakpoints). Anything else an instruction can do (an exception, a bus error, a COP0 write)
// hands that instruction to MAME's interpreter (bail) and ends the block.
#pragma once

#include "emu.h"
#include "cpu/psx/psx.h"

#include <cstring>

#define HK_INLINE inline __attribute__((always_inline))
#define HK_COLD __attribute__((noinline, cold))

// one recompiled block, generated from the instruction words it was compiled from
// one recompiled block, generated from the instruction words it was compiled from. A block runs
// on past conditional branches that aren't taken: its first stretch, up to the first one, is
// len instructions (cycles: MAME's PS1 CPU takes one per instruction); the block itself checks
// there is room in the timeslice before each further stretch.
// more code a block's function runs (the other blocks of a loop compiled with it)
struct hk573_range {
    uint32_t pc, nwords;
    const uint32_t *words;
};
struct hk573_block {
    uint32_t pc;
    uint32_t len;            // the first stretch
    uint32_t nwords;
    const uint32_t *words;   // the code it was compiled from (checked against memory on entry)
    void (*fn)(psxcpu_device &);
    uint32_t nextra;         // the other blocks of its loop, checked with it
    const hk573_range *extra;
};

// friend of psxcpu_device (third_party/mame/hiraku-edits.py)
struct hk_psx {
    using D = psxcpu_device;
    using page_t = address_space::page_t;
    static constexpr int PAGE_BITS = 16;   // the shim's pages for a 32-bit space (checked at start)
    static constexpr uint32_t PAGE_MASK = (1u << PAGE_BITS) - 1;
    static constexpr uint32_t SR_CU2 = 1u << 30;
    static constexpr int DELAY_PC = PSXCPU_DELAYR_PC, DELAY_NOTPC = PSXCPU_DELAYR_NOTPC;

    static HK_INLINE uint32_t &r(D &d, uint32_t i) { return d.m_r[i]; }
    static HK_INLINE uint32_t delayr(D &d) { return d.m_delayr; }
    static HK_INLINE uint32_t delayv(D &d) { return d.m_delayv; }
    static HK_INLINE int icount(D &d) { return d.m_icount; }
    static HK_INLINE int &icount_ref(D &d) { return d.m_icount; }
    // one instruction by MAME's interpreter, as execute_run()'s loop runs it
    static HK_INLINE void step(D &d) { d.execute_one(); d.m_icount--; }
    static HK_INLINE const page_t *pages(D &d) { return d.m_program->pages(); }
    static HK_INLINE uint32_t sr(D &d) { return d.m_cp0r[12]; }
    static HK_INLINE uint32_t cp0(D &d, int reg) { return d.m_cp0r[reg]; }

    // the block ends: MAME's state as the interpreter would have left it
    static HK_INLINE void leave(D &d, uint32_t pc, uint32_t delayr, uint32_t delayv, int icount, uint32_t last_pc) {
        d.m_pc = pc;
        d.m_delayr = delayr;
        d.m_delayv = delayv;
        d.m_icount = icount;
        d.m_hook_pc = last_pc;   // debugger_instruction_hook() of the last instruction
    }
    // the instruction at pc goes to the interpreter (an exception, a bus error, an instruction
    // blocks don't do), from the state before it; then the block ends
    static inline bool bailed = false;   // the interpreter ran an instruction: the mode may have changed
    // moves on whenever checked code may have changed (mips.cpp): a block that loops on itself
    // stops looping
    static inline uint64_t epoch = 1;
    static HK_COLD void bail(D &d, uint32_t pc, uint32_t delayr, uint32_t delayv, int icount) {
        bailed = true;
        d.m_pc = pc;
        d.m_delayr = delayr;
        d.m_delayv = delayv;
        d.m_icount = icount;
        d.execute_one();
        d.m_icount--;
    }

    // ---- memory: RAM and ROM pages directly, anything else through MAME's handlers. A slow
    // access sees MAME's m_icount for this instruction. st: 1 when a device cut the timeslice
    // short (abort_timeslice, eat_cycles), 2 on a bus error (the access is repeated by bail) ----
    struct Slow { uint32_t v; int st; };
    static inline bool note_on = false;           // HK_573_MIPS_STATS: count the slow accesses
    static void note(uint32_t a, int size);       // (mips.cpp)
    // after a slow access: 2 on a bus error; 1 when the block must end after this instruction (a
    // device cut the timeslice short, or changed the memory map); else 0
    static HK_INLINE int after_slow(D &d, int ic, uint32_t map_version) {
        if (d.m_berr) { d.m_berr = 0; return 2; }
        return d.m_icount != ic || d.m_program->version() != map_version ? 1 : 0;
    }
    static HK_COLD Slow rd8_slow(D &d, uint32_t a, int ic) { if (note_on) note(a, 1); const uint32_t mv = d.m_program->version(); d.m_icount = ic; uint32_t v = d.m_data.read_byte(a); return {v, after_slow(d, ic, mv)}; }
    static HK_COLD Slow rd16_slow(D &d, uint32_t a, int ic) { if (note_on) note(a, 2); const uint32_t mv = d.m_program->version(); d.m_icount = ic; uint32_t v = d.m_data.read_word(a); return {v, after_slow(d, ic, mv)}; }
    static HK_COLD Slow rd32_slow(D &d, uint32_t a, int ic) { if (note_on) note(a, 4); const uint32_t mv = d.m_program->version(); d.m_icount = ic; uint32_t v = d.m_data.read_dword(a); return {v, after_slow(d, ic, mv)}; }
    static HK_COLD Slow rd32m_slow(D &d, uint32_t a, uint32_t m, int ic) { if (note_on) note(a, 3); const uint32_t mv = d.m_program->version(); d.m_icount = ic; uint32_t v = d.m_data.read_dword(a, m); return {v, after_slow(d, ic, mv)}; }
    static HK_COLD int wr32_slow(D &d, uint32_t a, uint32_t v, int ic) { if (note_on) note(a, 8); const uint32_t mv = d.m_program->version(); d.m_icount = ic; d.m_data.write_dword(a, v); return after_slow(d, ic, mv); }
    static HK_COLD int wr32m_slow(D &d, uint32_t a, uint32_t v, uint32_t m, int ic) { if (note_on) note(a, 7); const uint32_t mv = d.m_program->version(); d.m_icount = ic; d.m_data.write_dword(a, v, m); return after_slow(d, ic, mv); }

    // the scratchpad (1 KiB at 0x1f800000, on a page shared with I/O) when it is mapped as RAM in
    // both directions: else nullptr. Set by the dispatcher whenever the map changes.
    static constexpr uint32_t SP_BASE = 0x1f800000, SP_MASK = 0x3ff;
    static inline uint8_t *sp = nullptr;
    static HK_INLINE const uint8_t *rd_ptr(const page_t *pg, uint32_t a) {
        if (const uint8_t *p = pg[a >> PAGE_BITS].direct_r) [[likely]] return p + (a & PAGE_MASK);
        if ((a & ~SP_MASK) == SP_BASE && sp) return sp + (a & SP_MASK);
        return nullptr;
    }
    static HK_INLINE uint8_t *wr_ptr(const page_t *pg, uint32_t a) {
        if (uint8_t *p = pg[a >> PAGE_BITS].direct_w) [[likely]] return p + (a & PAGE_MASK);
        if ((a & ~SP_MASK) == SP_BASE && sp) return sp + (a & SP_MASK);
        return nullptr;
    }

    // MAME's readbyte/readhalf/readword/readword_masked (the bus is attached: SR.IsC is clear)
    static HK_INLINE uint32_t rd8(D &d, const page_t *pg, uint32_t a, int ic, int &st) {
        if (const uint8_t *p = rd_ptr(pg, a)) [[likely]] return *p;
        Slow s = rd8_slow(d, a, ic); st = s.st; return s.v & 0xff;
    }
    static HK_INLINE uint32_t rd16(D &d, const page_t *pg, uint32_t a, int ic, int &st) {
        if (const uint8_t *p = rd_ptr(pg, a)) [[likely]] { uint16_t v; memcpy(&v, p, 2); return v; }
        Slow s = rd16_slow(d, a, ic); st = s.st; return s.v & 0xffff;
    }
    static HK_INLINE uint32_t rd32(D &d, const page_t *pg, uint32_t a, int ic, int &st) {
        if (const uint8_t *p = rd_ptr(pg, a)) [[likely]] { uint32_t v; memcpy(&v, p, 4); return v; }
        Slow s = rd32_slow(d, a, ic); st = s.st; return s.v;
    }
    static HK_INLINE uint32_t rd32m(D &d, const page_t *pg, uint32_t a, uint32_t m, int ic, int &st) {
        if (const uint8_t *p = rd_ptr(pg, a & ~3u)) [[likely]] { uint32_t v; memcpy(&v, p, 4); return v & m; }
        Slow s = rd32m_slow(d, a, m, ic); st = s.st; return s.v;
    }
    // MAME's writeword (whole word) and writeword_masked (the lanes in m of the word at a & ~3)
    static HK_INLINE void wr32(D &d, const page_t *pg, uint32_t a, uint32_t v, int ic, int &st) {
        if (uint8_t *p = wr_ptr(pg, a)) [[likely]] { memcpy(p, &v, 4); return; }
        st = wr32_slow(d, a, v, ic);
    }
    static HK_INLINE void wr32m(D &d, const page_t *pg, uint32_t a, uint32_t v, uint32_t m, int ic, int &st) {
        if (uint8_t *w = wr_ptr(pg, a & ~3u)) [[likely]] {
            uint32_t o;
            memcpy(&o, w, 4);
            o = (o & ~m) | (v & m);
            memcpy(w, &o, 4);
            return;
        }
        st = wr32m_slow(d, a, v, m, ic);
    }

    // ---- the multiplier (MAME computes on first read of HI or LO) ----
    static HK_INLINE uint32_t hi(D &d) { if (d.m_multiplier_operation != 0) d.multiplier_update(); return d.m_hi; }
    static HK_INLINE uint32_t lo(D &d) { if (d.m_multiplier_operation != 0) d.multiplier_update(); return d.m_lo; }
    static HK_INLINE void mthi(D &d, uint32_t v) { d.m_multiplier_operation = 0; d.m_hi = v; }
    static HK_INLINE void mtlo(D &d, uint32_t v) { d.m_multiplier_operation = 0; d.m_lo = v; }
    // op: 1 mult, 2 multu, 3 div, 4 divu (MULTIPLIER_OPERATION_* in psx.cpp)
    static HK_INLINE void muldiv(D &d, int op, uint32_t a, uint32_t b) {
        d.m_multiplier_operation = op;
        d.m_multiplier_operand1 = a;
        d.m_multiplier_operand2 = b;
        d.m_lo = a;
        if (op >= 3) d.m_hi = 0;
    }

    // ---- the GTE (COP2); the pc is only for MAME's GTE log ----
    static HK_INLINE uint32_t mfc2(D &d, uint32_t pc, int reg) { return d.m_gte.getcp2dr(pc, reg); }
    static HK_INLINE uint32_t cfc2(D &d, uint32_t pc, int reg) { return d.m_gte.getcp2cr(pc, reg); }
    static HK_INLINE void mtc2(D &d, uint32_t pc, int reg, uint32_t v) { d.m_gte.setcp2dr(pc, reg, v); }
    static HK_INLINE void ctc2(D &d, uint32_t pc, int reg, uint32_t v) { d.m_gte.setcp2cr(pc, reg, v); }
    static HK_INLINE void cop2(D &d, uint32_t pc, uint32_t fun) {
        if (!d.m_gte.docop2(pc, fun)) d.m_hook_pc = pc;   // MAME's stop(): a debugger break, and the hook
    }

    // ---- for the dispatcher (mips.cpp) ----
    static HK_INLINE bool fast_mode(D &d) {
        // kernel mode, caches not isolated, no hardware breakpoints, no pending bus error
        return (d.m_cp0r[12] & ((1u << 16) | (1u << 1))) == 0 && (d.m_cp0r[7] & (1u << 23)) == 0 && !d.m_berr;
    }
    static HK_INLINE uint32_t pc(D &d) { return d.m_pc; }
    static HK_INLINE address_space &program(D &d) { return *d.m_program; }
    static HK_INLINE ram_device &ram(D &d) { return *d.m_ram; }
};
