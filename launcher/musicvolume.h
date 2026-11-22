// SPDX-License-Identifier: GPL-3.0-or-later
// The Music dock's volume (launcher/music.cpp), its rules apart from the window so they can be
// tested alone (tests/launcher/test_music_volume.cpp).
//
// The volume is heard the same way whichever synth plays: hiraku-midi scales the card's and the
// SoundFont's sound by (v/100)^2, and Windows' synth's parts' volumes by v/100, which General MIDI
// synths hear as its square (runtime/midi/part_volume.h). So 45 is 14 dB under 100, 10 is 40 dB
// under, and 1 is silence.
#pragma once

#include <algorithm>
#include <cmath>
#include <optional>

namespace musicvolume {

inline constexpr int kDefault = 45;   // the slider's middle, as a new launcher has it
inline constexpr int kLow = 20;       // under this, a volume saved before the check is set back

// A volume saved while the slider did nothing (Windows, before 0.12.0) is heard since: a player who
// dragged it down then found silence later. Once per settings file (`checked`: the key that says it
// was done), a saved volume under kLow goes back to kDefault; after that the player's own choice is
// kept, a low one too.
struct Start {
    int volume;    // the slider's value
    bool reset;    // set back now: the dock says so, once
};
inline Start start(std::optional<int> saved, bool checked) {
    const int v = std::clamp(saved.value_or(kDefault), 0, 100);
    if (!checked && saved && *saved < kLow) return {kDefault, true};
    return {v, false};
}

// What the 16 parts' meters are multiplied by at volume v: the song's own levels (hiraku-midi's
// "levels" lines) as the volume leaves them heard. The sound falls 40 log10(v/100) dB; the meters
// show it as a 48 dB window, so a part at full drops about one of its seven segments per 7 dB:
// 100 as the song has them, 45 at 0.71 (still lively), 20 at 0.42, 10 at 0.17 (a segment or so),
// nothing at 6 and under (-48 dB and lower: silence in an arcade, digital silence at 1).
inline constexpr double kMeterRangeDb = 48;
inline float meterScale(int volume) {
    if (volume <= 0) return 0;
    if (volume >= 100) return 1;
    const double db = 40 * std::log10(volume / 100.0);
    return (float)std::clamp(1 + db / kMeterRangeDb, 0.0, 1.0);
}

}  // namespace musicvolume
