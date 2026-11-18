// SPDX-License-Identifier: GPL-3.0-or-later
// SHA-1 (FIPS 180-4), for telling programs apart: the hash games/<id>/game.toml's [program] sha1
// gives and tools/ingest.py checks (Python's hashlib), here for a mod's `program` (docs/MODS.md).
// Not for anything that has to resist someone making a collision on purpose.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace hk {

class Sha1 {
public:
    void add(const void *data, size_t n) {
        const uint8_t *p = static_cast<const uint8_t *>(data);
        total_ += n;
        while (n) {
            const size_t k = std::min(n, sizeof buf_ - used_);
            memcpy(buf_ + used_, p, k);
            used_ += k, p += k, n -= k;
            if (used_ == sizeof buf_) block(buf_), used_ = 0;
        }
    }
    // the 40 hex digits, lower case (the hasher is spent)
    std::string hex() {
        const uint64_t bits = total_ * 8;
        const uint8_t one = 0x80, zero = 0;
        add(&one, 1);
        while (used_ != 56) add(&zero, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
        add(len, 8);
        char out[41];
        for (int i = 0; i < 5; i++) snprintf(out + 8 * i, 9, "%08x", h_[i]);
        return std::string(out, 40);
    }

private:
    static uint32_t rol(uint32_t v, int s) { return v << s | v >> (32 - s); }
    void block(const uint8_t *b) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) w[i] = (uint32_t)b[4 * i] << 24 | (uint32_t)b[4 * i + 1] << 16 | (uint32_t)b[4 * i + 2] << 8 | b[4 * i + 3];
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h_[0], bb = h_[1], c = h_[2], d = h_[3], e = h_[4];
        for (int i = 0; i < 80; i++) {
            const uint32_t f = i < 20 ? (bb & c) | (~bb & d) : i < 40 ? bb ^ c ^ d : i < 60 ? (bb & c) | (bb & d) | (c & d) : bb ^ c ^ d;
            const uint32_t k = i < 20 ? 0x5a827999u : i < 40 ? 0x6ed9eba1u : i < 60 ? 0x8f1bbcdcu : 0xca62c1d6u;
            const uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d, d = c, c = rol(bb, 30), bb = a, a = t;
        }
        h_[0] += a, h_[1] += bb, h_[2] += c, h_[3] += d, h_[4] += e;
    }
    uint32_t h_[5] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u, 0xc3d2e1f0u};
    uint8_t buf_[64];
    size_t used_ = 0;
    uint64_t total_ = 0;
};

}  // namespace hk
