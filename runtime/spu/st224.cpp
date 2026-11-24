// SPDX-License-Identifier: GPL-3.0-or-later
#include "st224.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace hk::spu {

namespace {

constexpr int BLOCK = 32;             // the controls move every 32nd sample
constexpr float PI = 3.14159265358979f;
// The SOURCE pad's level (A0 6D, 0-99) that leaves the sound as loud as it came: HIRAKU's choice.
// The game sends each effect its own level, 50 to 85, higher for the effects that take sound away
// (the filters, EXTREAM EQ) and lower for those that add it (REVERB, DIMENSION, EXCITER): a level
// that evens the effects out. Nine of the nineteen, the ones that neither add nor take much (the
// modulations, RING MODULATOR, LO FI, STEP CRY), get 70, so 70 is taken as as loud as it came. (The
// unit's own reset value, 49, is only where a pad starts.)
constexpr float LEVEL_UNITY = 70;

// ---------------------------------------------------------------- the effects
// What PARAM A and B do (the unit's EDIT 1 wheel and EDIT 2 knob) follows the unit's manual; the
// ranges, where it gives none, and how each effect is made are HIRAKU's, to be tuned by ear. A
// knob's `at` maps the game's 0-100: straight, along an exponential (equal steps multiply), or a
// filter's resonance (Q 0.71, no peak, for most of the way, rising to 6 at the end)
enum class Curve : uint8_t { LIN, EXP, RES, RAW };   // RAW: the 0-100 itself, read by the effect
struct Knob {
    const char *what;
    float lo, hi;
    Curve curve;
    float at(float v) const {
        const float t = std::clamp(v, 0.0f, 100.0f) / 100.0f;
        switch (curve) {
        case Curve::EXP: return lo * std::pow(hi / lo, t);
        case Curve::RES: return 0.7071f * std::pow(8.5f, t * t * t);
        case Curve::RAW: return std::clamp(v, 0.0f, 100.0f);
        default: return lo + (hi - lo) * t;
        }
    }
};

enum Kind : uint8_t { THRU, DELAY, FLANGER, CHORUS, PITCH, DISTORTION, LOW_PASS, EXTREME_EQ, LO_FI, TREMOLO, VOLUME_BASS,
                      REVERB, STEP_CRY, PHASER, DIMENSION, RING_MOD, HIGH_PASS, RESONANCE, AUTO_PAN, EXCITER };

struct Program {
    int number;          // the unit's program (C0 nn): the game's table and the unit's manual agree
    const char *name;    // the game's name for it
    Kind kind;
    // from the game's table (each game's the same): the level it sends (A0 6D), its PARAM A and B to
    // start with, the range each knob turns through. Kept here for the unit tests and the log; the
    // game sends them itself.
    int level, a, b, a_min, a_max, b_min, b_max;
    Knob ka, kb;         // what each does (the manual's) over what range (HIRAKU's, but where noted)
};

// The game's table, in the game's order (alphabetical) of the PROGRAM knob. The unit's programs 0
// (a time stretch), 11 (a scratch) and 19 (a compressor) are never sent: none of them here
const Program PROGRAMS[] = {
    {20, "AUTO PAN", AUTO_PAN, 70, 15, 70, 0, 100, 0, 100, {"rate Hz", 0.1f, 10, Curve::EXP}, {"width", 0, 1, Curve::LIN}},
    {3, "CHORUS", CHORUS, 70, 60, 70, 0, 100, 0, 100, {"rate Hz", 0.1f, 5, Curve::EXP}, {"mix", 0, 1, Curve::LIN}},
    // (time: the manual's maximum, 505 ms)
    {1, "DELAY", DELAY, 60, 50, 50, 0, 100, 0, 100, {"time ms", 0, 505, Curve::LIN}, {"repeats and mix", 0, 1, Curve::LIN}},
    {15, "DIMENSION", DIMENSION, 55, 50, 90, 0, 100, 0, 100, {"intensity", 0, 1, Curve::LIN}, {"tone Hz", 1500, 16000, Curve::EXP}},
    {5, "DISTORTION", DISTORTION, 60, 30, 90, 0, 100, 20, 100, {"gain", 1, 300, Curve::EXP}, {"tone Hz", 800, 10000, Curve::EXP}},
    {21, "EXCITER", EXCITER, 55, 100, 100, 0, 100, 0, 100, {"sensitivity", 0, 1, Curve::LIN}, {"frequency Hz", 1000, 8000, Curve::EXP}},
    // A: 50 flat, up takes the chosen band away, down everything but it; B: LOW, MID or HIGH
    {7, "EXTREAM EQ", EXTREME_EQ, 85, 0, 30, 0, 100, 0, 100, {"gain", 0, 0, Curve::RAW}, {"band", 0, 0, Curve::RAW}},
    {2, "FLANGER", FLANGER, 70, 5, 20, 0, 100, 0, 100, {"rate Hz", 0.05f, 5, Curve::EXP}, {"feedback and mix", 0, 1, Curve::LIN}},
    {17, "HIGH PASS", HIGH_PASS, 80, 40, 50, 0, 100, 0, 100, {"cutoff Hz", 30, 8000, Curve::EXP}, {"resonance Q", 0, 0, Curve::RES}},
    {8, "LO FI EFFECT", LO_FI, 70, 70, 90, 0, 100, 20, 100, {"record noise", 0, 1, Curve::LIN}, {"tone Hz", 1000, 12000, Curve::EXP}},
    {6, "LOW PASS", LOW_PASS, 85, 80, 60, 0, 100, 0, 100, {"cutoff Hz", 100, 10000, Curve::EXP}, {"resonance Q", 0, 0, Curve::RES}},
    {14, "PHASER", PHASER, 70, 80, 25, 0, 100, 0, 100, {"rate Hz", 0.05f, 5, Curve::EXP}, {"resonance", 0, 0.85f, Curve::LIN}},
    // (pitch: the manual's +-1 octave, here in whole semitones)
    {4, "PITCH SHIFTER", PITCH, 75, 70, 90, 0, 100, 20, 100, {"semitones", -12, 12, Curve::LIN}, {"tone Hz", 1000, 18000, Curve::EXP}},
    // a filter that follows the sound's level
    {18, "RESONANCE", RESONANCE, 80, 50, 60, 0, 100, 50, 100, {"resonance Q", 1, 15, Curve::EXP}, {"sensitivity", 0, 1, Curve::LIN}},
    {12, "REVERB", REVERB, 55, 40, 70, 0, 100, 0, 100, {"time (room)", 0.70f, 0.98f, Curve::LIN}, {"mix", 0, 1, Curve::LIN}},
    {16, "RING MODULATOR", RING_MOD, 70, 80, 20, 0, 100, 0, 100, {"frequency Hz", 20, 2000, Curve::EXP}, {"balance", 0, 1, Curve::LIN}},
    // A: up more cry (sweeps, resonance), down more step; B: sensitivity (how far it sweeps)
    {13, "STEP CRY", STEP_CRY, 70, 50, 20, 0, 100, 0, 100, {"rate, cry or step", 0, 0, Curve::RAW}, {"sensitivity", 0, 1, Curve::LIN}},
    // A: rate and how far the wave is clipped toward a square
    {9, "TREMOLO", TREMOLO, 70, 20, 60, 0, 100, 0, 100, {"rate and clip", 0, 0, Curve::RAW}, {"depth", 0, 1, Curve::LIN}},
    // A: volume, 0 dB at 50 and the manual's +6 dB at 100 (the game keeps it at 50); B: bass boost
    {10, "VOLUME BASS", VOLUME_BASS, 50, 50, 70, 50, 50, 0, 100, {"volume", 0, 0, Curve::RAW}, {"bass boost dB", 0, 15, Curve::LIN}},
};

const Program *find(int number) {
    for (const Program &p : PROGRAMS)
        if (p.number == number) return &p;
    return nullptr;
}

// ---------------------------------------------------------------- building blocks
struct Line {   // a delay line per channel, read back at a fractional delay (linear)
    std::vector<float> buf;
    unsigned mask = 0, at = 0;
    void make(unsigned size) { buf.assign(size, 0.0f), mask = size - 1, at = 0; }
    void clear() { std::fill(buf.begin(), buf.end(), 0.0f), at = 0; }
    void put(float x) { buf[at++ & mask] = x; }
    float get(float d) const {   // d samples back from the newest, d >= 1
        const float p = (float)at - d;
        const float f = std::floor(p);
        const unsigned i = (unsigned)(int)f;
        const float t = p - f;
        const float a = buf[(i - 1) & mask], b = buf[i & mask];
        return a + (b - a) * t;
    }
};

struct Svf {   // Simper's state-variable filter (trapezoidal); stays stable while it moves
    float a1 = 0, a2 = 0, a3 = 0, k = 1, ic1[2] = {}, ic2[2] = {};
    void set(float hz, float q, float rate) {
        const float g = std::tan(PI * std::min(hz, rate * 0.45f) / rate);
        k = 1.0f / q;
        a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
    }
    void clear() { ic1[0] = ic1[1] = ic2[0] = ic2[1] = 0; }
    // the band (v1) and low (v2) outputs
    void run(float x, int c, float &band, float &low) {
        const float v3 = x - ic2[c];
        const float v1 = a1 * ic1[c] + a2 * v3, v2 = ic2[c] + a2 * ic1[c] + a3 * v3;
        ic1[c] = 2 * v1 - ic1[c], ic2[c] = 2 * v2 - ic2[c];
        band = v1, low = v2;
    }
};

struct Shelf {   // RBJ's shelving filter, a channel pair
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z[2][2] = {};
    float db_now = 1e9f;
    void set(bool low, float hz, float db, float rate) {
        if (db == db_now) return;
        db_now = db;
        const float A = std::pow(10.0f, db / 40.0f), w = 2 * PI * hz / rate, c = std::cos(w), s = std::sin(w);
        const float al = s / 2 * std::sqrt(2.0f), sa = 2 * std::sqrt(A) * al;
        float B0, B1, B2, A0, A1, A2;
        if (low) {
            B0 = A * ((A + 1) - (A - 1) * c + sa), B1 = 2 * A * ((A - 1) - (A + 1) * c), B2 = A * ((A + 1) - (A - 1) * c - sa);
            A0 = (A + 1) + (A - 1) * c + sa, A1 = -2 * ((A - 1) + (A + 1) * c), A2 = (A + 1) + (A - 1) * c - sa;
        } else {
            B0 = A * ((A + 1) + (A - 1) * c + sa), B1 = -2 * A * ((A - 1) + (A + 1) * c), B2 = A * ((A + 1) + (A - 1) * c - sa);
            A0 = (A + 1) - (A - 1) * c + sa, A1 = 2 * ((A - 1) - (A + 1) * c), A2 = (A + 1) - (A - 1) * c - sa;
        }
        b0 = B0 / A0, b1 = B1 / A0, b2 = B2 / A0, a1 = A1 / A0, a2 = A2 / A0;
    }
    void clear() { std::memset(z, 0, sizeof z), db_now = 1e9f; }
    float run(float x, int c) {
        const float y = b0 * x + z[c][0];
        z[c][0] = b1 * x - a1 * y + z[c][1];
        z[c][1] = b2 * x - a2 * y;
        return y;
    }
};

// Freeverb's reverb (Jezar at Dreampoint, public domain): eight damped combs and four all-passes a
// channel, the right's 23 samples longer
struct Reverb {
    struct Comb {
        std::vector<float> buf;
        unsigned at = 0;
        float store = 0;
    };
    struct Pass {
        std::vector<float> buf;
        unsigned at = 0;
    };
    Comb comb[2][8];
    Pass pass[2][4];
    void make(float rate) {
        static const int COMB[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617}, PASS[4] = {556, 441, 341, 225};
        for (int c = 0; c < 2; c++) {
            for (int i = 0; i < 8; i++) comb[c][i].buf.assign((size_t)((COMB[i] + 23 * c) * rate / 44100.0f), 0.0f);
            for (int i = 0; i < 4; i++) pass[c][i].buf.assign((size_t)((PASS[i] + 23 * c) * rate / 44100.0f), 0.0f);
        }
    }
    void clear() {
        for (auto &cc : comb)
            for (Comb &k : cc) std::fill(k.buf.begin(), k.buf.end(), 0.0f), k.at = 0, k.store = 0;
        for (auto &pp : pass)
            for (Pass &a : pp) std::fill(a.buf.begin(), a.buf.end(), 0.0f), a.at = 0;
    }
    float run(float x, int c, float fb, float damp) {
        float y = 0;
        for (Comb &k : comb[c]) {
            const float o = k.buf[k.at];
            k.store = o * (1 - damp) + k.store * damp;
            k.buf[k.at] = x + k.store * fb;
            if (++k.at == k.buf.size()) k.at = 0;
            y += o;
        }
        for (Pass &a : pass[c]) {
            const float b = a.buf[a.at];
            a.buf[a.at] = y + b * 0.5f;
            if (++a.at == a.buf.size()) a.at = 0;
            y = b - y;
        }
        return y;
    }
};

float soft(float x) {   // a soft clip, tanh-like, to +-1
    x = std::clamp(x, -3.0f, 3.0f);
    return x * (27 + x * x) / (27 + 9 * x * x);
}

}  // namespace

struct St224::Impl {
    float rate;
    State st;
    // the MIDI parser
    uint8_t status = 0, data[2] = {};
    int have = 0;
    bool sysex = false;

    // what runs: the program, faded in and out (g toward g_to, a step a sample)
    const Program *cur = nullptr;
    float g = 0, g_to = 0, g_step;
    uint64_t t = 0;              // samples processed since power-on
    // the controls, gliding to what the game sent (the PARAMs 0-100, the level as a gain)
    float a = 0, b = 0, level = 1, glide;
    bool fresh = true;           // the first block of a program: no glide from what was before

    // the effects' parts (one set; a new program starts them clear)
    Line line[2];                // delay, flanger, chorus, pitch, dimension
    float lp[2] = {}, tone[2] = {}, hold[2] = {}, env = 0, click = 0;
    int hold_n = 0;
    double phase = 0;            // an LFO's, in cycles
    float d_prev = 0, d_now = 0; // a delay's length a block ago and now (samples)
    Svf svf, xo[9];              // (xo: EXTREAM EQ's crossovers)
    Shelf shelf;
    Reverb rev;
    float ap[2][6] = {}, ap_fb[2] = {}, apc_prev = 0, apc_now = 0;   // the phaser's
    float band_g[3] = {1, 1, 1};  // EXTREAM EQ's low, mid, high gains (gliding)
    uint32_t lfsr = 0xace1u, noise = 0x12345678u;
    float step_hz = 1000, step_to = 1000;

    explicit Impl(int r) : rate((float)r) {
        g_step = 1.0f / (0.010f * rate);
        glide = 1 - std::exp(-(float)BLOCK / (0.020f * rate));
        for (Line &l : line) l.make(65536);   // 1.49 s at 44.1 kHz
        rev.make(rate);
    }

    void clear_parts() {
        for (Line &l : line) l.clear();
        lp[0] = lp[1] = tone[0] = tone[1] = hold[0] = hold[1] = 0, env = click = 0, hold_n = 0;
        phase = 0;
        svf.clear();
        for (Svf &f : xo) f.clear();
        shelf.clear();
        rev.clear();
        std::memset(ap, 0, sizeof ap), ap_fb[0] = ap_fb[1] = 0;
        band_g[0] = band_g[1] = band_g[2] = 1;
        lfsr = 0xace1u, noise = 0x12345678u, step_hz = step_to = 1000;
        fresh = true;
    }

    // ---- MIDI (the unit's manual: what it takes, and what it holds after a reset)
    void reset_controllers() {
        st.a = st.b = 0, st.on = false, st.sync = false, st.bpm = 120;
        st.pad_effect = false, st.level = 49;
    }
    void message() {
        st.messages++;
        const int kind = status & 0xf0;
        if (kind == 0xb0) {
            switch (data[0]) {
            case 12: st.a = data[1]; break;
            case 13: st.b = data[1]; break;
            case 14: st.bpm = 40 + data[1]; break;                       // the effect's tempo, 40-167 ...
            case 15: st.bpm = 168 + std::min<int>(data[1], 82); break;   // ... and 168-250
            case 80: st.on = data[1] >= 64; break;
            case 81: st.sync = data[1] >= 64; break;
            case 120: clear_parts(); break;                       // all sounds off: what rings is gone
            case 121: reset_controllers(); break;
            case 123: case 124: st.source = false; break;         // all notes off: the SOURCE pad let go
            default: st.unknown++; break;
            }
        } else if (kind == 0xc0) {
            st.program = data[0];
        } else if (kind == 0xa0) {
            if (data[0] == 108) st.pad_effect = data[1] != 0;   // the SOURCE pad through the effect
            else if (data[0] == 109) st.level = std::min<int>(data[1], 99);   // the SOURCE pad's level
            else st.unknown++;
        } else if (kind == 0x90 || kind == 0x80) {
            // the SOURCE pad (note 35): the input passes while it's down. Other notes play the unit's
            // own samples, which it has none of here
            if (data[0] == 35) st.source = kind == 0x90 && data[1] != 0;
            else st.unknown++;
        } else {
            st.unknown++;
        }
    }
    void midi(uint8_t x) {
        st.bytes++;
        if (x >= 0xf8) return;                            // real time: anywhere, changes nothing
        if (x == 0xf0) { sysex = true; status = 0; return; }
        if (x >= 0xf0) { sysex = false; status = 0; return; }   // end of exclusive, system common: no running status
        if (x & 0x80) { sysex = false; status = x; have = 0; return; }
        if (sysex || !status) return;
        data[have++] = x;
        const int need = ((status & 0xf0) == 0xc0 || (status & 0xf0) == 0xd0) ? 1 : 2;
        if (have == need) { message(); have = 0; }        // (running status: the next data byte starts another)
    }

    // ---- a block's controls
    // an effect is heard when it's on, the SOURCE pad is down and goes through the effect, and the
    // program is one of the game's
    const Program *wanted() const { return st.on && st.source && st.pad_effect ? find(st.program) : nullptr; }
    void control() {
        const Program *w = wanted();
        if (w != cur) {
            g_to = 0;
            if (g == 0) {
                if (w) clear_parts();
                cur = w;
            }
        }
        if (cur && cur == w) g_to = 1;
        const float ta = (float)std::min(st.a, 100), tb = (float)std::min(st.b, 100), tl = (float)st.level / LEVEL_UNITY;
        if (fresh) a = ta, b = tb, level = tl;
        a += (ta - a) * glide, b += (tb - b) * glide, level += (tl - level) * glide;
        if (std::fabs(ta - a) < 0.01f) a = ta;
        if (std::fabs(tb - b) < 0.01f) b = tb;
        if (std::fabs(tl - level) < 1e-4f) level = tl;
        if (cur) setup();
        fresh = false;
    }

    // what the program's controls mean this block
    float pa = 0, pb = 0;
    float one_pole(float hz) const { return 1 - std::exp(-2 * PI * std::min(hz, rate * 0.45f) / rate); }
    void setup() {
        pa = cur->ka.at(a), pb = cur->kb.at(b);
        const bool first = fresh;
        switch (cur->kind) {
        case DELAY:
            d_prev = d_now;
            d_now = std::max(1.0f, pa * rate / 1000);
            if (first) d_prev = d_now;
            break;
        case LOW_PASS: case HIGH_PASS: svf.set(pa, pb, rate); break;
        case EXCITER: svf.set(pb, 0.707f, rate); break;
        case EXTREME_EQ: {
            // the bands: below 300 Hz, 300 Hz-3 kHz, above 3 kHz (HIRAKU's); A at 50 leaves all, at
            // 100 the chosen band gone, at 0 everything but it gone
            if (first) {
                for (int k = 0; k < 4; k++) xo[k].set(300, 0.7071f, rate);
                for (int k = 4; k < 8; k++) xo[k].set(3000, 0.7071f, rate);
                xo[8].set(3000, 0.5f, rate);
            }
            const int chosen = pb < 100.0f / 3 ? 0 : pb < 200.0f / 3 ? 1 : 2;
            const float cut = pa > 50 ? 1 - (pa - 50) / 50 : 1, rest = pa < 50 ? pa / 50 : 1;
            for (int k = 0; k < 3; k++) {
                const float to = k == chosen ? cut : rest;
                band_g[k] = first ? to : band_g[k] + (to - band_g[k]) * glide;
            }
            break;
        }
        case VOLUME_BASS: shelf.set(true, 100, pb, rate); break;
        case STEP_CRY: svf.set(step_hz, 1.5f + 10.0f * pa / 100, rate); break;
        case PHASER: {
            // the all-passes' corner sweeps 200 Hz to 3 kHz (exponentially) with the LFO
            const float s = 0.5f + 0.5f * std::sin(2 * PI * (float)phase);
            const float hz = 200.0f * std::pow(15.0f, s);
            const float tn = std::tan(PI * hz / rate);
            apc_prev = first ? (tn - 1) / (tn + 1) : apc_now;
            apc_now = (tn - 1) / (tn + 1);
            break;
        }
        default: break;
        }
    }

    // a deterministic noise, -1 to 1
    float white() {
        noise = noise * 1664525u + 1013904223u;
        return (float)(int32_t)noise / 2147483648.0f;
    }

    // ---- the sound: y = the program's output for x (n <= BLOCK samples within one block)
    void run(const float *x, float *y, int n, unsigned pos) {
        const Kind kind = cur ? cur->kind : THRU;
        switch (kind) {
        case DELAY: {
            // the repeats and their level both grow with B
            const float fb = 0.8f * pb, wet = 0.25f + 0.5f * pb, damp = one_pole(5000);
            for (int i = 0; i < n; i++) {
                const float d = d_prev + (d_now - d_prev) * (float)(pos + i + 1) / BLOCK;
                for (int c = 0; c < 2; c++) {
                    const float e = line[c].get(d);
                    lp[c] += damp * (e - lp[c]);   // each echo a little darker
                    line[c].put(x[2 * i + c] + fb * lp[c]);
                    y[2 * i + c] = x[2 * i + c] + wet * e;
                }
            }
            break;
        }
        case FLANGER: case CHORUS: case DIMENSION: {
            // a short delay swept by an LFO, the right a quarter cycle on (DIMENSION's opposite).
            // (ms) FLANGER: 0.3 + 3 at A's rate, B its feedback and mix; CHORUS: 12 + 3 at A's rate,
            // B its mix; DIMENSION: 8 + up to 3 at 0.4 Hz, A its intensity, B the tone of what it adds
            float base = 0.3f, depth = 3.0f, hz = pa, fb = 0.0f, mix = 0.5f;
            if (kind == FLANGER) fb = 0.85f * pb, mix = 0.3f + 0.2f * pb;
            else if (kind == CHORUS) base = 12.0f, mix = pb;
            else base = 8.0f, depth = 0.5f + 2.5f * pa, hz = 0.4f, mix = pa;
            const float tk = kind == DIMENSION ? one_pole(pb) : 1.0f;
            const double inc = hz / rate;
            for (int i = 0; i < n; i++) {
                const float s = (float)phase;
                phase += inc;
                if (phase >= 1) phase -= 1;
                const float mono = 0.5f * (x[2 * i] + x[2 * i + 1]);
                for (int c = 0; c < 2; c++) {
                    const float off = kind == DIMENSION ? 0.5f * c : 0.25f * c;
                    const float m = 0.5f + 0.5f * std::sin(2 * PI * (s + off));
                    const float d = std::max(1.0f, (base + depth * m) * rate / 1000);
                    const float e = line[c].get(d);
                    if (kind == DIMENSION) {
                        line[c].put(mono);
                        tone[c] += tk * (e - tone[c]);
                        y[2 * i + c] = x[2 * i + c] * (1 - 0.3f * mix) + 0.7f * mix * tone[c];
                    } else if (kind == FLANGER) {
                        line[c].put(x[2 * i + c] + fb * e);
                        y[2 * i + c] = (1 - mix) * x[2 * i + c] + mix * e;
                    } else {
                        line[c].put(x[2 * i + c]);
                        y[2 * i + c] = (1 - 0.5f * mix) * x[2 * i + c] + 0.7f * mix * e;
                    }
                }
            }
            break;
        }
        case PITCH: {
            // two readings 1024 samples apart running through a 2048-sample window at the new speed,
            // each faded in and out (sine) as it passes the window's ends; the shifted sound only,
            // through B's tone
            constexpr float W = 2048;
            const float ratio = std::pow(2.0f, std::round(pa) / 12.0f), tk = one_pole(pb);
            for (int i = 0; i < n; i++) {
                phase += (1 - ratio) / W;
                phase -= std::floor(phase);
                for (int c = 0; c < 2; c++) line[c].put(x[2 * i + c]);
                for (int c = 0; c < 2; c++) {
                    float e = 0;
                    for (int k = 0; k < 2; k++) {
                        float p = (float)phase + 0.5f * k;
                        p -= std::floor(p);
                        e += std::sin(PI * p) * line[c].get(1 + p * W);
                    }
                    tone[c] += tk * (0.8f * e - tone[c]);   // (two sines' sum: 1 to 1.41)
                    y[2 * i + c] = tone[c];
                }
            }
            break;
        }
        case DISTORTION: {   // mono, as the manual says
            const float gain = pa, norm = 0.3f / soft(gain * 0.3f), tk = one_pole(pb);
            for (int i = 0; i < n; i++) {
                const float m = 0.5f * (x[2 * i] + x[2 * i + 1]);
                tone[0] += tk * (soft(gain * m) * norm - tone[0]);
                y[2 * i] = y[2 * i + 1] = tone[0];
            }
            break;
        }
        case LOW_PASS: case HIGH_PASS: case STEP_CRY: case EXCITER:
            for (int i = 0; i < n; i++) {
                if (kind == STEP_CRY && (pos + i) % 8 == 0) {
                    // the band's centre to the step's: at once at A = 0 (step), gliding over most of a
                    // step at 100 (cry)
                    const float glide_s = 0.002f + 0.8f * (pa / 100) / (2.0f * std::pow(4.0f, pa / 100));
                    step_hz += (step_to - step_hz) * (1 - std::exp(-8.0f / (glide_s * rate)));
                    svf.set(step_hz, 1.5f + 10.0f * pa / 100, rate);
                }
                for (int c = 0; c < 2; c++) {
                    const float in = x[2 * i + c];
                    float band, low;
                    svf.run(in, c, band, low);
                    switch (kind) {
                    case LOW_PASS: y[2 * i + c] = low; break;
                    case HIGH_PASS: y[2 * i + c] = in - svf.k * band - low; break;
                    case STEP_CRY: y[2 * i + c] = 0.3f * in + 2.5f * svf.k * band; break;
                    default: y[2 * i + c] = in + pa * 0.6f * soft(4 * (in - svf.k * band - low)); break;   // EXCITER
                    }
                }
                if (kind == STEP_CRY) {
                    // 2 to 8 steps a second as A goes up; each to one of 16 frequencies from 300 Hz up
                    // to as far as B lets it (to 750 Hz at 0, 3 kHz at 100)
                    phase += 2.0f * std::pow(4.0f, pa / 100) / rate;
                    if (phase >= 1) {
                        phase -= 1;
                        lfsr = (lfsr >> 1) ^ (-(lfsr & 1u) & 0xb400u);
                        step_to = 300.0f * std::pow(10.0f, (0.4f + 0.6f * pb) * (float)(lfsr & 15) / 15.0f);
                    }
                }
            }
            break;
        case RESONANCE: {
            // a resonant low-pass whose corner follows the sound's level: 150 Hz quiet, up to 6 kHz as
            // loud as B lets it (an envelope of 3 ms up, 120 ms down)
            const float up = one_pole(1.0f / (2 * PI * 0.003f)), down = one_pole(1.0f / (2 * PI * 0.120f));
            const float out = 1.5f / (1 + 0.5f * std::sqrt(pa));
            for (int i = 0; i < n; i++) {
                const float lev = 0.5f * (std::fabs(x[2 * i]) + std::fabs(x[2 * i + 1]));
                env += (lev > env ? up : down) * (lev - env);
                if ((pos + i) % 8 == 0) svf.set(150.0f * std::pow(40.0f, std::min(1.0f, env * 10.0f * pb)), pa, rate);
                for (int c = 0; c < 2; c++) {
                    float band, low;
                    svf.run(x[2 * i + c], c, band, low);
                    y[2 * i + c] = out * low;
                }
            }
            break;
        }
        case EXTREME_EQ: {
            // three bands by Linkwitz-Riley crossovers (two Butterworth sections each way), which add
            // back up to the sound's own level (only its phase moved), so a band taken away is gone
            // (24 dB an octave) and A at 50 changes nothing to the ear
            auto low = [](Svf &f, float v, int c) { float bd, lo; f.run(v, c, bd, lo); return lo; };
            auto high = [](Svf &f, float v, int c) { float bd, lo; f.run(v, c, bd, lo); return v - f.k * bd - lo; };
            for (int i = 0; i < n; i++)
                for (int c = 0; c < 2; c++) {
                    const float in = x[2 * i + c];
                    float lo = low(xo[1], low(xo[0], in, c), c);
                    const float rest = high(xo[3], high(xo[2], in, c), c);
                    const float mid = low(xo[5], low(xo[4], rest, c), c), hi = high(xo[7], high(xo[6], rest, c), c);
                    float bd, l2;
                    xo[8].run(lo, c, bd, l2);
                    lo = lo - 2 * xo[8].k * bd;   // (the low band through the upper crossover's all-pass)
                    y[2 * i + c] = band_g[0] * lo + band_g[1] * mid + band_g[2] * hi;
                }
            break;
        }
        case VOLUME_BASS: {
            // A: 0 dB at 50, +6 dB at 100 (the manual's), down to -40 dB at 0 (HIRAKU's)
            const float db = pa >= 50 ? 6.0f * (pa - 50) / 50 : -40.0f * (50 - pa) / 50, v = std::pow(10.0f, db / 20);
            for (int i = 0; i < n; i++)
                for (int c = 0; c < 2; c++) y[2 * i + c] = v * shelf.run(x[2 * i + c], c);
            break;
        }
        case LO_FI: {
            // the sound coarser (12 bits, every second sample held) through B's tone, with a record's
            // crackle and hiss as much as A says (HIRAKU's making)
            const float tk = one_pole(pb), q = 2048.0f, pop = one_pole(1.0f / (2 * PI * 0.0005f));
            for (int i = 0; i < n; i++) {
                if (hold_n-- <= 0) {
                    hold_n = 1;
                    for (int c = 0; c < 2; c++) hold[c] = std::round(x[2 * i + c] * q) / q;
                }
                const float r = white();
                if (std::fabs(r) < pa * 0.002f) click += r * 60.0f * pa;   // a crackle now and then
                click -= pop * click;
                const float noise_now = click + 0.004f * pa * white();
                for (int c = 0; c < 2; c++) {
                    tone[c] += tk * (hold[c] + noise_now - tone[c]);
                    y[2 * i + c] = tone[c];
                }
            }
            break;
        }
        case TREMOLO: case AUTO_PAN: case RING_MOD: {
            // TREMOLO's A: 1-20 Hz, the wave clipped toward a square as it goes up
            const float hz = kind == TREMOLO ? std::pow(20.0f, pa / 100) : pa;
            const float clip = kind == TREMOLO ? 1 + 6 * pa / 100 : 1;
            const double inc = hz / rate;
            for (int i = 0; i < n; i++) {
                const float s = std::sin(2 * PI * (float)phase);
                phase += inc;
                if (phase >= 1) phase -= 1;
                if (kind == TREMOLO) {
                    const float w = std::clamp(s * clip, -1.0f, 1.0f);
                    const float gain = 1 - pb * (0.5f + 0.5f * w);
                    y[2 * i] = x[2 * i] * gain, y[2 * i + 1] = x[2 * i + 1] * gain;
                } else if (kind == AUTO_PAN) {   // equal power, both 1 in the middle
                    const float p = (pb * s + 1) * (PI / 4);
                    y[2 * i] = x[2 * i] * std::cos(p) * 1.41421356f, y[2 * i + 1] = x[2 * i + 1] * std::sin(p) * 1.41421356f;
                } else {
                    for (int c = 0; c < 2; c++) y[2 * i + c] = (1 - pb) * x[2 * i + c] + pb * x[2 * i + c] * s;
                }
            }
            break;
        }
        case REVERB:
            for (int i = 0; i < n; i++) {
                const float in = (x[2 * i] + x[2 * i + 1]) * 0.015f;   // Freeverb's input gain
                for (int c = 0; c < 2; c++) y[2 * i + c] = (1 - 0.4f * pb) * x[2 * i + c] + pb * 3 * rev.run(in, c, pa, 0.3f);
            }
            break;
        case PHASER: {
            const double inc = pa / rate;
            for (int i = 0; i < n; i++) {
                phase += inc;
                if (phase >= 1) phase -= 1;
                const float k = apc_prev + (apc_now - apc_prev) * (float)(pos + i + 1) / BLOCK;
                for (int c = 0; c < 2; c++) {
                    float v = x[2 * i + c] + pb * ap_fb[c];
                    for (int s = 0; s < 6; s++) {   // first-order all-passes
                        const float o = k * v + ap[c][s];
                        ap[c][s] = v - k * o;
                        v = o;
                    }
                    ap_fb[c] = v;
                    y[2 * i + c] = 0.7f * (x[2 * i + c] + v);
                }
            }
            break;
        }
        default:
            std::memcpy(y, x, sizeof(float) * 2 * (size_t)n);
            break;
        }
    }

    void process(float *lr, int n) {
        // the program is given the sound faded as its output is (a delay or reverb doesn't start
        // from a sound cut off mid-wave, a click it would give back later)
        float y[2 * BLOCK], in[2 * BLOCK], gs[BLOCK];
        for (int i = 0; i < n;) {
            const unsigned pos = (unsigned)(t % BLOCK);
            if (pos == 0) control();
            const int m = std::min(n - i, BLOCK - (int)pos);
            float *x = lr + 2 * i;
            for (int k = 0; k < m; k++) {
                g = g < g_to ? std::min(g_to, g + g_step) : std::max(g_to, g - g_step);
                gs[k] = g;
                in[2 * k] = x[2 * k] * g, in[2 * k + 1] = x[2 * k + 1] * g;
            }
            if (cur) run(in, y, m, pos);
            else std::memcpy(y, in, sizeof(float) * 2 * (size_t)m);
            for (int k = 0; k < m; k++)
                for (int c = 0; c < 2; c++) x[2 * k + c] += gs[k] * (level * y[2 * k + c] - x[2 * k + c]);
            if (g == 0 && g_to == 0 && cur != wanted()) {   // faded out: the next program from the next block
                cur = nullptr;
            }
            t += (uint64_t)m, i += m;
        }
    }

    bool active() const { return cur || g > 0 || wanted(); }
};

St224::St224(int rate) : p_(std::make_unique<Impl>(rate)) {}
St224::~St224() = default;
void St224::midi(uint8_t byte) { p_->midi(byte); }
bool St224::active() const { return p_->active(); }
void St224::process(float *lr, int n) {
    if (!p_->active()) {   // (the samples counted all the same: the blocks stay where they were)
        p_->t += (uint64_t)n;
        return;
    }
    p_->process(lr, n);
}
void St224::reset() {
    const int r = (int)p_->rate;
    p_ = std::make_unique<Impl>(r);
}
const St224::State &St224::state() const { return p_->st; }
const char *St224::program_name(int program) {
    const Program *p = find(program);
    return p ? p->name : "";
}
bool St224::known(int program) { return find(program) != nullptr; }

}  // namespace hk::spu
