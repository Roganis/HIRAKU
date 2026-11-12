// SPDX-License-Identifier: GPL-3.0-or-later
// Windows: a sound card through its ASIO driver (--audio-asio), as --audio-exclusive opens one in
// WASAPI exclusive mode: the card's own driver, no Windows audio in between, its buffer as the
// driver allows. The driver calls HIRAKU back on its own thread at each half of its double buffer,
// which is filled from an SDL audio stream that converts the game's sound to the card's rate; the
// samples are written in whatever form the driver asks (16, 24 or 32 bits, float).
//
// The driver interface is declared in asio.cpp from the ASIO specification (as Wine's pwasio does),
// without Steinberg's SDK files.
#pragma once

#ifdef _WIN32
#include <SDL3/SDL.h>

#include <string>
#include <vector>

namespace hk::host::asio {

// the installed ASIO drivers by name (the registry's HKEY_LOCAL_MACHINE\SOFTWARE\ASIO)
std::vector<std::string> drivers();

// `driver` (one of drivers(); "" for the first) at the game's rate if the card takes it (else 48,
// 44.1 or 96 kHz, else the card's own), buffers of about `frames` frames (0: the driver's own
// choice, from its control panel): `stream` converts `src` to it. "" when playing, else why not.
std::string open(const std::string &driver, const SDL_AudioSpec &src, int frames, SDL_AudioStream *&stream,
                 std::string &what);
void close();
bool active();
// The driver asked to be restarted (its settings were changed in its control panel, or its card
// went): close() then open() again, from the thread that opened it. Many drivers stop calling back
// until then.
bool restart_wanted();
int period_frames();   // the driver's buffer, at the card's rate
// For the report: frames the driver took against its rate, the times the sound came short, the
// driver's overloads and restart requests
std::string describe();

}  // namespace hk::host::asio
#endif
