// SPDX-License-Identifier: GPL-3.0-or-later
// The launcher's look as its programs draw it themselves: the game menu (hiraku-menu --look) and
// the boxes over a game's picture (host/uibox.h, HK_UI_LOOK). The launcher writes it
// (launcher/theme.h: menuLook): "name=#rrggbb,..." with
//     ground, foot   the ground, fading to the foot down the screen
//     ink, dim       text, and its quieter shade
//     line           lines, and the empty part of a bar
//     bar, point     the chosen row's bar; arrows and the like
//     stripe         the six colours, colons between ("#2457e8:#17b24f:..."); none: a line
//     round          how round a name's pill is, in pixels (0-40)
// A colour not given keeps the one of the look it is like: dark (the defaults, amber on night
// blue) or light, by its ground.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hk::ui {

struct Look {
    uint32_t ground = 0x0a0d1c, foot = 0x141a38, ink = 0xe8ecf4, dim = 0x8088a4, line = 0x3a4060, bar = 0xffb830, point = 0xffb830;
    std::vector<uint32_t> stripe;
    int round = 6;
    bool parse(const std::string &text, std::string &error);
};

bool light(uint32_t rgb);                                // a colour a light ground would be (dark text on it)
bool colour(const std::string &text, uint32_t &rgb);     // "#rrggbb" as 0xrrggbb
uint32_t mix(uint32_t a, uint32_t b, int t);             // `t` 256ths of the way from a to b

}  // namespace hk::ui
