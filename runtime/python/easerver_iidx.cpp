// SPDX-License-Identifier: GPL-3.0-or-later
// beatmania IIDX 9th style (model C02, a PC game: runtime/pc/, PLAN.md section 33) on the server
// (easerver.h): its module "C02", the scores and the rankings.
//
// A call is <C02 method="music" command="reg CARD 1 908 12 5 7 0"/>: a method and a command line
// (sonet.dll's builders, one per request kind, print its fields in turn; names in EUC-JP). The
// answer names what it answers (method="musicreg": the method and the command's first word), its
// status a word (SOK), its values attributes; lists are child elements (any name), read in turn
// into the game's arrays without a bound, so never more than the game asked for. What the game
// asks and what it does with the answers (bm2dx JAH):
//   music reg CARD chart song pgreat great djlevel cleared, after each stage: chart 0-2 SP
//     normal/hyper/another, 3-5 DP; DJ level 0 AAA ... 7 F (bm2dx 0x425800: 8 - 9 * EX / (2 *
//     notes), EX = 2 * pgreat + great). Answer mid, clid, bestScore (1: a new best; the game's
//     only use of the answer).
//   music getrank CARD 120: the player's DJ level and clear by chart (music select's marks,
//     0x415e70), with the weekly song (mid, wid). A child per chart (cl="chart") whose text is 4
//     hex digits a song: song << 5 | DJ level << 2 | cleared << 1. 1050 songs at most (0x189c bytes).
//   music getranking 20: the most played songs, a child each (mid).
//   weekly getmid: the week's song (mid) and the week (wid); weekly entry CARD wid chart pgreat
//     great NAME SHOP PREFECTURE opt, after playing it: answer bstscr.
//   ranking: EXPERT mode's courses (12 a class, the course's number its id in the program's
//     table; STYLISH is class 0 course 0). getinfo: the ranking (rid) and the six courses attract
//     shows (c0-c5); getranker RID CLID 20 COID: a course's top 20 (a child each: pid the
//     prefecture, gnum great, pgnum pgreat, opt, name, opname the shop, name2, udate); entry after a
//     course, when the player agrees to send it (the PASSWORD FOR INTERNET RANKING screen); getme
//     CARD 120 when EXPERT is chosen: the card's last try on each course.
// The week's song and the courses were Konami's server's to choose: here a file in the server's
// folder (iidx9-weekly-song: a song number; iidx9-courses: rid c0 ... c5) or, without one, a
// song played on this server (a different one each week) and courses 0-5.
#include "python/easerver_parts.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <sstream>

namespace hk::py {

namespace {

std::vector<std::string> words(const std::string &s) {
    std::vector<std::string> w;
    std::istringstream in(s);
    for (std::string x; in >> x;) w.push_back(x);
    return w;
}

long field(const Record &r, const char *k) { return num(r, k); }
std::string get(const Record &r, const char *k) {
    const auto it = r.find(k);
    return it == r.end() ? "" : it->second;
}
long ex(const Record &r) { return 2 * field(r, "pg") + field(r, "gr"); }

std::string ok(const std::string &method, const std::string &command, const std::string &attrs = "",
               const std::string &children = "") {
    const std::string head = "<C02 status=\"SOK\" method=\"" + esc(method + command) + "\"" + attrs;
    return children.empty() ? head + "/>" : head + ">" + children + "</C02>";
}

std::string a(const char *name, long v) { return std::string(" ") + name + "=\"" + std::to_string(v) + "\""; }
std::string a(const char *name, const std::string &v) { return std::string(" ") + name + "=\"" + esc(v) + "\""; }

}  // namespace

std::string EamuseServer::answer_c02(const Request &r) {
    const std::string method = r.method;
    const std::vector<std::string> w = words(attr(r.attrs, "command"));
    const std::string cmd = w.empty() ? "" : w[0];
    auto arg = [&](size_t i) { return i < w.size() ? w[i] : std::string(); };
    auto iarg = [&](size_t i) { return std::strtol(arg(i).c_str(), nullptr, 10); };
    const time_t t = now ? (time_t)now() : time(nullptr);
    const long wid = (long)((t / 86400 + 3) / 7);   // weeks from Monday 1969-12-29
    char date[16];
    {
        struct tm tmv;
#ifdef _WIN32
        gmtime_s(&tmv, &t);
#else
        gmtime_r(&t, &tmv);
#endif
        strftime(date, sizeof date, "%Y%m%d", &tmv);
    }

    std::vector<Record> scores = from_lines(read_file("iidx9-scores"));
    // the week's song: the operator's, else one of the songs played here, a different one a week
    auto weekly_song = [&]() -> long {
        if (const std::string s = read_file("iidx9-weekly-song"); !s.empty()) return std::strtol(s.c_str(), nullptr, 10);
        std::set<long> songs;
        for (const Record &s : scores) songs.insert(field(s, "mid"));
        if (songs.empty()) return 0;
        return *std::next(songs.begin(), wid % (long)songs.size());
    };

    if (method == "music") {
        const std::string card = arg(1);
        if (cmd == "reg") {   // reg CARD chart song pgreat great djlevel cleared
            const long cl = iarg(2), mid = iarg(3), pg = iarg(4), gr = iarg(5), lv = iarg(6), clear = iarg(7);
            if (!safe_name(card) || mid <= 0 || mid > 2047 || cl < 0 || cl > 5)
                return ok(method, cmd, a("mid", mid) + a("clid", cl) + a("bestScore", 0L));
            bool best = false, found = false;
            for (Record &s : scores) {
                if (s["card"] != card || field(s, "mid") != mid || field(s, "cl") != cl) continue;
                found = true;
                if (2 * pg + gr > ex(s)) {
                    best = true;
                    s["pg"] = std::to_string(pg);
                    s["gr"] = std::to_string(gr);
                }
                s["lv"] = std::to_string(std::min(lv, field(s, "lv")));
                s["clear"] = std::to_string(clear || field(s, "clear"));
                s["plays"] = std::to_string(field(s, "plays") + 1);
            }
            if (!found) {
                best = 2 * pg + gr > 0;
                scores.push_back({{"card", card}, {"mid", std::to_string(mid)}, {"cl", std::to_string(cl)},
                                  {"pg", std::to_string(pg)}, {"gr", std::to_string(gr)}, {"lv", std::to_string(lv)},
                                  {"clear", std::to_string(clear ? 1 : 0)}, {"plays", "1"}});
            }
            std::string text;
            for (const Record &s : scores) text += to_line(s);
            write_file("iidx9-scores", text);
            return ok(method, cmd, a("mid", mid) + a("clid", cl) + a("bestScore", best ? 1L : 0L));
        }
        if (cmd == "getrank") {   // getrank CARD n: by chart, the songs' DJ level and clear
            std::string children;
            int n = 0;
            for (long cl = 0; cl < 6; cl++) {
                std::string text;
                for (const Record &s : scores) {
                    if (get(s, "card") != card || field(s, "cl") != cl || n >= 1050) continue;
                    char h[8];
                    snprintf(h, sizeof h, "%04lx", (field(s, "mid") << 5 | (field(s, "lv") & 7) << 2 | (field(s, "clear") ? 2 : 0)) & 0xffff);
                    text += h;
                    n++;
                    if (text.size() >= 256) {   // a child for each 64 songs: no text split by the game's reader
                        children += "<rank" + a("cl", cl) + ">" + text + "</rank>";
                        text.clear();
                    }
                }
                if (!text.empty()) children += "<rank" + a("cl", cl) + ">" + text + "</rank>";
            }
            return ok(method, cmd, a("mid", weekly_song()) + a("wid", wid), children);
        }
        if (cmd == "getranking") {   // getranking n: the most played songs
            std::map<long, long> plays;
            for (const Record &s : scores) plays[field(s, "mid")] += field(s, "plays");
            std::vector<std::pair<long, long>> top;
            for (const auto &[mid, p] : plays) top.push_back({p, mid});
            std::stable_sort(top.begin(), top.end(), [](const auto &x, const auto &y) { return x.first > y.first; });
            std::string children;
            const long n = std::clamp(iarg(1), 0L, 100L);
            for (long i = 0; i < (long)top.size() && i < n; i++) children += "<music" + a("mid", top[i].second) + "/>";
            return ok(method, cmd, "", children);
        }
        return ok(method, cmd);
    }

    if (method == "weekly") {
        std::vector<Record> entries = from_lines(read_file("iidx9-weekly"));
        const long song = weekly_song();
        if (cmd == "getmid") return ok(method, cmd, a("mid", song) + a("wid", wid));
        if (cmd == "getinfo")
            return ok(method, cmd, a("nowtim", (long)t),
                      "<week" + a("wid", wid) + a("mid", song) + a("since", wid * 7 * 86400L - 3 * 86400L) +
                          a("till", (wid + 1) * 7 * 86400L - 3 * 86400L) + a("cmnt", "") + "/>");
        if (cmd == "entry") {   // entry CARD wid chart pgreat great NAME SHOP PREFECTURE opt
            const std::string card = arg(1);
            Record e = {{"card", card}, {"wid", arg(2)}, {"cl", arg(3)}, {"pg", arg(4)}, {"gr", arg(5)}, {"name", arg(6)},
                        {"shop", arg(7)}, {"pref", arg(8)}, {"opt", arg(9)}, {"date", date}};
            long best = ex(e);
            bool found = false;
            for (Record &x : entries) {
                if (x["card"] != card || x["wid"] != e["wid"]) continue;
                found = true;
                if (ex(e) > ex(x)) x = e;
                best = ex(x);
            }
            if (!found && safe_name(card)) entries.push_back(e);
            std::string text;
            for (const Record &x : entries) text += to_line(x);
            write_file("iidx9-weekly", text);
            return ok(method, cmd, a("bstscr", best));
        }
        if (cmd == "getranker" || cmd == "getme") {
            std::vector<Record> list;
            for (const Record &x : entries)
                if (get(x, "wid") == std::to_string(wid) && (cmd == "getranker" || get(x, "card") == arg(1))) list.push_back(x);
            std::stable_sort(list.begin(), list.end(), [](const Record &x, const Record &y) { return ex(x) > ex(y); });
            std::string children;
            for (size_t i = 0; i < list.size() && i < (cmd == "getme" ? 1 : 20); i++) {
                const Record &x = list[i];
                children += cmd == "getme"
                    ? "<me" + a("wid", wid) + a("clid", field(x, "cl")) + a("pgnum", field(x, "pg")) + a("gnum", field(x, "gr")) + "/>"
                    : "<ranker" + a("pid", field(x, "pref")) + a("gnum", field(x, "gr")) + a("pgnum", field(x, "pg")) +
                          a("opt", field(x, "opt")) + a("name", get(x, "name")) + a("opname", get(x, "shop")) + a("name2", "") +
                          a("udate", field(x, "date")) + "/>";
            }
            return ok(method, cmd, "", children);
        }
        return ok(method, cmd);
    }

    if (method == "ranking") {
        // the ranking and its six courses: the operator's, else one ranking of courses 0-5
        std::vector<std::string> info = words(read_file("iidx9-courses"));
        if (info.size() < 7) info = {"1", "0", "1", "2", "3", "4", "5"};
        const std::string rid = info[0];
        std::vector<Record> entries = from_lines(read_file("iidx9-ranking"));
        if (cmd == "getinfo") {
            std::string at = a("rid", std::strtol(rid.c_str(), nullptr, 10));
            static const char *C[] = {"c0", "c1", "c2", "c3", "c4", "c5"};
            for (int i = 0; i < 6; i++) at += a(C[i], std::clamp(std::strtol(info[1 + i].c_str(), nullptr, 10), 0L, 11L));
            return ok(method, cmd, at);
        }
        // an entry's place: the ranking, the class (clid) and the course (coid)
        auto same = [&](const Record &x, const std::string &clid, const std::string &coid) {
            return get(x, "rid") == rid && get(x, "clid") == clid && get(x, "coid") == coid;
        };
        if (cmd == "entry") {
            // entry CARD clid coid pgreat great NAME PIN opt SHOP n PREFECTURE stages [CARD2 NAME2]
            // (0x436960, 0x10004d50; two players: both cards and names, their counts added up).
            // The PIN (the game's password seed) isn't kept.
            Record e = {{"rid", rid}, {"date", date}, {"card", arg(1)}, {"clid", arg(2)}, {"coid", arg(3)},
                        {"pg", arg(4)}, {"gr", arg(5)}, {"name", arg(6)}, {"opt", arg(8)}, {"shop", arg(9)},
                        {"n", arg(10)}, {"pref", arg(11)}, {"stages", arg(12)}, {"card2", arg(13)}, {"name2", arg(14)}};
            if (!safe_name(e["card"])) return ok(method, cmd, a("rentry", 0L) + a("bstscr", 0L) + a("bstranking", 0L) + a("topranker", 0L));
            entries.push_back(e);
            std::string text;
            for (const Record &x : entries) text += to_line(x);
            write_file("iidx9-ranking", text);
            long count = 0, best = 0, top = 0;
            for (const Record &x : entries)
                if (same(x, e["clid"], e["coid"])) {
                    count++;
                    top = std::max(top, ex(x));
                    if (get(x, "card") == e["card"]) best = std::max(best, ex(x));
                }
            long place = 1;
            std::set<std::string> ahead;
            for (const Record &x : entries)
                if (same(x, e["clid"], e["coid"]) && ex(x) > best && ahead.insert(get(x, "card")).second) place++;
            return ok(method, cmd, a("rentry", count) + a("bstscr", best) + a("bstranking", place) + a("topranker", top));
        }
        if (cmd == "getranker") {   // getranker RID CLID 20 COID: the course's top 20, a card's best each
            std::map<std::string, Record> best;
            for (const Record &x : entries)
                if (same(x, arg(2), arg(4))) {
                    auto it = best.find(get(x, "card"));
                    if (it == best.end() || ex(x) > ex(it->second)) best[get(x, "card")] = x;
                }
            std::vector<Record> list;
            for (const auto &[c, x] : best) list.push_back(x);
            std::stable_sort(list.begin(), list.end(), [](const Record &x, const Record &y) { return ex(x) > ex(y); });
            std::string children;
            for (size_t i = 0; i < list.size() && i < (size_t)std::clamp(iarg(3), 0L, 20L); i++) {
                const Record &x = list[i];
                children += "<ranker" + a("pid", field(x, "pref")) + a("gnum", field(x, "gr")) + a("pgnum", field(x, "pg")) +
                            a("opt", field(x, "opt")) + a("name", get(x, "name")) + a("opname", get(x, "shop")) +
                            a("name2", get(x, "name2")) + a("udate", field(x, "date")) + "/>";
            }
            return ok(method, cmd, "", children);
        }
        if (cmd == "getme") {
            // getme CARD 120: the card's last try on each course (course select's 前回のスコア), its
            // tries and whether one went through all five stages; the game's array is 12 courses a
            // class (0x10005510: clid * 12 + coid), 120 in all
            std::map<std::pair<long, long>, std::pair<Record, long>> last;
            for (const Record &x : entries) {
                if (get(x, "rid") != rid || get(x, "card") != arg(1)) continue;
                const long clid = field(x, "clid"), coid = field(x, "coid");
                if (clid < 0 || coid < 0 || coid >= 12 || clid * 12 + coid >= std::clamp(iarg(2), 0L, 120L)) continue;
                auto &[r, tries] = last[{clid, coid}];
                r = x;
                tries++;
            }
            std::string children;
            for (const auto &[k, v] : last)
                children += "<course" + a("clid", k.first) + a("coid", k.second) + a("gnum", field(v.first, "gr")) +
                            a("pgnum", field(v.first, "pg")) + a("tcnt", v.second) + a("cflg", field(v.first, "stages") >= 5 ? 1L : 0L) + "/>";
            return ok(method, cmd, "", children);
        }
        return ok(method, cmd);
    }
    return ok(method, cmd);   // grade raised
}

// --- RED, HAPPY SKY and DistorteD (E11, ECO, FDD): the same kind of call, <E11 method="pc"
// command="get CARD 0"/>, from libjnet, built into bm2dx.exe (RED JAD: its method tables at
// 0x4ea288 shop, 0x4ea318 grade, 0x4ea368 weekly, 0x4ea3b8 ranking, 0x4ea528 music, 0x4ea698 pc:
// per command its handlers by element depth, its name and its field count). An answer is matched
// to its command by method, the method and the command's first word run together (musicgetrank).
std::string EamuseServer::answer_iidx_red(const std::string &game, const Request &r) {
    const std::string method = r.method;
    const std::vector<std::string> w = words(attr(r.attrs, "command"));
    const std::string cmd = w.empty() ? "" : w[0];
    auto ok = [&](const std::string &attrs = "", const std::string &children = "") {
        const std::string head = "<" + game + " status=\"SOK\" method=\"" + esc(method + cmd) + "\"" + attrs;
        return children.empty() ? head + "/>" : head + ">" + children + "</" + game + ">";
    };
    auto arg = [&](size_t i) { return i < w.size() ? w[i] : std::string(); };

    // The players (pc): a card's record, kept here (new in RED: 9th style kept it on the card).
    // pc reg CARD NAME PIN x PREFECTURE y, after NAME ENTRY: the answer's id is the player's IIDX
    // ID (8 digits, shown at once). pc get CARD n, as a card is read: <pc> with the record (id, idstr,
    // name, pass the PIN, pid the prefecture, ...; RED JAD 0x47a120 reads it), then the player's
    // grades, EX, skins, rivals and courses (not kept yet).
    const std::string players_file = "iidx-" + game + "-players";
    std::vector<Record> players = from_lines(read_file(players_file));
    auto player = [&](const std::string &card) -> Record * {
        for (Record &p : players)
            if (get(p, "card") == card) return &p;
        return nullptr;
    };
    auto save_players = [&] {
        std::string text;
        for (const Record &p : players) text += to_line(p);
        write_file(players_file, text);
    };
    if (method == "pc" && cmd == "reg") {
        const std::string card = arg(1);
        if (!safe_name(card)) return "<" + game + " status=\"EPARAM\" method=\"pcreg\"/>";
        Record *p = player(card);
        if (!p) {
            long id = 10000000;   // IIDX IDs in turn, from 1000-0001
            for (const Record &x : players) id = std::max(id, field(x, "id"));
            players.push_back({{"card", card}, {"id", std::to_string(id + 1)}});
            p = &players.back();
        }
        (*p)["name"] = arg(2), (*p)["pass"] = arg(3), (*p)["r3"] = arg(4), (*p)["pid"] = arg(5), (*p)["r5"] = arg(6);
        const std::string id = get(*p, "id");
        save_players();
        return ok(a("id", id));
    }
    if (method == "pc" && cmd == "get") {
        const Record *p = player(arg(1));
        if (!p) return "<" + game + " status=\"ENOTFOUND\" method=\"pcget\"/>";
        const std::string id = get(*p, "id");
        auto dj = [&](const char *k) { const std::string v = get(*p, k); return v.empty() ? std::string("0.000000") : v; };
        return ok("", "<pc" + a("id", id) + a("idstr", id.substr(0, 4) + "-" + id.substr(4)) + a("name", get(*p, "name")) +
                          a("pass", get(*p, "pass")) + a("cpass", "") + a("spnum", field(*p, "spnum")) +
                          a("dpnum", field(*p, "dpnum")) + a("sach", dj("sach")) + a("dach", dj("dach")) +
                          a("pid", field(*p, "pid")) + "/>");
    }
    // the rest are by IIDX ID (the 8 digits pc reg gave)
    auto by_id = [&](const std::string &id) -> Record * {
        for (Record &p : players)
            if (get(p, "id") == id) return &p;
        return nullptr;
    };
    if (method == "pc" && cmd == "save") {
        // save ID x SP-DJ-POINTS DP-DJ-POINTS SP-PLAYS DP-PLAYS ... at the credit's end: kept for pc get
        // (sach and dach the DJ points, as floats; spnum and dpnum the plays)
        if (Record *p = by_id(arg(1))) {
            (*p)["sach"] = arg(3), (*p)["dach"] = arg(4), (*p)["spnum"] = arg(5), (*p)["dpnum"] = arg(6);
            (*p)["save"] = attr(r.attrs, "command");
            save_players();
        }
        return ok();
    }

    // The scores (music): music reg ID chart song pgreat great djlevel cleared PREFECTURE SHOP
    // after each stage (chart 0-2 SP normal/hyper/another, 3-5 DP; DJ level 0 AAA ... 7 F), kept
    // by IIDX ID, song and chart (the best EX, the best DJ level, cleared ever, plays).
    // music getrank ID n, as a card is read: per chart a child cl="chart" whose text is 5 letters
    // a song from "0123456789:;", a-z, A-Z (6 bits each, the first highest): the song (bits 19-29),
    // the DJ level (16-18), the clear (13-15: 0 NO PLAY, 2 EASY CLEAR, 5 FULL COMBO as music select
    // shows them), the EX score (0-12); RED JAD 0x479720, 1200 at most
    const std::string scores_file = "iidx-" + game + "-scores";
    std::vector<Record> scores = from_lines(read_file(scores_file));
    if (method == "music" && cmd == "reg") {
        const std::string id = arg(1);
        const long cl = std::strtol(arg(2).c_str(), nullptr, 10), mid = std::strtol(arg(3).c_str(), nullptr, 10);
        const long pg = std::strtol(arg(4).c_str(), nullptr, 10), gr = std::strtol(arg(5).c_str(), nullptr, 10);
        const long lv = std::strtol(arg(6).c_str(), nullptr, 10), clear = std::strtol(arg(7).c_str(), nullptr, 10);
        if (!safe_name(id) || mid <= 0 || mid > 2047 || cl < 0 || cl > 5) return ok(a("mid", mid) + a("clid", cl));
        bool found = false;
        for (Record &x : scores) {
            if (get(x, "id") != id || field(x, "mid") != mid || field(x, "cl") != cl) continue;
            found = true;
            if (2 * pg + gr > ex(x)) x["pg"] = std::to_string(pg), x["gr"] = std::to_string(gr);
            x["lv"] = std::to_string(std::min(lv, field(x, "lv")));
            x["clear"] = std::to_string(std::max(clear, field(x, "clear")));
            x["plays"] = std::to_string(field(x, "plays") + 1);
        }
        if (!found)
            scores.push_back({{"id", id}, {"mid", std::to_string(mid)}, {"cl", std::to_string(cl)}, {"pg", std::to_string(pg)},
                              {"gr", std::to_string(gr)}, {"lv", std::to_string(lv)}, {"clear", std::to_string(clear)}, {"plays", "1"}});
        std::string text;
        for (const Record &x : scores) text += to_line(x);
        write_file(scores_file, text);
        return ok(a("mid", mid) + a("clid", cl));
    }
    if (method == "music" && cmd == "getrank") {
        static const char letters[] = "0123456789:;abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
        std::string children;
        int n = 0;
        for (long cl = 0; cl < 6; cl++) {
            std::string text;
            for (const Record &x : scores) {
                if (get(x, "id") != arg(1) || field(x, "cl") != cl || n >= 1200) continue;
                const uint32_t v = (uint32_t)(field(x, "mid") & 0x7ff) << 19 | (uint32_t)(field(x, "lv") & 7) << 16 |
                                   (uint32_t)(field(x, "clear") & 7) << 13 | (uint32_t)std::min(ex(x), 0x1fffL);
                for (int k = 4; k >= 0; k--) text += letters[v >> (6 * k) & 63];
                n++;
            }
            if (!text.empty()) children += "<rank" + a("cl", cl) + ">" + text + "</rank>";
        }
        return ok("", children);
    }
    if (method == "grade" && cmd == "raised") {   // raised ID ... SHOP PREFECTURE, after a dan course: kept
        if (Record *p = by_id(arg(1))) {
            (*p)["grade"] = attr(r.attrs, "command");
            save_players();
        }
        return ok();
    }
    return ok();
}

// --- DistorteD (FDD): libjnet again (JAG: tables at 0x5e2a68 shop ... 0x5e2c34 pc), each kind of
// call a module of its own with attributes, <pc method="get" ver="1" rid="REFID"/>; an answer is
// <pc status="0" method="pcget">: status a number this time (0 fine), method as RED's (an answer
// without it isn't read: the call fails). The pass is cardmng's (easerver_popn.cpp): a new one is
// registered by the game (its PIN twice, getrefid), then its DJ name entered and sent with pc reg.
//   pc reg rid name pid cid: the player made; the answer's id is the IIDX ID
//   pc get rid: the player (JAG 0x4ce840: <pc id idstr name pid spnum dpnum sach dach sflg0-2 gno hdsd
//     sp_opt dp_opt dp_opt2 mcomb ncomb mode/>, then grade, ex, skin, rlist, ocrs, weekly: not kept yet)
//   pc save iidxid cltype ... after the credit: its fields kept and given back by pc get
std::string EamuseServer::answer_iidx_fdd(const std::string &game, const Request &r) {
    // DistorteD's calls (FDD) and GOLD's (GLD, over libavs): each answer's fields as the game's reply
    // parsers read them (GOLD 0x495e70 pc.get, 0x497b80 pc.reg, ...: docs/EAMUSE3.md); a field or
    // element they read and don't find fails the call (the network library's status -10), and the
    // player is played as a guest ("e-AMUSEMENT PASS not used")
    const std::string &mod = r.module, method = r.method;
    auto ok = [&](const std::string &attrs = "", const std::string &children = "") {
        const std::string head = "<" + mod + " status=\"0\" method=\"" + esc(mod + method) + "\"" + attrs;
        return children.empty() ? head + "/>" : head + ">" + children + "</" + mod + ">";
    };
    std::vector<Record> players = from_lines(read_file("iidx-" + game + "-players"));
    auto player = [&](const std::string &k, const std::string &v) -> Record * {
        for (Record &p : players)
            if (get(p, k.c_str()) == v) return &p;
        return nullptr;
    };
    auto save_players = [&] {
        std::string text;
        for (const Record &p : players) text += to_line(p);
        write_file("iidx-" + game + "-players", text);
    };
    auto idstr = [](const std::string &id) { return id.size() == 8 ? id.substr(0, 4) + "-" + id.substr(4) : id; };
    if (mod == "pc" && method == "reg") {   // the IIDX ID, and as written on the screen
        const std::string rid = attr(r.attrs, "rid");
        if (!safe_name(rid)) return "<pc status=\"1\" method=\"pcreg\"/>";
        Record *p = player("rid", rid);
        if (!p) {
            long id = 10000000;   // IIDX IDs in turn, from 1000-0001
            for (const Record &x : players) id = std::max(id, field(x, "id"));
            players.push_back({{"rid", rid}, {"id", std::to_string(id + 1)}});
            p = &players.back();
        }
        (*p)["name"] = attr(r.attrs, "name"), (*p)["pid"] = attr(r.attrs, "pid");
        const std::string id = get(*p, "id");
        save_players();
        return ok(a("id", id) + a("id_str", idstr(id)));
    }
    if (mod == "pc" && method == "get") {
        // pcdata (the record: each of these), grade (the dan grades, -1 none), ex, skin and rlist
        // (lists, 8 characters an entry; empty for none), gold (GOLD's points) and jnum and bjnum
        // (EMPRESS's jewels) all needed; rivals, courses and the weekly song not
        // DistorteD (JAG 0x4ce7c0): the answer itself names the player and gives the pass's PIN, its
        // record is a child named pc (hdsd, not GOLD's sdhd; no pmode), and no gold
        const Record *p = player("rid", attr(r.attrs, "rid"));
        if (!p) return "<pc status=\"1\" method=\"pcget\"/>";
        const bool fdd = game == "FDD";
        const std::string id = get(*p, "id");
        std::string attrs = a("id", id) + a("idstr", idstr(id)) + a("name", get(*p, "name")) + a("pid", field(*p, "pid"));
        for (const char *k : {"spnum", "dpnum", "sach", "dach", "sflg0", "sflg1", "sflg2", "gno", fdd ? "hdsd" : "sdhd", "sp_opt",
                              "dp_opt", "dp_opt2", "mcomb", "ncomb", "mode"})
            attrs += a(k, field(*p, k));
        const std::string sgid = get(*p, "sgid"), dgid = get(*p, "dgid");
        const std::string rest = "<grade" + a("sgid", sgid.empty() ? "-1" : sgid) + a("dgid", dgid.empty() ? "-1" : dgid) +
                                 "/><ex></ex>" + (game == "I00" ? "" : "<skin></skin>") + "<rlist></rlist>";   // (EMPRESS: skin, if given, 12 u16)
        if (fdd) {
            std::string pass;
            for (const Record &c : from_lines(read_file("cards")))
                if (attr(c, "refid") == attr(r.attrs, "rid")) pass = attr(c, "pass");
            return ok(a("name", get(*p, "name")) + a("pass", pass) + a("pid", field(*p, "pid")), "<pc" + attrs + "/>" + rest);
        }
        // (EMPRESS also the lift's length, LIFT's option)
        return ok("", "<pcdata" + attrs + a("pmode", field(*p, "pmode")) + a("lift_len", field(*p, "lift_len")) + "/>" + rest + "<gold" + a("now_g", field(*p, "now_g")) +
                          a("all_g", field(*p, "all_g")) + a("use_g", field(*p, "use_g")) + "/>" +
                          // EMPRESS's jewels (needed): the count, and 18 more by kind
                          "<jnum __type=\"s64\">" + std::to_string(field(*p, "jnum")) + "</jnum>"
                          "<bjnum __type=\"s32\" __count=\"18\">0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0</bjnum>");
    }
    if (mod == "pc" && method == "getname") {   // a pass's player, by its ref id (the takeover's)
        const Record *p = player("rid", attr(r.attrs, "rid"));
        if (!p) return "<pc status=\"1\" method=\"pcgetname\"/>";
        return ok(a("name", get(*p, "name")) + a("idstr", idstr(get(*p, "id"))) + a("pid", field(*p, "pid")));
    }
    if (mod == "pc" && method == "save") {   // by IIDX ID: the record's fields as the game sends them
        if (Record *p = player("id", attr(r.attrs, "iidxid"))) {
            for (const auto &[k, v] : r.attrs)
                if (k != "method" && k != "ver" && k != "iidxid") (*p)[k] = v;
            save_players();
        }
        return ok();
    }
    // GOLD's scores: music reg after a stage (mid, the chart clid, pgnum and gnum, rid the DJ level
    // 0 AAA ... 7 F, cflg the clear), the best kept by IIDX ID, song and chart; music getrank as a
    // pass is read: an r element per version (v: mid / 100), 5 letters a chart from "0-9:;a-zA-Z"
    // (6 bits each, the first highest): the song in the version << 23 | chart << 19 | EX << 6 |
    // clear << 3 | DJ level (GOLD 0x4947a0)
    if (game != "FDD" && mod == "music" && method == "reg") {
        const std::string iidxid = attr(r.attrs, "iidxid"), mid = attr(r.attrs, "mid"), clid = attr(r.attrs, "clid");
        if (safe_name(iidxid) && safe_name(mid) && safe_name(clid)) {
            std::vector<Record> scores = from_lines(read_file("iidx-" + game + "-scores"));
            const long ex = 2 * std::atol(attr(r.attrs, "pgnum").c_str()) + std::atol(attr(r.attrs, "gnum").c_str());
            const long rid = std::atol(attr(r.attrs, "rid").c_str()), cflg = std::atol(attr(r.attrs, "cflg").c_str());
            Record *best = nullptr;
            for (Record &x : scores)
                if (get(x, "id") == iidxid && get(x, "mid") == mid && get(x, "cl") == clid) best = &x;
            if (!best) {
                scores.push_back({{"id", iidxid}, {"mid", mid}, {"cl", clid}, {"ex", "0"}, {"rid", "7"}, {"clear", "0"}, {"plays", "0"}});
                best = &scores.back();
            }
            (*best)["ex"] = std::to_string(std::max(field(*best, "ex"), ex));
            (*best)["rid"] = std::to_string(std::min(field(*best, "rid"), rid));
            (*best)["clear"] = std::to_string(std::max(field(*best, "clear"), cflg));
            (*best)["plays"] = std::to_string(field(*best, "plays") + 1);
            std::string text;
            for (const Record &x : scores) text += to_line(x);
            write_file("iidx-" + game + "-scores", text);
        }
        return ok(a("mid", mid) + a("clid", clid) + a("crate", "0") + a("frate", "0"));
    }
    if (game != "FDD" && mod == "music" && method == "getrank") {
        static const char letters[] = "0123456789:;abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
        std::map<long, std::string> by_version;
        for (const Record &x : from_lines(read_file("iidx-" + game + "-scores"))) {
            if (get(x, "id") != attr(r.attrs, "iidxid")) continue;
            const long mid = field(x, "mid");
            const uint32_t v = (uint32_t)(mid % 100 & 0x7f) << 23 | (uint32_t)(field(x, "cl") & 15) << 19 |
                               (uint32_t)(field(x, "ex") & 0x1fff) << 6 | (uint32_t)(field(x, "clear") & 7) << 3 | (uint32_t)(field(x, "rid") & 7);
            std::string &t = by_version[mid / 100];
            for (int k = 4; k >= 0; k--) t += letters[v >> (6 * k) & 63];
        }
        std::string children;
        for (const auto &[ver, t] : by_version) children += "<r" + a("v", std::to_string(ver)) + ">" + t + "</r>";
        return ok("", children);
    }
    if (mod == "pc" && method == "common")   // the shop's VIP grade (gshop needed), nothing else
        return ok("", "<gshop vipg=\"0\"/>");
    if (mod == "pc" && method == "takeover") return ok(a("id", attr(r.attrs, "iidxid")));
    if (mod == "pc" && method == "visit")   // DJ TROOPERS: the shop's visit counts and their bonuses, none
        return ok(a("anum", "0") + a("snum", "0") + a("pnum", "0") + a("aflg", "0") + a("sflg", "0") + a("pflg", "0"));
    if (mod == "pc" && method == "lcommon") return ok(a("ls", "0"));
    if (mod == "music" && (method == "reg" || method == "play" || method == "lplay"))   // the chart's clear rate
        return ok(a("mid", attr(r.attrs, "mid")) + a("clid", attr(r.attrs, "clid")) + a("crate", "0") + a("frate", "0"));   // (frate: EMPRESS's)
    if (mod == "grade" && method == "raised") return ok(a("pnum", "0"));
    if (mod == "ranking" && method == "entry") return ok(a("anum", "0") + a("jun", "0"));
    if (mod == "shop" && method == "getname")   // the shop's name (the location's), its prefecture, its closing time
        return ok(a("opname", "HIRAKU") + a("pid", "13") + a("cls_opt", "0") + a("hr", "0") + a("mi", "0"));
    return ok();
}

}  // namespace hk::py
