// SPDX-License-Identifier: GPL-3.0-or-later
// Everything the e-AMUSEMENT server keeps (store.h), each kind once: its name and the folder back
// end's file pattern, a {} for each part of its key. This file is the list of what the server keeps;
// a thing kept is named nowhere else.
#pragma once

#include <string>

#include "eamuse/store.h"

namespace hk::ea::tables {

// Cards and their players, for every game (server_popn.cpp's cardmng, server.cpp's numbering, userid)
inline constexpr Table cards{"cards", "cards"};           // every card: its id, its player's ref id, type, PIN
inline constexpr Table refid{"refid", "refid"};           // the next ref id's count
inline constexpr Table numbering{"numbering", "numbering"};   // the next card number's count
inline constexpr Table users{"users", "users"};           // KONAMI IDs: user, password, cards

// A card's record as the game writes it, <userdata>: pop'n 9's (C00) by card; pop'n 10 and 11 and
// IIDX 9th style to DistorteD's by game and card. userdata_of chooses.
inline constexpr Table userdata_c00{"userdata_c00", "userdata-{}"};
inline constexpr Table userdata{"userdata", "userdata-{}-{}"};
// pop'n 9-14's ee'MALL items, by card: one number a line
inline constexpr Table eemall{"eemall", "eemall-{}"};

// pop'n 10-14: mileage and tickets, by game and card
inline constexpr Table player{"player", "player-{}-{}"};
// pop'n 12-18: a player's record (the game's bytes), its best scores and its songs played, by game
// and card (pop'n 12) or ref id (13 on)
inline constexpr Table pdata{"pdata", "pdata-{}-{}"};
inline constexpr Table pscore{"pscore", "pscore-{}-{}"};
inline constexpr Table pplays{"pplays", "pplays-{}-{}"};
// the 64-bit PC pop'n (server_hc.cpp): a player, the player's scores, by ref id
inline constexpr Table phc{"phc", "phc-{}"};
inline constexpr Table phcs{"phcs", "phcs-{}"};
// the last 32-bit PC pop'n (server_p24.cpp): a player, the player's scores, by ref id; the credits'
// count; the NET TAISEN lobby's rooms
inline constexpr Table p24{"p24", "p24-{}"};
inline constexpr Table p24s{"p24s", "p24s-{}"};
inline constexpr Table p24_plays{"p24_plays", "p24-plays"};
inline constexpr Table lobby24{"lobby24", "lobby24-M39"};

// Rankings (server_rank.cpp): pop'n 9's three boards; pop'n 10 on, by game: the expert courses'
// board (18's internet ranking's), the shops' event board, pop'n 13's shops' phases
inline constexpr Table rank_score{"rank_score", "rank-score"};
inline constexpr Table rank_hitasura{"rank_hitasura", "rank-hitasura"};
inline constexpr Table rank_gamblerz{"rank_gamblerz", "rank-gamblerz"};
inline constexpr Table rank_exp{"rank_exp", "{}-rank-exp"};
inline constexpr Table rank_ir{"rank_ir", "{}-rank-ir"};
inline constexpr Table rank_shop{"rank_shop", "{}-rank-shop"};
inline constexpr Table shop_phase{"shop_phase", "{}-shop-phase"};

// beatmania IIDX (server_iidx.cpp): 9th style's scores, weekly song's entries and ranking's; 10th
// style on, by game: the players, their scores, the expert courses' bests
inline constexpr Table iidx9_scores{"iidx9_scores", "iidx9-scores"};
inline constexpr Table iidx9_weekly{"iidx9_weekly", "iidx9-weekly"};
inline constexpr Table iidx9_ranking{"iidx9_ranking", "iidx9-ranking"};
inline constexpr Table iidx_players{"iidx_players", "iidx-{}-players"};
inline constexpr Table iidx_scores{"iidx_scores", "iidx-{}-scores"};
inline constexpr Table iidx_courses{"iidx_courses", "iidx-{}-courses"};

// GuitarFreaks and DrumMania V (server_gfdmv.cpp): the cards, the next gdid's count, a player's
// records by gdid
inline constexpr Table gfdmv_cards{"gfdmv_cards", "gfdmv-cards"};
inline constexpr Table gfdmv_gdid{"gfdmv_gdid", "gfdmv-gdid"};
inline constexpr Table gfdmv{"gfdmv", "gfdmv-{}"};
// the System 573 GuitarFreaks and DrumMania (server_gfdm.cpp): the cards; a player's results by
// game and gdid
inline constexpr Table gfdm_cards{"gfdm_cards", "gfdm-cards"};
inline constexpr Table gfdm{"gfdm", "gfdm-{}-{}"};

// The owner's settings, files the owner writes (the server only reads them): the cabinets' title
// message; a setting by name and game (phase-<game>, net_event-<game>, pop'n 15-18's NET TAISEN
// numbers <name>-G15...); the 64-bit PC pop'n's phases and unlocks; the last 32-bit PC pop'n's NET
// TAISEN; IIDX 9th style's weekly song and ranking courses
inline constexpr Table message{"message", "message"};
inline constexpr Table setting{"setting", "{}-{}"};
inline constexpr Table phases_phc{"phases_phc", "phases-phc"};
inline constexpr Table unlock_phc{"unlock_phc", "unlock-phc"};
inline constexpr Table netvs_m39{"netvs_m39", "netvs-M39"};
inline constexpr Table iidx9_weekly_song{"iidx9_weekly_song", "iidx9-weekly-song"};
inline constexpr Table iidx9_courses{"iidx9_courses", "iidx9-courses"};
// An arcade's own value of a game's setting, over the server's (games.h's setting(); hiraku-server's
// arcades, server/accounts.h): by arcade id, setting name and model (arcade-3-net_event-F14)
inline constexpr Table arcade{"arcade", "arcade-{}-{}-{}"};

// Every table, for tests and tools
inline constexpr const Table *all[] = {
    &cards, &refid, &numbering, &users, &userdata_c00, &userdata, &eemall, &player, &pdata, &pscore, &pplays,
    &phc, &phcs, &p24, &p24s, &p24_plays, &lobby24, &rank_score, &rank_hitasura, &rank_gamblerz, &rank_exp,
    &rank_ir, &rank_shop, &shop_phase, &iidx9_scores, &iidx9_weekly, &iidx9_ranking, &iidx_players,
    &iidx_scores, &iidx_courses, &gfdmv_cards, &gfdmv_gdid, &gfdmv, &gfdm_cards, &gfdm, &message, &setting,
    &phases_phc, &unlock_phc, &netvs_m39, &iidx9_weekly_song, &iidx9_courses, &arcade};

// A card's <userdata> of a game: pop'n 9's (C00) is by card alone
struct Kept {
    const Table &table;
    Key key;
};
inline Kept userdata_of(const std::string &game, const std::string &card) {
    if (game == "C00") return {userdata_c00, {card}};
    return {userdata, {game, card}};
}

}  // namespace hk::ea::tables
