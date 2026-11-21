// SPDX-License-Identifier: GPL-3.0-or-later
// Smooth motion in songs only (runtime/shell/smoothgate.h) and the word condition it shares with a
// mod's [[rate]] (runtime/shell/wordcond.h), on made-up memory: on after the word changed on two
// frames in a row, off after two still ones, while_equal off at once; a game.toml's [smooth] read.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "shell/smoothgate.h"

namespace {

int failures = 0;
void check(bool ok, const char *what) {
    if (!ok) failures++, std::printf("FAIL: %s\n", what);
}

// the game's memory: a word at 0x100 (the counter), one at 0x200 (a phase)
uint32_t g_counter = 0, g_phase = 0;
bool g_readable = true;
const uint8_t *read(uint32_t a) {
    if (!g_readable) return nullptr;
    if (a == 0x100) return reinterpret_cast<const uint8_t *>(&g_counter);
    if (a == 0x200) return reinterpret_cast<const uint8_t *>(&g_phase);
    return nullptr;
}

// the word's values frame by frame: whether the condition held after each
std::vector<int> run(hk::WordCondition c, const std::vector<uint32_t> &values, const std::vector<uint32_t> &phases = {}) {
    std::vector<int> on;
    bool now = false;
    for (size_t i = 0; i < values.size(); i++) {
        g_counter = values[i];
        g_phase = i < phases.size() ? phases[i] : 0;
        bool known;
        now = c.step(now, read, known);
        on.push_back(now);
    }
    return on;
}

}  // namespace

int main() {
    hk::WordCondition c;
    c.address = 0x100;
    // still, then counting (on from its second change), held (off after two still), counting again
    check(run(c, {0, 0, 0, 1, 2, 3, 4, 4, 4, 4, 5, 6}) == std::vector<int>{0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 1},
          "on after two changing frames, off after two still");
    // one change alone (a load) doesn't count
    check(run(c, {0, 7, 7, 7, 7}) == std::vector<int>{0, 0, 0, 0, 0}, "a single change isn't a song");
    // one still frame inside a song keeps it on
    check(run(c, {0, 1, 2, 2, 3, 4}) == std::vector<int>{0, 0, 1, 1, 1, 1}, "one still frame doesn't end it");

    // while_equal: only while the phase holds the value, off at once when it doesn't (and on again
    // at once when it does, the word still changing)
    hk::WordCondition e = c;
    e.gated = true, e.gate = 0x200, e.value = 4;
    check(run(e, {0, 1, 2, 3, 4, 5, 6}, {4, 4, 4, 4, 3, 4, 4}) == std::vector<int>{0, 0, 1, 1, 0, 1, 1}, "while_equal");
    check(run(e, {0, 1, 2, 3, 4}, {3, 3, 3, 3, 3}) == std::vector<int>{0, 0, 0, 0, 0}, "while_equal never holding");

    // a word that can't be read: nothing counted, the state given back
    {
        hk::WordCondition u = c;
        bool known = true;
        g_readable = false;
        check(u.step(true, read, known) && !known, "unreadable: kept, not known");
        g_readable = true;
    }

    // game.toml's [smooth]
    {
        hk::WordCondition p;
        bool has = true;
        std::string err;
        check(hk::smoothgate::parse("id = \"x\"\n[program]\nload = 0x80000000\n", p, has, err) && !has, "no [smooth]: none");
        check(hk::smoothgate::parse("[smooth]\n# a comment\nwhile_changing = 0x80107f30   # the counter\n[board]\nx = 1\n", p, has, err) &&
                  has && p.address == 0x80107f30 && !p.gated,
              "[smooth] while_changing");
        check(hk::smoothgate::parse("[smooth]\nwhile_changing = 256\nwhile_equal = 0x200\nvalue = 4\n", p, has, err) && p.address == 256 &&
                  p.gated && p.gate == 0x200 && p.value == 4,
              "[smooth] while_equal and value");
        check(!hk::smoothgate::parse("[smooth]\nwhile_equal = 0x200\nvalue = 4\n", p, has, err), "while_changing is needed");
        check(!hk::smoothgate::parse("[smooth]\nwhile_changing = 1\nwhile_equal = 2\n", p, has, err), "while_equal needs value");
        check(!hk::smoothgate::parse("[smooth]\nwhile_changin = 1\n", p, has, err), "an unknown key");
        check(!hk::smoothgate::parse("[smooth]\nwhile_changing = \"0x10\"\n", p, has, err), "not a number");
    }

    // the gate: open on every frame without a condition; with one, as the condition, frames counted
    {
        hk::smoothgate::State &s = hk::smoothgate::state();
        check(hk::smoothgate::open(), "no condition: open");
        s.has = true;
        s.cond = c;
        std::vector<int> open;
        for (uint32_t v : {0u, 0u, 1u, 2u, 3u, 3u, 3u}) {
            g_counter = v;
            hk::smoothgate::frame(read, false);
            open.push_back(hk::smoothgate::open());
        }
        hk::smoothgate::frame(read, true);   // a repeated frame isn't counted
        check(open == std::vector<int>{0, 0, 0, 1, 1, 1, 0}, "the gate follows the condition");
        check(s.frames == 7 && s.open_frames == 3, "frames counted");
    }

    if (failures) return 1;
    std::printf("smoothgate: ok\n");
    return 0;
}
