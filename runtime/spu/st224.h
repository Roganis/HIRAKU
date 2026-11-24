// SPDX-License-Identifier: GPL-3.0-or-later
// A stand-in for beatmania III's outboard effects unit (an ST-224 sampler with effects, used for its
// effects only), driven by what the game sends it on the extend board's MIDI port and applied where
// the unit sat: on the RF5C400's effect outputs, which the game turns the music (and, by the
// effector's ELEMENT switch, the keys) over to while an effect is on.
//
// What the game sends (all five games the same, one MIDI channel, nothing read back), and what it
// means to the unit (per the unit's manual):
//   at start-up  B0 79 00, B0 7B 00, B0 78 00: controllers reset, notes and sounds off;
//                A0 6C 7F: the SOURCE pad (the unit's input) through the effect;
//                90 23 7F: the SOURCE pad held down (its input passes only while it is); then the
//                effect below, off
//   an effect    C0 prog (the effect type), B0 0C a / B0 0D b (its two parameters, 0-100),
//                A0 6D level (the SOURCE pad's level, 0-99: the game's per effect); a value only
//                when it changed, all three again every 30 frames while an effect is on
//   on / off     B0 50 7F / B0 50 00 (the pedal; another player's pedal: that player's effect)
// The unit's effect tempo (CC 14, 15) and tempo sync (CC 81) are kept but change nothing: the game
// never sends them, and sync stays off as a reset leaves it. Notes other than 35 would play the
// unit's own samples: there are none here.
//
// What each parameter does follows the unit's manual; the ranges where it gives none, and how each
// effect is made, are HIRAKU's (no recording of the unit at hand), in st224.cpp's PROGRAMS. While no
// effect is heard (off, or the SOURCE pad not held or not through the effect, or before anything is
// sent) the sound is left exactly as it is (active() false: the caller skips process()); the unit
// would pass the input dry at the pad's level, but the game sends nothing to it then (the music
// is back on the main outputs). An effect fades in and out over 10 ms, a new program by fading the
// old one out first; parameter moves glide (20 ms).
//
// Deterministic: the state moves only with the bytes and the samples processed. process() is the
// same however the sound is cut into calls (the controls move on every 32nd sample counted from the
// start, not per call).
#pragma once

#include <cstdint>
#include <memory>

namespace hk::spu {

class St224 {
public:
    struct State {
        bool on = false;          // CC 80: >= 64 on
        int program = 0;          // program change: the unit's effect types 0-21
        int a = 0, b = 0;         // CC 12, CC 13 (the game sends 0-100; above 100 taken as 100)
        int level = 49;           // poly pressure on note 109: the SOURCE pad's level, 0-99
        bool pad_effect = false;  // poly pressure on note 108: the SOURCE pad through the effect
        bool source = false;      // note 35 on (off): the SOURCE pad held
        int bpm = 120;            // CC 14, 15: kept, not used
        bool sync = false;        // CC 81: kept, not used
        uint64_t bytes = 0, messages = 0, unknown = 0;
    };

    explicit St224(int rate = 44100);
    ~St224();

    void midi(uint8_t byte);           // a byte from the game's MIDI transmitter
    bool active() const;               // false: process() would leave the sound exactly as it is
    void process(float *lr, int n);    // stereo frames, interleaved, in place
    void reset();                      // as at power-on

    const State &state() const;
    static const char *program_name(int program);   // "REVERB"; "" when the game has no such program
    static bool known(int program);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace hk::spu
