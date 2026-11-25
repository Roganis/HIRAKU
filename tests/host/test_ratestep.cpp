// SPDX-License-Identifier: GPL-3.0-or-later
// A [[rate]] stepped down on a PC that can't keep it (runtime/host/ratestep.h): always a whole
// fraction of the rate asked, never under the cabinet's rate, the smallest step that fits
#include "host/ratestep.h"

#include <cstdio>
#include <initializer_list>

static int fails;
static void expect(double hz, double ms, double floor_hz, int want) {
    const int k = hk::host::rate_divisor(hz, ms, floor_hz);
    if (k != want) {
        fprintf(stderr, "rate_divisor(%g Hz, %g ms, floor %g): %d, expected %d\n", hz, ms, floor_hz, k, want);
        fails++;
    }
}

int main() {
    // a game that keeps up: the rate as asked
    expect(240, 1.0, 59.94, 1);
    expect(240, 3.1, 59.94, 1);   // within 3/4 of 4.17 ms
    // ... and one that doesn't: the smallest step whose period holds it
    expect(240, 3.2, 59.94, 2);   // 120 Hz
    expect(240, 6.0, 59.94, 2);
    expect(240, 7.0, 59.94, 3);   // 80 Hz
    expect(240, 11.0, 59.94, 4);  // 60 Hz (the owner's 2026-11-22: 11 ms a picture)
    // never under the cabinet's rate, however long the pictures take
    expect(240, 40.0, 59.94, 4);
    expect(144, 11.0, 59.94, 2);  // 72 Hz: 48 would be under 60
    expect(144, 5.0, 59.94, 1);
    expect(120, 7.0, 59.94, 2);
    expect(119.88, 7.0, 59.94, 2);   // 59.94: the cabinet's own
    expect(75, 20.0, 59.94, 1);      // 37.5 would be under: kept
    expect(60, 20.0, 59.94, 1);
    // nothing known: as asked
    expect(240, 0, 59.94, 1);
    expect(0, 11.0, 59.94, 1);
    // the step always divides the rate exactly
    for (double hz : {120.0, 144.0, 165.0, 240.0, 360.0})
        for (double ms = 0.5; ms < 30; ms += 0.5) {
            const int k = hk::host::rate_divisor(hz, ms, 59.94);
            if (k < 1 || hz / k < 59.44) {
                fprintf(stderr, "rate_divisor(%g Hz, %g ms): %d goes under the cabinet's rate\n", hz, ms, k);
                fails++;
            }
        }
    if (!fails) printf("rate steps: all as expected\n");
    return fails ? 1 : 0;
}
