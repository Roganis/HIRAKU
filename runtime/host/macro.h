// SPDX-License-Identifier: GPL-3.0-or-later
// Code macros: a game's button codes (hidden songs, modes, unlocks that last a credit or until
// power-off) typed for the player. games/<id>/macros.txt holds them:
//
//   button 1 "P1 Button 1"          numbers used by `keys` (default: N = "P1 Button N")
//   macro "LESSON (hidden song)"     one macro; Ctrl+1..9 in the window types the 1st..9th
//     text "Title screen, credits in: the hidden song LESSON."
//     at 3000                        optional: run by itself at this frame (power-on codes),
//                                    when the launcher asks for it (--macro NAME)
//     keys 94781362                  taps, in order: one digit each, or numbers apart (2 2 10 9)
//     press "P1 Button 5" [frames]   one tap (held `frames`, default 8)
//     hold "2 Players Start"         down until `release`
//     release "2 Players Start"
//     set "P2 Wheel" 255             an analog input
//     wait 30                        frames
//     tap 8 gap 16                   tap length and spacing for what follows (frames; the
//                                    default: the games miss shorter taps when they stall)
//   end
//
// A mod can bring a macros.txt of its own: its macros come after the game's, the mods' in their
// order (runtime/main.cpp, launcher/unlocksdialog.cpp: the same numbering for Ctrl+1..9).
//
// Steps run one after another, a tap every `gap` frames. Inputs go through core().set_input,
// like the input scripts', alongside the player's own.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hk::macro {

struct Info {
    std::string name, text;
    int64_t at = -1;    // the frame to run at by itself, or -1
};

// Load a game's macros (no file: none). Returns false on a parse error (printed).
bool load(const std::string &path);
const std::vector<Info> &list();
// Run macro `index` (0-based) from the next frame; false if there is none or one is running.
bool trigger(size_t index);
// Run a macro by name: at its own `at` frame, or at `frame` if given (NAME@FRAME); one due while
// another runs waits for it. False if unknown.
bool schedule(const std::string &spec);
// Run steps as a macro's (macros.txt's spelling, one a line, without `macro` and `end`) from the
// next frame, or after the one running: what HIRAKU types for the player (--auto-pin). Any thread.
// False if they don't parse (printed).
bool post(const std::string &name, const std::string &steps);
// Call once per frame (from the frame hook).
void frame(uint64_t f);
// Told when a macro starts ("" when it ends): the window shows it in its title.
void on_change(std::function<void(const std::string &)> cb);

}  // namespace hk::macro
