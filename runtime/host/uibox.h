// SPDX-License-Identifier: GPL-3.0-or-later
// The boxes over a game's picture (a notice: host/notice.h; the hold to the game menu:
// host/menuview.h; the fast start-up: host/warp.h) in the launcher's look, when the launcher
// started the game and said how:
//     HK_UI_LOOK   its colours (shell/uilook.h)
//     HK_UI_FONT   its fonts, drawn for the screen (menu/font.h: small, a title, a heading), with
//                  the boxes' icons as characters of their own (Icon below)
//     HK_UI_LOGO   HIRAKU's name, white on black (a BMP), as high as the heading
//     HK_UI_SCALE  how many times a 360-line screen's size the fonts were drawn (1-8; 1.333 for 480)
// Each box is drawn at that size: rounded, on the look's ground, the six colours on its bars.
// Without them (a game started on its own) the boxes stay SDL's 8x8 font on dark, scale 1.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "shell/uilook.h"

namespace hk::host::ui {

// The icons in the font file: Material Design Icons (third_party/mdi), drawn by the launcher at
// each size's height as the characters U+F0000 on (launcher/menufont.h)
enum class Icon : uint32_t {
    none = 0,
    info = 0xF0000,   // information
    alert,            // alert-circle: a setup problem
    update,           // download: a new release
    picture,          // camera: a picture kept for the game menu
    sound,            // volume-off: the sound device
    card,             // card-account-details: a card, e-AMUSEMENT
    disk,             // floppy: a disk
    dialpad,          // dialpad: the 10-key's hold
    fast,             // fast-forward: the fast start-up
    menu,             // view-list: the game menu
};
constexpr int kIcons = 10;   // info .. menu
const char *icon_name(Icon i);   // its MDI name ("alert-circle"), "" for none

// A box's picture: ARGB (straight alpha), w x h, drawn at `scale` times a 360-line screen's size
struct Image {
    std::vector<uint32_t> px;
    int w = 0, h = 0;
    double scale = 1;
};

// The launcher's look and fonts were given (read once, at the first call)
bool styled();
double scale();
const hk::ui::Look &look();
// An icon's colour in this look (each its own of the six)
uint32_t icon_colour(Icon i);

// A notice: its icon in its colour, its first line as the title, the others under it
void notice_box(Image &out, Icon icon, const std::vector<std::string> &lines);
// A box with a heading (HIRAKU's name before it, or an icon), a line with an icon before it, a bar
// filled `done` of the way (0..1) in the six colours, and a last line under it ("" none)
void bar_box(Image &out, bool logo, Icon heading_icon, const std::string &heading, Icon line_icon, const std::string &line,
             double done, const std::string &foot);

// (tests: the look and fonts as if from the environment; font "" takes them away)
bool set_for_test(const std::string &look, const std::string &font, const std::string &logo, double scale);

}  // namespace hk::host::ui
