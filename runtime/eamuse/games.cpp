// SPDX-License-Identifier: GPL-3.0-or-later
// The games HIRAKU's e-AMUSE3 server answers (games.h).
#include "eamuse/games.h"
#include "eamuse/readers.h"
#include "eamuse/server.h"
#include "eamuse/tables.h"

#include <cstring>

namespace hk::ea {

// The owner's settings (games.h's Setting), each declared once: its default here is the one the
// answers take (they read it through setting()). docs/EAMUSE.md and docs/EAMUSE3.md tell more.
namespace {
constexpr Setting::Kind NUMBER = Setting::number, TEXT = Setting::text;
const Table *const BY_GAME = &tables::setting;

// pop'n music 10-14: the event phase (message.get, game get)
Setting phase(long last) {
    return {"phase", "Event phase", NUMBER, -1, last, "-1",
            "The event phase given to the cabinets, set when it differs from a cabinet's own (it opens the events and "
            "songs by stages; the game's Unlocks list them); -1 leaves each cabinet's own.",
            BY_GAME};
}
const Setting NET_EVENT_13 = {"net_event", "Network event", NUMBER, 0, 3, "2",
                              "The network event the cabinets run, one at a time: 2 NET TAISEN, 1 the INTERNET RANKING (the "
                              "attract mode's course boards and the courses' results online).",
                              BY_GAME};
// pop'n music 15
const Setting PHASE_15 = {"phase", "Event phase", NUMBER, -1, 2, "-1",
                          "The songs' event stage (0-2): the cabinet's own unless this is higher; -1 leaves the cabinet's own.", BY_GAME};
const Setting IR_RANKING_15 = {"ir_ranking", "e-AMUSEMENT event", NUMBER, 0, 5, "2",
                               "The event the e-AMUSEMENT players see: 2 the ADVENTURE map, 1 the INTERNET RANKING (the expert "
                               "courses' boards), 4 and 5 the rankings' other periods.",
                               BY_GAME};
const Setting NET_EVENT_15 = {"net_event", "NET TAISEN", NUMBER, 0, 3, "1", "NET TAISEN on the mode wheel from 1; 0 none.", BY_GAME};
// NET TAISEN's numbers, pop'n 15 on (docs/EAMUSE3.md, "NET TAISEN")
const Setting LOCAL_MATCHING = {"local_matching_enable", "Same-shop matching", NUMBER, 0, 1, "1",
                                "1: cabinets of one shop may meet; 0: they may not.", BY_GAME};
const Setting MATCHING_SEC = {"matching_sec", "Seconds in the lobby", NUMBER, 0, 600, "120",
                              "How long a cabinet waits in the lobby for others (0: it leaves at once; nobody comes: CHALLENGE mode).",
                              BY_GAME};
const Setting OJAMA_RATE = {"ojama_gauge_rate", "Ojama gauge rate", NUMBER, 0, 1000, "100",
                            "How fast the ojama gauge fills, in percent of the game's own.", BY_GAME};
const Setting CPU_LEVEL = {"cpu_level", "CPU strength", TEXT, 0, 0, "100,100,100,100,100,100,100,100,100,100",
                           "Ten numbers, comma-separated: the CPU players' strength by level, in percent of the game's own.", BY_GAME};
const Setting DOWNLIMIT_WIN = {"downlimit_win", "Wins that keep a class", NUMBER, 0, 100, "0",
                               "The wins that keep a class once reached (0: none, players are demoted as they were).", BY_GAME};
const Setting DOWNLIMIT_CLASS = {"downlimit_class", "Classes a player may fall", NUMBER, 0, 10, "10",
                                 "How far below a kept class a player may fall (10: no floor).", BY_GAME};
const Setting JOB_SP_RATE = {"job_sp_rate", "Jobs' specials", TEXT, 0, 0,
                             "30,150,30,50,30,150,30,100,0,0,30,0,30,100,100,0,30,0,30,0,30,0,30,0,30,0",
                             "26 numbers, comma-separated, two a job (13 jobs): the special's chance in percent at the job's top "
                             "level, then its amount in percent.",
                             BY_GAME};
const Setting JOB_EXP_RATE = {"job_exp_rate", "Jobs' experience rate", NUMBER, 0, 1000, "100",
                              "Read by the game, never used.", BY_GAME};
// pop'n music 16 (docs/EAMUSE3.md, "game get": what is known of each)
const Setting PHASE_16 = {"phase", "Event phase", NUMBER, -1, 2, "",
                          "The event phase (0-2), sent as it is; empty: none sent, the cabinet's own.", BY_GAME};
const Setting NETVS_PHASE_16 = {"netvs_phase", "NET TAISEN phase", NUMBER, 0, 7, "1", "NET TAISEN from 1.", BY_GAME};
const Setting IR_PHASE_16 = {"ir_phase", "Internet ranking phase", NUMBER, 0, 3, "0", "The INTERNET RANKING's phase.", BY_GAME};
const Setting PRIVATE_PHASE_16 = {"private_phase", "Private phase", NUMBER, 0, 3, "1",
                                  "1 and 2 each turn something on; the highest isn't simply the most.", BY_GAME};
const Setting SECRET_PHASE_16 = {"normal_secret_phase", "Secret songs phase", NUMBER, 0, 5, "1", "The secret songs' phase.", BY_GAME};
const Setting HISCORE_PHASE_16 = {"all_hiscore_phase", "High scores phase", NUMBER, 0, 3, "1", "The high scores' phase.", BY_GAME};
const Setting CPU_HIGHER_16 = {"cpu_higher_release", "Stronger CPUs", NUMBER, 0, 1, "0", "Read by the game, never used.", BY_GAME};
// pop'n music 17 and 18: guesses but matching_sec (pop'n 15's)
const Setting GAME_PHASE = {"game_phase", "Game phase", NUMBER, 0, 2, "2", "The game's phase: the higher of this and the cabinet's.", BY_GAME};
const Setting EAMUSE_PHASE_17 = {"e_amuse_phase", "e-AMUSEMENT phase", NUMBER, 0, 15, "14",
                                 "Opens the content by stages: NET TAISEN from 4; the game's tables run to 14.", BY_GAME};
const Setting IR_PHASE_18 = {"ir_phase", "Internet ranking phase", NUMBER, 0, 13, "0", "The INTERNET RANKING runs in 1-5 and 7-12.", BY_GAME};
const Setting EVENT_PHASE_18 = {"event_phase", "Event phase", NUMBER, 0, 6, "1", "The event runs in 1-5.", BY_GAME};
const Setting NETVS_PHASE_18 = {"netvs_phase", "NET TAISEN phase", NUMBER, 0, 13, "1",
                                "NET TAISEN from 1, something more in 3, 6, 8, 10 and 12.", BY_GAME};
const Setting LICENSE_PHASE_18 = {"license_phase", "License phase", NUMBER, 0, 3, "1", "Its meaning isn't known.", BY_GAME};
const Setting PSP_PHASE_18 = {"psp_phase", "Portable tie-in phase", NUMBER, 0, 6, "0", "Its meaning isn't known.", BY_GAME};
const Setting PSP_MONTH_18 = {"psp_sale_month", "Portable tie-in month", NUMBER, 1, 12, "1", "Its meaning isn't known.", BY_GAME};
const Setting PSP_DAY_18 = {"psp_sale_day", "Portable tie-in day", NUMBER, 1, 31, "1", "Its meaning isn't known.", BY_GAME};
const Setting BOSS_DIFF_18 = {"boss_diff", "Boss difficulty", NUMBER, 0, 15, "0", "Its meaning isn't known.", BY_GAME};
const Setting BOSS_POINT_18 = {"boss_battle_point", "Boss battle points", NUMBER, 0, 99999999, "0", "Its meaning isn't known.", BY_GAME};
// the later PC pop'n games (model M39)
const Setting PHASES_LATER = {"phases", "Events' phases", TEXT, 0, 0, "",
                              "EVENT=PHASE pairs, space-separated (events 0-7; \"0=2 4=0\"): an event's phase; an event not named "
                              "stays at its last phase (empty: every event at its last).",
                              &tables::phases_phc};
const Setting UNLOCK_LATER = {"unlock", "Every song", NUMBER, 0, 1, "1",
                              "1: every song listed for every player, with its locked charts; 0: the songs the game itself opened.",
                              &tables::unlock_phc};
const Setting NETVS_LATER = {"netvs", "NET TAISEN phase", NUMBER, 0, 15, "1", "The NET TAISEN event's phase (event 3); 0 off.",
                             &tables::netvs_m39};
// beatmania IIDX 9th style
const Setting WEEKLY_SONG_9 = {"weekly_song", "Weekly song", NUMBER, 1, 2047, "",
                               "The week's song's number; empty: one of the songs played on this server, another each week.",
                               &tables::iidx9_weekly_song};
const Setting COURSES_9 = {"courses", "Ranking courses", TEXT, 0, 0, "1 0 1 2 3 4 5",
                           "Seven numbers, space-separated: the ranking's number, then the six courses the attract mode's "
                           "internet ranking shows (0-11 each).",
                           &tables::iidx9_courses};
// ee'MALL 1st Avenue (server_eemall.cpp): its songs locked until bought, and HIRAKU's rates (Konami's
// prices and rates are lost: these are guesses)
const Setting EEMALL_STATION = {"station", "Songs locked until bought", NUMBER, -1, 1, "-1",
                                "1: the pop'n games open the ee'MALL songs bought at the station (and the points); 0: the owner's "
                                "eemall-CARD lists, as before; -1: 1 once a station has booted against this server or the shop's "
                                "catalogue (tools/eemall_catalogue.py) is in its folder, else 0.",
                                BY_GAME};
const Setting EEMALL_CREDIT = {"credit_points", "Points a pop'n credit earns", NUMBER, 0, 1000, "100",
                               "ee'MALL points a pop'n credit played with a card earns, handed to the station at its next visit "
                               "(HIRAKU's guess, a song for three credits: the owner's choice, 2026-11-24; 30 was too slow).",
                               BY_GAME};
const Setting EEMALL_PRICE = {"song_price", "A song's price", NUMBER, 1, 99999, "300",
                              "A song's price in points at the station's RECORDS, written into the shop by "
                              "tools/eemall_catalogue.py (HIRAKU's guess).",
                              BY_GAME};
const Setting EEMALL_GIFT = {"restaurant_gift", "The RESTAURANT's free dish", NUMBER, 0, 2, "1",
                             "The RESTAURANT's free dish (500 points): 1 once a day per card, 0 never, 2 on every visit "
                             "(restrant.get's sub_code; Konami's rule is lost).",
                             BY_GAME};
}  // namespace

const std::vector<Setting> &server_settings() {
    static const std::vector<Setting> list = {
        {"message", "Title message", TEXT, 0, 0, "",
         "One line (at most 255 bytes, Shift-JIS or ASCII) scrolled across the title screen of the games that show one "
         "(pop'n music 9 on); empty: none.",
         &tables::message},
        {"limit-cards", "Most cards kept", NUMBER, 0, 100000000, "65536", "How many cards the server keeps; past it no new card is taken.",
         BY_GAME},
        {"limit-files", "Most files kept", NUMBER, 0, 100000000, "200000",
         "How many files the data folder may hold; past it nothing new is kept.", BY_GAME},
    };
    return list;
}

const Setting *find_setting(const Game *g, const char *name) {
    for (const Setting &s : g ? g->settings : server_settings())
        if (!std::strcmp(s.name, name)) return &s;
    return nullptr;
}

Key setting_key(const Game *g, const Setting &s, const std::string &model) {
    if (s.table != BY_GAME) return {};
    if (g) {
        const std::string m = *g->model && !std::strchr(g->model, '?') ? std::string(g->model) : model;
        return m.empty() ? Key{} : Key{s.name, m};
    }
    const char *dash = std::strchr(s.name, '-');
    return dash ? Key{std::string(s.name, dash), dash + 1} : Key{s.name};
}

Key arcade_setting_key(const Game &g, const Setting &s, const std::string &arcade, const std::string &model) {
    const std::string m = *g.model && !std::strchr(g.model, '?') ? std::string(g.model) : model;
    if (arcade.empty() || m.empty()) return {};
    return {arcade, s.name, m};
}

namespace {
std::string read_setting(const Store &st, const Game *g, const char *name, const std::string &model, const std::string &arcade) {
    const Setting *s = find_setting(g, name);
    if (!s) return "";
    if (g && !arcade.empty())   // (an arcade's own value first)
        if (const Key ak = arcade_setting_key(*g, *s, arcade, model); !ak.empty())
            if (std::string v = st.get(tables::arcade, ak); !v.empty()) return v;
    const Key key = setting_key(g, *s, model);
    if (s->table == BY_GAME && key.empty()) return s->by_default;
    const std::string v = st.get(*s->table, key);
    return v.empty() ? s->by_default : v;
}
}  // namespace

std::string setting(const Store &st, const Game &g, const char *name, const std::string &model, const std::string &arcade) {
    return read_setting(st, &g, name, model, arcade);
}
std::string setting(const Store &st, const char *name) { return read_setting(st, nullptr, name, "", ""); }

// Adding a game: its answers as members of Server in a server_<family>.cpp (or an existing family's
// file, server.h names them), one row here, its section in docs/EAMUSE3.md, its calls in a
// transcript (tests/eamuse/transcripts/). Each chain member answers what is its own and leaves the
// rest ("") to the next; most chains end in the later pop'n games' shared answers (answer_later:
// services, cards, the shop) and pop'n 9's (answer_shared, answer_popn9), which every game falls
// back to. A model with a '?' matches after every exact row: a later pop'n with a ?39 model (K39,
// L39) needs its own exact row, or it is taken for a System 573 GuitarFreaks.
const std::vector<Game> &games() {
    using S = Server;
    static const Answer later = &S::answer_later, shared = &S::answer_shared, popn9 = &S::answer_popn9;
    static const Answer c02 = &S::answer_c02, red = &S::answer_iidx_red, fdd = &S::answer_iidx_fdd, iidx_userdata = &S::answer_iidx_userdata;
    static const std::vector<Game> table = {
        {"popn9", "pop'n music 9", "C00", Family::popn9, {shared, popn9}, {}, &POPN_READERS},
        {"popn10", "pop'n music 10", "C20", Family::popn, {later, shared, popn9}, {phase(5)}, &POPN_READERS},
        {"popn11", "pop'n music 11", "D11", Family::popn, {later, shared, popn9}, {phase(5)}, &POPN_READERS},
        {"popn12", "pop'n music 12", "E12", Family::popn, {later, shared, popn9}, {phase(5)}, &POPN_READERS},
        {"popn13", "pop'n music 13", "E13", Family::popn, {later, shared, popn9}, {phase(4), NET_EVENT_13}, &POPN_READERS},
        {"popn14", "pop'n music 14", "F14", Family::popn, {later, shared, popn9}, {phase(9), NET_EVENT_13}, &POPN_READERS},
        // (15 and 16: in the order game get answers them)
        {"popn15", "pop'n music 15", "G15", Family::popn_pc, {&S::answer_g15, later, shared, popn9},
         {PHASE_15, IR_RANKING_15, NET_EVENT_15, LOCAL_MATCHING, MATCHING_SEC, OJAMA_RATE, CPU_LEVEL, DOWNLIMIT_WIN, DOWNLIMIT_CLASS,
          JOB_SP_RATE, JOB_EXP_RATE},
         &POPN_READERS},
        {"popn16", "pop'n music 16", "H16", Family::popn_pc, {&S::answer_j39, later, shared, popn9},
         {PHASE_16, NETVS_PHASE_16, IR_PHASE_16, PRIVATE_PHASE_16, SECRET_PHASE_16, HISCORE_PHASE_16, LOCAL_MATCHING, MATCHING_SEC,
          OJAMA_RATE, CPU_LEVEL, DOWNLIMIT_WIN, DOWNLIMIT_CLASS, JOB_EXP_RATE, CPU_HIGHER_16, JOB_SP_RATE},
         &POPN_READERS},
        {"popn17", "pop'n music 17", "I17", Family::popn_pc, {&S::answer_j39, later, shared, popn9},
         {GAME_PHASE, EAMUSE_PHASE_17, LOCAL_MATCHING, MATCHING_SEC}, &POPN_READERS},
        {"popn18", "pop'n music 18", "J39", Family::popn_pc, {&S::answer_j39, later, shared, popn9},
         {GAME_PHASE, IR_PHASE_18, EVENT_PHASE_18, NETVS_PHASE_18, LICENSE_PHASE_18, PSP_PHASE_18, PSP_MONTH_18, PSP_DAY_18,
          LOCAL_MATCHING, MATCHING_SEC, BOSS_DIFF_18, BOSS_POINT_18},
         &POPN_READERS},
        {"popn-pc-later", "pop'n music, later PC versions", "M39", Family::popn_pc_later, {&S::answer_hc, &S::answer_p24, later, shared, popn9},
         {PHASES_LATER, UNLOCK_LATER, NETVS_LATER}, &POPN_LATER_READERS, /*unlisted*/ true},
        {"iidx9", "beatmania IIDX 9th style", "C02", Family::iidx9, {c02, iidx_userdata, shared, popn9}, {WEEKLY_SONG_9, COURSES_9},
         &IIDX9_READERS},
        {"iidx10", "beatmania IIDX 10th style", "D01", Family::iidx10_13, {c02, red, iidx_userdata, shared, popn9}, {}, &IIDX_READERS},
        {"iidx11", "beatmania IIDX RED", "E11", Family::iidx10_13, {c02, red, iidx_userdata, shared, popn9}, {}, &IIDX_READERS},
        {"iidx12", "beatmania IIDX HAPPY SKY", "ECO", Family::iidx10_13, {c02, red, iidx_userdata, shared, popn9}, {}, &IIDX_READERS},
        {"iidx13", "beatmania IIDX DistorteD", "FDD", Family::iidx10_13, {c02, red, fdd, later, iidx_userdata, shared, popn9}, {}, &IIDX_READERS},
        {"iidx14", "beatmania IIDX GOLD", "GLD", Family::iidx14_16, {fdd, later, shared, popn9}, {}, &IIDX_READERS},
        {"iidx15", "beatmania IIDX DJ TROOPERS", "HDD", Family::iidx14_16, {fdd, later, shared, popn9}, {}, &IIDX_READERS},
        {"iidx16", "beatmania IIDX EMPRESS", "I00", Family::iidx14_16, {fdd, later, shared, popn9}, {}, &IIDX_READERS},
        // (the four V games keep one set of players: a card's player is every one's)
        {"drmnvj", "DrumMania V", "E02", Family::gfdm_v, {&S::answer_gfdmv, later, shared, popn9}, {}, &GFDMV_READERS},
        {"drmnv2j", "DrumMania V2", "F02", Family::gfdm_v, {&S::answer_gfdmv, later, shared, popn9}, {}, &GFDMV_READERS},
        {"", "GuitarFreaks V", "E03", Family::gfdm_v, {&S::answer_gfdmv, later, shared, popn9}, {}, &GFDMV_READERS},
        {"", "GuitarFreaks V2", "F03", Family::gfdm_v, {&S::answer_gfdmv, later, shared, popn9}, {}, &GFDMV_READERS},
        {"gfdm573-gf", "GuitarFreaks (System 573)", "?39", Family::gfdm_573, {&S::answer_gfdm, shared}, {}, &GFDM573_READERS},
        {"gfdm573-dm", "DrumMania (System 573)", "?40", Family::gfdm_573, {&S::answer_gfdm, shared}, {}, &GFDM573_READERS},
        {"eemall", "ee'MALL 1st Avenue", "C33", Family::eemall, {&S::answer_eemall, shared, popn9},
         {EEMALL_STATION, EEMALL_CREDIT, EEMALL_PRICE, EEMALL_GIFT}},
        // the 64-bit PC beatmania IIDX (games/iidx33): its modules' prefix moves with each version
        {"iidx-pc-later", "beatmania IIDX, later PC versions", "LDJ", Family::iidx_pc_later, {later, shared, popn9},
         {}, &IIDX_READERS, /*unlisted*/ true, "IIDX33"},
        // last: a model no row above names, answered as a later pop'n (its phase by the call's model: phase-Z99)
        {"", "a game the table doesn't name", "", Family::unknown, {later, shared, popn9}, {phase(-1)}},
    };
    return table;
}

namespace {
bool matches(const char *pattern, const std::string &model, bool wild) {
    if (model.size() < 3) return false;
    bool has_wild = false;
    for (int i = 0; i < 3; i++) {
        if (!pattern[i]) return false;
        if (pattern[i] == '?') has_wild = true;
        else if (pattern[i] != model[i]) return false;
    }
    return has_wild == wild;
}
}  // namespace

std::vector<std::string> own_modules(const Game &g) {
    std::vector<std::string> out;
    if (!g.module_prefix) return out;
    // the later PC beatmania IIDX's (the game's log's module_add lines at start-up)
    static const char *const IIDX_PC_LATER[] = {"pc", "music", "shop", "grade", "lobby", "streaming", "gameSystem"};
    if (g.family == Family::iidx_pc_later)
        for (const char *s : IIDX_PC_LATER) out.push_back(std::string(g.module_prefix) + s);
    return out;
}

std::string module_suffix(const std::string &m) {
    size_t p = 0;
    while (p < m.size() && ((m[p] >= 'A' && m[p] <= 'Z') || (m[p] >= 'a' && m[p] <= 'z'))) p++;
    const size_t letters = p;
    while (p < m.size() && m[p] >= '0' && m[p] <= '9') p++;
    return letters && p > letters ? m.substr(p) : "";
}

const Game *game_by_id(const std::string &id) {
    if (id.empty()) return nullptr;
    for (const Game &g : games())
        if (*g.id ? id == g.id : id == g.model) return &g;
    return nullptr;
}

const Game &game_of(const std::string &model) {
    const std::vector<Game> &t = games();
    // a call with no model: pop'n 9's (the first row), as the server always took it
    if (model.empty()) return t.front();
    for (bool wild : {false, true})
        for (const Game &g : t)
            if (matches(g.model, model, wild)) return g;
    return t.back();
}

}  // namespace hk::ea
