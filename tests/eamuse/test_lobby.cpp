// SPDX-License-Identifier: GPL-3.0-or-later
// NET TAISEN's lobby (runtime/python/lobby.h): what anyone who reaches it can make it hold is
// bounded. An entry is kept an hour at most, whatever it asks, and 1024 entries at most.
#include <cstdio>
#include <cstring>
#include <vector>

#include "python/lobby.h"

static int failed = 0;
static void check(bool ok, const char *what) {
    printf("%s  %s\n", ok ? "ok   " : "FAIL ", what);
    failed += !ok;
}

// a datagram as the game makes it: "lob0", length, CRC-16/X-25 (taken with its field 0), sequence, kind
static std::vector<uint8_t> packet(int kind, const std::vector<uint8_t> &body) {
    std::vector<uint8_t> p(12 + body.size(), 0);
    memcpy(p.data(), "lob0", 4);
    p[4] = (uint8_t)(p.size() >> 8), p[5] = (uint8_t)p.size();
    p[9] = 1, p[10] = (uint8_t)kind;
    memcpy(p.data() + 12, body.data(), body.size());
    uint32_t c = 0xffff;
    for (uint8_t b : p) {
        c ^= b;
        for (int k = 0; k < 8; k++) c = c & 1 ? c >> 1 ^ 0x8408 : c >> 1;
    }
    c = ~c & 0xffff;
    p[6] = (uint8_t)(c >> 8), p[7] = (uint8_t)c;
    return p;
}

// an entry in room 1 from 10.0.0.1:port, asking to be kept `seconds`
static std::vector<uint8_t> entry(uint16_t port, uint16_t seconds) {
    std::vector<uint8_t> b(28, 0);
    b[3] = 1;
    b[4] = 10, b[7] = 1;
    b[8] = (uint8_t)(port >> 8), b[9] = (uint8_t)port;
    b[10] = (uint8_t)(seconds >> 8), b[11] = (uint8_t)seconds;
    return packet(0, b);
}

// the ports listed for room 1 (the PC dialect: the header's byte 11 counts them, 0x1c bytes each)
static std::vector<uint16_t> listed(hk::py::Lobby &l, double now) {
    const std::vector<uint8_t> ask = packet(1, {0, 0, 0, 1});
    const std::vector<uint8_t> a = l.handle(ask.data(), ask.size(), now);
    std::vector<uint16_t> ports;
    for (size_t i = 0; a.size() >= 12 && i < a[11] && 12 + 0x1c * (i + 1) <= a.size(); i++)
        ports.push_back((uint16_t)(a[12 + 0x1c * i + 8] << 8 | a[12 + 0x1c * i + 9]));
    return ports;
}

int main() {
    {
        hk::py::Lobby l(true);
        const auto e = entry(5700, 30);
        check(!l.handle(e.data(), e.size(), 0).empty() && listed(l, 10).size() == 1 && listed(l, 31).empty(), "an entry is kept for the seconds it asks");
        const auto forever = entry(5701, 65535);
        l.handle(forever.data(), forever.size(), 100);
        check(listed(l, 100 + 3599).size() == 1 && listed(l, 100 + 3601).empty(), "... an hour at most, whatever it asks");
    }
    {
        hk::py::Lobby l(true);
        for (int i = 0; i < 3000; i++) {
            const auto e = entry((uint16_t)(1000 + i), 65535);
            l.handle(e.data(), e.size(), 0);
        }
        const std::vector<uint16_t> ports = listed(l, 1);
        check(!ports.empty() && ports[0] == 1000 + 3000 - 1024, "3000 entries: the last 1024 are kept, the ones that would go first gone");
        const auto again = entry(3999, 30);   // the same cabinet again is still one entry
        l.handle(again.data(), again.size(), 2);
        check(listed(l, 3)[0] == 1000 + 3000 - 1024, "... a cabinet entering again takes no other's place");
    }
    printf(failed ? "%d failed\n" : "all hold\n", failed);
    return failed ? 1 : 0;
}
