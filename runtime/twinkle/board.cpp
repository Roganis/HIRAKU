// SPDX-License-Identifier: GPL-3.0-or-later
// The Twinkle board: MAME's twinkle.cpp (BSD-3-Clause, smf, R. Belmont) as a device tree on the
// framework shim, with MAME's own devices. The driver class below follows twinkle_state; where it
// differs, a comment says so.
#include "twinkle/board.h"
#if HK_TWINKLE_VIDEO
#include "twinkle/video.h"
#endif

#include "shell/cabinet.h"
#include "shell/sys.h"
#include "sys573/link.h"
#include "sys573/mips.h"

#include "emu.h"
#include "screen.h"
#include "speaker.h"

#include "bus/ata/ataintf.h"
#include "bus/ata/hdd.h"
#include "bus/nscsi/cd.h"
#include "bus/rs232/rs232.h"
#include "bus/rs232/xvd701.h"
#include "cpu/m68000/m68000.h"
#include "cpu/psx/psx.h"
#include "imagedev/cdromimg.h"
#include "imagedev/harddriv.h"
#include "machine/fdc37c665gt.h"
#include "machine/i2cmem.h"
#include "machine/mb8421.h"
#include "machine/ncr53c90.h"
#include "machine/nscsi_bus.h"
#include "machine/ram.h"
#include "machine/rtc65271.h"
#include "machine/watchdog.h"
#include "machine/x76f041.h"
#include "sound/cdda.h"
#include "sound/rf5c400.h"
#include "sound/spu.h"
#include "video/psx.h"
#include "dirtc.h"

#include <sys/stat.h>

#include <cstdio>
#include <map>

namespace hk::twinkle {

namespace {

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

// The game's configuration while the board is built: the shim runs device_add_mconfig as each
// device is constructed, before init() can hand the board its cfg.
const Config *g_build = nullptr;

// the club kits' session cable to a DDR cabinet's process (Config::session_host), when there is one
k573::SessionLink g_link;
bool session(const Config &c) { return !c.session_host.empty() || !c.session_join.empty(); }

// MAME's ioport output field: the write callback runs when its bits change
// (dynamic_field::write)
struct OutField {
    u32 mask;
    std::function<void(int)> write;
    u32 old = 0;
};

class twinkle_state : public device_t {
public:
    twinkle_state(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock = 0)
        : device_t(mconfig, TWINKLE_STATE, tag, owner, clock),
          m_maincpu(*this, "maincpu"),
          m_audiocpu(*this, "audiocpu"),
          m_gpu(*this, "gpu"),
          m_spu(*this, "spu"),
          m_ram(*this, "ram"),
          m_gpu_ram(*this, "gpu_ram"),
          m_spu_ram(*this, "spu_ram"),
          m_ncr53cf96(*this, "ncr53cf96"),
          m_ata(*this, "ata"),
          m_dpram(*this, "dpram"),
          m_waveram(*this, "rfsnd"),
          m_screen(*this, "screen") {}

    static const device_creator<twinkle_state> TWINKLE_STATE;

    Config cfg;

    required_device<psxcpu_device> m_maincpu;
    required_device<m68000_device> m_audiocpu;
    required_device<psxgpu_device> m_gpu;
    required_device<spu_device> m_spu;
    required_device<ram_device> m_ram;
    required_device<ram_device> m_gpu_ram;
    required_device<ram_device> m_spu_ram;
    required_device<ncr53cf96_device> m_ncr53cf96;
    required_device<ata_interface_device> m_ata;
    required_device<cy7c131_device> m_dpram;
    required_shared_ptr<u16> m_waveram;
    required_device<screen_device> m_screen;
    x76f041_device *m_x76f041 = nullptr;
    i2cmem_device *m_i2cmem = nullptr;

    // ---- state (twinkle_state) ----
    emu_timer *m_dma_timer = nullptr;
    u16 m_spu_ctrl = 0;
    u32 m_spu_ata_dma = 0;
    int m_spu_ata_dmarq = 0;
    u32 m_wave_bank = 0;
    u32 *m_dma_data_ptr = nullptr;
    u32 m_dma_offset = 0;
    s32 m_dma_size = 0;
    bool m_dma_is_write = false;
    bool m_dma_requested = false;
    int m_io_offset = 0;
    int m_output_last[0x100] = {};
    int m_serial_shift = 0, m_serial_bits = 0, m_serial_cs = 0, m_serial_clock = 0;
    int m_output_shift = 0, m_output_bits = 0, m_output_cs = 0, m_output_clock = 0;
    // the sub board, as the game sets it (MAME ignores it): the Bt812's registers through its address
    // register, and the mixer's control register (bit 1: show the video under the PlayStation's black)
    u8 m_bt812_index = 0;
    u8 m_bt812[0x20] = {};
    u16 m_mixer = 0;

    // inputs (MAME's ports as they read)
    u8 m_in0 = 0xff;                // IN0: starts, VEFX, Effect, Test, Service 1, active low
    u8 m_table[2] = {0, 0};         // IN1 (2P), IN2 (1P): the turntables' positions
    // IN3-IN5: the effector's sliders, 4 bits each. MAME's AD_STICK fields start centred: for a
    // default at the range's minimum (0..15) its scaling gives (65536 * (15 << 24) / 131072 + 2^23)
    // >> 24 = 8 (analog_field::apply_settings)
    u8 m_volume[5] = {8, 8, 8, 8, 8};
    u32 m_in6 = 0xffff;             // IN6: the 14 keys and Coin 1, active low
    u32 m_outsec = 0;
    std::vector<OutField> m_outsec_fields;

    // outputs, as cab::lamps("iidx") numbers them: 0 = I/O 0x37, 1 = 0x87 (spotlights), 2 = 0x8f
    // (neon), 3 = the key lamps, 4 = the main board's LEDs; MAME's outputs start at 0 (off), which
    // for the active-low registers is all ones
    u32 m_lamp_reg[5] = {0xff, 0xff, 0xff, 0, 0x1ff};
    std::vector<u8> m_lamps;
    u64 m_lamp_changes = 0;
    char m_display[10] = "         ";   // the 9-character 16-segment display (I/O 0x3f-0x7f), as ASCII
    int m_dial_frac[2] = {0, 0};        // host turntable counts not yet a whole step of the 8-bit position
    void set_lamp_reg(int reg, u32 v);

    void device_add_mconfig(machine_config &config) override;
    void device_start() override;                  // MAME's machine_start: after every other device
    void device_reset_after_children() override;   // machine_reset

    void main_map(address_map &map);
    void sound_map(address_map &map);
    void rf5c400_map(address_map &map);

    // ---- handlers (twinkle.cpp) ----
    void twinkle_io_w(offs_t offset, u8 data);
    u8 twinkle_io_r(offs_t offset);
    void twinkle_videomixer_w(offs_t offset, u16 data);
    void led_w(u16 data) { set_lamp_reg(4, data & 0x1ff); }
    void key_led_w(u16 data) { set_lamp_reg(3, data & 0x3fff); }
    void serial_w(u16 data);
    u32 in6_r() { return m_in6; }
    void outsec_w(offs_t, u32 data, u32 mem_mask);
    u32 insec_r();
    void spu_ctrl_w(u16 data);
    void spu_ata_dma_low_w(u16 data) { m_spu_ata_dma = (m_spu_ata_dma & ~0xffff) | data; }
    void spu_ata_dma_high_w(u16 data) { m_spu_ata_dma = (m_spu_ata_dma & 0xffff) | ((u32)data << 16); }
    // (a bank past wave RAM's 32 MiB, which the register's 16 bits can name: MAME reads and writes
    // whatever memory is there. Here it reads 0 and keeps nothing; the same for the drive's DMA below)
    bool in_waveram(u64 word) { return word < m_waveram.length(); }
    u16 waveram_r(offs_t offset) { return in_waveram((u64)offset + m_wave_bank) ? m_waveram[offset + m_wave_bank] : 0; }
    void waveram_w(offs_t offset, u16 data, u16 mem_mask) {
        if (in_waveram((u64)offset + m_wave_bank)) COMBINE_DATA(&m_waveram[offset + m_wave_bank]);
    }
    void spu_led_w(u16) {}
    void spu_wavebank_w(offs_t, u16 data, u16) {   // (a bank past wave RAM: one no word is in, below)
        m_wave_bank = (u64)data * 0x400000 < m_waveram.length() ? data * 0x400000 : 0xffffffffu;
    }
    u16 spu_dsw_r() { return 0xff; }   // MAME: eight unknown DIP switches, off (active low)
    void spu_ata_irq(int state);
    void spu_ata_dmarq(int state);
    void scsi_dma_read(u32 *ram, u32 n_address, s32 n_size);
    void scsi_dma_write(u32 *ram, u32 n_address, s32 n_size);
    void scsi_drq(int state);
    void scsi_dma_transfer(s32);
};

const device_creator<twinkle_state> twinkle_state::TWINKLE_STATE("twinkle", "Konami Twinkle");

// ---- configuration: twinkle(), then twinkle_dvd_type1/2() and the dongle (twinklex, twinklei) ----
void twinkle_state::device_add_mconfig(machine_config &config) {
    const Config &c = *g_build;
    CXD8530CQ(config, m_maincpu, 67.7376_MHz_XTAL);
    m_maincpu->set_addrmap(AS_PROGRAM, address_map_constructor(&twinkle_state::main_map, this));
    m_maincpu->set_ram(m_ram);
    m_maincpu->subdevice<psxdma_device>("dma")->install_read_handler(5, psxdma_device::read_delegate(&twinkle_state::scsi_dma_read, this));
    m_maincpu->subdevice<psxdma_device>("dma")->install_write_handler(5, psxdma_device::write_delegate(&twinkle_state::scsi_dma_write, this));

    RAM(config, m_ram).set_bits(32).set_default_size("4M").set_default_value(0);

    M68000(config, m_audiocpu, 32000000 / 2);   // 16.000 MHz
    m_audiocpu->set_addrmap(AS_PROGRAM, address_map_constructor(&twinkle_state::sound_map, this));
    // MAME: irq1_line_assert at 60 Hz; the control register clears it
    m_audiocpu->set_periodic_int([this] { m_audiocpu->set_input_line(M68K_IRQ_1, ASSERT_LINE); }, attotime::from_hz(60));

    WATCHDOG_TIMER(config, "watchdog").set_time(attotime::from_msec(1200));   // the LTC1232

    CY7C131(config, m_dpram);
    m_dpram->intl_callback().set_inputline(m_audiocpu, M68K_IRQ_4);

    auto &scsi = NSCSI_BUS(config, "scsi");
    auto &cdrom = NSCSI_XM5401(config, "cdrom");
    cdrom.hk_set_read_rate(c.cd_rate);
    scsi.set_external_device(4, cdrom);
    NCR53CF96(config, m_ncr53cf96, 32_MHz_XTAL / 2);
    scsi.set_external_device(7, *m_ncr53cf96);
    m_ncr53cf96->irq_handler_cb().set(":maincpu:irq", &psxirq_device::intin10);
    m_ncr53cf96->drq_handler_cb().set(*this, &twinkle_state::scsi_drq);

    ATA_INTERFACE(config, m_ata).options(ata_devices, "hdd", nullptr, true);
    m_ata->irq_handler().set(*this, &twinkle_state::spu_ata_irq);
    m_ata->dmarq_handler().set(*this, &twinkle_state::spu_ata_dmarq);

    RTC65271(config, "rtc");

    FDC37C665GT(config, "fdc37c665gt", XTAL(24'000'000));

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

    rf5c400_device &rf5c400 = RF5C400(config, "rfsnd", XTAL(33'868'800) / 2);
    rf5c400.set_addrmap(0, address_map_constructor(&twinkle_state::rf5c400_map, this));

    // the DVD player: the PS1's SIO1 (1st style, twinkle_dvd_type1) or the super I/O's second
    // UART (twinkle_dvd_type2); the other port is the network's, with nothing plugged in
    rs232_port_device &rs232 = RS232_PORT(config, "rs232_dvd");
    rs232.option_add("xvd701", JVC_XVD701);
    rs232.set_default_option("xvd701");
    ns16550_device &uart = *subdevice<ns16550_device>("fdc37c665gt:uart2");
    rs232_port_device &rs232_network = RS232_PORT(config, "rs232_network", default_rs232_devices, nullptr);
    // The club kits' session (Config::session_host): the network port on the cable to the DDR
    // cabinet's process (runtime/sys573/link.h), a null modem as the fork's notes join the two
    // MAMEs: TXD to the far RXD, RTS to the far CTS (Club Version 1 sees the cable on CTS), DTR to
    // the far DSR and DCD. The far side's lines start idle (high), as an empty port's
    if (c.dvd_port == "sio1" && session(c)) {
        auto &sio1 = *subdevice<psxsio1_uart_device>("maincpu:sio1");
        rs232.rxd_handler().set(sio1, &psxsio1_uart_device::write_rxd);
        rs232.dsr_handler().set(sio1, &psxsio1_uart_device::write_dsr);
        rs232.cts_handler().set(sio1, &psxsio1_uart_device::write_cts);
        sio1.txd_handler().set(rs232, &rs232_port_device::write_txd);
        sio1.dtr_handler().set(rs232, &rs232_port_device::write_dtr);
        sio1.rts_handler().set(rs232, &rs232_port_device::write_rts);
        uart.out_tx_callback().set([](int s) { g_link.out(k573::SessionLink::TXD, s); });
        uart.out_dtr_callback().set([](int s) { g_link.out(k573::SessionLink::DTR, s); });
        uart.out_rts_callback().set([](int s) { g_link.out(k573::SessionLink::RTS, s); });
        g_link.in = [&uart](k573::SessionLink::Line l, int s) {
            if (l == k573::SessionLink::TXD) uart.rx_w(s);
            else if (l == k573::SessionLink::RTS) uart.cts_w(s);
            else { uart.dsr_w(s); uart.dcd_w(s); }
        };
    } else if (session(c)) {
        // Club Version 2: SIO1 the fork's UART (hk_psx_sio1_uart), as the fork's SIO1 is
        auto &sio1 = *subdevice<psxsio1_uart_device>("maincpu:sio1");
        sio1.txd_handler().set([](int s) { g_link.out(k573::SessionLink::TXD, s); });
        sio1.dtr_handler().set([](int s) { g_link.out(k573::SessionLink::DTR, s); });
        sio1.rts_handler().set([](int s) { g_link.out(k573::SessionLink::RTS, s); });
        g_link.in = [&sio1](k573::SessionLink::Line l, int s) {
            if (l == k573::SessionLink::TXD) sio1.write_rxd(s);
            else if (l == k573::SessionLink::RTS) sio1.write_cts(s);
            else sio1.write_dsr(s);
        };
        uart.out_tx_callback().set(rs232, &rs232_port_device::write_txd);
        uart.out_dtr_callback().set(rs232, &rs232_port_device::write_dtr);
        uart.out_rts_callback().set(rs232, &rs232_port_device::write_rts);
        rs232.rxd_handler().set(uart, &ns16550_device::rx_w);
        rs232.dcd_handler().set(uart, &ns16550_device::dcd_w);
        rs232.dsr_handler().set(uart, &ns16550_device::dsr_w);
        rs232.ri_handler().set(uart, &ns16550_device::ri_w);
        rs232.cts_handler().set(uart, &ns16550_device::cts_w);
    } else if (c.dvd_port == "sio1") {
        // A departure from MAME (TW-5): the game sends the player a byte only while SIO1's CTS is
        // on (status bit 8), which MAME's SIO lacks, so it stops at E600 DVD INIT ERROR. SIO1 is
        // the fork's UART here (sio_uart.h, hk_psx_sio1_uart, as on the 573's multi-session
        // unit), with the player's CTS wired as MAME's driver has it commented out.
        auto &sio1 = *subdevice<psxsio1_uart_device>("maincpu:sio1");
        rs232.rxd_handler().set(sio1, &psxsio1_uart_device::write_rxd);
        rs232.dsr_handler().set(sio1, &psxsio1_uart_device::write_dsr);
        rs232.cts_handler().set(sio1, &psxsio1_uart_device::write_cts);
        sio1.txd_handler().set(rs232, &rs232_port_device::write_txd);
        sio1.dtr_handler().set(rs232, &rs232_port_device::write_dtr);
        sio1.rts_handler().set(rs232, &rs232_port_device::write_rts);
        uart.out_tx_callback().set(rs232_network, &rs232_port_device::write_txd);
        uart.out_dtr_callback().set(rs232_network, &rs232_port_device::write_dtr);
        uart.out_rts_callback().set(rs232_network, &rs232_port_device::write_rts);
        rs232_network.rxd_handler().set(uart, &ns16550_device::rx_w);
        rs232_network.dcd_handler().set(uart, &ns16550_device::dcd_w);
        rs232_network.dsr_handler().set(uart, &ns16550_device::dsr_w);
        rs232_network.ri_handler().set(uart, &ns16550_device::ri_w);
        rs232_network.cts_handler().set(uart, &ns16550_device::cts_w);
    } else {
        auto *sio1 = subdevice<psxsio1_device>("maincpu:sio1");
        sio1->txd_handler().set(rs232_network, &rs232_port_device::write_txd);
        sio1->dtr_handler().set(rs232_network, &rs232_port_device::write_dtr);
        rs232_network.rxd_handler().set(*sio1, &psxsio1_device::write_rxd);
        rs232_network.dsr_handler().set(*sio1, &psxsio1_device::write_dsr);
        uart.out_tx_callback().set(rs232, &rs232_port_device::write_txd);
        uart.out_dtr_callback().set(rs232, &rs232_port_device::write_dtr);
        uart.out_rts_callback().set(rs232, &rs232_port_device::write_rts);
        rs232.rxd_handler().set(uart, &ns16550_device::rx_w);
        rs232.dcd_handler().set(uart, &ns16550_device::dcd_w);
        rs232.dsr_handler().set(uart, &ns16550_device::dsr_w);
        rs232.ri_handler().set(uart, &ns16550_device::ri_w);
        rs232.cts_handler().set(uart, &ns16550_device::cts_w);
    }

    // the dongle and its port bits (INPUT_PORTS twinklex / twinklei), in mask order as MAME
    // keeps ioport fields
    if (c.dongle_type == "x76f041") {
        m_x76f041 = &X76F041(config, "security");
        m_outsec_fields = {
            {0x04, [this](int s) { m_x76f041->write_cs(s); }},
            {0x08, [this](int s) { m_x76f041->write_sda(s); }},
            {0x10, [this](int s) { m_x76f041->write_scl(s); }},
        };
    } else {
        m_i2cmem = &I2C_M24C02(config, "security");
        m_outsec_fields = {
            {0x08, [this](int s) { m_i2cmem->write_sda(s); }},
            {0x10, [this](int s) { m_i2cmem->write_scl(s); }},
        };
    }
}

void twinkle_state::main_map(address_map &map) {
    map(0x1f000000, 0x1f0007ff).rw(m_dpram, &cy7c131_device::right_r, &cy7c131_device::right_w).umask32(0x00ff00ff);
    map(0x1f200000, 0x1f20001f).m(m_ncr53cf96, &ncr53cf96_device::map).umask16(0x00ff);
    map(0x1f20a01c, 0x1f20a01f).nopw();   // scsi?
    map(0x1f210000, 0x1f2107ff).rw("fdc37c665gt", &fdc37c665gt_device::read, &fdc37c665gt_device::write).umask32(0x00ff00ff);
    map(0x1f218000, 0x1f218000).w("watchdog", &watchdog_timer_device::reset_w);   // LTC1232
    map(0x1f220000, 0x1f220003).w(&twinkle_state::twinkle_io_w).umask32(0x00ff00ff);
    map(0x1f220004, 0x1f220007).r(&twinkle_state::twinkle_io_r).umask32(0x00ff00ff);
    map(0x1f230000, 0x1f230001).w(&twinkle_state::led_w);
    map(0x1f240000, 0x1f240003).r(&twinkle_state::in6_r);
    map(0x1f250000, 0x1f250001).w(&twinkle_state::key_led_w);
    map(0x1f260000, 0x1f260001).w(&twinkle_state::serial_w);
    map(0x1f270000, 0x1f270003).w(&twinkle_state::outsec_w);
    map(0x1f280000, 0x1f280003).r(&twinkle_state::insec_r);
    map(0x1f290000, 0x1f29007f).rw("rtc", &rtc65271_device::rtc_r, &rtc65271_device::rtc_w).umask32(0x00ff00ff);
    map(0x1f2a0000, 0x1f2a007f).rw("rtc", &rtc65271_device::xram_r, &rtc65271_device::xram_w).umask32(0x00ff00ff);
    map(0x1f2b0000, 0x1f2b00ff).w(&twinkle_state::twinkle_videomixer_w);
}

void twinkle_state::sound_map(address_map &map) {
    map(0x000000, 0x07ffff).rom();
    map(0x100000, 0x13ffff).ram();
    map(0x200000, 0x200001).r(&twinkle_state::spu_dsw_r);
    map(0x220000, 0x220001).w(&twinkle_state::spu_led_w);
    map(0x230000, 0x230003).w(&twinkle_state::spu_ctrl_w);
    map(0x240000, 0x240003).w(&twinkle_state::spu_ata_dma_low_w).nopr();
    map(0x250000, 0x250003).w(&twinkle_state::spu_ata_dma_high_w).nopr();
    map(0x260000, 0x260001).w(&twinkle_state::spu_wavebank_w).nopr();
    map(0x280000, 0x2807ff).rw(m_dpram, &cy7c131_device::left_r, &cy7c131_device::left_w).umask16(0x00ff);
    map(0x300000, 0x30000f).rw(m_ata, &ata_interface_device::cs0_r, &ata_interface_device::cs0_w);
    map(0x34000e, 0x34000f).nopw();
    map(0x400000, 0x400fff).rw("rfsnd", &rf5c400_device::rf5c400_r, &rf5c400_device::rf5c400_w);
    map(0x800000, 0xffffff).rw(&twinkle_state::waveram_r, &twinkle_state::waveram_w);
}

void twinkle_state::rf5c400_map(address_map &map) {
    map(0x0000000, 0x1ffffff).ram().share("rfsnd");
}

// MAME's driver_device starts last, once every other device has (driver_device::device_start)
void twinkle_state::device_start() {
    m_dma_timer = timer_alloc(&twinkle_state::scsi_dma_transfer, this);
    m_lamps.assign(cab::lamps(cfg.family).size(), 0);
    const u32 init[5] = {m_lamp_reg[0], m_lamp_reg[1], m_lamp_reg[2], m_lamp_reg[3], m_lamp_reg[4]};
    for (int r = 0; r < 5; r++) { m_lamp_reg[r] = ~init[r]; set_lamp_reg(r, init[r]); }
    m_lamp_changes = 0;
}

void twinkle_state::device_reset_after_children() {
    m_dma_timer->adjust(attotime::never);
    m_dma_data_ptr = nullptr;
    m_dma_offset = 0;
    m_dma_size = 0;
    m_dma_requested = m_dma_is_write = false;
}

// ---- handlers ------------------------------------------------------------------------------
// The I/O board behind an index register: lamps and the 16-segment display out, the inputs in.
// MAME only lights its outputs here; lamps come in TW-2.
void twinkle_state::twinkle_io_w(offs_t offset, u8 data) {
    switch (offset) {
    case 0:
        if (m_output_last[m_io_offset] != data) {
            m_output_last[m_io_offset] = data;
            switch (m_io_offset) {
            case 0x37: set_lamp_reg(0, data); break;
            case 0x87: set_lamp_reg(1, data); break;
            case 0x8f: set_lamp_reg(2, data); break;
            case 0x3f: case 0x47: case 0x4f: case 0x57: case 0x5f: case 0x67: case 0x6f: case 0x77: case 0x7f: {
                // MAME: asciicharset[(data ^ 0xff) & 0x7f], the segments of that ASCII character
                const char ch = (char)((data ^ 0xff) & 0x7f);
                m_display[(m_io_offset - 0x3f) / 8] = ch >= 0x20 && ch < 0x7f ? ch : ' ';
                m_lamp_changes++;
                static const bool trace = getenv("HK_TW_DISPLAY") != nullptr;   // HK_TW_DISPLAY=1: print its changes
                if (trace) fprintf(stderr, "display %.3f [%s]\n", machine().time().as_double(), m_display);
                break;
            }
            }
        }
        break;
    case 1:
        m_io_offset = data;
        break;
    }
}

u8 twinkle_state::twinkle_io_r(offs_t offset) {
    if (offset != 0) return 0;   // MAME: "led status? 1100", reads 0
    switch (m_io_offset) {
    case 0x07: return m_in0;
    case 0x0f: return m_table[1];
    case 0x17: return m_table[0];
    case 0x1f: return (u8)((m_volume[0] & 0x0f) | (m_volume[1] << 4));
    case 0x27: return (u8)((m_volume[2] & 0x0f) | (m_volume[3] << 4));
    case 0x2f: return (u8)(m_volume[4] & 0x0f);
    }
    return 0;
}

void twinkle_state::set_lamp_reg(int reg, u32 v) {
    if (m_lamp_reg[reg] == v) return;
    m_lamp_reg[reg] = v;
    const auto &defs = cab::lamps(cfg.family);
    m_lamps.resize(defs.size());
    for (size_t k = 0; k < defs.size(); k++)
        m_lamps[k] = (u8)(((m_lamp_reg[defs[k].reg] >> defs[k].bit) & 1) ^ (defs[k].active_low ? 1 : 0));
    m_lamp_changes++;
}

// The sub board's Bt812 and overlay: MAME keeps only the bit-banged serial line's shifting
void twinkle_state::twinkle_videomixer_w(offs_t offset, u16 data) {
    if (offset == 0x00) { m_bt812_index = (u8)data; return; }
    if (offset == 0x04) { if (m_bt812_index < sizeof(m_bt812)) m_bt812[m_bt812_index] = (u8)data; return; }
    if (offset == 0x08) { m_mixer = data; return; }
    if (offset != 0x10) return;
    const int clock = data & 1, _do = (data >> 1) & 1, cs = (data >> 2) & 1;
    if (!cs && m_output_cs) { m_output_shift = 0; m_output_bits = 0; }
    if (clock && !m_output_clock && m_output_bits < 8) {
        m_output_shift = (m_output_shift << 1) | _do;
        if (++m_output_bits == 8) { m_output_bits = 0; m_output_shift = 0; }
    }
    m_output_cs = cs;
    m_output_clock = clock;
}

void twinkle_state::serial_w(u16 data) {
    const int _do = (data >> 4) & 1, clock = (data >> 5) & 1, cs = (data >> 6) & 1;
    if (!cs && m_serial_cs) { m_serial_shift = 0; m_serial_bits = 0; }
    if (clock && !m_serial_clock && m_serial_bits < 8) {
        m_serial_shift = (m_serial_shift << 1) | _do;
        if (++m_serial_bits == 8) { m_serial_bits = 0; m_serial_shift = 0; }
    }
    m_serial_cs = cs;
    m_serial_clock = clock;
}

void twinkle_state::outsec_w(offs_t, u32 data, u32 mem_mask) {
    COMBINE_DATA(&m_outsec);
    for (OutField &f : m_outsec_fields) {
        const u32 v = (m_outsec & f.mask) ? 1 : 0;
        if (v != f.old) {
            f.old = v;
            f.write((int)v);
        }
    }
}

u32 twinkle_state::insec_r() {
    const int sda = m_x76f041 ? m_x76f041->read_sda() : m_i2cmem->read_sda();
    return (u32)(sda & 1) << 12;
}

// The SPU board's control register: bits 7-10 enable IRQs 1, 2, 4 and 6; a 1 -> 0 acknowledges
void twinkle_state::spu_ctrl_w(u16 data) {
    if (!(data & 0x0080) && (m_spu_ctrl & 0x0080)) m_audiocpu->set_input_line(M68K_IRQ_1, CLEAR_LINE);
    else if (!(data & 0x0100) && (m_spu_ctrl & 0x0100)) m_audiocpu->set_input_line(M68K_IRQ_2, CLEAR_LINE);
    else if (!(data & 0x0200) && (m_spu_ctrl & 0x0200)) m_audiocpu->set_input_line(M68K_IRQ_4, CLEAR_LINE);
    else if (!(data & 0x0400) && (m_spu_ctrl & 0x0400)) m_audiocpu->set_input_line(M68K_IRQ_6, CLEAR_LINE);
    m_spu_ctrl = data;
}

void twinkle_state::spu_ata_irq(int state) {
    if (state && (m_spu_ctrl & 0x0400)) m_audiocpu->set_input_line(M68K_IRQ_6, ASSERT_LINE);
}

// MAME: the hard disk's DMA goes straight into wave RAM, all of it at once. The words before the
// end of each of the drive's buffers come in one go, straight into wave RAM (hk_read_dma_burst():
// read_dma()'s words, copied at once), the last one through read_dma(), which refills the buffer or
// ends the transfer.
void twinkle_state::spu_ata_dmarq(int state) {
    if (m_spu_ata_dmarq == state) return;
    m_spu_ata_dmarq = state;
    if (!m_spu_ata_dmarq) return;
    m_ata->write_dmack(ASSERT_LINE);
    while (m_spu_ata_dmarq) {
        const size_t at = (size_t)m_wave_bank + m_spu_ata_dma, words = m_waveram.length();
        if (at < words) {
            const int n = m_ata->hk_read_dma_burst(m_waveram.target() + at, (int)std::min<size_t>(words - at, 1 << 16));
            m_spu_ata_dma += n;
            if (n) continue;
        }
        const u16 data = m_ata->read_dma();   // (a word at a time, as MAME; past wave RAM's end it is taken and dropped)
        if (at < words) m_waveram[at] = data;
        m_spu_ata_dma++;
    }
    m_ata->write_dmack(CLEAR_LINE);
}

// SCSI: the PS1's DMA channel 5 to and from the 53CF96, a byte at a time when it asks
void twinkle_state::scsi_dma_read(u32 *ram, u32 n_address, s32 n_size) {
    m_dma_data_ptr = ram;
    m_dma_offset = n_address;
    m_dma_size = n_size * 4;
    m_dma_is_write = false;
    m_dma_timer->adjust(attotime::zero);
}

void twinkle_state::scsi_dma_write(u32 *ram, u32 n_address, s32 n_size) {
    m_dma_data_ptr = ram;
    m_dma_offset = n_address;
    m_dma_size = n_size * 4;
    m_dma_is_write = true;
    m_dma_timer->adjust(attotime::zero);
}

void twinkle_state::scsi_dma_transfer(s32) {
    u8 *ram = reinterpret_cast<u8 *>(m_dma_data_ptr);   // MAME: little_endian_cast (the host is little-endian)
    const u32 ram_bytes = m_ram->size();   // (a transfer sized past the end of RAM: 0 goes out, nothing is kept)
    while (m_dma_requested && m_dma_data_ptr != nullptr && m_dma_size > 0) {
        if (m_dma_is_write) m_ncr53cf96->dma_w(m_dma_offset < ram_bytes ? ram[m_dma_offset] : 0);
        else if (const u8 v = m_ncr53cf96->dma_r(); m_dma_offset < ram_bytes) ram[m_dma_offset] = v;
        m_dma_offset++;
        m_dma_size--;
    }
}

void twinkle_state::scsi_drq(int state) {
    if (!m_dma_requested && state) m_dma_timer->adjust(attotime::zero);
    m_dma_requested = state;
}

// ---- the oracle-format trace (oracle/trace.py): the groups and folding of oracle/lua/hiraku.lua --
struct Tracer {
    FILE *f = nullptr;
    bool have = false;
    u8 kind = 0, group = 0;
    u32 addr = 0, mask = 0, data = 0, hash = 0, frame = 0, pc = 0;
    u16 n = 0;
    bool varied = false;
    double time = 0;
    static u32 fnv(u32 h, u32 v) {
        for (int sh = 0; sh <= 24; sh += 8) h = (h ^ ((v >> sh) & 0xff)) * 0x01000193u;
        return h;
    }
    void emit() {
        if (!have || !f) return;
        u8 rec[32];
        auto put32 = [&](int o, u32 v) { memcpy(rec + o, &v, 4); };
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
    void record(u8 k, u8 g, u32 a, u32 d, u32 m, u32 cur_pc, u32 cur_frame, double t) {
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
        frame = cur_frame;
        time = t;
        pc = cur_pc;
    }
} g_trace;

// the main board's groups (KUSEG addresses) and the 68000's
u8 trace_group_main(u32 a) {
    if (a >= 0x1f000000 && a <= 0x1f0007ff) return 21;   // dpram
    if (a >= 0x1f200000 && a <= 0x1f20ffff) return 22;   // scsi
    if (a >= 0x1f210000 && a <= 0x1f2107ff) return 23;   // superio
    if (a >= 0x1f218000 && a <= 0x1f28ffff) return 24;   // io
    if (a >= 0x1f290000 && a <= 0x1f2a007f) return 25;   // rtc
    if (a >= 0x1f2b0000 && a <= 0x1f2b00ff) return 26;   // mixer
    if (a >= 0x1f801070 && a <= 0x1f80112f) return 15;   // irq, dma, root counters
    if (a >= 0x1f801810 && a <= 0x1f801817) return 16;   // gpu
    if (a >= 0x1f801c00 && a <= 0x1f801fff) return 17;   // spu
    if (a >= 0x1f801040 && a <= 0x1f80105f) return 18;   // sio
    return 0;
}
u8 trace_group_audio(u32 a) {
    if (a >= 0x200000 && a <= 0x26ffff) return 31;
    if (a >= 0x280000 && a <= 0x2807ff) return 32;
    if (a >= 0x300000 && a <= 0x34000f) return 33;
    if (a >= 0x400000 && a <= 0x400fff) return 34;
    return 0;
}

// ---- the machine --------------------------------------------------------------------------
#if HK_TWINKLE_VIDEO
// A DVD player's state from the game's commands (JLIP, as MAME's xvd701 decodes them), with the
// chapter number as sent: three digits, where MAME's model takes a Video CD's two (it never knows
// which disc is in). The rules are xvd701's: PLAY from STOP starts the chapter again, a seek starts
// the chapter and plays unless paused, previous from chapter 1 stays at 1 when not playing.
struct DvdState {
    int chapter = 0, status = 0;   // STOP/PLAYING/PAUSE: 0/1/2
    bool powered = false;
    void command(const unsigned char *c, double t);
};
#endif

struct Board {
#if HK_TWINKLE_VIDEO
    PlayerVideo video;
    DvdState dvd;
#endif
    std::unique_ptr<running_machine> machine;
    twinkle_state *state = nullptr;
    Config cfg;
    FrameHook frame_hook;
    PresentHook present_hook;
    std::vector<u32> present_rgb;
    AudioSink audio_sink;
    u64 frames = 0;
    float sound_peak = 0;
    u64 sound_over = 0, sound_samples = 0;
    std::vector<u32> frame_rgb;
} g;

#if HK_TWINKLE_VIDEO
void DvdState::command(const unsigned char *c, double t) {
    bool restart = false;
    auto seek = [&](int n) {
        if (n <= 0) return;
        chapter = n;
        restart = true;
        if (status != 2) status = 1;
    };
    if (c[3] == 0x0c && c[4] == 0x43 && c[5] == 0x6d) status = 2;                       // PAUSE
    else if (c[3] == 0x0c && c[4] == 0x43 && c[5] == 0x75) {                            // PLAY
        if (status == 0 && chapter <= 0) return;   // nothing to play: an error, nothing changes
        if (status == 0) seek(chapter);
        status = 1;
    } else if (c[3] == 0x0c && c[4] == 0x44 && c[5] == 0x60) status = 0;                // STOP
    else if (c[3] == 0x0c && c[4] == 0x50 && c[5] == 0x20)                              // a chapter
        seek(c[6] % 10 * 100 + c[7] % 10 * 10 + c[8] % 10);
    else if (c[3] == 0x0c && c[4] == 0x50 && c[5] == 0x61)                              // previous
        seek(status != 1 && chapter - 1 == 0 ? 1 : chapter - 1);
    else if (c[3] == 0x0c && c[4] == 0x50 && c[5] == 0x73) seek(chapter + 1);           // next
    else if (c[3] == 0x3e && c[4] == 0x40 && c[5] == 0x60) powered = false;             // power off
    else if (c[3] == 0x3e && c[4] == 0x40 && c[5] == 0x70) powered = true;              // power on
    else return;
    g.video.command(t, chapter, status, powered, restart);
}
#endif

}  // namespace

// The picture: the screen's visible area from `bm`, and the sub board's overlay, the player's
// picture where the PlayStation draws black, at the machine's time.
void picture(screen_device &screen, const bitmap_rgb32 &bm, std::vector<u32> &out, int &w, int &h, bool frame,
             double later = 0) {
    const rectangle &vis = screen.visible_area();
    w = vis.width(), h = vis.height();
    out.resize((size_t)w * h);
    u32 *__restrict dst = out.data();
    for (int y = 0; y < h; y++) {
        const u32 *__restrict src = &bm.pix(vis.top() + y, vis.left());
        for (int x = 0; x < w; x++) dst[x] = src[x] | 0xff000000u;
        dst += w;
    }
#if HK_TWINKLE_VIDEO
    twinkle_state &st = *g.state;
    if (g.video.is_open() && (st.m_mixer & 2)) {
        PlayerVideo::Picture pic;
        pic.brightness = (s8)st.m_bt812[0x08];
        pic.contrast = st.m_bt812[0x09];
        pic.saturation = st.m_bt812[0x0a];
        pic.hue = (s8)st.m_bt812[0x0b];
        g.video.composite(screen.machine().time().as_double() + later, out.data(), w, h, pic);
        if (frame && getenv("HK_TW_VIDEO_AT") && g.frames == (u64)atoll(getenv("HK_TW_VIDEO_AT")))
            fprintf(stderr, "frame %llu t %.6f %dx%d bri %d con %d sat %d hue %d\n", (unsigned long long)g.frames,
                    screen.machine().time().as_double(), w, h, pic.brightness, pic.contrast, pic.saturation, pic.hue);
    }
#else
    (void)frame;
#endif
}

bool init(const Config &cfg) {
    g.cfg = cfg;
    // SIO1 with CTS for the player (TW-5), or for the session cable (Club Version 2)
    hk_psx_sio1_uart = cfg.dvd_port == "sio1" || session(cfg);
    if (session(cfg)) {
        // the IIDX cabinet plays the session's music (the fork's notes): its sound goes with its steps
        const bool host = !cfg.session_host.empty();
        if (!g_link.open(host ? cfg.session_host : cfg.session_join, host, attotime::from_usec(cfg.session_quantum_us), true))
            return false;
    }
    auto rom = [&](const std::string &f) { return cfg.rom_dir + "/" + f; };
    g.machine = std::make_unique<running_machine>();
    running_machine &m = *g.machine;
    m.m_exact = true;
    m.m_base_time = cfg.rtc_time;
    if (!cfg.nvram_dir.empty()) { sys::make_dir(cfg.nvram_dir); m.m_nvram_dir = cfg.nvram_dir; }

    std::vector<u8> bios = load_file(rom(cfg.bios)), spu = load_file(rom(cfg.spu)), dongle = load_file(rom(cfg.dongle));
    if (bios.size() != 0x80000 || spu.size() != 0x80000 || dongle.empty()) {
        fprintf(stderr, "Twinkle needs %s, %s and %s in %s\n", cfg.bios.c_str(), cfg.spu.c_str(), cfg.dongle.c_str(), cfg.rom_dir.c_str());
        return false;
    }
    // the 68000's program (ROM_LOAD16_WORD_SWAP): the file's 16-bit words are little-endian, which
    // is how the shim's big-endian space holds bus words on this host: as it is
    m.add_region(":maincpu:rom", std::move(bios));
    m.add_region(":audiocpu", std::move(spu));
    m.add_region(":security", std::move(dongle));
    cdrom_image_device::hk_set_path(":cdrom:image", cfg.cd);
    harddisk_image_device::hk_set_disk(":ata:0:hdd:image", cfg.hdd, cfg.hdd_cylinders, cfg.hdd_heads, cfg.hdd_sectors);

    g_build = &g.cfg;
    twinkle_state &st = twinkle_state::TWINKLE_STATE(m.config(), "");
    g.state = &st;
    st.cfg = cfg;
    st.m_screen->m_hk_frame = [](screen_device &screen) {
        g.frames++;
        if (!g.frame_hook) return;
        int w, h;
        picture(screen, screen.m_bitmap, g.frame_rgb, w, h, true);
        g.frame_hook(g.frames, g.frame_rgb.data(), w, h);
    };
    m.m_after_nvram_load = [&m] {
        // MAME's set_rtc_datetime: battery-backed RTCs get the power-on time
        for (device_rtc_interface *rtc : device_interface_enumerator<device_rtc_interface>(*m.m_devices.front()))
            if (rtc->has_battery()) rtc->set_current_time(system_time(m.m_base_time));
    };
    m.start();
    m.reset();
    g_link.attach(m);

    // the game's R3000 blocks, recompiled as on the 573 (HK_573_RECORD, HK_573_BLOCKS: sys573/mips.h)
    k573::mips::init(*st.m_maincpu);

    // the player's commands (a HIRAKU edit of MAME's xvd701): HK_TW_PLAYER=1 prints each, with its
    // machine time and frame; with a DVD they drive the picture (below)
    auto *player = st.subdevice<jvc_xvd701_device>("rs232_dvd:xvd701");
    static const bool log_player = getenv("HK_TW_PLAYER") != nullptr;
    running_machine *mp = g.machine.get();
    if (player) {
        player->m_hk_command = [mp](const unsigned char *c) {
            if (log_player) {
                fprintf(stderr, "player f%llu t %.6f:", (unsigned long long)g.frames, mp->time().as_double());
                for (int i = 0; i < 11; i++) fprintf(stderr, " %02x", c[i]);
                fprintf(stderr, "\n");
            }
#if HK_TWINKLE_VIDEO
            if (g.video.is_open() && g.video.is_dvd()) g.dvd.command(c, mp->time().as_double());
#endif
        };
    }

#if HK_TWINKLE_VIDEO
    // the background video. A Video CD follows the player's state from MAME's xvd701 (a HIRAKU
    // edit), at machine time. A DVD follows the game's commands through DvdState: MAME's player
    // reads every chapter number as a Video CD's (divided by ten), and its replies, which the game
    // sees, stay MAME's.
    const std::string &disc = !cfg.video_dvd.empty() ? cfg.video_dvd : cfg.video_cd;
    if (cfg.video && !disc.empty()) {
        if (player && g.video.open(disc)) {
            if (!g.video.is_dvd())
                player->m_hk_state = [mp](int chapter, int status, bool powered, bool from_start) {
                    g.video.command(mp->time().as_double(), chapter, status, powered, from_start);
                };
        } else if (!player) {
            fprintf(stderr, "video: no DVD player in the machine\n");
        }
    }
#endif

    if (!cfg.trace_dir.empty()) {
        g_trace.f = fopen((cfg.trace_dir + "/mmio.bin").c_str(), "wb");
        if (!g_trace.f) { fprintf(stderr, "cannot write %s/mmio.bin\n", cfg.trace_dir.c_str()); return false; }
        address_space &ms = st.m_maincpu->space(AS_PROGRAM), &as = st.m_audiocpu->space(AS_PROGRAM);
        cpu_device *main = st.m_maincpu.target(), *audio = st.m_audiocpu.target();
        auto tap = [&m](cpu_device *cpu, u8 (*grp)(u32), u32 amask, u8 kind) {
            return [&m, cpu, grp, amask, kind](offs_t a, u64 d, u64 mask) {
                const u32 pa = a & amask;
                if (u8 gr = grp(pa)) g_trace.record(kind, gr, pa, (u32)d, (u32)mask, cpu->pc(), (u32)g.frames, m.time().as_double());
            };
        };
        ms.m_read_tap = tap(main, trace_group_main, 0x1fffffff, 0);
        ms.m_write_tap = tap(main, trace_group_main, 0x1fffffff, 1);
        as.m_read_tap = tap(audio, trace_group_audio, 0xffffff, 0);
        as.m_write_tap = tap(audio, trace_group_audio, 0xffffff, 1);
    }
    return true;
}

void set_frame_hook(FrameHook h) { g.frame_hook = std::move(h); }
void set_present_hook(PresentHook h) {
    g.present_hook = std::move(h);
    g.state->m_screen->m_hk_vblank_end = [](screen_device &screen, bitmap_rgb32 &bm) {
        int w, h;
        picture(screen, bm, g.present_rgb, w, h, false);
        g.present_hook(g.present_rgb.data(), w, h);
    };
}
void set_audio_sink(AudioSink s) { g.audio_sink = std::move(s); }

// The GPU draws the frame on show again with what moves carried on (psxgpu_device::hk_between),
// and the player's video comes from the in-between picture's moment too
const u32 *render_between(double t, int &w, int &h) {
    static bitmap_rgb32 bm;
    static std::vector<u32> out;
    screen_device &screen = *g.state->m_screen;
    if (!g.state->m_gpu->hk_between(t, screen, bm)) return nullptr;
    picture(screen, bm, out, w, h, false, t * screen.frame_period().as_double());
    return out.data();
}

[[noreturn]] void run() {
    running_machine &m = *g.machine;
    sound_stream *spu = nullptr, *rf = nullptr, *cd = nullptr;
    for (sound_stream *s : m.m_streams) {
        if (s->m_device == g.state->m_spu.target()) spu = s;
        else if (dynamic_cast<rf5c400_device *>(s->m_device)) rf = s;
        else if (dynamic_cast<cdda_device *>(s->m_device)) cd = s;
    }
    // --fine-sound (Config::fine_sound): the sound for the host every millisecond of machine time,
    // not in MAME's 50 Hz pieces (see the 573's run(), runtime/sys573/board.cpp). The machine is
    // unchanged, the sound isn't: MAME's RF5C400 applies key-on, key-off and the channel registers
    // without first bringing its stream up to date (rf5c400_w), so a keysound starts at the
    // previous update, up to 20 ms before the game triggered it. Updated every millisecond, it
    // starts within a millisecond of it. The SPU's and the CD's sound are the same either way.
    const attotime step = attotime::from_msec(1);
    attotime next_audio;
    if (g.cfg.fine_sound) m.alloc_timer([](s32) {})->adjust(step, 0, step);
    for (;;) {
        m.timeslice();
        // sound: MAME's speaker, the SPU at 0.75, the RF5C400's two main outputs and the CD drive's
        // audio at 1.0 (twinkle.cpp's routes; the RF5C400's effect outputs 2 and 3 go nowhere)
        // (on the session cable, the mix is made for the DDR cabinet even with no sound here)
        if ((!g.audio_sink && !g_link.is_open()) || !spu || !rf) {
            for (sound_stream *s : m.m_streams) for (auto &o : s->m_out) o.clear();
            continue;
        }
        if (g.cfg.fine_sound && m.time() >= next_audio) {
            next_audio = m.time() + step;
            spu->update();
            rf->update();
            if (cd) cd->update();
        }
        size_t n = std::min({spu->m_out[0].size(), spu->m_out[1].size(), rf->m_out[0].size(), rf->m_out[1].size()});
        if (cd) n = std::min({n, cd->m_out[0].size(), cd->m_out[1].size()});
        for (int k = 2; k < rf->output_count(); k++) rf->m_out[k].clear();
        if (!n) continue;
        std::vector<float> lr(n * 2);
        for (int ch = 0; ch < 2; ch++) {
            auto &s = spu->m_out[ch], &r = rf->m_out[ch];
            for (size_t i = 0; i < n; i++) lr[2 * i + ch] = s[i] * 0.75f + r[i];
            s.erase(s.begin(), s.begin() + n);
            r.erase(r.begin(), r.begin() + n);
            if (cd) {
                auto &c = cd->m_out[ch];
                const float gain = static_cast<cdda_device *>(cd->m_device)->output_gain(ch);
                for (size_t i = 0; i < n; i++) lr[2 * i + ch] += c[i] * gain;
                c.erase(c.begin(), c.begin() + n);
            }
        }
        // the session's music for the DDR cabinet too (link.h), at MAME's level: the far side
        // scales it as it does its own sound
        if (g_link.is_open()) g_link.send_audio(lr.data(), n);
        for (float &v : lr) {
            v *= g.cfg.mix_gain;
            const float a = std::fabs(v);
            if (a > g.sound_peak) g.sound_peak = a;
            if (a > 1.0f) g.sound_over++;
        }
        g.sound_samples += lr.size();
        if (g.audio_sink) g.audio_sink(lr.data(), (int)n);
    }
}

void save_nvram() {
    if (g_trace.f) { g_trace.emit(); fclose(g_trace.f); g_trace.f = nullptr; }
    if (g.machine) g.machine->nvram_save();
    k573::mips::finish();
}

void save_nvram_now() {
    if (g.machine) g.machine->nvram_save_now();
}

double seconds() { return g.machine ? g.machine->m_basetime.as_double() : 0.0; }
uint64_t frame_count() { return g.frames; }
const std::string &family() { return g.cfg.family; }

bool set_input(const std::string &name, int value) {
    twinkle_state &st = *g.state;
    // digital inputs by MAME's names (INPUT_PORTS twinkle), active low
    static const std::map<std::string, std::pair<int, u32>> bits = {
        {"1 Player Start", {0, 0x01}}, {"2 Players Start", {0, 0x02}}, {"VEFX", {0, 0x04}}, {"Effect", {0, 0x08}},
        {"Test", {0, 0x10}}, {"Service 1", {0, 0x20}},
        {"P1-1 (SW1)", {6, 0x0001}}, {"P1-2 (SW2)", {6, 0x0002}}, {"P1-3 (SW3)", {6, 0x0004}}, {"P1-4 (SW4)", {6, 0x0008}},
        {"P1-5 (SW5)", {6, 0x0010}}, {"P1-6 (SW6)", {6, 0x0020}}, {"P1-7 (SW7)", {6, 0x0040}},
        {"P2-1 (SW8)", {6, 0x0080}}, {"P2-2 (SW9)", {6, 0x0100}}, {"P2-3 (SW10)", {6, 0x0200}}, {"P2-4 (SW11)", {6, 0x0400}},
        {"P2-5 (SW12)", {6, 0x0800}}, {"P2-6 (SW13)", {6, 0x1000}}, {"P2-7 (SW14)", {6, 0x2000}}, {"Coin 1", {6, 0x4000}},
    };
    std::string n = name == "Service Mode" ? "Test" : name == "Service" ? "Service 1" : name;
    // the keys as the other beatmania cabinets name them (one bindings file for all): "P2 Button 3" -> "P2-3 (SW10)"
    if (n.size() == 11 && n[0] == 'P' && (n[1] == '1' || n[1] == '2') && n.compare(2, 8, " Button ") == 0 && n[10] >= '1' &&
        n[10] <= '7') {
        int p = n[1] - '1', k = n[10] - '0';
        n = "P" + std::to_string(p + 1) + "-" + std::to_string(k) + " (SW" + std::to_string(p * 7 + k) + ")";
    }
    if (auto it = bits.find(n); it != bits.end()) {
        if (it->second.first == 0) {
            if (value) st.m_in0 &= (u8)~it->second.second; else st.m_in0 |= (u8)it->second.second;
        } else {
            if (value) st.m_in6 &= ~it->second.second; else st.m_in6 |= it->second.second;
        }
        return true;
    }
    // analog: the turntables' positions and the effector's sliders
    if (n == "1P Table") { st.m_table[0] = (u8)value; return true; }
    if (n == "2P Table") { st.m_table[1] = (u8)value; return true; }
    for (int k = 0; k < 5; k++)
        if (n == "Volume " + std::to_string(k + 1)) { st.m_volume[k] = (u8)(value & 0x0f); return true; }
    return false;
}

const std::vector<uint8_t> &lamp_state() { return g.state->m_lamps; }
std::string display_text() { return g.state ? std::string(g.state->m_display) : std::string(); }

// Host turntables arrive in Firebeat units (1024 counts a turn); MAME's "1P Table" / "2P Table"
// ports are 8-bit positions, 256 a turn, as DJ Main's dials
void turntable_add(int player, int delta) {
    if (player < 0 || player > 1 || !g.state) return;
    twinkle_state &st = *g.state;
    st.m_dial_frac[player] += delta;
    const int whole = st.m_dial_frac[player] / 4;
    st.m_dial_frac[player] -= whole * 4;
    st.m_table[player] = (u8)(st.m_table[player] + whole);
}
uint64_t lamp_changes() { return g.state->m_lamp_changes; }

std::string sound_report() {
    if (!g.sound_samples) return {};
    char b[160];
    snprintf(b, sizeof b, "sound: mix peak %.2f of full scale, %.3f %% of samples over it", g.sound_peak,
             100.0 * (double)g.sound_over / (double)g.sound_samples);
    return b;
}

const uint8_t *mem(uint32_t addr, uint32_t len) {
    // main RAM through any of its mirrors (KUSEG, KSEG0, KSEG1), little-endian as the guest sees it
    const u32 phys = addr & 0x1fffffff;
    ram_device &ram = *g.state->m_ram;
    if ((uint64_t)phys + len > ram.size()) return nullptr;   // (in 64 bits: phys + len wraps in 32)
    return ram.pointer() + phys;
}

std::string video_report() {
#if !HK_TWINKLE_VIDEO
    return "video: not built (no FFmpeg libraries)";
#else
    if (!g.video.is_open()) return {};
    char b[160];
    snprintf(b, sizeof b, "video: %d chapters on the disc, %llu player commands, %llu frames decoded", g.video.chapters(),
             (unsigned long long)g.video.commands(), (unsigned long long)g.video.frames_decoded());
    return b;
#endif
}

uint32_t pc() { return g.state ? static_cast<cpu_device &>(*g.state->m_maincpu).pc() : 0; }
uint32_t audio_pc() { return g.state ? g.state->m_audiocpu->hk_pc() : 0; }

}  // namespace hk::twinkle
