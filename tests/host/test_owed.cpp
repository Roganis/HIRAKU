// SPDX-License-Identifier: GPL-3.0-or-later
// A PC game's picture that comes while it owes frames (runtime/pc/owed.h): made up when its frame
// was quicker than a period (a game catching up), shown when it took a period or more (a game that
// can't keep its rate would otherwise never show a picture)
#include "pc/owed.h"

#include <cstdio>

static int fails;
static void expect(double frame_ms, double period_ms, bool want) {
    const bool show = hk::pc::show_owed_picture(frame_ms, period_ms);
    if (show != want) {
        fprintf(stderr, "show_owed_picture(%g ms, period %g ms): %d, expected %d\n", frame_ms, period_ms, show, want);
        fails++;
    }
}

int main() {
    // catching up after a hold-up: frames drawn at once, made up
    expect(0.5, 1e3 / 120, false);
    expect(2.0, 1e3 / 120, false);
    expect(5.0, 1e3 / 60, false);
    expect(8.0, 1e3 / 120, false);   // just under the period: still a gain
    // a game slower than its rate (a 64-bit PC IIDX's music select: 11 ms a frame at 120 Hz): shown
    expect(11.0, 1e3 / 120, true);
    expect(1e3 / 120, 1e3 / 120, true);
    expect(17.0, 1e3 / 60, true);
    expect(5.0, 1e3 / 240, true);
    // no rate known: as before
    expect(50.0, 0, false);
    if (fails) return 1;
    puts("owed pictures: all as expected");
    return 0;
}
