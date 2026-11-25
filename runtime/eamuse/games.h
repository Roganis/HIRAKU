// SPDX-License-Identifier: GPL-3.0-or-later
// The games HIRAKU's e-AMUSE3 server answers (server.h): one table, a row a game (or a family of
// games one model serves), each with the answers its calls are tried on, in order. A call finds its
// row by its model's first three letters (game_of).
#pragma once

#include <string>
#include <vector>

#include "eamuse/protocol.h"
#include "eamuse/store.h"

namespace hk::ea {

class Server;
struct Game;
struct Readers;   // readers.h: what code outside the answers reads of a game's players

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
    iidx_pc_later,    // the later PC beatmania IIDX games (model LDJ)
    unknown,          // a model the table doesn't name
    eemall,           // Konami's ee'MALL station (1st Avenue, C33: a Windows PC, pop'n music 9's library)
};

// A setting the owner may give (a file of the data folder): what it is, its range, and the value
// the server takes without it. A game's are kept by name and model (tables::setting: NAME-MODEL)
// unless their table is one of their own (a file named once: tables::phases_phc...); the server's
// own (server_settings) are their table's alone, or for tables::setting the name split at its '-'
// ("limit-cards": limit-cards).
struct Setting {
    const char *name;
    const char *label;        // for a person to read
    enum Kind { number, text } kind;
    long min, max;            // a number's range
    const char *by_default;   // the value without the owner's ("" for some: what that means is in help)
    const char *help;
    const Table *table;
};

struct Game {
    const char *id;       // HIRAKU's id of the game ("popn12", as games/<id>/), or of the family for a row of several
    const char *title;    // for a person to read
    const char *model;    // the call's model's first three letters ("E12"); '?' stands for any one letter ("?39")
    Family family;
    std::vector<Answer> chain;   // tried in order; after the last, <MODULE status="0"/>
    std::vector<Setting> settings = {};   // what the owner may set for it
    const Readers *readers = nullptr;     // its players, names and scores (readers.h); null: not read yet
    bool unlisted = false;                // left out of what hiraku-server's API lists and shows (its own
                                          // switch, list_all_games, lists it): no effect on the answers
    const char *module_prefix = nullptr;  // a game whose library names its own modules after its version:
                                          // their prefix (own_modules(); the one place such a name is kept)
};

const std::vector<Game> &games();                 // the one table: every game the server answers
const Game &game_of(const std::string &model);    // exact rows first, then rows with '?', then the row for a model it doesn't know
const Game *game_by_id(const std::string &id);    // a row by its id, or by its model for a row without one; null: none
// A game's own modules, as its library registers them (services.get lists them, or the library fails
// their calls before sending): its module_prefix + each of its family's suffixes. Empty: none. The
// answers match these modules by their suffix (module_suffix()), never by a versioned name
std::vector<std::string> own_modules(const Game &);
// A module's name after its letters+number prefix ("shop" of "<letters><number>shop"; "" if none)
std::string module_suffix(const std::string &module);

// The settings that are no game's: the title message, the data folder's limits
const std::vector<Setting> &server_settings();
// A setting's declaration (game null: the server's); null if it has none of that name
const Setting *find_setting(const Game *, const char *name);
// The key a setting is kept under (model: as setting()'s; "" and a row without a model: none)
Key setting_key(const Game *, const Setting &, const std::string &model = "");
// A setting's value: the owner's as kept (the file's text as it is, when it isn't empty), else the
// declared default. One that isn't declared reads as "". model: the call's model's three letters,
// the key's for a row that has no model of its own (a model the table doesn't name: phase-Z99);
// without them such a row has only its defaults.
// arcade: the asking cabinet's arcade (Server::Cabinet), whose own value (tables::arcade) comes
// first; "": the server's.
std::string setting(const Store &, const Game &, const char *name, const std::string &model = "", const std::string &arcade = "");
std::string setting(const Store &, const char *name);   // the server's
// The key an arcade's own value of a game's setting is kept under (tables::arcade: arcade id, name,
// model; model as setting()'s); empty: none (no arcade, or a row with no model and none given)
Key arcade_setting_key(const Game &, const Setting &, const std::string &arcade, const std::string &model = "");

}  // namespace hk::ea
