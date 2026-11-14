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
#include <deque>
#include <fstream>
#include <vector>

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

    void arm_line() {
        if (line.empty()) return;
        attotime d = line.front().first - machine.time();
        line_timer->adjust(d < attotime::zero ? attotime::zero : d);
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
    s.machine.add_region("maincpu", std::move(prg));
    s.machine.add_region("swp30", std::move(wave));
    s.machine.add_region("sintab", std::move(sintab));
    s.dev = &XT446(s.machine.config(), "xt446");
    s.dev->m_swp30->hk_on_sample = [&s](const std::array<s32, 4> &adc) {
        // MAME: stream outputs 0/1 (the DAC) to the speaker, put_int_clamp(.., 1 << 17)
        for (int i = 0; i < 2; i++) s.out.push_back((float)std::clamp<s32>(adc[i], -(1 << 17), (1 << 17) - 1) / (float)(1 << 17));
    };
    s.line_timer = s.machine.alloc_timer([&s](s32) { s.line_fire(); });
    s.machine.start();
    s.machine.reset();
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
    return true;
}

void Xt446::midi(uint8_t b) {
    Impl &s = *m;
    if (!s.dev) return;
    s.midi_count++;
    // 8N1 at 31250 baud, timed like MAME's 16550 (ins8250.cpp/diserial.cpp): a byte
    // reaches the shift register now, or when the previous one starts its stop bit;
    // its bit clock (toggled every half bit) then makes its first rising edge, the
    // start bit, one bit later. Bytes back to back are 320 us apart.
    const attotime bit = attotime::from_usec(32);
    attotime t = std::max(s.machine.time(), s.line_free) + bit;
    s.line.emplace_back(t, 0);
    for (int i = 0; i < 8; i++) s.line.emplace_back(t + bit * (u32)(i + 1), (b >> i) & 1);
    s.line.emplace_back(t + bit * 9u, 1);
    s.line_free = t + bit * 9u;
    s.arm_line();
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
    }
    if (add)
        for (size_t k = 0; k < need; k++) lr[k] += s.out[s.out_pos + k] * gain;
    else
        std::copy_n(s.out.begin() + (std::ptrdiff_t)s.out_pos, need, lr);
    s.out_pos += need;
    if (s.out_pos > 65536) {
        s.out.erase(s.out.begin(), s.out.begin() + (std::ptrdiff_t)s.out_pos);
        s.out_pos = 0;
    }
    s.cpu_s += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

uint64_t Xt446::samples() const { return m->sample_index; }
uint64_t Xt446::midi_bytes() const { return m->midi_count; }
double Xt446::cpu_seconds() const { return m->cpu_s; }

}  // namespace hk
