// SPDX-License-Identifier: GPL-3.0-or-later
// HIRAKU's logo opening and closing ("hiraku", 開く, is "to open"): a line grows in the middle and
// opens into a black box, the name uncovered inside it as it widens; closing, the box shuts over the
// name into a line that shrinks away. Made in AniCraft (tools/data/logo/), baked by
// tools/logoanim.py into runtime/menu/logo.anim, which the menu carries inside itself.
//
// The pictures are the box's alone, 501x145 (a third of the 1920x1080 they were made at), grey: two
// bytes a pixel, how light and how opaque. Around the box nothing is drawn. Opening ends on the box
// standing still, and closing starts from that same picture.
#pragma once

#include <cstdint>

namespace hk::menu::logo {

enum Anim { opening = 0, closing = 1 };

// The file read (once, at the first call); false if it couldn't be, and then there is no logo
bool ready();
int width();
int height();
int frames(Anim a);       // 105 and 54, at 60 a second
int still_from(Anim a);   // the first frame of the ones at its end that are all the same
// Frame n's picture (n past the end: the last), width x height x (light, opaque)
const uint8_t *frame(Anim a, int n);

}  // namespace hk::menu::logo
