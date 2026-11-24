// SPDX-License-Identifier: GPL-3.0-or-later
// The Firebeat SPU board (beatmania III, pop'n music): a 68000 running Konami's
// sound program, an RF5C400 with 32 MiB of wave RAM, its own ATA bus (HDD or
// DVD-ROM) feeding the wave RAM by DMA, and a dual-port RAM mailbox to the
// PowerPC. Board semantics follow MAME's firebeat.cpp (BSD-3-Clause).
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace hk::spu {

struct Config {
    std::string rom;               // 512 KiB 68000 program (word-swapped, as in MAME sets)
    std::string hdd;               // beatmania III: raw HDD image on the SPU's ATA bus
    std::string dvd;               // pop'n music: DVD image on the SPU's ATA bus
    bool mix_effect = true;        // beatmania III routes the effect outputs to the speakers too
    bool log = false;
};

bool init(const Config &cfg);
bool present();

// PowerPC side of the dual-port RAM (0x74000000-0x740003ff)
uint8_t ppc_read(uint32_t offset);
void ppc_write(uint32_t offset, uint8_t v);

// Bring the 68000 (and the sound it produces) up to the current virtual time.
void catch_up();

// beatmania III: a byte the game sent its effects unit (the extend board's MIDI port), at this moment
// of virtual time. The unit (st224.h) works on the RF5C400's effect outputs; HK_ST224=0 leaves them
// as they are (as MAME).
void effects_midi(uint8_t b);

// Stereo output at 44.1 kHz, delivered in chunks as virtual time advances.
void set_audio_sink(std::function<void(const float *lr, int frames)> sink);

struct Stats {
    uint64_t cycles = 0, irq2 = 0, irq4 = 0, irq6 = 0, dma_words = 0, ata_commands = 0, key_ons = 0;
    uint32_t pc = 0, sr = 0;
    // input -> key-on latency (virtual time), for key-ons within 100 ms of a player key press
    uint64_t lat_n = 0;
    double lat_sum_ms = 0, lat_min_ms = 1e9, lat_max_ms = 0;
    uint32_t lat_hist[16] = {};   // 2 ms buckets
};
const Stats &stats();

}  // namespace hk::spu
