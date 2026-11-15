// SPDX-License-Identifier: GPL-3.0-or-later
// MAME's 68000 with its waiting loop's turns counted (runtime/dev/m68000_loops.h) against the
// same processor running every turn: two bare 68000s with one small program, a main loop that
// calls two routines as a sound board's does. The first looks at a flag in RAM and counts when it
// is set; the second saves registers, adds up a table and, as a mode word says, also moves a
// counter on every turn (a turn that is never the same) or reads a device (which counts its
// reads). The test sets the flag, changes the mode and the table, writes over the stack the turn
// has used and raises an interrupt between timeslices of every length, the same on both, and
// compares the processors' whole state, their RAM and the device's counts after each. No game
// needed.
#include "emu.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "cpu/m68000/m68000.h"
#include "dev/m68000_loops.h"

namespace {

constexpr uint32_t RAM = 0x100000, RAM_SIZE = 0x10000;
constexpr uint32_t FLAG = RAM, MODE = RAM + 2, SEEN = RAM + 4, MOVED = RAM + 8, INTERRUPTS = RAM + 12, TABLE = RAM + 0x100;
constexpr uint32_t DEVICE = 0x200000, ACK = 0x200002;
constexpr uint32_t CLOCK = 16000000;

class bare_68k : public device_t {
public:
    bare_68k(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock = 0)
        : device_t(mconfig, TYPE, tag, owner, clock), m_cpu(*this, "cpu") {}
    static const device_creator<bare_68k> TYPE;
    required_device<m68000_device> m_cpu;
    uint32_t reads = 0, acks = 0;

    u16 device_r() { return (u16)reads++; }
    void ack_w(u16) {
        acks++;
        m_cpu->set_input_line(M68K_IRQ_1, CLEAR_LINE);
    }
    void map(address_map &map) {
        map(0x000000, 0x07ffff).rom();
        map(RAM, RAM + RAM_SIZE - 1).ram();
        map(DEVICE, DEVICE + 1).r(&bare_68k::device_r);
        map(ACK, ACK + 1).w(&bare_68k::ack_w);
    }
    void device_add_mconfig(machine_config &config) override {
        M68000(config, m_cpu, CLOCK);
        m_cpu->set_addrmap(AS_PROGRAM, address_map_constructor(&bare_68k::map, this));
    }
    void device_start() override {}
};
const device_creator<bare_68k> bare_68k::TYPE("bare_68k", "a 68000, its ROM and RAM");

// the program, as bus words
std::vector<u8> program() {
    std::vector<u8> rom(0x80000, 0);
    auto put = [&](uint32_t at, std::initializer_list<uint16_t> words) {
        for (uint16_t w : words) memcpy(&rom[at], &w, 2), at += 2;   // (the shim holds a bus word as the host does)
    };
    auto vector = [&](int n, uint32_t to) { put((uint32_t)n * 4, {(uint16_t)(to >> 16), (uint16_t)to}); };
    for (int n = 2; n < 64; n++) vector(n, 0x7f0);
    vector(0, RAM + 0x8000);   // the stack
    vector(1, 0x400);
    vector(25, 0x700);         // the level 1 interrupt
    put(0x7f0, {0x60fe});      // anything else: stops here
    // main: move #$2000,sr; then for ever: jsr look; jsr scan
    put(0x400, {0x46fc, 0x2000, 0x4eb9, 0x0000, 0x0500, 0x4eb9, 0x0000, 0x0600, 0x60f2});
    // look: tst.w FLAG; beq out; addq.l #1,SEEN; clr.w FLAG; out: rts
    put(0x500, {0x4a79, 0x0010, 0x0000, 0x670c, 0x52b9, 0x0010, 0x0004, 0x4279, 0x0010, 0x0000, 0x4e75});
    // scan: movem.l d0-d2/a0,-(sp); lea TABLE,a0; moveq #7,d1; each: move.w (a0)+,d0; add.w d0,d2; dbra d1,each;
    //       move.w MODE,d0; cmpi.w #1,d0; bne +; addq.l #1,MOVED; +: cmpi.w #2,d0; bne +; move.w DEVICE,d0;
    //       +: movem.l (sp)+,d0-d2/a0; rts
    put(0x600, {0x48e7, 0xe080, 0x41f9, 0x0010, 0x0100, 0x7207, 0x3018, 0xd440, 0x51c9, 0xfffa,
                0x3039, 0x0010, 0x0002, 0x0c40, 0x0001, 0x6606, 0x52b9, 0x0010, 0x0008,
                0x0c40, 0x0002, 0x6606, 0x3039, 0x0020, 0x0000, 0x4cdf, 0x0107, 0x4e75});
    // the interrupt: addq.l #1,INTERRUPTS; move.w d0,ACK (the line is let go); rte
    put(0x700, {0x52b9, 0x0010, 0x000c, 0x33c0, 0x0020, 0x0002, 0x4e73});
    return rom;
}

struct Machine {
    running_machine m;
    bare_68k *dev = nullptr;
    m68000_device &cpu() { return *dev->m_cpu; }
    address_space &space() { return cpu().space(AS_PROGRAM); }
    void build() {
        m.m_exact = true;
        m.add_region(":cpu", program());
        dev = &bare_68k::TYPE(m.config(), "");
        m.start();
        m.reset();
    }
    uint32_t dword(uint32_t a) { return (uint32_t)space().read_word(a) << 16 | space().read_word(a + 2); }
};

uint64_t g_rs = 0x2545f4914f6cdd1dULL;
uint32_t rnd() {
    g_rs ^= g_rs >> 12, g_rs ^= g_rs << 25, g_rs ^= g_rs >> 27;
    return (uint32_t)((g_rs * 0x2545f4914f6cdd1dULL) >> 32);
}

std::string differ(Machine &a, Machine &b, bool all_ram) {
    std::vector<char> x, y;
    hk::m68000_loops::state(a.cpu(), x);
    hk::m68000_loops::state(b.cpu(), y);
    if (x != y) {
        size_t i = 0;
        while (x[i] == y[i]) i++;
        char at[48];
        snprintf(at, sizeof at, "%06x and %06x", (unsigned)a.cpu().hk_pc(), (unsigned)b.cpu().hk_pc());
        return "the processors differ (byte " + std::to_string(i) + " of their state), at " + at;
    }
    if (a.dev->reads != b.dev->reads) return "the device was read " + std::to_string(a.dev->reads) + " and " + std::to_string(b.dev->reads) + " times";
    if (a.dev->acks != b.dev->acks) return "the interrupt was taken " + std::to_string(a.dev->acks) + " and " + std::to_string(b.dev->acks) + " times";
    for (uint32_t at = RAM; at < RAM + RAM_SIZE; at += 2) {
        if (!all_ram && at == RAM + 0x200) at = RAM + 0x7f00;   // (most steps: the variables, the table and the stack)
        if (!all_ram && at >= RAM + 0x8000) break;
        if (a.space().read_word(at) != b.space().read_word(at)) {
            char where[32];
            snprintf(where, sizeof where, "%06x", (unsigned)at);
            return std::string("RAM differs at ") + where;
        }
    }
    return "";
}

}  // namespace

int main(int argc, char **argv) {
    const int steps = argc > 1 ? atoi(argv[1]) : 30000;
    if (const char *e = getenv("HK_68K_LOOPS"); e && !strcmp(e, "0")) {
        printf("test_m68000_loops: skip: HK_68K_LOOPS=0\n");
        return 0;
    }
    auto counted = std::make_unique<Machine>(), plain = std::make_unique<Machine>();
    counted->build();
    plain->build();
    hk::m68000_loops::attach(counted->cpu());
    uint64_t cycles = 0, next_frame = 0;
    uint32_t pokes[4] = {};
    for (int s = 0; s < steps; s++) {
        // between timeslices, what a main board or a timer does: the same to both
        const uint32_t what = rnd() % 64;
        for (Machine *mc : {counted.get(), plain.get()}) {
            if (what < 6) mc->space().write_word(FLAG, 1);
            else if (what == 6) mc->space().write_word(MODE, (u16)(s % 5 == 0 ? 1 : s % 5 == 1 ? 2 : 0));
            else if (what == 7) mc->space().write_word(TABLE + 2 * (uint32_t)(s % 8), (u16)s);
            else if (what == 8) mc->cpu().set_input_line(M68K_IRQ_1, ASSERT_LINE);
            // (a word of the stack the turn saves registers in: the next turn writes it again, and a counted
            // turn must leave it as written too. Often, because it only shows when the timeslice before
            // ended between the turn's last use of the word and the loop's head. Not the words an
            // interrupt's own frame can lie in, at any depth of the turn: the program would not come back)
            else if (what < 24) mc->space().write_word(RAM + 0x8000 - 20 + 2 * (uint32_t)(s % 5), (u16)(s * 40503u));
        }
        if (what < 9) pokes[what < 6 ? 0 : what - 5]++;
        const uint32_t k = rnd() % 16;
        cycles += k < 3 ? 1 + rnd() % 40 : k < 6 ? 100 + rnd() % 2000 : 2000 + rnd() % 30000;
        const attotime t = attotime::from_ticks(cycles, CLOCK);
        counted->m.run_until(t);
        plain->m.run_until(t);
        if (cycles >= next_frame) {
            hk::m68000_loops::frame(counted->cpu());
            next_frame = cycles + CLOCK / 60;
        }
        const std::string d = differ(*counted, *plain, s % 64 == 63 || s == steps - 1);
        if (!d.empty()) {
            printf("test_m68000_loops: FAILED: step %d, %llu cycles: %s\n", s, (unsigned long long)cycles, d.c_str());
            return 1;
        }
    }
    const uint64_t turns = hk::m68000_loops::counted(counted->cpu());
    printf("test_m68000_loops: %d timeslices, %llu cycles; the flag set %u times (seen %u), the mode changed %u, the table %u, "
           "%u interrupts (%u taken), the device read %u times; %llu turns counted\n",
           steps, (unsigned long long)cycles, pokes[0], plain->dword(SEEN), pokes[1], pokes[2], pokes[3], plain->dword(INTERRUPTS),
           plain->dev->reads, (unsigned long long)turns);
    // the program ran as meant (else two processors stopped the same way would agree too), and turns were counted
    if (plain->dword(SEEN) < pokes[0] / 2 || plain->dword(MOVED) == 0 || plain->dev->reads == 0 || plain->dword(INTERRUPTS) == 0 ||
        plain->dev->acks != plain->dword(INTERRUPTS) || plain->cpu().hk_pc() >= 0x700) {
        printf("test_m68000_loops: FAILED: the program did not do all it is there to do\n");
        return 1;
    }
    if (turns < (uint64_t)steps) {
        printf("test_m68000_loops: FAILED: hardly a turn counted\n");
        return 1;
    }
    printf("test_m68000_loops: the counted processor and the other agree\n");
    return 0;
}
