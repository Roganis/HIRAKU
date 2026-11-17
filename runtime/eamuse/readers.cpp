// SPDX-License-Identifier: GPL-3.0-or-later
// The players the e-AMUSEMENT server keeps, read (readers.h): each family's own way, from the files
// its answers write (server_<family>.cpp say what each holds).
#include "eamuse/readers.h"
#include "eamuse/server_parts.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <utility>

namespace hk::ea {

std::vector<Identity> players(const Store &st) {
    std::map<std::string, std::string> found;   // card -> ref id
    auto add = [&](const std::string &card, const std::string &ref = "") {
        if (!safe_name(card)) return;
        std::string &r = found[card];
        if (r.empty()) r = ref;
    };
    for (const Record &c : from_lines(st.get(tables::cards, {}))) add(attr(c, "cardid"), attr(c, "refid"));
    for (const Key &k : st.keys(tables::userdata_c00)) add(k[0]);
    for (const Key &k : st.keys(tables::userdata)) add(k[1]);
    for (const Key &k : st.keys(tables::eemall)) add(k[0]);
    for (const Key &k : st.keys(tables::player)) add(k[1]);
    for (const Key &k : st.keys(tables::pdata))
        if (k[0] == "E12") add(k[1]);   // (pop'n 12 keeps its players by card, the later ones by ref id)
    for (const Record &c : from_lines(st.get(tables::gfdmv_cards, {}))) add(attr(c, "cardid"));
    for (const Record &c : from_lines(st.get(tables::gfdm_cards, {}))) add(attr(c, "id"));
    for (const Record &s : from_lines(st.get(tables::iidx9_scores, {}))) add(attr(s, "card"));
    for (const Key &k : st.keys(tables::iidx_players))
        for (const Record &p : from_lines(st.get(tables::iidx_players, k))) add(attr(p, "card"));
    std::vector<Identity> out;
    for (const auto &[card, ref] : found) out.push_back({card, ref});
    return out;
}

bool profile(const Store &st, const Game &g, const Identity &id, Profile &p) {
    p = Profile{};
    return g.readers && g.readers->profile && g.readers->profile(st, g, id, p);
}

std::vector<Score> scores(const Store &st, const Game &g, const Identity &id) {
    if (!g.readers || !g.readers->scores) return {};
    return g.readers->scores(st, g, id);
}

namespace {
const ClearWord *words_of(const Game &g) { return g.readers && g.readers->clear_words ? g.readers->clear_words(g) : nullptr; }
}  // namespace

Shows shows(const Game &g) {
    Shows s = g.readers && g.readers->shows ? g.readers->shows(g) : Shows{};
    const ClearWord *w = words_of(g);
    s.clear = s.clear && w && w->word;   // (a mark with no word for any value: no column)
    return s;
}

const char *clear_name(const Game &g, int clear) {
    if (clear < 0) return nullptr;
    for (const ClearWord *w = words_of(g); w && w->word; w++)
        if (w->value == clear) return w->word;
    return nullptr;
}

namespace {

// The PIN places inside pop'n records (server_popn.cpp's answer_playerdata and conv): a digit a
// byte, four bytes. pop'n 12's record by card, 13's by ref id; 11's userdata (base64) by card
struct PinAt {
    const char *model;
    size_t at, size;   // where, and the record's size (exactly; 0: at least at + 4)
};
constexpr PinAt POPN_PINS[] = {{"D11", 0x9f3, 0}, {"E12", 0x34, 0x708}, {"E13", 0x1c, 0xd6c}};

std::string popn_pin_record(const Store &st, const PinAt &p, const Identity &id) {
    const std::string m = p.model, key = m == "E13" ? id.ref : id.card;
    if (!safe_name(key)) return "";
    std::string rec = m == "D11" ? unbase64_b(st.get(tables::userdata, {m, key})) : st.get(tables::pdata, {m, key});
    if (p.size ? rec.size() != p.size : rec.size() < p.at + 4) return "";
    return rec;
}

bool four_digits(const std::string &pin) {
    if (pin.size() != 4) return false;
    for (char c : pin)
        if (c < '0' || c > '9') return false;
    return true;
}

}  // namespace

std::vector<std::string> pins(const Store &st, const Identity &id) {
    std::vector<std::string> out;
    auto add = [&](const std::string &pin) {
        if (!pin.empty() && std::find(out.begin(), out.end(), pin) == out.end()) out.push_back(pin);
    };
    if (id.card.empty()) return out;
    for (const Record &c : from_lines(st.get(tables::cards, {})))
        if (attr(c, "cardid") == id.card) add(attr(c, "pass"));
    for (const Record &c : from_lines(st.get(tables::gfdmv_cards, {})))
        if (attr(c, "cardid") == id.card) add(attr(c, "pass"));
    for (const Record &c : from_lines(st.get(tables::gfdm_cards, {})))
        if (attr(c, "id") == id.card) add(attr(c, "pass"));
    for (const Key &k : st.keys(tables::iidx_players))
        for (const Record &p : from_lines(st.get(tables::iidx_players, k)))
            if (attr(p, "card") == id.card || (!id.ref.empty() && attr(p, "rid") == id.ref)) add(attr(p, "pass"));
    for (const PinAt &p : POPN_PINS) {
        const std::string rec = popn_pin_record(st, p, id);
        if (rec.empty()) continue;
        std::string pin;
        for (size_t i = 0; i < 4; i++)
            if ((uint8_t)rec[p.at + i] <= 9) pin += (char)('0' + rec[p.at + i]);
        if (pin.size() == 4) add(pin);
    }
    return out;
}

bool reset_pin(Store &st, const Identity &id, const std::string &pin, PinReset &did) {
    did = PinReset{};
    if (!four_digits(pin) || id.card.empty()) return false;
    const auto held = st.hold();
    bool ok = true;
    // a table of records, a line each: the card's PIN changed where it has one
    auto lines = [&](const Table &t, const Key &k, const char *card, const std::string &what) {
        std::vector<Record> recs = from_lines(st.get(t, k));
        bool changed = false;
        for (Record &r : recs)
            if (attr(r, card) == id.card || (&t == &tables::iidx_players && !id.ref.empty() && attr(r, "rid") == id.ref))
                if (!attr(r, "pass").empty()) r["pass"] = pin, changed = true;
        if (!changed) return;
        std::string text;
        for (const Record &r : recs) text += to_line(r);
        if (st.put(t, k, text)) did.done.push_back(what);
        else ok = false;
    };
    lines(tables::cards, {}, "cardid", "the cards (pop'n music 13 on, IIDX DistorteD on)");
    lines(tables::gfdmv_cards, {}, "cardid", "GuitarFreaks and DrumMania V");
    lines(tables::gfdm_cards, {}, "id", "the System 573 GuitarFreaks and DrumMania");
    for (const Key &k : st.keys(tables::iidx_players)) lines(tables::iidx_players, k, "card", "beatmania IIDX " + k[0]);
    for (const PinAt &p : POPN_PINS) {
        std::string rec = popn_pin_record(st, p, id);
        if (rec.empty()) continue;
        const std::string m = p.model;
        if (m == "D11") {   // (the game's own bytes, given back as it wrote them)
            did.not_done.push_back("pop'n music 11 (its own record: whether the game checks it whole isn't known)");
            continue;
        }
        for (size_t i = 0; i < 4; i++) rec[p.at + i] = (char)(pin[i] - '0');
        if (st.put(tables::pdata, {m, m == "E13" ? id.ref : id.card}, rec)) did.done.push_back(m == "E12" ? "pop'n music 12" : "pop'n music 13");
        else ok = false;
    }
    if (!st.get(tables::userdata_c00, {id.card}).empty()) did.not_done.push_back("pop'n music 9 (the PIN's place in its record isn't known)");
    if (!st.get(tables::userdata, {"C20", id.card}).empty()) did.not_done.push_back("pop'n music 10 (the PIN's place in its record isn't known)");
    if (!st.get(tables::userdata, {"C02", id.card}).empty())
        did.not_done.push_back("beatmania IIDX 9th style (the PIN's place in its record isn't known)");
    return ok;
}

namespace {

// --- pop'n music 9-18: the record (pop'n 9-11's userdata, 12 on the game's bytes), clear marks in it
// (popn::marks_in), best scores beside it or in it (popn::scores_of). The name: 12 on, the record's
// first 13 bytes; pop'n 9-11's isn't read.
bool blob_game(const std::string &m) { return m == "C00" || m == "C20" || m == "D11"; }
std::string popn_key(const Game &g, const Identity &id) {   // pop'n 9-12 by card, 13 on by ref id
    const std::string m = g.model;
    return blob_game(m) || m == "E12" ? id.card : id.ref;
}
std::string popn_record(const Store &st, const Game &g, const Identity &id) {
    const std::string m = g.model, key = popn_key(g, id);
    if (!safe_name(key)) return "";
    if (m == "C00") return unbase64_b(st.get(tables::userdata_c00, {key}));
    if (blob_game(m)) return unbase64_b(st.get(tables::userdata, {m, key}));
    return st.get(tables::pdata, {m, key});
}
// pop'n 15-18: the songs played, a count a song (pplays-GAME-REF, server_pc.cpp: a <music> of a
// set counts one). pop'n 9-14 keep no count of plays: their records and set calls carry none
bool counts_plays(const std::string &m) { return m == "G15" || m == "H16" || m == "I17" || m == "J39"; }
bool popn_profile(const Store &st, const Game &g, const Identity &id, Profile &p) {
    const std::string rec = popn_record(st, g, id);
    if (rec.empty()) return false;
    if (!blob_game(g.model)) {
        if (rec.size() < 13) return false;
        p.name = rec.substr(0, 13);
        p.name.resize(std::strlen(p.name.c_str()));
    }
    if (counts_plays(g.model)) {   // (the songs played: a credit is one to four of them)
        p.plays = 0;
        for (const Record &e : from_lines(st.get(tables::pplays, {g.model, popn_key(g, id)}))) p.plays += std::max(0L, num(e, "count"));
    }
    return true;
}
// A chart's clear mark: pop'n 9-18 keep two bits for each of a song's four charts 0-3 (Normal,
// Hyper, 5 buttons, EX: popn::marks_in). Their other sheets (4-5 battle, 13-18's own 5-button
// charts as 6 and 7) have no mark in the record: -1. A chart's plays: none (15-18 count a song's,
// not a chart's: the profile's plays)
std::vector<Score> popn_scores(const Store &st, const Game &g, const Identity &id) {
    const std::string m = g.model, rec = popn_record(st, g, id);
    std::map<std::pair<long, long>, Score> out;
    auto mark = [&](long n, long chart) -> int {
        if (rec.empty() || chart < 0 || chart > 3 || !popn::has_song(m, (size_t)n)) return -1;
        return popn::marks_in(m, rec, (size_t)n) >> 2 * chart & 3;
    };
    for (size_t n = 0; n < popn::SONG_MAX && !rec.empty(); n++)
        for (long c = 0; c < 4; c++)
            if (const int v = mark((long)n, c); v > 0) out[{(long)n, c}] = Score{(long)n, c, -1, -1, v};
    if (!blob_game(m))   // (pop'n 9-11 keep no scores)
        for (const auto &[chart, v] : popn::scores_of(st, m, popn_key(g, id))) {
            Score &s = out[chart];
            s.song = chart.first, s.chart = chart.second, s.score = v, s.clear = mark(chart.first, chart.second);
        }
    std::vector<Score> list;
    for (const auto &[k, s] : out) list.push_back(s);
    return list;
}

// --- pop'n music, later PC versions: a player a line (phc-REF, p24-REF), scores a line a chart
// (phcs-REF, p24s-REF). The row is both games': what either keeps
bool later_profile(const Store &st, const Game &, const Identity &id, Profile &p) {
    if (!safe_name(id.ref)) return false;
    bool any = false;
    for (const Table *t : {&tables::phc, &tables::p24}) {
        const std::vector<Record> recs = from_lines(st.get(*t, {id.ref}));
        if (recs.empty() || recs[0].empty()) continue;   // (a file no field of which reads isn't a player)
        if (p.name.empty()) p.name = attr(recs[0], "name");
        p.plays = (any ? p.plays : 0) + num(recs[0], "account.total_play_cnt");
        any = true;
    }
    return any;
}
std::vector<Score> later_scores(const Store &st, const Game &, const Identity &id) {
    std::vector<Score> out;
    if (!safe_name(id.ref)) return out;
    for (const Table *t : {&tables::phcs, &tables::p24s})
        for (const Record &e : from_lines(st.get(*t, {id.ref})))
            out.push_back({num(e, "music"), num(e, "sheet"), num(e, "score"), num(e, "cnt"), (int)num(e, "clear_type")});
    return out;
}

// --- beatmania IIDX 9th style: the card's userdata (its name isn't read from it: the name the
// card gave its weekly and ranking entries), the scores a line a chart in iidx9-scores
bool iidx9_profile(const Store &st, const Game &g, const Identity &id, Profile &p) {
    if (!safe_name(id.card) || st.get(tables::userdata, {g.model, id.card}).empty()) return false;
    for (const Record &e : from_lines(st.get(tables::iidx9_weekly, {})))
        if (attr(e, "card") == id.card && !attr(e, "name").empty()) p.name = attr(e, "name");
    for (const Record &e : from_lines(st.get(tables::iidx9_ranking, {}))) {
        if (attr(e, "card") == id.card && !attr(e, "name").empty()) p.name = attr(e, "name");
        if (attr(e, "card2") == id.card && !attr(e, "name2").empty()) p.name = attr(e, "name2");
    }
    return true;
}
std::vector<Score> iidx9_scores(const Store &st, const Game &, const Identity &id) {
    std::vector<Score> out;
    for (const Record &s : from_lines(st.get(tables::iidx9_scores, {})))
        if (!id.card.empty() && attr(s, "card") == id.card)   // (the score: EX, 2 a PGREAT and 1 a GREAT)
            out.push_back({num(s, "mid"), num(s, "cl"), 2 * num(s, "pg") + num(s, "gr"), num(s, "plays"), (int)num(s, "clear")});
    return out;
}

// --- beatmania IIDX 10th style on: the players of a game in iidx-MODEL-players (10th style to
// HAPPY SKY by card, DistorteD on by the card's ref id), each with its IIDX ID; the scores by IIDX
// ID in iidx-MODEL-scores (EX as pgreat and great, or GOLD's way, ex)
bool iidx_player(const Store &st, const Game &g, const Identity &id, Record &out) {
    for (const Record &p : from_lines(st.get(tables::iidx_players, {g.model})))
        if ((!id.card.empty() && attr(p, "card") == id.card) || (!id.ref.empty() && attr(p, "rid") == id.ref)) {
            out = p;
            return true;
        }
    return false;
}
bool iidx_profile(const Store &st, const Game &g, const Identity &id, Profile &p) {
    Record r;
    if (!iidx_player(st, g, id, r)) return false;
    p.name = attr(r, "name");
    if (r.count("spnum") || r.count("dpnum")) p.plays = num(r, "spnum") + num(r, "dpnum");
    return true;
}
std::vector<Score> iidx_scores(const Store &st, const Game &g, const Identity &id) {
    std::vector<Score> out;
    Record r;
    if (!iidx_player(st, g, id, r) || attr(r, "id").empty()) return out;
    for (const Record &s : from_lines(st.get(tables::iidx_scores, {g.model})))
        if (attr(s, "id") == attr(r, "id"))
            out.push_back({num(s, "mid"), num(s, "cl"), s.count("ex") ? num(s, "ex") : 2 * num(s, "pg") + num(s, "gr"), num(s, "plays"),
                           (int)num(s, "clear")});
    return out;
}

// --- GuitarFreaks and DrumMania V: the cards in gfdmv-cards (a player once registered, with its
// name), each player's records in gfdmv-GDID (a song and sequence a line, and its totals)
bool gfdmv_card(const Store &st, const Identity &id, Record &out) {
    for (const Record &c : from_lines(st.get(tables::gfdmv_cards, {})))
        if (!id.card.empty() && attr(c, "cardid") == id.card && !attr(c, "pass").empty()) {
            out = c;
            return true;
        }
    return false;
}
bool gfdmv_profile(const Store &st, const Game &, const Identity &id, Profile &p) {
    Record c;
    if (!gfdmv_card(st, id, c)) return false;
    p.name = attr(c, "name");
    p.plays = 0;
    if (safe_name(attr(c, "gdid")))
        for (const Record &e : from_lines(st.get(tables::gfdmv, {attr(c, "gdid")})))
            if (attr(e, "total") == "1") p.plays = num(e, "credits");
    return true;
}
std::vector<Score> gfdmv_scores(const Store &st, const Game &, const Identity &id) {
    std::vector<Score> out;
    Record c;
    if (!gfdmv_card(st, id, c) || !safe_name(attr(c, "gdid"))) return out;
    for (const Record &e : from_lines(st.get(tables::gfdmv, {attr(c, "gdid")})))
        if (attr(e, "total") != "1") out.push_back({num(e, "netid"), num(e, "seqmode"), num(e, "score"), num(e, "plays"), -1});
    return out;
}

// --- the System 573 GuitarFreaks and DrumMania: the cards in gfdm-cards (registered, with a name
// and a gdid), each game's results by gdid in gfdm-GAME-GDID; a row is every version of its model
// ("?39": D39, E39...)
bool gfdm_card(const Store &st, const Identity &id, Record &out) {
    for (const Record &c : from_lines(st.get(tables::gfdm_cards, {})))
        if (!id.card.empty() && attr(c, "id") == id.card) {
            out = c;
            return true;
        }
    return false;
}
std::vector<Record> gfdm_rows(const Store &st, const Game &g, const std::string &gdid) {
    std::vector<Record> rows;
    if (!safe_name(gdid)) return rows;
    for (const Key &k : st.keys(tables::gfdm)) {
        bool mine = k[1] == gdid && k[0].size() == 3 && std::strlen(g.model) == 3;
        for (size_t i = 0; mine && i < 3; i++) mine = g.model[i] == '?' || g.model[i] == k[0][i];
        if (!mine) continue;
        for (Record &r : from_lines(st.get(tables::gfdm, k))) rows.push_back(std::move(r));
    }
    return rows;
}
bool gfdm_profile(const Store &st, const Game &g, const Identity &id, Profile &p) {
    Record c;
    if (!gfdm_card(st, id, c)) return false;
    p.name = attr(c, "name");
    p.plays = 0;
    for (const Record &r : gfdm_rows(st, g, attr(c, "gdid"))) p.plays += num(r, "plays");
    return true;
}
std::vector<Score> gfdm_scores(const Store &st, const Game &g, const Identity &id) {
    std::vector<Score> out;
    Record c;
    if (!gfdm_card(st, id, c)) return out;
    for (const Record &r : gfdm_rows(st, g, attr(c, "gdid")))   // (a score is kept for a cleared stage only)
        out.push_back({num(r, "netid"), num(r, "seqmode"), attr(r, "score").empty() ? -1 : num(r, "score"), num(r, "plays"),
                       attr(r, "clear") == "1" ? 1 : 0});
    return out;
}

// --- what each family's rows can hold, and its clear marks as words (a table a game: shows() and
// clear_name() both read it, so a game with no word has no clear column). Only the meanings the
// server's code and docs/EAMUSE3.md establish; any other value: no word (a page shows nothing)
constexpr ClearWord NO_WORDS[] = {{-1, nullptr}};   // (an empty table: the end only)
// pop'n 9-18: two bits a chart, 1 a clear (pop'n 9 keeps only that), 2-3 "the better medals": clears
// too, which medal is which isn't established, so "Cleared"; 0 with a score kept: played, not cleared
constexpr ClearWord POPN_WORDS[] = {{0, "Failed"}, {1, "Cleared"}, {2, "Cleared"}, {3, "Cleared"}, {-1, nullptr}};
Shows popn_shows(const Game &g) { return {counts_plays(g.model), false, true}; }
const ClearWord *popn_words(const Game &) { return POPN_WORDS; }
// pop'n music, later PC versions: clear_type, kept as the game sends it (its values aren't established)
Shows later_shows(const Game &) { return {true, true, true}; }
const ClearWord *later_words(const Game &) { return NO_WORDS; }
// IIDX 9th style: cleared ever (music reg's last argument); the System 573 GuitarFreaks and DrumMania:
// a stage cleared or not (a score kept for a cleared one)
constexpr ClearWord CLEARED_WORDS[] = {{0, "Failed"}, {1, "Cleared"}, {-1, nullptr}};
Shows iidx9_shows(const Game &) { return {false, true, true}; }
const ClearWord *cleared_words(const Game &) { return CLEARED_WORDS; }
// IIDX 10th style on: each game's music reg clear as it sends it (server_iidx.cpp, docs/EAMUSE3.md).
// 10th style keeps no plays (no pc save); 10th's 0 is "none" (no mark); RED's and HAPPY SKY's 1, 3, 4
// and GOLD's and DJ TROOPERS' 3-5 aren't seen; EMPRESS's values aren't established
constexpr ClearWord IIDX10_WORDS[] = {{1, "Easy clear"}, {2, "Clear"}, {3, "Full combo"}, {-1, nullptr}};
constexpr ClearWord IIDX11_WORDS[] = {{0, "No play"}, {2, "Easy clear"}, {5, "Full combo"}, {-1, nullptr}};
constexpr ClearWord IIDX13_WORDS[] = {{1, "Failed"}, {2, "Easy clear"}, {3, "Clear"}, {4, "Hard clear"}, {5, "Full combo"}, {-1, nullptr}};
constexpr ClearWord IIDX14_WORDS[] = {{0, "No play"}, {1, "Failed"}, {2, "Assist clear"}, {6, "Full combo"}, {-1, nullptr}};
Shows iidx_shows(const Game &g) { return {std::strcmp(g.model, "D01") != 0, true, true}; }
const ClearWord *iidx_words(const Game &g) {
    const std::string m = g.model;
    if (m == "D01") return IIDX10_WORDS;
    if (m == "E11" || m == "ECO") return IIDX11_WORDS;
    if (m == "FDD") return IIDX13_WORDS;
    if (m == "GLD" || m == "HDD") return IIDX14_WORDS;
    return NO_WORDS;
}
// GuitarFreaks and DrumMania V: no clear mark kept
Shows gfdmv_shows(const Game &) { return {true, true, false}; }
Shows gfdm_shows(const Game &) { return {true, true, true}; }

}  // namespace

const Readers POPN_READERS{popn_profile, popn_scores, popn_shows, popn_words};
const Readers POPN_LATER_READERS{later_profile, later_scores, later_shows, later_words};
const Readers IIDX9_READERS{iidx9_profile, iidx9_scores, iidx9_shows, cleared_words};
const Readers IIDX_READERS{iidx_profile, iidx_scores, iidx_shows, iidx_words};
const Readers GFDMV_READERS{gfdmv_profile, gfdmv_scores, gfdmv_shows, nullptr};
const Readers GFDM573_READERS{gfdm_profile, gfdm_scores, gfdm_shows, cleared_words};

}  // namespace hk::ea
