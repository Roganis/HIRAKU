// SPDX-License-Identifier: GPL-3.0-or-later
// The MIDI file reader of the launcher's player (runtime/midi/smf.h), on files made here:
//   - tracks merged in time order, running status, a tempo change part-way, SysEx
//   - format 2 (one song after another), SMPTE time, a RIFF .rmi around a file
//   - files cut short or not MIDI at all: what came before the cut, or a reason
//   - starting part-way: the set-up kept (last controller of each, programs, RPNs), no notes
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "midi/smf.h"

using namespace hk::midi;
using Bytes = std::vector<uint8_t>;

static int g_failed = 0;
#define CHECK(c)                                                                      \
    do {                                                                              \
        if (!(c)) g_failed++, fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); \
    } while (0)

static bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

static void be(Bytes &o, uint32_t v, int n) {
    for (int k = n - 1; k >= 0; k--) o.push_back((uint8_t)(v >> (8 * k)));
}

static Bytes vlq(uint32_t v) {
    Bytes o{(uint8_t)(v & 0x7f)};
    while (v >>= 7) o.insert(o.begin(), (uint8_t)(0x80 | (v & 0x7f)));
    return o;
}

// a track from (delta, bytes) pairs, with its end of track
static Bytes mtrk(const std::vector<std::pair<uint32_t, Bytes>> &evs) {
    Bytes body;
    for (auto &[d, b] : evs) {
        Bytes v = vlq(d);
        body.insert(body.end(), v.begin(), v.end());
        body.insert(body.end(), b.begin(), b.end());
    }
    for (uint8_t b : {0x00, 0xff, 0x2f, 0x00}) body.push_back(b);
    Bytes o{'M', 'T', 'r', 'k'};
    be(o, (uint32_t)body.size(), 4);
    o.insert(o.end(), body.begin(), body.end());
    return o;
}

static Bytes smf(uint16_t format, uint16_t division, const std::vector<Bytes> &tracks) {
    Bytes o{'M', 'T', 'h', 'd', 0, 0, 0, 6};
    be(o, format, 2), be(o, (uint32_t)tracks.size(), 2), be(o, division, 2);
    for (auto &t : tracks) o.insert(o.end(), t.begin(), t.end());
    return o;
}

static bool read(const Bytes &b, Song &s) {
    std::string err;
    return parse(b.data(), b.size(), s, err);
}

int main() {
    // format 1, 96 ticks a quarter: a conductor track (name, tempo 120 then 60 at tick 192) and a
    // piano track with running status and a SysEx
    const Bytes conductor = mtrk({{0, {0xff, 0x03, 4, 'S', 'o', 'n', 'g'}}, {192, {0xff, 0x51, 3, 0x0f, 0x42, 0x40}}});
    const Bytes piano = mtrk({{0, {0xf0, 0x05, 0x7e, 0x7f, 0x09, 0x01, 0xf7}},
                              {0, {0xc0, 5}},
                              {0, {0x90, 60, 100}},
                              {96, {62, 100}},          // running status: a second note on
                              {96, {0x80, 60, 0}},       // tick 192: 1.0 s
                              {96, {62, 0}},             // tick 288: past the tempo change, 1.0 + 1.0 s
                              {0, {0xb0, 7, 80}}});
    Song s;
    CHECK(read(smf(1, 96, {conductor, piano}), s));
    CHECK(s.title == "Song");
    CHECK(s.events.size() == 7);
    if (s.events.size() == 7) {
        CHECK((s.events[0].bytes == Bytes{0xf0, 0x7e, 0x7f, 0x09, 0x01, 0xf7}));
        CHECK((s.events[1].bytes == Bytes{0xc0, 5}));
        CHECK((s.events[3].bytes == Bytes{0x90, 62, 100}) && near(s.events[3].time, 0.5));
        CHECK((s.events[4].bytes == Bytes{0x80, 60, 0}) && near(s.events[4].time, 1.0));
        CHECK((s.events[5].bytes == Bytes{0x80, 62, 0}) && near(s.events[5].time, 2.0));
        CHECK((s.events[6].bytes == Bytes{0xb0, 7, 80}) && near(s.events[6].time, 2.0));
    }
    CHECK(near(s.length, 2.0));

    // the same tick in two tracks: the first track's first
    {
        Song m;
        CHECK(read(smf(1, 96, {mtrk({{10, {0x91, 1, 1}}}), mtrk({{10, {0x92, 2, 2}}})}), m));
        CHECK(m.events.size() == 2 && m.events[0].bytes[0] == 0x91 && m.events[1].bytes[0] == 0x92);
    }
    // format 2: the second track after the first
    {
        Song m;
        CHECK(read(smf(2, 96, {mtrk({{96, {0x90, 1, 1}}}), mtrk({{96, {0x90, 2, 2}}})}), m));
        CHECK(m.events.size() == 2 && near(m.events[1].time, 1.0));
    }
    // SMPTE: 25 frames of 40 ticks, 1000 ticks a second
    {
        Song m;
        CHECK(read(smf(0, (uint16_t)((uint8_t)-25 << 8 | 40), {mtrk({{500, {0x90, 1, 1}}})}), m));
        CHECK(m.events.size() == 1 && near(m.events[0].time, 0.5));
    }
    // a RIFF .rmi around the file
    {
        const Bytes inner = smf(0, 96, {mtrk({{96, {0x90, 1, 1}}})});
        Bytes r{'R', 'I', 'F', 'F', 0, 0, 0, 0, 'R', 'M', 'I', 'D', 'd', 'a', 't', 'a'};
        for (int k = 0; k < 4; k++) r.push_back((uint8_t)(inner.size() >> (8 * k)));
        r.insert(r.end(), inner.begin(), inner.end());
        Song m;
        CHECK(read(r, m));
        CHECK(m.events.size() == 1 && near(m.events[0].time, 0.5));
    }
    // cut short in the middle of a message: what came before
    {
        Bytes f = smf(1, 96, {conductor, piano});
        f.resize(f.size() - 9);
        Song m;
        CHECK(read(f, m));
        CHECK(m.events.size() == 5);   // (the last note off, cut in two, and what follows it: gone)
    }
    // not MIDI; a format that doesn't exist; no ticks per anything
    {
        Song m;
        std::string err;
        const Bytes junk{'R', 'I', 'F', 'F', 1, 2, 3, 4, 'W', 'A', 'V', 'E'};
        CHECK(!parse(junk.data(), junk.size(), m, err) && !err.empty());
        CHECK(!read(smf(3, 96, {}), m));
        CHECK(!read(smf(0, 0, {mtrk({})}), m));
        CHECK(!parse(nullptr, 0, m, err));
    }
    // a non-UTF-8 name (Shift-JIS) is left out
    {
        Song m;
        CHECK(read(smf(0, 96, {mtrk({{0, {0xff, 0x03, 2, 0x82, 0xa0}}})}), m));
        CHECK(m.title.empty());
    }

    // starting part-way: a fade (CC 7 four times), an RPN (pitch bend range), program, bend, notes
    {
        Song m;
        CHECK(read(smf(0, 96, {mtrk({{0, {0xb0, 101, 0}},
                                     {0, {0xb0, 100, 0}},
                                     {0, {0xb0, 6, 12}},
                                     {0, {0xc0, 40}},
                                     {0, {0x90, 60, 90}},
                                     {10, {0xb0, 7, 100}},
                                     {10, {0xb0, 7, 90}},
                                     {10, {0xe0, 0, 70}},
                                     {10, {0xb0, 7, 80}},
                                     {10, {0xb0, 123, 0}},
                                     {10, {0xc0, 41}},
                                     {96, {0x80, 60, 0}},
                                     {0, {0xb0, 7, 10}}})}),
                   m));
        size_t first = 0;
        auto set = setup_before(m, 0.7, first);   // (tick 60 is 0.3125 s, tick 156 0.8125 s)
        CHECK(first == 11 && m.events.size() == 13);
        const std::vector<Bytes> want{{0xb0, 101, 0}, {0xb0, 100, 0}, {0xb0, 6, 12}, {0xe0, 0, 70}, {0xb0, 7, 80}, {0xc0, 41}};
        CHECK(set == want);
        auto none = setup_before(m, 0.0, first);
        CHECK(none.empty() && first == 0);
        auto all = setup_before(m, 99.0, first);
        CHECK(first == 13 && all.back() == (Bytes{0xb0, 7, 10}));
    }

    if (g_failed) fprintf(stderr, "test_smf: %d failed\n", g_failed);
    else printf("test_smf: all passed\n");
    return g_failed ? 1 : 0;
}
