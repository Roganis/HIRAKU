// SPDX-License-Identifier: GPL-3.0-or-later
// A stand-in for the Twinkle sound board's effect chip, a Mitsubishi (later Renesas) M65851FP
// karaoke processor: what beatmania IIDX 1st to 8th style's effector (VEFX: KEY, ECHO, SRD1-3, and
// the three sliders) changes in the sound. MAME has the chip in its list of the board's parts and
// does nothing with it.
//
// The control side is exact. The sound board's 68000 clocks 16-bit words out on its register
// 0x230000: bit 2 data, bit 1 clock (the bit is taken as the clock falls), bit 0 a latch (the word
// is taken as it rises). The program sends its word's bit 15 first, which is the chip's D0, so in
// the program's value the chip's Dn is bit 15-n. D14 low and D15 high are the chip's address (the
// program's word always has bit 0 set and bit 1 clear); D0 D1 choose one of four blocks of
// settings and D2-D13 are that block's:
//   block 0 (delay)   D2 surround (or echo), D3-D4 the echo time (100/130/150/200 ms), D5-D7 the
//                     surround time (10/15/20/30/50 ms), D8-D10 the delay volume (+6 to -12 dB in
//                     3 dB steps, or off), D11-D13 the feedback volume (-2 to -6 dB in 1 dB steps,
//                     -8, -10, or off)
//   block 1 (key)     D2 automatic reset, D3 VSC/VMC, D4 up, D5-D8 the key change in half tones
//                     (0-8), D9-D11 the key control volume (+4 to -6 dB in 2 dB steps, -10, or
//                     off), D12 the delay volume as a microphone volume, D13 delay mute
//   block 2 (tone)    D2-D3 vocal support, D4-D5 phase shifter (off, min, typ, max), D6 boost and
//                     D7-D9 the treble in 2 dB steps (0-6), D10 boost and D11-D13 the bass
//   block 3 (line)    D2-D3 the source (stereo, L, R, vocal cut), D4 the delay block fed (L+R)/2
//                     (or L-R), D5 the key control bypassed, D6 key control mixing, D7 the delay
//                     mixed into the outputs, D8 in reverse phase, D9-D12 scoring, D13 voice key
// What the games send (bmiidx2, 3 and 8 traced; the sound program is one file in every set):
//   reset and "off"   c101 3d1d 4051 8001: the key control not bypassed but at key 0, the delay
//                     volume off, the tone flat. As loud and as clean as it came (below).
//   KEY               slider 1 = the key, -5 at the bottom to +3 at the top, the delay block's
//                     switch on echo while the key isn't 0 (its volume stays off)
//   ECHO              c581 3d8d: the key control bypassed, surround 50 ms, the delay volume
//                     -3 dB fed L-R and mixed in reverse phase; slider 1 = the feedback volume
//                     (off, -10 ... -2 dB)
//   SRD1, SRD2, SRD3  c501 3c79: surround 10 ms, feedback -3 dB, same phase; slider 1 = the
//                     delay volume (off, -12 ... 0 dB). The three send the same words: they sound
//                     the same here (whatever told them apart on a cabinet isn't on this line)
//   slider 2, 3       bass and treble, -12 to +4 dB, in every type
//
// The sound side is a reconstruction, HIRAKU's: the chip is analogue around a 1-bit-style
// converter and two 16 Kbit RAMs, and nobody here has heard a cabinet's. What it is made of:
//   key control      a two-tap pitch shifter (a delay line read at the key's speed, two read
//                    points half a 46 ms window apart, cross-faded), on both channels alike, at the
//                    key control volume. Key 0 is the sound untouched. (The chip's own table makes
//                    the music (L+R)/2 whenever the key control isn't bypassed, which the games
//                    leave it all the time: HIRAKU keeps the stereo, key 0 or not.)
//   delay block      one delay line fed L-R or (L+R)/2 plus the feedback, low-passed before and
//                    after (Butterworth, 3 kHz in echo mode, 7 kHz in surround: the chip's two
//                    corners), out at the delay volume into L and R, R in reverse phase when so
//                    set. The feedback acts in every mode: the chip's delay block diagram takes it
//                    from the delay's output back to its input ahead of the switches, which only
//                    choose what reaches the output (and ECHO's slider moves it). Off when the
//                    delay mixing is off, the delay volume is off or the delay is muted. The
//                    chip's echo-mode dry path (its input added to the delay output, for a
//                    microphone) is left out: the games use echo mode only with the volume off.
//   tone             bass a peak or dip at 100 Hz (Q 1), treble a shelf from 3.5 kHz: the chip's
//                    test points are 100 Hz and 10 kHz, its real corners are set by the board's
//                    capacitors, which nobody has: a choice. Flat is untouched.
//   left out         the phase shifter (the games never turn it on), the sources other than
//                    stereo (never sent; taken as stereo), vocal support, scoring, the microphone,
//                    key control mixing, automatic reset.
// The key and the delay fade in and out over 10 ms as they turn on and off; every other change
// (a level, a key, a time, the tone) takes effect at its sample.
//
// While nothing is on (before any word, after the reset words, after "off" once the glides are
// done) active() is false and the caller leaves the sound as it is: the same to the bit as without
// the chip. Deterministic and the same on every machine: the arithmetic is float with no fused
// multiply-adds (-ffp-contract=off) and the coefficients come from constants and +-*/ and sqrt
// (no libm functions), per sample, so it is the same however the sound is cut into calls.
#pragma once

#include <cstdint>
#include <vector>

namespace hk::twinkle {

// The serial port: the register's bits 0-2 as each write leaves them.
struct M65851Port {
    uint16_t shift = 0;
    uint8_t pins = 0;
    // true, with `word`, when this write raised the latch
    bool write(unsigned bits, uint16_t &word) {
        if ((pins & 2) && !(bits & 2)) shift = (uint16_t)(shift << 1 | (bits >> 2 & 1));
        const bool latch = !(pins & 1) && (bits & 1);
        pins = (uint8_t)(bits & 7);
        if (latch) word = shift;
        return latch;
    }
};

class M65851 {
public:
    struct Settings {
        // block 0
        bool surround = true;
        int echo_ms = 150, surround_ms = 50;
        int delay_db = 0;   bool delay_off = true;   // delay volume (dB; off = -inf)
        int feedback_db = 0; bool feedback_off = true;
        // block 1
        bool auto_reset = false, vsc = false;
        int key = 0;                                   // half tones, -8..+8
        int key_db = 0;     bool key_off = false;
        bool mic_volume = false, delay_mute = false;
        // block 2
        bool vocal_support = false;
        int phase_shifter = 0;                         // 0 off, 1 min, 2 typ, 3 max
        int treble_db = 0, bass_db = 0;                // -12..+12
        // block 3
        int source = 0;                                // 0 stereo, 1 L, 2 R, 3 vocal cut
        bool delay_sum = false;                        // the delay block fed (L+R)/2 (else L-R)
        bool key_bypass = true, key_mixing = false;
        bool delay_mix = false, delay_reverse = false;
        int scoring = 0;
        bool voice_key = false;
    };

    explicit M65851(int rate = 44100);

    // A word as the program sends it (bit 15 = D0). false: not this chip's address (ignored).
    bool write(uint16_t word);
    const Settings &settings() const { return s_; }
    int delay_ms() const { return s_.surround ? s_.surround_ms : s_.echo_ms; }

    // false: process() would leave the sound exactly as it is (the caller can skip it)
    bool active() const;
    void process(float *lr, int n);   // stereo frames, interleaved, in place
    void reset();                     // as at power-on: everything off

    static uint16_t word_of(int block, uint16_t d2_d13);   // for tests: a word from its fields
    // false when HK_M65851=0: the board leaves the chip out (read each call; the board reads it once)
    static bool enabled();

private:
    struct Biquad {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        float z[2][2] = {};
        float run(float x, int c) {
            const float y = b0 * x + z[c][0];
            z[c][0] = b1 * x - a1 * y + z[c][1];
            z[c][1] = b2 * x - a2 * y;
            return y;
        }
        void clear() { z[0][0] = z[0][1] = z[1][0] = z[1][1] = 0; }
    };
    bool key_wanted() const { return !s_.key_bypass && s_.key != 0; }
    bool delay_wanted() const { return s_.delay_mix && !s_.delay_off && !s_.delay_mute; }
    bool tone_wanted() const { return s_.bass_db != 0 || s_.treble_db != 0; }
    void settle();   // coefficients and targets from s_
    void start();    // lines and filters empty

    int rate_;
    Settings s_;
    bool running_ = false;
    // key control
    std::vector<float> kline_[2];
    unsigned kat_ = 0;
    uint32_t kphase_ = 0;
    int32_t kstep_ = 0;
    int kwindow_ = 2048;
    float kgain_ = 1, kmix_ = 0, kmix_target_ = 0;
    // delay block
    std::vector<float> dline_;
    unsigned dat_ = 0;
    int dlen_ = 0;
    Biquad din_, dout_;
    float fb_ = 0, dgain_ = 0, dsign_ = 1;
    float dfade_ = 0, dfade_target_ = 0;   // the delay's output faded in and out
    // tone
    Biquad bass_, treble_;
    bool tone_ = false;
    float glide_;   // per sample, 10 ms from 0 to 1
};

}  // namespace hk::twinkle
