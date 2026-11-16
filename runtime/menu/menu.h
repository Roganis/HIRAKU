// SPDX-License-Identifier: GPL-3.0-or-later
// The game menu (hiraku-menu): a list of the games to choose from with the cabinet's own controls,
// shown by the launcher when a game was left by the 10-key's 5, 7 and 3 (host/menukey.h), or at its
// start. It is a core of the shell like a game's (menu/core.cpp), so the cabinet's controls, lamps,
// I/O boards, readers and picture are the shell's, as in a game.
//
// The launcher writes the list (--list FILE), one line each, tabs between the fields:
//     hiraku-menu-list 1
//     current <id>                        the game just left: where the menu opens
//     game <id> <shelf> <title> <detail> [<picture>]  in the order shown; a shelf is its games in a row
//     shelf <shelf> <#rrggbb>             a shelf's colour (its series'), drawn with --look
// and reads the choice (--choice FILE: the id and a new line; no file when the menu was closed).
// --notice "LINE|LINE" is said on opening: a game that came back here because it ended with an error.
// --look "name=#rrggbb,..." draws it in the launcher's colours (Look below); without, amber on night blue.
// Text is drawn in the launcher's fonts where it gives them (--font: menu/font.h), any alphabet;
// else in SDL's 8x8 font, ASCII (anything else shows as '?'). A game's picture is a BMP
// file's path, shown behind the list while the game is the one chosen: the player's own, or the
// title screen the shell kept from a run of the game (host/menupic.h); HIRAKU comes with none.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "shell/uilook.h"

namespace hk::menu {

struct Game {
    std::string id, title, detail;
    std::string picture;   // a BMP file, "" if the game has none
};
struct Shelf {
    std::string name;
    std::vector<Game> games;
    uint32_t colour = 0;   // its series' (0xrrggbb), with has_colour
    bool has_colour = false;
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
    // The logo's way up to the top (logo_at below): at the end of its opening, or sooner at a press
    uint64_t dock_at() const { return dock_at_; }
    void animate();   // once a frame
    // Something to tell the player on opening (--notice: the game just left ended with an error),
    // "a first line|a second, smaller": shown where the controls are said, until the first move
    void set_notice(const std::string &text) { notice_ = text; }
    const std::string &notice() const { return notice_; }

private:
    List list_;
    int shelf_ = 0;
    std::vector<int> rows_;   // each shelf's row, kept when the shelf is left
    bool chosen_ = false;
    uint64_t chosen_at_ = 0, shelf_at_ = 0, moved_at_ = 0, dock_at_ = UINT64_MAX;
    int shelf_dir_ = 0;
    double shown_row_ = 0;
    std::string notice_;
};

// --- the picture (menu/draw.cpp) -------------------------------------------------
// The launcher's look (--look; shell/uilook.h)
using ui::Look;
using ui::light;
void set_look(const Look &l);   // (no call: the menu's own, amber on night blue)
void own_look();                 // ... and back to that
// The menu's frame, w x h (640 or 848 by 480), ARGB
void draw(const State &s, const std::string &family, uint64_t frame, uint32_t *px, int w, int h);
// The launcher's fonts for it (--font: menu/font.h; false and why if the file isn't one), and
// HIRAKU's name as a picture for the top (--logo: a BMP, white on black). "" takes them away.
bool set_font(const std::string &path, std::string &error);
void set_logo(const std::string &path);
// A text as the 8x8 font has it: a '?' for each character that isn't ASCII
std::string ascii_only(const std::string &text);

// HIRAKU's logo over the menu (menu/logo.h): it opens in the middle of a black screen as the menu
// opens, stands a moment, and glides up into the top's left corner, where it stays, the black
// fading away quickly as it goes (at once, at the first press). A game chosen, it glides back down
// to the middle, the screen going black again, and closes; the menu ends on the black.
constexpr uint64_t kLogoGlide = 18;   // frames, each way
constexpr uint64_t kLogoFade = 8;     // frames: the black's fade, each way
struct LogoAt {
    int anim = -1, frame = 0;   // which of logo.h's animations, and its frame; -1: none drawn
    int x = 0, y = 0, w = 0, h = 0;   // where, and how big
    int dark = 0;               // the black over the menu, under the logo: 0 none .. 255 whole
};
LogoAt logo_at(const State &s, uint64_t frame, int w, int h);
// The frame the logo has closed by, once a game is chosen (0 if there is no logo)
uint64_t logo_closed(const State &s);

}  // namespace hk::menu
