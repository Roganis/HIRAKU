// SPDX-License-Identifier: GPL-3.0-or-later
// What each cabinet family has to offer the player: its controls (MAME input
// names, with the default bindings) and its lamps (where the game drives them).
// Shared by the runtime and the launcher; no SDL, no board state.
#pragma once

#include <string>
#include <vector>

namespace hk::cab {

struct Control {
    std::string name;       // MAME input name, as fb::set_input and bindings files use it
    std::string label;      // for people ("Button 1 (white)")
    std::string group;      // "Player 1", "Player 2", "Cabinet"
    bool analog = false;    // takes an axis (joyN:axisK), not a button
    std::string defaults;   // default bindings, "key:Z, joy0:button0"
};

// A lamp is one bit of a lamp register, as MAME's firebeat.cpp decodes it.
// reg 0 = 0x7d000804 (all families), 1 = 0x7d000320, 2 = 0x7d000324.
struct Lamp {
    std::string name;       // as lights files and the lights socket use it
    std::string group;
    int reg = 0, bit = 0;
    bool active_low = false;
    int width = 1;          // a level of that many bits from `bit` up (a dimmed lamp): lit unless it is 0
};

const std::vector<Control> &controls(const std::string &family);
const std::vector<Lamp> &lamps(const std::string &family);
// The defaults as a bindings file ("<name> = <bindings>" lines)
std::string default_bindings(const std::string &family);

// Bindings/lights file syntax: "<name> = <item>[, <item>...]" lines, '#' comments.
// Each entry is (name, items) in file order; a later entry for a name replaces an
// earlier one. Lines without '=' are ignored.
struct Entry {
    std::string name;
    std::vector<std::string> items;
    int line = 0;
};
std::vector<Entry> parse_list_file(const std::string &text);

// A controller as bindings name it ("joy[DAO FPS]:button3"): SDL's name, with the characters the
// syntax uses ([ ] @ , # =) and control characters made '_', and no spaces at the ends. A second
// controller of the same name is "<name>@2", and so on, in the order SDL lists them.
std::string pad_name(const std::string &sdl_name);
std::string format_entry(const std::string &name, const std::vector<std::string> &items);

}  // namespace hk::cab
