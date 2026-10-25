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

}  // namespace hk::py
