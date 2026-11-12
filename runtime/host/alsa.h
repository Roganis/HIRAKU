// SPDX-License-Identifier: GPL-3.0-or-later
// A sound card of HIRAKU's own on Linux (--audio-device): written through ALSA by HIRAKU itself, a
// period at a time into a ring kept full (alsa.cpp says why), its card reserved from PulseAudio and
// PipeWire over D-Bus. libasound and libdbus are loaded at run time: no build dependency.
#pragma once

#include <cstdint>
#include <string>

struct SDL_AudioSpec;
struct SDL_AudioStream;

namespace hk::host::alsa {

// The card `device` ("hw:1,0"...) opened for `src`'s sound, `period` frames a period; `stream` is
// what the game's sound goes into, *what what was opened. "" if it worked, else why not
std::string open(const std::string &device, const SDL_AudioSpec &src, int period, SDL_AudioStream *&stream, std::string &what);
void close();
int period();            // the period the card took, in frames
// The card is gone (a USB card pulled out): nothing more is written to it, and the sound's stream
// is no longer emptied. Until close()
bool lost();
std::string describe();  // the card's pace and its writer, for the report ("" if none was opened)
// RealtimeKit over D-Bus: "" if `method` worked for thread `tid`, else why not
std::string rtkit(const char *method, uint64_t tid, bool realtime, int value);

}  // namespace hk::host::alsa
