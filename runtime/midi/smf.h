// SPDX-License-Identifier: GPL-3.0-or-later
// Standard MIDI files (.mid, .midi, .kar, and RIFF .rmi around one) for hiraku-midi, the launcher's
// MIDI player (runtime/midi/player.cpp): the file's tracks merged into one list of messages, each
// at its time in seconds, as they would go down a MIDI cable.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hk::midi {

struct Event {
    double time = 0;              // seconds from the song's start
    std::vector<uint8_t> bytes;   // a channel message with its status byte, or a SysEx from F0 to F7
};

struct Song {
    std::vector<Event> events;    // in time order; at one time, in the order the file gives them
    double length = 0;            // the last event's time (the end of track, when that comes later)
    std::string title;            // the first track's name, when it is UTF-8 (often it is Shift-JIS)
};

// False with a reason in err when it isn't a MIDI file at all; a file cut short keeps what came
// before the cut.
bool parse(const uint8_t *data, size_t size, Song &song, std::string &err);
bool load(const std::string &path, Song &song, std::string &err);   // (a UTF-8 path)

// Starting part-way: the messages before `at` that set up the channels (programs, controllers,
// pitch bends, SysEx), without the notes, and without the controller changes a later one of the
// same controller overrides (a fade's hundred volume changes: the last). The ones that go in a
// sequence (bank select, RPN and NRPN, data entry) all stay. Then the song from `first`, the first
// event at or after `at`.
std::vector<std::vector<uint8_t>> setup_before(const Song &song, double at, size_t &first);

}  // namespace hk::midi
