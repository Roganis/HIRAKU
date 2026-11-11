// SPDX-License-Identifier: GPL-3.0-or-later
// The System 573 board: MAME's ksys573.cpp (BSD-3-Clause, R. Belmont, smf) as a
// device tree on the framework shim, with MAME's own devices. The driver class
// below follows ksys573_state and ddr_state; where it differs, a comment says so.
#include "sys573/board.h"
#include "sys573/mips.h"

#include "shell/cabinet.h"
#include "shell/sys.h"

#include "emu.h"
#include "screen.h"
#include "speaker.h"

#include "cpu/psx/psx.h"
#include "video/psx.h"
#include "sound/spu.h"
#include "sound/cdda.h"
#include "sound/mas3507d.h"
#include "machine/ram.h"
#include "machine/intelfsh.h"
#include "machine/timekpr.h"
#include "machine/adc083x.h"
#include "bus/ata/ataintf.h"
#include "bus/ata/cr589.h"
#include "bus/pccard/pccard.h"
#include "bus/pccard/linflash.h"
#include "sys573/link.h"
#include "sys573/cardunit.h"
#include "sys573/rental.h"
#include "sys573/mcal.h"
#include "sys573/eagfdm.h"
#include "sys573/npu.h"
#include "imagedev/cdromimg.h"
#include "k573cass.h"
#include "k573dio.h"
#include "k573mcr.h"
#include "k573msu.h"
#include "sound/tc9446f.h"
#include "cpu/psx/sio_uart.h"
#include "machine/ins8250.h"
#include "machine/jvshost.h"
#include "dirtc.h"
#include <numeric>

#include <sys/stat.h>

#include <cstdio>
#include <map>

namespace hk::k573 {

namespace {

constexpr int ATAPI_CYCLES_PER_SECTOR = 30000;   // MAME: plenty of time to allow DMA setup etc.

std::vector<u8> load_file(const std::string &path) {
    std::vector<u8> d;
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return d;
    fseek(f, 0, SEEK_END);
    d.resize((size_t)ftell(f));
    fseek(f, 0, SEEK_SET);
    if (fread(d.data(), 1, d.size(), f) != d.size()) d.clear();
    fclose(f);
    return d;
}

// ksys573.cpp's sys573_jvs_host: the JVS host as the 573 reads and writes it
class sys573_jvs_host : public jvs_host {
public:
    sys573_jvs_host(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock = 0);
    void send_packet(u8 *data, int length) {
        while (length > 0) { push(*data); data++; length--; }
        commit_raw();
    }
    int received_packet(u8 *buffer) {
        if (jvs_sense_r()) return 0;   // the reset command is sent twice; no reply to it
        u32 length;
        const u8 *data;
        get_raw_reply(data, length);
        if (length > 0) {
            // MAME: the games don't unescape the data in memory; send it unescaped
            u8 checksum = std::accumulate(data, data + length, 0);
            buffer[0] = 0xe0;
            memcpy(buffer + 1, data, length);
            buffer[length + 1] = checksum;
            buffer[length + 2] = 0;
            length += 2;
            commit_encoded();
        }
        return (int)length;
    }
    int jvs_sense_r() { return !get_address_set_line(); }
};
const device_creator<sys573_jvs_host> SYS573_JVS_HOST("sys573_jvs_host", "JVS Host (System 573)");
sys573_jvs_host::sys573_jvs_host(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock)
    : jvs_host(mconfig, SYS573_JVS_HOST, tag, owner, clock) {}

// The game's configuration while the board is built: the shim runs device_add_mconfig as each
// device is constructed, before init() can hand the board its cfg.
const Config *g_build = nullptr;
SessionLink g_link;   // the session cable to another cabinet's process (link.h), when there is one
Eamuse g_eamuse;      // the network unit's network and e-AMUSEMENT server (eagfdm.h), with the unit
CardUnit g_cards;     // the e-AMUSEMENT card units (cardunit.h), with the network unit
RentalUnit g_rental;  // Mambo a GoGo's e-Amusement rental unit (rental.h)

// MAME's ioport output field: the write callback runs when its bits change
// (dynamic_field::write), with the active-low inversion of the field's default.
struct OutField {
    u32 mask;
    bool active_low;
    std::function<void(int)> write;
    u32 old = 0;
};

class sys573_state : public device_t {
public:
    sys573_state(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock = 0)
        : device_t(mconfig, SYS573_STATE, tag, owner, clock),
          m_maincpu(*this, "maincpu"),
          m_ram(*this, "ram"),
          m_gpu(*this, "gpu"),
          m_gpu_ram(*this, "gpu_ram"),
          m_spu(*this, "spu"),
          m_spu_ram(*this, "spu_ram"),
          m_ata(*this, "ata"),
          m_cassette(*this, "cassette"),
          m_adc(*this, "adc0834"),
          m_m48t58(*this, "m48t58"),
          m_pccard1(*this, "pccard1"),
          m_pccard2(*this, "pccard2"),
          m_k573dio(*this, "k573dio"),
          m_sys573_jvs_host(*this, "sys573_jvs_host"),
          m_screen(*this, "screen") {}

    static const device_creator<sys573_state> SYS573_STATE;

    Config cfg;

    required_device<psxcpu_device> m_maincpu;
    required_device<ram_device> m_ram;
    required_device<psxgpu_device> m_gpu;
    required_device<ram_device> m_gpu_ram;
    required_device<spu_device> m_spu;
    required_device<ram_device> m_spu_ram;
    required_device<ata_interface_device> m_ata;
    required_device<konami573_cassette_slot_device> m_cassette;
    required_device<adc0834_device> m_adc;
    required_device<timekeeper_device> m_m48t58;
    required_device<pccard_slot_device> m_pccard1, m_pccard2;
    optional_device<k573dio_device> m_k573dio;
    required_device<sys573_jvs_host> m_sys573_jvs_host;
    required_device<screen_device> m_screen;
    fujitsu_29f016a_device *m_flash[8] = {};   // 31m 27m 31l 27l 31j 27j 31h 27h

    // ---- state (ksys573_state) ----
    u16 m_control = 0, m_n_security_control = 0;
    int m_pccard_cd[2] = {1, 1};   // MAME: card detect is active low (no card = 1)
    u32 *m_p_n_psxram = nullptr;
    u32 m_atapi_xferbase = 0;
    int m_atapi_xfersize = 0;
    emu_timer *m_atapi_timer = nullptr;
    const u8 *m_h8_response = nullptr;
    int m_h8_length = 0, m_h8_index = 0, m_h8_clk = 0;
    u32 m_out0 = 0, m_out1 = 0, m_out2 = 0;
    int m_jvs_input_idx_r = 0, m_jvs_input_idx_w = 0, m_jvs_output_idx_w = 0, m_jvs_output_len_w = 0;
    u8 m_jvs_input_buffer[512] = {}, m_jvs_output_buffer[512] = {};
    std::vector<OutField> m_out0_fields, m_out1_fields, m_out2_fields;
    u8 m_gx700pwbf_output_data[4] = {};   // the analog I/O board's output latches

    // ddr_state: the GN845-PWB(B) stage multiplexor
    struct stage_state { int DO = 0, clk = 0, shift = 0, state = 0, bit = 0; } m_stage_state[2];
    u32 m_stage_mask = 0xffffffff;
    u32 m_stage = 0x0f0f;     // STAGE port: panels, active low

    // inputs (active level as the port reads it)
    u32 m_in2_buttons = 0xf0f0;   // IN2 outside the stage bits: starts and buttons, active low
    u32 m_in3 = 0x0f000f00;       // IN3: P1/P2 buttons 4-6, service: active low
    u32 m_in1_coins = 0x13000000; // coins and service 1, active low
    u8 m_analog[4] = {0, 0, 0, 0};

    u64 m_outputs = 0;              // the I/O board's outputs, lit = 1, by output number; 32 + k: LAMPS bit k
    std::vector<u8> m_lamps;        // in cab::lamps(family) order
    u64 m_lamp_changes = 0;

    void device_add_mconfig(machine_config &config) override;
    void device_start() override;   // MAME's driver start: after every other device
    void device_reset_after_children() override;   // machine_reset

    void konami573_map(address_map &map);
    void konami573a_map(address_map &map);
    void konami573d_map(address_map &map);

    // ---- handlers (ksys573.cpp) ----
    u16 flash_r(offs_t offset, u16 mem_mask);
    void flash_w(offs_t offset, u16 data, u16 mem_mask);
    u32 in0_r() { return 0xffffffff; }
    u32 in1_r();
    u16 port_in2_jvs_r(offs_t offset, u16 mem_mask);
    u32 in3_r() { return m_in3; }
    void out0_w(offs_t, u32 data, u32 mem_mask) { port_write(m_out0, m_out0_fields, data, mem_mask); }
    u16 control_r(offs_t, u16) { return m_control; }
    void control_w(offs_t, u16 data, u16 mem_mask);
    void atapi_reset_w(u16 data) { if (!(data & 1)) m_ata->reset(); }
    // MAME's LAMPS port: GuitarFreaks' lamps (gtrfrks_lamps_b4..b7, active low); Dance Maniax's
    // (dmx_lamps_b0..b5) and Mambo a GoGo's (mamboagg_lamps_b3..b5), active low, as lamps 32 + bit
    void lamps_w(offs_t, u32 data, u32 mem_mask) {
        if (cfg.family == "dmx" || cfg.family == "mambo") {
            const u32 used = cfg.family == "dmx" ? 0x3f : 0x38;
            for (int k = 0; k < 8; k++)
                if (used & mem_mask & (1u << k)) set_lamp(32 + k, !(data & (1u << k)));
            return;
        }
        if (cfg.family != "gtrf" || !(mem_mask & 0xf0)) return;
        for (int k = 0; k < 4; k++) set_lamp(k, !(data & (0x80u >> k)));   // spot L, spot R, start L, start R
    }
    u16 jvs_input_r(offs_t, u16);
    void jvs_input_w(offs_t, u16 data, u16);
    bool jvs_is_valid_packet();
    int jvs_rx_r();
    u16 security_r(offs_t, u16) { return m_n_security_control; }
    void security_w(offs_t, u16 data, u16 mem_mask);

    void port_write(u32 &port, std::vector<OutField> &fields, u32 data, u32 mem_mask);
    void ata_interrupt(int state) {
        if (state && m_hk_in_xfer) m_hk_block_raised = true;   // a fresh block, raised while the DMA read the last one
        m_maincpu->subdevice<psxirq_device>("irq")->intin10(state);
    }
    bool m_hk_in_xfer = false, m_hk_block_raised = false;   // atapi_xfer_end's departure from MAME
    bool m_hk_exact_dma = false;                            // ... for the game that needs it (gtrfrk9m)
    void atapi_xfer_end(s32);
    void cdrom_dma_read(u32 *, u32 n_address, s32 n_size) { (void)n_address; (void)n_size; }
    void cdrom_dma_write(u32 *ram, u32 n_address, s32 n_size);
    void h8_clk_w(int state);
    int h8_d(int bit) { return (m_h8_response[m_h8_index] >> bit) & 1; }
    double analogue_inputs_callback(u8 input);

    // GX700-PWB(F), the analog I/O board
    u16 gx700pwbf_io_r(offs_t offset, u16 mem_mask);
    void gx700pwbf_io_w(offs_t offset, u16 data, u16 mem_mask);
    void gx700pwbf_output(int offset, u8 data);

    // ddr_state
    void gn845pwbb_do_w(int offset, int data) { m_stage_state[offset].DO = !data; }
    void gn845pwbb_clk_w(int offset, int data);
    void ddr_output_callback(offs_t offset, u8 data);
    void drmn_output_callback(offs_t offset, u8 data);
    void ddrsolo_output_callback(offs_t offset, u8 data);
    void dmx_output_callback(offs_t offset, u8 data);
    void mamboagg_output_callback(offs_t offset, u8 data);
    void set_lamp(offs_t offset, bool lit);
};

const device_creator<sys573_state> sys573_state::SYS573_STATE("sys573", "Konami System 573");

// ---- configuration: konami573(), then k573d() + ddr3m() or k573a() + ddr() -------------------
void sys573_state::device_add_mconfig(machine_config &config) {
    const Config &c = *g_build;
    const bool digital = c.io_board == "digital";
    CXD8530CQ(config, m_maincpu, 67.7376_MHz_XTAL);
    m_maincpu->set_addrmap(AS_PROGRAM, digital ? address_map_constructor(&sys573_state::konami573d_map, this)
                                               : address_map_constructor(&sys573_state::konami573a_map, this));
    m_maincpu->set_ram(m_ram);
    m_maincpu->subdevice<psxdma_device>("dma")->install_read_handler(5, psxdma_device::read_delegate(&sys573_state::cdrom_dma_read, this));
    m_maincpu->subdevice<psxdma_device>("dma")->install_write_handler(5, psxdma_device::write_delegate(&sys573_state::cdrom_dma_write, this));

    RAM(config, m_ram).set_bits(32).set_default_size("4M").set_default_value(0);

    ATA_INTERFACE(config, m_ata);
    m_ata->irq_handler().set(*this, &sys573_state::ata_interrupt);
    m_ata->slot(0).option_add("cr589", CR589);
    m_ata->slot(0).set_option_machine_config("cr589", [](device_t *) {});   // MAME: cdda routes, disc regions
    m_ata->slot(0).set_default_option("cr589");

    KONAMI573_CASSETTE_SLOT(config, m_cassette);
    if (c.msu) {
        // the multi-session unit on SIO1 (a UART then: hk_psx_sio1_uart), as the fork's msu_local()
        auto &sio1 = *m_maincpu->subdevice<psxsio1_uart_device>("sio1");
        m_cassette->dsr_handler().set(sio1, &psxsio1_uart_device::write_dsr);
        auto &msu = KONAMI_573_MULTI_SESSION_UNIT(config, "k573msu", 0);
        ns16550_device &uart = msu.link_uart();
        if (g_cards.fitted()) {
            // SIO1 switched between the unit and the card units by the cassette's d7 (the game's
            // swuart: EXTIO or MSU); the side switched out sends into nothing and hears idle. SIO1's
            // CTS is the switched-in side's: the unit's RTS, or ready for the card units (the 573
            // waits on it to send: at boot, while the unit starts, its RTS held the card check's
            // bytes back)
            static int unit_rts = 0;
            sio1.txd_handler().set([&uart](int s) { if (g_cards.selected()) g_cards.txd(s); else uart.rx_w(s); });
            uart.out_tx_callback().set([&sio1](int s) { if (!g_cards.selected()) sio1.write_rxd(s); });
            uart.out_rts_callback().set([&sio1](int s) { unit_rts = s; if (!g_cards.selected()) sio1.write_cts(s); });
            g_cards.on_select = [&sio1](bool extio) { sio1.write_cts(extio ? 0 : unit_rts); };
        } else {
            sio1.txd_handler().set(uart, &ins8250_uart_device::rx_w);
            uart.out_tx_callback().set(sio1, &psxsio1_uart_device::write_rxd);
            uart.out_rts_callback().set(sio1, &psxsio1_uart_device::write_cts);
        }
        sio1.dtr_handler().set(uart, &ins8250_uart_device::dsr_w);
        sio1.rts_handler().set(uart, &ins8250_uart_device::cts_w);
        // not the unit's DTR: SIO1's DSR is the cassette's presence (the fork wires both to it, and
        // the unit's firmware drops DTR as it sets up its UARTs, so the game lost its cassette:
        // ERROR 120)
        if (!c.session_host.empty()) {
            // the other cabinet (GuitarFreaks) on the unit's second port, the fork's msu_port2,
            // wired as SIO1 is to the first
            auto &port2 = *msu.subdevice<ns16550_device>("duart_com_0:chan0");
            port2.out_tx_callback().set([](int s) { g_link.out(SessionLink::TXD, s); });
            port2.out_dtr_callback().set([](int s) { g_link.out(SessionLink::DTR, s); });
            port2.out_rts_callback().set([](int s) { g_link.out(SessionLink::RTS, s); });
            g_link.in = [&port2](SessionLink::Line l, int s) {
                if (l == SessionLink::TXD) port2.rx_w(s);
                else if (l == SessionLink::DTR) port2.dsr_w(s);
                else port2.cts_w(s);
            };
        }
    } else if (!c.session_join.empty() || !c.session_host.empty()) {
        // a cabinet joining another's multi-session unit (the fork's msu_remote): SIO1 a UART on
        // the session cable; the unit's TXD and RTS come back, not its DTR (SIO1's DSR is the
        // cassette's, as above). The mixes before the unit (DrumMania 2nd's Session Power Up Kit
        // and 3rd, GuitarFreaks 3rd and 4th) join SIO1 to SIO1: either side is wired so, the host
        // the one that waits for the other
        auto &sio1 = *m_maincpu->subdevice<psxsio1_uart_device>("sio1");
        m_cassette->dsr_handler().set(sio1, &psxsio1_uart_device::write_dsr);
        sio1.txd_handler().set([](int s) {
            if (g_cards.selected()) g_cards.txd(s);   // the card units switched in (d7), as above
            else g_link.out(SessionLink::TXD, s);
        });
        sio1.dtr_handler().set([](int s) { g_link.out(SessionLink::DTR, s); });
        sio1.rts_handler().set([](int s) { g_link.out(SessionLink::RTS, s); });
        static int link_rts = 0;   // the far unit's RTS: SIO1's CTS while the card units are switched out (as above)
        g_link.in = [&sio1](SessionLink::Line l, int s) {
            if (l == SessionLink::TXD) { if (!g_cards.selected()) sio1.write_rxd(s); }
            else if (l == SessionLink::RTS) { link_rts = s; if (!g_cards.selected()) sio1.write_cts(s); }
        };
        g_cards.on_select = [&sio1](bool extio) { sio1.write_cts(extio ? 0 : link_rts); };
    } else if (c.rental) {
        // the e-Amusement rental unit alone on SIO1 (rental.h): its TXD and RXD; CTS held ready
        auto &sio1 = *m_maincpu->subdevice<psxsio1_uart_device>("sio1");
        m_cassette->dsr_handler().set(sio1, &psxsio1_uart_device::write_dsr);
        sio1.txd_handler().set([](int s) { g_rental.txd(s); });
    } else if (g_cards.fitted()) {
        // the card units alone on SIO1 (no session)
        auto &sio1 = *m_maincpu->subdevice<psxsio1_uart_device>("sio1");
        m_cassette->dsr_handler().set(sio1, &psxsio1_uart_device::write_dsr);
        sio1.txd_handler().set([](int s) { if (g_cards.selected()) g_cards.txd(s); });
    } else {
        m_cassette->dsr_handler().set(*m_maincpu->subdevice<psxsio1_device>("sio1"), &psxsio1_device::write_dsr);
    }

    static const char *const flash_tags[8] = {"29f016a.31m", "29f016a.27m", "29f016a.31l", "29f016a.27l",
                                              "29f016a.31j", "29f016a.27j", "29f016a.31h", "29f016a.27h"};
    for (int k = 0; k < 8; k++) m_flash[k] = &FUJITSU_29F016A(config, flash_tags[k]);

    PCCARD_SLOT(config, m_pccard1);
    m_pccard1->cd1().set([this](int state) { m_pccard_cd[0] = state; });
    PCCARD_SLOT(config, m_pccard2);
    m_pccard2->cd1().set([this](int state) { m_pccard_cd[1] = state; });

    CXD8561Q(config, m_gpu, 67.7376_MHz_XTAL / 2);
    m_gpu->set_cpu(m_maincpu);
    m_gpu->set_ram(m_gpu_ram);
    m_gpu->set_screen("screen");
    m_gpu->set_vclkn(53.693175_MHz_XTAL);
    RAM(config, m_gpu_ram).set_bits(16).set_default_size("2M").set_default_value(0);

    SCREEN(config, "screen");
    SPEAKER(config, "speaker", 2).front();

    SPU(config, m_spu, 67.7376_MHz_XTAL / 2);
    m_spu->set_cpu(m_maincpu);
    m_spu->set_ram(m_spu_ram);
    RAM(config, m_spu_ram).set_bits(16).set_default_size("512K").set_default_value(0);

    M48T58(config, "m48t58");
    ADC0834(config, "adc0834").set_input_callback(*this, &sys573_state::analogue_inputs_callback);
    SYS573_JVS_HOST(config, m_sys573_jvs_host);

    if (digital) {
        // k573d(): the digital I/O board; ddr3m(): DDR's outputs on it
        KONAMI_573_DIGITAL_IO_BOARD(config, m_k573dio, XTAL(19'660'800));
        // ddr3m(): DDR's outputs; drmn2m() to drmn10m(): DrumMania's
        if (c.family == "drmn") m_k573dio->output_callback().set(*this, &sys573_state::drmn_output_callback);
        else if (c.family == "ddrsolo") m_k573dio->output_callback().set(*this, &sys573_state::ddrsolo_output_callback);
        else if (c.family == "dmx") m_k573dio->output_callback().set(*this, &sys573_state::dmx_output_callback);       // dmx()
        else if (c.family == "mambo") m_k573dio->output_callback().set(*this, &sys573_state::mamboagg_output_callback);   // mamboagg()
        else m_k573dio->output_callback().set(*this, &sys573_state::ddr_output_callback);
        // ddrsbm(): Solo Bass Mix's own FPGA program, whose MP3 counter runs free from power-on (the
        // game syncs its playback to it; without, it waits at READY with the counter at 0)
        if (c.game == "ddrsbm") m_k573dio->set_ddrsbm_fpga(true);
    }
    // GuitarFreaks 9th: atapi_xfer_end's departure from MAME (the DMA hack misfires on its /DATA4 read)
    m_hk_exact_dma = c.game == "gtrfrk9m";
    // k573a(): the analog board is the driver's own handlers (konami573a_map); init_ddr() sends
    // its outputs to ddr_output_callback (gx700pwbf_output)
    if (c.pccard1 == "32mb") {   // pccard1_32mb() (MAME also offers id245p01 there)
        m_pccard1->option_add("32mb", FUJITSU_32MB_FLASH_CARD);
        m_pccard1->set_default_option("32mb");
    } else if (c.pccard1 == "16mb") {   // pccard1_16mb(): DDR 2nd Mix Link, Best of Cool Dancers
        m_pccard1->option_add("16mb", FUJITSU_16MB_FLASH_CARD);
        m_pccard1->set_default_option("16mb");
    }
    if (c.pccard2 == "32mb") {   // pccard2_32mb()
        m_pccard2->option_add("32mb", FUJITSU_32MB_FLASH_CARD);
        m_pccard2->set_default_option("32mb");
    } else if (c.pccard2 == "npu") {   // the network PCB unit (MAME: a stub, not fitted), high-level
        m_pccard2->option_add("npu", KONAMI_573_NPU_HLE);
        m_pccard2->set_default_option("npu");
        const std::string nv = c.nvram_dir;
        m_pccard2->set_option_machine_config("npu", [nv](device_t *d) {
            auto *npu = static_cast<k573npu_hle_device *>(d);
            npu->set_dirs(nv.empty() ? "" : nv + "/npu_hdd", nv.empty() ? "" : nv + "/npu_ram");
            npu->set_lan(g_eamuse.lan());
        });
    }
    // the cassette: konami573() adds cassx(); ddr3m's cassyyi() replaces it
    // (cassyyi() and cassxxi() add the install cassette, of the game's kind)
    // (cassxzi() pairs a ZS01 game cassette with an X76F041 install cassette: install_kind)
    for (const char *slot : {"game", "install"}) {
        if (!strcmp(slot, "install") && c.install_eeprom.empty()) continue;
        const std::string &kind = !strcmp(slot, "install") && !c.install_kind.empty() ? c.install_kind : c.cassette;
        if (kind == "zi") m_cassette->option_add(slot, KONAMI573_CASSETTE_ZI);   // casszi(): ZS01 + DS2401
        else if (kind == "yi") m_cassette->option_add(slot, KONAMI573_CASSETTE_YI);
        else if (kind == "xi") m_cassette->option_add(slot, KONAMI573_CASSETTE_XI);
        else if (kind == "y") m_cassette->option_add(slot, KONAMI573_CASSETTE_Y);
        else m_cassette->option_add(slot, KONAMI573_CASSETTE_X);
    }
    m_cassette->set_default_option(c.cassette_slot.c_str());
    if (c.memory_card_reader) KONAMI_573_MEMORY_CARD_READER(config, "k573mcr", m_sys573_jvs_host);
    // the Master Calendar (mcal.h), a factory device MAME has but doesn't fit: --master-calendar
    if (c.master_calendar >= 0) KONAMI_573_MASTER_CALENDAR_HLE(config, "k573mcal", 0, m_sys573_jvs_host).set_area((u8)c.master_calendar);

    // MAME's OUT0/OUT1/OUT2 port fields, in mask order (ioport fields are kept sorted)
    m_out0_fields = {
        {0x001, false, [this](int s) { m_adc->di_write(s); }},
        {0x002, false, [this](int s) { m_adc->cs_write(s); }},
        {0x004, false, [this](int s) { m_adc->clk_write(s); }},
        {0x100, false, [this](int s) { h8_clk_w(s); }},
    };
    m_out1_fields = {
        {0x01, false, [this](int s) { m_cassette->write_line_d0(s); }},
        {0x02, false, [this](int s) { m_cassette->write_line_d1(s); }},
        {0x04, false, [this](int s) { m_cassette->write_line_d2(s); }},
        {0x08, false, [this](int s) { m_cassette->write_line_d3(s); }},
        {0x10, false, [this](int s) { m_cassette->write_line_d4(s); }},
        {0x20, false, [this](int s) { m_cassette->write_line_d5(s); }},
        {0x40, false, [this](int s) { m_cassette->write_line_d6(s); }},
        {0x80, false, [this](int s) {
            m_cassette->write_line_d7(s);
            g_cards.select(s != 0);   // on the e-AMUSEMENT cabinets d7 also switches SIO1 to the card units (the fork's toggle_serial)
        }},
    };
    m_out2_fields = {
        {0x40, true, [this](int s) { m_cassette->write_line_zs01_sda(s); }},
    };
    for (auto *f : {&m_out0_fields, &m_out1_fields, &m_out2_fields})
        for (OutField &o : *f) o.old = o.active_low ? 1 : 0;   // MAME: the field's default value
}

void sys573_state::konami573_map(address_map &map) {
    map(0x1f000000, 0x1f3fffff).rw(FUNC(sys573_state::flash_r), FUNC(sys573_state::flash_w));
    map(0x1f400000, 0x1f400003).r(FUNC(sys573_state::in0_r)).w(FUNC(sys573_state::out0_w));
    map(0x1f400004, 0x1f400007).r(FUNC(sys573_state::in1_r));
    map(0x1f400008, 0x1f40000b).r(FUNC(sys573_state::port_in2_jvs_r));
    map(0x1f40000c, 0x1f40000f).r(FUNC(sys573_state::in3_r));
    map(0x1f480000, 0x1f48000f).rw(m_ata, FUNC(ata_interface_device::cs0_r), FUNC(ata_interface_device::cs0_w));
    map(0x1f4c0000, 0x1f4c000f).rw(m_ata, FUNC(ata_interface_device::cs1_r), FUNC(ata_interface_device::cs1_w));
    map(0x1f500000, 0x1f500001).rw(FUNC(sys573_state::control_r), FUNC(sys573_state::control_w));
    map(0x1f560000, 0x1f560001).w(FUNC(sys573_state::atapi_reset_w));
    map(0x1f5c0000, 0x1f5c0003).nopw();   // watchdog?
    map(0x1f600000, 0x1f600003).w(FUNC(sys573_state::lamps_w));
    map(0x1f620000, 0x1f623fff).rw("m48t58", FUNC(timekeeper_device::read), FUNC(timekeeper_device::write)).umask32(0x00ff00ff);
    map(0x1f680000, 0x1f680001).rw(FUNC(sys573_state::jvs_input_r), FUNC(sys573_state::jvs_input_w));
    map(0x1f6a0000, 0x1f6a0001).rw(FUNC(sys573_state::security_r), FUNC(sys573_state::security_w));
}

void sys573_state::konami573d_map(address_map &map) {
    konami573_map(map);
    map(0x1f640000, 0x1f6400ff).m(m_k573dio, FUNC(k573dio_device::amap));
}

void sys573_state::konami573a_map(address_map &map) {
    konami573_map(map);
    map(0x1f640000, 0x1f6400ff).rw(FUNC(sys573_state::gx700pwbf_io_r), FUNC(sys573_state::gx700pwbf_io_w));
}

// MAME's driver_device starts last, once every other device has (driver_device::device_start)
void sys573_state::device_start() {
    m_atapi_timer = timer_alloc(&sys573_state::atapi_xfer_end, this);
    m_atapi_timer->adjust(attotime::never);
    memory_region *h8 = memregion(":h8_response");
    m_h8_response = h8->base();
    m_h8_length = (int)h8->bytes();
    m_h8_index = 0;
    m_stage_mask = 0xffffffff;   // ddr_state::machine_start
    // ddrsolo: IN2's 0x80 (P2 START) is an unwired custom bit, active high: it reads 0
    if (cfg.family == "ddrsolo") m_in2_buttons &= ~0x0080u;
    // gtrfrks: the effect knobs (MAME's dials, PORT_REVERSE over 0..3) read 3 at rest: m_stage's
    // reset value already has those bits set
    m_lamps.assign(cab::lamps(cfg.family).size(), 0);
}

void sys573_state::device_reset_after_children() {
    m_n_security_control = 0;
    m_control = 0;
    m_h8_index = 0;
    m_h8_clk = 0;
    m_jvs_input_idx_r = m_jvs_input_idx_w = 0;
    m_jvs_output_idx_w = m_jvs_output_len_w = 0;
    std::fill_n(m_jvs_input_buffer, sizeof(m_jvs_input_buffer), 0);
    std::fill_n(m_jvs_output_buffer, sizeof(m_jvs_output_buffer), 0);
}

bool sys573_state::jvs_is_valid_packet() {
    if (m_jvs_input_idx_w < 5) return false;   // sync, node, size, a request byte, checksum
    if (m_jvs_input_buffer[0] != 0xe0 || m_jvs_input_buffer[1] == 0x00) return false;
    int command_size = m_jvs_input_buffer[2] + 3;
    if (m_jvs_input_idx_w < command_size) return false;
    u8 checksum = 0;
    for (int i = 1; i < command_size - 1; i++) checksum += m_jvs_input_buffer[i];
    return checksum == m_jvs_input_buffer[command_size - 1];
}

void sys573_state::jvs_input_w(offs_t, u16 data, u16) {
    static const bool jvs_raw = getenv("HK_573_JVS_LOG") && atoi(getenv("HK_573_JVS_LOG")) >= 2;
    if (jvs_raw) fprintf(stderr, "jvs %.6f w %04x\n", machine().time().as_double(), data);
    // (MAME's buffer has no end: bytes after a sync that never make a packet are written on past
    // it, and no packet is heard again until a reset. Here the same, but for the writing past it: a
    // packet is 258 bytes at most, so the bytes a full buffer turns away were never going to be one)
    if (m_jvs_input_idx_w + 2 > (int)sizeof(m_jvs_input_buffer)) return;
    m_jvs_input_buffer[m_jvs_input_idx_w++] = data & 0xff;
    m_jvs_input_buffer[m_jvs_input_idx_w++] = data >> 8;
    if (m_jvs_input_buffer[0] != 0xe0) m_jvs_input_idx_w = 0;
    // With the Master Calendar fitted (mcal.h, not MAME's config): the boot program's bus test
    // first writes "e0 00 1f 00", then packets with a wrong checksum on purpose. MAME's framing
    // never gets past either (the buffer only restarts on a byte other than e0), so the calendar
    // would never hear its reset and address. Here a packet for node 0, or a whole one whose sum is
    // wrong, is dropped and the framing starts again.
    else if (cfg.master_calendar >= 0 && m_jvs_input_idx_w >= 2 &&
             (m_jvs_input_buffer[1] == 0x00 ||
              (m_jvs_input_idx_w >= 3 && m_jvs_input_idx_w >= m_jvs_input_buffer[2] + 3 && !jvs_is_valid_packet()))) {
        m_jvs_input_idx_w = 0;
        return;
    }
    if (jvs_is_valid_packet()) {
        int command_size = m_jvs_input_buffer[2] + 3;
        // HK_573_JVS_LOG=1: each packet the game sends on its JVS bus (node, size, data, checksum)
        static const bool jvs_log = getenv("HK_573_JVS_LOG") != nullptr;
        if (jvs_log) {
            fprintf(stderr, "jvs %.3f s:", machine().time().as_double());
            for (int i = 1; i < command_size; i++) fprintf(stderr, " %02x", m_jvs_input_buffer[i]);
            fprintf(stderr, "\n");
        }
        m_sys573_jvs_host->send_packet(m_jvs_input_buffer + 1, command_size - 2);   // no checksum
        m_jvs_input_idx_w = 0;
        m_jvs_input_idx_r = 0;
        m_jvs_input_buffer[0] = 0;
    }
}

u16 sys573_state::jvs_input_r(offs_t, u16) {
    if (m_jvs_input_idx_r + 2 > (int)sizeof(m_jvs_input_buffer)) return 0;   // (read past the buffer's end: MAME reads on)
    u16 data = m_jvs_input_buffer[m_jvs_input_idx_r++];
    data |= m_jvs_input_buffer[m_jvs_input_idx_r++] << 8;
    return data;
}

int sys573_state::jvs_rx_r() {
    if (m_jvs_output_len_w <= 0) {
        m_jvs_output_len_w = m_sys573_jvs_host->received_packet(m_jvs_output_buffer);
        static const bool jvs_log = getenv("HK_573_JVS_LOG") != nullptr;
        if (jvs_log && m_jvs_output_len_w > 0) {
            fprintf(stderr, "jvs reply:");
            for (int i = 0; i < m_jvs_output_len_w; i++) fprintf(stderr, " %02x", m_jvs_output_buffer[i]);
            fprintf(stderr, "\n");
        }
    }
    return m_jvs_output_len_w > 0;
}

// ---- handlers -----------------------------------------------------------------------------
// The flash bank (MAME: an address_map_bank_device, 16-bit, banks of 4 MiB selected by
// control bits 0-5): four pairs of 8-bit 29F016As on the two byte lanes, then the cards.
u16 sys573_state::flash_r(offs_t offset, u16 mem_mask) {
    const u32 a = (u32)(m_control & 0x3f) * 0x400000 + offset * 2;
    if (a < 0x1000000) {
        const int pair = (int)(a >> 22) * 2;
        const offs_t o = (a & 0x3fffff) >> 1;
        u16 d = 0;
        if (mem_mask & 0x00ff) d |= m_flash[pair]->read(o);
        if (mem_mask & 0xff00) d |= (u16)(m_flash[pair + 1]->read(o) << 8);
        return d;
    }
    if (a >= 0x4000000 && a < 0x8000000) return m_pccard1->read_memory((a - 0x4000000) >> 1, mem_mask);
    if (a >= 0x8000000 && a < 0xc000000) return m_pccard2->read_memory((a - 0x8000000) >> 1, mem_mask);
    return 0;
}

void sys573_state::flash_w(offs_t offset, u16 data, u16 mem_mask) {
    machine().m_nvram_large_written = true;   // (the flash chips are looked at again at the next save while playing)
    const u32 a = (u32)(m_control & 0x3f) * 0x400000 + offset * 2;
    if (a < 0x1000000) {
        const int pair = (int)(a >> 22) * 2;
        const offs_t o = (a & 0x3fffff) >> 1;
        if (mem_mask & 0x00ff) m_flash[pair]->write(o, data & 0xff);
        if (mem_mask & 0xff00) m_flash[pair + 1]->write(o, data >> 8);
        return;
    }
    if (a >= 0x4000000 && a < 0x8000000) m_pccard1->write_memory((a - 0x4000000) >> 1, data, mem_mask);
    else if (a >= 0x8000000 && a < 0xc000000) m_pccard2->write_memory((a - 0x8000000) >> 1, data, mem_mask);
}

u32 sys573_state::in1_r() {
    // konami573 IN1: DIP switches (default: boot from flash), the H8 response, the
    // cassette's ADC and serial lines, the ADC0834, JVS sense (no devices: 1), coins
    u32 v = 0x00000007;                       // DIP 1-3 off; DIP 4 "Start Up Device" = Flash ROM
    if (cfg.boot_cd) v |= 0x00000008;         // ... or the CD-ROM drive (--boot-cd)
    v |= (u32)h8_d(0) << 4 | (u32)h8_d(1) << 5 | (u32)h8_d(2) << 6 | (u32)h8_d(3) << 7;
    v |= (u32)(m_cassette->read_line_adc083x_do() & 1) << 8;
    v |= (u32)(m_cassette->read_line_adc083x_sars() & 1) << 9;
    v |= 0x00001000;                          // "Network?" off
    v |= (u32)(m_cassette->read_line_ds2401() & 1) << 14;
    v |= (u32)(m_adc->do_read() & 1) << 16;
    v |= (u32)(m_cassette->read_line_secflash_sda() & 1) << 18;
    v |= (u32)(m_sys573_jvs_host->jvs_sense_r() & 1) << 19;
    v |= (u32)(jvs_rx_r() & 1) << 20;
    v |= m_in1_coins;
    v |= (u32)(m_pccard_cd[0] & 1) << 26;
    v |= (u32)(m_pccard_cd[1] & 1) << 27;
    return v;
}

u16 sys573_state::port_in2_jvs_r(offs_t offset, u16) {
    if (offset == 0) return (u16)((m_in2_buttons & ~0x0f0fu) | ((m_stage & m_stage_mask) & 0x0f0fu));
    // 0x1f40000a: the JVS reply, two bytes at a time
    if (m_jvs_output_len_w <= 0) return 0;
    u16 data = m_jvs_output_buffer[m_jvs_output_idx_w] | (m_jvs_output_buffer[m_jvs_output_idx_w + 1] << 8);
    m_jvs_output_idx_w += 2;
    if (m_jvs_output_idx_w >= m_jvs_output_len_w) {
        m_jvs_output_idx_w = 0;
        m_jvs_output_len_w = 0;
    }
    return data;
}

void sys573_state::port_write(u32 &port, std::vector<OutField> &fields, u32 data, u32 mem_mask) {
    COMBINE_DATA(&port);
    for (OutField &f : fields) {
        const u32 def = f.active_low ? f.mask : 0;
        u32 v = ((port ^ def) & f.mask) ? 1 : 0;
        if (v != f.old) {
            f.old = v;
            f.write((int)v);
        }
    }
}

void sys573_state::control_w(offs_t, u16 data, u16 mem_mask) {
    COMBINE_DATA(&m_control);
    port_write(m_out2, m_out2_fields, data, mem_mask);
}

void sys573_state::security_w(offs_t, u16 data, u16 mem_mask) {
    COMBINE_DATA(&m_n_security_control);
    port_write(m_out1, m_out1_fields, data, mem_mask);
}

void sys573_state::atapi_xfer_end(s32) {
    m_atapi_timer->adjust(attotime::never);
    m_hk_in_xfer = true;
    m_hk_block_raised = false;
    // (a transfer the game sized past the end of RAM: MAME writes on past it. Here those words
    // are taken from the drive and go nowhere)
    const u32 ram_words = m_ram->size() / 4;
    for (int i = 0; i < m_atapi_xfersize; i++) {
        u32 d = m_ata->cs0_r(0) << 0;
        d |= m_ata->cs0_r(0) << 16;
        if (m_atapi_xferbase / 4 < ram_words) m_p_n_psxram[m_atapi_xferbase / 4] = d;
        m_atapi_xferbase += 4;
    }
    m_hk_in_xfer = false;
    // MAME's HACK: konami80s only works if you dma more data than requested. MAME re-arms whenever
    // DRQ is up after the DMA, which it always is when the drive has raised the next block: if the
    // game's interrupt for that block comes later than the timer (GuitarFreaks 9th's /DATA4 read,
    // an interrupt 18 ms late), the timer moves the block itself and the game's count is a block
    // short (error -8: "DATA 7 : INITIALIZING" halts). A departure from MAME for that game only:
    // re-arm only when the DMA stopped inside a block. Other games rely on the re-arm after a whole
    // block (DDR 3rd Mix and Dancing Stage differ from MAME without it) (docs/CATALOGUE.md, Phase 4;
    // oracle/mame-patches/ksys573-dma.patch)
    if ((m_ata->cs1_r(6) & 8) != 0 && !(m_hk_exact_dma && m_hk_block_raised))
        m_atapi_timer->adjust(m_maincpu->cycles_to_attotime(ATAPI_CYCLES_PER_SECTOR * (m_atapi_xfersize / 64)));
}

void sys573_state::cdrom_dma_write(u32 *ram, u32 n_address, s32 n_size) {
    m_p_n_psxram = ram;
    m_atapi_xferbase = n_address;
    m_atapi_xfersize = n_size;
    m_atapi_timer->adjust(m_maincpu->cycles_to_attotime(ATAPI_CYCLES_PER_SECTOR * (n_size / 512)));
}

void sys573_state::h8_clk_w(int state) {
    if (m_h8_clk != state) {
        if (state && m_h8_index < m_h8_length - 1) m_h8_index++;
        m_h8_clk = state;
    }
}

double sys573_state::analogue_inputs_callback(u8 input) {
    switch (input) {
    case ADC083X_CH0: return (double)(5 * m_analog[0]) / 255.0;
    case ADC083X_CH1: return (double)(5 * m_analog[1]) / 255.0;
    case ADC083X_CH2: return (double)(5 * m_analog[2]) / 255.0;
    case ADC083X_CH3: return (double)(5 * m_analog[3]) / 255.0;
    case ADC083X_AGND: return 0;
    case ADC083X_VREF: return 5;
    }
    return 0;
}

// GX700-PWB(F), the analog I/O board: four 8-bit output latches; reads return 0 (MAME)
u16 sys573_state::gx700pwbf_io_r(offs_t, u16) { return 0; }

void sys573_state::gx700pwbf_io_w(offs_t offset, u16 data, u16) {
    switch (offset) {
    case 0x40: gx700pwbf_output(0, data & 0xff); break;
    case 0x44: gx700pwbf_output(1, data & 0xff); break;
    case 0x48: gx700pwbf_output(2, data & 0xff); break;
    case 0x4c: gx700pwbf_output(3, data & 0xff); break;
    }
}

void sys573_state::gx700pwbf_output(int offset, u8 data) {
    static const int shift[] = {7, 6, 1, 0, 5, 4, 3, 2};
    for (int i = 0; i < 8; i++) {
        int oldbit = (m_gx700pwbf_output_data[offset] >> shift[i]) & 1;
        int newbit = (data >> shift[i]) & 1;
        if (oldbit == newbit) continue;
        // where MAME's init sends them (gx700pwfbf_init): init_ddr to DDR's lamps, init_drmn to
        // DrumMania's; GuitarFreaks' empty_init to nothing (its lamps are the LAMPS port)
        if (cfg.family == "ddr") ddr_output_callback((offs_t)(offset * 8 + i), (u8)newbit);
        else if (cfg.family == "drmn") drmn_output_callback((offs_t)(offset * 8 + i), (u8)newbit);
    }
    m_gx700pwbf_output_data[offset] = data;
}

// ddr_state: GN845-PWB(B), the DDR stage multiplexor
void sys573_state::gn845pwbb_clk_w(int offset, int data) {
    static const int mask[] = {0, 6, 2, 4, 0, 4, 0, 4, 0, 4, 0, 4, 0, 4, 0, 4, 0, 4, 0, 4, 0, 6};
    int clk = !data;
    stage_state &st = m_stage_state[offset];
    if (clk == st.clk) return;
    st.clk = clk;
    if (!clk) return;
    st.shift = (st.shift >> 1) | (st.DO << 12);
    switch (st.state) {
    case 0:   // DDR_STAGE_IDLE
        if (st.shift == 0xc90) {
            st.state = 1;
            st.bit = 0;
            m_stage_mask = 0xfffff9f9;
        }
        break;
    case 1:   // DDR_STAGE_INIT
        st.bit++;
        if (st.bit < 22) {
            int a = ((((~0x06) | mask[m_stage_state[0].bit]) & 0xff) << 8);
            int b = ((((~0x06) | mask[m_stage_state[1].bit]) & 0xff) << 0);
            m_stage_mask = 0xffff0000 | a | b;
        } else {
            st.bit = 0;
            st.state = 0;
            m_stage_mask = 0xffffffff;
        }
        break;
    }
}

void sys573_state::ddr_output_callback(offs_t offset, u8 data) {
    switch (offset) {
    case 4: gn845pwbb_do_w(0, !data); return;
    case 7: gn845pwbb_clk_w(0, !data); return;
    case 12: gn845pwbb_do_w(1, !data); return;
    case 15: gn845pwbb_clk_w(1, !data); return;
    }
    // the cabinet's lamps (MAME's outputs): lit = data 0; the digital board's speaker neon (28)
    // is the analog board's 30
    if (offset == 28) offset = 30;
    set_lamp(offset, !data);
}

// MAME's drmn_output_callback (ksys573.cpp), the digital board's numbering (DrumMania 2nd and
// later): the drum lamps and the spots are lit at data 0, the neons, Start and Select at data 1.
// DrumMania 1st's analog board numbers them otherwise; they map onto the same lamps.
void sys573_state::drmn_output_callback(offs_t offset, u8 data) {
    static const s8 analog[32] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                                  0, 1, 2, 3, 12, 13, 14, -1, -1, -1, -1, 11, -1, -1, 8, 9};   // 16-31
    if (offset >= 16) {
        if (offset >= 32 || analog[offset] < 0) return;
        offset = (offs_t)analog[offset];
    }
    switch (offset) {
    case 0: case 1: case 2: case 3: case 8: case 12: set_lamp(offset, !data); break;
    case 9: case 11: case 13: case 14: set_lamp(offset, data != 0); break;
    default: break;
    }
}

// MAME's ddrsolo_output_callback (ksys573.cpp): the extra balls, the speaker, Start and the body
// lamps, lit at data 0; the stage outputs (4, 7, 12, 15) aren't wired on a Solo cabinet
void sys573_state::ddrsolo_output_callback(offs_t offset, u8 data) {
    switch (offset) {
    case 8: case 9: case 10: case 11: case 16: case 20: case 21: case 22: case 23: set_lamp(offset, !data); break;
    default: break;
    }
}

// MAME's dmx_output_callback (ksys573.cpp): the sensors' blue and red lamps, outputs 0-23, and
// the yellow, blue and pink spotlights (28, 29, 31), lit at data 0
void sys573_state::dmx_output_callback(offs_t offset, u8 data) {
    if (offset < 24 || offset == 28 || offset == 29 || offset == 31) set_lamp(offset, !data);
}

// MAME's mamboagg_output_callback (ksys573.cpp): the fire lamps and fans (4-7), the congas'
// lamps (28 left, 29 right, 31 centre), lit at data 0
void sys573_state::mamboagg_output_callback(offs_t offset, u8 data) {
    if ((offset >= 4 && offset <= 7) || offset == 28 || offset == 29 || offset == 31) set_lamp(offset, !data);
}

void sys573_state::set_lamp(offs_t offset, bool lit) {
    if (offset >= 64) return;
    const u64 bit = 1ull << offset, was = m_outputs;
    m_outputs = lit ? m_outputs | bit : m_outputs & ~bit;
    if (m_outputs == was) return;
    const auto &defs = cab::lamps(cfg.family);
    m_lamps.resize(defs.size());
    for (size_t k = 0; k < defs.size(); k++) m_lamps[k] = (u8)((m_outputs >> defs[k].bit) & 1);
    m_lamp_changes++;
}

// ---- the machine --------------------------------------------------------------------------
struct Board {
    std::unique_ptr<running_machine> machine;
    sys573_state *state = nullptr;
    Config cfg;
    FrameHook frame_hook;
    PresentHook present_hook;
    std::vector<u32> present_rgb;
    AudioSink audio_sink;
    // the MP3 decoders: the digital board's MAS3507D and the multi-session unit's four TC9446Fs,
    // each at 44.1 kHz, resampled for the host when the MP3 is another rate (the emulation never
    // sees this), and where its resampler is between samples
    struct Decoder {
        sound_stream *stream;
        float gain;               // MAME's route
        bool msu;
        // the samples waiting for the host, per channel: read from `head` on, the space before
        // it given back now and then (a deque here was ~3% of an install's frame time)
        struct Fifo {
            std::vector<float> v;
            size_t head = 0;
            size_t size() const { return v.size() - head; }
            float operator[](size_t i) const { return v[head + i]; }
            void push_back(float f) { v.push_back(f); }
            void append(const float *b, const float *e) { v.insert(v.end(), b, e); }
            void pop(size_t n) {
                head += n;
                if (head >= 4096 && head * 2 >= v.size()) { v.erase(v.begin(), v.begin() + (ptrdiff_t)head); head = 0; }
            }
        } q[2];
        double pos = 0;
        float peak = 0;           // its largest |sample|, and its rate (the report)
        u32 rate = 0;
        std::vector<float> in[2];   // its samples at its own rate, on their way to q
        // --fine-sound: the decoder's hk_predict(), the samples handed on ahead of its stream's
        // next update (from its first), and how many were, and turned out otherwise
        std::function<int(int, const float *&, const float *&)> predict;
        std::vector<float> ahead[2];
        u64 predicted = 0, differed = 0;
    };
    std::vector<Decoder> decoders;
    u64 frames = 0;
    float sound_peak = 0;     // the mix's largest |sample| (1.0 = full scale)
    u64 sound_over = 0;       // samples past full scale (clipped at the output)
    u64 sound_samples = 0;
    std::vector<u32> frame_rgb;
} g;

std::string nv(const char *name) { return g.cfg.nvram_dir.empty() ? std::string() : g.cfg.nvram_dir + "/" + name; }

// The picture: the screen's visible area from `bm`
void picture(screen_device &screen, const bitmap_rgb32 &bm, std::vector<u32> &out, int &w, int &h) {
    const rectangle &vis = screen.visible_area();
    w = vis.width(), h = vis.height();
    out.resize((size_t)w * h);
    // raw rows: through pix() and the vectors, every store reloads the bitmap's pointers
    u32 *__restrict dst = out.data();
    for (int y = 0; y < h; y++) {
        const u32 *__restrict src = &bm.pix(vis.top() + y, vis.left());
        for (int x = 0; x < w; x++) dst[x] = src[x] | 0xff000000u;
        dst += w;
    }
}

}  // namespace

bool init(const Config &cfg) {
    g.cfg = cfg;
    auto rom = [&](const std::string &f) { return cfg.rom_dir + "/" + f; };
    g.machine = std::make_unique<running_machine>();
    running_machine &m = *g.machine;
    m.m_exact = true;
    m.m_base_time = cfg.rtc_time;
    if (!cfg.nvram_dir.empty()) { sys::make_dir(cfg.nvram_dir); m.m_nvram_dir = cfg.nvram_dir; }

    struct R { const char *tag; std::string file; } regions[] = {
        {":maincpu:rom", rom(cfg.bios)},
        {":h8_response", rom(cfg.h8)},
        // none: MAME has no dump of it (ddrja), and its region is zeros, as the chip's own default
        {":cassette:game:eeprom", cfg.cassette_eeprom.empty() ? "" : rom(cfg.cassette_eeprom)},
        {":cassette:game:id", cfg.cassette_id.empty() ? "" : rom(cfg.cassette_id)},           // the "i" cassettes
        {":cassette:install:eeprom", cfg.install_eeprom.empty() ? "" : rom(cfg.install_eeprom)},
        {":cassette:install:id", cfg.install_id.empty() ? "" : rom(cfg.install_id)},
        {":k573dio:digital_id", cfg.io_board == "digital" ? rom(cfg.digital_id) : ""},
    };
    for (auto &r : regions) {
        if (r.file.empty()) continue;
        std::vector<u8> d = load_file(r.file);
        if (d.empty()) { fprintf(stderr, "cannot read %s\n", r.file.c_str()); return false; }
        // an older MAME's ZS01 dump (4,116 bytes, no configuration registers): MAME loads the
        // region's 140 bytes from it (WRONG LENGTH), so the game sees the same chip as in MAME
        const std::string tag = r.tag;
        const bool game_eeprom = tag == ":cassette:game:eeprom", install_eeprom = tag == ":cassette:install:eeprom";
        const std::string &kind = install_eeprom && !cfg.install_kind.empty() ? cfg.install_kind : cfg.cassette;
        if ((game_eeprom || install_eeprom) && kind == "zi" && d.size() > 0x8c) d.resize(0x8c);
        m.add_region(r.tag, std::move(d));
    }
    // chips the set has filled (an installed flash, a set timekeeper): the device's own region
    for (const std::string &p : cfg.preload) {
        const size_t c = p.find(':');
        if (c == std::string::npos) continue;
        std::vector<u8> d = load_file(rom(p.substr(c + 1)));
        if (d.empty()) { fprintf(stderr, "cannot read %s\n", rom(p.substr(c + 1)).c_str()); return false; }
        m.add_region(":" + p.substr(0, c), std::move(d));
    }
    cdrom_image_device::hk_set_path(":ata:0:cr589:image", cfg.cd);
    if (cfg.msu) {
        cdrom_image_device::hk_set_path(":k573msu:ata_cdrom:0:cdrom:image", cfg.msu_cd);
        // the unit's TX3927 firmware: ROM_LOAD16_WORD_SWAP into a big-endian 32-bit region, so each
        // 32-bit word is the file's first 16-bit (little-endian) word above its second; held as host
        // (little-endian) words, the two halves of every 32-bit word swap
        std::vector<u8> fw = load_file(rom(cfg.msu_flash)), rtc = load_file(rom(cfg.msu_rtc)),
                        id = load_file(rom(cfg.digital_id));
        if (fw.size() != 0x80000 || rtc.empty() || id.empty()) {
            fprintf(stderr, "the multi-session unit needs %s, %s and %s in %s\n", cfg.msu_flash.c_str(), cfg.msu_rtc.c_str(),
                    cfg.digital_id.c_str(), cfg.rom_dir.c_str());
            return false;
        }
        for (size_t k = 0; k < fw.size(); k += 4) {
            std::swap(fw[k], fw[k + 2]);
            std::swap(fw[k + 1], fw[k + 3]);
        }
        m.add_region(":k573msu:tmpr3927", std::move(fw));
        m.add_region(":k573msu:m48t58y", std::move(rtc));
        m.add_region(":k573msu:digital_id", std::move(id));
    }
    if (cfg.pccard2 == "npu") {
        // the server's clock: the timekeeper's, as it was set at power-on and has run since
        const char *log = getenv("HK_573_NET_LOG");
        if (!g_eamuse.open(cfg.eamuse, cfg.nvram_dir.empty() ? "" : cfg.nvram_dir + "/eamuse", log ? log : "",
                           [] { return g.cfg.rtc_time + (int64_t)g.machine->time().as_double(); }, cfg.pcbid))
            return false;
    }
    g_cards.configure(cfg.card_readers, cfg.nvram_dir);
    hk_psx_sio1_uart = cfg.msu || !cfg.session_join.empty() || !cfg.session_host.empty() || cfg.card_readers > 0 || cfg.rental;
    if (!cfg.session_host.empty() || !cfg.session_join.empty()) {
        const bool host = !cfg.session_host.empty();
        if (!g_link.open(host ? cfg.session_host : cfg.session_join, host, attotime::from_usec(cfg.session_quantum_us),
                         host && cfg.msu))
            return false;
    }

    g_build = &g.cfg;
    sys573_state &st = sys573_state::SYS573_STATE(m.config(), "");
    g.state = &st;
    st.cfg = cfg;
    st.m_screen->m_hk_frame = [](screen_device &screen) {
        g.frames++;
        // HK_MSU_TRACE=1: where the multi-session unit's CPU is, every 300 frames
        static const bool msu_trace = getenv("HK_MSU_TRACE") != nullptr;
        if (msu_trace && g.frames % 300 == 0)
            if (auto *msu = g.state->subdevice<cpu_device>("k573msu:maincpu"))
                fprintf(stderr, "f%llu msu pc %08x cycles %llu\n", (unsigned long long)g.frames, msu->pc(), (unsigned long long)msu->total_cycles());
        // HK_MSU_HASH=file: every 60 frames, the unit's PC, cycle count and a hash of its RAM (to
        // check that a change meant to be invisible leaves the unit as it was: the 573's digest
        // doesn't see it)
        static FILE *msu_hash = getenv("HK_MSU_HASH") ? fopen(getenv("HK_MSU_HASH"), "w") : nullptr;
        if (msu_hash && g.frames % 60 == 0)
            if (auto *msu = g.state->subdevice<cpu_device>("k573msu:maincpu")) {
                ram_device *ram = g.state->subdevice<ram_device>("k573msu:ram");
                const u64 *w = (const u64 *)ram->pointer();
                u64 h = 0xcbf29ce484222325ull;
                for (size_t i = 0; i < ram->size() / 8; i++) h = (h ^ w[i]) * 0x100000001b3ull;
                fprintf(msu_hash, "%llu %08x %llu %016llx\n", (unsigned long long)g.frames, msu->pc(),
                        (unsigned long long)msu->total_cycles(), (unsigned long long)h);
                fflush(msu_hash);
            }
        if (!g.frame_hook) return;
        int w, h;
        picture(screen, screen.m_bitmap, g.frame_rgb, w, h);
        g.frame_hook(g.frames, g.frame_rgb.data(), w, h);
    };
    m.m_after_nvram_load = [&m] {
        // MAME's set_rtc_datetime: battery-backed RTCs get the power-on time
        for (device_rtc_interface *rtc : device_interface_enumerator<device_rtc_interface>(*m.m_devices.front()))
            if (rtc->has_battery()) rtc->set_current_time(system_time(m.m_base_time));
    };
    m.start();
    g_link.attach(m);
    if (g_cards.fitted()) {
        auto *sio1 = st.m_maincpu->subdevice<psxsio1_uart_device>("sio1");
        g_cards.attach(m, [sio1](int b) { sio1->write_rxd(b); });
    }
    if (cfg.rental) {
        auto *sio1 = st.m_maincpu->subdevice<psxsio1_uart_device>("sio1");
        g_rental.attach(m, [sio1](int b) { sio1->write_rxd(b); });
    }
    m.reset();
    g_cards.reset();
    if (cfg.rental) {
        g_rental.reset();
        st.m_maincpu->subdevice<psxsio1_uart_device>("sio1")->write_cts(0);
    }

    // HK_573_ITRACE=file,from,to: every instruction of frames [from, to) as "totalcycles pc"
    // (to compare with MAME's debugger trace, oracle/lua/itrace573.lua)
    if (const char *t = getenv("HK_573_ITRACE")) {
        static FILE *tf = nullptr;
        static u64 from = 0, to = 0;
        char path[512];
        unsigned long long a = 0, b = 0;
        if (sscanf(t, "%511[^,],%llu,%llu", path, &a, &b) == 3) {
            tf = fopen(path, "w");
            from = a; to = b;
            cpu_device *cpu = st.m_maincpu.target();
            cpu->m_hk_trace = [cpu](offs_t pc) {
                if (g.frames >= from && g.frames < to && tf) fprintf(tf, "%llu %08x\n", (unsigned long long)cpu->total_cycles(), pc);
                else if (g.frames >= to && tf) { fclose(tf); tf = nullptr; }
            };
        }
    }
    mips::init(*st.m_maincpu);
    return true;
}

void set_frame_hook(FrameHook h) { g.frame_hook = std::move(h); }
void set_present_hook(PresentHook h) {
    g.present_hook = std::move(h);
    g.state->m_screen->m_hk_vblank_end = [](screen_device &screen, bitmap_rgb32 &bm) {
        int w, h;
        picture(screen, bm, g.present_rgb, w, h);
        g.present_hook(g.present_rgb.data(), w, h);
    };
}
void set_audio_sink(AudioSink s) { g.audio_sink = std::move(s); }

// The GPU draws the frame on show again with what moves carried on (psxgpu_device::hk_between)
const u32 *render_between(double t, int &w, int &h) {
    static bitmap_rgb32 bm;
    static std::vector<u32> out;
    screen_device &screen = *g.state->m_screen;
    if (!g.state->m_gpu->hk_between(t, screen, bm)) return nullptr;
    picture(screen, bm, out, w, h);
    return out.data();
}

// --fine-sound (Config::fine_sound): the sound for the host every millisecond of machine time.
// MAME brings its streams up to date 50 times a second (and when the CPU touches a sound chip), so
// by default the host gets the sound in 20 ms pieces: the controls are read once per piece and
// the sound queue swings by 20 ms. A timer that does nothing ends a timeslice every millisecond
// (timeslice() returns only when a timer fires), and the SPU's stream is brought up to date then.
// The machine is unchanged: the SPU schedules its own IRQ at the exact sample (spu.cpp
// update_irq_event), reads of its registers update the stream first, and every regress case shows
// the same pictures. The sound is too on ddr2m-play and drmn10m-play, bit for bit; on ddr3mj-play
// the SPU's differs a little from 195 s on (2.7 % of samples, correlation 0.99; why is open). The
// MP3 decoders (the digital board's, the multi-session unit's) still update only at MAME's 50 Hz:
// decoding asks the board for more data, which the machine sees. Their samples up to now are
// worked out ahead of the update on copies (hk_predict, below), and checked when it comes.
[[noreturn]] void run() {
    running_machine &m = *g.machine;
    const attotime step = attotime::from_msec(1);
    attotime next_audio;
    if (g.cfg.fine_sound) m.alloc_timer([](s32) {})->adjust(step, 0, step);
    for (;;) {
        m.timeslice();
        const bool audio_step = g.cfg.fine_sound && m.time() >= next_audio;
        if (audio_step) next_audio = m.time() + step;
        // sound: MAME's speaker, the SPU plus the CR-589's CD audio (both 44.1 kHz, routed at 1.0;
        // the CD at its MODE SELECT volume, set_output_gain), plus the digital board's MP3 (routed
        // at 1.0) and the multi-session unit's four (0.65 each, as windyfairy's fork routes them)
        if (g.audio_sink) {
            static sound_stream *spu = nullptr, *cd = nullptr;
            if (!spu)
                for (sound_stream *s : m.m_streams) {
                    if (s->m_device == g.state->m_spu.target()) spu = s;
                    else if (dynamic_cast<cdda_device *>(s->m_device) && s->sample_rate() == 44100) cd = s;
                    else if (auto *mas = dynamic_cast<mas3507d_device *>(s->m_device)) {
                        g.decoders.push_back({s, 1.0f, false});
                        g.decoders.back().predict = [mas](int n, const float *&l, const float *&r) { return mas->hk_predict(n, l, r); };
                    } else if (auto *tc = dynamic_cast<tc9446f_device *>(s->m_device)) {
                        g.decoders.push_back({s, 0.65f, true});
                        g.decoders.back().predict = [tc](int n, const float *&l, const float *&r) { return tc->hk_predict(n, l, r); };
                    }
                }
            for (Board::Decoder &d : g.decoders) {
                // what its 50 Hz updates produced: the decoders decode and ask for data as their
                // streams update, so updating one here would change the machine
                auto &ml = d.stream->m_out[0], &mr = d.stream->m_out[1];
                const size_t real = std::min(ml.size(), mr.size());
                // --fine-sound: what was handed on ahead of the update, now that it has made it.
                // It's the same unless the game changed something in between (a new song's first
                // milliseconds, a volume); those samples have been played, so the update's own
                // are dropped either way, and the sound keeps its time
                const size_t k = std::min(real, d.ahead[0].size());
                for (size_t i = 0; i < k; i++)
                    if (ml[i] != d.ahead[0][i] || mr[i] != d.ahead[1][i]) d.differed++;
                d.ahead[0].erase(d.ahead[0].begin(), d.ahead[0].begin() + (ptrdiff_t)k);
                d.ahead[1].erase(d.ahead[1].begin(), d.ahead[1].begin() + (ptrdiff_t)k);
                for (size_t i = k; i < real; i++) d.peak = std::max({d.peak, std::fabs(ml[i]), std::fabs(mr[i])});
                d.in[0].insert(d.in[0].end(), ml.begin() + (ptrdiff_t)k, ml.begin() + (ptrdiff_t)real);
                d.in[1].insert(d.in[1].end(), mr.begin() + (ptrdiff_t)k, mr.begin() + (ptrdiff_t)real);
                ml.erase(ml.begin(), ml.begin() + (ptrdiff_t)real);
                mr.erase(mr.begin(), mr.begin() + (ptrdiff_t)real);
                // ... and the samples from its next update up to now, worked out ahead of it
                if (audio_step && d.predict) {
                    const u64 have = d.stream->m_write_sample + d.ahead[0].size(), want = d.stream->index_at(m.time());
                    if (want > have) {
                        const float *pl, *pr;
                        const size_t from = d.ahead[0].size();
                        const size_t got = (size_t)d.predict((int)(from + (want - have)), pl, pr);
                        for (size_t i = from; i < got; i++) {
                            d.ahead[0].push_back(pl[i]), d.ahead[1].push_back(pr[i]);
                            d.in[0].push_back(pl[i]), d.in[1].push_back(pr[i]);
                        }
                        if (got > from) d.predicted += got - from;
                    }
                }
                const size_t avail = d.in[0].size();
                const u32 rate = d.stream->sample_rate();
                d.rate = rate;
                size_t used = avail;
                const std::vector<float> &il = d.in[0], &ir = d.in[1];
                if (rate == 44100 || !rate) {
                    d.q[0].append(il.data(), il.data() + avail);
                    d.q[1].append(ir.data(), ir.data() + avail);
                } else {   // linear, from rate to 44.1 kHz
                    const double step = rate / 44100.0;
                    while (d.pos + 1 < (double)avail) {
                        const size_t i = (size_t)d.pos;
                        const float f = (float)(d.pos - (double)i);
                        d.q[0].push_back(il[i] + (il[i + 1] - il[i]) * f);
                        d.q[1].push_back(ir[i] + (ir[i + 1] - ir[i]) * f);
                        d.pos += step;
                    }
                    used = std::min(avail, (size_t)d.pos);
                    d.pos -= (double)used;
                }
                d.in[0].erase(d.in[0].begin(), d.in[0].begin() + (ptrdiff_t)used);
                d.in[1].erase(d.in[1].begin(), d.in[1].begin() + (ptrdiff_t)used);
            }
            if (spu) {
                // the CD stream to now, so the SPU's samples don't wait for its 50 Hz update (the
                // game sees the same either way: get_audio_lba() updates the stream itself)
                if (cd) cd->update();
                if (audio_step) spu->update();
                auto &l = spu->m_out[0], &r = spu->m_out[1];
                size_t n = std::min(l.size(), r.size());
                if (cd) n = std::min({n, cd->m_out[0].size(), cd->m_out[1].size()});
                for (const Board::Decoder &d : g.decoders) n = std::min(n, d.q[0].size());
                // joined to another cabinet's unit, or to a club kit's IIDX cabinet: its music too
                // (link.h), as the host waits for it
                const bool far_music = !g.cfg.msu && g_link.is_open() && g_link.far_audio();
                if (far_music) n = std::min(n, g_link.audio_available());
                if (n) {
                    std::vector<float> lr(n * 2);
                    for (size_t i = 0; i < n; i++) { lr[2 * i] = l[i]; lr[2 * i + 1] = r[i]; }
                    l.erase(l.begin(), l.begin() + n);
                    r.erase(r.begin(), r.begin() + n);
                    if (cd) {
                        auto *cdd = static_cast<cdda_device *>(cd->m_device);
                        for (int ch = 0; ch < 2; ch++) {
                            auto &c = cd->m_out[ch];
                            const float gain = cdd->output_gain(ch);
                            for (size_t i = 0; i < n; i++) lr[2 * i + ch] += c[i] * gain;
                            c.erase(c.begin(), c.begin() + n);
                        }
                    }
                    // the multi-session unit's share, which the other cabinet in a session plays
                    // too (link.h); and theirs, when this cabinet joined another's unit
                    const bool host = g_link.is_open() && g.cfg.msu;
                    std::vector<float> unit(host ? n * 2 : 0);
                    for (Board::Decoder &d : g.decoders)
                        for (int ch = 0; ch < 2; ch++) {
                            auto &q = d.q[ch];
                            for (size_t i = 0; i < n; i++) lr[2 * i + ch] += q[i] * d.gain;
                            if (host && d.msu)
                                for (size_t i = 0; i < n; i++) unit[2 * i + ch] += q[i] * d.gain;
                            q.pop(n);
                        }
                    if (host) g_link.send_audio(unit.data(), n);
                    else if (far_music) g_link.mix_audio(lr.data(), n);
                    for (float &v : lr) {
                        v *= g.cfg.mix_gain;
                        const float a = std::fabs(v);
                        if (a > g.sound_peak) g.sound_peak = a;
                        if (a > 1.0f) g.sound_over++;
                    }
                    g.sound_samples += lr.size();
                    g.audio_sink(lr.data(), (int)n);
                }
            }
        } else {
            for (sound_stream *s : m.m_streams) for (auto &o : s->m_out) o.clear();
        }
    }
}

void save_nvram() {
    if (g.machine) g.machine->nvram_save();
    g_cards.save();
    mips::finish();
}

void save_nvram_now() {
    if (g.machine) g.machine->nvram_save_now();
}

double seconds() { return g.machine ? g.machine->m_basetime.as_double() : 0.0; }
uint64_t frame_count() { return g.frames; }
const std::string &family() { return g.cfg.family; }

bool card_read(int player, const std::string &number) { return g_cards.fitted() && g_cards.card_read(player, number); }

bool set_input(const std::string &name, int value) {
    if (g_cards.set_input(name, value)) return true;   // the card units' slots and 10-keys
    sys573_state &st = *g.state;
    auto bit = [&](u32 &port, u32 mask) { if (value) port &= ~mask; else port |= mask; return true; };   // active low
    static const std::map<std::string, std::pair<int, u32>> names = {
        // STAGE (ddr): P1 0x100 left, 0x200 right, 0x400 up, 0x800 down; P2 0x1-0x8
        {"P1 Left", {0, 0x100}}, {"P1 Right", {0, 0x200}}, {"P1 Up", {0, 0x400}}, {"P1 Down", {0, 0x800}},
        {"P2 Left", {0, 0x001}}, {"P2 Right", {0, 0x002}}, {"P2 Up", {0, 0x004}}, {"P2 Down", {0, 0x008}},
        // IN2
        {"P1 Button 1", {1, 0x1000}}, {"P1 Button 2", {1, 0x2000}}, {"P1 Button 3", {1, 0x4000}}, {"1 Player Start", {1, 0x8000}},
        {"P2 Button 1", {1, 0x0010}}, {"P2 Button 2", {1, 0x0020}}, {"P2 Button 3", {1, 0x0040}}, {"2 Players Start", {1, 0x0080}},
        // IN3
        {"P1 Button 4", {2, 0x100}}, {"P1 Button 5", {2, 0x200}}, {"Service Mode", {2, 0x400}}, {"P1 Button 6", {2, 0x800}},
        {"P2 Button 4", {2, 0x1000000}}, {"P2 Button 5", {2, 0x2000000}}, {"P2 Button 6", {2, 0x8000000}},
        // IN1
        {"Coin 1", {3, 0x1000000}}, {"Coin 2", {3, 0x2000000}}, {"Service 1", {3, 0x10000000}},
        // DrumMania (MAME's drmn ports): the pads on the stage bits, the cymbal and the bass pedal
        // on IN2's buttons, the select buttons on player 2's left and right
        {"High Tom", {0, 0x100}}, {"Low Tom", {0, 0x200}}, {"Hi-Hat", {0, 0x400}}, {"Snare", {0, 0x800}},
        {"Select L", {0, 0x001}}, {"Select R", {0, 0x002}}, {"Cymbal", {1, 0x1000}}, {"Bass Drum", {1, 0x4000}},
        // GuitarFreaks (MAME's gtrfrks ports): the neck buttons on IN2's buttons, the pick and the
        // wailing on the stage bits; Service on IN3 (IN1's is unused)
        {"P1 Button R", {1, 0x1000}}, {"P1 Button G", {1, 0x2000}}, {"P1 Button B", {1, 0x4000}},
        {"P1 Pick", {0, 0x400}}, {"P1 Wailing", {0, 0x800}},
        {"P2 Button R", {1, 0x0010}}, {"P2 Button G", {1, 0x0020}}, {"P2 Button B", {1, 0x0040}},
        {"P2 Pick", {0, 0x004}}, {"P2 Wailing", {0, 0x008}},
        {"Service 1 (IN3)", {2, 0x100}},
    };
    // the effect knobs: MAME's dials, a 2-bit position (not active low) on the stage bits
    if (name == "P1 Effect Knob" || name == "P2 Effect Knob") {
        const int sh = name[1] == '1' ? 8 : 0;
        st.m_stage = (st.m_stage & ~(3u << sh)) | ((u32)(3 - value) & 3) << sh;   // PORT_REVERSE: 3 - position
        return true;
    }
    // DDR Solo (MAME's ddrsolo ports): each panel's two sensors; a panel name presses both
    if (g.cfg.family == "ddrsolo") {
        static const std::map<std::string, std::pair<int, u32>> solo = {
            {"P1 Left 1", {0, 0x100}}, {"P1 Right 1", {0, 0x200}}, {"P1 Up 1", {0, 0x400}}, {"P1 Down 1", {0, 0x800}},
            {"P1 Up-Left 1", {0, 0x001}}, {"P1 Up-Right 1", {0, 0x002}},
            {"P1 Up-Left 2", {1, 0x1000}}, {"P1 Left 2", {1, 0x2000}}, {"P1 Down 2", {1, 0x4000}},
            {"P1 Up 2", {1, 0x0010}}, {"P1 Right 2", {1, 0x0020}}, {"P1 Up-Right 2", {1, 0x0040}},
            {"P1 Select L", {2, 0x200}}, {"P1 Select R", {2, 0x2000000}},
        };
        auto port = [&](int p) -> u32 & { return p == 0 ? st.m_stage : p == 1 ? st.m_in2_buttons : st.m_in3; };
        if (auto s1 = solo.find(name + " 1"), s2 = solo.find(name + " 2"); s1 != solo.end() && s2 != solo.end()) {
            bit(port(s1->second.first), s1->second.second);
            return bit(port(s2->second.first), s2->second.second);
        }
        if (auto s = solo.find(name); s != solo.end()) return bit(port(s->second.first), s->second.second);
    }
    // Dance Maniax (MAME's dmx ports, "D-Sensor ..." for both players: here "P1 ..." and "P2 ...")
    // and Mambo a GoGo (MAME's mamboagg ports: every conga pad read by two sensors). A name of
    // the cabinet's own (runtime/shell/cabinet.cpp) presses each sensor it lists.
    if (g.cfg.family == "dmx" || g.cfg.family == "mambo") {
        using Bits = std::vector<std::pair<int, u32>>;   // (port as `names` numbers them, mask)
        static const std::map<std::string, Bits> dmx = {
            {"P1 D-Sensor D1 L", {{0, 0x100}}}, {"P1 D-Sensor D1 R", {{0, 0x200}}},
            {"P1 Select L", {{0, 0x400}}}, {"P1 Select R", {{0, 0x800}}},
            {"P1 D-Sensor U L", {{1, 0x1000}}}, {"P1 D-Sensor U R", {{1, 0x2000}}},
            {"P1 D-Sensor D0 L", {{2, 0x100}}}, {"P1 D-Sensor D0 R", {{2, 0x200}}},
            {"P2 D-Sensor D1 L", {{0, 0x001}}}, {"P2 D-Sensor D1 R", {{0, 0x002}}},
            {"P2 Select L", {{0, 0x004}}}, {"P2 Select R", {{0, 0x008}}},
            {"P2 D-Sensor U L", {{1, 0x0010}}}, {"P2 D-Sensor U R", {{1, 0x0020}}},
            {"P2 D-Sensor D0 L", {{2, 0x1000000}}}, {"P2 D-Sensor D0 R", {{2, 0x2000000}}},
            // the cabinet's four hand sensors per player: the lower ones are two beams (D0, D1)
            {"P1 Upper Left", {{1, 0x1000}}}, {"P1 Upper Right", {{1, 0x2000}}},
            {"P1 Lower Left", {{2, 0x100}, {0, 0x100}}}, {"P1 Lower Right", {{2, 0x200}, {0, 0x200}}},
            {"P2 Upper Left", {{1, 0x0010}}}, {"P2 Upper Right", {{1, 0x0020}}},
            {"P2 Lower Left", {{2, 0x1000000}, {0, 0x001}}}, {"P2 Lower Right", {{2, 0x2000000}, {0, 0x002}}},
        };
        static const std::map<std::string, Bits> mambo = {
            {"Left Pad 1 (Top Left)", {{1, 0x2000}}}, {"Left Pad 1 (Top Right)", {{1, 0x0080}}},
            {"Left Pad 2 (Middle Left)", {{2, 0x200}}}, {"Left Pad 2 (Bottom Left)", {{1, 0x1000}}},
            {"Left Pad 3 (Middle Right)", {{2, 0x100}}}, {"Left Pad 3 (Bottom Right)", {{1, 0x4000}}},
            {"Centre Pad 1 (Top Left)", {{0, 0x004}}}, {"Centre Pad 1 (Top Right)", {{0, 0x800}}},
            {"Centre Pad 2 (Middle Left)", {{0, 0x008}}}, {"Centre Pad 2 (Bottom Left)", {{0, 0x001}}},
            {"Centre Pad 3 (Middle Right)", {{0, 0x400}}}, {"Centre Pad 3 (Bottom Right)", {{0, 0x002}}},
            {"Right Pad 1 (Top Left)", {{1, 0x0020}}}, {"Right Pad 1 (Top Right)", {{3, 0x2000000}}},
            {"Right Pad 2 (Middle Left)", {{2, 0x2000000}}}, {"Right Pad 2 (Bottom Left)", {{1, 0x0010}}},
            {"Right Pad 3 (Middle Right)", {{2, 0x1000000}}}, {"Right Pad 3 (Bottom Right)", {{1, 0x0040}}},
            // each pad as one: both its sensors (MAME gives the two one button)
            {"Left Pad 1", {{1, 0x2000}, {1, 0x0080}}}, {"Left Pad 2", {{2, 0x200}, {1, 0x1000}}},
            {"Left Pad 3", {{2, 0x100}, {1, 0x4000}}}, {"Centre Pad 1", {{0, 0x004}, {0, 0x800}}},
            {"Centre Pad 2", {{0, 0x008}, {0, 0x001}}}, {"Centre Pad 3", {{0, 0x400}, {0, 0x002}}},
            {"Right Pad 1", {{1, 0x0020}, {3, 0x2000000}}}, {"Right Pad 2", {{2, 0x2000000}, {1, 0x0010}}},
            {"Right Pad 3", {{2, 0x1000000}, {1, 0x0040}}},
            // the panel's select buttons: konami573's P1 Left/Right, which mamboagg's ports keep
            // (its select lamps are mamboagg_lamps_b4/b5)
            {"P1 Left", {{0, 0x100}}}, {"P1 Right", {{0, 0x200}}},
            {"Select Left", {{0, 0x100}}}, {"Select Right", {{0, 0x200}}},
        };
        const auto &tab = g.cfg.family == "dmx" ? dmx : mambo;
        if (auto s = tab.find(name); s != tab.end()) {
            for (auto [p, mask] : s->second)
                bit(p == 0 ? st.m_stage : p == 1 ? st.m_in2_buttons : p == 2 ? st.m_in3 : st.m_in1_coins, mask);
            return true;
        }
        // their ports leave out the other cabinets' names on these bits (P2 Start, Coin 2, ...)
        if (name != "1 Player Start" && name != "Coin 1" && name != "Test" && name != "Service" &&
            name != "Service Mode" && name != "Service 1" && !(g.cfg.family == "dmx" && name == "2 Players Start"))
            return false;
    }
    // the shell's cabinet names for MAME's (runtime/shell/cabinet.cpp)
    const char *service = g.cfg.family == "gtrf" ? "Service 1 (IN3)" : "Service 1";
    auto it = names.find(name == "Test" ? "Service Mode" : name == "Service" ? service : name);
    if (it == names.end()) return false;
    switch (it->second.first) {
    case 0: return bit(st.m_stage, it->second.second);
    case 1: return bit(st.m_in2_buttons, it->second.second);
    case 2: return bit(st.m_in3, it->second.second);
    default: return bit(st.m_in1_coins, it->second.second);
    }
}

const std::vector<uint8_t> &lamp_state() { return g.state->m_lamps; }
std::string sound_report() {
    if (!g.sound_samples) return {};
    char b[160];
    snprintf(b, sizeof b, "sound: mix peak %.2f of full scale, %.3f %% of samples over it", g.sound_peak,
             100.0 * (double)g.sound_over / (double)g.sound_samples);
    std::string out = b;
    std::string msu;
    u32 msu_rate = 0;
    for (const Board::Decoder &d : g.decoders) {
        if (d.msu) {
            snprintf(b, sizeof b, "%s%.2f", msu.empty() ? "" : "/", d.peak);
            msu += b;
            msu_rate = std::max(msu_rate, d.rate);
            continue;
        }
        snprintf(b, sizeof b, "; the MP3 peaks at %.2f, at %u Hz", d.peak, d.rate);
        out += b;
    }
    if (!msu.empty()) out += "; the unit's MP3s peak at " + msu + ", at " + std::to_string(msu_rate) + " Hz";
    if (g.cfg.fine_sound) {   // the MP3 handed on ahead of the decoders' updates (run())
        u64 predicted = 0, differed = 0;
        for (const Board::Decoder &d : g.decoders) predicted += d.predicted, differed += d.differed;
        snprintf(b, sizeof b, "\nsound: %llu MP3 samples handed on ahead of their decoder's update, %llu of them not as it made them",
                 (unsigned long long)predicted, (unsigned long long)differed);
        out += b;
    }
    return out;
}

uint64_t lamp_changes() { return g.state->m_lamp_changes; }

const uint8_t *mem(uint32_t addr, uint32_t len) {
    // main RAM through any of its mirrors (KUSEG, KSEG0, KSEG1), little-endian as the guest sees it
    const u32 phys = addr & 0x1fffffff;
    ram_device &ram = *g.state->m_ram;
    if ((uint64_t)phys + len > ram.size()) return nullptr;   // (in 64 bits: phys + len wraps in 32)
    return ram.pointer() + phys;
}

uint32_t pc() { return g.state ? static_cast<cpu_device &>(*g.state->m_maincpu).pc() : 0; }

}  // namespace hk::k573
