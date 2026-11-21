// license:BSD-3-Clause
// copyright-holders:R. Belmont, Olivier Galibert
//
// Yamaha XT446, Keyboardmania's synth card: an embedded MU100B (H8S/2655 CPU +
// SWP30 tone generator/effects). Ported from MAME's xt446.cpp; the CPU and the
// SWP30 are MAME's own sources (third_party/mame) on a small framework shim.
// Modified for HIRAKU: the card runs on its own clock, one 44.1 kHz sample at a
// time, as the owner pulls audio; MIDI bytes go onto the SCI0 receive line bit by
// bit (31250 baud, 8N1) from the sample where they arrive.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace hk {

class Xt446 {
public:
    Xt446();
    ~Xt446();

    // The MAME ROM set's files (xu50710-m27c160.bin, the six wave ROMs, and sin-table.bin,
    // which is rebuilt when an older set lacks it) from dir. False (with a message in err)
    // if any other is missing.
    bool load(const std::string &dir, std::string &err);
    void midi(uint8_t byte);                          // the next byte on the MIDI line, sent now
    void render(float *lr, int frames, float gain);   // add 44.1 kHz stereo (full scale = 1.0)
    void take(float *lr, int frames);                 // the same samples, written rather than added

    // The SWP30's effect programs as made into the host's code (xt446_meg.cpp) against their
    // interpreter, on programs and states drawn from seed, with no ROM: "" when they agree,
    // "skip: ..." where this build makes no code, else what differed (tests/jit).
    static std::string meg_selftest(unsigned seed, int rounds);
    // The H8 as made into the host's code (xt446_h8jit.cpp) against MAME's interpreter: two cards
    // running made-up programs (seed onwards), compared after every sample. "" when they agree,
    // "skip: ..." where this build makes no code, else what differed (tests/jit).
    static std::string h8_selftest(unsigned seed, int programs, int samples);

    // statistics
    uint64_t samples() const;
    uint64_t midi_bytes() const;
    double cpu_seconds() const;                       // host time spent emulating

private:
    void produce(int frames, float *lr, float gain, bool add);
    struct Impl;
    std::unique_ptr<Impl> m;
};

}  // namespace hk
