// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// A [[rate]] (docs/MODS.md) on a PC that can't keep it: a game asked to draw at the display's rate
// whose pictures take longer than a refresh falls behind without end (each late frame drawn at once
// after, its notes kept with its music, the display showing pictures again). The rate steps down
// instead, to the display's rate divided by a whole number, so every picture still holds the same
// number of refreshes: 240 Hz -> 120, 80, 60; 144 -> 72; never under the cabinet's own rate.

namespace hk::host {

// The divisor (1, 2, 3...) for a rate of `hz` whose game takes `picture_ms` for a picture: the
// smallest whose period holds the picture within `fit` of it, hz / k never under `floor_hz` (the
// cabinet's rate, a little under it allowed: 119.88 / 2); the largest allowed when none does. 1
// when nothing is known (no time measured, no rate).
inline int rate_divisor(double hz, double picture_ms, double floor_hz, double fit = 0.75) {
    if (!(hz > 0) || !(picture_ms > 0) || !(floor_hz > 0)) return 1;
    int k = 1;
    while (hz / (k + 1) >= floor_hz - 0.5 && picture_ms > fit * 1000.0 * k / hz) k++;
    return k;
}

}  // namespace hk::host
