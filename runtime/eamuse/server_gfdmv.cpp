// SPDX-License-Identifier: GPL-3.0-or-later
// GuitarFreaks and DrumMania V on the server (Konami Python 2) (server.h).
#include "eamuse/server_parts.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <set>
#include <ctime>

namespace hk::ea {

// GuitarFreaks and DrumMania V's e-AMUSEMENT (Konami Python 2; PLAN.md section 23, PY-6), from the
// game's eamservice module (in RAM: cardutil 0x94a348-0x94ae38):
//   - cardutil check <card no= id=/> per player: <card no state pass gdid skill/>; state 0 unknown
//     (not usable), 1 a card not registered (the game registers it; it keeps the gdid given), 2
//     registered (pass: its 4 digits, gdid: the player's 10 characters, skill), 3 expired
//     (0x1c483e4);
//   - cardutil regist <card cardid gdid kingid name pass type update/>, after the name and the
//     password (gdid: check's): <cardutil status/> and nothing in it (a child fails it, 0x94adbc);
//   - gameend regist, after a credit: <stage mode nr><stagedata no netid newmusic extra king clear/>..
//     </stage>, <player session king tower><playerdata card gdid styles perfect great good poor miss
//     time point><stage seqmode flags score spoint skill combo perc bscore/>..</playerdata></player>:
//     kept per player (gfdmv-GDID: each song and seqmode's best score, combo and percentage, its
//     plays; the player's totals);
//   - gametop get <player gdid/>: <player gdid styles pticket ticket> (0x94c990), in it musicdata
//     (base64, the game's own record of the player's songs, decoded into the player's 0xc30 bytes,
//     0x94c890), towerdata, kingdata. Answered without them so far.
// The players: the data folder's gfdmv-cards (cardid, gdid, then pass and name once registered);
// gdids are 10 digits from gfdmv-gdid.
std::string Server::answer_gfdmv(const Game &, const Call &, const Request &r, const std::string &) {
    const std::string &m = r.module, &f = r.method;
    std::vector<Record> cards = from_lines(read_file("gfdmv-cards"));
    auto find = [&](const std::string &id) -> Record * {
        for (Record &c : cards)
            if (attr(c, "cardid") == id) return &c;
        return nullptr;
    };
    auto save = [&] {
        std::string t;
        for (const Record &e : cards) t += to_line(e);
        write_file("gfdmv-cards", t);
    };
    auto children = [&](const char *name) {
        std::vector<std::map<std::string, std::string>> out;
        const std::string tag = std::string("<") + name;
        for (size_t at = r.inner.find(tag); at != std::string::npos; at = r.inner.find(tag, at + 1)) {
            std::map<std::string, std::string> c;
            bool sc = false;
            read_attrs(r.inner, at + tag.size(), c, &sc);
            out.push_back(std::move(c));
        }
        return out;
    };
    if (m == "cardutil" && f == "check") {
        std::string out = "<cardutil status=\"0\">";
        bool added = false;
        for (const auto &c : children("card")) {
            const std::string id = attr(c, "id");
            Record *p = find(id);
            if (!p && safe_name(id)) {   // a new card: its player's gdid now (regist sends it back)
                uint64_t next = 1;
                if (std::string n = read_file("gfdmv-gdid"); !n.empty()) next = std::strtoull(n.c_str(), nullptr, 10);
                write_file("gfdmv-gdid", std::to_string(next + 1) + "\n");
                char gdid[16];
                snprintf(gdid, sizeof gdid, "%010llu", (unsigned long long)next);
                cards.push_back({{"cardid", id}, {"gdid", gdid}});
                p = &cards.back();
                added = true;
            }
            out += "<card no=\"" + esc(attr(c, "no")) + "\"";
            if (!p) out += " state=\"0\"";
            else if (attr(*p, "pass").empty()) out += " state=\"1\" gdid=\"" + esc(attr(*p, "gdid")) + "\"";
            else out += " state=\"2\" pass=\"" + esc(attr(*p, "pass")) + "\" gdid=\"" + esc(attr(*p, "gdid")) + "\" skill=\"" +
                        esc(attr(*p, "skill").empty() ? "0" : attr(*p, "skill")) + "\"";
            out += "/>";
        }
        if (added) save();
        return out + "</cardutil>";
    }
    if (m == "cardutil" && f == "regist") {
        for (const auto &c : children("card")) {
            Record *p = find(attr(c, "cardid"));
            if (!p) return "<cardutil status=\"1\"/>";
            (*p)["name"] = attr(c, "name");
            (*p)["pass"] = attr(c, "pass");
            if ((*p)["skill"].empty()) (*p)["skill"] = "0";
        }
        save();
        return "<cardutil status=\"0\"/>";
    }
    if (m == "gameend" && f == "regist") {
        // the songs, in the order the players' stages follow
        std::vector<std::string> netids;
        for (const auto &d : children("stagedata")) netids.push_back(attr(d, "netid"));
        for (size_t pd = r.inner.find("<playerdata"); pd != std::string::npos; pd = r.inner.find("<playerdata", pd + 1)) {
            std::map<std::string, std::string> pl;
            bool sc = false;
            read_attrs(r.inner, pd + 11, pl, &sc);
            const std::string gdid = attr(pl, "gdid");
            if (!safe_name(gdid)) continue;
            std::vector<Record> recs = from_lines(read_file("gfdmv-" + gdid));
            Record *total = nullptr;
            for (Record &e : recs)
                if (attr(e, "total") == "1") total = &e;
            if (!total) { recs.push_back({{"total", "1"}}); total = &recs.back(); }
            for (const char *k : {"perfect", "great", "good", "poor", "miss", "time", "point"})
                (*total)[k] = std::to_string(num(*total, k) + num(pl, k));
            (*total)["styles"] = attr(pl, "styles");
            (*total)["credits"] = std::to_string(num(*total, "credits") + 1);
            const size_t end = r.inner.find("</playerdata>", pd);
            size_t i = 0;
            for (size_t st = r.inner.find("<stage ", pd); st != std::string::npos && st < end; st = r.inner.find("<stage ", st + 1), i++) {
                std::map<std::string, std::string> sd;
                read_attrs(r.inner, st + 6, sd, &sc);
                if (i >= netids.size()) break;
                Record *best = nullptr;
                for (Record &e : recs)
                    if (attr(e, "netid") == netids[i] && attr(e, "seqmode") == attr(sd, "seqmode")) best = &e;
                if (!best) { recs.push_back({{"netid", netids[i]}, {"seqmode", attr(sd, "seqmode")}}); best = &recs.back(); }
                for (const char *k : {"score", "combo", "perc", "skill"})
                    if (num(sd, k) > num(*best, k) || attr(*best, k).empty()) (*best)[k] = attr(sd, k);
                (*best)["plays"] = std::to_string(num(*best, "plays") + 1);
            }
            std::string t;
            for (const Record &e : recs) t += to_line(e);
            write_file("gfdmv-" + gdid, t);
        }
        return "<gameend status=\"0\"/>";
    }
    if (m == "gametop" && f == "get") {
        std::string out = "<gametop status=\"0\">";
        for (const auto &pl : children("player")) {
            const std::string gdid = attr(pl, "gdid");
            std::string styles = "0";
            if (safe_name(gdid))
                for (const Record &e : from_lines(read_file("gfdmv-" + gdid)))
                    if (attr(e, "total") == "1" && !attr(e, "styles").empty()) styles = attr(e, "styles");
            out += "<player gdid=\"" + esc(gdid) + "\" styles=\"" + esc(styles) + "\" pticket=\"0\" ticket=\"0\"/>";
        }
        return out + "</gametop>";
    }
    return "";
}

}  // namespace hk::ea
