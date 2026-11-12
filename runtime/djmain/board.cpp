// SPDX-License-Identifier: GPL-3.0-or-later
// The DJ Main board. Address decoding, handlers, interrupts, inputs and screen
// composition follow MAME's djmain.cpp (BSD-3-Clause, copyright-holders: smf);
// the sprite drawing follows its draw_sprites() and MAME's drawgfx (BSD-3-Clause,
// Nicola Salmoria, Aaron Giles).
#include "board.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>

#include "../core/prof.h"
#include "hiraku/scroll_guess.h"
#include "../dev/ata.h"
#include "../shell/cabinet.h"
#include "k054539.h"
#include "k056832.h"
#include "m68k.h"

extern "C" void hk_dj_itrace_frame(uint64_t frame);   // HK_DJ_ITRACE (below)

namespace hk::dj {

namespace {

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
Config g_cfg;
std::vector<uint8_t> g_rom(0x100000), g_ram(0x10000);
std::vector<uint8_t> g_palram(0x4440);                 // xBGR_888 words, big-endian
std::vector<uint8_t> g_objram(0x800);                  // sprite list, big-endian
std::vector<uint8_t> g_sndram(0x1000000);              // both K054539s' sample RAM
std::vector<uint8_t> g_objrom, g_tilerom;              // raw regions (POST readthrough)
std::vector<uint8_t> g_sprite_pens;                    // decoded 16x16 sprites, 256 pens each
uint32_t g_sprite_count = 0;
uint32_t g_obj_regs[0xa0 / 4]{};
uint8_t g_k055555[48]{};
K056832 g_tiles;
K054539 g_snd[2];
AtaBus g_ata;
uint32_t g_ata_disk_sectors = 0;

uint8_t g_sndram_bank = 0, g_pending_vb_int = 0;
uint16_t g_v_ctrl = 0, g_light_ctrl[2]{};
uint8_t g_pcb_leds = 0xff;
uint8_t g_turntable_select = 0, g_turntable_last_pos[2]{};
uint16_t g_turntable_pos[2]{};
// Inputs as MAME's ports: DSW3 BTN3 BTN2 BTN1 DSW1 DSW2 (input_r's order), active low
uint8_t g_in[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
uint8_t g_dial[2]{};                                   // TT1/TT2: MAME's 8-bit dial ports
int g_dial_frac[2]{};

// interrupts: MAME HOLD_LINE (cleared when the CPU acknowledges)
bool g_hold4 = false, g_hold1 = false;

uint64_t g_cycles = 0;          // CPU clocks at the start of the current m68k_execute
bool g_in_execute = false;
uint64_t g_frame = 0;
constexpr uint64_t VBLANK_FLOOR = 2 * 34 * 660;   // the shortest frame set_frame_rate gives: twice the vblank
uint64_t g_frame_start = 0;               // the CPU cycle the frame began at
uint64_t g_frame_cycles = FRAME_CYCLES;   // its length (set_frame_rate)
uint64_t g_want_frame_cycles = FRAME_CYCLES;
FrameHook g_frame_hook;
PresentHook g_present_hook;
bool g_frame_picture = true;   // the frame hook's picture is composed (frame_picture_unused)
AudioSink g_audio;
Stats g_stats;
std::vector<uint8_t> g_lamps;
uint64_t g_lamp_changes = 0;
std::vector<uint32_t> g_screen(SCREEN_W * SCREEN_H);
std::vector<uint8_t> g_shown_objram, g_prev_objram;   // the sprite lists of the last two frames
std::vector<uint32_t> g_between(SCREEN_W * SCREEN_H);  // render_between()'s picture
// the sprite list the picture just shown was drawn from, and the one before (render_between())
void keep_shown_sprites() {
    g_prev_objram.swap(g_shown_objram);
    g_shown_objram = g_objram;
}

// sound: both chips render at 48 kHz up to the present on every access
uint64_t g_snd_done = 0;        // 48 kHz samples rendered so far
std::vector<float> g_snd48;     // rendered, not yet handed to the shell (stereo, 48 kHz)

uint64_t now() { return g_cycles + (g_in_execute ? (uint64_t)m68k_cycles_run() : 0); }

// ---------------------------------------------------------------------------
// Interrupts
// ---------------------------------------------------------------------------
void update_irq() { m68k_set_irq(g_hold4 ? 4 : g_hold1 ? 1 : 0); }

// A device changed an interrupt line. Musashi looks at the lines only when
// m68k_execute() starts; MAME's scheduler aborts the timeslice on any input
// change (device_input::set_state_synced -> synchronize), so the interrupt is
// taken right after the current instruction. Do the same.
void irq_changed() {
    update_irq();
    if (g_in_execute) m68k_end_timeslice();
}

void ide_interrupt(bool state) {
    if (state) {
        g_hold1 = true;
        g_stats.irq1++;
    } else {
        g_hold1 = false;
    }
    irq_changed();
}

void vb_interrupt() {
    if (g_v_ctrl & 0x8000) {
        g_hold4 = true;
        g_stats.irq4++;
        g_pending_vb_int = 0;
        irq_changed();
    } else {
        g_pending_vb_int = 1;
    }
}

// ---------------------------------------------------------------------------
// Hard disk timing. AtaBus answers at once; MAME's drive (atastorage.cpp) is
// busy first: a read seeks (13 ms per full stroke, scaled by the cylinders
// travelled, plus 1.3 ms of rotation), simple commands take 10 us, READ
// SECTORS spaces its sectors 400 ns apart. The game's IDE task sleeps until the
// interrupt, so an instant interrupt (before it sleeps) would be lost. Here
// the drive shows BSY and the interrupt waits until the busy time is over.
// ---------------------------------------------------------------------------
uint32_t g_hdd_heads = 16, g_hdd_sectors = 63;   // the disk's geometry (Config::hdd_chs)
uint64_t g_ata_busy_until = 0;       // CPU clock when the drive stops being busy
bool g_ata_irq_deferred = false;      // AtaBus raised its interrupt while busy
bool g_ata_event = false;             // a busy period is pending
uint8_t g_ata_regs[8]{};              // the task file as written (for the LBA)
uint8_t g_ata_command = 0;
uint64_t g_ata_cur_lba = 0;           // MAME m_cur_lba: where the heads are
// a PIO read in progress (READ SECTORS / MULTIPLE): MAME moves the heads a sector at a time, as each
// sector is loaded (next_sector() in finished_read()), and a sector loads only once the game has
// read the one before. So where the heads are at the next command depends on how much was read.
bool g_ata_pio_read = false;
uint64_t g_ata_pio_start = 0;
uint32_t g_ata_pio_count = 0, g_ata_pio_words = 0;
uint64_t g_ata_index_since = 0;       // CPU clock of the last status read that showed IDX
uint32_t g_hdd_cylinders = 1;

// MAME's three address maps (main_map_j / _u / _a) differ only in where the
// tile RAM and the IDE ports are
char g_map = 'j';
uint32_t g_tile_base = 0xc00000, g_ata_base[2] = {0xf00000, 0xf40000};

// MAME's drive times are exact attotimes; its scheduler runs the CPU up to the
// last whole clock at or before a timer, so a busy period ends on that clock.
constexpr uint64_t AS_PER_US = 1'000'000'000'000ull, AS_PER_CYCLE = 62'500'000'000ull;   // 16 MHz
uint64_t as_to_cycles(uint64_t as) { return as / AS_PER_CYCLE; }
constexpr uint64_t TIME_BETWEEN_SECTORS_AS = 400'000'000'000ull;   // 400 ns
constexpr uint64_t MINIMUM_COMMAND_TIME_AS = 10 * AS_PER_US;

bool g_ide_line = false;   // the drive's INTRQ as MAME's ATA interface sees it

void ata_irq_from_bus(bool state) {
    // MAME (atahle.h set_irq, ataintf.cpp) passes the line on only when it changes
    if (state == g_ide_line) return;
    g_ide_line = state;
    if (g_ata_event && now() < g_ata_busy_until) {
        g_ata_irq_deferred = state;
        return;
    }
    ide_interrupt(state);
}

void ata_start_busy(uint64_t cycles) {
    g_ata_busy_until = now() + cycles;
    g_ata_event = true;
    g_ata_irq_deferred = false;
    m68k_end_timeslice();   // let the run loop stop at the end of the busy time
}

void ata_busy_done() {
    g_ata_event = false;
    if (g_ata_irq_deferred) ide_interrupt(true);
    g_ata_irq_deferred = false;
}

uint64_t ata_lba() {
    const uint8_t dev = g_ata_regs[6];
    if (dev & 0x40) return (uint64_t)(dev & 0x0f) << 24 | (uint64_t)g_ata_regs[5] << 16 | (uint64_t)g_ata_regs[4] << 8 | g_ata_regs[3];
    const uint32_t cyl = (uint32_t)g_ata_regs[5] << 8 | g_ata_regs[4];
    return ((uint64_t)cyl * g_hdd_heads + (dev & 0x0f)) * g_hdd_sectors + (g_ata_regs[3] ? g_ata_regs[3] - 1 : 0);
}

// MAME m_cur_lba after a PIO read: one next_sector() for every sector loaded but the last of the
// command. The first loads when the command completes, each next once the game has read the one
// before (fill_buffer()).
void ata_settle_heads() {
    if (!g_ata_pio_read) return;
    g_ata_pio_read = false;
    const uint32_t loaded = std::min<uint32_t>(g_ata_pio_count, 1 + g_ata_pio_words / 256);
    g_ata_cur_lba = g_ata_pio_start + loaded - (loaded == g_ata_pio_count ? 1 : 0);
}

// MAME seek_time(): from the current cylinder to the command's
uint64_t ata_seek_cycles() {
    ata_settle_heads();
    const uint64_t spc = (uint64_t)g_hdd_heads * g_hdd_sectors, lba = ata_lba();
    const int64_t diff = std::llabs((int64_t)(g_ata_cur_lba / spc) - (int64_t)(lba / spc));
    g_ata_cur_lba = lba;
    if (diff == 0) return as_to_cycles(TIME_BETWEEN_SECTORS_AS);
    // TIME_FULL_STROKE_SEEK * diff / cylinders (attotime division: floor), + 1300 us
    const uint64_t us = 13000 * (uint64_t)diff, q = us / g_hdd_cylinders, r = us % g_hdd_cylinders;
    return as_to_cycles(q * AS_PER_US + r * AS_PER_US / g_hdd_cylinders + 1300 * AS_PER_US);
}

void ata_write(int cs, int reg, uint16_t v) {
    if (cs == 0 && reg >= 1 && reg <= 6) g_ata_regs[reg] = (uint8_t)v;
    if (cs == 0 && reg == 7) {
        g_ata_command = (uint8_t)v;
        if (g_cfg.ata_log)
            fprintf(stderr, "djmain ata: frame %llu t %.9f pc %06x command %02x lba %llu count %u\n", (unsigned long long)g_frame,
                    (double)now() / CPU_HZ, m68k_get_reg(nullptr, M68K_REG_PPC), g_ata_command, (unsigned long long)ata_lba(),
                    g_ata_regs[2]);
        switch (g_ata_command) {
        case 0x20: case 0x21: case 0xc4: {
            ata_start_busy(ata_seek_cycles());   // read_first_sector()
            // the heads follow the sectors as the game reads them (ata_settle_heads)
            g_ata_pio_read = true;
            g_ata_pio_start = g_ata_cur_lba;
            g_ata_pio_count = g_ata_regs[2] ? g_ata_regs[2] : 256;
            g_ata_pio_words = 0;
            break;
        }
        case 0xc8: case 0xc9: case 0x40: case 0x41: {
            ata_start_busy(ata_seek_cycles());   // read_first_sector()
            // DMA and VERIFY run through every sector: MAME's next_sector() for all but the last
            const uint32_t count = g_ata_regs[2] ? g_ata_regs[2] : 256;
            g_ata_cur_lba += count - 1;
            break;
        }
        case 0xec: case 0x10: case 0x91: case 0xef: case 0xe7:
            ata_settle_heads();
            ata_start_busy(as_to_cycles(MINIMUM_COMMAND_TIME_AS));
            break;
        default:
            break;   // SET MULTIPLE, IDLE, ...: the interrupt is immediate
        }
    }
    g_ata.write(cs, reg, v);
}

// MAME ide_hdd_device_base::calculate_status(): a status read more than one
// rotation (1/90 s at 5400 rpm) after the last one that showed the index pulse
// (or after power-on) shows it (IDX, bit 1). More than 1/90 s: 177778 clocks.
uint16_t ata_index(uint16_t status) {
    const uint64_t t = now();
    if (t - g_ata_index_since <= 177777) return status;
    g_ata_index_since = t;
    return status | 0x02;
}

uint16_t ata_read(int cs, int reg) {
    const bool status = (cs == 0 && reg == 7) || (cs == 1 && reg == 6);
    if (g_ata_event && now() < g_ata_busy_until) {
        if (cs == 0 && reg == 0) return 0;
        // while busy, MAME answers every register but the data port with the status
        if (cs == 0 || status) return ata_index(0xd0);   // BSY | DRDY | DSC
    }
    const bool irq_before = g_hold1;
    if (cs == 0 && reg == 0 && g_ata_pio_read) g_ata_pio_words++;
    uint16_t v = g_ata.read(cs, reg);
    if (status) v = ata_index(v);
    // READ SECTORS: the next sector's interrupt comes 400 ns after the last word
    if (cs == 0 && reg == 0 && (g_ata_command == 0x20 || g_ata_command == 0x21) && !irq_before && g_hold1) {
        g_hold1 = false;
        irq_changed();
        ata_start_busy(as_to_cycles(TIME_BETWEEN_SECTORS_AS));
        g_ata_irq_deferred = true;
    }
    return v;
}

// ---------------------------------------------------------------------------
// Sound
// ---------------------------------------------------------------------------
void sound_sync() {
    // MAME's stream update: every sample whose time has come
    const uint64_t target = now() * (uint64_t)K054539::RATE / CPU_HZ;
    while (g_snd_done < target) {
        // MAME's sound manager also updates every stream 50 times a second
        // (sound_manager::STREAMS_UPDATE_FREQUENCY): the chips commit there.
        constexpr uint64_t TICK = K054539::RATE / 50;
        const uint64_t tick = (g_snd_done / TICK + 1) * TICK, end = std::min(target, tick);
        const size_t n = (size_t)(end - g_snd_done), at = g_snd48.size();
        g_snd48.resize(at + 2 * n, 0.0f);
        for (auto &c : g_snd) c.render(g_snd48.data() + at, (int)n);
        g_snd_done = end;
        g_stats.sound_samples += n;
        if (end == tick)
            for (auto &c : g_snd) c.commit();
    }
}

// HK_DJ_RAW48=FILE: also write the chips' own 48 kHz output (16-bit stereo,
// headerless), to compare with MAME's -wavwrite sample for sample.
FILE *raw48() {
    static FILE *f = getenv("HK_DJ_RAW48") ? fopen(getenv("HK_DJ_RAW48"), "wb") : nullptr;
    return f;
}

// Hand what has been rendered to the shell, at the chips' own 48 kHz.
void sound_flush() {
    sound_sync();
    if (FILE *f = raw48()) {
        for (float x : g_snd48) {
            const float v = x * 32768.0f;
            const int16_t i = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
            fwrite(&i, 2, 1, f);
        }
        fflush(f);
    }
    if (g_audio && !g_snd48.empty()) g_audio(g_snd48.data(), (int)(g_snd48.size() / 2));
    g_snd48.clear();
}

// ---------------------------------------------------------------------------
// Lamps
// ---------------------------------------------------------------------------
void update_lamps() {
    const auto &defs = cab::lamps(g_cfg.family);
    g_lamps.resize(defs.size());
    bool changed = false;
    for (size_t k = 0; k < defs.size(); k++) {
        uint32_t reg = defs[k].reg == 0 ? g_light_ctrl[0] : defs[k].reg == 1 ? g_light_ctrl[1] : g_pcb_leds;
        uint8_t on = (uint8_t)(((reg >> defs[k].bit) & 1) ^ (defs[k].active_low ? 1 : 0));
        if (g_lamps[k] != on) g_lamps[k] = on, changed = true;
    }
    if (changed) g_lamp_changes++;
}

// ---------------------------------------------------------------------------
// The bus, as MAME's 32-bit big-endian address space for the 68EC020 (main_map_j)
// ---------------------------------------------------------------------------
inline uint32_t be32(const std::vector<uint8_t> &v, uint32_t a) {
    return (uint32_t)v[a] << 24 | (uint32_t)v[a + 1] << 16 | (uint32_t)v[a + 2] << 8 | v[a + 3];
}
inline void put_be32(std::vector<uint8_t> &v, uint32_t a, uint32_t data, uint32_t mask) {
    for (int b = 0; b < 4; b++)
        if (mask >> (24 - 8 * b) & 0xff) v[a + b] = (uint8_t)(data >> (24 - 8 * b));
}

// A 16-bit device on the 32-bit bus: the upper half is offset*2, the lower offset*2+1.
template <class R>
uint32_t read16x2(uint32_t offset, uint32_t mask, R r) {
    uint32_t v = 0;
    if (mask & 0xffff0000u) v |= (uint32_t)r(offset * 2, (uint16_t)(mask >> 16)) << 16;
    if (mask & 0x0000ffffu) v |= r(offset * 2 + 1, (uint16_t)mask);
    return v;
}
template <class W>
void write16x2(uint32_t offset, uint32_t data, uint32_t mask, W w) {
    if (mask & 0xffff0000u) w(offset * 2, (uint16_t)(data >> 16), (uint16_t)(mask >> 16));
    if (mask & 0x0000ffffu) w(offset * 2 + 1, (uint16_t)data, (uint16_t)mask);
}

uint8_t input_r(uint32_t offset) {
    offset = (offset & 3) | ((offset >> 15) & 1) << 2;
    return offset < 6 ? g_in[offset] : 0xff;
}

uint32_t turntable_r(uint32_t mask) {
    uint32_t result = 0;
    if (mask & 0x0000ff00u) {
        const int sel = g_turntable_select;
        const uint8_t pos = g_dial[sel];
        int delta = pos - g_turntable_last_pos[sel];
        if (delta < -128) delta += 256;
        if (delta > 128) delta -= 256;
        g_turntable_pos[sel] = (uint16_t)(g_turntable_pos[sel] + delta * 70);
        g_turntable_last_pos[sel] = pos;
        result |= g_turntable_pos[sel] & 0xff00;
    }
    return result;
}

// HK_DJ_UNMAPPED=1: the first 40 accesses outside the map, with the PC (MAME logs its own unmapped ones)
void log_unmapped(const char *what, uint32_t a, uint32_t data, uint32_t mask) {
    static const bool on = getenv("HK_DJ_UNMAPPED") != nullptr;
    static int n = 0;
    if (on && n++ < 40)
        fprintf(stderr, "unmapped %s %06x = %08x mask %08x at pc %06x\n", what, a, data, mask,
                m68k_get_reg(nullptr, M68K_REG_PPC));
}

uint32_t bus_read32(uint32_t a, uint32_t mask) {
    a &= 0xfffffc;   // 24-bit address bus (68EC020), dword aligned
    if (a < 0x100000) return be32(g_rom, a);
    if (a >= 0x400000 && a < 0x410000) return be32(g_ram, a - 0x400000);
    if (a >= 0x480000 && a < 0x484440) return be32(g_palram, a - 0x480000);
    if (a >= 0x500000 && a < 0x580000) {   // sndram_r: a window into the sample RAM
        uint32_t off = ((a - 0x500000) >> 2) | 0x20000u * g_sndram_bank;
        return be32(g_sndram, (off * 4) & 0xffffff);
    }
    if (a >= 0x580000 && a < 0x580040)
        return read16x2((a - 0x580000) >> 2, mask, [](uint32_t o, uint16_t) { return (uint32_t)g_tiles.word_r(o); });
    if (a >= 0x5b0000 && a < 0x5b0500) {   // K054539 x2: umask 0xff00ff00 / 0x00ff00ff
        sound_sync();
        const uint32_t o = ((a - 0x5b0000) >> 2) * 2;
        uint32_t v = 0;
        if (mask & 0xff000000u) v |= (uint32_t)g_snd[0].read(o) << 24;
        if (mask & 0x00ff0000u) v |= (uint32_t)g_snd[1].read(o) << 16;
        if (mask & 0x0000ff00u) v |= (uint32_t)g_snd[0].read(o + 1) << 8;
        if (mask & 0x000000ffu) v |= g_snd[1].read(o + 1);
        return v;
    }
    if ((a & ~0x8000u) == 0x5c0000) {      // inputs: 0x5c0000-3, select bit 15
        uint32_t v = 0;
        for (int b = 0; b < 4; b++)
            if (mask >> (24 - 8 * b) & 0xff) v |= (uint32_t)input_r((a - 0x5c0000) + (uint32_t)b) << (24 - 8 * b);
        return v;
    }
    if (a == 0x5e0000) return turntable_r(mask);
    if (a >= 0x600000 && a < 0x602000) {   // v_rom_r: tile ROM readthrough (POST)
        const int bank = g_tiles.word_r(0x34 / 2) & 0xff;
        uint32_t offset = ((a - 0x600000) >> 2) * 2;
        if (!(mask & 0xff000000u)) offset += 1;
        offset += (uint32_t)bank * 0x800 * 4;
        if (g_v_ctrl & 0x020) offset += 0x800 * 2;
        return g_tilerom.empty() ? 0 : (uint32_t)g_tilerom[offset % g_tilerom.size()] * 0x01010000u;
    }
    if (a >= 0x801000 && a < 0x801800) return be32(g_objram, a - 0x801000);
    if (a >= 0x803000 && a < 0x8030a0) return g_obj_regs[(a - 0x803000) >> 2];
    if (a >= 0x803800 && a < 0x804000) {   // obj_rom_r: sprite ROM readthrough (POST)
        const int bank = g_obj_regs[0x28 / 4] >> 16 & 0x3ff;
        uint32_t offset = ((a - 0x803800) >> 2) + (uint32_t)bank * 0x200;
        offset *= 4;
        if (mask & 0x0000ffffu) offset += 2;
        if (mask & 0xff00ff00u) offset++;
        return g_objrom.empty() ? 0 : (uint32_t)g_objrom[offset % g_objrom.size()] * 0x01010101u;
    }
    if (a - g_tile_base < 0x2000)
        return read16x2((a - g_tile_base) >> 2, mask, [](uint32_t o, uint16_t) { return (uint32_t)g_tiles.ram_word_r(o); });
    if (a - g_ata_base[0] < 0x10)
        return read16x2((a - g_ata_base[0]) >> 2, mask, [](uint32_t o, uint16_t) { return (uint32_t)ata_read(0, (int)o); });
    if (a - g_ata_base[1] < 0x10)
        return read16x2((a - g_ata_base[1]) >> 2, mask, [](uint32_t o, uint16_t) { return (uint32_t)ata_read(1, (int)o); });
    g_stats.unmapped++;
    log_unmapped("read", a, 0, mask);
    return 0;
}

void bus_write32(uint32_t a, uint32_t data, uint32_t mask) {
    a &= 0xfffffc;
    if (a >= 0x400000 && a < 0x410000) return put_be32(g_ram, a - 0x400000, data, mask);
    if (a >= 0x480000 && a < 0x484440) return put_be32(g_palram, a - 0x480000, data, mask);
    if (a >= 0x500000 && a < 0x580000) {
        sound_sync();   // the chips play from this RAM
        uint32_t off = ((a - 0x500000) >> 2) | 0x20000u * g_sndram_bank;
        return put_be32(g_sndram, (off * 4) & 0xffffff, data, mask);
    }
    if (a >= 0x580000 && a < 0x580040)
        return write16x2((a - 0x580000) >> 2, data, mask, [](uint32_t o, uint16_t d, uint16_t m) { g_tiles.word_w(o, d, m); });
    if (a >= 0x590000 && a < 0x590008) return;   // unknown (M66011FP?)
    if (a >= 0x5a0000 && a < 0x5a0060) {          // K055555_long_w: bytes 31-24 and 15-8
        const uint32_t offset = (a - 0x5a0000) >> 2;
        if (mask & 0xff000000u) g_k055555[(offset << 1) % 48] = (uint8_t)(data >> 24);
        if (mask & 0x0000ff00u) g_k055555[((offset << 1) + 1) % 48] = (uint8_t)(data >> 8);
        return;
    }
    if (a >= 0x5b0000 && a < 0x5b0500) {
        sound_sync();
        const uint32_t o = ((a - 0x5b0000) >> 2) * 2;
        if (mask & 0xff000000u) g_snd[0].write(o, (uint8_t)(data >> 24));
        if (mask & 0x00ff0000u) g_snd[1].write(o, (uint8_t)(data >> 16));
        if (mask & 0x0000ff00u) g_snd[0].write(o + 1, (uint8_t)(data >> 8));
        if (mask & 0x000000ffu) g_snd[1].write(o + 1, (uint8_t)data);
        return;
    }
    if (a == 0x5d0000 || a == 0x5d2000) {         // light_ctrl_1_w / _2_w (upper half)
        if (mask & 0xffff0000u) {
            uint16_t &r = g_light_ctrl[a == 0x5d2000];
            const uint16_t m = (uint16_t)(mask >> 16);
            r = (uint16_t)((r & ~m) | ((data >> 16) & m));
            update_lamps();
        }
        return;
    }
    if (a == 0x5d4000) {                          // v_ctrl_w
        if (mask & 0xffff0000u) {
            const uint16_t m = (uint16_t)(mask >> 16);
            g_v_ctrl = (uint16_t)((g_v_ctrl & ~m) | ((data >> 16) & m));
            if ((g_v_ctrl & 0x8000) && g_pending_vb_int) {
                g_pending_vb_int = 0;
                g_hold4 = true;
                g_stats.irq4++;
                irq_changed();
            }
        }
        return;
    }
    if (a == 0x5d6000) {
        if (mask & 0xff000000u) { g_pcb_leds = (uint8_t)(data >> 24); update_lamps(); }   // pcb_leds_w
        if (mask & 0x00ff0000u) g_sndram_bank = (uint8_t)(data >> 16) & 0x1f;            // sndram_bank_w
        return;
    }
    if (a == 0x5e0000) {                          // turntable_select_w
        if (mask & 0x00ff0000u) g_turntable_select = (data >> 19) & 1;
        return;
    }
    if (a >= 0x801000 && a < 0x801800) return put_be32(g_objram, a - 0x801000, data, mask);
    if (a >= 0x802000 && a < 0x803000) return;   // unknown
    if (a >= 0x803000 && a < 0x8030a0) {
        uint32_t &r = g_obj_regs[(a - 0x803000) >> 2];
        r = (r & ~mask) | (data & mask);
        return;
    }
    if (a - g_tile_base < 0x2000)
        return write16x2((a - g_tile_base) >> 2, data, mask, [](uint32_t o, uint16_t d, uint16_t m) { g_tiles.ram_word_w(o, d, m); });
    if (g_map == 'j' && a >= 0xc02000 && a < 0xc02048) return;   // unknown (main_map_j only)
    if (a - g_ata_base[0] < 0x10)
        return write16x2((a - g_ata_base[0]) >> 2, data, mask, [](uint32_t o, uint16_t d, uint16_t) { ata_write(0, (int)o, d); });
    if (a - g_ata_base[1] < 0x10)
        return write16x2((a - g_ata_base[1]) >> 2, data, mask, [](uint32_t o, uint16_t d, uint16_t) { ata_write(1, (int)o, d); });
    g_stats.unmapped++;
    log_unmapped("write", a, data, mask);
}

// Byte lanes of address a within its dword (big-endian)
inline uint32_t lane8(uint32_t a) { return 0xff000000u >> (8 * (a & 3)); }

uint32_t read8(uint32_t a) { return bus_read32(a, lane8(a)) >> (24 - 8 * (a & 3)) & 0xff; }
uint32_t read16(uint32_t a) {
    if (a & 1) return read8(a) << 8 | read8(a + 1);   // MAME splits unaligned words into bytes
    const uint32_t m = (a & 2) ? 0x0000ffffu : 0xffff0000u;
    return bus_read32(a, m) >> ((a & 2) ? 0 : 16) & 0xffff;
}
uint32_t read32(uint32_t a) {
    if ((a & 3) == 0) return bus_read32(a, 0xffffffffu);
    if ((a & 1) == 0) return read16(a) << 16 | read16(a + 2);
    return read8(a) << 24 | read16(a + 1) << 8 | read8(a + 3);
}
void write8(uint32_t a, uint32_t v) { bus_write32(a, (v & 0xff) << (24 - 8 * (a & 3)), lane8(a)); }
void write16(uint32_t a, uint32_t v) {
    if (a & 1) { write8(a, v >> 8); write8(a + 1, v); return; }
    if (a & 2) bus_write32(a, v & 0xffff, 0x0000ffffu);
    else bus_write32(a, (v & 0xffff) << 16, 0xffff0000u);
}
void write32(uint32_t a, uint32_t v) {
    if ((a & 3) == 0) return bus_write32(a, v, 0xffffffffu);
    if ((a & 1) == 0) { write16(a, v >> 16); write16(a + 2, v); return; }
    write8(a, v >> 24); write16(a + 1, v >> 8); write8(a + 3, v);
}

// ---------------------------------------------------------------------------
// Video: MAME djmain_state::screen_update()
// ---------------------------------------------------------------------------
constexpr int NPENS = 0x4440 / 4;
uint32_t g_pens[NPENS];

void update_pens() {
    // palette_device::xBGR_888: red in bits 0-7, green 8-15, blue 16-23
    for (int i = 0; i < NPENS; i++) {
        uint32_t v = be32(g_palram, (uint32_t)i * 4);
        g_pens[i] = 0xff000000u | (v & 0xff) << 16 | (v & 0xff00) | (v >> 16 & 0xff);
    }
}

// MAME gfx_element::transpen / zoom_transpen (drawgfxzoom_core), pen 0 transparent
void draw_sprite(int code, int color, int colorbase, bool flipx, bool flipy, int sx, int sy, uint32_t scalex, uint32_t scaley) {
    const uint8_t *src = &g_sprite_pens[(size_t)((uint32_t)code % g_sprite_count) * 256];
    const int base = colorbase + 16 * (color % (NPENS / 16));
    auto put = [&](uint32_t *d, uint8_t pen) {
        if (pen && base + pen < NPENS) *d = g_pens[base + pen];
    };
    if (scalex == 0x10000 && scaley == 0x10000) {   // drawgfx_core
        for (int y = 0; y < 16; y++) {
            const int dy = sy + y;
            if (dy < 0 || dy >= SCREEN_H) continue;
            const uint8_t *row = src + (flipy ? 15 - y : y) * 16;
            for (int x = 0; x < 16; x++) {
                const int dx = sx + x;
                if (dx < 0 || dx >= SCREEN_W) continue;
                put(&g_screen[(size_t)dy * SCREEN_W + (size_t)dx], row[flipx ? 15 - x : x]);
            }
        }
        return;
    }
    const uint32_t dstwidth = (scalex * 16 + 0x8000) >> 16, dstheight = (scaley * 16 + 0x8000) >> 16;
    if (dstwidth < 1 || dstheight < 1) return;
    int32_t dx = (int32_t)((16u << 16) / dstwidth), dy = (int32_t)((16u << 16) / dstheight);
    int32_t destx = sx, desty = sy;
    int32_t destendx = destx + (int32_t)dstwidth - 1;
    if (destx > SCREEN_W - 1 || destendx < 0) return;
    int32_t srcx = 0;
    if (destx < 0) { srcx = (0 - destx) * dx; destx = 0; }
    if (destendx > SCREEN_W - 1) destendx = SCREEN_W - 1;
    int32_t destendy = desty + (int32_t)dstheight - 1;
    if (desty > SCREEN_H - 1 || destendy < 0) return;
    int32_t srcy = 0;
    if (desty < 0) { srcy = (0 - desty) * dy; desty = 0; }
    if (destendy > SCREEN_H - 1) destendy = SCREEN_H - 1;
    if (flipx) { srcx = (int32_t)(dstwidth - 1) * dx - srcx; dx = -dx; }
    if (flipy) { srcy = (int32_t)(dstheight - 1) * dy - srcy; dy = -dy; }
    for (int32_t cury = desty; cury <= destendy; cury++) {
        uint32_t *d = &g_screen[(size_t)cury * SCREEN_W + (size_t)destx];
        const uint8_t *row = src + (srcy >> 16) * 16;
        int32_t cursrcx = srcx;
        srcy += dy;
        for (int32_t curx = destx; curx <= destendx; curx++, d++) {
            put(d, row[cursrcx >> 16]);
            cursrcx += dx;
        }
    }
}

void draw_sprites() {
    constexpr int NUM_SPRITES = 128;
    const int colorbase = g_k055555[29] * 0x400;   // K55_PALBASE_SUB2
    int sortedlist[NUM_SPRITES];
    for (int &s : sortedlist) s = -1;
    auto obj = [](int i) { return be32(g_objram, (uint32_t)i * 4); };
    for (int offs = 0; offs < NUM_SPRITES * 4; offs += 4)
        if (obj(offs) & 0x00008000) {
            if (obj(offs) & 0x80000000) continue;
            sortedlist[obj(offs) & (NUM_SPRITES - 1)] = offs;
        }
    static const int xoffset[8] = {0, 1, 4, 5, 16, 17, 20, 21};
    static const int yoffset[8] = {0, 2, 8, 10, 32, 34, 40, 42};
    static const int sizetab[4] = {1, 2, 4, 8};
    for (int pri_code = NUM_SPRITES - 1; pri_code >= 0; pri_code--) {
        const int offs = sortedlist[pri_code];
        if (offs == -1) continue;
        const int code = (int)(obj(offs) >> 16);
        const bool flipx = obj(offs) >> 10 & 1, flipy = obj(offs) >> 11 & 1;
        const int size = sizetab[(obj(offs) >> 8) & 3];
        int ox = (int16_t)(obj(offs + 1) & 0xffff);
        int oy = (int16_t)(obj(offs + 1) >> 16);
        int xscale = (int)(obj(offs + 2) >> 16), yscale = (int)(obj(offs + 2) & 0xffff);
        if (!xscale || !yscale) continue;
        xscale = (0x40 << 16) / xscale;
        yscale = (0x40 << 16) / yscale;
        ox -= (size * xscale) >> 13;
        oy -= (size * yscale) >> 13;
        const int color = (int)(obj(offs + 3) >> 16) & 15;
        for (int x = 0; x < size; x++)
            for (int y = 0; y < size; y++) {
                int c = code + (flipx ? xoffset[size - x - 1] : xoffset[x]) + (flipy ? yoffset[size - y - 1] : yoffset[y]);
                if (xscale != 0x10000 || yscale != 0x10000) {
                    const int sx = ox + ((x * xscale + (1 << 11)) >> 12);
                    const int sy = oy + ((y * yscale + (1 << 11)) >> 12);
                    const int zw = ox + (((x + 1) * xscale + (1 << 11)) >> 12) - sx;
                    const int zh = oy + (((y + 1) * yscale + (1 << 11)) >> 12) - sy;
                    draw_sprite(c, color, colorbase, flipx, flipy, sx, sy, (uint32_t)(zw << 12), (uint32_t)(zh << 12));
                } else {
                    draw_sprite(c, color, colorbase, flipx, flipy, ox + (x << 4), oy + (y << 4), 0x10000, 0x10000);
                }
            }
    }
}

void screen_update() {
    prof::Scope ps(prof::DRAW);
    update_pens();
    const int enables = g_k055555[45];   // K55_INPUT_ENABLES
    int pri[3] = {g_k055555[7], g_k055555[10], g_k055555[17]};   // K55_PRIINP_0, _3, _10
    int order[3] = {0, 1, 2};
    for (int i = 0; i < 2; i++)
        for (int j = i + 1; j < 3; j++)
            if (pri[order[i]] > pri[order[j]]) std::swap(order[i], order[j]);
    std::fill(g_screen.begin(), g_screen.end(), g_pens[0]);
    for (int i = 0; i < 3; i++) {
        const int layer = order[i];
        if (layer == 2) {
            if (enables & 0x40) draw_sprites();   // K55_INP_SUB2
        } else if (enables & (0x01 << layer)) {
            g_tiles.draw_dj(layer, g_screen.data(), SCREEN_W, SCREEN_H, g_pens, NPENS);
        }
    }
}

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------
bool read_file(const std::string &path, std::vector<uint8_t> &out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), {});
    return true;
}

// MAME ROM_LOAD16_BYTE pairs: file 2k at even bytes, file 2k+1 at odd bytes
bool load_interleaved(const std::vector<std::string> &files, std::vector<uint8_t> &region, const char *what) {
    region.clear();
    for (size_t k = 0; k + 1 < files.size(); k += 2) {
        std::vector<uint8_t> even, odd;
        if (!read_file(g_cfg.rom_dir + "/" + files[k], even) || !read_file(g_cfg.rom_dir + "/" + files[k + 1], odd) ||
            even.size() != odd.size()) {
            fprintf(stderr, "djmain: cannot load the %s ROMs %s / %s from %s\n", what, files[k].c_str(), files[k + 1].c_str(),
                    g_cfg.rom_dir.c_str());
            return false;
        }
        const size_t at = region.size();
        region.resize(at + 2 * even.size());
        for (size_t i = 0; i < even.size(); i++) region[at + 2 * i] = even[i], region[at + 2 * i + 1] = odd[i];
    }
    return !region.empty();
}

void decode_sprites() {
    // MAME gfx_8x8x4_row_2x2_group_packed_lsb: 16x16, 128 bytes, low nibble first;
    // the four 8x8 quarters are left/right 32 bytes apart, top/bottom 64 apart
    g_sprite_count = (uint32_t)(g_objrom.size() / 128);
    g_sprite_pens.assign((size_t)g_sprite_count * 256, 0);
    for (uint32_t t = 0; t < g_sprite_count; t++)
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++) {
                const size_t byte = (size_t)t * 128 + (y < 8 ? (size_t)y * 4 : 64 + (size_t)(y - 8) * 4) + (x < 8 ? 0 : 32) +
                                    (size_t)((x & 7) >> 1);
                const uint8_t b = g_objrom[byte];
                g_sprite_pens[(size_t)t * 256 + (size_t)y * 16 + (size_t)x] = (x & 1) ? b >> 4 : b & 0x0f;
            }
}

}  // namespace

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------
bool init(const Config &cfg) {
    g_cfg = cfg;
    g_map = cfg.map.empty() ? 'j' : cfg.map[0];
    switch (g_map) {
    case 'j': g_tile_base = 0xc00000; g_ata_base[0] = 0xf00000; g_ata_base[1] = 0xf40000; break;
    case 'u': g_tile_base = 0xe00000; g_ata_base[0] = 0xd00000; g_ata_base[1] = 0xd40000; break;
    case 'a': g_tile_base = 0xf00000; g_ata_base[0] = 0xc00000; g_ata_base[1] = 0xc40000; break;
    default: fprintf(stderr, "djmain: unknown address map '%s' (j, u or a)\n", cfg.map.c_str()); return false;
    }
    std::vector<uint8_t> prog;
    if (!load_interleaved(cfg.program, prog, "program")) return false;
    std::copy(prog.begin(), prog.begin() + (ptrdiff_t)std::min(prog.size(), g_rom.size()), g_rom.begin());
    if (!load_interleaved(cfg.sprites, g_objrom, "sprite")) return false;
    if (!load_interleaved(cfg.tiles, g_tilerom, "tile")) return false;
    decode_sprites();
    g_tiles.init(g_tilerom);
    // MAME video_start()
    g_tiles.set_layer_offs(0, -92, -27);
    g_tiles.set_layer_offs(1, -88, -27);
    for (auto &c : g_snd) c.set_rom(g_sndram.data(), (uint32_t)g_sndram.size() - 1);

    auto disk = std::make_unique<AtaDevice>("hdd", false);
    disk->disk = std::make_unique<DiskImage>();
    if (!disk->disk->open(cfg.hdd)) {
        fprintf(stderr, "djmain: cannot open the hard disk image '%s'\n", cfg.hdd.c_str());
        return false;
    }
    g_ata_disk_sectors = disk->disk->sectors();
    // MAME's drive takes the geometry from the CHD; its cylinders set the seek times
    g_hdd_heads = cfg.hdd_chs[1] ? cfg.hdd_chs[1] : 16;
    g_hdd_sectors = cfg.hdd_chs[2] ? cfg.hdd_chs[2] : 63;
    disk->disk->set_geometry(cfg.hdd_chs[0], g_hdd_heads, g_hdd_sectors);
    disk->master_password = cfg.ata_master_password;   // MAME init_<game>(): complete MIX and later
    disk->user_password = cfg.ata_user_password;
    g_ata.attach(0, std::move(disk));
    g_ata.log = cfg.ata_log;
    g_ata.pio_end_irq = false;   // MAME's drive
    g_ata.irq = ata_irq_from_bus;
    g_hdd_cylinders = std::max<uint32_t>(1, cfg.hdd_chs[0] ? cfg.hdd_chs[0] :
                                                std::min<uint32_t>(g_ata_disk_sectors / (g_hdd_heads * g_hdd_sectors), 16383));
    update_lamps();

    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68EC020);
    m68k_pulse_reset();
    return true;
}

void set_frame_hook(FrameHook h) { g_frame_hook = std::move(h); }
void set_present_hook(PresentHook h) { g_present_hook = std::move(h); }
void frame_picture_unused() { g_frame_picture = false; }
void set_audio_sink(AudioSink s) { g_audio = std::move(s); }

void run() {
    // MAME: the vblank interrupt at line 384 of each frame; the frame (screen
    // update, Lua frame notifier, snapshot) completes at the start of the next one.
    auto run_until = [](uint64_t t) {
        while (g_cycles < t) {
            uint64_t stop = t;
            if (g_ata_event) stop = std::min(stop, std::max(g_ata_busy_until, g_cycles + 1));
            g_in_execute = true;
            int ran = m68k_execute((int)(stop - g_cycles));
            g_in_execute = false;
            g_cycles += (uint64_t)ran;
            if (g_ata_event && g_cycles >= g_ata_busy_until) ata_busy_done();
        }
    };
    // The vertical blank: the 34 lines after the 384 visible ones (418 in all). At its end the
    // monitor starts showing the picture as the game left it (its vblank interrupt may flip
    // pages and scroll): that is the window's picture (set_present_hook). The run stops there in
    // any case, so a run with the window goes as one without.
    constexpr uint64_t VBLANK_CYCLES = 34 * 660;
    for (;;) {
        run_until(g_frame_start + g_frame_cycles);
        g_frame_start += g_frame_cycles;
        g_frame_cycles = g_want_frame_cycles;   // a new rate from this frame on
        g_frame++;
        vb_interrupt();
        // the picture as vertical blank starts, for the frame hook: not composed when nobody looks
        // at it (frame_picture_unused: the window shows the one below; a quarter of a millisecond
        // a frame for nothing), the hook then given its size only
        const bool picture = g_frame_picture || !g_present_hook;
        if (picture) screen_update();
        if (!g_present_hook) keep_shown_sprites();
        sound_flush();
        if (g_frame_hook) g_frame_hook(g_frame, picture ? g_screen.data() : nullptr, SCREEN_W, SCREEN_H);
        hk_dj_itrace_frame(g_frame);
        run_until(g_frame_start + VBLANK_CYCLES);
        if (g_present_hook) {
            screen_update();
            keep_shown_sprites();
            g_present_hook(g_screen.data(), SCREEN_W, SCREEN_H);
        }
    }
}

uint64_t cycles() { return now(); }
uint64_t frame_count() { return g_frame; }

// In-between pictures (a display faster than the board): the last frame, each sprite that moved
// since the frame before drawn `t` of a frame further along that move. The game rebuilds its
// sprite list every frame, slots in another order (pop'n music: a note's slot changes as notes
// come and go), so a sprite is matched to one of the frame before with the same tile, the scroll
// voted first (hiraku/scroll_guess.h, pair: what is fast may be further than 16 pixels on);
// one without a match, or that hasn't moved, stays where it is. The tilemaps are as they are. A
// sprite that stops or turns overshoots for less than a frame. What scrolls with the frame's other
// moving sprites goes along the scroll's steady line (hiraku/scroll_guess.h): notes step whole
// pixels, and carried on by their last step they judder.
hk::ScrollGuess g_scroll;
const uint32_t *render_between(double t) {
    if (g_shown_objram.size() != g_objram.size() || g_prev_objram.size() != g_objram.size()) return nullptr;
    struct Spr { int code, x, y; };
    auto list = [](const std::vector<uint8_t> &ram) {
        std::vector<Spr> v(128, Spr{-1, 0, 0});
        for (int i = 0; i < 128; i++) {
            const uint32_t w0 = be32(ram, (uint32_t)i * 16), w1 = be32(ram, (uint32_t)i * 16 + 4);
            if ((w0 & 0x00008000) && !(w0 & 0x80000000)) v[i] = {(int)(w0 >> 16), (int16_t)(w1 & 0xffff), (int16_t)(w1 >> 16)};
        }
        return v;
    };
    const std::vector<Spr> cur = list(g_shown_objram), prev = list(g_prev_objram);
    std::vector<uint8_t> moved = g_shown_objram;
    std::vector<std::pair<int, int>> moves(128, {0, 0});
    std::vector<hk::ScrollGuess::Candidate> cands;
    for (int i = 0; i < 128; i++) {
        const Spr &c = cur[i];
        if (c.code < 0) continue;
        for (int j = 0; j < 128; j++) {
            const Spr &p = prev[j];
            if (p.code != c.code || std::abs(c.x - p.x) > hk::ScrollGuess::REACH || std::abs(c.y - p.y) > hk::ScrollGuess::REACH) continue;
            cands.push_back({i, j, c.x - p.x, c.y - p.y, 0, true});
        }
    }
    const std::vector<int> paired = g_scroll.pair(g_frame, 128, cands, true);
    std::vector<int> from(128, -1);   // each sprite's own track: its slot in the frame before
    for (int i = 0; i < 128; i++)
        if (paired[i] >= 0) moves[i] = {cands[(size_t)paired[i]].dx, cands[(size_t)paired[i]].dy}, from[i] = cands[(size_t)paired[i]].from;
    g_scroll.frame(g_frame, moves, from);
    for (int i = 0; i < 128; i++) {
        if (!moves[i].first && !moves[i].second) continue;
        int ox, oy;
        g_scroll.offset(moves[i].first, moves[i].second, t, ox, oy, i);
        if (!ox && !oy) continue;
        const int x = cur[i].x + ox, y = cur[i].y + oy;
        put_be32(moved, (uint32_t)i * 16 + 4, (uint32_t)(uint16_t)x | (uint32_t)(uint16_t)y << 16, 0xffffffffu);
    }
    // drawn as screen_update() draws, into g_between, the machine's own state untouched
    g_objram.swap(moved);
    g_screen.swap(g_between);
    screen_update();
    g_screen.swap(g_between);
    g_objram.swap(moved);
    return g_between.data();
}
void set_frame_rate(double hz) {
    g_want_frame_cycles = hz > 0 ? std::max<uint64_t>(VBLANK_FLOOR, (uint64_t)(CPU_HZ / hz + 0.5)) : FRAME_CYCLES;
}
const std::string &family() { return g_cfg.family; }
const Stats &stats() { return g_stats; }
const std::vector<uint8_t> &lamp_state() { return g_lamps; }
uint64_t lamp_changes() { return g_lamp_changes; }
uint32_t pc() { return m68k_get_reg(nullptr, M68K_REG_PC); }

const uint8_t *mem(uint32_t addr, uint32_t len) {
    auto in = [&](uint32_t lo, const std::vector<uint8_t> &v) {
        return addr >= lo && addr - lo <= v.size() && len <= v.size() - (addr - lo) ? v.data() + (addr - lo) : nullptr;
    };
    if (auto p = in(0x000000, g_rom)) return p;
    if (auto p = in(0x400000, g_ram)) return p;
    if (auto p = in(0x480000, g_palram)) return p;
    if (auto p = in(0x801000, g_objram)) return p;
    return nullptr;
}

bool set_dip(const std::string &bank, uint8_t value) {
    const int port = bank == "DSW1" ? 4 : bank == "DSW2" ? 5 : bank == "DSW3" ? 0 : -1;   // input_r's order
    if (port < 0) return false;
    g_in[port] = value;
    return true;
}

bool set_input(const std::string &name, int value) {
    struct Bit { const char *name; int port, mask; };
    // ports: 0 DSW3, 1 BTN3, 2 BTN2, 3 BTN1, 4 DSW1, 5 DSW2
    static const Bit common[] = {{"Test", 1, 0x01}, {"Reset", 1, 0x02}, {"Service", 1, 0x04}, {"Coin 1", 2, 0x80}};
    static const Bit bm[] = {{"P1 Button 1", 3, 0x10}, {"P1 Button 2", 3, 0x08}, {"P1 Button 3", 3, 0x04},
                             {"P1 Button 4", 3, 0x02}, {"P1 Button 5", 3, 0x01}, {"P2 Button 1", 3, 0x40},
                             {"P2 Button 2", 3, 0x80}, {"P2 Button 3", 2, 0x01}, {"P2 Button 4", 2, 0x02},
                             {"P2 Button 5", 2, 0x04}, {"1 Player Start", 2, 0x10}, {"2 Players Start", 2, 0x20},
                             {"Effect", 2, 0x40}};
    static const Bit popn[] = {{"P1 Button 1", 3, 0x10}, {"P1 Button 2", 3, 0x08}, {"P1 Button 3", 3, 0x04},
                               {"P1 Button 4", 3, 0x02}, {"P1 Button 5", 3, 0x01}, {"P1 Button 6", 3, 0x40},
                               {"P1 Button 7", 3, 0x80}, {"P1 Button 8", 2, 0x01}, {"P1 Button 9", 2, 0x02}};
    auto try_set = [&](const Bit *b, size_t n) {
        for (size_t k = 0; k < n; k++)
            if (name == b[k].name) {
                if (value) g_in[b[k].port] &= (uint8_t)~b[k].mask;
                else g_in[b[k].port] |= (uint8_t)b[k].mask;
                return true;
            }
        return false;
    };
    // pop'n stage (MAME popnstage): popn's nine, a tenth, and three selection buttons read as starts
    static const Bit pstage[] = {{"P1 Button 10", 2, 0x04}, {"1 Player Start", 2, 0x10}, {"2 Players Start", 2, 0x20},
                                 {"3 Players Start", 2, 0x40}};
    if (try_set(common, std::size(common))) return true;
    if (g_cfg.family == "djpopn") return try_set(popn, std::size(popn));
    if (g_cfg.family == "djpstage") return try_set(popn, std::size(popn)) || try_set(pstage, std::size(pstage));
    if (try_set(bm, std::size(bm))) return true;
    // MAME's dials: "TT1/Dial" (absolute 8-bit position), as the oracle scripts set them
    if (name == "TT1/Dial" || name == "TT2/Dial") {
        g_dial[name[2] - '1'] = (uint8_t)value;
        return true;
    }
    return false;
}

// Host turntables arrive in Firebeat units (1024 counts a turn); MAME's dial
// port has 256. (MAME then scales each dial step by 70 into the board's counter.)
void turntable_add(int player, int delta) {
    if (player < 0 || player > 1 || g_cfg.family != "djbm") return;
    g_dial_frac[player] += delta;
    const int whole = g_dial_frac[player] / 4;
    g_dial_frac[player] -= whole * 4;
    g_dial[player] = (uint8_t)(g_dial[player] + whole);
}

}  // namespace hk::dj

// Musashi: memory and interrupt acknowledge
extern "C" {
unsigned int m68k_read_memory_8(unsigned int a) { return hk::dj::read8(a); }
unsigned int m68k_read_memory_16(unsigned int a) { return hk::dj::read16(a); }
unsigned int m68k_read_memory_32(unsigned int a) { return hk::dj::read32(a); }
void m68k_write_memory_8(unsigned int a, unsigned int v) { hk::dj::write8(a, v); }
void m68k_write_memory_16(unsigned int a, unsigned int v) { hk::dj::write16(a, v); }
void m68k_write_memory_32(unsigned int a, unsigned int v) { hk::dj::write32(a, v); }
unsigned int m68k_read_disassembler_8(unsigned int a) { return hk::dj::read8(a); }
unsigned int m68k_read_disassembler_16(unsigned int a) { return hk::dj::read16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return hk::dj::read32(a); }

// HK_DJ_ITRACE=FROM,TO,FILE: every instruction run from frame FROM's end to frame TO's, as
// "PC: disassembly" lines, to compare with MAME's debugger trace (oracle .his `itrace on|off`)
int hk_dj_itrace_on = 0;
static FILE *g_itrace_file = nullptr;

void hk_dj_itrace(unsigned int pc) {
    char buf[128];
    m68k_disassemble(buf, pc, M68K_CPU_TYPE_68EC020);
    fprintf(g_itrace_file, "%llu %06X: %s\n", (unsigned long long)(hk::dj::g_cycles + m68k_cycles_run()), pc, buf);
}

// at each frame's end: start or stop the trace
void hk_dj_itrace_frame(uint64_t frame) {
    static long from = -1, to = -1;
    static bool parsed = false;
    if (!parsed) {
        parsed = true;
        if (const char *e = getenv("HK_DJ_ITRACE")) {
            char file[512] = {};
            if (sscanf(e, "%ld,%ld,%511s", &from, &to, file) == 3) g_itrace_file = fopen(file, "w");
        }
    }
    if (!g_itrace_file) return;
    hk_dj_itrace_on = (long)frame >= from && (long)frame < to;
    if ((long)frame == to) fflush(g_itrace_file);
}

int hk_dj_int_ack(int level) {
    // HOLD_LINE: the acknowledge clears the line
    if (level == 4) hk::dj::g_hold4 = false;
    if (level == 1) hk::dj::g_hold1 = false;
    hk::dj::update_irq();
    return (int)M68K_INT_ACK_AUTOVECTOR;
}
}
