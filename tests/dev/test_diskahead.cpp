// SPDX-License-Identifier: GPL-3.0-or-later
// The disk read-ahead (dev/diskahead.h) on a made-up 24 MiB image (each 512-byte sector holds its
// own number):
//   - after a read somewhere, nothing more is read until the game reads on from there; then twice
//     what it has read, up to the window, and no further until it moves
//   - the game's reads along the stream are found read ahead; a read elsewhere starts a new stream
//   - nothing past the end of the file; off, nothing is read but the reads are still timed
//   - a hard disk read through dev/ata.h with the read-ahead on gets the image's bytes
//   - destroyed while its thread reads: no harm
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "dev/ata.h"
#include "dev/diskahead.h"

using namespace hk;

namespace {
int g_fail = 0;
#define CHECK(c)                                                                 \
    do {                                                                         \
        if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } \
    } while (0)

constexpr uint64_t MIB = 1 << 20, SIZE = 24 * MIB, WINDOW = 2 * MIB, PIECE = 64 << 10;

void sector_of(uint64_t lba, uint8_t *d) {
    for (int k = 0; k < 512; k += 8) memcpy(d + k, &lba, 8);
}

void read_at(DiskAhead &a, uint64_t off, uint64_t n) {
    a.before(off, n);
    a.after();
}
}  // namespace

int main(int argc, char **argv) {
    const std::string dir = argc > 1 ? argv[1] : ".";
    const std::string path = dir + "/diskahead-test.img";
    {
        FILE *f = fopen(path.c_str(), "wb");
        if (!f) { fprintf(stderr, "can't write %s\n", path.c_str()); return 1; }
        uint8_t s[512];
        for (uint64_t lba = 0; lba < SIZE / 512; lba++) sector_of(lba, s), fwrite(s, 1, 512, f);
        fclose(f);
    }

    {   // the window after the first read, then nothing more until the game moves on
        DiskAhead a(path, SIZE, WINDOW, PIECE, 1);
        CHECK(a.on());
        read_at(a, 0, 512);
        a.wait_caught_up();
        DiskAhead::Stats s = a.stats();
        CHECK(s.reads == 1 && s.hit_bytes == 0 && s.jumps == 0);
        // the first 64 KiB read by the caller in one go, nothing by the thread (twice 512 bytes)
        CHECK(s.ahead_bytes == 0);
        // scattered reads of 8 KiB: none read ahead either
        for (uint64_t k = 1; k <= 8; k++) read_at(a, k * 2 * MIB, 8192);
        a.wait_caught_up();
        CHECK(a.stats().ahead_bytes == 0 && a.stats().jumps == 8);
        read_at(a, 0, 512);
        a.wait_caught_up();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CHECK(a.stats().ahead_bytes == 0);
        // along the stream: every read found read ahead, and the window moves with them
        for (uint64_t off = 512; off < MIB; off += 512) {
            a.wait_caught_up();
            read_at(a, off, 512);
        }
        a.wait_caught_up();
        s = a.stats();
        CHECK(s.hit_bytes == MIB - 512);
        CHECK(s.ahead_bytes == MIB + WINDOW - (64 << 10));   // (the window full: 1 MiB read, 2 ahead)
        CHECK(s.jumps == 9);
        // somewhere else: a new stream there, read ahead as the game reads on
        read_at(a, 20 * MIB, 4096);
        CHECK(a.stats().jumps == 10);
        CHECK(a.stats().hit_bytes == MIB - 512);
        for (uint64_t off = 20 * MIB + 4096; off < 21 * MIB; off += 4096) {
            a.wait_caught_up();
            read_at(a, off, 4096);
        }
        CHECK(a.stats().hit_bytes == MIB - 512 + MIB - 4096);
        // a little further on (within the window): the same stream, no jump
        read_at(a, 20 * MIB + MIB + 8192, 512);
        CHECK(a.stats().jumps == 10);
        // to the end of the file: nothing read past it
        read_at(a, SIZE - 4096, 4096);
        a.wait_caught_up();
        const uint64_t before = a.stats().ahead_bytes;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CHECK(a.stats().ahead_bytes == before);
        // a slow read is timed
        a.before(0, 512);
        std::this_thread::sleep_for(std::chrono::milliseconds(6));
        a.after();
        s = a.stats();
        CHECK(s.over_4ms == 1 && s.over_16ms == 0 && s.longest_ns >= 6'000'000 && s.longest_at == 0);
        CHECK(s.jumps == 12);   // (the read near the end was one too)
        CHECK(a.report().find("ahead; the game read") != std::string::npos);
    }

    {   // off: nothing read, the reads timed all the same
        DiskAhead a(path, SIZE, WINDOW, PIECE, 0);
        CHECK(!a.on());
        for (uint64_t off = 0; off < MIB; off += 512) read_at(a, off, 512);
        a.wait_caught_up();
        DiskAhead::Stats s = a.stats();
        CHECK(s.reads == MIB / 512 && s.ahead_bytes == 0 && s.hit_bytes == 0);
        CHECK(a.report().find("off (HK_DISK_AHEAD=0") != std::string::npos);
    }

    {   // destroyed while its thread reads a large window
        DiskAhead a(path, SIZE, 16 * MIB, 4096, 1);
        read_at(a, 0, 512);
    }

    {   // a hard disk through dev/ata.h (the read-ahead on unless HK_DISK_AHEAD=0 here: either way the
        // image's bytes), along a stream and after a jump
        DiskImage d;
        CHECK(d.open(path));
        std::vector<uint8_t> got(64 * 512);
        uint8_t want[512];
        bool same = true;
        for (uint32_t lba : {0u, 64u, 128u, 30000u, 30064u, 100u}) {
            CHECK(d.read_sectors(lba, 64, got.data()));
            for (uint32_t k = 0; k < 64; k++) {
                sector_of(lba + k, want);
                same = same && memcmp(got.data() + (size_t)k * 512, want, 512) == 0;
            }
        }
        CHECK(same);
    }

    remove(path.c_str());
    if (g_fail) {
        fprintf(stderr, "%d failed\n", g_fail);
        return 1;
    }
    printf("disk read-ahead: ok\n");
    return 0;
}
