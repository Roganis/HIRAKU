// license:BSD-3-Clause
// copyright-holders:R. Belmont, Olivier Galibert
//
// Yamaha XT446 (embedded MU100B), ported from MAME's xt446.cpp: same memory maps,
// ROM layout and CPU hookups, except that MIDI comes in on SCI1 (see midi_w). See xt446.h for
// what else differs.
#include "xt446.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <vector>

#include "dev/xt446_h8jit.h"
#include "emu.h"
#include "cpu/h8/h8s2655.h"
#include "sound/swp30.h"

namespace {

class xt446_device : public device_t {
public:
    xt446_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock = 0);

    // The game's MIDI is the MU100 firmware's port A (parts 1-16), which is SCI1, as MAME's MU100
    // driver wires it; SCI0 is port B (parts 17-32). MAME's xt446.h uses SCI0, and then a song that
    // sets parts 17-32's receive channels silences the game's own channels (issue 14).
    void midi_w(int state) { m_maincpu->sci_rx_w<1>(state); }

    required_device<h8s2655_device> m_maincpu;
    required_device<swp30_device> m_swp30;

protected:
    void device_add_mconfig(machine_config &config) override;

private:
    void xt446_map(address_map &map);
    void swp30_map(address_map &map);
};

DEFINE_DEVICE_TYPE(XT446, xt446_device, "xt446", "Yamaha XT446 synth (embedded MU100B)")

xt446_device::xt446_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock)
    : device_t(mconfig, XT446, tag, owner, clock), m_maincpu(*this, "maincpu"), m_swp30(*this, "swp30") {}

void xt446_device::xt446_map(address_map &map) {
    map(0x000000, 0x1fffff).rom().region("maincpu", 0);
    map(0x200000, 0x21ffff).ram();   // 128K work RAM
    map(0x400000, 0x401fff).m(m_swp30, FUNC(swp30_device::map));
}

void xt446_device::swp30_map(address_map &map) {
    map(0x000000, 0x1fffff).rom().region("swp30", 0).mirror(0x200000);
    map(0x400000, 0x4fffff).rom().region("swp30", 0x800000).mirror(0x300000);
    map(0x800000, 0x9fffff).rom().region("swp30", 0x1000000).mirror(0x200000);
}

void xt446_device::device_add_mconfig(machine_config &config) {
    H8S2655(config, m_maincpu, 16_MHz_XTAL);
    m_maincpu->set_addrmap(AS_PROGRAM, address_map_constructor(&xt446_device::xt446_map, this));
    m_maincpu->read_adc<0>().set_constant(0);
    m_maincpu->read_adc<1>().set_constant(0);
    m_maincpu->read_adc<2>().set_constant(0);
    m_maincpu->read_adc<3>().set_constant(0);
    m_maincpu->read_adc<4>().set_constant(0);
    m_maincpu->read_adc<5>().set_constant(0);
    m_maincpu->read_adc<6>().set_constant(0x200);
    m_maincpu->read_adc<7>().set_constant(0x3ff);
    m_maincpu->read_port1().set_constant(0);
    m_maincpu->write_port1().set_nop();
    m_maincpu->write_port2().set_nop();
    m_maincpu->read_port6().set_constant(0);
    m_maincpu->read_porta().set_constant(0);
    m_maincpu->write_portf().set_nop();

    SWP30(config, m_swp30);
    m_swp30->set_addrmap(AS_DATA, address_map_constructor(&xt446_device::swp30_map, this));
}

bool read_file(const std::string &path, size_t size, std::vector<u8> &out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f || (size_t)f.tellg() != size) return false;
    out.resize(size);
    f.seekg(0);
    return (bool)f.read(reinterpret_cast<char *>(out.data()), (std::streamsize)size);
}

// The SWP30's sine table (a quarter wave, 0x8000..0xffff), which MAME's sets carry as
// sin-table.bin only after 0.289: 32 points 1/256 apart, joined by straight lines and
// capped at 0xffff, give back the dump byte for byte (CRC 4305f63c)
std::vector<u8> swp30_sin_table() {
    static const u16 point[33] = {
        0, 13, 26, 38, 50, 62, 74, 86, 98, 109, 120, 131, 142, 152, 162, 172, 181,
        190, 198, 206, 213, 220, 226, 232, 237, 241, 245, 248, 251, 253, 255, 256, 256};
    std::vector<u8> out(0x10000);
    for (u32 i = 0; i < 0x8000; i++) {
        const u32 a = (0x100u + point[i >> 10]) << 7, b = (0x100u + point[(i >> 10) + 1]) << 7;
        const u32 v = std::min<u32>(0xffff, a + (b - a) * (i & 0x3ff) / 0x400);
        out[2 * i] = (u8)v;
        out[2 * i + 1] = (u8)(v >> 8);
    }
    return out;
}

}  // namespace

namespace hk {

struct Xt446::Impl {
    running_machine machine;
    xt446_device *dev = nullptr;

    // the MIDI line: level changes waiting for their time
    std::deque<std::pair<attotime, int>> line;
    attotime line_free;               // when the last queued byte starts its stop bit
    emu_timer *line_timer = nullptr;

    std::vector<float> out;           // rendered, not yet taken (interleaved L/R)
    size_t out_pos = 0;
    u64 sample_index = 0;             // samples run (the machine is at sample_index / 44100 s)
    u64 midi_count = 0;
    double cpu_s = 0;

    // HK_H8_JIT=check: the same card again with its H8 interpreted, run alongside and compared
    std::unique_ptr<Impl> shadow;
    u64 checked = 0;

    ~Impl() {
        if (dev && dev->m_maincpu->m_hk_ctx && getenv("HK_H8_STATS")) fprintf(stderr, "%s\n", hk::h8jit::stats(*dev->m_maincpu).c_str());
        if (dev) hk::h8jit::detach(*dev->m_maincpu);   // (its made code freed)
    }
    void start(const std::vector<u8> &prg, const std::vector<u8> &wave, const std::vector<u8> &sintab) {
        machine.add_region("maincpu", std::vector<u8>(prg));
        machine.add_region("swp30", std::vector<u8>(wave));
        machine.add_region("sintab", std::vector<u8>(sintab));
        dev = &XT446(machine.config(), "xt446");
        dev->m_swp30->hk_on_sample = [this](const std::array<s32, 4> &adc) {
            // MAME: stream outputs 0/1 (the DAC) to the speaker, put_int_clamp(.., 1 << 17)
            for (int i = 0; i < 2; i++) out.push_back((float)std::clamp<s32>(adc[i], -(1 << 17), (1 << 17) - 1) / (float)(1 << 17));
        };
        line_timer = machine.alloc_timer([this](s32) { line_fire(); });
        machine.start();
        machine.reset();
    }
    // the H8 made into host code unless switched off (HK_JIT=0, HK_H8_JIT=0); with
    // HK_H8_JIT=check a second card, interpreted, to compare with
    void h8_code(const std::vector<u8> &prg, const std::vector<u8> &wave, const std::vector<u8> &sintab) {
        const char *e = getenv("HK_H8_JIT");
        if (e && !strcmp(e, "check")) {
            shadow = std::make_unique<Impl>();
            shadow->start(prg, wave, sintab);
        }
        hk::h8jit::attach(*dev->m_maincpu);
    }
    // after each sample: the shadow card brought to the same time and compared (the H8's state
    // every sample, its RAM every 64th, and the sound)
    void check() {
        Impl &o = *shadow;
        o.sample_index = sample_index;
        o.machine.run_until(attotime::from_ticks(sample_index, 44100u));
        std::string d = hk::h8jit::compare(*dev->m_maincpu, *o.dev->m_maincpu);
        if (d.empty() && (sample_index & 63) == 0) {
            address_space &a = dev->m_maincpu->space(AS_PROGRAM), &b = o.dev->m_maincpu->space(AS_PROGRAM);
            for (offs_t r0 : {0x200000u, 0xffec00u}) {
                const offs_t len = r0 == 0x200000u ? 0x20000u : 0x1000u;
                for (offs_t k = 0; k < len && d.empty(); k += 0x100)
                    if (memcmp(a.mem_ptr(r0 + k, false), b.mem_ptr(r0 + k, false), 0x100)) d = "RAM near " + std::to_string(r0 + k);
            }
        }
        if (d.empty() && (out.size() != o.out.size() || !std::equal(out.begin(), out.end(), o.out.begin())))
            d = "sound";
        if (!d.empty()) {
            fprintf(stderr, "xt446: HK_H8_JIT=check: made code and the interpreter differ at sample %llu: %s\n", (unsigned long long)sample_index, d.c_str());
            abort();
        }
        checked++;
    }

    void arm_line() {
        if (line.empty()) return;
        attotime d = line.front().first - machine.time();
        line_timer->adjust(d < attotime::zero ? attotime::zero : d);
    }
    // 8N1 at 31250 baud, timed like MAME's 16550 (ins8250.cpp/diserial.cpp): a byte
    // reaches the shift register now, or when the previous one starts its stop bit;
    // its bit clock (toggled every half bit) then makes its first rising edge, the
    // start bit, one bit later. Bytes back to back are 320 us apart.
    void send(u8 b) {
        midi_count++;
        const attotime bit = attotime::from_usec(32);
        attotime t = std::max(machine.time(), line_free) + bit;
        line.emplace_back(t, 0);
        for (int i = 0; i < 8; i++) line.emplace_back(t + bit * (u32)(i + 1), (b >> i) & 1);
        line.emplace_back(t + bit * 9u, 1);
        line_free = t + bit * 9u;
        arm_line();
    }
    void line_fire() {
        attotime now = machine.time();
        while (!line.empty() && line.front().first <= now) {
            dev->midi_w(line.front().second);
            line.pop_front();
        }
        arm_line();
    }
};

Xt446::Xt446() : m(std::make_unique<Impl>()) {}
Xt446::~Xt446() = default;

bool Xt446::load(const std::string &dir, std::string &err) {
    auto path = [&](const char *f) { return dir + "/" + f; };
    // H8S program: ROM_LOAD16_WORD_SWAP into a big-endian 16-bit space = the file's
    // bytes as host (little-endian) words
    std::vector<u8> prg;
    if (!read_file(path("xu50710-m27c160.bin"), 0x200000, prg)) { err = "xu50710-m27c160.bin (2 MB) missing"; return false; }
    // wave ROMs: ROM_REGION32_LE, ROM_LOAD32_WORD (pairs of files interleaved by 16-bit word)
    std::vector<u8> wave(0x1800000, 0);
    struct { const char *name; size_t off, size; } waves[] = {
        {"sx518b0.ic34", 0x0000000, 0x400000}, {"sx743b0.ic35", 0x0000002, 0x400000},
        {"xt445a0-828.ic36", 0x0800000, 0x200000}, {"xt461a0-829.ic37", 0x0800002, 0x200000},
        {"xt462a0.ic39", 0x1000000, 0x400000}, {"xt463a0.ic38", 0x1000002, 0x400000},
    };
    for (auto &w : waves) {
        std::vector<u8> d;
        if (!read_file(path(w.name), w.size, d)) { err = std::string(w.name) + " missing"; return false; }
        for (size_t k = 0; k < w.size / 2; k++) {
            wave[w.off + 4 * k] = d[2 * k];
            wave[w.off + 4 * k + 1] = d[2 * k + 1];
        }
    }
    std::vector<u8> sintab;
    if (!read_file(path("sin-table.bin"), 0x10000, sintab)) sintab = swp30_sin_table();

    Impl &s = *m;
    s.start(prg, wave, sintab);
    if (const char *e = getenv("HK_XT446_SWPLOG"); e && *e) {
        // debugging: the H8's writes to the SWP30, in the MAME oracle's swp-w.txt format
        static FILE *f = fopen(e, "w");
        address_space &sp = s.dev->m_maincpu->space(AS_PROGRAM);
        sp.m_write_tap = [&s](offs_t a, u64 d, u64 mask) {
            if (a >= 0x400000 && a < 0x402000 && f)
                fprintf(f, "%.6f %06x %04x %04x\n", s.machine.time().as_double(), a, (unsigned)(d & 0xffff), (unsigned)(mask & 0xffff));
        };
        sp.m_read_tap = [&s](offs_t a, u64 d, u64 mask) {
            if (a >= 0x400000 && a < 0x402000 && f)
                fprintf(f, "%.6f %06x %04x %04x r\n", s.machine.time().as_double(), a, (unsigned)(d & 0xffff), (unsigned)(mask & 0xffff));
        };
    }
    s.h8_code(prg, wave, sintab);
    return true;
}

std::string Xt446::meg_selftest(unsigned seed, int rounds) {
    // two cards with blank ROMs, each in its own machine: started and reset, never run
    Impl a, b;
    for (Impl *s : {&a, &b}) {
        s->machine.add_region("maincpu", std::vector<u8>(0x200000, 0));
        s->machine.add_region("swp30", std::vector<u8>(0x1800000, 0));
        s->machine.add_region("sintab", swp30_sin_table());
        s->dev = &XT446(s->machine.config(), "xt446");
        s->machine.start();
        s->machine.reset();
    }
    return a.dev->m_swp30->hk_meg_selftest(*b.dev->m_swp30, seed, rounds);
}

std::string Xt446::h8_selftest(unsigned seed, int programs, int samples) {
    const std::vector<u8> wave(0x1800000, 0), sintab = swp30_sin_table();
    u64 made = 0, total = 0;
    for (int p = 0; p < programs; p++) {
        const unsigned sd = seed + (unsigned)p;
        const std::vector<u8> rom = hk::h8jit::test_program(sd);
        if (const char *e = getenv("HK_H8_TEST_DUMP")) {   // (the program, for a disassembler)
            if (FILE *f = fopen((std::string(e) + std::to_string(sd)).c_str(), "wb")) { fwrite(rom.data(), 1, rom.size(), f); fclose(f); }
        }
        Impl a, b;
        a.start(rom, wave, sintab);
        b.start(rom, wave, sintab);
        if (!hk::h8jit::attach(*a.dev->m_maincpu)) return "skip: this build makes no code for the H8";
        u64 x = 0x853c49e6748fea9bull ^ sd;
        auto rnd = [&x] { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return (u32)(x >> 24); };
        for (int k = 1; k <= samples; k++) {
            if (rnd() % 64 == 0) {   // a byte on the serial line now and then
                const u8 byte = (u8)rnd();
                a.send(byte);
                b.send(byte);
            }
            const attotime t = attotime::from_ticks((u64)k, 44100u);
            std::string ea, eb;
            try { a.machine.run_until(t); } catch (const std::exception &e) { ea = e.what(); }
            try { b.machine.run_until(t); } catch (const std::exception &e) { eb = e.what(); }
            char where[64];
            std::snprintf(where, sizeof where, "program %u, sample %d: ", sd, k);
            if (ea != eb) return where + std::string("made code: '") + ea + "', interpreter: '" + eb + "'";
            if (!ea.empty()) break;   // (both stopped the same way: the program wrote somewhere MAME gives up)
            std::string d = hk::h8jit::compare(*a.dev->m_maincpu, *b.dev->m_maincpu);
            if (d.empty() && ((k & 15) == 0 || k == samples)) {
                address_space &sa = a.dev->m_maincpu->space(AS_PROGRAM), &sb = b.dev->m_maincpu->space(AS_PROGRAM);
                for (offs_t r0 : {0x200000u, 0xffec00u}) {
                    const offs_t len = r0 == 0x200000u ? 0x20000u : 0x1000u;
                    for (offs_t o = 0; o < len && d.empty(); o += 0x100)
                        if (memcmp(sa.mem_ptr(r0 + o, false), sb.mem_ptr(r0 + o, false), 0x100)) d = "RAM near " + std::to_string(r0 + o);
                }
            }
            if (!d.empty()) return where + d;
        }
        if (getenv("HK_H8_STATS")) fprintf(stderr, "program %u: %s\n", sd, hk::h8jit::stats(*a.dev->m_maincpu).c_str());
        made += hk::h8jit::made_cycles(*a.dev->m_maincpu);
        total += a.dev->m_maincpu->total_cycles();
        hk::h8jit::detach(*a.dev->m_maincpu);
    }
    if (total && made * 4 < total) return "made code ran only " + std::to_string(100 * made / total) + " % of the cycles";
    return "";
}

void Xt446::midi(uint8_t b) {
    Impl &s = *m;
    if (!s.dev) return;
    s.send(b);
    if (s.shadow) {
        s.shadow->line = s.line;
        s.shadow->line_free = s.line_free;
        s.shadow->arm_line();
    }
}

void Xt446::render(float *lr, int frames, float gain) { produce(frames, lr, gain, true); }
void Xt446::take(float *lr, int frames) { produce(frames, lr, 1.0f, false); }

void Xt446::produce(int frames, float *lr, float gain, bool add) {
    Impl &s = *m;
    if (!s.dev || frames <= 0) return;
    auto t0 = std::chrono::steady_clock::now();
    size_t need = (size_t)frames * 2;
    while (s.out.size() - s.out_pos < need) {
        s.sample_index++;
        s.machine.run_until(attotime::from_ticks(s.sample_index, 44100u));
        if (s.shadow) s.check();
    }
    if (add)
        for (size_t k = 0; k < need; k++) lr[k] += s.out[s.out_pos + k] * gain;
    else
        std::copy_n(s.out.begin() + (std::ptrdiff_t)s.out_pos, need, lr);
    s.out_pos += need;
    if (s.out_pos > 65536) {
        s.out.erase(s.out.begin(), s.out.begin() + (std::ptrdiff_t)s.out_pos);
        if (s.shadow) s.shadow->out.erase(s.shadow->out.begin(), s.shadow->out.begin() + (std::ptrdiff_t)s.out_pos);
        s.out_pos = 0;
    }
    s.cpu_s += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

uint64_t Xt446::samples() const { return m->sample_index; }
uint64_t Xt446::midi_bytes() const { return m->midi_count; }
double Xt446::cpu_seconds() const { return m->cpu_s; }

}  // namespace hk
