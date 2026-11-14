// SPDX-License-Identifier: GPL-3.0-or-later
// The way out of a game to the game menu (runtime/menu/, the launcher's loop), from the cabinet
// itself: 5, 7 and 3 of an e-AMUSEMENT reader's 10-key held together (573: either player's, the
// three on the same 10-key), or the Test and Service switches held together on a cabinet with no
// 10-key, or a key or button of the player's choice (--game-menu-key), for a couple of seconds. A
// box on the picture says so while they are held (host/menuview.h). The game then ends as a closed
// window ends it, its saves written, and its program's exit code is EXIT_CODE: the launcher shows
// the menu; a game started on its own just ends.
//
// The inputs are watched as they go to the game, by the cabinet's names for them, whatever the
// game: one with no 10-key of its own (a DJ Main or Twinkle beatmania on an IIDX cabinet) still
// leaves by the readers'. The game gets them too (the digits at a PIN prompt, a credit from Service).
#pragma once

#include <string>

namespace hk::host::menukey {

constexpr int EXIT_CODE = 73;
// Its name here for the player's own key or button (the window gives its presses, host/window.cpp)
constexpr const char *OWN_KEY = "\x01menu key";

// Off until this: the holds are watched, and how long they take (seconds)
void set(bool on, double hold_seconds = 2.0);
bool on();
// An input as the game is given it (any thread): its cabinet name, 1 held, 0 let go
void input(const std::string &name, int value);
// Whether any control at all was given pressed since the program started (the menu's picture of a
// game is taken only from one nobody has touched yet: host/menupic.h)
bool touched();
// Once a frame, with the time (now()): true once, when one of the holds has lasted
bool frame(double now_seconds);
// How far the hold is, 0 to 1, for the box; 0 while there is none or it has only just begun
// (a touch of the three keys shows nothing)
double progress(double now_seconds);
double now();   // seconds on the steady clock

}  // namespace hk::host::menukey
