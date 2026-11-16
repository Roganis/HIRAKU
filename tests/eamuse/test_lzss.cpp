// SPDX-License-Identifier: GPL-3.0-or-later
// The server's LZSS packer (runtime/eamuse/server.cpp, a player's record to a game): the same
// bytes as the plain search it was (every place of the window tried in full), on records like a
// player's (mostly zeros), on text, on noise and on short ends; and what it packs unpacks to what
// went in. How long each takes is printed.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>

#include "eamuse/server_parts.h"

namespace {

// the packer as it was: no short cuts
std::string plain(const std::string &in) {
    const size_t n = in.size();
    std::string out;
    for (int k = 0; k < 4; k++) out += (char)(n >> (8 * k) & 255);
    size_t i = 0;
    while (i < n) {
        const size_t flags_at = out.size();
        out += '\0';
        uint8_t flags = 0;
        for (int bit = 0; bit < 8 && i < n; bit++) {
            size_t best = 0, from = 0;
            for (size_t q = i > 4078 ? i - 4078 : 0; q < i; q++) {
                size_t len = 0;
                while (len < 18 && i + len < n && in[q + len] == in[i + len]) len++;
                if (len > best) best = len, from = q;
            }
            if (best >= 3) {
                const unsigned at = (unsigned)(0xfee + from) & 0xfff;
                out += (char)(at & 255);
                out += (char)(((at >> 4) & 0xf0) | (best - 3));
                i += best;
            } else {
                flags |= (uint8_t)(1 << bit);
                out += in[i++];
            }
        }
        out[flags_at] = (char)flags;
    }
    return out;
}

// as a game unpacks it: a 4 KiB window that starts at 0xfee
std::string unpack(const std::string &in) {
    if (in.size() < 4) return {};
    size_t n = 0;
    for (int k = 0; k < 4; k++) n |= (size_t)(uint8_t)in[(size_t)k] << (8 * k);
    std::string out;
    uint8_t window[4096] = {};
    unsigned w = 0xfee;
    size_t i = 4;
    while (out.size() < n && i < in.size()) {
        const uint8_t flags = (uint8_t)in[i++];
        for (int bit = 0; bit < 8 && out.size() < n && i < in.size(); bit++) {
            if (flags >> bit & 1) {
                out += in[i], window[w] = (uint8_t)in[i++], w = (w + 1) & 0xfff;
                continue;
            }
            if (i + 1 >= in.size()) return out;
            unsigned at = (uint8_t)in[i] | ((uint8_t)in[i + 1] & 0xf0u) << 4;
            const int len = ((uint8_t)in[i + 1] & 15) + 3;
            i += 2;
            for (int k = 0; k < len && out.size() < n; k++, at = (at + 1) & 0xfff) {
                const uint8_t b = window[at];
                out += (char)b, window[w] = b, w = (w + 1) & 0xfff;
            }
        }
    }
    return out;
}

double ms(const std::chrono::steady_clock::time_point &a) { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count(); }

}  // namespace

int main() {
    std::mt19937 rng(573);
    int failed = 0, tried = 0;
    double fast_ms = 0, plain_ms = 0, rec_fast = 0, rec_plain = 0;
    const auto one = [&](const std::string &in, const char *what) {
        auto t = std::chrono::steady_clock::now();
        const std::string a = hk::ea::lzss(in);
        const double fast = ms(t);
        t = std::chrono::steady_clock::now();
        const std::string b = plain(in);
        const double slow = ms(t);
        fast_ms += fast, plain_ms += slow;
        if (what[2] == 'r') rec_fast += fast, rec_plain += slow;   // "a record"
        tried++;
        if (a != b || unpack(a) != in) failed++, fprintf(stderr, "FAIL %s, %zu bytes: %s\n", what, in.size(), a != b ? "not the plain search's bytes" : "doesn't unpack to what went in");
    };
    one("", "nothing");
    for (size_t n = 1; n <= 40; n++) one(std::string(n, 'a'), "a run"), one(std::string(n, '\0'), "zeros");
    for (int k = 0; k < 60; k++) {
        // a player's record: mostly zeros, some fields set
        std::string rec(k % 3 == 0 ? 0xb38 : k % 3 == 1 ? 0xf38 : 0x2998, '\0');
        for (int f = 0; f < 40 + k * 8; f++) rec[rng() % rec.size()] = (char)(rng() % 7 ? rng() & 0xff : 0xff);
        for (int f = 0; f < 6; f++) {   // (and stretches of something that repeats: a name, a table)
            const size_t at = rng() % (rec.size() - 64);
            for (size_t j = 0; j < 48; j++) rec[at + j] = (char)("POPN MUSIC "[j % 11]);
        }
        one(rec, "a record");
    }
    for (int k = 0; k < 40; k++) {
        std::string noise(1 + rng() % 9000, '\0'), few(1 + rng() % 9000, '\0'), text;
        for (char &c : noise) c = (char)rng();
        for (char &c : few) c = (char)('a' + rng() % 3);   // (long matches everywhere, many as long)
        while (text.size() < 3000 + rng() % 4000) text += "the quick brown fox " + std::to_string(rng() % 50) + " jumps over ";
        one(noise, "noise"), one(few, "three letters"), one(text, "text");
    }
    printf("%d inputs: %.1f ms, the plain search %.1f ms\n", tried, fast_ms, plain_ms);
    printf("the 60 records among them: %.2f ms, the plain search %.1f ms (%.0f times as long)\n", rec_fast, rec_plain, rec_plain / (rec_fast > 0 ? rec_fast : 1));
    printf(failed ? "%d failed\n" : "ok\n", failed);
    return failed ? 1 : 0;
}
