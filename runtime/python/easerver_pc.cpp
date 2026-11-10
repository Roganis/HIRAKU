// SPDX-License-Identifier: GPL-3.0-or-later
// pop'n music 15-18 on the server (PC games: runtime/pc/) (easerver.h).
#include "python/easerver_parts.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <set>
#include <ctime>
#include "python/kbin.h"
#include "python/lobby.h"

namespace hk::py {

// pop'n 15 ADVENTURE (G15: popn15.exe through hiraku-pc, docs/EAMUSE3.md). Its module's methods
// are type_method (game_get, playerdata_get...); the method table is at 0x568700, each with its
// request builder and answer parser. The player is a record of 0xb38 bytes, sent LZSS'd in <b>
// with f_compress="1" (get, new and conv: decoded into 0x2234150, copied to the card's own copy
// and loaded at 0x4819f0, which bounds every field) and kept here as pdata-G15-<ref id>. set sends
// the credit's changes as attributes and children (built at 0x49ef10 from the set block 0x5dab20,
// which the game fills from the record: 0x482460-0x482b20, 0x4a4dc0 for netvs), merged into the
// record where the loader reads them:
//   0x00 name (Shift-JIS, 13)   0x1a mode_num        0x1b botton_num   0x1c option (32 bits)
//   0x20 chara_num (16)         0x22 music_num (16), 0x24 sheet_num, 0x25 category_num: the last
//   song chosen ("MUSIC %d PLAYDATA %d CATEGORY %d", 0x456f63)   0x27 skin_flag
//   0x28 skin_count (7 bytes)   0x30 the 20 most played songs (16 bits each, 0xffff none: a music
//   select folder, 0x456a0f; the server's to count)   0x58 a 16-bit word per song (763): the
//   two-bit clear marks of the four charts in the low byte, as pop'n 12, and played flags above
//   0x650, 0x790: per expert course (20), its four stages' scores: the player's best, and the
//   course's best player's (the server's, from rank_exp-entry)
//   0x8d0 expert_data, a word per expert course (expert_course_num)
//   0x8f8 private (the ADVENTURE map, game_get ir_ranking 2, 0x476940): count, record_time,
//   record_count, mapinfo (8, 32 bits each), event (32 bytes at 0x924), point (16) 0x944,
//   element 0x946, position 0x947
//   0x948 the NET TAISEN credits played (the server's count), then NET TAISEN's fields (netvs,
//   below; as pop'n 14's in other places). set's matching (how many players the match had) and
//   room_num are the operator's statistics, not kept
std::string EamuseServer::answer_g15(const Request &r) {
    const std::string &f = r.method;
    const std::string open = "<G15 status=\"0\"";
    constexpr size_t RECORD = 0xb38, SONGS = 0x2fb;
    auto list = [](const std::string &v) {   // "a,b,c"
        std::vector<long> out;
        for (size_t at = 0; at <= v.size() && !v.empty();) {
            out.push_back(std::strtol(v.c_str() + at, nullptr, 10));
            const size_t comma = v.find(',', at);
            if (comma == std::string::npos) break;
            at = comma + 1;
        }
        return out;
    };
    auto setting = [&](const char *name, long fallback) {   // the owner's choice, a file in the data folder
        const std::string v = read_file(std::string(name) + "-G15");
        return v.empty() ? fallback : std::strtol(v.c_str(), nullptr, 10);
    };

    if (f == "game_get") {
        // phase (0-2, the songs' event stage, 0x427130: the cabinet's own unless the server's is
        // higher; left out, it is the cabinet's); ir_ranking (0x427110), the event the
        // e-AMUSEMENT players see: 1 INTERNET RANKING (the expert courses' boards, rank_exp-get),
        // 2 ADVENTURE (the map: playerdata's private), 4 and 5 the rankings' other periods;
        // net_event (0-3, 0x4270f0): NET TAISEN from 1, on unless the data folder says 0 (its lobby
        // entries carry the number)
        std::string a;
        if (const long ph = setting("phase", -1); ph >= 0) a += " phase=\"" + std::to_string(ph) + "\"";
        a += " ir_ranking=\"" + std::to_string(setting("ir_ranking", 2)) + "\"";
        a += " net_event=\"" + std::to_string(setting("net_event", 1)) + "\"";
        // NET TAISEN's settings, each the data folder's NAME-G15 if there is one. Konami's values
        // went with its server; these follow what the game does with them and what players
        // described (docs/EAMUSE3.md, "pop'n music 15"): cabinets of one shop may meet; a
        // cabinet waits in the lobby matching_sec for others (0: it leaves at once; 120, pop'n
        // 19's as its server sent it; nobody: CHALLENGE mode); ojama_gauge_rate and cpu_level (10, the CPUs' strength) in
        // percent of the game's own (0x42e4b0, 0x42f4b3); downlimit_win/downlimit_class: the wins
        // that keep a class once reached and how far below it a player may fall (0x48e5a2,
        // 0x48e633: 10 rooms down is no floor, players are demoted as they were); job_sp_rate
        // (26): the 13 jobs' specials, the even ones each job's chance in percent at its top
        // level (0x42d0a0, the game's level table 0x4ff058 below that), the odd ones amounts in
        // percent (the magician's ojama longer x1.5, the warrior's shorter x0.5, the fighter's
        // gauge faster x1.5; 0x42d200, 0x48e7f1). job_exp_rate, cpu_higher_release,
        // shop_event_ranking: read, never used.
        static const struct { const char *name, *value; } netvs[] = {
            {"local_matching_enable", "1"}, {"matching_sec", "120"}, {"ojama_gauge_rate", "100"},
            {"cpu_level", "100,100,100,100,100,100,100,100,100,100"}, {"downlimit_win", "0"}, {"downlimit_class", "10"},
            {"job_sp_rate", "30,150,30,50,30,150,30,100,0,0,30,0,30,100,100,0,30,0,30,0,30,0,30,0,30,0"}, {"job_exp_rate", "100"}};
        for (const auto &n : netvs) {
            std::string v = read_file(std::string(n.name) + "-G15");
            while (!v.empty() && std::isspace((unsigned char)v.back())) v.pop_back();
            a += std::string(" ") + n.name + "=\"" + esc(v.empty() ? n.value : v) + "\"";
        }
        return open + a + " message=\"" + esc(message()) + "\"/>";
    }
    // the expert courses' board; its entries keep their four stages' scores for the records
    std::vector<const char *> keep = EXP_KEEP;
    keep.push_back("stage_score");
    const Board board{"G15-rank-exp", "ref_id", keep, "score", "cool", 10};
    if (f == "rank_exp-entry") {   // an expert course played: course_num, stage_score (4), cool... (0x49fec0)
        const Board &b = board;
        const Entered e = enter(b, r);
        return open + " update=\"" + std::string(e.update ? "1" : "0") + "\" entry=\"" + std::to_string(e.entries) + "\" rank=\"" +
               std::to_string(e.place) + "\" hi_score=\"" + esc(attr(e.kept, "score")) + "\" hi_cool=\"" + esc(attr(e.kept, "cool")) + "\"/>";
    }
    if (f == "rank_exp-get")   // the courses' boards, 0x2998 bytes LZSS'd (0x4a03d0)
        return open + " f_compress=\"1\"><b>" + base64(lzss(rank_table("G15"))) + "</b></G15>";
    if (f.compare(0, 11, "playerdata_") != 0) return open + "/>";   // game_active, playerdata_expire

    const std::string ref = attr(r.attrs, "ref_id");
    if (!safe_name(ref)) return "<G15 status=\"1\"/>";
    const std::string file = "pdata-G15-" + ref, scores_file = "pscore-G15-" + ref, plays_file = "pplays-G15-" + ref;
    std::string rec = read_file(file);
    auto put16 = [&](size_t at, long v) { if (at + 2 <= rec.size()) { rec[at] = (char)(v & 255); rec[at + 1] = (char)(v >> 8 & 255); } };
    auto put32 = [&](size_t at, long v) { put16(at, v & 0xffff); put16(at + 2, v >> 16 & 0xffff); };
    auto set_name = [&](const std::string &name) {
        for (size_t i = 0; i < 13; i++) rec[i] = i < name.size() && i < 12 ? name[i] : 0;
    };
    auto fresh = [&] {   // a player with nothing yet, as the game clears its own (0x482060)
        rec.assign(RECORD, 0);
        rec[0x1b] = (char)0xff;   // no buttons chosen
        put16(0x20, 0xffff);      // no character
        put16(0x22, 0xffff);      // no song chosen
        rec[0x25] = (char)0xff;
        for (size_t i = 0; i < 20; i++) put16(0x30 + 2 * i, 0xffff);
        rec[0x947] = 1;           // the ADVENTURE map's start
    };
    auto answer = [&] { return open + " f_compress=\"1\"><b>" + base64(lzss(rec)) + "</b></G15>"; };
    // (the game asks new, or conv, only after get's 109: a card that has its player keeps it)
    auto kept = [&] {
        if (rec.size() != RECORD) return false;
        say_kept(file);
        return true;
    };
    auto blank = [&] {   // a new player's record; what was there and wasn't a record kept aside
        keep_aside(file, rec);
        fresh();
    };
    auto pin = [&](const std::string &pass) {   // the card's PIN, if it has none yet (cardmng authpass checks it)
        std::vector<Record> cards = from_lines(read_file("cards"));
        bool changed = false;
        for (Record &c : cards)
            if (attr(c, "refid") == ref && attr(c, "pass").empty() && !pass.empty()) { c["pass"] = pass; changed = true; }
        if (!changed) return;
        std::string out;
        for (const Record &c : cards) out += to_line(c);
        write_file("cards", out);
    };

    if (f == "playerdata_get") {   // 109: none (the game goes on to make one, 0x4866e3)
        // model="F14": pop'n 14's player of this card, as 14 keeps it (0xdb8, the game shows it and
        // asks to carry it over: playerdata_conv)
        if (attr(r.attrs, "model") == "F14") {
            const std::string old = read_file("pdata-F14-" + ref);
            if (old.size() != 0xdb8) return "<G15 status=\"109\"/>";
            return open + " f_compress=\"1\"><b>" + base64(lzss(old)) + "</b></G15>";
        }
        if (rec.size() != RECORD) return "<G15 status=\"109\"/>";
        // the card's clear marks from the other pop'n games (one card for all)
        for (const Record &c : from_lines(read_file("cards")))
            if (attr(c, "refid") == ref && safe_name(attr(c, "cardid"))) rec = with_shared_marks("G15", attr(c, "cardid"), rec);
        // the most played songs, most first
        std::vector<Record> plays = from_lines(read_file(plays_file));
        std::stable_sort(plays.begin(), plays.end(), [&](const Record &x, const Record &y) { return num(x, "count") > num(y, "count"); });
        for (size_t i = 0; i < 20; i++) put16(0x30 + 2 * i, i < plays.size() ? num(plays[i], "num") : 0xffff);
        // the 20 expert courses' stage scores: 0x790 the course's best player's, the scores to beat
        // while the INTERNET RANKING runs (0x4266ce), and 0x650 the player's own best
        const std::vector<Record> entries = from_lines(read_file("G15-rank-exp"));
        for (size_t c = 0; c < 20; c++) {
            const Record *top = nullptr, *own = nullptr;
            for (const Record &e : entries) {
                if (attr(e, "course") != std::to_string(c)) continue;
                if (!top || better(board, e, *top)) top = &e;
                if (attr(e, "who") == ref) own = &e;
            }
            const std::vector<long> mine = own ? list(attr(*own, "stage_score")) : std::vector<long>(),
                                    best = top ? list(attr(*top, "stage_score")) : std::vector<long>();
            for (size_t st = 0; st < 4; st++) {
                put32(0x650 + 16 * c + 4 * st, st < mine.size() ? mine[st] : 0);
                put32(0x790 + 16 * c + 4 * st, st < best.size() ? best[st] : 0);
            }
        }
        return answer();
    }
    if (f == "playerdata_new") {   // name, ea_passwd (0x49e960)
        if (kept()) return answer();
        blank();
        set_name(attr(r.attrs, "name"));
        pin(attr(r.attrs, "ea_passwd"));
        write_file(file, rec);
        return answer();
    }
    // conv (name): the pop'n 14 player the game was given (get model="F14") carried over. The
    // songs keep their numbers (0x482d70 reads both records' words by the same index): pop'n 14's
    // 32-bit word's low half, its marks and played flags, as pop'n 15's word.
    if (f == "playerdata_conv") {
        const std::string old = read_file("pdata-F14-" + ref);
        if (old.size() != 0xdb8) return "<G15 status=\"109\"/>";
        if (kept()) return answer();
        blank();
        set_name(attr(r.attrs, "name"));
        for (size_t n = 0; n < SONGS && 0x24 + 4 * n + 2 <= old.size(); n++)
            put16(0x58 + 2 * n, ((uint8_t)old[0x24 + 4 * n] | (uint8_t)old[0x25 + 4 * n] << 8) & 0x7fff);
        write_file(file, rec);
        return answer();
    }
    if (f == "playerdata_score") {   // music_num sheet_num s_challenge: score (0x49ece0)
        return open + " score=\"" + std::to_string(shared_score("G15", ref, attr(r.attrs, "music_num"), attr(r.attrs, "sheet_num"))) + "\"/>";
    }
    if (f != "playerdata_set") return open + "/>";

    if (rec.size() != RECORD) blank();
    auto has = [&](const std::map<std::string, std::string> &m, const char *k) { return m.count(k) && attr(m, k) != "-1"; };
    const auto &a = r.attrs;
    if (has(a, "option")) put32(0x1c, num(a, "option"));
    if (has(a, "chara_num")) put16(0x20, num(a, "chara_num"));
    if (has(a, "mode_num")) rec[0x1a] = (char)num(a, "mode_num");
    if (has(a, "botton_num")) rec[0x1b] = (char)num(a, "botton_num");
    if (has(a, "music_num")) put16(0x22, num(a, "music_num"));
    if (has(a, "sheet_num")) rec[0x24] = (char)num(a, "sheet_num");
    if (has(a, "category_num")) rec[0x25] = (char)num(a, "category_num");
    if (has(a, "skin_flag")) rec[0x27] = (char)num(a, "skin_flag");
    if (has(a, "skin_count")) {
        const std::vector<long> v = list(attr(a, "skin_count"));
        for (size_t i = 0; i < 7 && i < v.size(); i++) rec[0x28 + i] = (char)v[i];
    }
    if (has(a, "expert_course_num") && has(a, "expert_data")) {
        const long c = num(a, "expert_course_num");
        if (c >= 0 && c < 20) put16(0x8d0 + 2 * (size_t)c, num(a, "expert_data"));
    }
    // the songs played: their word into the record; the score kept if better; a play counted
    std::vector<Record> scores = from_lines(read_file(scores_file)), plays = from_lines(read_file(plays_file));
    for (size_t at = r.inner.find("<music"); at != std::string::npos; at = r.inner.find("<music", at + 1)) {
        std::map<std::string, std::string> m;
        bool sc = false;
        read_attrs(r.inner, at + 6, m, &sc);
        const long n = num(m, "music_num");
        if (n < 0 || n >= (long)SONGS) continue;
        if (m.count("data")) put16(0x58 + 2 * (size_t)n, num(m, "data") & 0x7fff);
        const std::string song = std::to_string(n), sheet = attr(m, "sheet_num");
        bool found = false;
        for (Record &e : scores)
            if (attr(e, "num") == song && attr(e, "sheet") == sheet) {
                found = true;
                if (num(m, "score") > num(e, "score")) e["score"] = attr(m, "score");
            }
        if (!found && m.count("score")) scores.push_back({{"num", song}, {"sheet", sheet}, {"score", attr(m, "score")}});
        found = false;
        for (Record &e : plays)
            if (attr(e, "num") == song) { e["count"] = std::to_string(num(e, "count") + 1); found = true; }
        if (!found) plays.push_back({{"num", song}, {"count", "1"}});
    }
    std::string t;
    for (const Record &e : scores) t += to_line(e);
    if (!scores.empty()) write_file(scores_file, t);
    t.clear();
    for (const Record &e : plays) t += to_line(e);
    if (!plays.empty()) write_file(plays_file, t);
    // the ADVENTURE map (<private>), each attribute where the loader 0x476940 reads it
    if (const size_t pv = r.inner.find("<private"); pv != std::string::npos) {
        std::map<std::string, std::string> m;
        bool sc = false;
        read_attrs(r.inner, pv + 8, m, &sc);
        if (m.count("count")) put32(0x8f8, num(m, "count"));
        if (m.count("record_time")) put32(0x8fc, num(m, "record_time"));
        if (m.count("record_count")) put32(0x900, num(m, "record_count"));
        const std::vector<long> map = list(attr(m, "mapinfo")), ev = list(attr(m, "event"));
        for (size_t i = 0; i < 8 && i < map.size(); i++) put32(0x904 + 4 * i, map[i]);
        for (size_t i = 0; i < 32 && i < ev.size(); i++) rec[0x924 + i] = (char)ev[i];
        if (m.count("point")) put16(0x944, num(m, "point"));
        if (m.count("element")) rec[0x946] = (char)num(m, "element");
        if (m.count("position")) rec[0x947] = (char)num(m, "position");
    }
    // NET TAISEN (<netvs>): as pop'n 14's, its places those 0x4a4dc0 reads for the next set
    struct Field { const char *name; size_t at; int count, width; };   // width: bytes, or 13 for a name
    static const Field netvs15[] = {
        {"music_num", 0x94a, 1, 2}, {"sheet_num", 0x94c, 1, 1}, {"category_num", 0x94d, 1, 1}, {"badge", 0x94e, 1, 1},
        {"caption", 0x94f, 1, 1}, {"job_num", 0x950, 1, 1}, {"class", 0x951, 1, 1}, {"class_skill", 0x952, 1, 1},
        {"class_start", 0x953, 1, 1}, {"class_upgrade", 0x954, 1, 1}, {"record", 0x955, 1, 1},
        {"member_name", 0x969, 3, 13}, {"member_pref", 0x9de, 3, 1}, {"ojama_condition", 0x9e7, 51, 1},
        {"job_var", 0xad6, 1, 2}, {"job_exp", 0xad8, 13, 4}, {"set_ojama", 0xb25, 3, 1},
        {"get_ojama_hi", 0xb28, 1, 4}, {"get_ojama_lo", 0xb2c, 1, 4}, {"item_hi", 0xb30, 1, 4}, {"item_lo", 0xb34, 1, 4}};
    if (const size_t nv = r.inner.find("<netvs"); nv != std::string::npos) {
        std::map<std::string, std::string> m;
        bool sc = false;
        read_attrs(r.inner, nv + 6, m, &sc);
        // 0x948, the NET TAISEN credits played, isn't sent: the server counts them (the game's
        // first ones explain more, 0x459e2f: none, fewer than 5)
        const long played = (uint8_t)rec[0x948] | (uint8_t)rec[0x949] << 8;
        put16(0x948, played < 0xffff ? played + 1 : played);
        for (const Field &fd : netvs15) {
            if (!m.count(fd.name)) continue;
            const std::string v = attr(m, fd.name);
            size_t at = 0;
            for (int i = 0; i < fd.count; i++) {
                const size_t comma = v.find(',', at);
                const std::string one = v.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
                const size_t place = fd.at + (size_t)i * (size_t)fd.width;
                if (place + (size_t)fd.width > rec.size()) break;
                if (fd.width == 13)
                    for (size_t b = 0; b < 13; b++) rec[place + b] = b < one.size() && b < 12 ? one[b] : 0;
                else {
                    const long n = std::strtol(one.c_str(), nullptr, 10);
                    for (int b = 0; b < fd.width; b++) rec[place + (size_t)b] = (char)(n >> (8 * b) & 255);
                }
                if (comma == std::string::npos) break;
                at = comma + 1;
            }
        }
    }
    write_file(file, rec);
    return open + "/>";
}

// pop'n 18 せんごく列伝 (J39: popn18.exe through hiraku-pc). Its calls come as binary XML (kbin.h),
// modules game and playerdata (method tables 0x6a6800, 0x6a6858). game.get's parser (0x453a70)
// reads game_phase, ir_phase, event_phase, netvs_phase, license_phase, psp_phase, psp_sale_month,
// psp_sale_day, local_matching_enable, matching_sec, boss_diff, boss_battle_point (each the data
// folder's NAME-J39 if there is one). playerdata.get's (0x454810) copies <b>, the player's record
// of 0x1700 bytes, whole, and unpacks <hiscore>: 0x1ab7 scores (977 songs x 7 charts) of 17 bits,
// one after another (0x456570); new (0x454ac0) answers the record. set (0x454bf0) sends the
// credit's changes as attributes and children (music, license, private, netvs, boss): merged into
// the record below (license, netvs and boss not yet).
//
// pop'n 17 THE MOVIE (I17: popn17.exe) the same way, its own sizes: game.get (0x435190) reads
// game_phase, e_amuse_phase, local_matching_enable, matching_sec; playerdata.get (0x435b80)
// needs f_compress and takes <b> as the record packed (lzss, as pop'n 14's and 15's; binary, not
// base64 text, which crashes its unpacker), 0x74bc bytes (the scores within it).
//
// pop'n 16 PARTY (H16: popn16.exe) as pop'n 17: game.get (0x434990) reads pop'n 15's NET TAISEN
// settings and phase, netvs_phase, ir_phase, private_phase, normal_secret_phase, all_hiscore_phase;
// playerdata.get (0x435680) needs f_compress, its record 0x6784 bytes (pop'n 15's 0xb38 when the
// game asks for that one, to carry it over).
std::string EamuseServer::answer_j39(const std::string &game, const Request &r) {
    const std::string &m = r.module, &f = r.method;
    const bool p16 = game == "H16", p17 = game == "I17", packed = p16 || p17;
    const size_t RECORD = p16 ? 0x6784 : p17 ? 0x74bc : 0x1700;
    constexpr size_t SCORES = 0x1ab7, SCORE_BITS = 17;
    auto hex = [](const std::string &b) {
        static const char *hx = "0123456789abcdef";
        std::string o;
        for (unsigned char ch : b) o += hx[ch >> 4], o += hx[ch & 15];
        return o;
    };
    if (m == "game" && f == "get") {
        if (p16) {   // as pop'n 15's game_get, each the data folder's NAME-H16 if there is one
            static const struct { const char *name, *value; } fields16[] = {
                {"netvs_phase", "1"}, {"ir_phase", "0"}, {"private_phase", "1"}, {"normal_secret_phase", "1"},
                {"all_hiscore_phase", "1"}, {"local_matching_enable", "1"}, {"matching_sec", "120"}, {"ojama_gauge_rate", "100"},
                {"cpu_level", "100,100,100,100,100,100,100,100,100,100"}, {"downlimit_win", "0"}, {"downlimit_class", "10"},
                {"job_exp_rate", "100"}, {"cpu_higher_release", "0"},
                {"job_sp_rate", "30,150,30,50,30,150,30,100,0,0,30,0,30,100,100,0,30,0,30,0,30,0,30,0,30,0"}};
            std::string a;
            if (const std::string ph = read_file("phase-H16"); !ph.empty()) a += " phase=\"" + std::to_string(std::strtol(ph.c_str(), nullptr, 10)) + "\"";
            for (const auto &fd : fields16) {
                std::string v = read_file(std::string(fd.name) + "-H16");
                while (!v.empty() && std::isspace((unsigned char)v.back())) v.pop_back();
                a += std::string(" ") + fd.name + "=\"" + esc(v.empty() ? fd.value : v) + "\"";
            }
            return "<game status=\"0\"" + a + " message=\"" + esc(message()) + "\"/>";
        }
        struct Field { const char *name; long value; };
        // guesses but matching_sec (pop'n 15's): to be worked out from each one's users
        static const Field fields18[] = {{"game_phase", 2}, {"ir_phase", 0}, {"event_phase", 1}, {"netvs_phase", 1},
                                         {"license_phase", 1}, {"psp_phase", 0}, {"psp_sale_month", 1}, {"psp_sale_day", 1},
                                         {"local_matching_enable", 1}, {"matching_sec", 120}, {"boss_diff", 0}, {"boss_battle_point", 0}},
                            // 17's e_amuse_phase (0-15) opens its content by stages, NET対戦 from 4 (0x49adab:
                            // the mode select's fifth mode); its tables run to 14 (0x4c73bd, 0x4521d1), taken
                            // as the last
                            fields17[] = {{"game_phase", 2}, {"e_amuse_phase", 14}, {"local_matching_enable", 1}, {"matching_sec", 120}};
        std::string a;
        for (const Field &fd : p17 ? std::vector<Field>(std::begin(fields17), std::end(fields17))
                                   : std::vector<Field>(std::begin(fields18), std::end(fields18))) {
            const std::string v = read_file(std::string(fd.name) + "-" + game);
            a += std::string(" ") + fd.name + "=\"" + std::to_string(v.empty() ? fd.value : std::strtol(v.c_str(), nullptr, 10)) + "\"";
        }
        return "<game status=\"0\"" + a + "/>";
    }
    if (m == "game") return "<game status=\"0\"/>";   // active
    // The rankings. 16 and 17: the expert courses' boards, as pop'n 15's rank_exp (entry: ref_id
    // course_num, the four stages' scores (16 stage_score "a,b,c,d", 17 stage_score_1..4), cool
    // great good bad combo, pref; get: the 11 boards, 0x14cc bytes packed); 18: its INTERNET
    // RANKING (entry: ref_id ir_num sheet_num chara_num option score cool norma_1 norma_2 pref),
    // a board per ir_num. The name is the record's
    if (m == "ranking") {
        if (f == "get") {
            if (game == "J39") return "<ranking status=\"0\"/>";
            return "<ranking status=\"0\" f_compress=\"1\"><b __type=\"bin\">" + hex(lzss(rank_table(game))) + "</b></ranking>";
        }
        if (f != "entry") return "<ranking status=\"0\"/>";
        Request e = r;
        if (game == "J39") e.attrs["course_num"] = attr(r.attrs, "ir_num");
        if (game == "I17" && !e.attrs.count("stage_score"))
            e.attrs["stage_score"] = attr(r.attrs, "stage_score_1") + "," + attr(r.attrs, "stage_score_2") + "," +
                                     attr(r.attrs, "stage_score_3") + "," + attr(r.attrs, "stage_score_4");
        std::vector<const char *> keep = EXP_KEEP;
        keep.push_back("stage_score");
        if (game == "J39") keep.insert(keep.end(), {"sheet_num", "chara_num", "norma_1", "norma_2"});
        const std::string file = game + (game == "J39" ? "-rank-ir" : "-rank-exp");
        const Board b{file.c_str(), "ref_id", keep, "score", "cool", 10};
        const Entered en = enter(b, e);
        return "<ranking status=\"0\" update=\"" + std::string(en.update ? "1" : "0") + "\" entry=\"" + std::to_string(en.entries) +
               "\" rank=\"" + std::to_string(en.place) + "\" hi_score=\"" + esc(attr(en.kept, "score")) + "\" hi_cool=\"" +
               esc(attr(en.kept, "cool")) + "\"/>";
    }
    const std::string ref = attr(r.attrs, "ref_id");
    if (ref.empty() && f == "set") return "<playerdata status=\"0\"/>";   // a credit played without a card
    if (!safe_name(ref)) return "<playerdata status=\"1\"/>";
    // Each game copies the record to its own (16: 0xf313b8, 17: 0x1228988, 18: 0x10fc930), bounding
    // every field (16: 0x447dc0, 17: 0x440f30, 18: 0x460320), and doesn't change that copy in a
    // credit: set sends the credit's changes, merged here where the loader reads them. As pop'n
    // 15's (answer_g15), shifted:
    //   0x00 name (13), then two more names
    //   option (32 bits; 16: 0x1c, 17: 0x20, 18: 0x34), then chara_num (16), music_num (16),
    //   sheet_num, category_num: the last song chosen
    //   the 20 most played songs (16 bits each, 0xffff none: 16: 0x28, 17: 0x2c, 18: 0x40)
    //   a 16-bit word per song: the charts' clear marks and played flags (16: 0x50, 839 songs;
    //   17: 0x54, 907; 18: 0x68, 976)
    //   the best scores (16, 17): 32 bits per chart (7) and song (16: 0x8dc, 846 a chart; 17:
    //   0xcd4, 950), chart-major (17's 0x443c92). 18's are its <hiscore>: 17 bits per song and chart
    //   (song-major, 7 a song), least significant bit first (0x456570), kept here as pscore-J39-<ref>
    struct Layout { size_t option, songs, words, nwords, scores, per_chart; };
    const Layout L = p16 ? Layout{0x1c, 0x28, 0x50, 839, 0x8dc, 846}
                     : p17 ? Layout{0x20, 0x2c, 0x54, 907, 0xcd4, 950}
                           : Layout{0x34, 0x40, 0x68, 976, 0, 0};
    const std::string file = "pdata-" + game + "-" + ref, plays_file = "pplays-" + game + "-" + ref,
                      scores_file = "pscore-" + game + "-" + ref;
    std::string rec = read_file(file);
    auto put16 = [&](size_t at, long v) { if (at + 2 <= rec.size()) { rec[at] = (char)(v & 255); rec[at + 1] = (char)(v >> 8 & 255); } };
    auto put32 = [&](size_t at, long v) { put16(at, v & 0xffff); put16(at + 2, v >> 16 & 0xffff); };
    auto get32 = [&](size_t at) -> long {
        if (at + 4 > rec.size()) return 0;
        return (long)((uint32_t)(uint8_t)rec[at] | (uint32_t)(uint8_t)rec[at + 1] << 8 | (uint32_t)(uint8_t)rec[at + 2] << 16 | (uint32_t)(uint8_t)rec[at + 3] << 24);
    };
    std::string hiscore = read_file(scores_file);   // 18's, as the game's <hiscore>
    hiscore.resize((SCORES * SCORE_BITS + 7) / 8, '\0');
    auto hiscore_get = [&](size_t i) {
        long v = 0;
        for (size_t b = 0; b < SCORE_BITS; b++)
            if ((uint8_t)hiscore[(i * SCORE_BITS + b) / 8] >> ((i * SCORE_BITS + b) % 8) & 1) v |= 1L << b;
        return v;
    };
    auto hiscore_put = [&](size_t i, long v) {
        for (size_t b = 0; b < SCORE_BITS; b++) {
            const size_t bit = i * SCORE_BITS + b;
            if (v >> b & 1) hiscore[bit / 8] = (char)((uint8_t)hiscore[bit / 8] | 1 << bit % 8);
            else hiscore[bit / 8] = (char)((uint8_t)hiscore[bit / 8] & ~(1 << bit % 8));
        }
    };
    auto fresh = [&] {   // a player with nothing yet: no character, song or most played
        rec.assign(RECORD, '\0');
        rec[0x1b] = (char)0xff;
        put16(L.option + 4, 0xffff);
        put16(L.option + 6, 0xffff);
        rec[L.option + 9] = (char)0xff;
        for (size_t i = 0; i < 20; i++) put16(L.songs + 2 * i, 0xffff);
    };
    auto answer = [&] {
        // the most played songs, most first
        std::vector<Record> plays = from_lines(read_file(plays_file));
        std::stable_sort(plays.begin(), plays.end(), [&](const Record &x, const Record &y) { return num(x, "count") > num(y, "count"); });
        for (size_t i = 0; i < 20; i++) put16(L.songs + 2 * i, i < plays.size() ? num(plays[i], "num") : 0xffff);
        if (packed) return "<playerdata status=\"0\" f_compress=\"1\"><b __type=\"bin\">" + hex(lzss(rec)) + "</b></playerdata>";
        return "<playerdata status=\"0\"><b __type=\"bin\">" + hex(rec) + "</b><hiscore __type=\"bin\">" + hex(hiscore) +
               "</hiscore></playerdata>";
    };
    // (the game asks new, or conversion, only after get's 109: a card that has its player keeps it)
    auto kept = [&] {
        if (rec.size() != RECORD) return false;
        say_kept(file);
        return true;
    };
    auto blank = [&] {   // a new player's record; what was there and wasn't a record kept aside
        keep_aside(file, rec);
        fresh();
    };
    if (f == "new") {   // the name (Shift-JIS, 12 bytes) first, as pop'n 12-15's records
        if (kept()) return answer();
        blank();
        const std::string name = attr(r.attrs, "name");
        for (size_t i = 0; i < name.size() && i < 12; i++) rec[i] = name[i];
        write_file(file, rec);
        return answer();
    }
    // The game before (16: pop'n 15, 17: 16, 18: 17), whose player the game offers to carry over:
    // get model=<it> shows it (16 copies 15's whole record, 17 the first 0x6b0 bytes of 16's,
    // 18 the first 0x72c of 17's, unpacked: 0x45490a), conversion makes this game's from it
    const char *before = p16 ? "G15" : p17 ? "H16" : "I17";
    const size_t BEFORE = p16 ? 0xb38 : p17 ? 0x6784 : 0x74bc;
    if (f == "get") {
        if (!attr(r.attrs, "model").empty()) {
            const std::string old = attr(r.attrs, "model").substr(0, 3) == before ? read_file(std::string("pdata-") + before + "-" + ref) : "";
            if (old.size() != BEFORE) return packed ? "<playerdata status=\"109\" f_compress=\"1\"/>" : "<playerdata status=\"109\"/>";
            if (packed) return "<playerdata status=\"0\" f_compress=\"1\"><b __type=\"bin\">" + hex(lzss(old)) + "</b></playerdata>";
            return "<playerdata status=\"0\"><b __type=\"bin\">" + hex(old.substr(0, 0x72c)) + "</b></playerdata>";
        }
        if (rec.size() != RECORD) return packed ? "<playerdata status=\"109\" f_compress=\"1\"/>" : "<playerdata status=\"109\"/>";
        // one card for all: the card's clear marks and best scores from its other pop'n games,
        // put in this answer only (set merges the credit into the record as it is kept)
        for (const Record &c : from_lines(read_file("cards")))
            if (attr(c, "refid") == ref && safe_name(attr(c, "cardid"))) rec = with_shared_marks(game, attr(c, "cardid"), rec);
        for (const auto &[chart, score] : shared_scores(game, ref)) {
            const size_t n = (size_t)chart.first, sheet = (size_t)chart.second;
            if (sheet >= 7 || score <= 0) continue;
            if (L.scores) {
                const size_t place = L.scores + 4 * (sheet * L.per_chart + n);
                if (n < L.per_chart && score > get32(place)) put32(place, score);
            } else if (n * 7 + sheet < SCORES && score > hiscore_get(n * 7 + sheet))
                hiscore_put(n * 7 + sheet, std::min(score, (1L << SCORE_BITS) - 1));
        }
        return answer();
    }
    // conversion (ref_id card_id name): this game's player made from the one before. The songs keep
    // their numbers: each one's word (clear marks, played flags) and best scores come along
    if (f == "conversion") {
        const std::string old = read_file(std::string("pdata-") + before + "-" + ref);
        if (old.size() != BEFORE) return packed ? "<playerdata status=\"109\" f_compress=\"1\"/>" : "<playerdata status=\"109\"/>";
        if (kept()) return answer();
        blank();
        const std::string name = attr(r.attrs, "name");
        for (size_t i = 0; i < 13; i++) rec[i] = name.empty() ? old[i] : i < name.size() && i < 12 ? name[i] : '\0';
        auto old16 = [&](size_t at) { return at + 2 <= old.size() ? (long)((uint8_t)old[at] | (uint8_t)old[at + 1] << 8) : 0L; };
        auto old32 = [&](size_t at) { return at + 4 <= old.size() ? old16(at) | old16(at + 2) << 16 : 0L; };
        const size_t old_words = p16 ? 0x58 : p17 ? 0x50 : 0x54, old_n = p16 ? 763 : p17 ? 839 : 907;
        for (size_t n = 0; n < old_n && n < L.nwords; n++) put16(L.words + 2 * n, old16(old_words + 2 * n) & 0x7fff);
        auto keep = [&](size_t n, size_t sheet, long score) {
            if (score <= 0 || sheet >= 7) return;
            if (L.scores) {
                if (n < L.per_chart) put32(L.scores + 4 * (sheet * L.per_chart + n), score);
            } else if (n * 7 + sheet < SCORES) hiscore_put(n * 7 + sheet, std::min(score, (1L << SCORE_BITS) - 1));
        };
        if (p16) {   // pop'n 15's scores are the server's (pscore-G15)
            for (const Record &e : from_lines(read_file("pscore-G15-" + ref)))
                keep((size_t)num(e, "num"), (size_t)num(e, "sheet"), num(e, "score"));
        } else {     // 16's and 17's are in their records, chart-major
            const size_t at = p17 ? 0x8dc : 0xcd4, per = p17 ? 846 : 950;
            for (size_t sheet = 0; sheet < 7; sheet++)
                for (size_t n = 0; n < per; n++) keep(n, sheet, old32(at + 4 * (sheet * per + n)));
        }
        write_file(file, rec);
        if (!L.scores) write_file(scores_file, hiscore);
        return answer();
    }
    if (f == "score") {   // 16 (17's is a stub): music_num sheet_num s_challenge -> the card's best (0x435d70)
        const long best = shared_score(game, ref, attr(r.attrs, "music_num"), attr(r.attrs, "sheet_num"));
        return "<playerdata status=\"0\" score=\"" + std::to_string(best) + "\" nr_active_friend=\"0\"/>";
    }
    if (f != "set") return "<playerdata status=\"0\"/>";   // expire, friend: nothing
    if (rec.size() != RECORD) blank();
    auto has = [&](const std::map<std::string, std::string> &m, const char *k) { return m.count(k) && attr(m, k) != "-1"; };
    const auto &a = r.attrs;
    if (has(a, "option")) put32(L.option, num(a, "option"));
    if (has(a, "chara_num")) put16(L.option + 4, num(a, "chara_num"));
    if (has(a, "music_num")) put16(L.option + 6, num(a, "music_num"));
    if (has(a, "sheet_num")) rec[L.option + 8] = (char)num(a, "sheet_num");
    if (has(a, "category_num")) rec[L.option + 9] = (char)num(a, "category_num");
    if (p16 && has(a, "mode_num")) rec[0x1a] = (char)num(a, "mode_num");   // as pop'n 15's
    if (p16 && has(a, "botton_num")) rec[0x1b] = (char)num(a, "botton_num");
    // 17's and 18's last mode, where their mode select starts (the record's 0x1a: 17 0x49ad1e, 18
    // 0x4c383f). set's play_mode is the mode and how it went (17 0x442bba, 18 0x461691), so a mode
    // is several: 17's 0-5 are play_mode 0-1, 2-3 and 11, 5, 6 (the expert courses), 7-10 and 13
    // (NET TAISEN), 4 and 12; 18's 0-4 are 0-1, 2-3 and 10, 5, 6-9 and 12 (NET TAISEN), 4 and 11
    if ((p17 || !packed) && has(a, "play_mode")) {
        static const int mode17[14] = {0, 0, 1, 1, 5, 2, 3, 4, 4, 4, 4, 1, 5, 4}, mode18[13] = {0, 0, 1, 1, 4, 2, 3, 3, 3, 3, 1, 4, 3};
        const long pm = num(a, "play_mode");
        if (pm >= 0 && pm < (p17 ? 14 : 13)) rec[0x1a] = (char)(p17 ? mode17[pm] : mode18[pm]);
    }
    // last_play_flag: the record's 0x1c, set from it and the credit (17 0x442bae: bits 0-1; 18
    // 0x461685: bits 0-6, as their loaders keep them)
    if ((p17 || !packed) && has(a, "last_play_flag")) rec[0x1c] = (char)(num(a, "last_play_flag") & (p17 ? 3 : 0x7f));
    // 18's norma_point: the record's 0x3e, worked out anew in some credits (0x4963c0)
    if (!packed && has(a, "norma_point")) rec[0x3e] = (char)num(a, "norma_point");
    if (packed) {
        // 16's and 17's extra stage tickets: the record's 0x26 / 0x2a, at most 2 (17 0x4716b0); the
        // expert courses (17 0x442a20): each course's clear (0-4) a 16-bit word (16: 11 courses
        // from 0x83c; 17: 19 from 0x990), the game sending the better of the record's and the
        // credit's; a random course by its level (4-42), how many times cleared and full-comboed
        // (at most 99), bytes (16: from 0x852 and 0x87d; 17: 0x9b6 and 0x9e1), as their loaders
        // bound them (16 0x447dc0, 17 0x440f30)
        const size_t ticket = p16 ? 0x26 : 0x2a, courses = p16 ? 11 : 19, clears = p16 ? 0x83c : 0x990,
                     random_clear = p16 ? 0x852 : 0x9b6, random_combo = p16 ? 0x87d : 0x9e1;
        if (has(a, "extra_ticket")) rec[ticket] = (char)std::clamp(num(a, "extra_ticket"), 0L, 2L);
        if (has(a, "expert_course_num") && has(a, "expert_data")) {
            const long c = num(a, "expert_course_num");
            if (c >= 0 && (size_t)c < courses) put16(clears + 2 * (size_t)c, std::clamp(num(a, "expert_data"), 0L, 4L));
        }
        if (const long lv = has(a, "expert_random_course_num") ? num(a, "expert_random_course_num") : -1; lv >= 0 && lv < 43) {
            if (has(a, "expert_random_course_clear")) rec[random_clear + (size_t)lv] = (char)std::clamp(num(a, "expert_random_course_clear"), 0L, 99L);
            if (has(a, "expert_random_course_combo")) rec[random_combo + (size_t)lv] = (char)std::clamp(num(a, "expert_random_course_combo"), 0L, 99L);
        }
    }
    auto numbers = [](const std::string &v) {   // "a,b,c"
        std::vector<long> out;
        for (size_t from = 0; !v.empty() && from <= v.size();) {
            out.push_back(std::strtol(v.c_str() + from, nullptr, 10));
            const size_t comma = v.find(',', from);
            if (comma == std::string::npos) break;
            from = comma + 1;
        }
        return out;
    };
    // 16's <private> (PARTY: the guests): the record keeps how many times each of the five guests
    // came, bytes from 0x6564, and the secrets found, 32 bits at 0x656c; the game sends the
    // record's count when it's more and the record's secrets OR'd in (0x448ef0). zoom, seed, the
    // invites and ticket_point are the credit's own
    if (const size_t pv = r.inner.find("<private"); p16 && pv != std::string::npos) {
        std::map<std::string, std::string> m;
        bool sc = false;
        read_attrs(r.inner, pv + 8, m, &sc);
        for (int k = 0; k < 5; k++)
            if (const std::string g = "guest_count_" + std::to_string(k); m.count(g)) rec[0x6564 + (size_t)k] = (char)std::clamp(num(m, g.c_str()), 0L, 255L);
        if (m.count("secret")) put32(0x656c, num(m, "secret"));
    }
    // 17's <private>: the record's 0xc00-0xcd4, which the game copies to 0x1209aec and keeps
    // through the credit (0x451d20); set sends it from there (0x452dc9, 0x442a00): secret (32
    // bits) 0xc00, main_chara (3 words) 0xc04, sub_chara (10) 0xc0a, latest_main_0-4 (3 each)
    // from 0xc1e, latest_sub_0-4 (10 each) from 0xc3c, point 0xcba, ticket_point (5 bytes)
    // 0xcbb, target 0xcc0, latest_target (5 bytes) 0xcc1
    if (const size_t pv = r.inner.find("<private"); p17 && pv != std::string::npos) {
        std::map<std::string, std::string> m;
        bool sc = false;
        read_attrs(r.inner, pv + 8, m, &sc);
        auto words = [&](const std::string &k, size_t at, size_t count) {
            if (!m.count(k)) return;
            const std::vector<long> v = numbers(attr(m, k.c_str()));
            for (size_t i = 0; i < count && i < v.size(); i++) put16(at + 2 * i, v[i]);
        };
        auto bytes = [&](const std::string &k, size_t at, size_t count) {
            if (!m.count(k)) return;
            const std::vector<long> v = numbers(attr(m, k.c_str()));
            for (size_t i = 0; i < count && i < v.size(); i++) rec[at + i] = (char)v[i];
        };
        if (m.count("secret")) put32(0xc00, num(m, "secret"));
        words("main_chara", 0xc04, 3);
        words("sub_chara", 0xc0a, 10);
        for (size_t k = 0; k < 5; k++) {
            words("latest_main_" + std::to_string(k), 0xc1e + 6 * k, 3);
            words("latest_sub_" + std::to_string(k), 0xc3c + 0x14 * k, 10);
        }
        bytes("point", 0xcba, 1);
        bytes("ticket_point", 0xcbb, 5);
        bytes("target", 0xcc0, 1);
        bytes("latest_target", 0xcc1, 5);
    }
    // 18's <license> (each a license and part the credit changed: license_num, sub_license_num,
    // clear, data1-4): the record keeps 13 licenses of 11 parts, each a clear word and four data
    // words (0x14 bytes, the licenses 0xdc apart) from 0x89c, which the game copies to its table
    // 0x105b244 (0x470640) and sends from (0x461862)
    for (size_t at = packed ? std::string::npos : r.inner.find("<license"); at != std::string::npos; at = r.inner.find("<license", at + 1)) {
        std::map<std::string, std::string> m;
        bool sc = false;
        read_attrs(r.inner, at + 8, m, &sc);
        const long lic = m.count("license_num") ? num(m, "license_num") : -1, sub = m.count("sub_license_num") ? num(m, "sub_license_num") : -1;
        if (lic < 0 || lic >= 13 || sub < 0 || sub >= 11) continue;
        const size_t place = 0x89c + 0xdc * (size_t)lic + 0x14 * (size_t)sub;
        if (m.count("clear")) put32(place, num(m, "clear"));
        for (size_t k = 0; k < 4; k++)
            if (const std::string d = "data" + std::to_string(k + 1); m.count(d)) put32(place + 4 + 4 * k, num(m, d.c_str()));
    }
    // the songs played: their word, the score kept if better, a play counted
    std::vector<Record> plays = from_lines(read_file(plays_file));
    bool scored = false;
    for (size_t at = r.inner.find("<music"); at != std::string::npos; at = r.inner.find("<music", at + 1)) {
        std::map<std::string, std::string> m;
        bool sc = false;
        read_attrs(r.inner, at + 6, m, &sc);
        const long n = num(m, "music_num"), sheet = num(m, "sheet_num"), score = num(m, "score");
        if (n < 0 || n >= (long)L.nwords) continue;
        if (m.count("data")) put16(L.words + 2 * (size_t)n, num(m, "data") & 0x7fff);
        if (sheet >= 0 && sheet < 7 && score > 0) {
            if (L.scores) {
                const size_t place = L.scores + 4 * ((size_t)sheet * L.per_chart + (size_t)n);
                if ((size_t)n < L.per_chart && score > get32(place)) put32(place, score);
            } else if ((size_t)n * 7 + (size_t)sheet < SCORES && score > hiscore_get((size_t)n * 7 + (size_t)sheet)) {
                hiscore_put((size_t)n * 7 + (size_t)sheet, std::min(score, (1L << SCORE_BITS) - 1));
                scored = true;
            }
        }
        bool found = false;
        for (Record &e : plays)
            if (attr(e, "num") == std::to_string(n)) { e["count"] = std::to_string(num(e, "count") + 1); found = true; }
        if (!found) plays.push_back({{"num", std::to_string(n)}, {"count", "1"}});
    }
    // 18's story map (<private>): the record's last 0x128 bytes, which the game copies to 0x10af5a8
    // and keeps there through the credit; set reads them back (0x474e93)
    if (const size_t pv = r.inner.find("<private"); !packed && pv != std::string::npos) {
        std::map<std::string, std::string> m;
        bool sc = false;
        read_attrs(r.inner, pv + 8, m, &sc);
        constexpr size_t MAP = 0x15d8;
        auto list = [&](const char *k, size_t at, size_t count, int width) {
            const std::string v = attr(m, k);
            size_t from = 0;
            for (size_t i = 0; i < count && m.count(k); i++) {
                const size_t comma = v.find(',', from);
                const long n = std::strtol(v.c_str() + from, nullptr, 10);
                if (width == 2) put16(at + 2 * i, n);
                else rec[at + i] = (char)n;
                if (comma == std::string::npos) break;
                from = comma + 1;
            }
        };
        // a map not begun yet is all zero; the game begins one with its character lists empty
        // (0x08-0xf7 0xff, 0x473824)
        if (rec.find_first_not_of('\0', MAP) == std::string::npos)
            for (size_t i = 0x08; i < 0xf8; i++) rec[MAP + i] = (char)0xff;
        if (m.count("subjection_0")) put32(MAP + 0x00, num(m, "subjection_0"));
        if (m.count("subjection_1")) put32(MAP + 0x04, num(m, "subjection_1"));
        // two lists, newest first: the characters used (10 from 0x08) and played (50 from 0x1c); set
        // sends the credit's, which go in front, the others moved along (as 0x473cb0 does them)
        auto push = [&](size_t at, size_t size, const std::vector<long> &v) {
            const size_t k = std::min(v.size(), size);
            for (size_t i = size; i-- > k;) put16(at + 2 * i, (uint8_t)rec[at + 2 * (i - k)] | (uint8_t)rec[at + 2 * (i - k) + 1] << 8);
            for (size_t i = 0; i < k; i++) put16(at + 2 * i, v[i]);
        };
        if (m.count("used_chara")) push(MAP + 0x08, 10, {num(m, "used_chara")});
        std::vector<long> played;
        for (size_t from = 0, comma = 0; m.count("played_chara") && comma != std::string::npos; from = comma + 1) {
            const std::string &v = m.at("played_chara");
            comma = v.find(',', from);
            const long n = std::strtol(v.c_str() + from, nullptr, 10);
            if (n >= 0) played.push_back(n);
        }
        push(MAP + 0x1c, 50, played);
        if (m.count("ticket_open")) rec[MAP + 0xf8] = (char)num(m, "ticket_open");
        if (m.count("ticket_get")) rec[MAP + 0xf9] = (char)num(m, "ticket_get");
        if (m.count("position")) rec[MAP + 0xfa] = (char)num(m, "position");
        list("open_point", MAP + 0xfb, 29, 1);
        if (m.count("point")) rec[MAP + 0x118] = (char)num(m, "point");
    }
    // 16's NET TAISEN (<netvs>): the record's last 0x1f0 bytes (0x6594), pop'n 15's fields in other
    // places, as 0x43bf30 copies them to set's block; 0x6594 counts the NET credits (the server's to
    // count). A block not begun (0 credits) gets the game's defaults first (0x43c180), as the game
    // does itself
    if (const size_t nv = r.inner.find("<netvs"); p16 && nv != std::string::npos) {
        std::map<std::string, std::string> m;
        bool sc = false;
        read_attrs(r.inner, nv + 6, m, &sc);
        constexpr size_t NV = 0x6594;
        const long credits = (uint8_t)rec[NV] | (uint8_t)rec[NV + 1] << 8;
        if (credits == 0) {
            std::fill(rec.begin() + NV, rec.begin() + NV + 0x1f0, '\0');
            put16(0x6596, 0x30e), rec[0x6599] = 0xb, rec[0x659b] = 0x38, put32(0x65a0, 0x1000000), rec[0x65ae] = 0xf;
            std::fill(rec.begin() + 0x6643, rec.begin() + 0x6643 + 51, '\x32');
            rec[0x66ed] = 1, rec[0x66ee] = 0x10, rec[0x66ef] = 0x23, put16(0x6732, 1);
            for (size_t i = 0; i < 5; i++) put16(0x6758 + 2 * i, 0xffff), put16(0x6772 + 2 * i, 0xffff), put16(0x6762 + 3 * i, 0xffff), rec[0x6764 + 3 * i] = (char)0xff;
            rec[0x6771] = 1, put32(0x677c, 0x18), put32(0x6780, 0xf001f);
        }
        put16(NV, credits < 0xffff ? credits + 1 : credits);
        struct Field { const char *name; size_t at; int count, width; };   // width: bytes, or 13 for a name
        static const Field netvs16[] = {
            {"music_num", 0x6596, 1, 2}, {"sheet_num", 0x6598, 1, 1}, {"category_num", 0x6599, 1, 1}, {"badge", 0x659a, 1, 1},
            {"title_select", 0x659b, 1, 1}, {"title_0", 0x659c, 1, 4}, {"title_1", 0x65a0, 1, 4}, {"title_2", 0x65a4, 1, 4},
            {"title_3", 0x65a8, 1, 4}, {"job_num", 0x65ac, 1, 1}, {"class", 0x65ad, 1, 1}, {"class_skill", 0x65ae, 1, 1},
            {"class_start", 0x65af, 1, 1}, {"class_upgrade", 0x65b0, 1, 1}, {"record", 0x65b1, 1, 1},
            {"member_name", 0x65c5, 3, 13}, {"member_pref", 0x663a, 3, 1}, {"ojama_condition", 0x6643, 51, 1},
            {"set_ojama", 0x66ed, 3, 1}, {"job_var", 0x6732, 1, 2}, {"job_exp", 0x6734, 9, 4}, {"recommend", 0x6771, 1, 1},
            {"get_ojama_hi", 0x677c, 1, 4}, {"get_ojama_lo", 0x6780, 1, 4}};
        for (const Field &fd : netvs16) {
            if (!m.count(fd.name)) continue;
            const std::string v = attr(m, fd.name);
            size_t from = 0;
            for (int i = 0; i < fd.count; i++) {
                const size_t comma = v.find(',', from);
                const std::string one = v.substr(from, comma == std::string::npos ? std::string::npos : comma - from);
                const size_t place = fd.at + (size_t)i * (size_t)fd.width;
                if (fd.width == 13)
                    for (size_t b = 0; b < 13; b++) rec[place + b] = b < one.size() && b < 12 ? one[b] : '\0';
                else
                    for (int b = 0; b < fd.width; b++) rec[place + (size_t)b] = (char)(std::strtol(one.c_str(), nullptr, 10) >> (8 * b) & 255);
                if (comma == std::string::npos) break;
                from = comma + 1;
            }
        }
    }
    // 18's NET TAISEN (<netvs>), where the loader reads them (0x4606a0-0x460e90): what a player has
    // won (ojama, items: two 32-bit words each; titles: five), experience (clamped by the loader),
    // the last battle's song, chart and category, the title shown, the skill (15 to start), the 51
    // ojama conditions, the ojama and items set, the four parameters, the event points. Checked
    // against the game's own copy, which it fills with a new player's defaults.
    // 17's the same, 0x9bc lower, 0x9c0 after its four titles: checked against its loader (0x4411c0). 17
    // starts a new NET player's block with its defaults while 0xa40, the NET credits, is 0
    // (0x43c950): the server counts them, writing those defaults first (0x43c7f0), as 16's
    if (const size_t nv = r.inner.find("<netvs"); !p16 && nv != std::string::npos) {
        std::map<std::string, std::string> m;
        bool sc = false;
        read_attrs(r.inner, nv + 6, m, &sc);
        if (p17) {
            const long credits = (uint8_t)rec[0xa40] | (uint8_t)rec[0xa41] << 8;
            if (credits == 0) {
                std::fill(rec.begin() + 0xa0c, rec.begin() + 0xc00, '\0');
                put32(0xa0c, 0x7000f), put32(0xa10, 0x1c), put32(0xa14, 7), put32(0xa2c, 0x20000000), put16(0xa42, 0x34b);
                rec[0xa95] = 2, rec[0xa96] = 0x1d, rec[0xa99] = 0xf;
                std::fill(rec.begin() + 0xb47, rec.begin() + 0xb47 + 51, '\x32');
                rec[0xbf1] = 1, rec[0xbf2] = 0x10, rec[0xbf3] = 0x22, rec[0xbf8] = 1, put32(0xbf9, 0x1010101);
                for (size_t i = 0; i < 5; i++) put16(0xa80 + 2 * i, 0xffff), put16(0xa8a + 2 * i, 0xffff), put16(0xb2e + 3 * i, 0xffff), rec[0xb30 + 3 * i] = (char)0xff;
            }
            put16(0xa40, credits < 0xffff ? credits + 1 : credits);
        }
        // the rest as the battle's results fill set's block (17 0x476c95, 18 0x49f4d5) from the
        // NET state, which holds the record's block (17: the record's offset less 0x7fc, 18 less
        // 0x1198): what was won and used (ojama, items: two 32-bit words each), the room, where
        // it started and its step up, the record, the last battle's members (names, prefectures),
        // the recommended ojama. 17's are 18's 0x9bc lower up to its titles won (four, not five),
        // 0x9c0 after
        struct Field { const char *name; size_t at; int count, width; };   // width: bytes, or 13 for a name
        static const Field netvs17[] = {
            {"get_ojama_0", 0xa0c, 1, 4}, {"get_ojama_1", 0xa10, 1, 4}, {"get_item_0", 0xa14, 1, 4}, {"get_item_1", 0xa18, 1, 4},
            {"used_ojama_0", 0xa1c, 1, 4}, {"used_ojama_1", 0xa20, 1, 4}, {"used_item_0", 0xa24, 1, 4}, {"used_item_1", 0xa28, 1, 4},
            {"get_title_0", 0xa2c, 1, 4}, {"get_title_1", 0xa30, 1, 4}, {"get_title_2", 0xa34, 1, 4}, {"get_title_3", 0xa38, 1, 4},
            {"exp", 0xa3c, 1, 4}, {"music_num", 0xa42, 1, 2}, {"sheet_num", 0xa94, 1, 1}, {"category_num", 0xa95, 1, 1},
            {"title_select", 0xa96, 1, 1}, {"room", 0xa98, 1, 1}, {"room_skill", 0xa99, 1, 1}, {"room_start", 0xa9a, 1, 1},
            {"room_upgrade", 0xa9b, 1, 1}, {"record", 0xa9c, 1, 1}, {"member_name", 0xab0, 3, 13}, {"member_pref", 0xb25, 3, 1},
            {"ojama_condition", 0xb47, 51, 1}, {"set_ojama", 0xbf1, 3, 1}, {"recommend_ojama", 0xbf4, 3, 1}, {"set_item", 0xbf7, 2, 1},
            {"parameter", 0xbf9, 4, 1}};
        static const Field netvs18[] = {
            {"get_ojama_0", 0x13c8, 1, 4}, {"get_ojama_1", 0x13cc, 1, 4}, {"get_item_0", 0x13d0, 1, 4}, {"get_item_1", 0x13d4, 1, 4},
            {"used_ojama_0", 0x13d8, 1, 4}, {"used_ojama_1", 0x13dc, 1, 4}, {"used_item_0", 0x13e0, 1, 4}, {"used_item_1", 0x13e4, 1, 4},
            {"get_title_0", 0x13e8, 1, 4}, {"get_title_1", 0x13ec, 1, 4}, {"get_title_2", 0x13f0, 1, 4}, {"get_title_3", 0x13f4, 1, 4},
            {"get_title_4", 0x13f8, 1, 4}, {"exp", 0x13fc, 1, 4}, {"music_num", 0x1402, 1, 2}, {"sheet_num", 0x1454, 1, 1},
            {"category_num", 0x1455, 1, 1}, {"set_title", 0x1456, 1, 1}, {"room", 0x1458, 1, 1}, {"room_skill", 0x1459, 1, 1},
            {"room_start", 0x145a, 1, 1}, {"room_upgrade", 0x145b, 1, 1}, {"record", 0x145c, 1, 1}, {"member_name", 0x1470, 3, 13},
            {"member_pref", 0x14e5, 3, 1}, {"ojama_condition", 0x1507, 51, 1}, {"set_ojama", 0x15b1, 3, 1},
            {"recommend_ojama", 0x15b4, 3, 1}, {"set_item", 0x15b7, 2, 1}, {"parameter", 0x15b9, 4, 1}, {"event_point", 0x15bd, 1, 1}};
        for (const Field &fd : p17 ? std::vector<Field>(std::begin(netvs17), std::end(netvs17))
                                   : std::vector<Field>(std::begin(netvs18), std::end(netvs18))) {
            if (!m.count(fd.name)) continue;
            const std::string v = attr(m, fd.name);
            size_t from = 0;
            for (int i = 0; i < fd.count; i++) {
                const size_t comma = v.find(',', from);
                const std::string one = v.substr(from, comma == std::string::npos ? std::string::npos : comma - from);
                const size_t place = fd.at + (size_t)i * (size_t)fd.width;
                if (fd.width == 13) {
                    for (size_t b = 0; b < 13 && place + b < rec.size(); b++) rec[place + b] = b < one.size() && b < 12 ? one[b] : '\0';
                } else {
                    const long n = std::strtol(one.c_str(), nullptr, 10);
                    for (int b = 0; b < fd.width && place + (size_t)b < rec.size(); b++) rec[place + (size_t)b] = (char)(n >> (8 * b) & 255);
                }
                if (comma == std::string::npos) break;
                from = comma + 1;
            }
        }
    }
    std::string t;
    for (const Record &e : plays) t += to_line(e);
    if (!plays.empty()) write_file(plays_file, t);
    if (scored) write_file(scores_file, hiscore);
    write_file(file, rec);
    return "<playerdata status=\"0\"/>";
}

}  // namespace hk::py
