// SPDX-License-Identifier: GPL-3.0-or-later
// The System 573's memory card reader with its PlayStation memory cards (runtime/sys573/memcard.h),
// with no game: MAME's reader (k573mcr) on a JVS host and MAME's cards (psxcard) in its two ports.
//
//   The card's own protocol, bit by bit on the controller port as the reader drives it: a read
//   ('R': the IDs, the address back, the frame, its checksum, 'G'), a write ('W': written on 'G'
//   only when the checksum is right, else 'N' and the card as it was), an empty slot (no answer).
//
//   The reader's JVS commands: its name, the status of an empty slot, frames read into its buffer
//   and read back from it, the first write after a reset not reaching the card (MAME's model) and
//   the next ones reaching it, a card taken out mid-way, the second slot.
//
//   The cards' files (MemoryCards): a file that isn't there becomes a formatted card, a write is
//   in the file after keep(), a file that isn't 128 KiB is refused.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "sys573/memcard.h"
#include "emu.h"
#include "imagedev/memcard.h"
#include "bus/psx/ctlrport.h"
#include "bus/psx/memcard.h"
#include "machine/jvshost.h"
#include "k573mcr.h"

namespace {

int g_failed = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c), g_failed++; \
    } while (0)

class test_host : public jvs_host {
public:
    test_host(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock = 0)
        : jvs_host(mconfig, TYPE, tag, owner, clock) {}
    static const device_creator<test_host> TYPE;
    // a packet to `node` (its command bytes), and the reply's bytes after the header: the status,
    // then the report and data of each command
    std::vector<u8> send(u8 node, const std::vector<u8> &cmd) {
        push(node);
        push((u8)(cmd.size() + 1));
        for (u8 b : cmd) push(b);
        commit_raw();
        const u8 *r;
        u32 n;
        get_raw_reply(r, n);
        return n > 2 ? std::vector<u8>(r + 2, r + n) : std::vector<u8>();
    }
};
const device_creator<test_host> test_host::TYPE("test_host", "a JVS host for the test");

class rig : public device_t {
public:
    rig(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock = 0)
        : device_t(mconfig, TYPE, tag, owner, clock), m_host(*this, "host") {}
    static const device_creator<rig> TYPE;
    required_device<test_host> m_host;
    void device_add_mconfig(machine_config &config) override {
        test_host::TYPE(config, "host");
        KONAMI_573_MEMORY_CARD_READER(config, "k573mcr", m_host);
    }
    void device_start() override {}
};
const device_creator<rig> rig::TYPE("rig", "a JVS host and the memory card reader");

// A card with something in each frame: frame n's bytes are (n * 7 + i) & 0xff
std::vector<u8> patterned() {
    std::vector<u8> c = hk::k573::blank_memory_card();
    for (size_t n = 64; n < 1024; n++)
        for (size_t i = 0; i < 128; i++) c[n * 128 + i] = (u8)(n * 7 + i);
    return c;
}

// The controller port, a byte at a time, as the reader drives it (k573mcr.cpp's
// controller_port_send_byte): the clock down, the bit out, the clock up; the card answers on the
// rising edge, least significant bit first
struct Port {
    psxcontrollerports_device *ports;
    psx_controller_port_device *p[2];
    bool clock = false;
    void select(int k) { ports->write_dtr(!!k), ports->write_dtr(!k); }
    u8 byte(u8 out) {
        u8 in = 0;
        for (int i = 0; i < 8; i++) {
            ports->write_sck(clock), clock = !clock;
            ports->write_txd(BIT(out, i));
            ports->write_sck(clock), clock = !clock;
            in |= (u8)((p[0]->rx_r() && p[1]->rx_r()) << i);
        }
        return in;
    }
};

u8 xor_of(const u8 *b, size_t n, u8 x = 0) {
    for (size_t i = 0; i < n; i++) x ^= b[i];
    return x;
}

void test_blank() {
    const std::vector<u8> c = hk::k573::blank_memory_card();
    CHECK(c.size() == 131072);
    CHECK(c[0] == 'M' && c[1] == 'C');
    for (size_t n = 0; n < 64; n++) CHECK(c[n * 128 + 127] == hk::k573::memory_card_frame_sum(&c[n * 128]));
    for (size_t n = 1; n <= 15; n++) CHECK(c[n * 128] == 0xa0 && c[n * 128 + 8] == 0xff && c[n * 128 + 9] == 0xff);
    for (size_t n = 16; n <= 35; n++) CHECK(c[n * 128] == 0xff && c[n * 128 + 3] == 0xff);
    CHECK(memcmp(&c[63 * 128], &c[0], 128) == 0);
    CHECK(c[64 * 128] == 0 && c[131071] == 0);
}

void test_card_protocol(rig &r) {
    Port port;
    port.ports = r.subdevice<psxcontrollerports_device>("k573mcr:controllers");
    port.p[0] = r.subdevice<psx_controller_port_device>("k573mcr:port1");
    port.p[1] = r.subdevice<psx_controller_port_device>("k573mcr:port2");
    auto *card = r.subdevice<psxcard_device>("k573mcr:port1:card");
    CHECK(port.ports && port.p[0] && port.p[1] && card);
    if (!port.ports || !card) return;

    // no card: nothing answers (the line stays high)
    port.select(0);
    CHECK(port.byte(0x81) == 0xff);
    CHECK(port.byte('R') == 0xff);

    std::vector<u8> bytes = patterned();
    card->hk_insert(&bytes);

    // a read of frame 0x123
    port.select(0);
    CHECK(port.byte(0x81) == 0xff);   // (the line before the card's first answer)
    CHECK(port.byte('R') == 0x00);    // the flag byte (MAME's card: 0)
    CHECK(port.byte(0) == 0x5a);
    CHECK(port.byte(0) == 0x5d);
    CHECK(port.byte(0x01) == 0x00);
    CHECK(port.byte(0x23) == 0x01);   // the address's high byte back
    CHECK(port.byte(0) == 0x5c);      // (MAME's card: the low byte's turn answers the acknowledge)
    CHECK(port.byte(0) == 0x5d);
    CHECK(port.byte(0) == 0x01);
    CHECK(port.byte(0) == 0x23);
    u8 frame[128];
    for (u8 &b : frame) b = port.byte(0);
    CHECK(memcmp(frame, &bytes[0x123 * 128], 128) == 0);
    const u8 sum = port.byte(0);
    CHECK(sum == xor_of(frame, 128, 0x01 ^ 0x23));
    CHECK(port.byte(0) == 'G');

    // a write with a wrong checksum: 'N', the card as it was
    auto write = [&](u16 addr, const u8 *data, u8 sum) {
        port.select(0);
        port.byte(0x81);
        port.byte('W');
        port.byte(0), port.byte(0);
        port.byte((u8)(addr >> 8)), port.byte((u8)addr);
        for (int i = 0; i < 128; i++) port.byte(data[i]);
        port.byte(sum);
        CHECK(port.byte(0) == 0x5c);
        CHECK(port.byte(0) == 0x5d);
        return port.byte(0);
    };
    u8 data[128];
    for (int i = 0; i < 128; i++) data[i] = (u8)(0xa5 ^ i);
    const u8 good = xor_of(data, 128, 0x02 ^ 0x40);
    const std::vector<u8> before = bytes;
    const u64 writes = card->hk_writes();
    CHECK(write(0x240, data, (u8)(good ^ 1)) == 'N');
    CHECK(bytes == before);
    CHECK(card->hk_writes() == writes);
    // ... and with the right one: 'G', the frame written, nothing else
    CHECK(write(0x240, data, good) == 'G');
    CHECK(memcmp(&bytes[0x240 * 128], data, 128) == 0);
    CHECK(card->hk_writes() == writes + 1);
    std::vector<u8> expect = before;
    memcpy(&expect[0x240 * 128], data, 128);
    CHECK(bytes == expect);
    card->hk_insert(nullptr);
}

void test_reader(rig &r) {
    test_host &host = *r.m_host;
    auto *card1 = r.subdevice<psxcard_device>("k573mcr:port1:card");
    auto *card2 = r.subdevice<psxcard_device>("k573mcr:port2:card");
    CHECK(card1 && card2);
    if (!card1 || !card2) return;

    // the address, and what the reader says it is
    std::vector<u8> a = host.send(0xff, {0xf1, 0x01});
    CHECK(a.size() >= 2 && a[0] == 0x01 && a[1] == 0x01);
    std::vector<u8> id = host.send(0x01, {0x10});
    CHECK(id.size() > 2 && std::string((const char *)&id[2]) == "KONAMI CO.,LTD.;White I/O;Ver1.0;White I/O PCB");

    auto status = [&] {
        std::vector<u8> s = host.send(0x01, {0x71});
        return s.size() >= 4 ? (u16)(s[2] << 8 | s[3]) : (u16)0xffff;
    };
    // frames from slot `k`'s card at `addr` into the reader's buffer at `ram`
    auto read = [&](int k, u16 addr, u32 ram, u16 count) {
        std::vector<u8> s = host.send(0x01, {0x76, 0x74, (u8)((k << 7) | (addr >> 8)), (u8)addr, (u8)(ram >> 16), (u8)(ram >> 8),
                                             (u8)ram, (u8)(count >> 8), (u8)count});
        CHECK(s.size() >= 3 && s[1] == 0x01);
    };
    auto write = [&](int k, u16 addr, u32 ram, u16 count) {
        std::vector<u8> s = host.send(0x01, {0x76, 0x75, (u8)(ram >> 16), (u8)(ram >> 8), (u8)ram, (u8)((k << 7) | (addr >> 8)),
                                             (u8)addr, (u8)(count >> 8), (u8)count});
        CHECK(s.size() >= 3 && s[1] == 0x01);
    };
    auto buffer = [&](u32 ram, u8 n) {   // the reader's buffer, back to the host
        std::vector<u8> s = host.send(0x01, {0x70, 0x00, (u8)(ram >> 16), (u8)(ram >> 8), (u8)ram, n});
        return s.size() >= 2 + (size_t)n ? std::vector<u8>(s.begin() + 2, s.begin() + 2 + n) : std::vector<u8>();
    };
    auto to_buffer = [&](u32 ram, const u8 *data, u8 n) {
        std::vector<u8> cmd = {0x70, 0x01, (u8)(ram >> 16), (u8)(ram >> 8), (u8)ram, n};
        cmd.insert(cmd.end(), data, data + n);
        host.send(0x01, cmd);
    };

    // an empty slot
    read(0, 0, 0x020000, 1);
    CHECK(status() == 0x0008);

    std::vector<u8> one = patterned(), two = hk::k573::blank_memory_card();
    card1->hk_insert(&one);
    card2->hk_insert(&two);
    read(0, 0, 0x020000, 1);   // the header frame
    CHECK(status() == 0x8000);
    std::vector<u8> got = buffer(0x020000, 128);
    CHECK(got.size() == 128 && memcmp(got.data(), &one[0], 128) == 0);
    read(0, 0x0100, 0x020000, 2);   // two frames
    CHECK(status() == 0x8000);
    got = buffer(0x020080, 128);
    CHECK(got.size() == 128 && memcmp(got.data(), &one[0x101 * 128], 128) == 0);
    read(1, 0x0003, 0x030000, 1);   // the second slot
    got = buffer(0x030000, 128);
    CHECK(got.size() == 128 && memcmp(got.data(), &two[3 * 128], 128) == 0);

    // the first write after a reset: the reader takes it (the game's 0xff block) but the card
    // doesn't (MAME's model); the next one reaches the card
    u8 data[128];
    for (int i = 0; i < 128; i++) data[i] = (u8)(0x3c + i);
    to_buffer(0x040000, data, 128);
    const std::vector<u8> before = one;
    const u64 writes = card1->hk_writes();
    write(0, 0x0080, 0x040000, 1);
    CHECK(one == before);
    CHECK(card1->hk_writes() == writes);
    write(0, 0x0080, 0x040000, 1);
    CHECK(status() == 0x8000);
    CHECK(memcmp(&one[0x80 * 128], data, 128) == 0);
    CHECK(card1->hk_writes() == writes + 1);
    std::vector<u8> expect = before;
    memcpy(&expect[0x80 * 128], data, 128);
    CHECK(one == expect);
    CHECK(two == hk::k573::blank_memory_card());

    // taken out: no card, no write
    card1->hk_insert(nullptr);
    write(0, 0x0081, 0x040000, 1);
    CHECK(status() == 0x0008);
    CHECK(one == expect);
    card2->hk_insert(nullptr);
}

void test_files(rig &r, const std::string &dir) {
    namespace fs = std::filesystem;
    fs::remove_all(dir);
    fs::create_directories(dir);
    auto *card1 = r.subdevice<psxcard_device>("k573mcr:port1:card");
    auto *card2 = r.subdevice<psxcard_device>("k573mcr:port2:card");
    const std::string f1 = dir + "/one.mcr", f2 = dir + "/sub/two.mcr";
    hk::k573::MemoryCards cards;
    cards.configure(f1, "");
    std::string why;
    CHECK(cards.attach(card1, card2, why));
    CHECK(cards.in(0) && !cards.in(1));
    auto file = [](const std::string &f) {
        std::ifstream in(f, std::ios::binary);
        return std::vector<u8>(std::istreambuf_iterator<char>(in), {});
    };
    CHECK(file(f1) == hk::k573::blank_memory_card());   // made, formatted

    // a frame written through the card: in the file after keep()
    card1->fseek(70 * 128, SEEK_SET);
    u8 data[128];
    memset(data, 0x5a, sizeof data);
    card1->fwrite(data, 128);
    cards.tick(10.0);
    cards.keep(false, 10.5);   // (written half a second ago: still being written, waits)
    CHECK(file(f1) == hk::k573::blank_memory_card());
    cards.keep(true, 10.5);    // (the end)
    std::vector<u8> f = file(f1);
    CHECK(f.size() == 131072 && memcmp(&f[70 * 128], data, 128) == 0);

    // taken out and in again: read from the file
    CHECK(cards.toggle(0) == "Memory card 1 taken out");
    CHECK(!cards.in(0));
    CHECK(cards.toggle(1).empty());   // (no card given for slot 2)
    CHECK(cards.toggle(0) == "Memory card 1 in its slot");
    std::vector<u8> back(128);
    card1->fseek(70 * 128, SEEK_SET);
    card1->fread(back.data(), 128);
    CHECK(memcmp(back.data(), data, 128) == 0);

    // the first write after a card goes in goes nowhere (the departure from MAME, memcard.h): the
    // game's block of ff to frame 63; the next one lands
    const std::vector<u8> before_init = file(f1);
    u8 ff[128];
    memset(ff, 0xff, sizeof ff);
    const u64 w0 = card1->hk_writes();
    cards.before_reader_write(0);
    card1->fseek(63 * 128, SEEK_SET);
    card1->fwrite(ff, 128);
    cards.after_reader_write();
    CHECK(card1->hk_writes() == w0);
    cards.keep(true, 20.0);
    CHECK(file(f1) == before_init);
    cards.before_reader_write(0);
    card1->fseek(71 * 128, SEEK_SET);
    card1->fwrite(ff, 128);
    cards.after_reader_write();
    CHECK(card1->hk_writes() == w0 + 1);
    cards.keep(true, 20.0);
    std::vector<u8> after = file(f1);
    CHECK(after.size() == 131072 && memcmp(&after[71 * 128], ff, 128) == 0 && memcmp(&after[63 * 128], &before_init[63 * 128], 128) == 0);
    // ... and again after the card is taken out and put back
    cards.toggle(0), cards.toggle(0);
    cards.before_reader_write(0);
    card1->fseek(63 * 128, SEEK_SET);
    card1->fwrite(ff, 128);
    cards.after_reader_write();
    cards.keep(true, 30.0);
    CHECK(file(f1) == after);

    // a file that isn't a raw card image: refused
    fs::create_directories(dir + "/sub");
    std::ofstream(f2, std::ios::binary) << std::string(1000, 'x');
    hk::k573::MemoryCards other;
    other.configure("", f2);
    CHECK(!other.attach(card1, card2, why));
    CHECK(why.find("raw image") != std::string::npos);
    card1->hk_insert(nullptr);
    card2->hk_insert(nullptr);
    fs::remove_all(dir);
}

}  // namespace

int main(int argc, char **argv) {
    const std::string dir = argc > 1 ? argv[1] : (std::filesystem::temp_directory_path() / "hiraku_test_memcard").string();
    test_blank();
    running_machine m;
    m.m_exact = true;
    rig &r = static_cast<rig &>(rig::TYPE(m.config(), ""));
    m.start();
    m.reset();
    test_card_protocol(r);
    test_reader(r);
    test_files(r, dir);
    if (g_failed) {
        fprintf(stderr, "memcard: %d checks failed\n", g_failed);
        return 1;
    }
    printf("memcard: card protocol, reader commands and card files as expected\n");
    return 0;
}
