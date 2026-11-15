// SPDX-License-Identifier: GPL-3.0-or-later
// Firebeat SPU board. Memory map, interrupt wiring, the 500 Hz IRQ2/IRQ6 timer
// and the ATA DMA engine follow MAME's firebeat.cpp (license BSD-3-Clause,
// copyright-holders: Ville Linde); the CPU core is Musashi (MIT licence).
#include "spu.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <vector>

#include "../core/cpu403.h"
#include "../core/prof.h"
#include "../core/runtime.h"
#include "../core/sched.h"
#include "../dev/ata.h"
#include "../dev/rf5c400.h"
#include "../firebeat/board.h"
#include "../firebeat/spectrum.h"

extern "C" {
#include "m68k.h"
}

namespace hk::spu {

namespace {

constexpr double SPU_HZ = 16'000'000.0;          // MC68HC000 at 16 MHz

Config g_cfg;
bool g_present = false;
std::vector<uint8_t> g_rom;                      // big-endian, as the 68000 sees it
std::vector<uint8_t> g_ram(0x40000);
std::vector<uint16_t> g_wave(0x1000000);         // 32 MiB wave RAM, 16-bit words
uint8_t g_dpram[0x400];
uint32_t g_wave_bank = 0;                        // in words
uint32_t g_dma_addr = 0;                         // in words, relative to the bank
bool g_dmarq = false;
bool g_sync_ata_irq = false;
std::unique_ptr<AtaBus> g_ata;
std::unique_ptr<Rf5c400> g_rf;
Stats g_stats;

// interrupt lines into the 68000 (levels 1, 2, 4, 6)
uint8_t g_lines = 0;
void update_irq() {
    int level = 0;
    for (int l = 7; l >= 1; l--)
        if (g_lines & (1 << l)) { level = l; break; }
    m68k_set_irq(level);
}
void set_line(int level, bool on) {
    uint8_t old = g_lines;
    if (on) g_lines |= (uint8_t)(1 << level);
    else g_lines &= (uint8_t)~(1 << level);
    if (g_lines != old) update_irq();
}

// ---- time ------------------------------------------------------------------------
bool g_lat_trace = getenv("HK_LAT_TRACE") != nullptr;   // HK_LAT_TRACE=1: key press -> key-on timeline
uint64_t g_cycles = 0;                           // 68000 cycles executed so far
bool g_running = false;                          // inside m68k_execute

uint64_t cycles_now() { return g_cycles + (g_running ? (uint64_t)m68k_cycles_run() : 0); }

// ---- audio -----------------------------------------------------------------------
uint64_t g_samples = 0;                          // RF5C400 samples rendered so far
std::function<void(const float *, int)> g_sink;
std::vector<float> g_buf4, g_buf2;

void render_until(uint64_t cycles) {
    uint64_t target = (uint64_t)((double)cycles * g_rf->sample_rate() / SPU_HZ);
    while (g_samples < target) {
        int n = (int)std::min<uint64_t>(target - g_samples, 1024);
        g_buf4.resize((size_t)n * 4);
        g_buf2.resize((size_t)n * 2);
        g_rf->render(g_buf4.data(), n);
        for (int i = 0; i < n; i++) {
            float l = g_buf4[4 * i], r = g_buf4[4 * i + 1];
            if (g_cfg.mix_effect) { l += g_buf4[4 * i + 2]; r += g_buf4[4 * i + 3]; }
            g_buf2[2 * i] = 0.5f * l;    // MAME routes each output at 0.5
            g_buf2[2 * i + 1] = 0.5f * r;
        }
        if (g_cfg.mix_effect) spectrum::push(g_buf2.data(), n, (int)g_rf->sample_rate());   // beatmania III extend board
        if (g_sink) g_sink(g_buf2.data(), n);
        g_samples += (uint64_t)n;
    }
}

// ---- the 500 Hz command tick (MAME: "500 hz seems ok for beatmania III") -----------
void tick_fire();
Timer g_tick("spu-tick", tick_fire);
void tick_fire() {
    catch_up();
    if (g_sync_ata_irq) { set_line(6, true); g_stats.irq6++; }
    else { set_line(2, true); g_stats.irq2++; }
    g_tick.again(ticks_from_hz(500));
}

// ---- ATA DMA into wave RAM: one word every 350 ns while DMARQ is up, as in MAME ------
void dma_fire();
Timer g_dma("spu-dma", dma_fire);
constexpr int DMA_BURST = 64;
void dma_fire() {
    catch_up();
    for (int k = 0; k < DMA_BURST && g_dmarq; k++) {
        uint16_t w = g_ata->read_dma();
        g_wave[(g_wave_bank + g_dma_addr) & 0xffffff] = w;
        g_dma_addr++;
        g_stats.dma_words++;
    }
    if (g_dmarq) g_dma.again(ticks_from_us(0.35 * DMA_BURST));
}

// ---- 68000 address space ------------------------------------------------------------
bool g_trace_io = getenv("HK_SPU_IO") != nullptr;
int g_trace_n = 0;
void trace_io(const char *rw, uint32_t a, uint32_t v) {
    if (g_trace_io && g_trace_n++ < 400)
        fprintf(stderr, "spu %s %06x = %04x  (68k pc %06x)\n", rw, a, v, m68k_get_reg(nullptr, M68K_REG_PPC));
}

uint16_t read16(uint32_t a) {
    a &= 0xfffffe;
    if (a < 0x80000) return (uint16_t)(g_rom[a] << 8 | g_rom[a + 1]);
    if (a >= 0x100000 && a < 0x140000) { a -= 0x100000; return (uint16_t)(g_ram[a] << 8 | g_ram[a + 1]); }
    if (a >= 0x800000) return g_wave[(((a - 0x800000) >> 1) + g_wave_bank) & 0xffffff];
    if (a >= 0x200000 && a < 0x200002) return 0x00ff;            // SPU DIP switches, all off (active low)
    if (a >= 0x280000 && a < 0x280800) {                         // dual-port RAM, left side, low byte lane
        uint32_t o = (a - 0x280000) >> 1;
        if (o == 0x3fe) set_line(4, false);                      // left read of 0x3fe acknowledges INTL
        return g_dpram[o];
    }
    if (a >= 0x300000 && a < 0x300010) { uint16_t v = g_ata->read(0, (a & 0xf) >> 1); if ((a & 0xf) != 0) trace_io("R", a, v); return v; }
    if (a >= 0x340000 && a < 0x340010) { uint16_t v = g_ata->read(1, (a & 0xf) >> 1); trace_io("R", a, v); return v; }
    if (a >= 0x400000 && a < 0x401000) return g_rf->read((a - 0x400000) >> 1);
    return 0;
}

void write16(uint32_t a, uint16_t v, uint16_t mask) {
    a &= 0xfffffe;
    if (a >= 0x100000 && a < 0x140000) {
        a -= 0x100000;
        if (mask & 0xff00) g_ram[a] = (uint8_t)(v >> 8);
        if (mask & 0x00ff) g_ram[a + 1] = (uint8_t)v;
        return;
    }
    if (a >= 0x800000) {
        uint16_t &w = g_wave[(((a - 0x800000) >> 1) + g_wave_bank) & 0xffffff];
        w = (uint16_t)((w & ~mask) | (v & mask));
        return;
    }
    switch (a & 0xff0000) {
    case 0x220000: return;                                       // status LEDs
    case 0x230000:                                               // interrupt acknowledge
        if (mask & 0x00ff) {
            if (v & 1) set_line(1, false);
            if (v & 2) set_line(2, false);
            if (v & 8) set_line(6, false);
        }
        return;
    case 0x240000: g_dma_addr = (g_dma_addr & ~0xffffu) | v; return;
    case 0x250000: g_dma_addr = (g_dma_addr & 0xffff) | (uint32_t)v << 16; return;
    case 0x260000: g_wave_bank = (uint32_t)v * 4 * 1024 * 1024; return;
    default: break;
    }
    if (a >= 0x280000 && a < 0x280800) {
        if (mask & 0x00ff) {
            uint32_t o = (a - 0x280000) >> 1;
            g_dpram[o] = (uint8_t)v;
            if (o == 0x3ff) cpu403::set_irq_line(cpu403::irq_ext(3), true);   // left write of 0x3ff: INTR -> PPC
        }
        return;
    }
    if (a >= 0x300000 && a < 0x300010) { trace_io("W", a, v); g_ata->write(0, (a & 0xf) >> 1, v); g_stats.ata_commands += ((a & 0xf) == 0xe); return; }
    if (a >= 0x340000 && a < 0x340010) { trace_io("W", a, v); g_ata->write(1, (a & 0xf) >> 1, v); return; }
    if (a >= 0x400000 && a < 0x401000) {
        uint32_t o = (a - 0x400000) >> 1;
        if (o == 1 && (v & 0x60) == 0x60) {
            g_stats.key_ons++;
            if (g_lat_trace) {
                double t = (double)cycles_now() / SPU_HZ * 1000.0 - (double)fb::last_input_change() / CPU_HZ * 1000.0;
                if (t < 100) fprintf(stderr, "lat %7.2f ms: RF5C400 key-on\n", t);
            }
            // latency: from the last player key press to this key-on, at the 68000's current time
            static uint64_t measured_for = UINT64_MAX;
            uint64_t in = fb::last_input_change();
            double t68 = (double)cycles_now() / SPU_HZ * 1000.0, tin = (double)in / CPU_HZ * 1000.0;
            if (in && in != measured_for && t68 >= tin && t68 - tin < 100.0) {
                measured_for = in;
                double ms = t68 - tin;
                g_stats.lat_n++;
                g_stats.lat_sum_ms += ms;
                g_stats.lat_min_ms = std::min(g_stats.lat_min_ms, ms);
                g_stats.lat_max_ms = std::max(g_stats.lat_max_ms, ms);
                g_stats.lat_hist[std::min(15, (int)(ms / 2))]++;
            }
        }
        g_rf->write(o, v);
        return;
    }
}

}  // namespace

bool present() { return g_present; }
const Stats &stats() {
    g_stats.pc = m68k_get_reg(nullptr, M68K_REG_PC);
    g_stats.sr = m68k_get_reg(nullptr, M68K_REG_SR);
    return g_stats;
}
void set_audio_sink(std::function<void(const float *, int)> sink) { g_sink = std::move(sink); }

void catch_up() {
    if (!g_present || g_running) return;
    uint64_t target = (uint64_t)((double)now() * SPU_HZ / (double)CPU_HZ);
    prof::Scope ps(prof::SPU68K);
    while (g_cycles < target) {
        g_running = true;
        int ran = m68k_execute((int)std::min<uint64_t>(target - g_cycles, 20000));
        g_running = false;
        g_cycles += (uint64_t)ran;
        g_stats.cycles = g_cycles;
    }
    render_until(g_cycles);
}

uint8_t ppc_read(uint32_t o) {
    catch_up();
    o &= 0x3ff;
    if (o == 0x3ff) cpu403::set_irq_line(cpu403::irq_ext(3), false);   // right read of 0x3ff acknowledges INTR
    return g_dpram[o];
}

static double lat_ms_since_input() { return ((double)now() - (double)fb::last_input_change()) / CPU_HZ * 1000.0; }

void ppc_write(uint32_t o, uint8_t v) {
    catch_up();
    o &= 0x3ff;
    if (g_lat_trace && lat_ms_since_input() < 60) fprintf(stderr, "lat %7.2f ms: ppc dpram[%03x] = %02x\n", lat_ms_since_input(), o, v);
    g_dpram[o] = v;
    if (o == 0x3fe) { set_line(4, true); g_stats.irq4++; }             // right write of 0x3fe: INTL -> 68000 IRQ4
}

bool init(const Config &cfg) {
    g_cfg = cfg;
    std::ifstream in(cfg.rom, std::ios::binary);
    if (!in) return false;
    std::vector<uint8_t> raw((std::istreambuf_iterator<char>(in)), {});
    if (raw.size() != 0x80000) return false;
    g_rom.resize(raw.size());
    for (size_t k = 0; k < raw.size(); k += 2) { g_rom[k] = raw[k + 1]; g_rom[k + 1] = raw[k]; }   // ROM_LOAD16_WORD_SWAP

    g_ata = std::make_unique<AtaBus>();
    g_ata->log = cfg.log;
    // A sector-by-sector (PIO) read ends without an interrupt, as in the ATA spec and MAME. The
    // sound program waits for one interrupt per sector; one more after the last left its "done"
    // flag set, so its next READ DMA didn't wait for the one before it: the stage's music landed
    // in the wrong bank and the song's channels began on what was left there (issue 15)
    g_ata->pio_end_irq = false;
    if (!cfg.hdd.empty()) {
        auto d = std::make_unique<AtaDevice>("spu-hdd", false);
        d->disk = std::make_unique<DiskImage>();
        if (!d->disk->open(cfg.hdd)) fatal(nullptr, "cannot open HDD image %s", cfg.hdd.c_str());
        g_ata->attach(0, std::move(d));
    } else if (!cfg.dvd.empty()) {
        auto d = std::make_unique<AtaDevice>("spu-dvd", true);
        d->disc = std::make_unique<DiscImage>();
        if (!d->disc->open(cfg.dvd)) fatal(nullptr, "cannot open DVD image %s", cfg.dvd.c_str());
        g_ata->attach(0, std::move(d));
    }
    // ATA interrupt: delivered as IRQ6 on the next 500 Hz tick (MAME spu_ata_interrupt)
    g_ata->irq = [](bool on) {
        if (!on) set_line(2, false);
        g_sync_ata_irq = on;
    };
    g_ata->dmarq = [](bool on) {
        if (on == g_dmarq) return;
        g_dmarq = on;
        if (on) g_dma.in(0);
    };

    g_rf = std::make_unique<Rf5c400>(16934400);
    g_rf->read_word = [](uint32_t w) { return g_wave[w & 0xffffff]; };
    g_rf->write_word = [](uint32_t w, uint16_t v) { g_wave[w & 0xffffff] = v; };
    g_rf->sync = [] { render_until(cycles_now()); };

    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_pulse_reset();
    g_present = true;
    g_tick.in(ticks_from_hz(500));
    return true;
}

}  // namespace hk::spu

// ---- Musashi memory callbacks --------------------------------------------------------
using namespace hk::spu;
extern "C" {
unsigned int m68k_read_memory_8(unsigned int a) {
    uint16_t w = read16(a);
    return (a & 1) ? (w & 0xff) : (w >> 8);
}
unsigned int m68k_read_memory_16(unsigned int a) { return read16(a); }
unsigned int m68k_read_memory_32(unsigned int a) { return (unsigned)read16(a) << 16 | read16(a + 2); }
void m68k_write_memory_8(unsigned int a, unsigned int v) {
    if (a & 1) write16(a, (uint16_t)(v & 0xff), 0x00ff);
    else write16(a, (uint16_t)((v & 0xff) << 8), 0xff00);
}
void m68k_write_memory_16(unsigned int a, unsigned int v) { write16(a, (uint16_t)v, 0xffff); }
void m68k_write_memory_32(unsigned int a, unsigned int v) {
    write16(a, (uint16_t)(v >> 16), 0xffff);
    write16(a + 2, (uint16_t)v, 0xffff);
}
unsigned int m68k_read_disassembler_8(unsigned int a) { return m68k_read_memory_8(a); }
unsigned int m68k_read_disassembler_16(unsigned int a) { return m68k_read_memory_16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return m68k_read_memory_32(a); }
}
