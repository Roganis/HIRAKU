// SPDX-License-Identifier: GPL-3.0-or-later
#include "twinkle/m65851.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace hk::twinkle {

namespace {

// 10^(dB/20) for whole dB from -12 to +12: every level the chip's tables name, and the tone's
// half-gains (dB/2). Constants, so the coefficients are the same whatever the C library.
constexpr double GAIN[25] = {
    0.25118864315095801, 0.28183829312644537, 0.31622776601683794, 0.35481338923357547, 0.3981071705534972,
    0.44668359215096315, 0.50118723362727224, 0.56234132519034907, 0.63095734448019325, 0.70794578438413791,
    0.79432823472428149, 0.89125093813374556, 1.0, 1.1220184543019633, 1.2589254117941673,
    1.4125375446227544, 1.5848931924611136, 1.7782794100389228, 1.9952623149688795, 2.2387211385683394,
    2.5118864315095801, 2.8183829312644537, 3.1622776601683795, 3.548133892335755, 3.9810717055349722};
double gain(int db) { return GAIN[std::clamp(db, -12, 12) + 12]; }

// 2^(k/12): the key's speed, -8..+8 half tones
constexpr double RATIO[17] = {
    0.6299605249474366, 0.66741992708501718, 0.70710678118654757, 0.74915353843834076, 0.79370052598409979,
    0.8408964152537145, 0.89089871814033927, 0.94387431268169353, 1.0, 1.0594630943592953,
    1.122462048309373, 1.189207115002721, 1.2599210498948732, 1.3348398541700344, 1.4142135623730951,
    1.4983070768766815, 1.5874010519681994};

constexpr double PI = 3.14159265358979323846;

// cos and sin of 0 <= w <= pi by their series: +-*/ only, so every machine gets the same bits
void cos_sin(double w, double &c, double &s) {
    double t = w, sum_s = 0, sum_c = 0, tc = 1;
    for (int k = 0; k < 30; k++) {
        sum_s += t, sum_c += tc;
        t = -t * w * w / ((2.0 * k + 2) * (2.0 * k + 3));
        tc = -tc * w * w / ((2.0 * k + 1) * (2.0 * k + 2));
    }
    c = sum_c, s = sum_s;
}

int D(uint16_t w, int n) { return w >> (15 - n) & 1; }
int bits(uint16_t w, int from, int count) {   // Dfrom (most significant) .. Dfrom+count-1
    int v = 0;
    for (int i = 0; i < count; i++) v = v << 1 | D(w, from + i);
    return v;
}

// the chip's 3-bit volume codes: 7 down to 1, 0 = off
constexpr int DELAY_DB[8] = {0, -12, -9, -6, -3, 0, 3, 6};
constexpr int FEEDBACK_DB[8] = {0, -10, -8, -6, -5, -4, -3, -2};
constexpr int KEY_DB[8] = {0, -10, -6, -4, -2, 0, 2, 4};
// D5 D6 D7 (D5 the low bit) -> ms: 10, 15, 20, 30, 50; the codes the chip doesn't name: 0 -> 10,
// 6 and 7 -> 50 (never sent)
constexpr int SURROUND_MS[8] = {10, 10, 15, 20, 30, 50, 50, 50};
constexpr int ECHO_MS[4] = {100, 130, 150, 200};   // D3 D4, D3 the low bit

// the corners: the delay block's low-passes (the chip's), the tone's (HIRAKU's choice)
constexpr double ECHO_LPF_HZ = 3000, SURROUND_LPF_HZ = 7000;
constexpr double BASS_HZ = 100, BASS_Q = 1.0, TREBLE_HZ = 3500;
constexpr int KEY_WINDOW_MS = 46;   // the pitch shifter's window (2048 samples at 44.1 kHz)

}  // namespace

M65851::M65851(int rate) : rate_(rate) {
    kwindow_ = 1;
    while (kwindow_ < rate * KEY_WINDOW_MS / 1000) kwindow_ <<= 1;
    for (auto &l : kline_) l.assign((size_t)kwindow_ * 2, 0.0f);
    unsigned n = 1;
    while (n < (unsigned)(rate / 5 + 4)) n <<= 1;   // 200 ms
    dline_.assign(n, 0.0f);
    glide_ = 100.0f / (float)rate;
    reset();
}

uint16_t M65851::word_of(int block, uint16_t d2_d13) {
    return (uint16_t)((block & 2) << 14 | (block & 1) << 14 | (d2_d13 & 0xfff) << 2 | 1);
}

bool M65851::enabled() {
    const char *e = std::getenv("HK_M65851");
    return !(e && e[0] == '0' && !e[1]);
}

void M65851::reset() {
    s_ = Settings();
    running_ = false;
    kmix_ = kmix_target_ = 0;
    dfade_ = dfade_target_ = 0;
    tone_ = false;
    settle();
}

bool M65851::write(uint16_t w) {
    if (D(w, 14) != 0 || D(w, 15) != 1) return false;
    Settings &s = s_;
    switch (D(w, 0) * 2 + D(w, 1)) {
    case 0:
        s.surround = D(w, 2);
        s.echo_ms = ECHO_MS[D(w, 3) | D(w, 4) << 1];
        s.surround_ms = SURROUND_MS[D(w, 5) | D(w, 6) << 1 | D(w, 7) << 2];
        s.delay_off = bits(w, 8, 3) == 0, s.delay_db = DELAY_DB[bits(w, 8, 3)];
        s.feedback_off = bits(w, 11, 3) == 0, s.feedback_db = FEEDBACK_DB[bits(w, 11, 3)];
        break;
    case 1: {
        s.auto_reset = D(w, 2), s.vsc = D(w, 3);
        const int m = std::min(bits(w, 5, 4), 8);   // (up with 0: taken as 0)
        s.key = D(w, 4) ? m : -m;
        s.key_off = bits(w, 9, 3) == 0, s.key_db = KEY_DB[bits(w, 9, 3)];
        s.mic_volume = D(w, 12), s.delay_mute = D(w, 13);
        break;
    }
    case 2: {
        s.vocal_support = D(w, 2);
        s.phase_shifter = D(w, 4) | D(w, 5) << 1;
        const int t = std::min(bits(w, 7, 3), 6), b = std::min(bits(w, 11, 3), 6);   // (7: taken as 6)
        s.treble_db = (D(w, 6) ? 2 : -2) * t;
        s.bass_db = (D(w, 10) ? 2 : -2) * b;
        break;
    }
    default:
        s.source = bits(w, 2, 2);
        s.delay_sum = D(w, 4);
        s.key_bypass = D(w, 5), s.key_mixing = D(w, 6);
        s.delay_mix = D(w, 7), s.delay_reverse = D(w, 8);
        s.scoring = bits(w, 9, 4);
        s.voice_key = D(w, 13);
        break;
    }
    settle();
    return true;
}

void M65851::settle() {
    const Settings &s = s_;
    // key control: the speed's phase step (the read points move by 1 - ratio samples a sample);
    // kept through a fade out to key 0
    if (s.key != 0) {
        const double r = RATIO[std::clamp(s.key, -8, 8) + 8];
        kstep_ = (int32_t)std::lround((1.0 - r) / kwindow_ * 4294967296.0);
    }
    kgain_ = s.key_off ? 0.0f : (float)gain(s.key_db);
    kmix_target_ = key_wanted() ? 1.0f : 0.0f;
    // delay block
    dlen_ = std::max(1, delay_ms() * rate_ / 1000);
    dgain_ = s.delay_off ? 0.0f : (float)gain(s.delay_db);
    fb_ = s.feedback_off ? 0.0f : (float)gain(s.feedback_db);
    dsign_ = s.delay_reverse ? -1.0f : 1.0f;
    dfade_target_ = delay_wanted() ? 1.0f : 0.0f;
    {   // RBJ's low-pass, Butterworth
        double c, sn;
        cos_sin(2 * PI * (s.surround ? SURROUND_LPF_HZ : ECHO_LPF_HZ) / rate_, c, sn);
        const double al = sn * std::sqrt(0.5), a0 = 1 + al;
        for (Biquad *f : {&din_, &dout_}) {
            f->b0 = (float)((1 - c) / 2 / a0), f->b1 = (float)((1 - c) / a0), f->b2 = f->b0;
            f->a1 = (float)(-2 * c / a0), f->a2 = (float)((1 - al) / a0);
        }
    }
    // tone: bass an RBJ peak, treble an RBJ high shelf (slope 1)
    const bool want = tone_wanted();
    if (want && !tone_) bass_.clear(), treble_.clear();
    tone_ = want;
    {
        double c, sn;
        cos_sin(2 * PI * BASS_HZ / rate_, c, sn);
        const double A = gain(s.bass_db / 2), al = sn / (2 * BASS_Q), a0 = 1 + al / A;
        bass_.b0 = (float)((1 + al * A) / a0), bass_.b1 = (float)(-2 * c / a0), bass_.b2 = (float)((1 - al * A) / a0);
        bass_.a1 = bass_.b1, bass_.a2 = (float)((1 - al / A) / a0);
    }
    {
        double c, sn;
        cos_sin(2 * PI * TREBLE_HZ / rate_, c, sn);
        const double A = gain(s.treble_db / 2), sa = 2 * std::sqrt(A) * (sn / 2 * std::sqrt(2.0));
        const double a0 = (A + 1) - (A - 1) * c + sa;
        treble_.b0 = (float)(A * ((A + 1) + (A - 1) * c + sa) / a0);
        treble_.b1 = (float)(-2 * A * ((A - 1) + (A + 1) * c) / a0);
        treble_.b2 = (float)(A * ((A + 1) + (A - 1) * c - sa) / a0);
        treble_.a1 = (float)(2 * ((A - 1) - (A + 1) * c) / a0);
        treble_.a2 = (float)(((A + 1) - (A - 1) * c - sa) / a0);
    }
}

bool M65851::active() const {
    return key_wanted() || delay_wanted() || tone_wanted() || kmix_ > 0 || dfade_ > 0;
}

void M65851::start() {
    for (auto &l : kline_) std::fill(l.begin(), l.end(), 0.0f);
    std::fill(dline_.begin(), dline_.end(), 0.0f);
    kat_ = dat_ = 0, kphase_ = 0;
    din_.clear(), dout_.clear(), bass_.clear(), treble_.clear();
    running_ = true;
}

namespace {
float toward(float v, float target, float step) {
    return v < target ? std::min(target, v + step) : std::max(target, v - step);
}
}  // namespace

void M65851::process(float *lr, int n) {
    if (!active()) return;
    if (!running_) start();
    const unsigned kmask = (unsigned)kline_[0].size() - 1, dmask = (unsigned)dline_.size() - 1;
    const double kscale = (double)kwindow_ / 4294967296.0;
    for (int i = 0; i < n; i++) {
        float l = lr[2 * i], r = lr[2 * i + 1];
        const float din = s_.delay_sum ? (l + r) * 0.5f : l - r;
        // key control
        // (the line is fed all the while the chip runs, so a key that comes back finds the
        // sound of now in it)
        kmix_ = toward(kmix_, kmix_target_, glide_);
        kline_[0][kat_ & kmask] = l, kline_[1][kat_ & kmask] = r;
        if (kmix_ > 0) {
            float out[2] = {0, 0};
            for (int tap = 0; tap < 2; tap++) {
                const uint32_t q = kphase_ + (tap ? 0x80000000u : 0u);
                const double d = 2.0 + (double)q * kscale;
                const unsigned di = (unsigned)d;
                const float f = (float)(d - (double)di);
                const float g = 1.0f - std::fabs((float)((double)q * (1.0 / 2147483648.0)) - 1.0f);
                for (int c = 0; c < 2; c++) {
                    const float a = kline_[c][(kat_ - di) & kmask], b = kline_[c][(kat_ - di - 1) & kmask];
                    out[c] += (a + (b - a) * f) * g;
                }
            }
            kphase_ += (uint32_t)kstep_;
            l = l + (out[0] * kgain_ - l) * kmix_;
            r = r + (out[1] * kgain_ - r) * kmix_;
        }
        kat_++;
        // tone
        if (tone_) {
            l = treble_.run(bass_.run(l, 0), 0);
            r = treble_.run(bass_.run(r, 1), 1);
        }
        // delay block
        // (as the key's line, fed all the while the chip runs)
        dfade_ = toward(dfade_, dfade_target_, glide_);
        const float y = dout_.run(dline_[(dat_ - (unsigned)dlen_) & dmask], 0);
        dline_[dat_ & dmask] = din_.run(din + fb_ * y, 0);
        dat_++;
        if (dfade_ > 0) {
            const float d = y * dgain_ * dfade_;
            l += d, r += d * dsign_;
        }
        lr[2 * i] = l, lr[2 * i + 1] = r;
    }
    if (!active()) running_ = false;
}

}  // namespace hk::twinkle
