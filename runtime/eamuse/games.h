// SPDX-License-Identifier: GPL-3.0-or-later
// The games HIRAKU's e-AMUSE3 server answers (server.h): one table, a row a game (or a family of
// games one model serves), each with the answers its calls are tried on, in order. A call finds its
// row by its model's first three letters (game_of).
#pragma once

#include <string>
#include <vector>

#include "eamuse/protocol.h"

namespace hk::ea {

class Server;
struct Game;

// One way of answering: a request to its answer, "" to leave it to the next in the game's chain
using Answer = std::string (Server::*)(const Game &, const Call &, const Request &, const std::string &host);

// What the answers shared by several families ask of a game, instead of testing its model
enum class Family {
    popn9,            // pop'n music 9 (Konami Python 1)
    popn,             // pop'n music 10 to 14 (Konami Python 1)
    popn_pc,          // pop'n music 15 to 18 (PC games over libavs-win32: runtime/pc/)
    popn_pc_later,    // the later PC pop'n games (M39)
    iidx9,            // beatmania IIDX 9th style (the PC's e-AMUSE3)
    iidx10_13,        // beatmania IIDX 10th style to DistorteD: their module named after the model
    iidx14_16,        // beatmania IIDX GOLD on (libavs, pop'n 15's transport)
    gfdm_v,           // GuitarFreaks and DrumMania V (Konami Python 2)
    gfdm_573,         // the System 573 GuitarFreaks and DrumMania
    unknown,          // a model the table doesn't name
};

struct Game {
    const char *id;       // HIRAKU's id of the game ("popn12", as games/<id>/), or of the family for a row of several
    const char *title;    // for a person to read
    const char *model;    // the call's model's first three letters ("E12"); '?' stands for any one letter ("?39")
    Family family;
    std::vector<Answer> chain;   // tried in order; after the last, <MODULE status="0"/>
};

const std::vector<Game> &games();                 // the one table: every game the server answers
const Game &game_of(const std::string &model);    // exact rows first, then rows with '?', then the row for a model it doesn't know

}  // namespace hk::ea
