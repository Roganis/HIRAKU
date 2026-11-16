// SPDX-License-Identifier: GPL-3.0-or-later
// The server's rankings: pop'n 9's boards, pop'n 10-14's rank.get tables (server.h).
#include "eamuse/server_parts.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <set>

#include "shell/sys.h"

namespace hk::ea {

const Board BOARD_SCORE = {&tables::rank_score, {}, "card_id", {"name", "score", "cool", "great", "good", "bad", "combo", "option", "konami_id", "shop_name", "area_num", "gpcb_id"}, "score", "cool", 10};
const Board BOARD_H = {&tables::rank_hitasura, {}, "card_id", {"name", "music", "combo", "konami_id", "shop_name", "area_num", "gpcb_id"}, "music", "combo", 10};
const Board BOARD_GZ = {&tables::rank_gamblerz, {}, "gpcb_id", {"prog", "parts", "shop_name", "area_num"}, "prog", "parts", 16};
// pop'n 10 on: rank.exp sends what pop'n 9's rank.register did; one board per game (tables::rank_exp, by game)
const std::vector<const char *> EXP_KEEP = {"name", "score", "cool", "great", "good", "bad", "combo", "option", "shop_name", "area_num"};

bool better(const Board &b, const Record &x, const Record &y) {   // x ranks above y
    const long x1 = num(x, b.first), y1 = num(y, b.first);
    if (x1 != y1) return x1 > y1;
    return num(x, b.second, b.second_base) > num(y, b.second, b.second_base);
}

std::string Server::rank_register(const Board &b, const Request &r) {
    const Entered e = enter(b, r);
    return item(b, e.kept, e.place) + "entry=\"" + std::to_string(e.entries) + "\" update=\"" + (e.update ? "1" : "0") + "\"/>";
}

// A result entered on a board: kept if it beats the entrant's best; where the entrant now stands
Server::Entered Server::enter(const Board &b, const Request &r) {
    // pop'n 12 on name the course course_num
    const char *course_key = attr(r.attrs, "course").empty() && !attr(r.attrs, "course_num").empty() ? "course_num" : "course";
    const std::string course = b.table == BOARD_GZ.table ? "0" : std::to_string(num(r.attrs, course_key));
    std::string who = attr(r.attrs, b.who);
    if (who.empty()) who = attr(r.attrs, "ref_id");   // pop'n 13 on: the player's ref id
    std::vector<Record> all = from_lines(store_->get(*b.table, b.key));
    Record mine{{"course", course}, {"who", who}};
    for (const char *k : b.keep) mine[k] = attr(r.attrs, k);
    if (mine.count("area_num") && mine["area_num"].empty()) mine["area_num"] = attr(r.attrs, "pref");   // pop'n 12 on
    // pop'n 14 sends neither a score nor a name: the stages' scores added up, and the player's
    // name from the record the server keeps (tables::pdata, the name at 0x00, hex as the others)
    if (mine.count("score") && mine["score"].empty() && r.attrs.count("stage_score")) {
        long total = 0;
        const std::string ss = attr(r.attrs, "stage_score");
        for (size_t at = 0; at < ss.size();) {
            total += std::strtol(ss.c_str() + at, nullptr, 10);
            const size_t comma = ss.find(',', at);
            if (comma == std::string::npos) break;
            at = comma + 1;
        }
        mine["score"] = std::to_string(total);
    }
    if (mine.count("name") && mine["name"].empty() && safe_name(who)) {
        const std::string rec = b.key.empty() ? "" : store_->get(tables::pdata, {b.key[0], who});
        std::string hex;
        for (size_t i = 0; i < 12 && i < rec.size() && rec[i]; i++) {
            char h[3];
            snprintf(h, sizeof h, "%02X", (unsigned char)rec[i]);
            hex += h;
        }
        mine["name"] = hex;
    }
    bool update = true;
    bool found = false;
    for (Record &e : all) {
        if (e["course"] != course || e["who"] != who) continue;
        found = true;
        if (better(b, mine, e)) e = mine;   // a better result replaces the kept one
        else update = false;
    }
    if (!found) all.push_back(mine);
    std::string text;
    for (const Record &e : all) text += to_line(e);
    if (update) store_->put(*b.table, b.key, text);
    // the entrant's place in the course
    std::vector<Record> board;
    for (const Record &e : all)
        if (e.at("course") == course) board.push_back(e);
    std::stable_sort(board.begin(), board.end(), [&](const Record &x, const Record &y) { return better(b, x, y); });
    Entered out{mine, 1, board.size(), update};
    for (size_t i = 0; i < board.size(); i++)
        if (board[i].at("who") == who) { out.place = i + 1; out.kept = board[i]; }
    return out;
}

std::string Server::rank_get(const Board &b, const Request &r) {
    const std::vector<Record> all = from_lines(store_->get(*b.table, b.key));
    const bool courses = b.table != BOARD_GZ.table;
    const long c0 = courses ? num(r.attrs, "course_offset") : 0, cn = courses ? std::max(1L, num(r.attrs, "course_num")) : 1;
    const long r0 = std::max(1L, num(r.attrs, "rank_offset")), rn = std::min(100L, std::max(0L, num(r.attrs, "rank_num")));
    std::string out;
    for (long c = c0; c < c0 + cn && c < c0 + 20; c++) {
        std::vector<Record> board;
        for (const Record &e : all)
            if (std::strtol(attr(e, "course").c_str(), nullptr, 10) == c) board.push_back(e);
        std::stable_sort(board.begin(), board.end(), [&](const Record &x, const Record &y) { return better(b, x, y); });
        out += courses ? "<item course=\"" + std::to_string(c) + "\" entry=\"" + std::to_string(board.size()) + "\"/>"
                       : "<item entry=\"" + std::to_string(board.size()) + "\"/>";
        for (long i = r0 - 1; i < (long)board.size() && i < r0 - 1 + rn; i++) out += item(b, board[(size_t)i], (size_t)i + 1) + "/>";
    }
    return out;
}

// An entry as the game reads it: <item rank=.. (the board's fields) area=.. id=..
std::string Server::item(const Board &b, const Record &e, size_t place) {
    std::string o = "<item rank=\"" + std::to_string(place) + "\" ";
    if (b.table == BOARD_SCORE.table)
        o += "name=\"" + esc(attr(e, "name")) + "\" score=\"" + esc(attr(e, "score")) + "\" cool=\"" + esc(attr(e, "cool")) + "\" ";
    else if (b.table == BOARD_H.table)
        o += "name=\"" + esc(attr(e, "name")) + "\" music=\"" + esc(attr(e, "music")) + "\" combo=\"" + esc(attr(e, "combo")) + "\" ";
    else
        o += "prog=\"" + esc(attr(e, "prog")) + "\" parts=\"" + esc(attr(e, "parts")) + "\" shop_name=\"" + esc(attr(e, "shop_name")) + "\" ";
    const std::string id = !attr(e, "konami_id").empty() ? attr(e, "konami_id") : attr(e, "who");
    return o + "area=\"" + esc(attr(e, "area_num")) + "\" id=\"" + esc(id) + "\" ";
}

// pop'n 10-14's rankings, as rank.get's <b>. pop'n 10 (0x187be4 copies it to 0x18287c8,
// 0x1f30 bytes): 15 EXPERT courses of {count, 20 entries of 0x18: name (Shift-JIS, 13 bytes),
// place, prefecture, score, cool} (0x12b218 draws them; the score only with the game's score
// display on), then the shops by HOSHI NI NEGAIO wishes: {count, 20 entries of 0x24: shop name
// (25 bytes), place, prefecture, the wishes word (64-bit)} (0x13afa0). pop'n 11 (0x1932a8, 0x1f80
// bytes): the same courses (but the prefecture at 0x0d, the place at 0x0e), then the shops by
// POPN TOURIST finds: {count, 20 entries of 0x28: shop name (24 bytes), place, prefecture, the month (4-6), day, hour and minute it got there, the
// finds word, extra} (0x129240). pop'n 12 (0x1974a8, 0x1f30 bytes, at 0x181c360): pop'n 10's
// sizes, the prefecture before the place in both, and the shops' NINPOUCHO word at 0x20 (the
// scrolls drawn per bit). pop'n 13 (0x17eaed8, 0x22a4 bytes, at 0x160fb20): the same courses, then
// two shop boards (POP'N LAND's, POP'N WORLD's: 0x17eb010) of {count, 20 entries of 0x28: shop
// name (24 bytes), place, prefecture, stage, the area's words (low, high) at 0x20 and 0x24: the
// attractions built, drawn per bit}. pop'n 14 (0x17db420, 0x1894 bytes, at 0x15f28f8): 13 such
// courses, no shop board. Little-endian.
std::string Server::rank_table(const std::string &game) {
    const bool p11 = game == "D11", p13 = game == "E13", p14 = game == "F14", pref_first = game != "C20";
    // pop'n 15: 22 courses (0x2998 bytes, its check of the table 0x427e40), no shop board; 16 and
    // 17: 11 (0x14cc, 16 0x4374c0, 17 0x437be9)
    const bool p15 = game == "G15", p1617 = game == "H16" || game == "I17";
    std::string t(p11 ? 0x1f80 : p13 ? 0x22a4 : p14 ? 0x1894 : p15 ? 0x2998 : p1617 ? 0x14cc : 0x1f30, '\0');
    auto put32 = [&](size_t at, uint32_t v) { for (int k = 0; k < 4; k++) t[at + k] = (char)(v >> (8 * k) & 255); };
    auto bytes = [](const std::string &v) {   // the requests send names as hex Shift-JIS
        std::string out;
        bool hex = !v.empty() && v.size() % 2 == 0;
        for (char c : v) hex = hex && std::isxdigit((unsigned char)c);
        if (!hex) return v;
        for (size_t i = 0; i + 1 < v.size(); i += 2) out += (char)std::strtoul(v.substr(i, 2).c_str(), nullptr, 16);
        return out;
    };
    auto name_at = [&](size_t at, const std::string &v, size_t room) {
        const std::string b = bytes(v);
        for (size_t i = 0; i + 1 < room && i < b.size(); i++) t[at + i] = b[i];
    };
    const Board b{nullptr, {}, "card_id", EXP_KEEP, "score", "cool", 10};
    const std::vector<Record> all = from_lines(store_->get(tables::rank_exp, {game}));
    for (int course = 0; course < (p14 ? 13 : p15 ? 22 : p1617 ? 11 : 15); course++) {
        std::vector<Record> board;
        for (const Record &e : all)
            if (attr(e, "course") == std::to_string(course)) board.push_back(e);
        std::stable_sort(board.begin(), board.end(), [&](const Record &x, const Record &y) { return better(b, x, y); });
        const size_t base = (size_t)course * 0x1e4, n = std::min<size_t>(board.size(), 20);
        put32(base, (uint32_t)n);
        for (size_t i = 0; i < n; i++) {
            const size_t e = base + 4 + i * 0x18;
            name_at(e, attr(board[i], "name"), 13);
            t[e + (pref_first ? 0x0e : 0x0d)] = (char)(i + 1);   // pop'n 11 on have the prefecture first
            t[e + (pref_first ? 0x0d : 0x0e)] = (char)num(board[i], "area_num");
            put32(e + 0x10, (uint32_t)num(board[i], "score"));
            put32(e + 0x14, (uint32_t)num(board[i], "cool"));
        }
    }
    if (p14 || p15 || p1617) {   // no shop board; pop'n 15-17's comes whole (LZSS)
        while (p14 && !t.empty() && t.back() == '\0') t.pop_back();
        return t;
    }
    std::vector<Record> shops = from_lines(store_->get(tables::rank_shop, {game}));
    // the shops' event words, as sent: signed decimal ("%d"); pop'n 10's first records named them
    // praystar_lo/hi
    auto word = [](const Record &e, const char *k) {
        std::string v = attr(e, k);
        if (v.empty()) v = attr(e, std::string(k) == "mask" ? "praystar_lo" : "praystar_hi");
        return (uint32_t)std::strtoll(v.c_str(), nullptr, 10);
    };
    // pop'n 10's wishes are 64 bits; pop'n 11 counts its finds word only (0x12b250; extra is not
    // finds), pop'n 12 its NINPOUCHO word
    auto finds = [&](const Record &e) {
        return __builtin_popcountll(word(e, "mask") | (game == "C20" || p13 ? (uint64_t)word(e, "mask_hi") << 32 : 0));
    };
    std::stable_sort(shops.begin(), shops.end(), [&](const Record &x, const Record &y) {
        if (finds(x) != finds(y)) return finds(x) > finds(y);
        return attr(x, "when") < attr(y, "when");   // the earlier there first
    });
    // pop'n 13: POP'N LAND's board, then POP'N WORLD's (each shop's record names its board)
    const int boards = p13 ? 2 : 1;
    for (int bd = 0; bd < boards; bd++) {
        std::vector<Record> mine;
        for (const Record &e : shops)
            if (!p13 || num(e, "board") == bd) mine.push_back(e);
        const size_t size = p11 || p13 ? 0x28 : 0x24, base = 15 * 0x1e4 + (size_t)bd * (4 + 20 * size);
        const size_t n = std::min<size_t>(mine.size(), 20);
        put32(base, (uint32_t)n);
        for (size_t i = 0; i < n; i++) {
            const Record &shop = mine[i];
            const size_t e = base + 4 + i * size;
            if (p13) {
                name_at(e, attr(shop, "shop_name"), 0x18);
                t[e + 0x18] = (char)(i + 1);
                t[e + 0x19] = (char)num(shop, "area_num");
                t[e + 0x1a] = (char)num(shop, "stage");
                put32(e + 0x20, word(shop, "mask"));
                put32(e + 0x24, word(shop, "mask_hi"));
                continue;
            }
            name_at(e, attr(shop, "shop_name"), p11 ? 0x18 : 0x19);
            if (p11) {
                const std::string w = attr(shop, "when");   // MMDDhhmm
                t[e + 0x18] = (char)(i + 1);
                t[e + 0x19] = (char)num(shop, "area_num");
                for (int k = 0; k < 4 && w.size() == 8; k++) t[e + 0x1a + k] = (char)std::strtol(w.substr(2 * k, 2).c_str(), nullptr, 10);
                put32(e + 0x20, word(shop, "mask"));
                put32(e + 0x24, word(shop, "mask_hi"));
            } else {
                t[e + (pref_first ? 0x1a : 0x19)] = (char)(i + 1);
                t[e + (pref_first ? 0x19 : 0x1a)] = (char)num(shop, "area_num");
                if (game == "E12") {   // NINPOUCHO's word
                    put32(e + 0x20, word(shop, "mask"));
                } else {
                    put32(e + 0x1c, word(shop, "mask"));
                    put32(e + 0x20, word(shop, "mask_hi"));
                }
            }
        }
    }
    // the game clears its buffer before decoding (0x187be4): trailing zeros needn't be sent
    while (!t.empty() && t.back() == '\0') t.pop_back();
    return t;
}

// rank.get's answer: the table in <b> pieces of 768 bytes (the game's reader refuses one long
// text, and appends each <b> after the last: 0x19e300)
std::string Server::rank_b(const std::string &game) {
    const std::string t = rank_table(game);
    std::string out;
    for (size_t at = 0; at < t.size(); at += 768) out += "<b>" + base64(t.substr(at, 768)) + "</b>";
    return out;
}

// A shop's event report (pop'n 10 rank.shop praystar_lo/hi; pop'n 11 and 12 rank shop item/extra;
// pop'n 13 item_lo/item_hi and stage), kept per shop (and board) with when its finds last grew
// (the server's clock: MMDDhhmm, for pop'n 11's board)
void Server::rank_shop(const std::string &game, const Request &r, const char *mask, const char *mask_hi) {
    std::vector<Record> shops = from_lines(store_->get(tables::rank_shop, {game}));
    const std::string name = attr(r.attrs, "shop_name");
    const time_t now_t = now ? (time_t)now() : 0;
    const struct tm tm = sys::utc(now_t);
    char when[16];
    snprintf(when, sizeof when, "%02d%02d%02d%02d", tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
    const std::string area = attr(r.attrs, "area_num").empty() ? attr(r.attrs, "pref") : attr(r.attrs, "area_num");   // pop'n 12 on: pref
    Record mine{{"shop_name", name}, {"area_num", area}, {"mask", attr(r.attrs, mask)},
                {"mask_hi", attr(r.attrs, mask_hi)}, {"when", when}};
    if (r.attrs.count("stage")) mine["stage"] = attr(r.attrs, "stage");
    if (game == "E13") {   // the board of the shop's area (answer_game: game active); a shop has one per board
        mine["board"] = "0";
        for (const Record &e : from_lines(store_->get(tables::shop_phase, {game})))
            if (attr(e, "shop_name") == name && num(e, "phase") == 2) mine["board"] = "1";
    }
    auto bits = [&](const Record &e) {
        return __builtin_popcountll((uint32_t)std::strtoll(attr(e, "mask").c_str(), nullptr, 10) |
                                    (game == "C20" || game == "E13" ? (uint64_t)(uint32_t)std::strtoll(attr(e, "mask_hi").c_str(), nullptr, 10) << 32 : 0));
    };
    bool found = false;
    for (Record &e : shops) {
        if (attr(e, "shop_name") != name || attr(e, "board") != attr(mine, "board")) continue;
        if (bits(mine) <= bits(e)) mine["when"] = attr(e, "when");   // no more found: the date stays
        e = mine;
        found = true;
    }
    if (!found) shops.push_back(mine);
    std::string out;
    for (const Record &e : shops) out += to_line(e);
    store_->put(tables::rank_shop, {game}, out);
}

}  // namespace hk::ea
