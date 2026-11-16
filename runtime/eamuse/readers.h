// SPDX-License-Identifier: GPL-3.0-or-later
// What the e-AMUSEMENT server keeps of its players, read by code outside the games' answers (the
// tools, later an API): who has a player, its name, its scores. Free functions over a Store
// (store.h); each game's way of reading is behind its row of the table of games (games.h, Readers).
// Nothing here writes but reset_pin.
#pragma once

#include <string>
#include <vector>

#include "eamuse/games.h"
#include "eamuse/store.h"

namespace hk::ea {

struct Identity {   // a card and, where the server gave it one, its player's ref id
    std::string card, ref;
};
struct Profile {
    std::string name;   // as the game keeps it (its own encoding, usually Shift-JIS); "" if it keeps none
    long plays = -1;    // -1: not kept
};
struct Score {
    long song = 0, chart = 0;   // the game's own numbers
    long score = -1, plays = -1;
    int clear = -1;             // the game's own mark (pop'n: 1 cleared, 2-3 the better medals); -1: the game keeps none
};

// Every card the store knows of, from every table that names one (by card)
std::vector<Identity> players(const Store &);
// The card's player of this game; false: none (or the game isn't read yet)
bool profile(const Store &, const Game &, const Identity &, Profile &);
std::vector<Score> scores(const Store &, const Game &, const Identity &);

// Every PIN kept for the card, in any game (each once): the cards' table's (pop'n 13 on, IIDX
// DistorteD on), IIDX 10th style to HAPPY SKY's players, the GuitarFreaks and DrumMania cards (V and
// System 573), and the ones inside pop'n records: 11's userdata (0x9f3), 12's record (0x34), 13's
// (0x1c). For hiraku-server's card linking (server/accounts.h): a PIN never leaves the process
std::vector<std::string> pins(const Store &, const Identity &);
// The card's PIN (four digits) put in every place pins() reads that the server may write (the
// records' bytes where the answers write them too): the places done, and those the card has a player
// in whose PIN isn't changed (by name, for a person; why not in brackets). false: a PIN that isn't
// four digits, or a place not written
struct PinReset {
    std::vector<std::string> done, not_done;
};
bool reset_pin(Store &, const Identity &, const std::string &pin, PinReset &);

// A family's way of reading (a row's Readers in games.cpp)
struct Readers {
    bool (*profile)(const Store &, const Game &, const Identity &, Profile &);
    std::vector<Score> (*scores)(const Store &, const Game &, const Identity &);
};
extern const Readers POPN_READERS;         // pop'n music 9-18
extern const Readers POPN_LATER_READERS;   // the later PC pop'n games (M39)
extern const Readers IIDX9_READERS;        // beatmania IIDX 9th style
extern const Readers IIDX_READERS;         // beatmania IIDX 10th style to EMPRESS
extern const Readers GFDMV_READERS;        // GuitarFreaks and DrumMania V
extern const Readers GFDM573_READERS;      // the System 573 GuitarFreaks and DrumMania

}  // namespace hk::ea
