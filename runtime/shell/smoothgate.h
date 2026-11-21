// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Smooth motion in songs only (--smooth-motion songs, the default; docs/SMOOTH.md): a game whose
// game.toml has a [smooth] table says by a word of its memory when a song is being played, the
// condition of a mod's [[rate]] (shell/wordcond.h):
//
//   [smooth]
//   while_changing = 0x80107f30   # the stage's frame counter: +1 a frame while a song plays
//   # while_equal = 0x...         # and (optional) while this word holds `value`
//   # value = 4
//
// While it doesn't hold, the in-between pictures aren't asked for and display pacing runs as
// without smooth motion; the core is still asked for one in-between picture a frame (not shown), so
// what it pairs from frame to frame is kept, and when the gate opens its in-between pictures are
// those smooth motion everywhere would make. A game without the
// table, or --smooth-motion everywhere: open on every frame, as before.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "wordcond.h"

namespace hk::smoothgate {

struct State {
    bool has = false;   // a condition: the game's [smooth], and songs only asked for
    WordCondition cond;
    bool on = false;
    uint64_t frames = 0, open_frames = 0;   // looked at; of them open
};
inline State &state() {
    static State s;
    return s;
}
// whether smooth motion's in-between pictures are wanted on this frame
inline bool open() { return !state().has || state().on; }

// A game.toml's [smooth] table into `c`: false with `err` when it's there but can't be used;
// `has` whether it's there
inline bool parse(const std::string &text, WordCondition &c, bool &has, std::string &err) {
    has = false;
    c = WordCondition{};
    bool in = false, changing = false, equal = false, value = false;
    size_t at = 0;
    for (int n = 1; at < text.size(); n++) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        at = end + 1;
        if (const size_t h = line.find('#'); h != std::string::npos) line.erase(h);
        const size_t a = line.find_first_not_of(" \t\r"), b = line.find_last_not_of(" \t\r");
        if (a == std::string::npos) continue;
        line = line.substr(a, b - a + 1);
        if (line[0] == '[') {
            in = line == "[smooth]";
            has |= in;
            continue;
        }
        if (!in) continue;
        const size_t eq = line.find('=');
        auto fail = [&](const std::string &why) {
            err = "game.toml:" + std::to_string(n) + ": [smooth]: " + why;
            return false;
        };
        if (eq == std::string::npos) return fail("expected key = number");
        std::string key = line.substr(0, eq), val = line.substr(eq + 1);
        key.erase(key.find_last_not_of(" \t") + 1);
        val.erase(0, val.find_first_not_of(" \t"));
        char *e = nullptr;
        const unsigned long long v = std::strtoull(val.c_str(), &e, 0);
        if (val.empty() || *e || v > 0xffffffffull) return fail("'" + key + "' is a number (decimal or 0x hex)");
        if (key == "while_changing") c.address = (uint32_t)v, changing = true;
        else if (key == "while_equal") c.gate = (uint32_t)v, equal = true;
        else if (key == "value") c.value = (uint32_t)v, value = true;
        else return fail("unknown key '" + key + "' (while_changing, while_equal, value)");
    }
    if (!has) return true;
    if (!changing) {
        err = "game.toml: [smooth] needs while_changing";
        return false;
    }
    if (equal != value) {
        err = "game.toml: [smooth]: while_equal and value go together";
        return false;
    }
    c.gated = equal;
    return true;
}

// Once a frame, before it runs: read(address) the game's word (Core::mem). A repeated frame (the
// core showing one again) isn't counted
template <class Read> void frame(Read read, bool repeated) {
    State &s = state();
    if (repeated) return;
    s.frames++;
    if (s.has) {
        bool known;
        s.on = s.cond.step(s.on, read, known);
    }
    if (open()) s.open_frames++;
}

}  // namespace hk::smoothgate
