// SPDX-License-Identifier: GPL-3.0-or-later
// Konami Firebeat board. Address map, port semantics, the DS1991 dongle
// protocol and the interrupt wiring follow MAME's firebeat.cpp
// (license BSD-3-Clause, copyright-holders: Ville Linde); this is a
// re-implementation for HIRAKU's runtime, not a copy of MAME's device model.
#include "board.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <fstream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "../core/cpu403.h"
#include "../core/prof.h"
#include "../core/runtime.h"
#include "../core/sched.h"
#include "../dev/ata.h"
#include "../dev/fdc.h"
#include "../dev/flash29f016a.h"
#include "../dev/k057714.h"
#include "../dev/rtc65271.h"
#include "../dev/ymz280b.h"
#include "../spu/spu.h"
#include "../shell/cabinet.h"
#include "../shell/saves.h"
#include "../shell/sys.h"
#include "spectrum.h"
#include "synth.h"

namespace hk::fb {

namespace {

Config g_cfg;
Inputs g_in;
K057714 *g_gcu = nullptr;
K057714 *g_gcu2 = nullptr;          // Keyboardmania: the right screen's GCU
AtaBus *g_ata = nullptr;
Rtc65271 *g_rtc = nullptr;
Flash29F016A *g_flash = nullptr;   // [3]: main, snd1, snd2
Fdc37c665 *g_fdc = nullptr;        // beatmania III extend board
Ymz280b *g_ymz = nullptr;
uint64_t g_last_input = 0;
uint8_t g_dpram[0x400];
uint8_t g_ext_irq_enable = 0x3f, g_ext_irq_active = 0;
uint8_t g_control = 0;
uint32_t g_cabinet_info = 0;
FrameHook g_frame_hook;
PresentHook g_present_hook;
bool g_frame_picture = true;   // the frame hook's picture is composed (frame_picture_unused)
uint64_t g_frame = 0;
std::vector<uint32_t> g_fb;

// ---- DS1991 iButton on the 403 serial port (MAME firebeat.cpp) ---------------
struct IButton {
    uint8_t subkey[3][64]{};   // 8 id + 8 password + 48 data each
    int state = 0, ptr = 0;
    uint8_t buf[0x40]{};
} g_ib;

void security_w(uint8_t data) {
    int r = -1;
    if (g_ib.state == 0) {
        switch (data) {
        case 0xc1: r = 0xcd; break;                      // DS2480B reset
        case 0xe1: case 0xe3: break;                     // DS2480B data / command mode
        case 0x66: r = 0x66; g_ib.state = 1; g_ib.ptr = 0; break;   // DS1991 read subkey
        case 0xcc: r = 0xcc; break;                      // skip ROM
        default: fatal(nullptr, "ibutton: unknown command %02x", data);
        }
    } else {
        if (g_ib.ptr == 0) {
            int sk = (data >> 6) & 3;
            r = data;
            if (sk < 3) memcpy(g_ib.buf, g_ib.subkey[sk], 0x40);
            else memset(g_ib.buf, 0, 0x40);
        } else if (g_ib.ptr == 1) {
            r = data;
        } else {
            r = g_ib.buf[g_ib.ptr - 2];
        }
        if (++g_ib.ptr >= 0x42) g_ib.state = 0;
    }
    if (r >= 0) cpu403::spu_receive((uint8_t)r);
}

// ---- 16550 UARTs ---------------------------------------------------------------
// Nothing is attached (the ST-224 effector is not emulated, as in MAME), so a
// transmitted byte simply leaves after one character time; the transmitter-
// empty interrupt is modelled because the game enables and services it.
struct Uart {
    uint8_t ier = 0, lcr = 0, mcr = 0, scr = 0, dll = 0, dlm = 0, fcr = 0;
    bool thre_int = false;      // transmitter-empty interrupt pending
    bool thr_empty = true;      // holding register
    bool tsr_busy = false;      // shift register sending a byte
    bool overrun = false;
    std::deque<uint8_t> rx;     // receive FIFO (16 bytes with FIFOs enabled, else 1)
    std::function<void(bool)> irq;
    std::function<void(uint8_t)> on_tx;   // each transmitted byte (Keyboardmania: MIDI to the synth)
    Timer *tx = nullptr;

    bool rx_int() const { return (ier & 0x01) && !rx.empty(); }
    void update() { if (irq) irq(rx_int() || ((ier & 0x02) && thre_int)); }
    void rx_push(uint8_t b) {                            // a byte arrives on RX
        if (rx.size() >= ((fcr & 0x01) ? 16u : 1u)) { overrun = true; return; }
        rx.push_back(b);
        update();
    }
    uint8_t read(int reg) {
        switch (reg) {
        case 0: {
            if (lcr & 0x80) return dll;
            if (rx.empty()) return 0;
            uint8_t b = rx.front();
            rx.pop_front();
            update();
            return b;
        }
        case 1: return (lcr & 0x80) ? dlm : ier;
        case 2: {                                        // IIR: received data first; reading it clears a THRE interrupt
            uint8_t iir = rx_int() ? 0x04 : ((ier & 0x02) && thre_int) ? 0x02 : 0x01;
            if (iir == 0x02) { thre_int = false; update(); }
            return iir | ((fcr & 0x01) ? 0xc0 : 0x00);  // FIFOs enabled
        }
        case 3: return lcr;
        case 4: return mcr;
        case 5: {
            uint8_t lsr = (rx.empty() ? 0 : 0x01) | (overrun ? 0x02 : 0) | (thr_empty ? 0x20 : 0) | (thr_empty && !tsr_busy ? 0x40 : 0);
            overrun = false;
            return lsr;
        }
        case 6: return 0x00;
        default: return scr;
        }
    }
    void write(int reg, uint8_t v) {
        switch (reg) {
        case 0:
            if (lcr & 0x80) { dll = v; break; }
            if (on_tx) on_tx(v);
            if (!tsr_busy) {                             // straight into the shift register: THR empty again
                tsr_busy = true;
                thr_empty = true;
                thre_int = true;
                if (tx) tx->in(ticks_from_us(320));      // 10 bits at 31250 baud (MIDI)
            } else {
                thr_empty = false;
                thre_int = false;
            }
            update();
            break;
        case 1:
            if (lcr & 0x80) { dlm = v; break; }
            if (!(ier & 0x02) && (v & 0x02) && thr_empty) thre_int = true;   // enabling THRE with THR empty
            ier = v;
            update();
            break;
        case 2:                                          // FIFO control (reset bits self-clear)
            fcr = v & 0xc9;
            if ((v & 0x02) && !rx.empty()) {             // receive FIFO reset
                rx.clear();
                update();
            }
            break;
        case 3: lcr = v; break;
        case 4: mcr = v; break;
        case 7: scr = v; break;
        default: break;
        }
    }
    void tx_done() {                                     // shift register finished its byte
        if (!thr_empty) {                                // the waiting byte moves in
            thr_empty = true;
            thre_int = true;
            if (tx) tx->in(ticks_from_us(320));
        } else {
            tsr_busy = false;
        }
        update();
    }
} g_com[2], g_midi[2];
Timer g_midi_tx0("midi-tx0", [] { g_midi[0].tx_done(); });
Timer g_midi_tx1("midi-tx1", [] { g_midi[1].tx_done(); });
Timer g_com_tx0("com-tx0", [] { g_com[0].tx_done(); });
Timer g_com_tx1("com-tx1", [] { g_com[1].tx_done(); });

// ---- Keyboardmania MIDI keyboards (MAME konami/midikbd.cpp) ------------------------
// Every 10 ms each keyboard sends note on/off (90/80 nn 7f) for its changed keys,
// or Active Sense (fe) when nothing changed, at 31250 baud into the extend board's
// MIDI UARTs: left keyboard -> channel 1, right -> channel 0.
struct MidiKbd {
    uint32_t keys = 0, sent = 0;     // 24 keys, bit 0 = C1 (MIDI note 0x3c)
    std::deque<uint8_t> q;
    Uart *uart = nullptr;
    Timer *byte = nullptr;
} g_kbd[2];
void kbd_byte(int k) {
    MidiKbd &kb = g_kbd[k];
    if (kb.q.empty()) return;
    kb.uart->rx_push(kb.q.front());
    kb.q.pop_front();
    if (!kb.q.empty()) kb.byte->in(ticks_from_us(320));
}
Timer g_kbd_byte0("kbd0-byte", [] { kbd_byte(0); });
Timer g_kbd_byte1("kbd1-byte", [] { kbd_byte(1); });
void kbd_scan_fire();
Timer g_kbd_scan("kbd-scan", kbd_scan_fire);
void kbd_scan_fire() {
    for (auto &kb : g_kbd) {
        bool idle = kb.q.empty();
        if (kb.keys == kb.sent) {
            kb.q.push_back(0xfe);
        } else {
            for (int i = 0; i < 24; i++) {
                uint32_t m = 1u << i;
                if ((kb.sent & m) && !(kb.keys & m)) kb.q.insert(kb.q.end(), {0x80, (uint8_t)(0x3c + i), 0x7f});
                else if (!(kb.sent & m) && (kb.keys & m)) kb.q.insert(kb.q.end(), {0x90, (uint8_t)(0x3c + i), 0x7f});
            }
        }
        kb.sent = kb.keys;
        if (idle && !kb.byte->armed()) kb.byte->in(ticks_from_us(320));
    }
    g_kbd_scan.again(ticks_from_us(10000));
}
uint8_t g_wheel[2] = {0x80, 0x80};
uint64_t g_midi_out_bytes = 0;

// HK_FDC_LOG=FILE: every access to the floppy controller, a line each ("seconds frame R|W port
// value"), as a MAME tap on 0x70001000-0x70001fff prints it: the two are compared command by command
FILE *g_fdc_log = nullptr;
void fdc_log(char kind, uint32_t p, uint8_t v) {
    fprintf(g_fdc_log, "%.6f f%llu %c %x %02x\n", (double)now() / CPU_HZ, (unsigned long long)g_frame + 1, kind,
            0x3f0 + (((p - 0x70001000) >> 2) & 7), v);
}

// ---- extend board interrupt latch -------------------------------------------
// As in MAME: device callbacks drive the CPU's EXT1 line; the active bits are
// sticky until the game acknowledges them by writing 0x70006000.
void ext1_line(bool on) { cpu403::set_irq_line(cpu403::irq_ext(1), on); }

// ---- the oracle-format trace (oracle/trace.py) --------------------------------
struct Tracer {
    FILE *f = nullptr;
    // folding state, identical to oracle/lua/hiraku.lua
    bool have = false;
    uint8_t kind = 0, group = 0;
    uint32_t addr = 0, mask = 0, data = 0, hash = 0, frame = 0, pc = 0;
    uint16_t n = 0;
    bool varied = false;
    double time = 0;
    static uint32_t fnv(uint32_t h, uint32_t v) {
        for (int sh = 0; sh <= 24; sh += 8) h = (h ^ ((v >> sh) & 0xff)) * 0x01000193u;
        return h;
    }
    void emit() {
        if (!have || !f) return;
        uint8_t rec[32];
        auto put32 = [&](int o, uint32_t v) { memcpy(rec + o, &v, 4); };
        put32(0, frame);
        memcpy(rec + 4, &time, 8);
        put32(12, pc);
        put32(16, addr);
        put32(20, varied ? hash : data);
        put32(24, mask);
        rec[28] = kind | (varied ? 2 : 0);
        rec[29] = group;
        memcpy(rec + 30, &n, 2);
        fwrite(rec, 1, 32, f);
        have = false;
    }
    void record(uint8_t k, uint8_t g, uint32_t a, uint32_t d, uint32_t m, uint32_t cur_pc) {
        if (!f) return;
        if (have && n < 0xffff && k == kind && a == addr && m == mask && g == group) {
            n++;
            hash = fnv(hash, d);
            if (d != data) varied = true;
            return;
        }
        emit();
        have = true;
        kind = k; group = g; addr = a; mask = m; data = d; n = 1;
        hash = fnv(0x811c9dc5u, d);
        varied = false;
        frame = (uint32_t)g_frame;
        time = (double)now() / (double)CPU_HZ;
        pc = cur_pc;
    }
} g_trace;

hk_ctx *g_ctx_for_trace = nullptr;

uint8_t trace_group(uint32_t p) {
    if (p >= 0x7e800000 && p <= 0x7e8001ff) return 1;
    if (p >= 0x74000000 && p <= 0x740003ff) return 2;
    if ((p >= 0x7fe00000 && p <= 0x7fe0000f) || (p >= 0x7fe80000 && p <= 0x7fe8000f)) return 3;
    if (p >= 0x7d000000 && p <= 0x7d0008ff) return 4;
    if (p >= 0x7dc00000 && p <= 0x7dc0000f) return 5;
    if (p >= 0x7e000000 && p <= 0x7e00013f) return 6;
    if (p >= 0x70000000 && p <= 0x7000ffff) return 7;
    if (p >= 0x7d400000 && p <= 0x7dbfffff) return 8;
    return 0;
}

// ---- unmapped access reporting ------------------------------------------------
std::set<uint32_t> g_unmapped;
void unmapped(const char *rw, uint32_t p, int size, uint32_t v) {
    if (g_unmapped.insert(p).second) fprintf(stderr, "board: unmapped %s %08x/%d = %08x\n", rw, p, size, v);
}

// ---- byte-lane helpers: a (phys, size) access on the 32-bit big-endian bus ----
inline uint32_t lane_shift(uint32_t p, int size) { return (uint32_t)(4 - (int)(p & 3) - size) * 8; }
inline uint32_t size_mask(int size) { return size == 4 ? 0xffffffffu : ((1u << (8 * size)) - 1); }

// ---- lamps: as MAME, an access updates the lamps on the byte lanes it touches ----
std::vector<uint8_t> g_lamps;
uint64_t g_lamp_changes = 0;

void lamp_write(int reg, uint32_t data, uint32_t mask) {
    auto &defs = cab::lamps(g_cfg.family);
    for (size_t k = 0; k < defs.size(); k++) {
        auto &l = defs[k];
        if (l.reg != reg || !((mask >> l.bit) & 1)) continue;
        uint8_t on = (uint8_t)(((data >> l.bit) & 1) ^ (l.active_low ? 1 : 0));
        if (g_lamps[k] != on) {
            g_lamps[k] = on;
            g_lamp_changes++;
        }
    }
}

// Byte-wide devices: call `f(offset)` for each byte of the access, assembling the value.
template <typename F>
uint32_t bytes_read(uint32_t p, int size, uint32_t base, F f) {
    uint32_t v = 0;
    for (int k = 0; k < size; k++) v = (v << 8) | f(p + k - base);
    return v;
}
template <typename F>
void bytes_write(uint32_t p, int size, uint32_t v, uint32_t base, F f) {
    for (int k = 0; k < size; k++) f(p + k - base, (uint8_t)(v >> (8 * (size - 1 - k))));
}

uint16_t swap16(uint16_t v) { return (uint16_t)(v << 8 | v >> 8); }

// Flash chips: main 0x7d400000, sound 1 0x7d800000, sound 2 0x7da00000 (2 MiB each)
int flash_chip(uint32_t p, uint32_t &base) {
    if (p >= 0x7d400000 && p <= 0x7d5fffff) { base = 0x7d400000; return 0; }
    if (p >= 0x7d800000 && p <= 0x7d9fffff) { base = 0x7d800000; return 1; }
    if (p >= 0x7da00000 && p <= 0x7dbfffff) { base = 0x7da00000; return 2; }
    return -1;
}

// ATA on the Firebeat: 16-bit registers at base + 2*reg, byte-swapped (MAME cs*_swap_*)
uint32_t ata_read(int cs, uint32_t p, int size) {
    uint32_t v = 0;
    for (uint32_t a = p & ~1u; a < p + size; a += 2) {
        uint16_t r = swap16(g_ata->read(cs, (a & 0xf) >> 1));
        v = (v << 16) | r;
    }
    if (size == 1) v = (p & 1) ? (v & 0xff) : (v >> 8);
    return v;
}
void ata_write(int cs, uint32_t p, int size, uint32_t v) {
    if (size == 1) {   // byte write to a 16-bit register: place it in its lane
        g_ata->write(cs, (p & 0xf) >> 1, swap16((p & 1) ? (uint16_t)v : (uint16_t)(v << 8)));
        return;
    }
    for (int k = size / 2 - 1; k >= 0; k--, v >>= 16) {
        uint32_t a = (p & ~1u) + 2 * k;
        g_ata->write(cs, (a & 0xf) >> 1, swap16((uint16_t)v));
    }
}

// ---- the bus ---------------------------------------------------------------------
uint32_t bus_read(uint32_t p, int size) {
    // GCU: 32-bit registers
    if (p >= 0x7e800000 && p <= 0x7e8000ff) {
        uint32_t r = g_gcu->read(p & 0xfc);
        return (r >> lane_shift(p, size)) & size_mask(size);
    }
    if (p >= 0x7e800100 && p <= 0x7e8001ff) {                       // second GCU (Keyboardmania; else open)
        if (!g_gcu2) return 0;
        return (g_gcu2->read(p & 0xfc) >> lane_shift(p, size)) & size_mask(size);
    }
    if (p >= 0x7fe00000 && p <= 0x7fe0000f) return ata_read(0, p, size);
    if (p >= 0x7fe80000 && p <= 0x7fe8000f) return ata_read(1, p, size);
    if (p >= 0x7e000000 && p <= 0x7e00003f) return bytes_read(p, size, 0x7e000000, [](uint32_t o) { return g_rtc->read(false, o); });
    if (p >= 0x7e000100 && p <= 0x7e00013f) return bytes_read(p, size, 0x7e000100, [](uint32_t o) { return g_rtc->read(true, o); });
    {
        uint32_t base;
        int chip = flash_chip(p, base);
        if (chip >= 0) return bytes_read(p, size, base, [chip](uint32_t o) { return g_flash[chip].read(o); });
    }
    if (p >= 0x7dc00000 && p <= 0x7dc0000f)
        return bytes_read(p, size, 0x7dc00000, [](uint32_t o) { return g_com[(o >> 3) & 1].read(o & 7); });
    if (p >= 0x7d000800 && p <= 0x7d000803) {
        static bool trace = getenv("HK_LAT_TRACE") != nullptr;
        static uint64_t seen = 0;
        if (trace && g_last_input && g_last_input != seen) {
            seen = g_last_input;
            fprintf(stderr, "lat %7.2f ms: ppc reads inputs (%08x)\n", (double)(now() - g_last_input) / CPU_HZ * 1000.0, p);
        }
        return bytes_read(p, size, 0x7d000800, [](uint32_t o) { return g_in.in[o & 3]; });
    }
    if (p >= 0x7d000200 && p <= 0x7d00021f) {
        uint32_t r = ((p - 0x7d000200) >> 2) == 0 ? g_cabinet_info << 28 : 0;
        return (r >> lane_shift(p, size)) & size_mask(size);
    }
    if (p >= 0x7d000400 && p <= 0x7d000401) return bytes_read(p, size, 0x7d000400, [](uint32_t o) { return g_ymz->read((int)o); });
    if (p >= 0x74000000 && p <= 0x740003ff) {                       // SPU dual-port RAM (right side)
        if (spu::present()) return bytes_read(p, size, 0x74000000, [](uint32_t o) { return spu::ppc_read(o); });
        return bytes_read(p, size, 0x74000000, [](uint32_t o) -> uint8_t { return g_dpram[o]; });
    }
    if (p >= 0x7000a000 && p <= 0x7000a003) return bytes_read(p, size, 0x7000a000, [](uint32_t) { return (uint8_t)~g_ext_irq_active; });
    if (g_cfg.family == "kbm") {
        if (p >= 0x70000000 && p <= 0x70000fff)                      // MIDI UARTs (keyboards, synth), lane 0 only
            return (p & 3) == 0 ? (uint32_t)g_midi[((p - 0x70000000) >> 8) >> 3 & 1].read(((p - 0x70000000) >> 8) & 7) << (8 * (size - 1)) : 0;
        if (p >= 0x70008010 && p <= 0x700080ff) return 0;          // read by the game, unmapped in MAME too
        if (p >= 0x70008000 && p <= 0x7000800f)                      // modulation wheels: P1 at +0, P2 at +8
            return bytes_read(p, size, 0x70008000, [](uint32_t o) -> uint8_t { return o == 0 ? g_wheel[0] : o == 8 ? g_wheel[1] : 0; });
    }
    if (g_cfg.family == "ppp" && p >= 0x7d000340 && p <= 0x7d00035f) {   // sensor bars: 16-bit
        uint32_t v = 0;
        for (uint32_t a = p & ~1u; a < p + size; a += 2) {
            uint32_t o = (a - 0x7d000340) >> 1;
            v = (v << 16) | (o < 4 ? (uint16_t)(g_in.sensor[o] | 0x100) : 0);
        }
        if (size == 1) v = (p & 1) ? (v & 0xff) : (v >> 8);
        return v;
    }
    if (g_cfg.family == "bm3") {
        if (p >= 0x7d000340 && p <= 0x7d00035f) {                  // sensors: 16-bit
            uint32_t v = 0;
            for (uint32_t a = p & ~1u; a < p + size; a += 2) {
                uint32_t o = (a - 0x7d000340) >> 1;
                uint16_t r = 0;
                switch (o) {
                case 0: case 1: case 2: case 3: r = g_in.io[o] | 0x100; break;
                case 5: r = (g_in.turntable[0] >> 8) | 0x100; break;
                case 6: r = (g_in.turntable[0] & 0xff) | 0x100; break;
                case 7: r = (g_in.turntable[1] >> 8) | 0x100; break;
                case 8: r = (g_in.turntable[1] & 0xff) | 0x100; break;
                default: if (o >= 9 && o <= 15) r = g_in.effect[o - 9] | 0x100; break;
                }
                v = (v << 16) | r;
            }
            if (size == 1) v = (p & 1) ? (v & 0xff) : (v >> 8);
            return v;
        }
        if (p >= 0x70000000 && p <= 0x70000fff)                      // MIDI UART, lane 0 only
            return (p & 3) == 0 ? (uint32_t)g_midi[((p - 0x70000000) >> 8) >> 3 & 1].read(((p - 0x70000000) >> 8) & 7) << (8 * (size - 1)) : 0;
        if (p >= 0x70001000 && p <= 0x70001fff) {                    // FDC37C665GT, lane 0: ISA port = offset / 4
            if (p & 3) return 0;
            const uint8_t b = g_fdc->read((int)((p - 0x70001000) >> 2));
            if (g_fdc_log) fdc_log('R', p, b);
            return (uint32_t)b << (8 * (size - 1));
        }
        if (p >= 0x70008000 && p <= 0x7000807f)                      // spectrum analyser (fed by the SPU's output)
            return bytes_read(p, size, 0x70008000, [](uint32_t o) { return spectrum::read(o); });
    }
    unmapped("R", p, size, 0);
    return 0;
}

void bus_write(uint32_t p, uint32_t v, int size) {
    if (p >= 0x7e800000 && p <= 0x7e8000ff) {
        uint32_t sh = lane_shift(p, size);
        g_gcu->write(p & 0xfc, v << sh, size_mask(size) << sh);
        return;
    }
    if (p >= 0x7e800100 && p <= 0x7e8001ff) {                       // other games' boot code writes here too
        if (g_gcu2) {
            uint32_t sh = lane_shift(p, size);
            g_gcu2->write(p & 0xfc, v << sh, size_mask(size) << sh);
        }
        return;
    }
    if (p >= 0x7fe00000 && p <= 0x7fe0000f) { ata_write(0, p, size, v); return; }
    if (p >= 0x7fe80000 && p <= 0x7fe8000f) { ata_write(1, p, size, v); return; }
    if (p >= 0x7e000000 && p <= 0x7e00003f) { bytes_write(p, size, v, 0x7e000000, [](uint32_t o, uint8_t b) { g_rtc->write(false, o, b); }); return; }
    if (p >= 0x7e000100 && p <= 0x7e00013f) { bytes_write(p, size, v, 0x7e000100, [](uint32_t o, uint8_t b) { g_rtc->write(true, o, b); }); return; }
    {
        uint32_t base;
        int chip = flash_chip(p, base);
        if (chip >= 0) {
            bytes_write(p, size, v, base, [chip](uint32_t o, uint8_t b) { g_flash[chip].write(o, b); });
            return;
        }
    }
    if (p >= 0x7dc00000 && p <= 0x7dc0000f) {
        bytes_write(p, size, v, 0x7dc00000, [](uint32_t o, uint8_t b) { g_com[(o >> 3) & 1].write(o & 7, b); });
        return;
    }
    if (p >= 0x7d000500 && p <= 0x7d000501) {                       // control
        uint8_t d = (uint8_t)v;
        if ((d & 1) != (g_control & 1)) {
            g_gcu->set_pixclock((d & 1) ? 16'666'600.0 : 25'175'000.0);
            if (g_gcu2) g_gcu2->set_pixclock((d & 1) ? 16'666'600.0 : 25'175'000.0);
        }
        g_control = d;
        return;
    }
    if (p >= 0x7d000320 && p <= 0x7d000327) { lamp_write(1 + ((p >> 2) & 1), v << lane_shift(p, size), size_mask(size) << lane_shift(p, size)); return; }
    if (p >= 0x7d000804 && p <= 0x7d000807) { lamp_write(0, v << lane_shift(p, size), size_mask(size) << lane_shift(p, size)); return; }   // lamps, status LEDs
    if (p >= 0x7d000400 && p <= 0x7d000401) {
        bytes_write(p, size, v, 0x7d000400, [](uint32_t o, uint8_t b) { g_ymz->write((int)o, b); });
        return;
    }
    if (p >= 0x74000000 && p <= 0x740003ff) {
        if (spu::present()) bytes_write(p, size, v, 0x74000000, [](uint32_t o, uint8_t b) { spu::ppc_write(o, b); });
        else bytes_write(p, size, v, 0x74000000, [](uint32_t o, uint8_t b) { g_dpram[o] = b; });
        return;
    }
    if (p >= 0x70006000 && p <= 0x70006003) {                       // extend board IRQ enable/ack
        bool fdd_was = g_ext_irq_enable & 0x04;
        g_ext_irq_active &= ~(uint8_t)v;
        g_ext_irq_enable = (uint8_t)v;
        if (bool(g_ext_irq_enable & 0x04) != fdd_was) ext1_line(false);   // MAME: helps the FDD not get stuck
        // The floppy controller's interrupt is a level: still asked for when the game lets it through
        // again, it comes again. MAME's board keeps only its rising edges, and one that comes between
        // the handler's last look at the controller and its acknowledge here is lost; with the
        // chip's FIFO, which asks again at the next byte, a read would stop there (dev/fdc.h).
        if (!g_cfg.mame_fdc && g_fdc && g_fdc->irq_line() && !(g_ext_irq_enable & 0x04)) {
            g_ext_irq_active |= 0x04;
            ext1_line(true);
        }
        return;
    }
    if (g_cfg.family == "ppp" && p >= 0x7d000300 && p <= 0x7d00033f) return;   // written at boot; unmapped in MAME too
    if (g_cfg.family == "kbm" && p >= 0x70000000 && p <= 0x70000fff) {
        if ((p & 3) == 0) g_midi[((p - 0x70000000) >> 8) >> 3 & 1].write(((p - 0x70000000) >> 8) & 7, (uint8_t)(v >> (8 * (size - 1))));
        return;
    }
    if (g_cfg.family == "bm3") {
        if (p >= 0x7d000330 && p <= 0x7d00033f) return;
        if (p >= 0x70000000 && p <= 0x70000fff) {
            if ((p & 3) == 0) g_midi[((p - 0x70000000) >> 8) >> 3 & 1].write(((p - 0x70000000) >> 8) & 7, (uint8_t)(v >> (8 * (size - 1))));
            return;
        }
        if (p >= 0x70001000 && p <= 0x70001fff) {
            if (p & 3) return;
            if (g_fdc_log) fdc_log('W', p, (uint8_t)(v >> (8 * (size - 1))));
            g_fdc->write((int)((p - 0x70001000) >> 2), (uint8_t)(v >> (8 * (size - 1))));
            return;
        }
    }
    unmapped("W", p, size, v);
}

uint32_t traced_read(uint32_t p, int size) {
    uint32_t v = bus_read(p, size);
    if (g_trace.f) {
        if (uint8_t g = trace_group(p)) {
            uint32_t sh = lane_shift(p, size);
            // MAME's tap sees what the device's handler returned: for 32-bit devices
            // (the GCU) that is the whole register, not just the lanes asked for.
            uint32_t d = (g == 1 && p <= 0x7e8000ff) ? g_gcu->read(p & 0xfc) : v << sh;
            g_trace.record(0, g, p & ~3u, d, size_mask(size) << sh, g_ctx_for_trace ? g_ctx_for_trace->pc : 0);
        }
    }
    return v;
}

void traced_write(uint32_t p, uint32_t v, int size) {
    if (g_trace.f) {
        if (uint8_t g = trace_group(p)) {
            uint32_t sh = lane_shift(p, size);
            g_trace.record(1, g, p & ~3u, v << sh, size_mask(size) << sh, g_ctx_for_trace ? g_ctx_for_trace->pc : 0);
        }
    }
    bus_write(p, v, size);
}

// ---- board audio: YMZ280B + CD audio ------------------------------------------------------
// Rendered in step with virtual time: by a 2 ms timer and before every register or
// playback-state access. YMZ samples come from the sound flash chips (MAME
// ymz280b_map); its 88.2 kHz output is averaged to 44.1 kHz, then the slave CD
// drive's Red Book audio is mixed in.
uint64_t g_ymz_samples = 0;     // 88.2 kHz samples rendered
AudioSink g_audio_sink;
std::vector<float> g_audio_held;   // see audio_release()
float g_ymz_carry[2];
bool g_ymz_odd = false;

void ymz_render_until(uint64_t t) {
    uint64_t target = (uint64_t)((double)t * Ymz280b::RATE / CPU_HZ);
    static float l[1024], r[1024], out[1024];
    while (g_ymz_samples < target) {
        int n = (int)std::min<uint64_t>(target - g_ymz_samples, 1024);
        memset(l, 0, sizeof(float) * n);
        memset(r, 0, sizeof(float) * n);
        g_ymz->render(l, r, n);
        g_ymz_samples += (uint64_t)n;
        int m = 0;
        for (int k = 0; k < n; k++) {
            if (!g_ymz_odd) {
                g_ymz_carry[0] = l[k];
                g_ymz_carry[1] = r[k];
            } else {
                out[2 * m] = (g_ymz_carry[0] + l[k]) * 0.5f;
                out[2 * m + 1] = (g_ymz_carry[1] + r[k]) * 0.5f;
                m++;
            }
            g_ymz_odd = !g_ymz_odd;
        }
        if (m) g_ata->cdda_render(1, out, m, 0.5f);      // CD audio from the slave drive (MAME: 0.5 to the speaker)
        if (m && synth::deferred()) {                    // the synth card adds its part at the frame's end
            synth::request(m);
            g_audio_held.insert(g_audio_held.end(), out, out + 2 * m);
            continue;
        }
        if (m) synth::render(out, m, 1.0f);              // Keyboardmania's synth card (stand-in)
        if (g_audio_sink && m) g_audio_sink(out, m);
    }
}

// The audio held for the synth card's samples (synth::deferred()), handed on
// once a frame: the same samples, sums and order as chunk by chunk.
void audio_release() {
    if (g_audio_held.empty()) return;
    const int n = (int)(g_audio_held.size() / 2);
    synth::collect(g_audio_held.data(), n, 1.0f);
    if (g_audio_sink) g_audio_sink(g_audio_held.data(), n);
    g_audio_held.clear();
}

void ymz_tick_fire();
Timer g_ymz_tick("ymz", ymz_tick_fire);
void ymz_tick_fire() {
    ymz_render_until(now());
    g_ymz_tick.again(ticks_from_us(2000));
}

// ---- video timing ---------------------------------------------------------------------
void vblank_fire();
Timer g_vblank("vblank", vblank_fire);

uint64_t frame_ticks() {
    double period = (double)g_gcu->htotal() * g_gcu->vtotal() / g_gcu->pixclock();
    return (uint64_t)(period * CPU_HZ + 0.5);
}

// The GCU's picture now, into `out` (w x h). Keyboardmania: two portrait monitors (MAME ROT270),
// left and right side by side; each w x h screen becomes h x w, (x, y) -> (y, w - 1 - x)
void draw_picture(std::vector<uint32_t> &out, int &ow, int &oh) {
    prof::Scope ps(prof::DRAW);
    const int w = g_gcu->hvisible(), h = g_gcu->vvisible();
    if (!g_gcu2) {
        out.resize((size_t)w * h);
        g_gcu->draw(out.data(), w, h);
        ow = w, oh = h;
        return;
    }
    static std::vector<uint32_t> half;
    ow = 2 * h, oh = w;
    out.resize((size_t)ow * oh);
    half.resize((size_t)w * h);
    for (int k = 0; k < 2; k++) {
        (k ? g_gcu2 : g_gcu)->draw(half.data(), w, h);
        // in 32x32 tiles, so that neither the reads nor the column writes thrash the cache
        constexpr int T = 32;
        for (int y0 = 0; y0 < h; y0 += T)
            for (int x0 = 0; x0 < w; x0 += T)
                for (int x = x0; x < std::min(x0 + T, w); x++) {
                    uint32_t *d = &out[(size_t)(w - 1 - x) * ow + (size_t)k * h];
                    for (int y = y0; y < std::min(y0 + T, h); y++) d[y] = half[(size_t)y * w + x];
                }
    }
}

// The end of vertical blank: the picture as the monitor then shows it, the game's flip in its
// vblank interrupt included, for the window (set_present_hook). The timer runs with or without
// the window, so a run goes the same either way.
void vblank_end_fire();
Timer g_vblank_end("vblank end", vblank_end_fire);
std::vector<uint32_t> g_present_fb;
void vblank_end_fire() {
    if (!g_present_hook) return;
    int w, h;
    draw_picture(g_present_fb, w, h);
    g_present_hook(g_present_fb.data(), w, h);
}

void vblank_fire() {
    audio_release();
    g_frame++;
    g_vblank.again(frame_ticks());
    g_vblank_end.in((uint64_t)((double)g_gcu->htotal() * (g_gcu->vtotal() - g_gcu->vvisible()) / g_gcu->pixclock() * CPU_HZ + 0.5));
    g_gcu->vblank(true);
    if (g_frame_hook) {
        int w, h;
        if (!g_frame_picture) {   // nobody looks at it (frame_picture_unused): only its size
            w = g_gcu->hvisible(), h = g_gcu->vvisible();
            if (g_gcu2) std::swap(w, h), w *= 2;   // (draw_picture's two turned screens side by side)
            g_frame_hook(g_frame, nullptr, w, h);
            return;
        }
        draw_picture(g_fb, w, h);
        g_frame_hook(g_frame, g_fb.data(), w, h);
    }
}

bool read_file(const std::string &path, std::vector<uint8_t> &out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), {});
    return true;
}

// ---- beatmania III's floppy disk (--floppy) -----------------------------------------------------
// The image file is the disk. It is read whole when the disk goes in and written back whole
// (shell/saves.h) when the game has changed it: while playing once the drive's motor has stopped
// (the game's writing is over then, so the file is a disk as the game left it, never half a
// cylinder), when the disk is taken out, and at the end.
uint64_t g_floppy_kept = 0;          // the disk's disk_writes() as its file has it
bool g_floppy_known = false;         // ... (false: nothing is known of the file)
unsigned g_floppy_failed = 0;        // the writer's failed sets when last looked at
std::function<void(const std::string &)> g_floppy_said;

std::pair<std::string, std::string> floppy_place() {   // the file's folder and its name there
    const std::string &f = g_cfg.floppy;
    const size_t slash = f.find_last_of("/\\");
    return slash == std::string::npos ? std::make_pair(std::string("."), f) : std::make_pair(f.substr(0, slash), f.substr(slash + 1));
}

// The file's disk into the drive: a new file is a blank formatted disk, a file that can't be
// written a write-protected one. false (and why): not a 1.44 MB disk's image
bool floppy_insert(bool at_power_on, std::string &why) {
    saves::writer().drain();   // (the file may still be on its way from when the disk came out)
    saves::recover(floppy_place().first);
    std::vector<uint8_t> image;
    bool is_new = false, read_only = false;
    bool on_file = true;
    if (!sys::exists(g_cfg.floppy)) {
        image = Fdc37c665::blank_disk();
        is_new = true;
        sys::make_dirs(floppy_place().first);
        on_file = sys::write_file(g_cfg.floppy, image.data(), image.size());
        if (!on_file) fprintf(stderr, "floppy: cannot write %s: what the game saves on this disk may not be kept\n", g_cfg.floppy.c_str());
    } else if (!read_file(g_cfg.floppy, image) || image.size() != Fdc37c665::kDiskBytes) {
        why = g_cfg.floppy + " is not a 1.44 MB disk's image (" + std::to_string(Fdc37c665::kDiskBytes) + " bytes of sectors)";
        return false;
    } else {
        std::error_code ec;
        const auto perms = std::filesystem::status(g_cfg.floppy, ec).permissions();
        read_only = !ec && (perms & std::filesystem::perms::owner_write) == std::filesystem::perms::none;
    }
    g_fdc->insert(std::move(image), read_only, at_power_on);
    g_floppy_kept = g_fdc->disk_writes(), g_floppy_known = on_file;
    // (the file's name on stderr: stdout's lines are a run's statistics, the same wherever its files are)
    printf("floppy: %s%s in the drive\n", is_new ? "a new blank disk" : "the disk", read_only ? ", write-protected," : "");
    fprintf(stderr, "floppy: %s\n", g_cfg.floppy.c_str());
    return true;
}

// The disk to its file if the game has changed it. now: before returning (else on the writer's thread)
void floppy_keep(bool now_) {
    if (!g_fdc || !g_fdc->has_disk() || g_cfg.floppy.empty()) return;
    if (now_) saves::writer().drain();
    // a set the writer couldn't write: the file may not be what it was thought to be
    if (const unsigned failed = saves::writer().failed(); failed != g_floppy_failed) g_floppy_failed = failed, g_floppy_known = false;
    if (g_floppy_known && g_fdc->disk_writes() == g_floppy_kept) return;
    const auto [dir, name] = floppy_place();
    const auto &d = g_fdc->disk();
    std::vector<saves::File> set{{name, std::string(d.begin(), d.end())}};
    g_floppy_kept = g_fdc->disk_writes(), g_floppy_known = true;
    if (!now_) {
        saves::writer().write(dir, std::move(set));
        return;
    }
    if (!saves::write_set(dir, set)) {
        g_floppy_known = false;
        fprintf(stderr, "floppy: cannot write %s: the disk as it was before is kept\n", g_cfg.floppy.c_str());
    }
}

}  // namespace

void init(const Config &cfg) {
    g_cfg = cfg;
    g_lamps.assign(cab::lamps(cfg.family).size(), 0);
    if (cfg.family == "popn") {                     // MAME `popn` ports, nothing pressed:
        g_in.in[0] = 0xff;                            // IN0: switches 1-8, active low
        g_in.in[2] = 0x55;                            // IN2: switch 9, coin, test, service released; the rest active high
    }
    if (cfg.family == "ppp") g_in.in[0] = 0x7f;    // IN0: buttons, test, service, coin, start released; bit 7 active high
    if (cfg.family == "kbm") {                      // MAME `kbm` ports, nothing pressed
        g_in.in[0] = 0xff;                            // IN0: starts, test, service, coin (active low)
        g_in.in[2] = 0xfe;                            // IN2: e-Amusement bit 0 active high, the rest active low
    }
    g_gcu = new K057714;
    g_ata = new AtaBus;
    g_rtc = new Rtc65271;
    g_flash = new Flash29F016A[3];
    for (int k = 0; k < 3; k++) {                 // erases take MAME's time (1 s a sector, 16 s the chip)
        g_flash[k].clock = [] { return now(); };
        g_flash[k].clock_hz = CPU_HZ;
    }
    g_fdc = new Fdc37c665;
    // FDC interrupt -> extend board bit 0x04 (if enabled: bit clear) -> EXT1 (MAME floppy_irq_callback)
    g_fdc->irq = [](bool on) {
        if (g_fdc_log)   // (six words: not an access)
            fprintf(g_fdc_log, "%.6f f%llu irq %d enable %02x\n", (double)now() / CPU_HZ, (unsigned long long)g_frame + 1, on, g_ext_irq_enable);
        if (on && !(g_ext_irq_enable & 0x04)) {
            g_ext_irq_active |= 0x04;
            ext1_line(true);
        }
    };
    // MIDI UART channel 1 (to the ST-224) -> extend board bit 0x01 (MAME midi_st224_irq_callback)
    g_midi[1].irq = [](bool on) {
        if (!(g_ext_irq_enable & 0x01) && on) {
            g_ext_irq_active |= 0x01;
            ext1_line(true);
        } else {
            ext1_line(false);
        }
    };
    g_midi[0].tx = &g_midi_tx0;
    g_midi[1].tx = &g_midi_tx1;
    g_com[0].tx = &g_com_tx0;
    g_com[1].tx = &g_com_tx1;
    g_ata->log = cfg.ata_log;

    g_gcu->irq = [](bool on) { cpu403::set_irq_line(cpu403::irq_ext(0), on); };
    if (cfg.family == "kbm") {
        // MAME firebeat_kbm: a second GCU (no vblank of its own), MIDI keyboards on
        // the extend board's UART receivers (left -> ch1 -> bit 0x01, right -> ch0 ->
        // bit 0x02), and channel 1's transmitter to the XT446 synth card
        g_gcu2 = new K057714;
        g_gcu2->irq = [](bool on) { cpu403::set_irq_line(cpu403::irq_ext(0), on); };
        g_midi[0].irq = [](bool on) {
            if (!(g_ext_irq_enable & 0x02) && on) {
                g_ext_irq_active |= 0x02;
                ext1_line(true);
            } else {
                ext1_line(false);
            }
        };
        g_kbd[0].uart = &g_midi[1];
        g_kbd[0].byte = &g_kbd_byte0;
        g_kbd[1].uart = &g_midi[0];
        g_kbd[1].byte = &g_kbd_byte1;
        g_kbd_scan.in(ticks_from_us(10000));
        static bool log_midi = getenv("HK_MIDI_LOG") != nullptr;
        g_midi[1].on_tx = [](uint8_t b) {
            g_midi_out_bytes++;
            if (log_midi) fprintf(stderr, "midi> %02x\n", b);
            if (synth::active()) {
                ymz_render_until(now());                 // the board's audio up to this moment first
                synth::midi(b);
            }
        };
    }
    g_ata->irq = [](bool on) { cpu403::set_irq_line(cpu403::irq_ext(4), on); };

    auto master = std::make_unique<AtapiDrive>("cd0");
    if (!cfg.cd_master.empty()) {
        master->disc = std::make_unique<DiscImage>();
        if (!master->disc->open(cfg.cd_master)) fatal(nullptr, "cannot open disc image %s", cfg.cd_master.c_str());
    }
    g_ata->attach(0, std::move(master));
    auto slave = std::make_unique<AtapiDrive>("cd1");
    if (!cfg.cd_slave.empty()) {
        slave->disc = std::make_unique<DiscImage>();
        if (!slave->disc->open(cfg.cd_slave)) fatal(nullptr, "cannot open disc image %s", cfg.cd_slave.c_str());
    }
    g_ata->attach(1, std::move(slave));

    // dongle: 3 x (8 id + 8 password + 48 data)
    std::vector<uint8_t> dongle;
    if (!cfg.dongle_file.empty() && read_file(cfg.rom_dir + "/" + cfg.dongle_file, dongle) && dongle.size() >= 0xc0)
        memcpy(g_ib.subkey, dongle.data(), 0xc0);
    else
        fprintf(stderr, "board: no dongle image (%s/%s): the security check will fail\n", cfg.rom_dir.c_str(), cfg.dongle_file.c_str());
    cpu403::spu_set_tx_handler(security_w);

    // RTC/NVRAM: saved state, else the ROM set's default, else blank
    std::vector<uint8_t> nv;
    if (!cfg.nvram_dir.empty() && read_file(cfg.nvram_dir + "/rtc.nv", nv) && g_rtc->load_nvram(nv.data(), nv.size()))
        fprintf(stderr, "board: NVRAM loaded from %s\n", cfg.nvram_dir.c_str());
    else if (!(read_file(cfg.rom_dir + "/rtc", nv) && g_rtc->load_nvram(nv.data(), nv.size())))
        fprintf(stderr, "board: no RTC NVRAM image: starting blank\n");
    {   // fixed, so runs are reproducible; HK_RTC="YYYY-MM-DD[ HH:MM:SS]" for experiments
        int y = 2002, mo = 7, d = 26, h = 12, mi = 0, se = 0;
        if (const char *e = getenv("HK_RTC")) sscanf(e, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &se);
        static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};   // Sakamoto: 0 = Sunday
        int yy = mo < 3 ? y - 1 : y;
        int wd = (yy + yy / 4 - yy / 100 + yy / 400 + t[mo - 1] + d) % 7;
        g_rtc->set_time(y, mo, d, wd + 1, h, mi, se);   // the chip's weekday: 1 = Sunday
    }
    for (int k = 0; k < 3; k++) {
        std::vector<uint8_t> f;
        const std::string path = cfg.nvram_dir + "/flash" + std::to_string(k) + ".bin";
        if (cfg.nvram_dir.empty() || !read_file(path, f)) continue;
        if (f.size() == 0x200000) g_flash[k].data() = f;
        else fprintf(stderr, "board: %s is %zu bytes, not a flash chip's 2 MiB: left as it is, the chip starts as the dump has it\n", path.c_str(), f.size());
    }

    g_fdc->set_mame_fifo(cfg.mame_fdc);
    if (!cfg.floppy.empty()) {   // the disk is in the drive when the machine is turned on
        std::string why;
        if (cfg.family != "bm3") fprintf(stderr, "board: --floppy: only beatmania III has a floppy drive\n"), g_cfg.floppy.clear();
        else if (!floppy_insert(true, why)) fatal(nullptr, "--floppy: %s", why.c_str());
    }
    if (const char *e = getenv("HK_FDC_LOG"); e && *e && cfg.family == "bm3") g_fdc_log = fopen(e, "w");

    // JP, non-rental; ParaPara adds bit 3, Keyboardmania bit 1 (MAME init_*_jp)
    g_cabinet_info = cfg.cabinet >= 0 ? (uint32_t)cfg.cabinet : cfg.family == "ppp" ? 0x8 : cfg.family == "kbm" ? 0x2 : 0;
    g_ext_irq_enable = 0x3f;
    g_ext_irq_active = 0;
    memset(g_dpram, 0, sizeof g_dpram);

    if (!cfg.trace_dir.empty()) {
        g_trace.f = fopen((cfg.trace_dir + "/mmio.bin").c_str(), "wb");
        if (!g_trace.f) fatal(nullptr, "cannot write %s/mmio.bin", cfg.trace_dir.c_str());
    }
    mmio_set_handler({traced_read, traced_write});

    // first vertical blank at the end of the first visible field
    g_vblank.in((uint64_t)((double)frame_ticks() * g_gcu->vvisible() / g_gcu->vtotal()));
    g_ymz = new Ymz280b;
    g_ymz->read_mem = [](uint32_t a) {   // MAME ymz280b_map: global mask 0x3fffff, two sound flash chips
        a &= 0x3fffff;
        return g_flash[1 + (a >> 21)].read(a & 0x1fffff);
    };
    g_ymz->sync = [] { ymz_render_until(now()); };
    g_ata->sync = [] { ymz_render_until(now()); };
    g_ymz_tick.in(ticks_from_us(2000));
}

// The saved memories that aren't what the save folder has, as they are at this moment
// (shell/saves.h): the clock chip with its RAM (its clock makes it another every second), and each
// flash chip written since it was last saved
static uint64_t g_rtc_kept = 0;
static bool g_rtc_known = false;
static unsigned g_saves_failed = 0;

static std::vector<saves::File> changed_saves() {
    // a set the writer couldn't write: nothing is known of the folder any more
    if (const unsigned failed = saves::writer().failed(); failed != g_saves_failed) {
        g_saves_failed = failed;
        g_rtc_known = false;
        for (int k = 0; k < 3; k++) g_flash[k].unsaved = g_flash[k].dirty;
    }
    std::vector<saves::File> set;
    std::string rtc = g_rtc->save_nvram();
    if (const uint64_t h = saves::hash(rtc); !g_rtc_known || h != g_rtc_kept) {
        g_rtc_kept = h, g_rtc_known = true;
        set.push_back({"rtc.nv", std::move(rtc)});
    }
    for (int k = 0; k < 3; k++) {
        if (!g_flash[k].unsaved) continue;
        g_flash[k].unsaved = false;
        const auto &d = g_flash[k].data();
        set.push_back({"flash" + std::to_string(k) + ".bin", std::string(d.begin(), d.end())});
    }
    return set;
}

void shutdown() {
    static bool done = false;
    if (done) return;
    done = true;
    audio_release();
    synth::stop();
    g_trace.emit();
    if (g_trace.f) fclose(g_trace.f);
    g_trace.f = nullptr;
    if (g_fdc_log) fclose(g_fdc_log);
    g_fdc_log = nullptr;
    floppy_keep(true);
    if (!g_cfg.nvram_dir.empty()) {
        // all whole or none (saves::write_set): a full or failing disk, or a kill half way, keeps the
        // save from before
        saves::writer().drain();   // (a set still being written: its files first)
        if (!saves::write_set(g_cfg.nvram_dir, changed_saves())) {
            g_rtc_known = false;
            fprintf(stderr, "board: cannot write the saved memories to %s: the ones from before are kept\n", g_cfg.nvram_dir.c_str());
        }
    }
}

void save_now() {
    if (!g_cfg.nvram_dir.empty() && g_rtc) saves::writer().write(g_cfg.nvram_dir, changed_saves());
    if (g_fdc && !g_fdc->motor_on()) floppy_keep(false);   // (the motor on: the game may be in the middle of a write)
}

std::string floppy_toggle() {
    if (!g_fdc || g_cfg.floppy.empty()) return {};
    std::string said;
    if (g_fdc->has_disk()) {
        const bool cut = g_fdc->writing();
        floppy_keep(false);   // (on the writer's thread: the game doesn't wait for the file)
        g_fdc->eject();
        said = cut ? "Floppy disk taken out while the game was writing it" : "Floppy disk taken out";
    } else {
        std::string why;
        said = floppy_insert(false, why) ? std::string("Floppy disk in the drive") + (g_fdc->write_protected() ? " (write-protected)" : "")
                                         : "No floppy disk: " + why;
    }
    printf("floppy: %s\n", said.c_str());
    if (g_floppy_said) g_floppy_said(said);
    return said;
}
bool floppy_in() { return g_fdc && g_fdc->has_disk(); }
void set_floppy_said(std::function<void(const std::string &)> f) { g_floppy_said = std::move(f); }

void note_input_change() { g_last_input = now(); }
uint64_t last_input_change() { return g_last_input; }

Inputs &inputs() { return g_in; }
const std::string &family() { return g_cfg.family; }

bool set_input(const std::string &name0, int value) try {
    std::string name = name0, port;
    if (auto slash = name.find('/'); slash != std::string::npos) { port = name.substr(0, slash); name = name.substr(slash + 1); }
    auto low16 = [&](uint16_t &p, uint16_t bit) { p = value ? (p & ~bit) : (p | bit); };
    auto low8 = [&](uint8_t &p, uint8_t bit) { p = value ? (p & ~bit) : (p | bit); };
    auto high8 = [&](uint8_t &p, uint8_t bit) { p = value ? (p | bit) : (p & ~bit); };
    if (g_cfg.family == "kbm") {
        // MAME's `kbm` ports; keys "P1 Key 1".."P1 Key 24" (or MAME's "P1 C1".."P1 B2"),
        // likewise P2; wheels "P1 Wheel"/"P2 Wheel" take 0-255
        static const char *notes[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
        if ((name.rfind("P1 ", 0) == 0 || name.rfind("P2 ", 0) == 0)) {
            int pl = name[1] - '1';
            std::string rest = name.substr(3);
            int key = -1;
            if (rest.rfind("Key ", 0) == 0) key = std::stoi(rest.substr(4)) - 1;
            else if (rest == "Wheel") { g_wheel[pl] = (uint8_t)std::clamp(value, 0, 255); return true; }
            else
                for (int k = 0; k < 24; k++) {           // MAME's names: letter, octave, then '#' ("C1#")
                    std::string n = std::string(1, notes[k % 12][0]) + std::to_string(k / 12 + 1) + (notes[k % 12][1] ? "#" : "");
                    if (rest == n) key = k;
                }
            if (key < 0 || key > 23) return false;
            if (value) g_kbd[pl].keys |= 1u << key;
            else g_kbd[pl].keys &= ~(1u << key);
            if (value) note_input_change();
            return true;
        }
        if (name == "1 Player Start") low8(g_in.in[0], 0x01);
        else if (name == "2 Players Start") low8(g_in.in[0], 0x02);
        else if (name == "Test" || name == "Service Mode") low8(g_in.in[0], 0x04);
        else if (name == "Service") low8(g_in.in[0], 0x08);
        else if (name == "Coin 1") low8(g_in.in[0], 0x10);
        else return false;
        return true;
    }
    if (g_cfg.family == "ppp") {
        // MAME's `ppp` ports: IN0 = left, right, test, service, coin, start (active low);
        // "P1 Button 3".."10" = sensor bars 1-8, each raising its three sensors together
        static const struct { uint8_t word; uint16_t bits; } bars[8][2] = {
            {{0, 0x0007}, {0, 0}}, {{0, 0x0038}, {0, 0}}, {{0, 0x00c0}, {1, 0x0001}}, {{1, 0x000e}, {0, 0}},
            {{2, 0x0007}, {0, 0}}, {{2, 0x0038}, {0, 0}}, {{2, 0x00c0}, {3, 0x0001}}, {{3, 0x000e}, {0, 0}}};
        if (name == "P1 Button 1") low8(g_in.in[0], 0x01);
        else if (name == "P1 Button 2") low8(g_in.in[0], 0x02);
        else if (name.rfind("P1 Button ", 0) == 0) {
            int k = std::stoi(name.substr(10));
            if (k < 3 || k > 10) return false;
            for (auto &b : bars[k - 3])
                if (b.bits) g_in.sensor[b.word] = value ? (g_in.sensor[b.word] | b.bits) : (g_in.sensor[b.word] & ~b.bits);
            if (value) note_input_change();
        }
        else if (name == "Test" || name == "Service Mode") low8(g_in.in[0], 0x04);
        else if (name == "Service") low8(g_in.in[0], 0x08);
        else if (name == "Coin 1") low8(g_in.in[0], 0x10);
        else if (name == "1 Player Start") low8(g_in.in[0], 0x20);
        else return false;
        return true;
    }
    if (g_cfg.family == "popn") {
        // MAME's `popn` ports: IN0 = switches 1-8, IN2 = switch 9, coin, test, service (all active low)
        if (name.rfind("P1 Button ", 0) == 0) {
            int k = std::stoi(name.substr(10));
            if (k < 1 || k > 9) return false;
            if (k <= 8) low8(g_in.in[0], (uint8_t)(1u << (k - 1)));
            else low8(g_in.in[2], 0x01);
            if (value) note_input_change();
        }
        else if (name == "Coin 1") low8(g_in.in[2], 0x04);
        else if (name == "Test" || name == "Service Mode") low8(g_in.in[2], 0x10);
        else if (name == "Service") low8(g_in.in[2], 0x40);
        else return false;
        return true;
    }
    if (name == "Coin 1") low16(g_in.io[1], 0x0001);
    else if (name == "1 Player Start") low16(g_in.io[0], 0x0020);
    else if (name == "2 Players Start") low16(g_in.io[2], 0x0020);
    else if (name.rfind("P1 Button ", 0) == 0 || name.rfind("P2 Button ", 0) == 0) {
        int k = std::stoi(name.substr(10));
        if (k < 1 || k > 5) return false;
        low16(g_in.io[name[1] == '1' ? 0 : 2], (uint16_t)(1u << (k - 1)));
        if (value) note_input_change();
    }
    else if (name == "Test") low8(g_in.in[0], 0x01);
    else if (name == "Service") low8(g_in.in[0], 0x02);
    else if (name == "Floppy") {                         // the drive's slot: a press takes the disk out or puts it in
        if (g_cfg.floppy.empty()) return true;           // (bound, but no disk was given: nothing to move)
        if (value) floppy_toggle();
    }
    else if (name == "A") low8(g_in.in[0], 0x04);
    else if (name == "B") low8(g_in.in[0], 0x08);
    else if (name == "P1 Foot") high8(g_in.in[0], 0x10);
    else if (name == "P2 Foot") high8(g_in.in[0], 0x20);
    else if (name == "Turntable") g_in.turntable[port == "TURNTABLE_P2" ? 1 : 0] = (uint16_t)(value & 0x3ff);
    else if (name.rfind("Effect ", 0) == 0) {
        int k = std::stoi(name.substr(7));
        if (k < 1 || k > 7) return false;
        g_in.effect[k - 1] = (uint8_t)(value & 0x1f);
    }
    else return false;
    return true;
} catch (const std::logic_error &) {   // "P1 Button R": no number where this machine's names have one
    return false;
}

void turntable_add(int player, int delta) {
    uint16_t &t = g_in.turntable[player & 1];
    t = (uint16_t)((t + delta) & 0x3ff);
}

void set_frame_hook(FrameHook h) { g_frame_hook = std::move(h); }
void set_present_hook(PresentHook h) { g_present_hook = std::move(h); }
void frame_picture_unused() { g_frame_picture = false; }

double frame_rate() { return g_gcu->pixclock() / ((double)g_gcu->htotal() * g_gcu->vtotal()); }

const uint32_t *render_between(double t, int &w, int &h) {
    static std::vector<uint32_t> between;
    if (g_gcu2) return nullptr;
    w = g_gcu->hvisible(), h = g_gcu->vvisible();
    between.resize((size_t)w * h);
    return g_gcu->draw_between(between.data(), w, h, t) ? between.data() : nullptr;
}
void set_audio_sink(AudioSink sink) { g_audio_sink = std::move(sink); }
int screens() { return g_gcu2 ? 2 : 1; }

bool sound_flash_blank() {
    if (g_cfg.family != "kbm" && g_cfg.family != "ppp") return false;
    for (int k = 1; k <= 2; k++) {
        const auto &d = g_flash[k].data();
        if (std::any_of(d.begin(), d.end(), [](uint8_t b) { return b != 0xff; })) return false;
    }
    return true;
}

uint64_t ymz_key_ons() { return g_ymz ? g_ymz->key_ons : 0; }
uint64_t midi_out_bytes() { return g_midi_out_bytes; }
uint64_t frame_count() { return g_frame; }
const std::vector<uint8_t> &lamp_state() { return g_lamps; }
uint64_t lamp_changes() { return g_lamp_changes; }
void set_trace_ctx(hk_ctx *c) { g_ctx_for_trace = c; }

}  // namespace hk::fb
