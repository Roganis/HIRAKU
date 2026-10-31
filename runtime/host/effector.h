// SPDX-License-Identifier: GPL-3.0-or-later
// A cabinet's sound effects, applied to a game's sound as its sound driver would: beatmania IIDX's
// effector (GOLD to EMPRESS set it in the cabinet PC's Realtek driver: runtime/pc/effector.c).
//
// What the game sets: an effect (a program of the effector's, below: the game's environment, as
// the driver numbers it, made out by the cabinet's DLL), a key shift (semitones, -4 to 4: the
// pitch changed, the tempo not), a 10-band equaliser (31 Hz to 16 kHz, -12 to 12 dB each) and the
// volume. The sound goes through the key shift, then the effect, the equaliser and the volume; each
// change comes in smoothly (no clicks), an effect turned off rings out. With nothing set the sound
// goes through untouched, with no delay: the key shift alone delays it (13 ms on average up, 21 ms
// down), while it's on.
#pragma once

#include <cstdint>
#include <memory>

namespace hk::host {

class Effector {
public:
    // The programs: what the games' effectors call ECHO 1-5 and REVERB 1-3, and for the driver's
    // other environments (named after places: a bathroom, a concert hall, mountains), a kind of
    // place. The DLLs number them the same (runtime/pc/effector.c)
    enum Program {
        NONE = 0,
        ECHO_1, ECHO_2, ECHO_3, ECHO_4, ECHO_5,
        REVERB_1, REVERB_2, REVERB_3,
        SMALL_ROOM, ROOM, HALL, OUTDOORS, UNDERWATER,
        PROGRAMS
    };
    struct Settings {
        int program = NONE;
        int key = 0;
        bool eq_on = false;
        int8_t eq_db[10] = {};
        float volume = 1;   // the whole sound's (the effector's play volume)
    };

    explicit Effector(int rate);
    ~Effector();

    void set(const Settings &s);
    // stereo frames, interleaved, in place
    void process(float *lr, int frames);
    static const char *program_name(int program);   // for the log ("REVERB 3")

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace hk::host
