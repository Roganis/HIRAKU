// SPDX-License-Identifier: GPL-3.0-or-later
// A new release, said for a few seconds on the game's window when the game was started on its
// own (the launcher says it itself, and tells its games so: HK_LAUNCHER). Only in a release
// package (its VERSION file, its tools/update.py), and not when the launcher's Settings >
// Launcher turned the check off. The last check's answer (tools/update.py check --save, the
// launcher's or a game's: update-check.json in the saves' folder) stands for 6 hours; else the
// game asks again in the background, with the package's Python. The banner shows only if the
// answer is there within the game's first 15 seconds, and never changes what the game computes.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hk::host::notice {

void start(const std::string &root);   // once the window is open

// A setup problem (Core::setup_warning), shown first, for `seconds`, edged in red
void warn(const std::vector<std::string> &lines, double seconds = 15);

// The banner over this frame: false when there is none. pixels: ARGB, w x h (SDL's 8x8 font),
// the same while one banner shows; alpha: 1, fading to 0 at its end.
struct Banner {
    const std::vector<uint32_t> *pixels;
    int w, h;
    float alpha;
    unsigned id;   // changes with the picture
};
bool banner(Banner &b);

// A hint from the game's core (any thread), shown after what is on screen, for `seconds`, edged
// in amber
void hint(const std::vector<std::string> &lines, double seconds = 12);

}  // namespace hk::host::notice
