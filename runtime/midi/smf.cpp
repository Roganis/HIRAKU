// SPDX-License-Identifier: GPL-3.0-or-later
#include "midi/smf.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace hk::midi {

namespace {

constexpr size_t kMaxEvents = 8u << 20;   // a file that says more is broken, or out to fill the memory

// One message or tempo change as a track gives it, in ticks
struct Raw {
    uint64_t tick;
    uint32_t track, seq;
    enum Kind : uint8_t { Message, Tempo, End } kind;
    uint32_t tempo;   // microseconds per quarter note
    std::vector<uint8_t> bytes;
};

uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
uint32_t le32(const uint8_t *p) { return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]; }

// a variable-length number (at most four bytes); false at the end of the data
bool vlq(const uint8_t *d, size_t end, size_t &pos, uint32_t &v) {
    v = 0;
    for (int k = 0; k < 4; k++) {
        if (pos >= end) return false;
        const uint8_t b = d[pos++];
        v = v << 7 | (b & 0x7f);
        if (!(b & 0x80)) return true;
    }
    return false;
}

int data_bytes(uint8_t status) { return (status & 0xf0) == 0xc0 || (status & 0xf0) == 0xd0 ? 1 : 2; }

bool utf8(const std::string &s) {
    for (size_t i = 0; i < s.size();) {
        const uint8_t c = (uint8_t)s[i];
        int n = c < 0x80 ? 0 : (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : -1;
        if (n < 0 || i + n >= s.size()) return false;
        for (int k = 1; k <= n; k++)
            if (((uint8_t)s[i + k] & 0xc0) != 0x80) return false;
        i += n + 1;
    }
    return true;
}

// one MTrk chunk's events into raws, from tick `base`; its end's tick
uint64_t track(const uint8_t *d, size_t end, uint32_t index, uint64_t base, std::vector<Raw> &raws, std::string &title) {
    size_t pos = 0;
    uint64_t tick = base;
    uint8_t running = 0;
    uint32_t seq = 0;
    bool named = false;
    while (pos < end && raws.size() < kMaxEvents) {
        uint32_t delta;
        if (!vlq(d, end, pos, delta) || pos >= end) break;
        tick += delta;
        uint8_t status = d[pos];
        if (status & 0x80) pos++;
        else if (running) status = running;
        else break;   // data with no status before it: the rest can't be read
        if (status < 0xf0) {
            const int n = data_bytes(status);
            if (pos + n > end) break;
            Raw r{tick, index, seq++, Raw::Message, 0, {status}};
            bool bad = false;
            for (int k = 0; k < n; k++) bad |= (d[pos + k] & 0x80) != 0, r.bytes.push_back(d[pos + k]);
            if (bad) break;
            pos += n;
            running = status;
            raws.push_back(std::move(r));
        } else if (status == 0xf0 || status == 0xf7) {   // SysEx, or a part of one / raw bytes ("escape")
            uint32_t len;
            if (!vlq(d, end, pos, len) || len > end - pos) break;
            Raw r{tick, index, seq++, Raw::Message, 0, {}};
            if (status == 0xf0) r.bytes.push_back(0xf0);
            r.bytes.insert(r.bytes.end(), d + pos, d + pos + len);
            pos += len;
            running = 0;
            if (!r.bytes.empty()) raws.push_back(std::move(r));
        } else if (status == 0xff) {   // meta event
            if (pos >= end) break;
            const uint8_t type = d[pos++];
            uint32_t len;
            if (!vlq(d, end, pos, len) || len > end - pos) break;
            if (type == 0x51 && len == 3) raws.push_back({tick, index, seq++, Raw::Tempo, (uint32_t)d[pos] << 16 | d[pos + 1] << 8 | d[pos + 2], {}});
            if (type == 0x03 && index == 0 && !named) title.assign((const char *)d + pos, len), named = true;
            pos += len;
            if (type == 0x2f) break;
        } else {
            break;   // a system message that has no place in a file
        }
    }
    raws.push_back({tick, index, seq++, Raw::End, 0, {}});
    return tick;
}

}  // namespace

bool parse(const uint8_t *data, size_t size, Song &song, std::string &err) {
    song = Song{};
    // RIFF MIDI (.rmi): the file is its "data" chunk
    if (size >= 12 && std::equal(data, data + 4, "RIFF") && std::equal(data + 8, data + 12, "RMID")) {
        for (size_t p = 12; p + 8 <= size;) {
            const uint32_t len = le32(data + p + 4);
            if (std::equal(data + p, data + p + 4, "data")) return parse(data + p + 8, std::min<size_t>(len, size - p - 8), song, err);
            p += 8 + (size_t)len + (len & 1);
        }
        err = "a RIFF file with no MIDI in it";
        return false;
    }
    if (size < 14 || !std::equal(data, data + 4, "MThd") || be32(data + 4) < 6) {
        err = "not a MIDI file";
        return false;
    }
    const uint16_t format = be16(data + 8), tracks = be16(data + 10), division = be16(data + 12);
    if (division == 0 || format > 2) {
        err = "a MIDI file this player can't read (format " + std::to_string(format) + ")";
        return false;
    }
    std::vector<Raw> raws;
    uint64_t base = 0;
    size_t p = 8 + (size_t)be32(data + 4);
    for (uint32_t t = 0; t < tracks && p + 8 <= size;) {
        const size_t len = std::min<size_t>(be32(data + p + 4), size - p - 8);
        if (std::equal(data + p, data + p + 4, "MTrk")) {
            const uint64_t end = track(data + p + 8, len, t, base, raws, song.title);
            if (format == 2) base = end;   // one song after another
            t++;
        }
        p += 8 + len;
    }
    if (raws.empty()) {
        err = "a MIDI file with no tracks";
        return false;
    }
    std::stable_sort(raws.begin(), raws.end(), [](const Raw &a, const Raw &b) {
        return a.tick != b.tick ? a.tick < b.tick : a.track != b.track ? a.track < b.track : a.seq < b.seq;
    });
    // ticks into seconds: a tempo map (quarter notes), or SMPTE frames
    const bool smpte = division & 0x8000;
    const int fps = -(int8_t)(division >> 8);
    const double tick_smpte = smpte ? 1.0 / ((fps == 29 ? 29.97 : fps) * (division & 0xff)) : 0;
    double tempo = 500000, time = 0;
    uint64_t last = 0;
    for (Raw &r : raws) {
        time += (double)(r.tick - last) * (smpte ? tick_smpte : tempo / 1e6 / division);
        last = r.tick;
        song.length = std::max(song.length, time);
        if (r.kind == Raw::Tempo && r.tempo) tempo = r.tempo;
        if (r.kind == Raw::Message) song.events.push_back({time, std::move(r.bytes)});
    }
    if (!utf8(song.title)) song.title.clear();
    while (!song.title.empty() && (song.title.back() == ' ' || song.title.back() == '\0')) song.title.pop_back();
    return true;
}

bool load(const std::string &path, Song &song, std::string &err) {
    std::ifstream f(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary);
    if (!f) {
        err = "can't be opened";
        return false;
    }
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return parse(d.data(), d.size(), song, err);
}

std::vector<std::vector<uint8_t>> setup_before(const Song &song, double at, size_t &first) {
    first = 0;
    while (first < song.events.size() && song.events[first].time < at) first++;
    // the last of each kept-only-last message: controllers per channel, program, pressure, bend
    std::vector<long> last_cc(16 * 128, -1), last_other(16 * 3, -1);
    auto in_sequence = [](int cc) {   // bank select, data entry, NRPN and RPN, the channel modes: all of them, in order
        return cc == 0 || cc == 32 || cc == 6 || cc == 38 || (cc >= 96 && cc <= 101) || cc >= 121;
    };
    auto other = [](uint8_t type) { return type == 0xc0 ? 0 : type == 0xd0 ? 1 : 2; };
    for (size_t k = 0; k < first; k++) {
        const auto &b = song.events[k].bytes;
        if (b.empty() || b[0] >= 0xf0) continue;
        const uint8_t type = b[0] & 0xf0, ch = b[0] & 0x0f;
        if (type == 0xb0 && b.size() >= 2) last_cc[ch * 128 + b[1]] = (long)k;
        else if (type >= 0xc0) last_other[ch * 3 + other(type)] = (long)k;
    }
    std::vector<std::vector<uint8_t>> out;
    for (size_t k = 0; k < first; k++) {
        const auto &b = song.events[k].bytes;
        if (b.empty()) continue;
        if (b[0] >= 0xf0) {
            out.push_back(b);
            continue;
        }
        const uint8_t type = b[0] & 0xf0, ch = b[0] & 0x0f;
        if (type < 0xb0) continue;   // notes and their pressure
        if (type == 0xb0) {
            if (b.size() < 2 || b[1] == 120 || b[1] == 123) continue;   // sound and notes off: none playing yet
            if (in_sequence(b[1]) || last_cc[ch * 128 + b[1]] == (long)k) out.push_back(b);
        } else if (last_other[ch * 3 + other(type)] == (long)k) {
            out.push_back(b);
        }
    }
    return out;
}

}  // namespace hk::midi
