// SPDX-License-Identifier: GPL-3.0-or-later
// The MIDI player's volume through each part's volume controller (runtime/midi/part_volume.h).
#include <cstdio>

#include "midi/part_volume.h"

int main() {
    int failed = 0;
    auto expect = [&](bool ok, const char *what) {
        if (!ok) printf("test_part_volume: FAILED: %s\n", what), failed++;
    };
    hk::midi::PartVolume v;
    uint8_t out = 0;
    // at full volume nothing changes
    const uint8_t cc7[] = {0xb3, 7, 90}, note[] = {0x93, 60, 100}, cc10[] = {0xb3, 10, 64};
    expect(v.volume(cc7, 3, out) && out == 90, "at full volume a part's volume goes as the song set it");
    expect(!v.volume(note, 3, out) && !v.volume(cc10, 3, out), "only controller 7 is a part's volume");
    expect(v.part(3) == 90 && v.part(0) == 100, "a part the song has not set is at 100");
    // half the slider: a gain of a quarter, the controller halved
    v.set(0.25f);
    expect(v.part(3) == 45 && v.part(0) == 50, "a gain of 1/4 is half the controller");
    const uint8_t louder[] = {0xb3, 7, 127};
    expect(v.volume(louder, 3, out) && out == 64, "a part's volume set after is scaled as it goes");
    v.set(0);
    expect(v.part(3) == 0 && v.part(9) == 0, "silent at 0");
    v.set(1);
    expect(v.part(3) == 127, "back at full, the song's own again");
    // a fresh start: every part back at 100
    const uint8_t gm[] = {0xf0, 0x7e, 0x7f, 0x09, 0x01, 0xf7};
    const uint8_t gs[] = {0xf0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7f, 0x00, 0x41, 0xf7};
    const uint8_t xg[] = {0xf0, 0x43, 0x10, 0x4c, 0x00, 0x00, 0x7e, 0x00, 0xf7};
    const uint8_t other[] = {0xf0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x11, 0x15, 0x02, 0x18, 0xf7};   // (GS: a part made drums)
    expect(!v.afresh(other, sizeof other) && v.part(3) == 127, "another SysEx changes nothing");
    expect(v.afresh(gm, sizeof gm) && v.part(3) == 100, "General MIDI on: every part at 100");
    v.volume(louder, 3, out);
    expect(v.afresh(gs, sizeof gs) && v.part(3) == 100, "GS reset: every part at 100");
    v.volume(louder, 3, out);
    expect(v.afresh(xg, sizeof xg) && v.part(3) == 100, "XG on: every part at 100");
    if (!failed) printf("test_part_volume: a part's volume carries the player's\n");
    return failed ? 1 : 0;
}
