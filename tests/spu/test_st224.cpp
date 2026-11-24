// SPDX-License-Identifier: GPL-3.0-or-later
// beatmania III's effects unit stand-in (runtime/spu/st224.h), with made-up byte streams shaped like
// what the games send:
//   - the MIDI reading: the start-up stream, running status, real-time bytes in between, exclusive
//     messages skipped, the program's numbers and names
//   - bypass: before anything is sent, with the effect off, and once an effect turned off has faded,
//     the sound is left exactly as it is
//   - an impulse through DELAY comes back at the time PARAM A sets (no feedback: once)
//   - the same sound in pieces of any size comes out the same, to the bit
//   - every program makes something finite, not silent, not the sound unchanged
//   - LOW PASS takes the top off; turning an effect on and off makes no click
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "spu/st224.h"

using namespace hk::spu;

namespace {
int g_fail = 0;
#define CHECK(c)                                                                \
    do {                                                                        \
        if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } \
    } while (0)

constexpr int RATE = 44100;

void send(St224 &u, std::initializer_list<int> bytes) {
    for (int b : bytes) u.midi((uint8_t)b);
}

// what the games send at start-up, then an effect chosen (REVERB) and left off
void boot(St224 &u) {
    send(u, {0xb0, 0x79, 0x00, 0xb0, 0x7b, 0x00, 0xb0, 0x78, 0x00, 0xa0, 0x6c, 0x7f, 0x90, 0x23, 0x7f});
    send(u, {0xc0, 0x0c, 0xb0, 0x0c, 0x28, 0xb0, 0x0d, 0x46, 0xa0, 0x6d, 0x37, 0xb0, 0x50, 0x00});
}
// an effect the way the game turns it on: the SOURCE pad through the effect and held (as at the
// game's start-up), program, PARAM A and B, level, on
void effect(St224 &u, int prog, int a, int b, int level = 70) {
    send(u, {0xa0, 0x6c, 0x7f, 0x90, 0x23, 0x7f});
    send(u, {0xc0, prog, 0xb0, 0x0c, a, 0xb0, 0x0d, b, 0xa0, 0x6d, level, 0xb0, 0x50, 0x7f});
}

std::vector<float> noise(int n, uint32_t seed = 1) {
    std::vector<float> v(2 * (size_t)n);
    for (float &x : v) {
        seed = seed * 1664525u + 1013904223u;
        x = ((float)(seed >> 8) / 16777216.0f - 0.5f) * 0.4f;
    }
    return v;
}
std::vector<float> sine(int n, float hz, float amp) {
    std::vector<float> v(2 * (size_t)n);
    for (int i = 0; i < n; i++) v[2 * i] = v[2 * i + 1] = amp * std::sin(2 * 3.14159265f * hz * i / RATE);
    return v;
}
double rms(const std::vector<float> &v, size_t from = 0) {
    double s = 0;
    for (size_t i = from; i < v.size(); i++) s += (double)v[i] * v[i];
    return std::sqrt(s / (double)(v.size() - from));
}

void reading() {
    St224 u;
    boot(u);
    CHECK(u.state().program == 12 && u.state().a == 40 && u.state().b == 70 && u.state().level == 55);
    CHECK(u.state().pad_effect && u.state().source && !u.state().on);
    CHECK(u.state().unknown == 0);
    CHECK(!u.active());
    // running status, with a timing clock and an active sense in the middle of a message
    send(u, {0xb0, 0x0c, 0xf8, 0x11, 0x0d, 0xfe, 0x22});
    CHECK(u.state().a == 0x11 && u.state().b == 0x22);
    // an exclusive message's data is not taken for running status
    send(u, {0xf0, 0x0c, 0x33, 0x0d, 0x44, 0xf7, 0x0c, 0x55});
    CHECK(u.state().a == 0x11 && u.state().b == 0x22);
    send(u, {0xb0, 0x50, 0x7f});
    CHECK(u.state().on && u.active());
    send(u, {0xb0, 0x50, 0x00});
    CHECK(!u.state().on);
    // the SOURCE pad let go (all notes off), or not through the effect: nothing heard, nothing changed
    send(u, {0xb0, 0x50, 0x7f, 0xb0, 0x7b, 0x00});
    CHECK(!u.state().source && !u.active());
    send(u, {0x90, 0x23, 0x7f, 0xa0, 0x6c, 0x00});
    CHECK(u.state().source && !u.state().pad_effect && !u.active());
    send(u, {0xa0, 0x6c, 0x7f});
    CHECK(u.active());
    send(u, {0x80, 0x23, 0x40});
    CHECK(!u.active());
    send(u, {0x90, 0x23, 0x7f, 0x90, 0x23, 0x00});   // (a note-on of velocity 0 lets go too)
    CHECK(!u.active());
    // values past the unit's ranges, its tempo and sync, then reset all controllers: the manual's values
    send(u, {0x90, 0x23, 0x7f, 0xa0, 0x6d, 0x7f, 0xb0, 0x0e, 0x50, 0xb0, 0x51, 0x7f});
    CHECK(u.state().level == 99 && u.state().bpm == 120 && u.state().sync);
    send(u, {0xb0, 0x79, 0x00});
    CHECK(u.state().a == 0 && u.state().b == 0 && !u.state().on && !u.state().pad_effect && u.state().level == 49 &&
          u.state().bpm == 120 && !u.state().sync && u.state().source);
    send(u, {0xb0, 0x0c, 0x28, 0xb0, 0x0d, 0x46, 0xa0, 0x6d, 0x37, 0xa0, 0x6c, 0x7f, 0xb0, 0x50, 0x00});
    // the game's 19 effects, by the unit's numbers
    const int progs[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 13, 14, 15, 16, 17, 18, 20, 21};
    for (int p : progs) CHECK(St224::known(p) && St224::program_name(p)[0]);
    CHECK(!St224::known(0) && !St224::known(11) && !St224::known(19) && !St224::known(22));
    CHECK(std::string(St224::program_name(12)) == "REVERB" && std::string(St224::program_name(1)) == "DELAY");
}

void bypass() {
    const auto in = noise(RATE);
    {   // nothing sent
        St224 u;
        auto v = in;
        u.process(v.data(), RATE);
        CHECK(v == in);
    }
    {   // the start-up, the effect off; an unknown program on
        St224 u;
        boot(u);
        auto v = in;
        u.process(v.data(), RATE);
        CHECK(v == in);
        send(u, {0xc0, 11, 0xb0, 0x50, 0x7f});
        CHECK(!u.active());
        u.process(v.data(), RATE);
        CHECK(v == in);
    }
    {   // on for half a second, off: after the fade (10 ms), exactly the sound again
        St224 u;
        boot(u);
        effect(u, 12, 40, 70);
        auto v = in;
        u.process(v.data(), RATE / 2);
        CHECK(v != in);
        send(u, {0xb0, 0x50, 0x00});
        u.process(v.data() + RATE, 1000);
        CHECK(!u.active());
        u.process(v.data() + RATE + 2000, RATE / 2 - 1000);
        for (size_t i = (size_t)RATE + 2000; i < v.size(); i++)
            if (v[i] != in[i]) { CHECK(v[i] == in[i]); break; }
    }
}

void delay_echo() {
    St224 u;
    effect(u, 1, 50, 0);   // DELAY, A 50: half the manual's 505 ms; B 0: one repeat, quiet; level 70 = unity
    std::vector<float> v(2 * (size_t)RATE * 2, 0.0f);
    v[2 * 1000] = v[2 * 1000 + 1] = 1.0f;   // an impulse, after the fade in
    u.process(v.data(), RATE * 2);
    int peak = 0;
    for (int i = 2000; i < RATE * 2; i++)
        if (std::fabs(v[2 * i]) > std::fabs(v[2 * peak])) peak = i;
    const double expect = 1000 + 0.2525 * RATE;
    CHECK(std::fabs(peak - expect) <= 1.5);
    CHECK(std::fabs(v[2 * 1000] - 1.0f) < 1e-6f);   // the sound itself goes through
    // and nothing else: one echo only
    double rest = 0;
    for (int i = 1001; i < RATE * 2; i++)
        if (std::abs(i - peak) > 2) rest = std::max(rest, (double)std::fabs(v[2 * i]));
    CHECK(rest < 1e-6);
    // with feedback: a second echo at twice the time
    St224 w;
    effect(w, 1, 50, 50);
    std::vector<float> z(2 * (size_t)RATE * 2, 0.0f);
    z[2 * 1000] = 1.0f;
    w.process(z.data(), RATE * 2);
    double second = 0;
    for (int i = (int)(1000 + 2 * 0.2525 * RATE) - 3; i <= (int)(1000 + 2 * 0.2525 * RATE) + 3; i++) second = std::max(second, (double)std::fabs(z[2 * i]));
    CHECK(second > 0.01);
    // A at 100: the manual's longest, 505 ms
    St224 l;
    effect(l, 1, 100, 0);
    std::vector<float> lv(2 * (size_t)RATE, 0.0f);
    lv[2 * 1000] = 1.0f;
    l.process(lv.data(), RATE);
    int far = 2000;
    for (int i = 2000; i < RATE; i++)
        if (std::fabs(lv[2 * i]) > std::fabs(lv[2 * far])) far = i;
    CHECK(std::fabs(far - (1000 + 0.505 * RATE)) <= 1.5);
}

void pieces() {
    const auto in = noise(RATE / 2, 7);
    const int progs[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 13, 14, 15, 16, 17, 18, 20, 21};
    const int at = 11111;   // a PARAM moves here (not at a block's start)
    for (int p : progs) {
        St224 a, b;
        effect(a, p, 60, 60), effect(b, p, 60, 60);
        auto x = in, y = in;
        a.process(x.data(), at);   // in two
        send(a, {0xb0, 0x0c, 0x10});
        a.process(x.data() + 2 * at, RATE / 2 - at);
        uint32_t s = 99;
        for (int i = 0; i < RATE / 2;) {   // in pieces of 1 to 97 samples
            s = s * 1664525u + 1013904223u;
            int n = std::min(RATE / 2 - i, (int)(s >> 24) % 97 + 1);
            if (i < at && i + n > at) n = at - i;
            if (i == at) send(b, {0xb0, 0x0c, 0x10});
            b.process(y.data() + 2 * i, n);
            i += n;
        }
        if (x != y) fprintf(stderr, "program %d: in pieces, not the same\n", p);
        CHECK(x == y);
    }
}

void every_program() {
    const auto in = noise(RATE, 3);
    const double r0 = rms(in);
    const int progs[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 13, 14, 15, 16, 17, 18, 20, 21};
    for (int p : progs) {
        St224 u;
        effect(u, p, 70, 70);
        auto v = in;
        u.process(v.data(), RATE);
        bool finite = true;
        for (float x : v) finite &= std::isfinite(x);
        const double r = rms(v, 2 * 4410);
        double diff = 0;
        for (size_t i = 2 * 4410; i < v.size(); i++) diff += std::fabs(v[i] - in[i]);
        if (!finite || r < 0.05 * r0 || r > 10 * r0 || diff < 1)
            fprintf(stderr, "program %d (%s): rms %.4f of %.4f, difference %.2f\n", p, St224::program_name(p), r, r0, diff);
        CHECK(finite && r > 0.05 * r0 && r < 10 * r0 && diff >= 1);
    }
}

// a tone's level after a program (from 0.2 s on, past the fade)
double through(int prog, int a, int b, float hz, float amp = 0.3f, int level = 70) {
    St224 u;
    effect(u, prog, a, b, level);
    auto v = sine(RATE / 2, hz, amp);
    u.process(v.data(), RATE / 2);
    return rms(v, 2 * 8820) / rms(sine(RATE / 2, hz, amp), 2 * 8820);
}

// what the parameters do, as the unit's manual has it
void meanings() {
    // EXTREAM EQ: A 50 leaves the sound; forward takes the chosen band away, back everything but it;
    // B chooses LOW, MID or HIGH
    CHECK(std::fabs(through(7, 50, 10, 100) - 1) < 0.02 && std::fabs(through(7, 50, 10, 8000) - 1) < 0.02);
    CHECK(through(7, 100, 10, 80) < 0.2 && through(7, 100, 10, 8000) > 0.9);   // LOW gone, HIGH kept
    CHECK(through(7, 0, 10, 80) > 0.8 && through(7, 0, 10, 8000) < 0.05);      // all but LOW gone (the game's start)
    CHECK(through(7, 100, 90, 8000) < 0.1 && through(7, 100, 90, 80) > 0.9);   // HIGH gone
    // VOLUME BASS: A 50 0 dB, 100 +6 dB; B boosts the bass from none
    CHECK(std::fabs(through(10, 50, 0, 1000) - 1) < 0.01 && std::fabs(through(10, 100, 0, 1000) - 2.0) < 0.03);
    CHECK(through(10, 50, 100, 60) > 3 && std::fabs(through(10, 50, 100, 2000) - 1) < 0.1);
    // LOW PASS at the game's start (80, 60): the top taken off, no peak to speak of below it
    CHECK(through(6, 80, 60, 12000) < 0.2 && through(6, 80, 60, 3000) < 1.3 && through(6, 80, 60, 500) > 0.9);
    // RESONANCE: the filter opens with the sound's level (B, sensitivity, at the game's 60)
    const double quiet = through(18, 50, 60, 2000, 0.01f), loud = through(18, 50, 60, 2000, 0.4f);
    CHECK(loud > 3 * quiet);
    // PITCH SHIFTER: A 75 is +6 semitones (half an octave); B the tone
    {
        St224 u;
        effect(u, 4, 75, 100);
        auto v = sine(RATE, 440, 0.3f);
        u.process(v.data(), RATE);
        int crossings = 0;
        for (int i = RATE / 2; i < RATE - 1; i++) crossings += (v[2 * i] < 0) != (v[2 * i + 2] < 0);
        const double hz = crossings / 2.0 / 0.5;
        CHECK(std::fabs(hz - 440 * std::sqrt(2.0)) < 15);
    }
    // DISTORTION is mono
    {
        St224 u;
        effect(u, 5, 30, 90);
        auto v = noise(RATE / 2, 5);
        u.process(v.data(), RATE / 2);
        bool same = true;
        for (int i = 4410; i < RATE / 2; i++) same &= std::fabs(v[2 * i] - v[2 * i + 1]) < 1e-6f;
        CHECK(same);
    }
}

void low_pass_and_clicks() {
    St224 u;
    effect(u, 6, 30, 10);   // LOW PASS at A 30 (about 480 Hz here)
    auto hi = sine(RATE / 2, 8000, 0.5f);
    const double r0 = rms(hi, 2 * 4410);
    u.process(hi.data(), RATE / 2);
    CHECK(rms(hi, 2 * 4410) < 0.01 * r0);
    // a 100 Hz tone; effects turned on and off along it: no step bigger than the tone's own
    for (int p : {6, 12, 1, 4, 9, 7, 17}) {
        St224 w;
        boot(w);
        auto v = sine(RATE, 100, 0.5f);
        const float own = 0.5f * 2 * 3.14159265f * 100 / RATE;   // the tone's biggest step
        for (int i = 0; i < RATE; i += 1000) {
            if (i == 10000) effect(w, p, 60, 60);
            if (i == 30000) send(w, {0xb0, 0x50, 0x00});
            w.process(v.data() + 2 * i, std::min(1000, RATE - i));
        }
        float step = 0;
        for (int i = 9000; i < 31000; i++) step = std::max(step, std::fabs(v[2 * i] - v[2 * i - 2]));
        if (step > 6 * own) fprintf(stderr, "program %d: a step of %.4f (the tone's own %.4f)\n", p, step, own);
        CHECK(step <= 6 * own);
    }
}
}  // namespace

int main() {
    reading();
    bypass();
    delay_echo();
    pieces();
    every_program();
    low_pass_and_clicks();
    meanings();
    if (g_fail) {
        fprintf(stderr, "%d failed\n", g_fail);
        return 1;
    }
    printf("st224: all passed\n");
    return 0;
}
