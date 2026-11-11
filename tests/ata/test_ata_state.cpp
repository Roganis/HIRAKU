// SPDX-License-Identifier: GPL-3.0-or-later
// The ATA bus's state across commands a host cuts short or sends oddly (narrow cases, each once a
// wrong answer or a crash):
//   - IDENTIFY DEVICE after a read whose data was never taken gives the identify block, not the
//     read's sectors; the same after a soft reset in the middle of a read
//   - the host choosing the unit with nothing on it while a command's data or packet is still going
//     out: taken and dropped, and the drive works afterwards
#include <cstdio>
#include <cstring>
#include <vector>

#include <unistd.h>

#include "dev/ata.h"

using namespace hk;

namespace {
int g_fail = 0;
#define CHECK(c)                                                                 \
    do {                                                                         \
        if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } \
    } while (0)

void select_lba(AtaBus &b, uint32_t lba, uint8_t count, int unit = 0) {
    b.write(0, 2, count);
    b.write(0, 3, lba & 0xff);
    b.write(0, 4, (lba >> 8) & 0xff);
    b.write(0, 5, (lba >> 16) & 0xff);
    b.write(0, 6, 0x40 | (unit << 4) | ((lba >> 24) & 0x0f));
}

// IDENTIFY DEVICE's first word: 0x0040 for a fixed disk
uint16_t identify(AtaBus &b) {
    b.write(0, 6, 0x40);
    b.write(0, 7, 0xec);
    CHECK(b.read(0, 7) & 0x08);
    const uint16_t first = b.read(0, 0);
    for (int k = 1; k < 256; k++) b.read(0, 0);
    return first;
}

// one sector read whole: its first byte (each sector is filled with its number)
int read_sector(AtaBus &b, uint32_t lba) {
    select_lba(b, lba, 1);
    b.write(0, 7, 0x20);
    if (!(b.read(0, 7) & 0x08)) return -1;
    const uint16_t w = b.read(0, 0);
    for (int k = 1; k < 256; k++) b.read(0, 0);
    return w & 0xff;
}
}  // namespace

int main() {
    char path[] = "/tmp/hk_ata_stateXXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return 2;
    for (int s = 0; s < 64; s++) {
        std::vector<uint8_t> sec(512, (uint8_t)s);
        if (write(fd, sec.data(), sec.size()) != 512) return 2;
    }
    close(fd);

    {   // a hard disk alone on the bus
        AtaBus bus;
        auto disk = std::make_unique<AtaDevice>("hdd", false);
        disk->disk = std::make_unique<DiskImage>();
        if (!disk->disk->open(path)) return 2;
        bus.attach(0, std::move(disk));

        CHECK(identify(bus) == 0x0040);
        CHECK(read_sector(bus, 7) == 7);

        select_lba(bus, 7, 1);                       // a read started, none of its data taken...
        bus.write(0, 7, 0x20);
        CHECK(identify(bus) == 0x0040);              // ... then IDENTIFY: its own block
        CHECK(read_sector(bus, 9) == 9);

        select_lba(bus, 3, 2);                       // a read cut by a soft reset
        bus.write(0, 7, 0x20);
        bus.write(1, 6, 0x04);
        bus.write(1, 6, 0x00);
        CHECK(bus.read(0, 0) == 0);                  // nothing of it left to read
        CHECK(identify(bus) == 0x0040);
        CHECK(read_sector(bus, 11) == 11);

        bus.write(0, 6, 0x40);                       // SECURITY UNLOCK's block going out...
        bus.write(0, 7, 0xf2);
        CHECK(bus.read(0, 7) & 0x08);
        for (int k = 0; k < 100; k++) bus.write(0, 0, 0x1234);
        bus.write(0, 6, 0x50);                       // ... and the host turns to unit 1, where nothing is
        for (int k = 100; k < 256; k++) bus.write(0, 0, 0x1234);
        CHECK(bus.read(0, 7) == 0);                  // nothing there answers
        CHECK(read_sector(bus, 13) == 13);           // and the disk is as it was
    }
    {   // a CD drive alone (no disc in it)
        AtaBus bus;
        bus.attach(0, std::make_unique<AtaDevice>("cd", true));
        bus.write(0, 6, 0x00);
        bus.write(0, 7, 0xa0);                       // PACKET: twelve bytes to come
        CHECK(bus.read(0, 7) & 0x08);
        bus.write(0, 0, 0x0000);
        bus.write(0, 6, 0x10);                       // the host turns to unit 1 before they are all out
        for (int k = 1; k < 6; k++) bus.write(0, 0, 0x0000);
        bus.write(0, 6, 0x00);
        bus.write(0, 7, 0xa0);                       // TEST UNIT READY on the drive: answered (no disc: an error)
        for (int k = 0; k < 6; k++) bus.write(0, 0, 0x0000);
        CHECK(bus.read(0, 7) & 0x01);
    }

    unlink(path);
    if (g_fail) { fprintf(stderr, "%d failure(s)\n", g_fail); return 1; }
    printf("ata state: ok\n");
    return 0;
}
