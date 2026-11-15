// SPDX-License-Identifier: GPL-3.0-or-later
// MAME's 68000: turns of its waiting loop counted instead of run (m68000_loops.h). The counting
// is dev/loop_counter.h's; this is what it needs of MAME's core.
#include "emu.h"

#include "dev/m68000_loops.h"

#include <cstring>
#include <memory>
#include <vector>

#include "cpu/m68000/m68000.h"
#include "dev/loop_counter.h"

// (m68000_device's friend: the processor's state is its own)
struct hk_m68000 {
    using Core = m68000_device;
    using base = Core::hk_noted::base;
    static constexpr const char *name = "68000";

    static uint32_t pc(Core &c) { return c.m_ipc; }
    static int &left(Core &c) { return c.m_icount; }
    static uint32_t run(Core &c) { return c.m_hk_runs; }
    // The processor between two instructions: everything MAME keeps of it, but the cycles left in
    // the timeslice and (unless `vpa`) the time of the last autovector, which is read and set only
    // when an interrupt is taken: a counted turn takes none.
    static void snap(Core &c, std::vector<char> &to, bool vpa = false) {
        const char *b = reinterpret_cast<const char *>(&c.m_da);
        to.assign(b, reinterpret_cast<const char *>(&c.m_post_run_cycles + 1));
        auto blank = [&](const void *p, size_t n) { memset(to.data() + (reinterpret_cast<const char *>(p) - b), 0, n); };
        blank(&c.m_icount, sizeof c.m_icount);
        if (!vpa) blank(&c.m_last_vpa_time, sizeof c.m_last_vpa_time);
        // (the bytes between members hold nothing: whatever the memory held when the device was made.
        // The same in every picture of one processor, but not in two processors', which a test compares)
        auto gap = [&](const char *from, const void *to_) { blank(from, (size_t)(reinterpret_cast<const char *>(to_) - from)); };
        gap(reinterpret_cast<const char *>(&c.m_dcr + 1), &c.m_virq_state);
        gap(reinterpret_cast<const char *>(&c.m_disable_interrupt_callback + 1), &c.m_last_vpa_time);
    }
    static int plain(Core &c, uint32_t a) { return c.m_program.hk_plain(a); }
    static uint16_t word(Core &c, uint32_t a) { return c.m_program.base::read_interruptible(a, 0xffff); }
    static void put(Core &c, uint32_t a, uint16_t v) { c.m_program.base::write_interruptible(a, v, 0xffff); }
    static void noting(Core &c, bool on) { c.m_hk_noting = on; }
    static void watch(Core &c, uint32_t pc, uint32_t mask) { c.m_hk_loop_pc = pc, c.m_hk_loop_mask = mask; }

    using Counter = hk::turns::Counter<hk_m68000>;
    static Counter *of(Core &c) { return static_cast<Counter *>(c.m_hk_loops); }

    static void attach(Core &c) {
        if (!hk::turns::loops_wanted() || c.m_hk_loops) return;
        // one space for all of the program's cycles, memory seen page by page: else left alone
        if (c.m_mmu || c.m_s_opcodes != c.m_s_program || c.m_s_uprogram != c.m_s_program || c.m_s_uopcodes != c.m_s_program ||
            !c.m_r_program.m_pages)
            return;
        static std::vector<std::unique_ptr<Counter>> all;   // (they report as the program ends)
        all.push_back(std::make_unique<Counter>());
        c.m_hk_loops = all.back().get();
        c.m_hk_note = [](Core &c, offs_t a, u16 d, u16 mask, bool write, int plain) { of(c)->note(c, a, d, mask, write, plain); };
        c.m_hk_loop = [](Core &c) { of(c)->at(c); };
    }
};

namespace hk::m68000_loops {
void attach(m68000_device &cpu) { hk_m68000::attach(cpu); }
void frame(m68000_device &cpu) {
    if (auto *l = hk_m68000::of(cpu)) l->frame(cpu);
}
void state(m68000_device &cpu, std::vector<char> &to) { hk_m68000::snap(cpu, to, true); }
uint64_t counted(m68000_device &cpu) {
    auto *l = hk_m68000::of(cpu);
    return l ? l->counted() : 0;
}
}  // namespace hk::m68000_loops
