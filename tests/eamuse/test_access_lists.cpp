// SPDX-License-Identifier: GPL-3.0-or-later
// runtime/eamuse/access.h: the addresses and the cabinets a server on a network takes, as written
// by whoever runs it.
//
//   test_access_lists      exit 0 when all hold
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

#include "eamuse/access.h"

static int failed = 0;
static void check(bool ok, const char *what) {
    printf("%s%s\n", ok ? "ok    " : "FAIL  ", what);
    failed += !ok;
}
static uint32_t ip(unsigned a, unsigned b, unsigned c, unsigned d) { return a << 24 | b << 16 | c << 8 | d; }

int main() {
    using namespace hk::ea;
    {
        AddressList none;
        check(none.empty() && none.has(ip(8, 8, 8, 8)), "no addresses named: every address is in");
        AddressList l;
        std::string error;
        check(l.add("192.168.1.0/24, 10.0.0.5 172.16.0.0/12", &error) && !l.empty(), "a range, an address and a range, with commas or spaces");
        check(l.has(ip(192, 168, 1, 0)) && l.has(ip(192, 168, 1, 255)) && !l.has(ip(192, 168, 2, 1)) && !l.has(ip(192, 168, 0, 255)), "a /24: its 256 addresses, not its neighbours");
        check(l.has(ip(10, 0, 0, 5)) && !l.has(ip(10, 0, 0, 4)) && !l.has(ip(10, 0, 0, 6)), "one address: that one");
        check(l.has(ip(172, 16, 0, 1)) && l.has(ip(172, 31, 255, 255)) && !l.has(ip(172, 32, 0, 0)) && !l.has(ip(172, 15, 255, 255)), "a /12: to its last address");
        check(l.has(ip(127, 0, 0, 1)) && l.has(ip(127, 5, 5, 5)), "this computer's own addresses: always in");
        check(!l.has(ip(8, 8, 8, 8)), "an address not named: out");
        AddressList any;
        check(any.add("0.0.0.0/0", &error) && any.has(ip(8, 8, 8, 8)), "0.0.0.0/0: every address");
        AddressList in_range;   // an address inside its range, as people write them
        check(in_range.add("192.168.1.20/24", &error) && in_range.has(ip(192, 168, 1, 99)), "192.168.1.20/24: that network");
        for (const char *bad : {"192.168.1.300", "1.2.3", "1.2.3.4/33", "1.2.3.4/", "1.2.3.4.5", "1.2.3.4/8/8", "host.example", "1.2.3.4x", "1.2.3.4/2x", "::1", "-1.2.3.4"}) {
            AddressList b;
            std::string why;
            const bool refused = !b.add(std::string("10.0.0.1, ") + bad, &why) && b.empty() && why.find(bad) != std::string::npos;
            if (!refused) printf("      %s\n", bad);
            check(refused, "not an address: refused, named, and nothing of the text taken");
        }
    }
    {
        check(pcbid_of("01a1b2c3d4e5f6071899") == "01A1B2C3D4E5F6071899", "a PCBID: 20 hex digits, read in capitals");
        check(pcbid_of("01A1B2C3D4E5F607189").empty() && pcbid_of("01A1B2C3D4E5F60718999").empty() && pcbid_of("01A1B2C3D4E5F607189G").empty() && pcbid_of("").empty(),
              "19 or 21 digits, another letter, nothing: not one");
        const uint8_t id[10] = {1, 1, 2, 3, 4, 5, 6, 7, 8, 0};
        const uint8_t c = pcbid_check(id);
        uint8_t again[10] = {9, 1, 2, 3, 4, 5, 6, 7, 8, 0x55};
        check(pcbid_check(again) == c, "the check byte: of the id's bytes 2-8 only (not its first, not itself)");
        again[1] = 0x77;
        check(pcbid_check(again) == c, "... nor its family byte");
        again[2] ^= 1;
        check(pcbid_check(again) != c, "... and another for another id");
        std::set<std::string> made;
        bool good = true;
        for (int i = 0; i < 200; i++) {
            const std::string p = new_pcbid();
            uint8_t b[10];
            for (int k = 0; k < 10; k++) b[k] = (uint8_t)std::stoul(p.substr(2 * k, 2), nullptr, 16);
            good = good && pcbid_of(p) == p && b[0] == 1 && b[9] == pcbid_check(b);
            made.insert(p);
        }
        check(good, "a new PCBID: 20 hex digits, starting 01, its check byte right");
        check(made.size() == 200, "... a different one each time");
    }
    {
        namespace fs = std::filesystem;
        const fs::path file = fs::temp_directory_path() / ("hk-cabinets-" + new_pcbid() + ".txt");
        CabinetList l;
        l.open(file.string());
        check(!l.in_force() && l.has("anything") && l.has(""), "no file: no list, every cabinet is in");
        { std::ofstream(file) << "# none yet\n"; }
        check(l.in_force() && l.size() == 0 && !l.has("01A1B2C3D4E5F6071899") && !l.has(""), "a file with no cabinet in it: a list in force, nobody in");
        check(l.add("01A1B2C3D4E5F6071899", "the living room") && l.has("01a1b2c3d4e5f6071899") && l.size() == 1, "a cabinet added: in, however its letters are written");
        check(!l.has("01A1B2C3D4E5F6071898") && !l.has("01A1B2C3D4E5F6071899 ") && !l.has(""), "... another, one with a space after it, none: out");
        check(l.add("0102030405060708090A", "") && l.has("0102030405060708090A") && l.has("01A1B2C3D4E5F6071899") && l.size() == 2, "a second: both in");
        { std::ofstream(file, std::ios::app) << "  0AAAAAAAAAAAAAAAAAAA# no space before the note\nshort\n01A1B2C3D4E5F6071899 again\n"; }
        check(l.has("0AAAAAAAAAAAAAAAAAAA") && l.size() == 3 && l.not_read().size() == 1 && l.not_read()[0].find("short") != std::string::npos,
              "the file changed by hand: read again; a line that is no PCBID is told, a cabinet twice counts once");
        fs::remove(file);
        check(!l.in_force() && l.has("anything"), "the file removed: no list again");
    }
    printf(failed ? "%d failed\n" : "all hold\n", failed);
    return failed ? 1 : 0;
}
