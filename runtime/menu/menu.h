// SPDX-License-Identifier: GPL-3.0-or-later
// The game menu (hiraku-menu): a list of the games to choose from with the cabinet's own controls,
// shown by the launcher when a game was left by the 10-key's 5, 7 and 3 (host/menukey.h), or at its
// start. It is a core of the shell like a game's (menu/core.cpp), so the cabinet's controls, lamps,
// I/O boards, readers and picture are the shell's, as in a game.
//
// The launcher writes the list (--list FILE), one line each, tabs between the fields:
//     hiraku-menu-list 1
//     current <id>                        the game just left: where the menu opens
//     game <id> <shelf> <title> <detail>  in the order shown; a shelf is its games in a row
// and reads the choice (--choice FILE: the id and a new line; no file when the menu was closed).
// Text is drawn in SDL's 8x8 font: ASCII (anything else shows as '?').
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hk::menu {

struct Game {
    std::string id, title, detail;
};
struct Shelf {
    std::string name;
    std::vector<Game> games;
};
struct List {
    std::vector<Shelf> shelves;
    std::string current;
    bool parse(const std::string &text, std::string &error);
    bool load(const std::string &path, std::string &error);
    size_t games() const;
};

// What a control does here
enum class Act { none, prev, next, shelf_prev, shelf_next, start };

// --- each cabinet's controls (menu/nav.cpp) --------------------------------------
// The action of a family's control by its name (shell/cabinet.h; "P1 Button 4", "Select L"), or of
// a 10-key's (any family: 8 and 2 choose, 4 and 6 change shelf, Enter plays)
Act action(const std::string &family, const std::string &control);
// A family's rules: every control that does something (the 10-key's aside)
struct Rule {
    std::string control;
    Act act;
};
const std::vector<Rule> &rules(const std::string &family);
// Its turntables choose (beatmania's)
bool turntable(const std::string &family);
// What to tell the player, three lines: to choose, to change shelf, to play
struct Hint {
    std::string control, does;
};
std::vector<Hint> legend(const std::string &family);
// The lamp of cab::lamps(family) that belongs to a control ("" if none)
std::string lamp_for(const std::string &family, const std::string &control);

// --- where the player is in the list ---------------------------------------------
class State {
public:
    void set(const List &l);
    // One step; false if it did nothing (an empty list). `frame` times the moves' animations
    bool act(Act a, uint64_t frame);
    const List &list() const { return list_; }
    int shelf() const { return shelf_; }
    int row() const { return rows_.empty() ? 0 : rows_[(size_t)shelf_]; }
    const Game *selected() const;
    // Started: the game chosen, and the frame it was
    const Game *chosen() const { return chosen_ ? selected() : nullptr; }
    uint64_t chosen_at() const { return chosen_at_; }
    // For the picture: the row the list is scrolled to (it follows row()), and the last shelf change
    double shown_row() const { return shown_row_; }
    int shelf_dir() const { return shelf_dir_; }
    uint64_t shelf_at() const { return shelf_at_; }
    uint64_t moved_at() const { return moved_at_; }   // the last move: a long title starts to roll from then
    void animate();   // once a frame

private:
    List list_;
    int shelf_ = 0;
    std::vector<int> rows_;   // each shelf's row, kept when the shelf is left
    bool chosen_ = false;
    uint64_t chosen_at_ = 0, shelf_at_ = 0, moved_at_ = 0;
    int shelf_dir_ = 0;
    double shown_row_ = 0;
};

// --- the picture (menu/draw.cpp) -------------------------------------------------
// The menu's frame, w x h (640 or 848 by 480), ARGB
void draw(const State &s, const std::string &family, uint64_t frame, uint32_t *px, int w, int h);

}  // namespace hk::menu
