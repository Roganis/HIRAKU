// SPDX-License-Identifier: GPL-3.0-or-later
// What a server's store holds, printed, and the owner's settings changed: hiraku-eamuse's --players,
// --scores, --settings and --set, hiraku-server's players, scores, settings and set. Tab-separated, a
// line a thing; names as the games keep them, their bytes (readers.h, games.h). No PIN or password
// is printed.
#pragma once

#include <string>

#include "eamuse/games.h"
#include "eamuse/store.h"

namespace hk::ea {

// How the program's messages name it and its commands: "hiraku-eamuse" and "--" ("--set"),
// "hiraku-server" and "" ("set")
struct AdminNames {
    const char *program;
    const char *dash;
};

// every card the store knows, its ref id, each game's player on it and its name
int print_players(const Store &);
// the card's scores (game: only that one's): game, song, chart, score, clear, plays
int print_scores(const Store &, const std::string &card, const Game *only);
// the owner's settings ("" every one, "server" the server's own, or a game's id or model)
int print_settings(const Store &, const std::string &which);
// one changed, checked against its range; value "": back to the default. The exit status
int set_setting(Store &, const std::string &game, const std::string &name, const std::string &value, const AdminNames &);

}  // namespace hk::ea
