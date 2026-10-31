// SPDX-License-Identifier: GPL-3.0-or-later
#include "effector.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace hk::host {

namespace {

constexpr int BLOCK = 32;   // settings move on a block at a time
constexpr double PI = 3.14159265358979323846;

// ---------------------------------------------------------------- the programs
// What each sounded like on a cabinet isn't known here (the driver's own effects): these are
// HIRAKU's, an echo or a reverb growing with the number on the game's label
struct Preset {
    const char *name;
    bool echo;
    // an echo's
    float ms, feedback, cutoff_hz;
    bool ping_pong;
    // a reverb's (Freeverb's room size and damping, 0-1) and its wait before it starts
    float room, damp, predelay_ms;
    float wet, dry;
};

const Preset PRESETS[Effector::PROGRAMS] = {
    {"none", false, 0, 0, 0, false, 0, 0, 0, 0, 1},
    {"ECHO 1", true, 125, 0.30f, 6000, false, 0, 0, 0, 0.45f, 0.85f},
    {"ECHO 2", true, 190, 0.40f, 5000, true, 0, 0, 0, 0.45f, 0.85f},
    {"ECHO 3", true, 280, 0.48f, 4500, true, 0, 0, 0, 0.45f, 0.82f},
    {"ECHO 4", true, 375, 0.55f, 4000, true, 0, 0, 0, 0.50f, 0.80f},
    {"ECHO 5", true, 500, 0.60f, 3500, true, 0, 0, 0, 0.50f, 0.80f},
    {"REVERB 1", false, 0, 0, 0, false, 0.55f, 0.50f, 10, 0.30f, 0.85f},
    {"REVERB 2", false, 0, 0, 0, false, 0.75f, 0.40f, 20, 0.33f, 0.82f},
    {"REVERB 3", false, 0, 0, 0, false, 0.85f, 0.30f, 30, 0.36f, 0.80f},
    {"small room", false, 0, 0, 0, false, 0.35f, 0.30f, 2, 0.22f, 0.90f},
    {"room", false, 0, 0, 0, false, 0.55f, 0.50f, 6, 0.25f, 0.88f},
    {"hall", false, 0, 0, 0, false, 0.80f, 0.45f, 25, 0.30f, 0.82f},
    {"outdoors", true, 260, 0.25f, 3000, false, 0, 0, 0, 0.35f, 0.88f},
    {"underwater", false, 0, 0, 0, false, 0.70f, 0.95f, 15, 0.40f, 0.70f},
};

const Preset &preset_of(int program) { return PRESETS[program > 0 && program < Effector::PROGRAMS ? program : 0]; }

// A value moving to its target by a step a block
struct Ramp {
    float now = 0, to = 0;
    void toward(float step) { now = now < to ? std::min(to, now + step) : std::max(to, now - step); }
    bool moving() const { return now != to; }
};

// ---------------------------------------------------------------- the equaliser
// Ten peaking filters (RBJ's cookbook), an octave each, at the driver's bands
struct Equaliser {
    static constexpr double HZ[10] = {31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000};
    struct Band {
        float db = 0, target = 0;
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        double z[2][2] = {};   // per channel (transposed direct form II)
    } band[10];
    int rate;

    void design(Band &b, double f) {
        f = std::min(f, rate * 0.45);
        const double A = std::pow(10.0, b.db / 40.0), w = 2 * PI * f / rate, alpha = std::sin(w) / (2 * 1.41);
        const double a0 = 1 + alpha / A;
        b.b0 = (1 + alpha * A) / a0;
        b.b1 = -2 * std::cos(w) / a0;
        b.b2 = (1 - alpha * A) / a0;
        b.a1 = b.b1;
        b.a2 = (1 - alpha / A) / a0;
    }
    // a block's gains moved toward the targets (0.25 dB a block: 12 dB in 35 ms)
    void step() {
        for (int i = 0; i < 10; i++) {
            Band &b = band[i];
            if (b.db == b.target) continue;
            b.db = b.db < b.target ? std::min(b.target, b.db + 0.25f) : std::max(b.target, b.db - 0.25f);
            design(b, HZ[i]);
        }
    }
    bool flat() const {
        for (const Band &b : band)
            if (b.db != 0 || b.target != 0) return false;
        return true;
    }
    void run(float *lr, int n) {
        for (Band &b : band) {
            if (b.db == 0 && b.target == 0) {
                b.z[0][0] = b.z[0][1] = b.z[1][0] = b.z[1][1] = 0;
                continue;
            }
            for (int c = 0; c < 2; c++) {
                double z0 = b.z[c][0], z1 = b.z[c][1];
                for (int i = 0; i < n; i++) {
                    const double x = lr[2 * i + c], y = b.b0 * x + z0;
                    z0 = b.b1 * x - b.a1 * y + z1;
                    z1 = b.b2 * x - b.a2 * y;
                    lr[2 * i + c] = (float)y;
                }
                b.z[c][0] = z0, b.z[c][1] = z1;
            }
        }
    }
};

// ---------------------------------------------------------------- the echo
struct Echo {
    std::vector<float> line[2];
    int at = 0, len = 0, delay = 0;
    float feedback = 0, lp = 1, state[2] = {};
    bool ping_pong = false;

    void start(int rate, const Preset &p) {
        len = rate * 2;
        line[0].assign(len, 0.0f);
        line[1].assign(len, 0.0f);
        at = 0;
        delay = std::clamp((int)(p.ms * rate / 1000), 1, len - 1);
        feedback = p.feedback;
        lp = (float)(1 - std::exp(-2 * PI * p.cutoff_hz / rate));
        ping_pong = p.ping_pong;
        state[0] = state[1] = 0;
    }
    // in: the send (stereo); out: the echoes
    void run(const float *in, float *out, int n) {
        for (int i = 0; i < n; i++) {
            const int from = (at - delay + len) % len;
            float y[2];
            for (int c = 0; c < 2; c++) {
                state[c] += lp * (line[c][from] - state[c]);   // the echo darker each time round
                y[c] = state[c];
            }
            if (ping_pong) {
                line[0][at] = 0.5f * (in[2 * i] + in[2 * i + 1]) + feedback * y[1];
                line[1][at] = feedback * y[0];
            } else {
                line[0][at] = in[2 * i] + feedback * y[0];
                line[1][at] = in[2 * i + 1] + feedback * y[1];
            }
            out[2 * i] = y[0], out[2 * i + 1] = y[1];
            if (++at == len) at = 0;
        }
    }
};

// ---------------------------------------------------------------- the reverb
// Freeverb's (Jezar at Dreampoint, public domain): eight damped combs side by side and four
// all-passes after them, each channel's 23 samples apart, lengths for 44.1 kHz scaled to the rate
struct Reverb {
    struct Comb {
        std::vector<float> buf;
        int at = 0;
        float store = 0;
        float run(float x, float fb, float damp) {
            const float y = buf[at];
            store = y * (1 - damp) + store * damp;
            buf[at] = x + store * fb;
            if (++at == (int)buf.size()) at = 0;
            return y;
        }
    };
    struct Allpass {
        std::vector<float> buf;
        int at = 0;
        float run(float x) {
            const float b = buf[at], y = b - x;
            buf[at] = x + b * 0.5f;
            if (++at == (int)buf.size()) at = 0;
            return y;
        }
    };
    Comb comb[2][8];
    Allpass pass[2][4];
    std::vector<float> pre;
    int pre_at = 0;
    float fb = 0, damp = 0;

    void start(int rate, const Preset &p) {
        static const int COMB[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617}, PASS[4] = {556, 441, 341, 225};
        const double k = rate / 44100.0;
        for (int c = 0; c < 2; c++) {
            for (int i = 0; i < 8; i++) comb[c][i] = Comb{std::vector<float>((size_t)std::max(1.0, (COMB[i] + 23 * c) * k), 0.0f)};
            for (int i = 0; i < 4; i++) pass[c][i] = Allpass{std::vector<float>((size_t)std::max(1.0, (PASS[i] + 23 * c) * k), 0.0f)};
        }
        pre.assign((size_t)std::max(1, (int)(p.predelay_ms * rate / 1000)), 0.0f);
        pre_at = 0;
        fb = p.room * 0.28f + 0.7f;
        damp = p.damp * 0.4f;
    }
    void run(const float *in, float *out, int n) {
        for (int i = 0; i < n; i++) {
            const float x = pre[pre_at] * 0.015f;   // Freeverb's fixed gain
            pre[pre_at] = in[2 * i] + in[2 * i + 1];
            if (++pre_at == (int)pre.size()) pre_at = 0;
            for (int c = 0; c < 2; c++) {
                float y = 0;
                for (Comb &k : comb[c]) y += k.run(x, fb, damp);
                for (Allpass &a : pass[c]) y = a.run(y);
                out[2 * i + c] = 3 * y;   // Freeverb's wet scale
            }
        }
    }
};

// ---------------------------------------------------------------- the key shift
// A pitch shifter with a short delay: the sound read back from a delay line faster or slower than
// it's written (the pitch moved, by 2^(key/12)), the reading jumping back (or on) by about a cycle
// of the sound whenever it gets too near the writing (or too far), crossfaded over 10 ms. Where
// it jumps to is where the sound looks most like where it is (a cross-correlation over 6 ms), so
// the jumps don't beat with the music: the tempo is the same, the delay 6 to 30 ms
struct KeyShift {
    static constexpr int SIZE = 4096;   // a power of two, past the longest delay
    float line[SIZE][2] = {};
    float mono[SIZE] = {};
    int64_t written = 0;
    double ratio = 1;
    double delay = 0, delay_old = 0;
    int fade = 0, fade_len = 441;
    // where the reading goes (samples of delay): rising pitch, the reading catches up, from up to
    // HIGH back down to LOW; falling, it falls back from LOW_DOWN up to HIGH_DOWN
    int low = 260, high = 1200, high_down = 1300, look = 256, search = 600;

    // the reading from the middle of its way, in the sound written so far (the line's always
    // written: feed())
    void start(int rate) {
        fade_len = std::max(16, rate / 100);
        const double k = rate / 44100.0;
        low = (int)(260 * k), high = (int)(1200 * k), high_down = (int)(1300 * k);
        look = (int)(256 * k), search = (int)(600 * k);
        delay = (low + high) / 2.0;
        fade = 0;
    }
    float at(double d, int c) const {   // the line d samples back (Catmull-Rom)
        const double pos = (double)written - d;
        const int64_t i = (int64_t)std::floor(pos);
        const float t = (float)(pos - (double)i);
        const float y0 = line[(i - 1) & (SIZE - 1)][c], y1 = line[i & (SIZE - 1)][c];
        const float y2 = line[(i + 1) & (SIZE - 1)][c], y3 = line[(i + 2) & (SIZE - 1)][c];
        return y1 + 0.5f * t * (y2 - y0 + t * (2 * y0 - 5 * y1 + 4 * y2 - y3 + t * (3 * (y1 - y2) + y3 - y0)));
    }
    // of the delays from..to, the one whose next `look` samples best match those at `now`
    double best(double now, int from, int to) const {
        const int64_t a = written - (int64_t)now;
        double e_best = -1e30;
        int d_best = (from + to) / 2;
        for (int d = from; d <= to; d++) {
            const int64_t b = written - d;
            double xy = 0, yy = 1e-9;
            for (int k = 0; k < look; k += 2) {
                const float x = mono[(a + k) & (SIZE - 1)], y = mono[(b + k) & (SIZE - 1)];
                xy += x * y, yy += y * y;
            }
            const double e = xy / std::sqrt(yy);
            if (e > e_best) e_best = e, d_best = d;
        }
        return d_best;
    }
    void put(const float *lr) {
        const int64_t w = written & (SIZE - 1);
        line[w][0] = lr[0], line[w][1] = lr[1];
        mono[w] = lr[0] + lr[1];
        ++written;
    }
    void feed(const float *lr, int n) {
        for (int i = 0; i < n; i++) put(lr + 2 * i);
    }
    void run(float *lr, int n) {
        for (int i = 0; i < n; i++) {
            put(lr + 2 * i);
            if (!fade) {   // time to jump?
                if (ratio > 1 && delay <= low) {
                    delay_old = delay, delay = best(delay, high - search, high), fade = fade_len;
                } else if (ratio < 1 && delay >= high_down) {
                    delay_old = delay, delay = best(delay, low, low + search), fade = fade_len;
                }
            }
            float out[2] = {at(delay, 0), at(delay, 1)};
            if (fade) {
                const float g = 0.5f - 0.5f * (float)std::cos(PI * (fade_len - fade) / fade_len);   // 0 to 1
                for (int c = 0; c < 2; c++) out[c] = out[c] * g + at(delay_old, c) * (1 - g);
                delay_old += 1 - ratio;
                --fade;
            }
            delay += 1 - ratio;
            delay = std::clamp(delay, 3.0, (double)SIZE - 4);
            lr[2 * i] = out[0], lr[2 * i + 1] = out[1];
        }
    }
};

}  // namespace

struct Effector::Impl {
    int rate;
    Settings want;
    Equaliser eq;
    // the environment running (0 none), its send (in) and its sound (out), the dry sound's level
    int env = 0, env_next = 0;   // programs
    const Preset *preset = nullptr;
    Ramp send, wet, dry;
    Echo echo;
    Reverb reverb;
    int quiet = 0;   // blocks the environment's sound has been silent with nothing sent
    std::vector<float> in, fx;
    // the key shift, faded in and out against the sound as it was
    KeyShift shift;
    Ramp shifted;
    int key = 0;
    std::vector<float> plain;
    Ramp volume;

    explicit Impl(int r) : rate(r) {
        eq.rate = r;
        dry.now = dry.to = 1;
        volume.now = volume.to = 1;
        shift.start(r);
    }

    void start_env(int e) {
        env = e;
        preset = &preset_of(e);
        if (preset->echo) echo.start(rate, *preset);
        else reverb.start(rate, *preset);
        quiet = 0;
    }

    // a block's settings: toward what the game set
    void step() {
        for (int i = 0; i < 10; i++) eq.band[i].target = want.eq_on ? want.eq_db[i] : 0;
        eq.step();
        const float ms_step = (float)BLOCK / (rate * 0.015f);   // a ramp's whole way in 15 ms
        // the environment: a new one comes once the old one's sound has faded out
        env_next = want.program > 0 && want.program < PROGRAMS ? want.program : 0;
        if (env_next != env) {
            if (env && env_next) {   // another: the old one out quickly, then the new one in
                send.to = 0, wet.to = 0;
                if (!wet.now) start_env(env_next);
            } else if (env_next) {   // from none
                start_env(env_next);
                wet.now = 0;
            } else {   // to none: nothing more sent, what's there rings out
                send.to = 0;
                dry.to = 1;
                if (quiet > rate / BLOCK) env = 0, preset = nullptr;   // a second of silence: done
            }
        }
        if (env && env == env_next) send.to = 1, wet.to = preset->wet, dry.to = preset->dry;
        send.toward(ms_step), wet.toward(ms_step * 0.5f), dry.toward(ms_step * 0.2f);
        // the key shift
        if (want.key && !key) shift.start(rate);
        if (want.key) key = want.key, shift.ratio = std::pow(2.0, key / 12.0);
        shifted.to = want.key ? 1 : 0;
        shifted.toward((float)BLOCK / (rate * 0.010f));
        if (!shifted.now && !want.key) key = 0;
    }

    void block(float *lr, int n) {
        step();
        if (key) {   // the key shift, faded against the sound as it was
            plain.assign(lr, lr + 2 * n);
            shift.run(lr, n);
            if (shifted.now < 1) {   // equal power: the two don't match, so they'd partly cancel
                const float a = (float)std::sin(PI / 2 * shifted.now), b = (float)std::cos(PI / 2 * shifted.now);
                for (int i = 0; i < 2 * n; i++) lr[i] = lr[i] * a + plain[i] * b;
            }
        } else {
            shift.feed(lr, n);
        }
        if (env) {
            in.resize(2 * (size_t)n), fx.resize(2 * (size_t)n);
            float s = 0;
            for (int i = 0; i < 2 * n; i++) in[i] = lr[i] * send.now;
            if (preset->echo) echo.run(in.data(), fx.data(), n);
            else reverb.run(in.data(), fx.data(), n);
            for (int i = 0; i < 2 * n; i++) {
                lr[i] = lr[i] * dry.now + fx[i] * wet.now;
                s = std::max(s, std::fabs(fx[i]));
            }
            quiet = s < 1e-5f && send.now == 0 ? quiet + 1 : 0;
        } else if (dry.now != 1) {
            dry.to = 1;
            for (int i = 0; i < 2 * n; i++) lr[i] *= dry.now;
        }
        if (!eq.flat()) eq.run(lr, n);
        volume.to = std::clamp(want.volume, 0.0f, 2.0f);
        if (volume.now != 1 || volume.to != 1) {   // a block's way along (the whole way in 0.7 s)
            const float from = volume.now;
            volume.toward((float)BLOCK / (rate * 0.7f));
            for (int i = 0; i < n; i++) {
                const float g = from + (volume.now - from) * (i + 1) / n;
                lr[2 * i] *= g, lr[2 * i + 1] *= g;
            }
        }
    }
};

Effector::Effector(int rate) : p_(std::make_unique<Impl>(rate)) {}
Effector::~Effector() = default;

void Effector::set(const Settings &s) { p_->want = s; }

void Effector::process(float *lr, int frames) {
    Impl &p = *p_;
    if (!p.env && !p.want.program && !p.key && !p.want.key && p.dry.now == 1 && p.eq.flat() && !p.want.eq_on &&
        p.volume.now == 1 && p.want.volume == 1) {
        p.shift.feed(lr, frames);   // (the key shift's line kept: it starts from the sound as it is)
        return;
    }
    for (int i = 0; i < frames; i += BLOCK) p.block(lr + 2 * i, std::min(BLOCK, frames - i));
}

const char *Effector::program_name(int program) { return preset_of(program).name; }

}  // namespace hk::host
