// SPDX-License-Identifier: GPL-3.0-or-later
// The lobby pop'n music 12-14's NET TAISEN finds its opponents through (lobby.h).
#include "python/lobby.h"

#include <algorithm>
#include <cstring>

namespace hk::py {

namespace {

constexpr size_t HEADER = 12;
constexpr size_t ENTRY = 0x1c;
constexpr size_t MAX_ANSWER = 0x1bf0;   // the game's buffer (0x199aa8)
// What anyone who reaches the lobby can make it hold: an entry asks to be kept up to 65535 s, and
// each address and port is one. An hour at most (a cabinet enters again each time it waits), and
// 1024 entries (an answer lists 270 at most)
constexpr int MAX_SECONDS = 3600;
constexpr size_t MAX_ENTRIES = 1024;

uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

// CRC-16/X-25 (0x1adfd0): reflected 0x1021, starting at and finishing with ffff
uint16_t crc16(const uint8_t *p, size_t n) {
    uint32_t c = 0xffff;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int b = 0; b < 8; b++) c = c & 1 ? c >> 1 ^ 0x8408 : c >> 1;
    }
    return (uint16_t)~c;
}

}  // namespace

std::vector<uint8_t> Lobby::handle(const uint8_t *d, size_t n, double now) {
    if (n < HEADER || std::memcmp(d, "lob0", 4) != 0 || be16(d + 4) != n) return {};
    std::vector<uint8_t> req(d, d + n);
    req[6] = req[7] = 0;
    if (crc16(req.data(), n) != be16(d + 6)) return {};
    const uint8_t kind = d[10];
    const uint8_t *body = d + HEADER;

    std::erase_if(entries_, [&](const Entry &e) { return e.until <= now; });
    auto same = [](const Entry &e, const uint8_t *addr, uint16_t port) { return std::memcmp(e.addr, addr, 4) == 0 && e.port == port; };

    std::vector<uint8_t> out(HEADER, 0);
    auto enter = [&] {   // an entry, kept for its seconds, again on each entry
        Entry e;
        e.rooms = be32(body);
        std::memcpy(e.addr, body + 4, 4);
        e.port = be16(body + 8);
        e.until = now + std::min<int>(be16(body + 10), MAX_SECONDS);
        std::memcpy(e.own, body + 12, 16);
        std::erase_if(entries_, [&](const Entry &o) { return same(o, e.addr, e.port); });
        // (no more than MAX_ENTRIES at once: the one that would go first goes now)
        if (entries_.size() >= MAX_ENTRIES)
            entries_.erase(std::min_element(entries_.begin(), entries_.end(), [](const Entry &a, const Entry &b) { return a.until < b.until; }));
        entries_.push_back(e);
    };
    // the cabinets in a room of the mask, then a zero entry (pop'n 15: counted in the header's byte
    // 11 instead), but `except`
    auto list = [&](uint32_t rooms, const uint8_t *except) {
        for (const Entry &e : entries_) {
            if (!(e.rooms & rooms) || (except && same(e, except, be16(except + 4)))) continue;
            if (out.size() + 2 * ENTRY > MAX_ANSWER) break;
            uint8_t r[ENTRY] = {};
            r[0] = (uint8_t)(e.rooms >> 24); r[1] = (uint8_t)(e.rooms >> 16); r[2] = (uint8_t)(e.rooms >> 8); r[3] = (uint8_t)e.rooms;
            std::memcpy(r + 4, e.addr, 4);
            put16(r + 8, e.port);
            std::memcpy(r + 10, e.own, 16);
            out.insert(out.end(), r, r + ENTRY);
            if (pc_) out[11]++;
        }
        if (!pc_) out.resize(out.size() + ENTRY, 0);
    };
    if (kind == 0 && n >= 0x28) {   // entry
        enter();
    } else if (kind == 1 && n >= 0x10) {   // list
        list(be32(body), nullptr);
    } else if (kind == 2 && n >= 0x14) {   // leave
        std::erase_if(entries_, [&](const Entry &e) { return same(e, body, be16(body + 4)); });
    } else if (pc_ && kind == 3 && n >= 0x28) {   // pop'n 15's participate: an entry, and the others there
        enter();
        list(be32(body), body + 4);
    } else {
        return {};
    }
    std::memcpy(out.data(), "lob0", 4);
    put16(out.data() + 4, (uint16_t)out.size());
    out[8] = d[8];   // the sequence number; kind 0: done
    out[9] = d[9];
    put16(out.data() + 6, crc16(out.data(), out.size()));
    return out;
}

}  // namespace hk::py
