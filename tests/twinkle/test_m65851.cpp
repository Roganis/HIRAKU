// SPDX-License-Identifier: GPL-3.0-or-later
// The Twinkle sound board's effect chip stand-in (runtime/twinkle/m65851.h), with the words the
// games send (traced from bmiidx2, 3 and 8):
//   - the serial port: words clocked out on the pins as the 68000 does it (bit 15 first, the data
//     taken as the clock falls, the word as the latch rises), with the register's other writes
//     in between, read back; every traced word decoded; another chip's address ignored
//   - pass-through: before any word, after the reset words, and once "off" has faded, the sound is
//     left exactly as it is
//   - KEY: a 440 Hz sine comes out at 440 x 2^(key/12)
//   - the delay block: SRD's echo at 10 ms in the same phase, ECHO's at 50 ms in reverse phase,
//     at the delay volume, repeating at the feedback volume
//   - the tone: bass and treble at 100 Hz and 10 kHz
//   - the same sound in pieces of any size comes out the same, to the bit
//   - HK_M65851=0
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "twinkle/m65851.h"

using namespace hk::twinkle;

namespace {
int g_fail = 0;
#define CHECK(c)                                                                \
    do {                                                                        \
        if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } \
    } while (0)

constexpr int RATE = 44100;
constexpr double PI = 3.14159265358979323846;

void setenv_(const char *k, const char *v) {
#ifdef _WIN32
    _putenv_s(k, v ? v : "");
#else
    if (v) setenv(k, v, 1);
    else unsetenv(k);
#endif
}

void words(M65851 &c, std::initializer_list<uint16_t> ws) {
    for (uint16_t w : ws) CHECK(c.write(w));
}
// what the game sends at start-up and for "off"
void reset_words(M65851 &c) { words(c, {0xc101, 0x3d1d, 0x4051, 0x8001}); }

std::vector<float> noise(int n, uint32_t seed = 1) {
    std::vector<float> v(2 * (size_t)n);
    for (float &x : v) {
        seed = seed * 1664525u + 1013904223u;
        x = ((float)(seed >> 8) / 16777216.0f - 0.5f) * 0.4f;
    }
    return v;
}
std::vector<float> sine(int n, double hz, double amp) {
    std::vector<float> v(2 * (size_t)n);
    for (int i = 0; i < n; i++) v[2 * i] = v[2 * i + 1] = (float)(amp * std::sin(2 * PI * hz * i / RATE));
    return v;
}
double rms(const std::vector<float> &v, int ch, size_t from) {   // frames from `from`
    double s = 0;
    size_t n = 0;
    for (size_t i = from; 2 * i + ch < v.size(); i++, n++) s += (double)v[2 * i + ch] * v[2 * i + ch];
    return std::sqrt(s / (double)n);
}
// the strongest frequency of channel 0 from frame `from`, searched lo..hi in 0.25 Hz steps
double peak_hz(const std::vector<float> &v, size_t from, double lo, double hi) {
    double best = 0, at = 0;
    for (double f = lo; f <= hi; f += 0.25) {
        double re = 0, im = 0;
        for (size_t i = from; 2 * i < v.size(); i++) {
            const double w = 2 * PI * f * (double)(i - from) / RATE;
            re += v[2 * i] * std::cos(w), im += v[2 * i] * std::sin(w);
        }
        const double p = re * re + im * im;
        if (p > best) best = p, at = f;
    }
    return at;
}

// ---- the serial port
void serial() {
    // the program's way (the sender at 0x31b0): for each bit from 15 down, data with the clock
    // high then the clock low; the clock back high, the latch pulsed. 0x580: the interrupt
    // enables, as in the trace; a write of the 60 Hz handler's (0x0d00) between words
    M65851Port port;
    std::vector<uint16_t> got;
    auto put = [&](unsigned v) {
        uint16_t w;
        if (port.write(v & 7, w)) got.push_back(w);
    };
    const uint16_t sent[] = {0xc101, 0x3d1d, 0x4051, 0x8001, 0x42d1, 0x1d1d, 0x49d1, 0xc581, 0x3d8d,
                             0xc501, 0x3c79, 0x3cb9, 0x8019, 0x8281};
    for (uint16_t w : sent) {
        put(0x0d00);
        for (int b = 15; b >= 0; b--) {
            const unsigned d = (w >> b & 1) << 2;
            put(0x580 | d | 2), put(0x580 | d);
        }
        put(0x582), put(0x583), put(0x582);
    }
    CHECK(got.size() == sizeof sent / sizeof sent[0]);
    for (size_t i = 0; i < got.size() && i < sizeof sent / sizeof sent[0]; i++) CHECK(got[i] == sent[i]);

    M65851 c;
    const M65851::Settings &s = c.settings();
    reset_words(c);
    CHECK(s.source == 0 && !s.delay_sum && !s.key_bypass && !s.key_mixing && s.delay_mix && !s.delay_reverse);
    CHECK(s.surround && s.surround_ms == 50 && s.echo_ms == 200 && s.delay_off && !s.feedback_off && s.feedback_db == -2);
    CHECK(s.key == 0 && !s.key_off && s.key_db == 0 && !s.delay_mute && !s.mic_volume);
    CHECK(s.bass_db == 0 && s.treble_db == 0 && s.phase_shifter == 0 && !s.vocal_support);
    CHECK(!c.active());
    // KEY's slider ends and its switch
    words(c, {0x42d1});
    CHECK(c.settings().key == -5 && c.active());
    words(c, {0x49d1, 0x1d1d});
    CHECK(s.key == 3 && !s.surround && s.delay_off);
    // ECHO: bypass, reverse phase, 50 ms at -3 dB, feedback by slider 1
    words(c, {0xc581, 0x3d8d});
    CHECK(s.key_bypass && s.delay_mix && s.delay_reverse && !s.delay_sum);
    CHECK(s.surround && c.delay_ms() == 50 && !s.delay_off && s.delay_db == -3 && s.feedback_db == -6);
    words(c, {0x3d81});
    CHECK(s.feedback_off);
    words(c, {0x3d9d});
    CHECK(!s.feedback_off && s.feedback_db == -2);
    // SRD: same phase, 10 ms, feedback -3 dB, the delay volume by slider 1
    words(c, {0xc501, 0x3c79});
    CHECK(s.key_bypass && !s.delay_reverse && c.delay_ms() == 10 && s.delay_db == -6 && s.feedback_db == -3);
    words(c, {0x3c19});
    CHECK(s.delay_off);
    words(c, {0x3cb9});
    CHECK(!s.delay_off && s.delay_db == 0);
    // KEY after SRD (the game's 3c01, 1c01): delay and feedback off
    words(c, {0x3c01});
    CHECK(s.delay_off && s.feedback_off);
    // sliders 2 and 3
    words(c, {0x8019});
    CHECK(s.bass_db == -12 && s.treble_db == 0);
    words(c, {0x8029});
    CHECK(s.bass_db == 4);
    words(c, {0x8181});
    CHECK(s.treble_db == -12 && s.bass_db == 0);
    words(c, {0x8281});
    CHECK(s.treble_db == 4);
    // another chip's address (D14 high, or D15 low): nothing changes
    CHECK(!c.write(0x8283) && !c.write(0x8280) && s.treble_db == 4);
    CHECK(M65851::word_of(2, 0x0a0) == 0x8281);
}

// ---- pass-through
void bypass() {
    const std::vector<float> in = noise(RATE / 2);
    {
        M65851 c;   // nothing sent
        std::vector<float> v = in;
        CHECK(!c.active());
        c.process(v.data(), RATE / 2);
        CHECK(std::memcmp(v.data(), in.data(), v.size() * sizeof(float)) == 0);
        reset_words(c);
        CHECK(!c.active());
        c.process(v.data(), RATE / 2);
        CHECK(std::memcmp(v.data(), in.data(), v.size() * sizeof(float)) == 0);
    }
    {   // every effect on, then "off": once the glides are done, untouched again
        M65851 c;
        reset_words(c);
        words(c, {0x42d1, 0xc581, 0x3d9d, 0x8029});
        std::vector<float> v = in;
        c.process(v.data(), RATE / 2);
        CHECK(std::memcmp(v.data(), in.data(), v.size() * sizeof(float)) != 0);
        reset_words(c);
        v = in;
        c.process(v.data(), RATE / 10);   // 100 ms: past the 10 ms glides
        CHECK(!c.active());
        std::vector<float> w = in;
        c.process(w.data(), RATE / 2);
        CHECK(std::memcmp(w.data(), in.data(), w.size() * sizeof(float)) == 0);
    }
}

// ---- KEY
void key() {
    for (auto [word, k] : {std::pair<uint16_t, int>{0x49d1, 3}, {0x42d1, -5}, {0x48d1, 1}}) {
        M65851 c;
        reset_words(c);
        words(c, {word});
        CHECK(c.settings().key == k);
        std::vector<float> v = sine(RATE * 3 / 2, 440, 0.5);
        c.process(v.data(), RATE * 3 / 2);
        const double want = 440 * std::pow(2.0, k / 12.0), got = peak_hz(v, RATE / 2, 300, 600);
        printf("key %+d: %.2f Hz (%.2f expected), level %.3f\n", k, got, want, rms(v, 0, RATE / 2) / (0.5 / std::sqrt(2.0)));
        // (within a quarter of a half tone: each cross-fade from one read point to the other
        // steps the phase, which moves the strongest line a little off the exact ratio)
        CHECK(std::fabs(got / want - 1) < 0.015);
        // the level about as it came (the two read points' cross-fade dips it some)
        const double lvl = rms(v, 0, RATE / 2) / (0.5 / std::sqrt(2.0));
        CHECK(lvl > 0.6 && lvl < 1.1);
        CHECK(std::fabs(rms(v, 0, RATE / 2) - rms(v, 1, RATE / 2)) < 1e-6);   // both channels alike
    }
}

// ---- the delay block: an impulse on L (L-R = the impulse), its echoes on L and R
struct Echo {
    double l, r;
};
Echo echo_at(const std::vector<float> &v, size_t at) {   // the sums around a delay (DC gain 1)
    Echo e{0, 0};
    for (size_t i = at - 3; i < at + 60; i++) e.l += v[2 * i], e.r += v[2 * i + 1];
    return e;
}
double before(const std::vector<float> &v, size_t from, size_t to) {
    double m = 0;
    for (size_t i = from; i < to; i++) m = std::max({m, std::fabs((double)v[2 * i]), std::fabs((double)v[2 * i + 1])});
    return m;
}
void delay() {
    const size_t warm = 2000, n = 20000;
    {   // SRD at the top of slider 1: 10 ms at 0 dB, feedback -3 dB, same phase
        M65851 c;
        reset_words(c);
        words(c, {0xc501, 0x3cb9});
        std::vector<float> v(2 * n, 0.0f);
        v[2 * warm] = 1.0f;
        c.process(v.data(), (int)n);
        const Echo e1 = echo_at(v, warm + 441), e2 = echo_at(v, warm + 882);
        printf("SRD: %.4f %.4f at 10 ms, %.4f %.4f at 20 ms\n", e1.l, e1.r, e2.l, e2.r);
        CHECK(v[2 * warm] == 1.0f && v[2 * warm + 1] == 0.0f);   // the dry impulse as it was
        CHECK(before(v, warm + 1, warm + 438) < 1e-4);
        CHECK(std::fabs(e1.l - 1.0) < 0.03 && std::fabs(e1.r - 1.0) < 0.03);
        CHECK(std::fabs(e2.l - 0.7079) < 0.03 && std::fabs(e2.r - 0.7079) < 0.03);
    }
    {   // ECHO at the middle of slider 1: 50 ms at -3 dB, feedback -6 dB, reverse phase
        M65851 c;
        reset_words(c);
        words(c, {0xc581, 0x3d8d, 0x4051, 0x8001});
        std::vector<float> v(2 * n, 0.0f);
        v[2 * warm] = 1.0f;
        c.process(v.data(), (int)n);
        const Echo e1 = echo_at(v, warm + 2205), e2 = echo_at(v, warm + 4410);
        printf("ECHO: %.4f %.4f at 50 ms, %.4f %.4f at 100 ms\n", e1.l, e1.r, e2.l, e2.r);
        CHECK(before(v, warm + 1, warm + 2200) < 1e-4);
        CHECK(std::fabs(e1.l - 0.7079) < 0.03 && std::fabs(e1.r + 0.7079) < 0.03);
        CHECK(std::fabs(e2.l - 0.7079 * 0.5012) < 0.02 && std::fabs(e2.r + 0.7079 * 0.5012) < 0.02);
        // slider 1 at the bottom: the feedback off, one echo only
        words(c, {0x3d81});
        std::vector<float> w(2 * n, 0.0f);
        w[2 * warm] = 1.0f;
        c.process(w.data(), (int)n);
        CHECK(std::fabs(echo_at(w, warm + 2205).l - 0.7079) < 0.03 && before(w, warm + 2300, warm + 6000) < 1e-3);
    }
    {   // L+R the same (no L-R): SRD hears nothing
        M65851 c;
        reset_words(c);
        words(c, {0xc501, 0x3cb9});
        std::vector<float> v = sine(8000, 440, 0.5), in = v;
        c.process(v.data(), 8000);
        double d = 0;
        for (size_t i = 0; i < v.size(); i++) d = std::max(d, (double)std::fabs(v[i] - in[i]));
        CHECK(d < 1e-6);
    }
}

// ---- the tone
double tone_db(uint16_t word, double hz) {
    M65851 c;
    reset_words(c);
    words(c, {word});
    std::vector<float> v = sine(RATE / 2, hz, 0.25);
    c.process(v.data(), RATE / 2);
    return 20 * std::log10(rms(v, 0, RATE / 4) / (0.25 / std::sqrt(2.0)));
}
void tone() {
    const struct { uint16_t word; double low, high; } cases[] = {
        {0x8029, 4, 0}, {0x8019, -12, 0}, {0x8281, 0, 4}, {0x8181, 0, -12}};
    for (auto &k : cases) {
        const double lo = tone_db(k.word, 100), hi = tone_db(k.word, 10000);
        printf("tone %04x: %+.2f dB at 100 Hz, %+.2f dB at 10 kHz\n", k.word, lo, hi);
        CHECK(std::fabs(lo - k.low) < 1.0);
        CHECK(std::fabs(hi - k.high) < 1.0);
    }
}

// ---- in pieces
void pieces() {
    const int n = RATE;
    const std::vector<float> in = noise(n, 7);
    auto run = [&](bool cut) {
        M65851 c;
        reset_words(c);
        words(c, {0x42d1, 0xc581, 0x3d9d, 0x8029});
        std::vector<float> v = in;
        if (!cut) c.process(v.data(), n);
        else {
            uint32_t seed = 3;
            for (int i = 0; i < n;) {
                seed = seed * 1664525u + 1013904223u;
                const int m = std::min(n - i, (int)(seed >> 22) + 1);
                c.process(v.data() + 2 * i, m);
                i += m;
            }
        }
        return v;
    };
    const std::vector<float> a = run(false), b = run(true);
    CHECK(std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
    for (float x : a) CHECK(std::isfinite(x));
    // the bits, to compare between builds and machines (the same on every one)
    uint64_t h = 1469598103934665603ull;
    for (float x : a) {
        uint32_t u;
        std::memcpy(&u, &x, 4);
        h = (h ^ u) * 1099511628211ull;
    }
    printf("all on, a second of noise: %016llx\n", (unsigned long long)h);
}

void switch_() {
    setenv_("HK_M65851", "0");
    CHECK(!M65851::enabled());
    setenv_("HK_M65851", "1");
    CHECK(M65851::enabled());
    setenv_("HK_M65851", nullptr);
    CHECK(M65851::enabled());
}

}  // namespace

int main() {
    serial();
    bypass();
    key();
    delay();
    tone();
    pieces();
    switch_();
    if (g_fail) fprintf(stderr, "%d failed\n", g_fail);
    else printf("ok\n");
    return g_fail ? 1 : 0;
}
