// SPDX-License-Identifier: GPL-3.0-or-later
// e-AMUSEMENT for the System 573 GuitarFreaks and DrumMania (eagfdm.h).
#include "sys573/eagfdm.h"
#include "shell/sys.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <cstdio>
#include <ctime>
#include <thread>

namespace hk::k573 {

bool Eamuse::open(const std::string &where, const std::string &dir, const std::string &log, std::function<int64_t()> now,
                  const std::string &pcbid, const std::string &key) {
    pcbid_ = pcbid;
    if (where != "local") {
        remote_ = std::make_unique<ea::Remote>(where, net::Key(key));
        if (!remote_->ok()) {
            fprintf(stderr, "--eamuse: %s is not http://host[:port][/path]\n", where.c_str());
            return false;
        }
        // with a key: said now if the server's is another (the game would only show a network error)
        if (std::string why; remote_->key().set() && !remote_->reachable(&why)) fprintf(stderr, "e-AMUSEMENT: %s\n", why.c_str());
    }
    if (!log.empty() && !(log_ = fopen(log.c_str(), "w"))) {
        fprintf(stderr, "HK_573_NET_LOG: cannot write %s\n", log.c_str());
        return false;
    }
    server_ = std::make_unique<ea::Server>(dir, &Eamuse::answer);
    server_->now = std::move(now);
    lan_ = std::make_unique<ea::Lan>([this](const ea::Lan::Target &t, const std::string &req, std::function<void(std::string)> done) {
        if (!remote_) {
            done(server_->handle(req));
            return;
        }
        // online: off the emulator's thread; the unit's recv waits for it
        std::thread([this, t, req = std::string(req), done = std::move(done)]() mutable {
            std::string error;
            if (!pcbid_.empty()) ea::set_srcid(req, pcbid_);   // (-p: this cabinet's own, not the dump's)
            std::string answer = remote_->exchange(t, req, &error);
            if (log_) {
                fprintf(log_, "online %s:%u\n  %s\n  %s\n", t.services ? remote_->host().c_str() : t.host.c_str(),
                        t.services ? remote_->port() : t.port, ea::clear(req).c_str(),
                        answer.empty() ? ("failed: " + error).c_str() : ea::clear(answer).c_str());
                fflush(log_);
            }
            done(std::move(answer));
        }).detach();
    });
    lan_->now = server_->now;
    if (log_) lan_->log = server_->log = [this](const std::string &s) { fprintf(log_, "%s\n", s.c_str()); fflush(log_); };
    if (remote_)
        printf("e-AMUSEMENT: online, %s%s%s%s\n", where.c_str(), remote_->key().set() ? ", behind the server's key" : "",
               pcbid_.empty() ? "" : ", as cabinet ", pcbid_.c_str());
    return true;
}

namespace {

// A player's results in one game (GuitarFreaks D39, DrumMania D40, ...: the model's first three
// characters), kept per card (its gdid) in "gfdm-<game>-<gdid>": a record per song (its netid)
// and sequence (seqmode, the difficulty): the best percentage, skill, score and combo.
struct Player {
    const ea::Store &store;
    std::string file;
    std::vector<ea::Record> rows;

    Player(const ea::Store &st, const std::string &game, const std::string &gdid)
        : store(st), file("gfdm-" + game + "-" + gdid), rows(ea::from_lines(st.read(file))) {}
    void save() const {
        std::string t;
        for (const ea::Record &r : rows) t += ea::to_line(r);
        store.write(file, t);
    }
    ea::Record &row(long netid, long seqmode) {
        for (ea::Record &r : rows)
            if (ea::num(r, "netid") == netid && ea::num(r, "seqmode") == seqmode) return r;
        rows.push_back({{"netid", std::to_string(netid)}, {"seqmode", std::to_string(seqmode)}, {"perc", "-1"}});
        return rows.back();
    }
    // Each song's best skill (the game sends a cleared stage's own), and the player's: the best 25
    // songs' sum. The game keeps no formula for the total; this is the GuitarFreaks/DrumMania
    // rule of the time as known, an assumption.
    std::map<long, const ea::Record *> best_per_song() const {
        std::map<long, const ea::Record *> best;
        for (const ea::Record &r : rows) {
            const long n = ea::num(r, "netid");
            if (!best.count(n) || ea::num(r, "skill") > ea::num(*best[n], "skill")) best[n] = &r;
        }
        return best;
    }
    long skill() const {
        std::vector<long> v;
        for (const auto &[n, r] : best_per_song()) v.push_back(ea::num(*r, "skill"));
        std::sort(v.rbegin(), v.rend());
        long sum = 0;
        for (size_t i = 0; i < v.size() && i < 25; i++) sum += v[i];
        return sum;
    }
};

// Every player's skill in a game, for the ranking (skill_order of skill_all)
std::vector<long> all_skills(const ea::Store &st, const std::string &game, const std::vector<ea::Record> &cards) {
    std::vector<long> v;
    for (const ea::Record &c : cards) {
        Player p(st, game, ea::attr(c, "gdid"));
        if (!p.rows.empty()) v.push_back(p.skill());
    }
    return v;
}

}  // namespace

// What GuitarFreaks and DrumMania ask beyond the common modules
std::string Eamuse::answer(ea::Server &s, const ea::Call &call, const ea::Request &r, const std::string &host) {
    const std::string &m = r.module, &f = r.method;
    if (m == "services" && f == "get") {
        // the game looks up "posevent", "pcbtracker", "numbering", "userid" and "local" (the rest);
        // and two addresses: "keepalive" (ping://, pa=, ga=; drmn10m 0x8003caa8) and "ntp" (ntp://)
        std::string a = ea::Server::services(host, {"posevent", "pcbtracker", "numbering", "userid", "local"});
        a.insert(a.rfind("</services>"), "<item name=\"keepalive\" url=\"ping://192.168.50.1/?pa=192.168.50.1&amp;ga=192.168.50.1\"/>"
                                         "<item name=\"ntp\" url=\"ntp://192.168.50.1/\"/>");
        return a;
    }
    if (m == "gamedata") {
        // the answers' elements, from the game's handlers (drmn10m: the tables at 0x80133130-0x80133270)
        const std::string ok = "<gamedata status=\"0\"><system status=\"0\"/>";
        const std::string game = call.model.substr(0, 3);
        const std::vector<ea::Element> in = ea::elements(r.inner);
        auto child = [&](const char *name) -> const ea::Element * {
            for (const ea::Element &e : in)
                if (e.name == name) return &e;
            return nullptr;
        };
        // registered cards: one record each (id, pass, name, irid, type, gdid, skill, color, ...)
        std::vector<ea::Record> cards = ea::from_lines(s.store().read("gfdm-cards"));
        auto find = [&](const std::string &id) -> ea::Record * {
            for (ea::Record &c : cards)
                if (ea::attr(c, "id") == id) return &c;
            return nullptr;
        };
        if (f == "cardcheck") {   // <card id/>: the card's status (0x8003f14c, 0x80039c00): 1 new (the
            // game asks a name and registers it), 2 registered (with pass, skill, color, gdid),
            // 3 expired ("this entry card can't be used"), 0 an error
            const ea::Element *c = child("card");
            const ea::Record *rec = c ? find(ea::attr(c->attrs, "id")) : nullptr;
            if (!rec) return ok + "<card status=\"1\"/></gamedata>";
            const long skill = Player(s.store(), game, ea::attr(*rec, "gdid")).skill();
            return ok + "<card status=\"2\" pass=\"" + ea::esc(ea::attr(*rec, "pass")) + "\" skill=\"" +
                   std::to_string(skill) + "\" color=\"" +
                   ea::esc(ea::attr(*rec, "color").empty() ? "0" : ea::attr(*rec, "color")) + "\" gdid=\"" +
                   ea::esc(ea::attr(*rec, "gdid")) + "\"/></gamedata>";
        }
        if (f == "cardregist") {   // <card id irid name pass type update pazzleno recovery/>: card status, gdid (0x8003f840)
            const ea::Element *c = child("card");
            const std::string id = c ? ea::attr(c->attrs, "id") : "";
            if (!ea::safe_name(id)) return ok + "<card status=\"1\" gdid=\"0\"/></gamedata>";
            ea::Record *rec = find(id);
            if (!rec) {
                long next = 1;
                for (const ea::Record &x : cards) next = std::max(next, ea::num(x, "gdid") + 1);
                cards.push_back({{"id", id}, {"gdid", std::to_string(next)}});
                rec = &cards.back();
            }
            for (const auto &[k, v] : c->attrs)
                if (k != "id") (*rec)[k] = v;
            std::string text;
            for (const ea::Record &x : cards) text += ea::to_line(x);
            s.store().write("gfdm-cards", text);
            return ok + "<card status=\"0\" gdid=\"" + ea::esc(ea::attr(*rec, "gdid")) + "\"/></gamedata>";
        }
        if (f == "gametop") {
            // <player no gdid recovery/> per player: its sequences and percentages, 256 songs each
            // (by netid; the handlers 0x80042144-0x80042220, the text 0x80041bd4):
            //   skillseqs: the sequence that counts for the skill, a digit a song ('x': none);
            //   musicseqs: the sequence last played, the same way;
            //   skillpercs seqmode=N: the best percentage in that sequence, two digits a song ("A0" 100).
            std::string out = ok;
            for (const ea::Element &e : in) {
                if (e.name != "player") continue;
                Player p(s.store(), game, ea::attr(e.attrs, "gdid"));
                std::string skillseqs(256, 'x'), musicseqs(256, 'x');
                std::map<long, std::string> percs;   // seqmode -> 512 characters
                for (const auto &[n, best] : p.best_per_song())
                    if (n >= 0 && n < 256 && ea::num(*best, "skill") > 0) skillseqs[n] = char('0' + ea::num(*best, "seqmode") % 10);
                for (const ea::Record &row : p.rows) {
                    const long n = ea::num(row, "netid"), sm = ea::num(row, "seqmode"), perc = ea::num(row, "perc");
                    if (n < 0 || n >= 256 || sm < 0 || sm > 5) continue;
                    if (ea::num(row, "last") > 0) musicseqs[n] = char('0' + sm);
                    std::string &pc = percs.try_emplace(sm, std::string(512, '0')).first->second;
                    if (perc >= 100) pc.replace(n * 2, 2, "A0");
                    else if (perc > 0) { char b[3]; snprintf(b, sizeof b, "%02ld", perc); pc.replace(n * 2, 2, b); }
                }
                out += "<player no=\"" + ea::esc(ea::attr(e.attrs, "no")) + "\" recovery=\"" + ea::esc(ea::attr(e.attrs, "recovery")) +
                       "\"><skillseqs>" + skillseqs + "</skillseqs><musicseqs>" + musicseqs + "</musicseqs>";
                for (const auto &[sm, pc] : percs) out += "<skillpercs seqmode=\"" + std::to_string(sm) + "\">" + pc + "</skillpercs>";
                out += "</player>";
            }
            return out + "</gamedata>";
        }
        if (f == "gameend") {
            // the songs (stagedata: stage no -> netid), then per player its stages played (no,
            // seqmode, perc, and for a cleared one score, skill, combo). The answer per player
            // (0x80040fd4): the new skill, its place among the players and their count; per stage
            // its place in that song and sequence, their count, and whether it is a new best.
            std::map<long, long> netid_of;
            std::string out = ok;
            Player *p = nullptr;
            std::unique_ptr<Player> holder;
            std::string stages, player_no, player_gdid;
            auto finish = [&] {
                if (!p) return;
                p->save();
                std::vector<long> skills = all_skills(s.store(), game, cards);
                const long mine = p->skill();
                long order = 1;
                for (long v : skills) order += v > mine;
                out += "<player no=\"" + ea::esc(player_no) + "\" skill=\"" + std::to_string(mine) + "\" skill_order=\"" +
                       std::to_string(order) + "\" skill_all=\"" + std::to_string(std::max<size_t>(skills.size(), 1)) + "\">" + stages + "</player>";
                stages.clear();
                p = nullptr;
            };
            bool in_play = false;
            for (const ea::Element &e : in) {
                if (e.name == "stage" && !p && e.attrs.count("netid")) netid_of[ea::num(e.attrs, "no")] = ea::num(e.attrs, "netid");
                else if (e.name == "player") {
                    finish();
                    holder = std::make_unique<Player>(s.store(), game, ea::attr(e.attrs, "gdid"));
                    p = holder.get();
                    player_no = ea::attr(e.attrs, "no");
                    player_gdid = ea::attr(e.attrs, "gdid");
                    for (ea::Record &row : p->rows) row.erase("last");
                    in_play = false;
                } else if (e.name == "play") in_play = true;
                else if (e.name == "stage" && p && in_play) {
                    const long n = netid_of.count(ea::num(e.attrs, "no")) ? netid_of[ea::num(e.attrs, "no")] : -1;
                    ea::Record &row = p->row(n, ea::num(e.attrs, "seqmode"));
                    row["last"] = "1";
                    row["plays"] = std::to_string(ea::num(row, "plays") + 1);
                    bool best = false;
                    if (ea::num(e.attrs, "perc") > ea::num(row, "perc")) { row["perc"] = ea::attr(e.attrs, "perc"); best = true; }
                    if (ea::attr(e.attrs, "clear") == "1") {
                        row["clear"] = "1";
                        for (const char *k : {"skill", "score", "combo"})
                            if (ea::num(e.attrs, k) > ea::num(row, k)) { row[k] = ea::attr(e.attrs, k); best |= !strcmp(k, "score"); }
                    }
                    // this song and sequence's ranking: every player's best score in it
                    long all = 1, order = 1;   // this player, and the others with a result there
                    const long score = ea::num(row, "score");
                    for (const ea::Record &c : cards) {
                        if (ea::attr(c, "gdid") == player_gdid) continue;
                        Player other(s.store(), game, ea::attr(c, "gdid"));
                        for (const ea::Record &o : other.rows)
                            if (ea::num(o, "netid") == n && ea::num(o, "seqmode") == ea::num(e.attrs, "seqmode")) {
                                all++;
                                order += ea::num(o, "score") > score;
                            }
                    }
                    stages += "<stage all=\"" + std::to_string(all) + "\" order=\"" + std::to_string(order) +
                              "\" best=\"" + (best ? "1" : "0") + "\"/>";
                }
            }
            finish();
            return out + "</gamedata>";
        }
        if (f == "dataget") {
            // <favorite nr/>: the most played songs, by every player here (0x8003fa68: nr, then
            // the netids each followed by '?', and from/to, the period, both required). The shop
            // ranking (shoprank) is left out: one cabinet has no shops to rank.
            std::map<long, long> plays;
            for (const ea::Record &c : cards)
                for (const ea::Record &row : Player(s.store(), game, ea::attr(c, "gdid")).rows)
                    if (ea::num(row, "netid") >= 0) plays[ea::num(row, "netid")] += std::max(1L, ea::num(row, "plays"));
            std::vector<std::pair<long, long>> order(plays.begin(), plays.end());
            std::stable_sort(order.begin(), order.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
            const ea::Element *fav = child("favorite");
            const size_t nr = std::min<size_t>(order.size(), fav ? std::max(0L, ea::num(fav->attrs, "nr")) : 0);
            if (nr == 0) return ok + "</gamedata>";
            std::string ids;
            for (size_t i = 0; i < nr; i++) ids += std::to_string(order[i].first) + "?";
            const time_t t = s.now ? (time_t)s.now() : 0;
            const struct tm tm = sys::utc(t);
            char day[16];
            snprintf(day, sizeof day, "%04d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
            return ok + "<favorite nr=\"" + std::to_string(nr) + "\" netids=\"" + ids + "\" from=\"" + day + "\" to=\"" + day +
                   "\"/></gamedata>";
        }
        // autoir, rend: the system's status (nothing kept)
        return ok + "</gamedata>";
    }
    if (m == "ntp" && f == "get") {   // pop'n 9's form, for now
        const time_t t = s.now ? (time_t)s.now() : 0;
        const struct tm tm = sys::utc(t);
        char b[160];
        snprintf(b, sizeof b, "<ntp status=\"0\"><time year=\"%d\" month=\"%d\" day=\"%d\" hour=\"%d\" min=\"%d\" sec=\"%d\"></time></ntp>",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
        return b;
    }
    return "";
}

}  // namespace hk::k573
