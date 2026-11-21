// SPDX-License-Identifier: GPL-3.0-or-later
// The XT446 card's H8S/2655 (Keyboardmania's synth: an MU100's firmware) made into host code as
// it runs (PLAN.md section 45). MAME's interpreter (cpu/h8) stays the reference and does
// everything made code doesn't: execute_run() offers each instruction start to run() (the
// HIRAKU edit in h8.cpp), which runs made code from there for as long as it can and returns,
// or says no and the interpreter takes the instruction.
//
// A block is the program's instructions in ROM from one address, made of the stencils
// tools/h8jitgen.py writes from MAME's own h8.lst (runtime/jit/h8_stencils.c), up to a jump or an
// instruction left to the interpreter (those that change the interrupt mask, sleep, the MAC...).
// Blocks chain to each other through a table by address without coming back here.
//
// Exact to the cycle: MAME's core stops an instruction between its bus cycles when an on-chip
// peripheral's event is due (m_bcount) or the timeslice is over, brings the peripherals up to
// date and goes on. Made code runs an instruction only where it has its most cycles in hand
// before that point, so MAME would have run it whole too; it touches only RAM and ROM (an access
// to anything else undoes the instruction and stops before it), so no peripheral changes and no
// interrupt can become due while it runs; it is entered only with none pending, no DMA or DTC
// transfer or other state asked for and no trace. What it leaves is what MAME leaves after the
// same instructions: registers, flags, m_icount, m_NPC/m_PC/m_PIR/m_IR[0]/m_inst_state, m_PPC.
// (MAME's m_TMP1/m_TMP2, written before read by every instruction and state, are not kept.)
//
// Switches: HK_H8_JIT=0 (or HK_JIT=0) interprets; HK_H8_JIT=check runs a second card, interpreted,
// alongside and compares the two after every sample (xt446.cpp); HK_H8_JIT=verify runs every
// entry into made code again by the interpreter from the same state and compares where it stopped
// (slow, for finding a fault). HK_H8_STATS=1: a line of counts at exit. For looking into it:
// HK_H8_JIT_MAX=n instructions a block at most (64), HK_H8_JIT_NOCHAIN=1 (every block back to the
// dispatcher), HK_H8_PERF_MAP=1 (/tmp/perf-<pid>.map: perf names made code by instruction).
#include "dev/xt446_h8jit.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>
#if defined(__linux__)
#include <unistd.h>
#endif

#include "emu.h"
#include "cpu/h8/h8.h"
#include "jit/jit.h"

#if defined(HK_H8_STENCILS)
extern "C" {
#include "jit/h8_layout.h"
}
#include "hk_h8_stencils.inc"
#include "hk_h8_table.inc"
#endif

struct hk_h8 {
#if defined(HK_H8_STENCILS)
    using Entry = int64_t (HK_JIT_ABI *)(char *s, int64_t ic, int64_t bc, uint32_t a, uint32_t f);

    struct Page {
        void *entry[128] = {};       // made code starting at each even address of the page
        uint8_t tries[128] = {};     // times the interpreter was here with none (255: none can be made)
        uint8_t cost[128] = {};      // the most cycles the code's first instruction takes
        uint8_t idle[128] = {};      // entries in a row that stopped before the first instruction
    };
    struct Ctx {
        hk_h8_state st{};
        std::vector<void *const *> l1;            // per 256-byte page: its Page's entries, or null
        std::vector<std::unique_ptr<Page>> pages;
        hk::jit::Arena arena{(size_t)64 << 20};
        address_space *space = nullptr;
        u32 version = 0;
        hk::jit::Code code{hk::jit::h8::SET};
        bool verify = false;                      // HK_H8_JIT=verify
        int max_block = 64;                       // instructions in a block at most (HK_H8_JIT_MAX)
        std::vector<void *const *> no_l1;         // HK_H8_JIT_NOCHAIN=1: made code finds none
        std::vector<u8 *> ram;                    // the space's writable pages (for verify)
        std::vector<void *> chain = std::vector<void *>((size_t)1 << 17);   // hk_h8_state::chain (then jumps look up by address)
        size_t next_slot = 0;
        int pending_slot = -1;                    // a jump's slot to fill once there is code at pending_npc
        u32 pending_npc = 0;
        FILE *perf_map = nullptr;                 // HK_H8_PERF_MAP=1: /tmp/perf-<pid>.map, for perf to name made code
        std::vector<const u8 *> rd;               // per page: its memory to read, or null (hk_h8_state::rd)
        std::vector<u8 *> wr;                     // ... to write
        // statistics
        uint64_t entries = 0, made_cycles = 0, blocks = 0, refused = 0, stops_before = 0, stops_jump = 0, nothing = 0;
    };

    static Ctx &ctx(h8_device &d) { return *static_cast<Ctx *>(d.m_hk_ctx); }

    // the word at byte address a when its page is ROM (plain memory to read, not written)
    static bool rom_word(const address_space &sp, u32 a, u16 &w) {
        const address_space::page_t &p = sp.pages()[(a & 0xffffff) >> HK_H8_PAGE_BITS];
        if (!p.direct_r || p.direct_w) return false;
        std::memcpy(&w, p.direct_r + (a & 0xfe), 2);
        return true;
    }

    static Page &page(Ctx &c, u32 npc) {
        const u32 pi = (npc & 0xffffff) >> HK_H8_PAGE_BITS;
        if (!c.pages[pi]) {
            c.pages[pi] = std::make_unique<Page>();
            c.l1[pi] = c.pages[pi]->entry;
        }
        return *c.pages[pi];
    }

    // A block from npc, or null where its first instruction can't be made. Its instructions run
    // on through conditional branches not taken (a taken one leaves: to an instruction of the
    // same block, or through the table by address) and end at an instruction the interpreter
    // must do, a jump, or MAX instructions.
    static void *make(Ctx &c, u32 npc, uint8_t &first_cost) {
        struct Ins { u32 addr; int op; uint16_t w[6]; u32 target; int to; bool known; };
        std::vector<Ins> ins;
        u32 addr = npc;
        bool before = false, jumped = false;
        uint16_t before_w0 = 0;
        while ((int)ins.size() < c.max_block) {
            Ins i{addr, -1, {}, 0, -1, false};
            bool rom = true;
            for (int k = 0; k < 6 && rom; k++) rom = rom_word(*c.space, addr + 2 * k, i.w[k]);   // (the words and the next one's: ROM)
            if (!rom) break;
            i.op = hk_h8_decode(i.w);
            if (i.op < 0 || hk_h8_ops[i.op].stencil < 0 || hk_h8_ops[i.op].words > 5) { before = true; before_w0 = i.w[0]; break; }
            const hk_h8_op &o = hk_h8_ops[i.op];
            const bool rel8 = o.branch && std::strstr(o.name, "_rel8"), rel16 = o.branch && std::strstr(o.name, "_rel16"),
                       abs24 = o.branch && std::strstr(o.name, "_abs24e");
            if (rel8) i.target = i.addr + 2 + (u32)(s32)(s8)(i.w[0] & 0xff);
            if (rel16) i.target = i.addr + 4 + (u32)(s32)(s16)i.w[1];
            if (abs24) i.target = ((u32)(i.w[0] & 0xff) << 16) | i.w[1];
            i.known = rel8 || rel16 || abs24;
            // (the made code reads a known target's word without a test: it must be memory)
            if (i.known && !c.rd[(i.target & 0xffffff) >> HK_H8_PAGE_BITS]) { before = true; before_w0 = i.w[0]; break; }
            ins.push_back(i);
            addr += 2 * o.words;
            if (o.branch) {
                // a b<cc> (rel8 or rel16) may go on; bt (always), the calls, jumps and returns end the block
                Ins &b = ins.back();
                const bool cond = (rel8 || rel16) && o.name[0] == 'b' && std::strncmp(o.name, "bt_", 3) && std::strncmp(o.name, "bsr", 3);
                if (!cond) { jumped = true; break; }
            }
        }
        if (ins.empty()) return nullptr;
        first_cost = hk_h8_ops[ins[0].op].cost;
        const size_t n = ins.size();
        std::vector<bool> target(n, false);
        for (Ins &b : ins)
            if (b.known && b.target <= 0xffffff && !std::strncmp(hk_h8_ops[b.op].name, "b", 1) && std::strncmp(hk_h8_ops[b.op].name, "bsr", 3))
                for (size_t j = 0; j < n; j++)
                    if (ins[j].addr == (b.target & 0xffffff)) { b.to = (int)j; target[j] = true; }
        namespace st = hk::jit::h8;
        hk::jit::Code &code = c.code;
        code.clear();
        std::vector<std::pair<size_t, const char *>> marks;   // (for the perf map)
        std::vector<int> start(n), exit(n), taken(n, -1);
        for (size_t i = 0; i < n; i++) {
            start[i] = code.label();
            exit[i] = code.label();
            if (hk_h8_ops[ins[i].op].branch) taken[i] = code.label();
        }
        for (size_t i = 0; i < n; i++) {
            const Ins &x = ins[i];
            const hk_h8_op &o = hk_h8_ops[x.op];
            if (target[i] && i) code.put(st::ST_x_ppc, {(int64_t)ins[i - 1].addr}, {});
            code.bind(start[i]);
            marks.emplace_back(code.size(), o.name);
            int64_t h[12] = {x.w[0], x.w[1], x.w[2], x.w[3], x.w[4], (int64_t)(x.addr + 2)};
            if (o.regs) o.regs(x.w, h);
            const std::initializer_list<int64_t> holes = {h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7], h[8], h[9], h[10], h[11]};
            if (o.branch) code.put(o.stencil, holes, {exit[i], taken[i]});
            else code.put(o.stencil, holes, {exit[i]});
        }
        const u32 last = ins[n - 1].addr;
        marks.emplace_back(code.size(), "exits");
        // a jump to a known place (without bits above the address): through a slot of the chain table
        auto leave = [&](const Ins &x) {
            if (x.known && x.target <= 0xffffff && c.next_slot < c.chain.size())
                code.put(st::ST_x_chain, {(int64_t)x.addr, (int64_t)c.next_slot++}, {});
            else
                code.put(st::ST_x_jump, {(int64_t)x.addr}, {});
        };
        if (jumped) leave(ins[n - 1]);
        else if (before) code.put(st::ST_x_before, {(int64_t)addr, before_w0, (int64_t)last}, {});
        else code.put(st::ST_x_next, {(int64_t)last, (int64_t)addr}, {});
        for (size_t i = 0; i < n; i++)
            if (taken[i] >= 0) {
                code.bind(taken[i]);
                if (ins[i].to >= 0) code.put(st::ST_x_loop, {(int64_t)ins[i].addr}, {start[(size_t)ins[i].to]});
                else leave(ins[i]);
            }
        for (size_t i = 0; i < n; i++) {
            code.bind(exit[i]);
            if (i == 0 || target[i]) code.put(st::ST_x_before_first, {(int64_t)ins[i].addr, ins[i].w[0]}, {});
            else code.put(st::ST_x_before, {(int64_t)ins[i].addr, ins[i].w[0], (int64_t)ins[i - 1].addr}, {});
        }
        if (!code.ok()) return nullptr;
        void *m = c.arena.add(code.bytes());
        if (m) c.blocks++;
        if (m && c.perf_map) {
            marks.emplace_back(code.size(), nullptr);
            for (size_t k = 0; k + 1 < marks.size(); k++)
                std::fprintf(c.perf_map, "%llx %llx h8:%s\n", (unsigned long long)((uintptr_t)m + marks[k].first),
                             (unsigned long long)(marks[k + 1].first - marks[k].first), marks[k].second);
            std::fflush(c.perf_map);
        }
        return m;
    }

    // HK_H8_JIT=verify: every entry into made code run again by the interpreter from the same
    // state, the two compared where the made code stopped (registers, flags, where it is, the
    // cycles, RAM); the program stops at the first difference. Slow: for finding a fault.
    struct Snap {
        u16 R[16]; u8 CCR, EXR; int icount; u32 NPC, PC, PPC; u16 PIR, IR0; int inst_state;
        std::vector<u8> ram;
        void take(h8_device &d, Ctx &c) {
            std::memcpy(R, d.m_R, 32); CCR = d.m_CCR; EXR = d.m_EXR; icount = d.m_icount; NPC = d.m_NPC; PC = d.m_PC;
            PPC = d.m_PPC; PIR = d.m_PIR; IR0 = d.m_IR[0]; inst_state = d.m_inst_state;
            ram.resize(c.ram.size() << HK_H8_PAGE_BITS);
            for (size_t i = 0; i < c.ram.size(); i++) std::memcpy(&ram[i << HK_H8_PAGE_BITS], c.ram[i], 1 << HK_H8_PAGE_BITS);
        }
        void put(h8_device &d, Ctx &c) const {
            std::memcpy(d.m_R, R, 32); d.m_CCR = CCR; d.m_EXR = EXR; d.m_icount = icount; d.m_NPC = NPC; d.m_PC = PC;
            d.m_PPC = PPC; d.m_PIR = PIR; d.m_IR[0] = IR0; d.m_inst_state = inst_state;
            for (size_t i = 0; i < c.ram.size(); i++) std::memcpy(c.ram[i], &ram[i << HK_H8_PAGE_BITS], 1 << HK_H8_PAGE_BITS);
        }
        std::string diff(const Snap &o) const {
            char m[160];
            for (int i = 0; i < 16; i++)
                if (R[i] != o.R[i]) { std::snprintf(m, sizeof m, "R[%d] %04x, interpreter %04x", i, R[i], o.R[i]); return m; }
#define HK_D(f) if (f != o.f) { std::snprintf(m, sizeof m, #f " %x, interpreter %x", (unsigned)f, (unsigned)o.f); return m; }
            HK_D(CCR) HK_D(EXR) HK_D(icount) HK_D(NPC) HK_D(PC) HK_D(PPC) HK_D(PIR) HK_D(IR0) HK_D(inst_state)
#undef HK_D
            for (size_t i = 0; i < ram.size(); i++)
                if (ram[i] != o.ram[i]) { std::snprintf(m, sizeof m, "RAM page %zu byte %zu", i >> HK_H8_PAGE_BITS, i & 255); return m; }
            return "";
        }
    };
    HK_JIT_ENTERS static bool run_verified(h8_device &d) {
        Ctx &c = ctx(d);
        Snap before, made, interp;
        before.take(d, c);
        if (!run_made(d)) return false;
        made.take(d, c);
        before.put(d, c);
        while (d.m_icount > made.icount) {
            if (d.m_inst_state < 0x10000) d.m_PPC = d.m_NPC;
            d.do_exec_full();
        }
        interp.take(d, c);
        if (std::string e = made.diff(interp); !e.empty()) {
            std::fprintf(stderr, "h8 jit: HK_H8_JIT=verify: made code from %06x differs: %s\n", before.NPC, e.c_str());
            uint16_t w[6] = {};
            for (int k = 0; k < 6; k++) rom_word(*c.space, before.NPC + 2 * k, w[k]);
            const int op = hk_h8_decode(w);
            std::fprintf(stderr, "  first: %04x %04x %04x: %s (cost %d)\n", w[0], w[1], w[2], op >= 0 ? hk_h8_ops[op].name : "?", op >= 0 ? hk_h8_ops[op].cost : -1);
            for (const Snap *x : {&before, &made, &interp}) {
                std::fprintf(stderr, "  icount %d NPC %06x PC %06x PPC %06x CCR %02x ER:", x->icount, x->NPC, x->PC, x->PPC, x->CCR);
                for (int i = 0; i < 8; i++) std::fprintf(stderr, " %04x%04x", x->R[i + 8], x->R[i]);
                std::fprintf(stderr, "\n");
            }
            std::abort();
        }
        return true;
    }

    HK_JIT_ENTERS static bool run(h8_device &d) { return ctx(d).verify ? run_verified(d) : run_made(d); }

    HK_JIT_ENTERS static bool run_made(h8_device &d) {
        Ctx &c = ctx(d);
        if (d.m_requested_state != -1 || d.m_current_dma != -1 || d.m_current_dtc || d.m_irq_vector) return false;
        if (d.m_has_trace && (d.m_EXR & h8_device::EXR_T)) return false;
        const u32 npc = d.m_NPC;
        if (d.m_PC != npc + 2 || (u16)d.m_inst_state != d.m_PIR || d.m_inst_substate) return false;
        if (c.space->version() != c.version) {   // the map changed: nothing made is right now
            d.m_hk_run = nullptr;
            return false;
        }
        Page &p = page(c, npc);
        const u32 slot = (npc & 0xff) >> 1;
        void *e = p.entry[slot];
        if (!e) {
            uint8_t &t = p.tries[slot];
            if (t == 255 || ++t < 2) return false;
            e = make(c, npc, p.cost[slot]);
            if (!e) { t = 255; c.refused++; return false; }
            p.entry[slot] = e;
        }
        if (c.pending_slot >= 0) {
            if (c.pending_npc == npc) c.chain[(size_t)c.pending_slot] = e;
            c.pending_slot = -1;
        }
        const int ic0 = d.m_icount;
        if (ic0 - (int)p.cost[slot] <= d.m_bcount) return false;   // (it would stop at once: too few cycles left)
        hk_h8_state &st = c.st;
        std::memcpy(st.R, d.m_R, sizeof st.R);
        st.EXR = d.m_EXR;
        st.ppc = d.m_PPC;
        st.slot = -1;
        const int64_t r = reinterpret_cast<Entry>(e)(reinterpret_cast<char *>(&st), ic0, d.m_bcount, 0, d.m_CCR);
        c.entries++;
        if (st.ic == ic0) {   // stopped before its first instruction, nothing changed: an access that isn't memory
            c.nothing++;
            if (++p.idle[slot] >= 3) {   // (again and again: that instruction is left to the interpreter)
                p.entry[slot] = nullptr;
                p.tries[slot] = 255;
            }
            return false;
        }
        p.idle[slot] = 0;
        c.made_cycles += (uint64_t)(ic0 - st.ic);
        std::memcpy(d.m_R, st.R, sizeof st.R);
        d.m_CCR = st.CCR;
        d.m_icount = st.ic;
        d.m_PPC = st.ppc;
        if (r == HK_H8_STOP_BEFORE) {
            c.stops_before++;
            d.m_NPC = st.npc;
            d.m_PC = st.npc + 2;
            d.m_PIR = d.m_IR[0] = (u16)st.ir0;
        } else {
            c.stops_jump++;
            d.m_PC = st.pc;
            d.m_NPC = (st.pc - 2) & 0xffffff;
            if (st.slot >= 0) {
                c.pending_slot = st.slot;
                c.pending_npc = d.m_NPC;
            }
            d.m_PIR = d.m_IR[0] = d.m_cache.read_word(d.m_NPC & ~1u);   // MAME's read16i(m_PC) (memory: checked)
        }
        d.m_inst_state = d.m_PIR;
        return true;
    }
#endif

    static bool attach(h8_device &d) {
#if defined(HK_H8_STENCILS)
        if (!hk::jit::wanted()) return false;
        if (const char *e = std::getenv("HK_H8_JIT"); e && !std::strcmp(e, "0")) return false;
        // what the stencils take for granted: an H8S (EXR, one-cycle accesses, the multiplier),
        // advanced mode, a 24-bit space of 256-byte pages with 16-bit words, no taps
        address_space &sp = d.space(AS_PROGRAM);
        static_assert(sizeof(address_space::page_t) == sizeof(hk_h8_page));
        static_assert(offsetof(address_space::page_t, direct_r) == offsetof(hk_h8_page, direct_r));
        static_assert(offsetof(address_space::page_t, direct_w) == offsetof(hk_h8_page, direct_w));
        if (!d.m_has_exr || !d.m_mode_advanced || !d.m_has_hc || !d.m_has_mac) return false;
        if (sp.page_bits() != HK_H8_PAGE_BITS || sp.bus_bytes() != 2 || sp.addr_shift() != 0 || sp.byte_mask() != 0xffffff) return false;
        if (sp.m_read_tap || sp.m_write_tap) return false;
        auto *c = new Ctx;
        c->space = &sp;
        c->version = sp.version();
        c->l1.assign((size_t)1 << (24 - HK_H8_PAGE_BITS), nullptr);
        c->pages.resize(c->l1.size());
        c->st.pages = reinterpret_cast<const hk_h8_page *>(sp.pages());
        c->rd.resize(c->l1.size());
        c->wr.resize(c->l1.size());
        for (size_t i = 0; i < c->l1.size(); i++) {
            c->rd[i] = sp.pages()[i].direct_r;
            c->wr[i] = sp.pages()[i].direct_w;
        }
        c->st.rd = c->rd.data();
        c->st.chain = c->chain.data();
        c->st.wr = c->wr.data();
        c->st.l1 = c->l1.data();
        if (const char *e = std::getenv("HK_H8_JIT_MAX")) c->max_block = std::max(1, std::atoi(e));
#if defined(__linux__)
        if (std::getenv("HK_H8_PERF_MAP")) c->perf_map = std::fopen(("/tmp/perf-" + std::to_string(getpid()) + ".map").c_str(), "a");
#endif
        if (std::getenv("HK_H8_JIT_NOCHAIN")) {
            c->no_l1.assign(c->l1.size(), nullptr);
            c->st.l1 = c->no_l1.data();
        }
        if (const char *e = std::getenv("HK_H8_JIT"); e && !std::strcmp(e, "verify")) {
            c->verify = true;
            for (size_t i = 0; i < c->l1.size(); i++)
                if (sp.pages()[i].direct_w) c->ram.push_back(sp.pages()[i].direct_w);
        }
        d.m_hk_ctx = c;
        d.m_hk_run = &run;
        return true;
#else
        (void)d;
        return false;
#endif
    }

    static void detach(h8_device &d) {
#if defined(HK_H8_STENCILS)
        if (!d.m_hk_ctx) return;
        // (called from outside execute_run(): no made code is running)
        d.m_hk_run = nullptr;
        delete static_cast<Ctx *>(d.m_hk_ctx);
        d.m_hk_ctx = nullptr;
#else
        (void)d;
#endif
    }

    static uint64_t made_cycles(h8_device &d) {
#if defined(HK_H8_STENCILS)
        return d.m_hk_ctx ? ctx(d).made_cycles : 0;
#else
        (void)d;
        return 0;
#endif
    }

    static std::string stats(h8_device &d) {
#if defined(HK_H8_STENCILS)
        if (!d.m_hk_ctx) return "";
        Ctx &c = ctx(d);
        char b[400];
        std::snprintf(b, sizeof b,
                      "h8 jit: %.1f%% of %llu cycles in made code, %llu blocks (%.1f MiB), %llu entries "
                      "(%llu stopped before an instruction, %llu after a jump, %llu ran nothing), %llu places refused, %zu jump slots",
                      d.total_cycles() ? 100.0 * (double)c.made_cycles / (double)d.total_cycles() : 0.0,
                      (unsigned long long)d.total_cycles(), (unsigned long long)c.blocks, (double)c.arena.used() / 1048576.0,
                      (unsigned long long)c.entries, (unsigned long long)c.stops_before, (unsigned long long)c.stops_jump,
                      (unsigned long long)c.nothing, (unsigned long long)c.refused, c.next_slot);
        return b;
#else
        (void)d;
        return "";
#endif
    }

    // ---- a made-up program for tests/jit/test_h8_jit.cpp (no game code) ----
    struct Rom {
        std::vector<u8> bytes = std::vector<u8>(0x200000, 0);
        u32 at = 0;
        void word(u16 w) { std::memcpy(&bytes[at], &w, 2); at += 2; }   // (the region holds host-order bus words)
        void words(std::initializer_list<u16> ws) { for (u16 w : ws) word(w); }
        void vec(int v, u32 target) { std::memcpy(&bytes[4 * v], (const u16[]){(u16)(target >> 16), (u16)target}, 4); }
    };
    struct Rng {
        uint64_t x;
        u32 operator()() { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return (u32)(x >> 16); }
        u32 below(u32 n) { return (*this)() % n; }
    };
    // a translated instruction that isn't a jump, its operands random but never ER7 (the stack:
    // interrupts arrive at any time), its addresses mostly RAM; false: none drawn
    static bool random_instruction(Rng &r, std::vector<u16> &out) {
#if defined(HK_H8_STENCILS)
        constexpr int N = (int)(sizeof hk_h8_ops / sizeof hk_h8_ops[0]);
        for (int tries = 0; tries < 1000; tries++) {
            const int op = (int)r.below(N);
            const hk_h8_op &o = hk_h8_ops[op];
            if (!o.name || o.stencil < 0 || o.branch) continue;
            if (!std::strncmp(o.name, "ldm", 3) || !std::strncmp(o.name, "stm", 3)) continue;   // the stack
            u16 w[6] = {};
            int k = 0;
            for (int v = 0; v < o.nval; v++) {
                if (v == 1 && o.skip) w[k++] = (u16)r();
                u16 x = (u16)((o.val[v] & o.mask[v]) | (r() & ~o.mask[v]));
                for (int sh = 0; sh < 16; sh += 4) {   // a register 7 in an operand's nibble: 6
                    const u16 nm = (u16)(0xf << sh), free = (u16)(~o.mask[v] & nm);
                    if (free & (1 << sh) && ((x >> sh) & 7) == 7) x = (u16)(x & ~(1 << sh));
                }
                w[k++] = x;
            }
            const bool abs32 = std::strstr(o.name, "abs32") || std::strstr(o.name, "abs24"), abs16 = std::strstr(o.name, "abs16"),
                       abs8 = std::strstr(o.name, "abs8");
            static const u8 safe_io[] = {0xd6, 0xd7, 0xd8, 0xd9, 0xd5, 0x60, 0x61, 0x84, 0x85, 0xc1};
            for (; k < o.words; k++) {
                u16 x = (u16)r();
                if (abs32 && k + 1 < o.words) {
                    const u32 a = r.below(10) < 7 ? 0x200000 + r.below(0x20000) : r.below(2) ? 0xffec00 + r.below(0x1000) : 0x300000;
                    w[k] = (u16)(a >> 16);
                    k++;
                    x = (u16)a;
                } else if (abs16) {
                    const u32 c = r.below(10);
                    x = c < 6 ? (u16)(0xec00 + r.below(0x1000)) : c < 8 ? (u16)(0xff00 | safe_io[r.below(sizeof safe_io)]) : (u16)(0x8000 + r.below(0x6000));
                }
                w[k] = x;
            }
            if (abs8)   // an @aa:8 operand (the first word's low byte): an I/O register safe to change
                w[0] = (u16)((w[0] & 0xff00) | safe_io[r.below(sizeof safe_io)]);
            if (hk_h8_decode(w) != op) continue;
            out.insert(out.end(), w, w + o.words);
            return true;
        }
#else
        (void)r; (void)out;
#endif
        return false;
    }

    static std::vector<u8> test_rom(unsigned seed) {
        Rng r{0x9e3779b97f4a7c15ull ^ (uint64_t)seed * 0x2545f4914f6cdd1dull};
        r();
        Rom m;
        for (int v = 0; v < 128; v++) m.vec(v, 0x800);
        m.vec(0, 0x1000);
        m.vec(32, 0x820);              // TPU 0's compare match A
        m.vec(84, 0x840);              // SCI 1's receive error ...
        m.vec(85, 0x840);              // ... and data
        m.at = 0x800; m.word(0x5670);  // rte
        m.at = 0x820; m.words({0x7fd5, 0x7200, 0x5670});   // bclr #0, @TSR0; rte
        m.at = 0x840; m.words({0x2885, 0x7f84, 0x7260, 0x7f84, 0x7250, 0x7f84, 0x7240, 0x7f84, 0x7230, 0x5670});
        // the start: the stack, TPU 0 counting to a random period and interrupting, SCI 1 receiving at 31250 baud
        m.at = 0x1000;
        m.words({0x7a07, 0x0021, 0xf000});
        m.words({0xf820, 0x38d0});                                   // TCR0: cleared at compare match A
        m.words({0x7900, (u16)(200 + r.below(3000)), 0x6b80, 0xffd8});   // TGRA0
        m.words({0xf801, 0x38d4, 0xf801, 0x38c0});                   // TIER0, TSTR
        m.words({0xf80f, 0x3881, 0xf850, 0x3882});                   // BRR1, SCR1
        m.words({0x067f, 0x5a00, 0x1100});                           // andc #0x7f, ccr; jmp @0x1100
        // a subroutine of random instructions at 0x8000
        std::vector<u16> sub;
        for (int i = 0, n = 2 + (int)r.below(12); i < n; i++) random_instruction(r, sub);
        m.at = 0x8000;
        for (u16 w : sub) m.word(w);
        m.word(0x5470);   // rts
        // the main loop: registers to RAM, random instructions, forward branches, calls
        m.at = 0x1100;
        for (int n = 0; n < 7; n++) {
            u32 a = 0x200000 + (r.below(0x1e000) & ~1u);
            // now and then just below where RAM ends: a long's second word, or a byte past it, isn't memory
            if (r.below(8) == 0) a = (r.below(2) ? 0x220000u : 0xfffc00u) - 2 * (1 + r.below(3)) + (r.below(3) ? 0 : 8);
            m.words({(u16)(0x7a00 | n), (u16)(a >> 16), (u16)a});
        }
        m.words({0x7a07, 0x0021, 0xf000});
        const int count = 20 + (int)r.below(200);
        enum { PLAIN, BRANCH, CALL, LOCAL };   // (LOCAL: bra over a short subroutine, which ends in rts)
        std::vector<std::pair<int, std::vector<u16>>> ins;
        for (int i = 0; i < count; i++) {
            std::vector<u16> w;
            const u32 c = r.below(100);
            if (c < 8) ins.push_back({BRANCH, {0, 0}});      // b<cc>, made below
            else if (c < 11) ins.push_back({CALL, {0, 0}});  // bsr
            else if (c < 13) {
                w = {0};
                for (int k = 0, n = 1 + (int)r.below(3); k < n; k++) random_instruction(r, w);
                w.push_back(0x5470);
                w[0] = (u16)(0x4000 | (2 * (w.size() - 1)));   // bra to after its rts
                ins.push_back({LOCAL, w});
            } else if (random_instruction(r, w)) ins.push_back({PLAIN, w});
        }
        std::vector<u32> addr(ins.size() + 1);
        u32 at = m.at;
        for (size_t i = 0; i < ins.size(); i++) { addr[i] = at; at += 2 * (u32)ins[i].second.size(); }
        addr[ins.size()] = at;
        u32 local = 0;   // the last short subroutine's address
        for (size_t i = 0; i < ins.size(); i++) {
            std::vector<u16> &w = ins[i].second;
            if (ins[i].first == BRANCH) {   // b<cc> forward over a few instructions (rel8), then a nop
                const size_t t = std::min(ins.size(), i + 1 + r.below(5));
                const s32 d = (s32)addr[t] - (s32)(addr[i] + 2);
                w = {(u16)(0x4000 | (r.below(16) << 8) | (u8)d), 0x0000};
            } else if (ins[i].first == CALL) {   // bsr (8 bits) to the last short one, else (16 bits) the long one
                const s32 d = (s32)local - (s32)(addr[i] + 2);
                if (local && d >= -128) w = {(u16)(0x5500 | (u8)d), 0x0000};
                else if (r.below(2)) w = {0x5c00, (u16)(0x8000 - (addr[i] + 4))};
                else w = {0x5e00, 0x8000};   // (jsr @0x8000:24)
            } else if (ins[i].first == LOCAL)
                local = addr[i] + 2;
            for (u16 x : w) m.word(x);
        }
        m.words({0x5a00, 0x1100});   // jmp @0x1100
        return m.bytes;
    }

    static std::string compare(h8_device &a, h8_device &b) {
        char m[200];
        for (int i = 0; i < 16; i++)
            if (a.m_R[i] != b.m_R[i]) { std::snprintf(m, sizeof m, "R[%d] %04x %04x", i, a.m_R[i], b.m_R[i]); return m; }
#define HK_CMP(f) if (a.f != b.f) { std::snprintf(m, sizeof m, #f " %llx %llx", (unsigned long long)a.f, (unsigned long long)b.f); return m; }
        HK_CMP(m_CCR) HK_CMP(m_EXR) HK_CMP(m_PC) HK_CMP(m_NPC) HK_CMP(m_PPC) HK_CMP(m_PIR) HK_CMP(m_IR[0])
        HK_CMP(m_inst_state) HK_CMP(m_inst_substate) HK_CMP(m_icount) HK_CMP(m_bcount) HK_CMP(m_irq_vector)
        HK_CMP(m_count_before_instruction_step) HK_CMP(total_cycles())
#undef HK_CMP
        return "";
    }
};

namespace hk::h8jit {
bool attach(h8_device &cpu) { return hk_h8::attach(cpu); }
void detach(h8_device &cpu) { hk_h8::detach(cpu); }
std::string stats(h8_device &cpu) { return hk_h8::stats(cpu); }
std::string compare(h8_device &a, h8_device &b) { return hk_h8::compare(a, b); }
std::vector<uint8_t> test_program(unsigned seed) { return hk_h8::test_rom(seed); }
uint64_t made_cycles(h8_device &cpu) { return hk_h8::made_cycles(cpu); }
}  // namespace hk::h8jit
