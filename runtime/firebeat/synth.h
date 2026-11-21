// SPDX-License-Identifier: GPL-3.0-or-later
// Keyboardmania's XT446 synth card (an embedded Yamaha MU100B), fed the game's
// MIDI stream in time order:
// - the card itself (dev/xt446: MAME's H8S/2655 + SWP30 running the MU100
//   firmware) when its ROMs are in the game's rom directory;
// - otherwise a stand-in: FluidSynth with a General MIDI SoundFont (the notes,
//   timing and controllers are the game's, the instruments whatever the font has).
//   Built only when FluidSynth is found.
#pragma once

#include <cstdint>
#include <string>

namespace hk::synth {

// rom_dir: the MAME set's files (XT446 program, wave ROMs, sin table).
// soundfont: for the stand-in, a .sf2 file, or "" to search $HK_SOUNDFONT,
// /usr/share/soundfonts and user_dir (shell/userdir.h: ~/.local/share/hiraku, or a portable
// install's user/data). stand_in forces it. Returns false (and stays silent) with neither.
bool init(const std::string &rom_dir, const std::string &soundfont, bool stand_in, const std::string &user_dir);
bool active();
void midi(uint8_t byte);                       // one byte of the MIDI stream, in time order
void render(float *lr, int frames, float gain);   // add 44.1 kHz stereo
// The XT446 runs on a thread of its own (HK_XT446_INLINE=1: not). Then the
// board asks for samples with request() as it goes, in the same order as its
// midi() calls, and adds them all to its own audio with collect() once a frame:
// collect(lr, n, gain) waits for the n samples requested since the last collect.
// collect_ready(lr, n, gain) adds, without waiting, those of the next n the card has
// already made, and says how many (the board hands its sound on as it comes, not a
// frame later); collect() then waits for the rest.
bool deferred();
void request(int frames);
void collect(float *lr, int frames, float gain);
int collect_ready(float *lr, int frames, float gain);
void stop();                                   // ends the thread (before describe() at exit)
std::string describe();

}  // namespace hk::synth
