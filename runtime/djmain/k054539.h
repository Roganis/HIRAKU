// license:BSD-3-Clause
// copyright-holders:Olivier Galibert
//
// Konami 054539 PCM sound chip. Ported to HIRAKU from MAME's
// devices/sound/k054539.cpp (BSD-3-Clause): same registers, sample formats,
// reverb and volume/pan tables. The chip renders on demand: the board calls
// render() up to the present before every register access, as MAME's
// stream->update() does on writes, so key-ons and key-offs land on the same
// sample as in MAME (see commit() for reads). DJ Main runs two of them on one
// shared 16 MiB sample RAM.
#pragma once

#include <cstdint>

namespace hk::dj {

class K054539 {
public:
    static constexpr int RATE = 48000;   // 18.432 MHz / 384

    K054539();
    void reset();
    // `rom`: the sample address space (DJ Main: sndram), `rom_mask` its size - 1.
    void set_rom(const uint8_t *rom, uint32_t rom_mask) { m_rom = rom; m_rom_mask = rom_mask; }

    uint8_t read(uint32_t offset);
    void write(uint32_t offset, uint8_t data);
    // Add n samples (48 kHz stereo, the chip's own scale: 1.0 = 32768) to lr.
    void render(float *lr, int n);
    // What a read of the registers the chip itself updates (key-on status 0x22c,
    // channel positions) returns is their state at the last commit(). MAME
    // brings the chip up to date on writes and on its 50 Hz stream update, not
    // on reads, so the game sees a sample end only then; the board renders
    // earlier than that (for latency) and commits where MAME would update.
    void commit();

private:
    struct Channel { uint32_t pos = 0; int pfrac = 0, val = 0, pval = 0; };
    bool regupdate() const { return !(m_regs[0x22f] & 0x80); }
    void keyon(int ch) { if (regupdate()) m_regs[0x22c] |= (uint8_t)(1 << ch); }
    void keyoff(int ch) { if (regupdate()) m_regs[0x22c] &= (uint8_t)~(1 << ch); }
    uint8_t read_byte(uint32_t a) const { return m_rom ? m_rom[a & m_rom_mask] : 0; }

    double m_voltab[256], m_pantab[0xf];
    double m_gain[8];
    uint8_t m_posreg_latch[8][3]{};
    int m_flags;
    void write_reg(uint32_t offset, uint8_t data);

    uint8_t m_regs[0x230]{};
    uint8_t m_seen_status = 0, m_seen_pos[8][3]{};   // as of the last commit()
    alignas(int16_t) uint8_t m_ram[0x8000]{};   // reverb RAM (read as 16-bit samples too: render)
    int m_reverb_pos = 0;
    int32_t m_cur_ptr = 0;
    int m_rom_addr = 0;
    Channel m_channels[8];
    const uint8_t *m_rom = nullptr;
    uint32_t m_rom_mask = 0;
};

}  // namespace hk::dj
