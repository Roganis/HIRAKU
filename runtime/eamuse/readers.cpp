// SPDX-License-Identifier: GPL-3.0-or-later
// The players the e-AMUSEMENT server keeps, read (readers.h): each family's own way, from the files
// its answers write (server_<family>.cpp say what each holds).
#include "eamuse/readers.h"
#include "eamuse/server_parts.h"

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
bool popn_profile(const Store &st, const Game &g, const Identity &id, Profile &p) {
    const std::string rec = popn_record(st, g, id);
    if (rec.empty()) return false;
    if (!blob_game(g.model)) {
        if (rec.size() < 13) return false;
        p.name = rec.substr(0, 13);
        p.name.resize(std::strlen(p.name.c_str()));
    }
    return true;
}
std::vector<Score> popn_scores(const Store &st, const Game &g, const Identity &id) {
    const std::string m = g.model, rec = popn_record(st, g, id);
    std::map<std::pair<long, long>, Score> out;
    auto mark = [&](long n, long chart) -> int {   // pop'n 12's four charts' marks (others: none)
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

// --- the later PC pop'n games: a player a line (phc-REF, p24-REF), scores a line a chart
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

}  // namespace

const Readers POPN_READERS{popn_profile, popn_scores};
const Readers POPN_LATER_READERS{later_profile, later_scores};
const Readers IIDX9_READERS{iidx9_profile, iidx9_scores};
const Readers IIDX_READERS{iidx_profile, iidx_scores};
const Readers GFDMV_READERS{gfdmv_profile, gfdmv_scores};
const Readers GFDM573_READERS{gfdm_profile, gfdm_scores};

}  // namespace hk::ea
