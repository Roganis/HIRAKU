// SPDX-License-Identifier: GPL-3.0-or-later
// PPC403GCX on-chip peripherals. Behaviour ported from MAME's ppccom.cpp
// (license BSD-3-Clause, copyright-holders: Aaron Giles), adapted to HIRAKU's
// virtual-time scheduler.
#include "cpu403.h"

#include <deque>

#include "runtime.h"
#include "sched.h"

namespace hk::cpu403 {

namespace {

void pit_fire();
void fit_fire();
void spu_fire();
Timer g_pit_timer("pit", pit_fire);
Timer g_fit_timer("fit", fit_fire);
Timer g_spu_timer("spu", spu_fire);

uint32_t g_irqstate = 0;        // current level of every interrupt input
uint32_t g_pit_reload = 0;
uint64_t g_pit_zero = 0;        // tick at which the PIT reaches zero (when running)
bool g_pit_running = false;

void pit_fire() {
    g_cpu.sprs[spr::TSR] |= TSR_PIS;
    g_pit_running = false;
    if ((g_cpu.sprs[spr::TCR] & TCR_PIE) && g_pit_reload && (g_cpu.sprs[spr::TCR] & TCR_ARE)) {
        g_pit_zero = now() + g_pit_reload + 1;
        g_pit_running = true;
        g_pit_timer.at(g_pit_zero);
    }
    sched_kick();
}

void fit_arm() {
    if (!(g_cpu.sprs[spr::TCR] & TCR_FIE)) return;
    uint64_t tb = now();
    uint64_t interval = 0x200ull << (4 * ((g_cpu.sprs[spr::TCR] & TCR_FP) >> 24));
    uint64_t target = (tb + interval) & ~(interval - 1);
    g_fit_timer.at(target + 1);
}

void fit_fire() {
    g_cpu.sprs[spr::TSR] |= TSR_FIS;
    fit_arm();
    sched_kick();
}

void pit_arm() {
    if ((g_cpu.sprs[spr::TCR] & TCR_PIE) && g_pit_reload) {
        g_pit_zero = now() + g_pit_reload + 1;
        g_pit_running = true;
        g_pit_timer.at(g_pit_zero);
    } else {
        g_pit_running = false;
        g_pit_timer.cancel();
    }
}

// ---- serial port -----------------------------------------------------------
enum { LINE_STATUS = 0, HANDSHAKE = 2, BRDH = 4, BRDL = 5, CONTROL = 6, RX_COMMAND = 7, TX_COMMAND = 8, BUFFER = 9 };
uint8_t g_spu_regs[16];
uint8_t g_spu_txbuf = 0, g_spu_rxbuf = 0;
std::deque<uint8_t> g_spu_rx;
std::function<void(uint8_t)> g_spu_tx;

void spu_update_irqs() {
    uint8_t ls = g_spu_regs[LINE_STATUS];
    bool rx = ((g_spu_regs[RX_COMMAND] & 0x60) == 0x20 && (ls & 0x80)) || ((g_spu_regs[RX_COMMAND] & 0x10) && (ls & 0x78));
    bool tx = ((g_spu_regs[TX_COMMAND] & 0x60) == 0x20 && (ls & 0x04)) || ((g_spu_regs[TX_COMMAND] & 0x10) && (ls & 0x02));
    set_irq_line(IRQ_SPUR, rx);
    set_irq_line(IRQ_SPUT, tx);
}

uint64_t spu_char_ticks() {
    // clock: system clock unless IOCR selects the external serial clock (3.6864 MHz in MAME)
    double clk = (g_cpu.dcrs[dcr::IOCR] & 0x02) ? 3'686'400.0 : (double)CPU_HZ;
    int divisor = ((g_spu_regs[BRDH] * 256 + g_spu_regs[BRDL]) & 0xfff) + 1;
    int bpc = 7 + ((g_spu_regs[CONTROL] & 8) >> 3) + 1 + (g_spu_regs[CONTROL] & 1);
    return (uint64_t)((double)CPU_HZ / clk * divisor * 16 * bpc);
}

void spu_timer_reset() {
    if ((g_spu_regs[RX_COMMAND] | g_spu_regs[TX_COMMAND]) & 0x80) g_spu_timer.in(spu_char_ticks());
    else g_spu_timer.cancel();
}
void spu_fire() {
    uint8_t &ls = g_spu_regs[LINE_STATUS];
    if (g_spu_regs[TX_COMMAND] & 0x80) {
        if (!(ls & 0x04)) {
            if (g_spu_tx) g_spu_tx(g_spu_txbuf);
            ls |= 0x04;
            ls &= ~0x02;
        } else if (!(ls & 0x02)) {
            ls |= 0x02;
        }
    }
    if ((g_spu_regs[RX_COMMAND] & 0x80) && !g_spu_rx.empty()) {
        uint8_t b = g_spu_rx.front();
        g_spu_rx.pop_front();
        if (!(ls & 0x80)) {
            g_spu_rxbuf = b;
            ls |= 0x80;
        } else {
            ls |= 0x20;   // overrun
        }
    }
    spu_update_irqs();
    spu_timer_reset();    // periodic while enabled
}
}  // namespace

// ---------------------------------------------------------------------------

void reset() {
    g_irqstate = 0;
    g_pit_reload = 0;
    g_pit_running = false;
    for (auto &r : g_spu_regs) r = 0;
    g_spu_regs[LINE_STATUS] = 0x06;   // transmitter empty, shift register empty
    g_spu_rx.clear();
}

void set_irq_line(uint32_t bit, bool asserted) {
    uint32_t old = g_irqstate;
    if (asserted) g_irqstate |= bit;
    else g_irqstate &= ~bit;
    uint32_t &exisr = g_cpu.dcrs[dcr::EXISR];
    if (((g_irqstate ^ old) & bit) && (g_irqstate & bit)) exisr |= bit;   // edge: latch
    // level-triggered sources pass straight through
    uint32_t level = IRQ_CRITICAL | IRQ_SPUR | IRQ_SPUT | (0x80000000u >> 6) | (0x80000000u >> 7) |
                     IRQ_DMA0 | (IRQ_DMA0 >> 1) | (IRQ_DMA0 >> 2) | (IRQ_DMA0 >> 3);
    uint32_t iocr = g_cpu.dcrs[dcr::IOCR];
    for (int n = 0; n < 5; n++)
        if (!(iocr & (0x80000000u >> (2 * n)))) level |= irq_ext(n);
    exisr = (exisr & ~level) | (g_irqstate & level);
    if (irq_pending()) sched_kick();
}

bool irq_pending() {
    if (g_cpu.dcrs[dcr::EXISR] & g_cpu.dcrs[dcr::EXIER]) return true;
    uint32_t tcr = g_cpu.sprs[spr::TCR], tsr = g_cpu.sprs[spr::TSR];
    return ((tcr & TCR_FIE) && (tsr & TSR_FIS)) || ((tcr & TCR_PIE) && (tsr & TSR_PIS));
}

uint32_t pending_vector() {
    if (!irq_pending()) return 0;
    uint32_t v = 0x500, tsr = g_cpu.sprs[spr::TSR];
    if (tsr & TSR_PIS) v = 0x1000;
    if (tsr & TSR_FIS) v = 0x1010;
    if (g_cpu.dcrs[dcr::EXISR] & g_cpu.dcrs[dcr::EXIER]) v = 0x500;
    return v;
}

void write_tcr(uint32_t v) {
    uint32_t old = g_cpu.sprs[spr::TCR];
    g_cpu.sprs[spr::TCR] = v | (old & 0x30000000);   // WRC bits are sticky
    if ((old ^ v) & TCR_FIE) {
        if (v & TCR_FIE) fit_arm(); else g_fit_timer.cancel();
    }
    if ((old ^ v) & TCR_PIE) pit_arm();
    if (irq_pending()) sched_kick();
}

void write_tsr(uint32_t v) {
    g_cpu.sprs[spr::TSR] &= ~v;
}

void write_pit(uint32_t v) {
    g_cpu.sprs[spr::PIT] = v;
    g_pit_reload = v;
    pit_arm();
}

uint32_t read_pit() {
    if (!g_pit_running) return 0;
    uint64_t t = now();
    return t >= g_pit_zero ? 0 : (uint32_t)(g_pit_zero - t - 1);
}

void dcr_written(uint32_t n, uint32_t v) {
    switch (n) {
    case dcr::EXISR:                                  // write 1 to clear
        g_cpu.dcrs[n] &= ~v;
        // ... but a level-triggered line that is still asserted stays pending (MAME:
        // ppc4xx_set_irq_line(0, 0) after the write): an interrupt asked for again between a
        // handler's last look at its device and this acknowledge isn't lost
        set_irq_line(0, false);
        break;
    case dcr::EXIER: g_cpu.dcrs[n] = v; break;
    case dcr::IOCR: g_cpu.dcrs[n] = v; break;
    default:
        if (n >= 0xc0 && n <= 0xdf) {                  // DMA channel registers
            g_cpu.dcrs[n] = v;
            uint32_t ch = (n - 0xc0) / 8, reg = (n - 0xc0) % 8;
            if (reg == 0 && (v & 0x80000000u)) {        // DMACRn with channel enable
                uint32_t mode = (v >> 21) & 3;
                if (mode != 2)
                    fatal(nullptr, "DMA channel %u: transfer mode %u not implemented (DMACR %08x)", ch, mode, v);
                // software-initiated memory-to-memory: do it now
                uint32_t width = 1u << ((v >> 26) & 3);
                uint32_t count = g_cpu.dcrs[0xc0 + ch * 8 + 1] & 0xffff;
                uint32_t dst = g_cpu.dcrs[0xc0 + ch * 8 + 2], src = g_cpu.dcrs[0xc0 + ch * 8 + 3];
                int sinc = (v & 0x01000000) ? (int)width : 0, dinc = (v & 0x02000000) ? (int)width : 0;
                hk_ctx dummy{};
                for (uint32_t k = 0; k < count; k++, src += sinc, dst += dinc) {
                    switch (width) {
                    case 1: hk_w8(&dummy, dst, hk_r8(&dummy, src)); break;
                    case 2: hk_w16(&dummy, dst, hk_r16(&dummy, src)); break;
                    default: hk_w32(&dummy, dst, hk_r32(&dummy, src)); break;
                    }
                }
                // no time charged: MAME's ppc4xx_dma_exec does a memory-to-memory transfer at once
                // (it was 2 cycles a unit, a guess: ~76 ms over pop'n music's 2.5 M-word GCU uploads)
                g_cpu.dcrs[0xc0 + ch * 8 + 1] = 0;
                g_cpu.dcrs[0xc0 + ch * 8 + 2] = dst;
                g_cpu.dcrs[0xc0 + ch * 8 + 3] = src;
                g_cpu.dcrs[0xc0 + ch * 8] &= ~0x80000000u;
                g_cpu.dcrs[0xe0] |= 0x80000000u >> ch;   // DMASR: terminal count reached
                if (v & 0x40000000u) set_irq_line(IRQ_DMA0 >> ch, true);
            }
        } else if (n == 0xe0) {                         // DMASR: write 1 to clear
            g_cpu.dcrs[n] &= ~v;
            for (int ch = 0; ch < 4; ch++)
                if (!(g_cpu.dcrs[n] & (0x88000000u >> ch))) set_irq_line(IRQ_DMA0 >> ch, false);
        } else {
            g_cpu.dcrs[n & 1023] = v;
        }
        return;
    }
    if (irq_pending()) sched_kick();
}

uint8_t spu_read(uint32_t off) {
    if (off == BUFFER) {
        g_spu_regs[LINE_STATUS] &= ~0x80;
        spu_update_irqs();
        return g_spu_rxbuf;
    }
    return off < 16 ? g_spu_regs[off] : 0xff;
}

void spu_write(uint32_t off, uint8_t data) {
    switch (off) {
    case LINE_STATUS:
        g_spu_regs[LINE_STATUS] &= ~(data & 0xf8);
        spu_update_irqs();
        break;
    case RX_COMMAND:
    case TX_COMMAND: {
        uint8_t old = g_spu_regs[RX_COMMAND] | g_spu_regs[TX_COMMAND];
        g_spu_regs[off] = data;
        if ((old ^ (g_spu_regs[RX_COMMAND] | g_spu_regs[TX_COMMAND])) & 0x80) spu_timer_reset();
        spu_update_irqs();
        break;
    }
    case BRDH:
    case BRDL:
        if (data != g_spu_regs[off]) { g_spu_regs[off] = data; spu_timer_reset(); }
        break;
    case CONTROL: {
        uint8_t old = g_spu_regs[off];
        g_spu_regs[off] = data;
        if ((old ^ data) & 0x09) spu_timer_reset();
        break;
    }
    case BUFFER:
        g_spu_txbuf = data;
        g_spu_regs[LINE_STATUS] &= ~0x04;
        break;
    default:
        if (off < 16) g_spu_regs[off] = data;
        break;
    }
}

void spu_set_tx_handler(std::function<void(uint8_t)> fn) { g_spu_tx = std::move(fn); }
void spu_receive(uint8_t b) { g_spu_rx.push_back(b); }

}  // namespace hk::cpu403
