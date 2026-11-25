// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// A PC game's picture that comes while it owes frames (core.cpp's frame loop: frames shown again
// while it was late, made up by not showing its next pictures and telling it to go on at once).
// Making up works only with frames quicker than a period: a picture whose own frame (from its go
// to its arrival) took a whole period or more made nothing up (a frame was shown again while it
// was drawn), and a game whose every frame takes that long would never show a picture again. Such
// a picture is shown as it comes; the frames still owed are made up later if the game gets quicker,
// or given up with the other pauses' (MAX_OWED). The catch-up of a game that is fast (its frames
// drawn at once, far under a period) is as it was.

namespace hk::pc {

// Show (true) or make up (false) a picture that came while frames are owed: its frame took
// `frame_ms` from its go, a frame of the game's rate is `period_ms`
inline bool show_owed_picture(double frame_ms, double period_ms) {
    return period_ms > 0 && frame_ms >= period_ms;
}

}  // namespace hk::pc
