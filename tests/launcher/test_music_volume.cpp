// SPDX-License-Identifier: GPL-3.0-or-later
// The Music dock's volume (launcher/musicvolume.h): a volume saved while the slider did nothing set
// back once per settings file, and the meters scaled by the volume as the sound is.
//
//   test_music_volume     Exit 0 when all hold
#include <cstdio>
#include <optional>

#include "musicvolume.h"

namespace {

int failed = 0;
void check(bool ok, const char *what) {
    printf("%s%s\n", ok ? "ok    " : "FAIL  ", what);
    failed += !ok;
}

}  // namespace

int main() {
    using musicvolume::meterScale;
    using musicvolume::start;
    const std::optional<int> none;

    // the first start with the check: what is saved, the key not yet written
    check(start(none, false).volume == 45 && !start(none, false).reset, "nothing saved: the middle, nothing said");
    check(start(0, false).volume == 45 && start(0, false).reset, "0 saved: back to 45, said");
    check(start(10, false).volume == 45 && start(10, false).reset, "10 saved: back to 45, said");
    check(start(19, false).volume == 45 && start(19, false).reset, "19 saved: back to 45, said");
    check(start(20, false).volume == 20 && !start(20, false).reset, "20 saved: kept");
    check(start(45, false).volume == 45 && !start(45, false).reset, "45 saved: kept");
    check(start(100, false).volume == 100 && !start(100, false).reset, "100 saved: kept");
    // the key written: the player's own choice kept for good, a low one too
    bool kept = true;
    for (int v : {0, 10, 19, 20, 45, 100}) kept = kept && start(v, true).volume == v && !start(v, true).reset;
    check(kept, "checked before: 0, 10, 19, 20, 45, 100 kept as they are, nothing said");
    check(start(none, true).volume == 45 && !start(none, true).reset, "checked, nothing saved: the middle");
    // the second start after a reset: 45 saved and the key there, nothing changes
    const auto first = start(3, false), second = start(first.volume, true);
    check(first.reset && second.volume == 45 && !second.reset, "the second start after a reset changes nothing");
    check(start(250, true).volume == 100 && start(-4, true).volume == 0, "a value off the slider's ends: to its ends");

    // the meters
    check(meterScale(0) == 0.0f, "volume 0: the meters show nothing");
    check(meterScale(100) == 1.0f, "volume 100: the song's own levels");
    bool up = true;
    for (int v = 1; v <= 100; v++) up = up && meterScale(v) >= meterScale(v - 1);
    check(up, "the meters never fall as the volume rises");
    check(meterScale(45) > 0.65f, "45 (the default): still lively");
    check(meterScale(10) > 0.0f && meterScale(10) < 0.25f, "10: clearly low");
    check(meterScale(6) == 0.0f && meterScale(1) == 0.0f, "6 and under (-48 dB and lower): nothing");
    printf("%s\n", failed ? "FAILED" : "all ok");
    return failed ? 1 : 0;
}
