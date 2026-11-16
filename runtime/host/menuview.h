// SPDX-License-Identifier: GPL-3.0-or-later
// The box over the picture while the way out to the game menu is held (host/menukey.h): what the
// hold does, and a bar of how far it is. Nothing while nothing is held.
#pragma once

#include <cstdint>
#include <vector>

namespace hk::host::menuview {

// The box over this frame: false when there is none. pixels ARGB, w x h (as host/warp.h's)
struct Panel {
    const std::vector<uint32_t> *pixels;
    int w, h;
    unsigned id;   // changes with the picture
    double scale = 1;   // drawn at this many times a 360-line screen's size (host/uibox.h)
};
bool panel(Panel &p);

}  // namespace hk::host::menuview
