// SPDX-License-Identifier: GPL-3.0-or-later
// Windows: a sound card in WASAPI exclusive mode (--audio-exclusive), as --audio-device opens one
// through ALSA on Linux: no system mixer in between, the device's own buffer as small as it allows.
// HIRAKU's thread (MMCSS "Pro Audio") fills it on the device's event from an SDL audio stream that
// converts the game's sound to whatever the device takes (float, 32, 24 or 16 bits; the game's
// rate, else 48 or 44.1 kHz).
#pragma once

#ifdef _WIN32
#include <SDL3/SDL.h>

#include <cstdint>
#include <string>

namespace hk::host::wasapi {

// `device` (its name as Windows shows it, "" for the default output) alone, periods of about
// `frames` frames at the game's rate: `stream` converts `src` to it. "" when playing, else why not
// (the device in use by another program in exclusive mode, or one that has no exclusive mode).
std::string open(const std::string &device, const SDL_AudioSpec &src, int frames, SDL_AudioStream *&stream,
                 std::string &what);
void close();
bool active();
// Windows took the device back (unplugged, turned off, its format changed in the Sound settings):
// nothing more is played on it, and the sound's stream is no longer emptied. Until close()
bool lost();
int period_frames();   // the device's buffer, at the device's rate
// For the report: frames the device took against its rate since the start, and the times the
// stream had less than a period ready
std::string describe();

}  // namespace hk::host::wasapi
#endif
