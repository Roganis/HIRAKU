// SPDX-License-Identifier: GPL-3.0-or-later
// The player's volume for a synth that plays on its own (Windows' General MIDI synth), put into
// each part's own volume controller as the messages go by.
//
// Such a synth is not mixed here, so its loudness has to be asked of it. Windows' midiOutSetVolume
// is the device's volume where the device has one: through the MIDI mapper on Windows 10 it did
// nothing (the launcher's slider on the IIDX cabinet, 2026-11-15). What every General MIDI synth
// has is controller 7, a part's volume, which a song sets as it likes: so each one a song sends is
// scaled on its way, the last one asked for is remembered for every part, and they are all sent
// again when the player's volume changes or the synth is told to start afresh (its parts' volumes
// are then back at 100). A part's loudness goes as the square of its controller (General MIDI's
// recommended practice), so a gain g of amplitude is the controller times the square root of g.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace hk::midi {

class PartVolume {
    std::array<uint8_t, 16> asked_;   // each part's volume as the song last set it
    float scale_ = 1;

public:
    PartVolume() { asked_.fill(100); }

    // the player's volume, as a gain of amplitude (0 to 1): every part is to be sent again (`part`)
    void set(float gain) { scale_ = std::sqrt(std::clamp(gain, 0.0f, 1.0f)); }
    // part ch's volume as the synth should have it now
    uint8_t part(int ch) const { return (uint8_t)std::min(127L, std::lround(asked_[(size_t)ch & 15] * scale_)); }

    // A message on its way: a part's volume is noted, and `value` is what to send as its third
    // byte instead (true). Else false.
    bool volume(const uint8_t *b, size_t n, uint8_t &value) {
        if (n != 3 || (b[0] & 0xf0) != 0xb0 || b[1] != 7) return false;
        asked_[b[0] & 15] = b[2] & 0x7f;
        value = part(b[0] & 15);
        return true;
    }
    // A SysEx on its way that starts the synth afresh (General MIDI on, GS reset, XG on): its
    // parts' volumes are back at 100, and all are to be sent again after it.
    bool afresh(const uint8_t *b, size_t n) {
        const bool gm = n == 6 && b[0] == 0xf0 && b[1] == 0x7e && b[3] == 0x09 && (b[4] == 0x01 || b[4] == 0x03);
        const bool gs = n == 11 && b[0] == 0xf0 && b[1] == 0x41 && b[3] == 0x42 && b[4] == 0x12 && b[5] == 0x40 && b[6] == 0x00 && b[7] == 0x7f && b[8] == 0x00;
        const bool xg = n == 9 && b[0] == 0xf0 && b[1] == 0x43 && b[3] == 0x4c && b[4] == 0x00 && b[5] == 0x00 && b[6] == 0x7e && b[7] == 0x00;
        if (!gm && !gs && !xg) return false;
        asked_.fill(100);
        return true;
    }
};

}  // namespace hk::midi
