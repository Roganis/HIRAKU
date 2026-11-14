// SPDX-License-Identifier: GPL-3.0-or-later
// A game's picture for the game menu (runtime/menu/menu.h): its title screen, taken from the game
// as the player's own machine runs it and kept in the player's own folder. HIRAKU comes with none:
// a title screen is the game's.
//
// --menu-picture FILE has the shell write one frame there, once the machine has run for the game's
// title_seconds (its game.toml; --menu-picture-at S instead): the launcher asks for it while the
// file isn't there yet. Not if a control was pressed before then (a player already past the title),
// and not a blank frame or a screen of text (a new cabinet's start-up error): the next run tries again.
#pragma once

#include <cstdint>
#include <string>

namespace hk::host::menupic {

constexpr int kLines = 240;   // the picture's height; its width is the monitor's shape's
constexpr int kColours = 8;   // fewer colours than this for 97 % of it: text, not a picture

// The frame (0x00RRGGBB, w x h) at the monitor's shape (width over height; 0: square pixels),
// scaled to kLines and written as a 24-bit BMP, whole or not at all, its folder made if need be.
// false if the frame is blank or a screen of text (nothing to show), or the file can't be written.
bool save(const std::string &path, const uint32_t *rgb, int w, int h, double aspect);

}  // namespace hk::host::menupic
