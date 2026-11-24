// SPDX-License-Identifier: GPL-3.0-or-later
// A disc image's sector format (dev/ata.cpp, DiscImage::open): the 2048 bytes of each sector are
// found whatever the file holds, on discs made up here (each sector's data is its own number):
//   - plain 2048-byte sectors
//   - raw 2352-byte sectors of mode 1 (data after the 16-byte header)
//   - raw 2352-byte sectors of mode 2, a CD-ROM XA disc (data after the 8-byte subheader too):
//     read as mode 1, every sector came 8 bytes early and a game's checks of its disc failed
//   - each of them alone, and through a sheet naming its mode; a sheet naming the wrong mode for a
//     single track (the file's own header is believed); a mode 2 track before an audio track
#include <cstdio>
#include <cstring>
#include <string>
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

const int N = 40;   // sectors of the data track

// sector `lba`'s 2048 bytes: its number in every other byte, a count in between
void data_of(int lba, uint8_t *d) {
    for (int k = 0; k < 2048; k++) d[k] = (k & 1) ? (uint8_t)(k >> 1) : (uint8_t)(lba + 1);
}

// mode 0: 2048-byte sectors; 1: raw mode 1; 2: raw mode 2 XA form 1
std::vector<uint8_t> disc(int mode) {
    std::vector<uint8_t> out;
    for (int lba = 0; lba < N; lba++) {
        uint8_t d[2048];
        data_of(lba, d);
        if (mode == 0) { out.insert(out.end(), d, d + 2048); continue; }
        uint8_t raw[2352] = {};
        memset(raw + 1, 0xff, 10);
        raw[12] = 0, raw[13] = 2, raw[14] = (uint8_t)lba, raw[15] = (uint8_t)mode;   // (address, then the mode)
        if (mode == 2) {
            const uint8_t sub[4] = {0, 0, 0x08, 0};   // (file, channel, submode: data, coding)
            memcpy(raw + 16, sub, 4), memcpy(raw + 20, sub, 4);
            memcpy(raw + 24, d, 2048);
        } else {
            memcpy(raw + 16, d, 2048);
        }
        out.insert(out.end(), raw, raw + 2352);
    }
    return out;
}

bool put(const std::string &path, const void *p, size_t n) {
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = fwrite(p, 1, n, f) == n;
    fclose(f);
    return ok;
}

// every sector read one by one and in runs: its own data
void check_disc(const std::string &path, const char *what) {
    DiscImage img;
    if (!img.open(path)) { fprintf(stderr, "FAIL %s: not opened\n", what); g_fail++; return; }
    int bad = 0;
    for (int lba = 0; lba < N; lba++) {
        uint8_t got[2048], want[2048];
        data_of(lba, want);
        if (!img.read_sector((uint32_t)lba, got) || memcmp(got, want, 2048) != 0) bad++;
    }
    std::vector<uint8_t> run(5 * 2048);
    if (img.read_sectors(7, 5, run.data())) {
        for (int k = 0; k < 5; k++) {
            uint8_t want[2048];
            data_of(7 + k, want);
            if (memcmp(run.data() + (size_t)k * 2048, want, 2048) != 0) bad++;
        }
    } else bad++;
    if (bad) { fprintf(stderr, "FAIL %s: %d sectors wrong\n", what, bad); g_fail++; }
    else printf("ok    %s\n", what);
}
}  // namespace

int main() {
    char dir[] = "/tmp/hk_disc_modesXXXXXX";
    if (!mkdtemp(dir)) return 2;
    const std::string d = dir;
    std::vector<std::string> made;
    auto file = [&](const std::string &name, const void *p, size_t n) {
        made.push_back(d + "/" + name);
        if (!put(made.back(), p, n)) exit(2);
        return made.back();
    };
    auto sheet = [&](const std::string &name, const std::string &bin, const char *tracks) {
        const std::string s = "FILE \"" + bin + "\" BINARY\n" + tracks;
        return file(name, s.data(), s.size());
    };
    const auto iso = disc(0), m1 = disc(1), m2 = disc(2);
    check_disc(file("plain.iso", iso.data(), iso.size()), "2048-byte sectors");
    check_disc(file("m1.bin", m1.data(), m1.size()), "raw mode 1, the file alone");
    check_disc(file("m2.bin", m2.data(), m2.size()), "raw mode 2, the file alone");
    check_disc(sheet("m1.cue", "m1.bin", "  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n"), "raw mode 1, by its sheet");
    check_disc(sheet("m2.cue", "m2.bin", "  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n"), "raw mode 2, by its sheet");
    check_disc(sheet("m2-wrong.cue", "m2.bin", "  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n"),
               "raw mode 2, its sheet saying mode 1");
    check_disc(sheet("m1-wrong.cue", "m1.bin", "  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n"),
               "raw mode 1, its sheet saying mode 2");
    // a mode 2 track, then two seconds of sound
    std::vector<uint8_t> mixed = m2;
    mixed.resize(mixed.size() + 150 * 2352, 0x11);
    file("mixed.bin", mixed.data(), mixed.size());
    check_disc(sheet("mixed.cue", "mixed.bin",
                     "  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n  TRACK 02 AUDIO\n    INDEX 01 00:00:40\n"),
               "raw mode 2 before an audio track");
    for (const auto &p : made) unlink(p.c_str());
    rmdir(dir);
    if (g_fail) return 1;
    puts("disc modes: all read");
    return 0;
}
