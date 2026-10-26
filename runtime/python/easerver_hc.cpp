// SPDX-License-Identifier: GPL-3.0-or-later
// The 64-bit PC pop'n (M39: runtime/pc64/, PLAN.md section 32) on the server
// (easerver.h): its module "player".
//
// The game reads each part of a player's record into a struct through AVS2's struct reader
// (avs2-core XCgsqzn00000b2: node, struct, descriptor). A descriptor is a list of 24-byte entries
// (a type byte; a flags byte, 1: may be left out, the default in the last word; the struct offset;
// the size, or an array's count; the node's name), so the record's XML is read off the program.
// The type byte is a node's type (2 s8, 3 u8, 4 s16, 5 u16, 6 s32, 7 u32), but 0x0a a string,
// 0x32 a bool and an array 0x41 above its values' (0x43 s8, 0x45 s16, 0x47 s32: avs2-core's table
// at 0x1800b4b40), and a node of another type isn't read (its log: psmap "failed to read"). player.read's and player.new's answers (0x1802c4a80,
// 0x1802c3fa0) go through the same parser (0x18009b180): ten parts in turn, each to succeed:
//   account (0x1807d3e00), info (0x180704200), config (0x1807d4310) and option (0x1807d4100),
//   item (any number), chara_param and chara_param_old (any number), customize (0x180703f00),
//   netvs (0x1807d47d0: every field may be left out, but see NETVS), course_data (any number), ex_info (any
//   number), event_p29 (may be left out).
// Below, each part's fields (those that may be left out last), with a new player's values. read's <result>: 0 the
// record follows, 1 a player of an older game to carry over (name, chara, con_type, medal_cnt),
// 2 none (the game asks for a name, then sends player.new).
#include "python/easerver_parts.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <iterator>

namespace hk::py {

namespace {

const Field ACCOUNT[] = {
    {"g_pm_id", "str", 0, nullptr},   // the player's friend id (12 characters at most): from the ref id
    {"name", "str", 0, nullptr},      // as player.new gave it (Shift-JIS, as the game sends it)
    {"tutorial", "s16", 0, "0"},
    {"read_news", "s16", 0, "0"},
    {"is_conv", "s8", 0, "0"},
    {"active_fr_num", "u8", 0, "0"},
    {"total_play_cnt", "s16", 0, "0"},
    {"latest_music", "s16", 30, "-1"},
    {"nice", "s16", 100, "-1"},
    {"favorite_chara", "s16", 100, "-1"},
    {"power_point", "s32", 0, "0"},
    {"power_point_list", "s32", 20, "-1"},
    // may be left out (the game's defaults)
    {"popn_class", "s8", 0, "-1"},
    {"option_tuto", "bool", 0, "0"},
    {"sc_news_no", "s32", 0, "-1"},
    {"read_policy", "s16", 0, "0"},
    {"language", "s8", 0, "-1"},
};
const Field INFO[] = {{"ep", "u16", 0, "0"}, {"estatus", "u16", 0, "0"}};
const Field CONFIG[] = {
    {"mode", "u8", 0, "0"},          {"chara", "s16", 0, "-1"},          {"music", "s16", 0, "-1"},
    {"sheet", "u8", 0, "0"},         {"category", "s8", 0, "1"},         {"sub_category", "s8", 0, "0"},
    {"chara_category", "s8", 0, "0"}, {"ms_banner_disp", "s8", 0, "0"}, {"ms_down_info", "s8", 0, "0"},
    {"ms_raise_type", "s8", 0, "0"}, {"banner_sort", "s8", 0, "0"},      {"disp_setting", "s8", 0, "0"},
    {"h_vol", "s8", 0, "1"},         {"hiscore_disp", "bool", 0, "0"},   {"lane_type", "s8", 0, "0"},
    {"brightness", "s8", 0, "100"},  {"key_beam", "s8", 0, "100"},       {"lane_line", "s8", 0, "1"},
};
const Field OPTION[] = {
    {"hispeed", "s16", 0, "10"},    {"popkun", "u8", 0, "0"},       {"hidden", "bool", 0, "0"},
    {"sudden", "bool", 0, "0"},     {"randmir", "s8", 0, "0"},      {"gauge_type", "s8", 0, "0"},
    {"ojama_0", "u8", 0, "0"},      {"ojama_1", "u8", 0, "0"},      {"forever_0", "bool", 0, "0"},
    {"forever_1", "bool", 0, "0"},  {"full_setting", "bool", 0, "0"}, {"judge", "u8", 0, "0"},
    {"guide_se", "s8", 0, "0"},     {"lift", "bool", 0, "0"},       {"lift_rate", "s16", 0, "0"},
    {"judge_ad", "s8", 0, "0"},     {"roof", "s16", 0, "0"},        {"long_pop", "s8", 0, "0"},
    {"hidden_rate", "s16", 0, "-70"}, {"sudden_rate", "s16", 0, "-270"}, {"guide_se_vol", "u8", 0, "3"},
};
const Field CUSTOMIZE[] = {
    {"seal_0", "u16", 0, "0"}, {"seal_1", "u16", 0, "0"},   {"seal_2", "u16", 0, "0"},     {"seal_3", "u16", 0, "0"},
    {"seal_4", "u16", 0, "0"}, {"seal_5", "u16", 0, "0"},   {"seal_6", "u16", 0, "0"},     {"seat", "u16", 0, "0"},
    {"touch_th", "u16", 0, "0"}, {"lane_cover", "u16", 0, "0"}, {"stage_bk", "u16", 0, "0"}, {"highlight", "u16", 0, "0"},
};
// netvs: every field may be left out, but set_recommend's default is broken (its "pointer" is 1:
// the struct reader faults, as the game never meets it without the field), so the arrays are given;
// the six dialog strings (an empty one isn't taken) left to their default, ""
const Field NETVS[] = {
    {"record", "s16", 6, "0"},          {"ojama_condition", "s8", 74, "0"}, {"set_ojama", "s8", 3, "0"},
    {"set_recommend", "s8", 3, "0"},    {"netvs_play_cnt", "u32", 0, "0"},
};

// The common info, info.common's answer (read by 0x180092e80; the game asks it at start-up and with
// a card, not player.start): the events' phases (<phase>: event_id, phase). The game keeps eight,
// each clamped to a last phase of its own (0x1805f4ac0: 7, 1, 3, none, 11, 2, 1, 1; event 3, the
// earlier games' NET TAISEN, has none in this game). A song may wait for an event's phase (the
// table at 0x1804e2d30: event, phase, song): event 0 opens new songs by stages (19 songs, phases
// 1-7), 4 an event's songs (17, phases 1-11); event 1 gives charts (113 songs). The others' meaning
// isn't known. Each at its last phase unless the data folder's phases-phc says otherwise ("EVENT=PHASE
// ..."). (Its license_music, the songs a cabinet is licensed for, is HIRAKU's loader's business:
// runtime/pc64/songs.c.)
constexpr int SONGS = 2374;   // the game's songs: its items of type 0 (0x1804e3240)
constexpr int LAST_PHASE[8] = {7, 1, 3, -1, 11, 2, 1, 1};

std::string common(const std::string &setting) {
    int phase[8];
    std::copy(std::begin(LAST_PHASE), std::end(LAST_PHASE), phase);
    for (size_t at = 0; (at = setting.find('=', at)) != std::string::npos; at++) {
        size_t from = at;
        while (from > 0 && std::isdigit((unsigned char)setting[from - 1])) from--;
        const long ev = std::strtol(setting.c_str() + from, nullptr, 10);
        if (from < at && ev >= 0 && ev < 8) phase[ev] = (int)std::strtol(setting.c_str() + at + 1, nullptr, 10);
    }
    std::string x;
    for (int ev = 0; ev < 8; ev++)
        if (phase[ev] >= 0)
            x += "<phase><event_id __type=\"s16\">" + std::to_string(ev) + "</event_id><phase __type=\"s16\">" + std::to_string(phase[ev]) +
                 "</phase></phase>";
    return x;
}

// The player's items (item, 0x1807042a0: type, id, param, is_new, get_time), in arrays by type
// (0x1804e3240: type 0 the songs, 2374): those the game wrote ("type:id:param" each), and with the
// data folder's unlock-phc not 0 (the default), every song as type 0 with its locked charts. A song
// an event or a purchase opened (mask bits 0x68000006) is listed only for a player who has its item
// (0x180064710); a locked Hyper (mask bit 0x40) or EX (0x80) chart is there only when the item's
// param has 4 or 8 (0x180076870; the music select shows its level as none).
std::string items(const Record &p, bool every_song) {
    std::map<std::pair<int, int>, long> got;   // (type, id) -> param
    const std::string kept = attr(p, "items");
    for (size_t at = 0; at < kept.size();) {
        size_t end = kept.find(' ', at);
        if (end == std::string::npos) end = kept.size();
        int type = 0, id = 0;
        long param = 0;
        if (std::sscanf(kept.substr(at, end - at).c_str(), "%d:%d:%ld", &type, &id, &param) == 3) got[{type, id}] = param;
        at = end + 1;
    }
    if (every_song)
        for (int id = 0; id < SONGS; id++) got[{0, id}] |= 4 | 8;
    std::string x;
    for (const auto &[k, param] : got)
        x += "<item><type __type=\"u8\">" + std::to_string(k.first) + "</type><id __type=\"u16\">" + std::to_string(k.second) +
             "</id><param __type=\"u16\">" + std::to_string(param) + "</param><is_new __type=\"bool\">0</is_new>"
             "<get_time __type=\"u64\">0</get_time></item>";
    return x;
}

}  // namespace

std::string EamuseServer::answer_hc(const Request &r) {
    const std::string &f = r.method;
    if (r.module == "info") return f == "common" ? "<info status=\"0\">" + common(read_file("phases-phc")) + "</info>" : "<info status=\"0\"/>";
    std::string ref = child(r.inner, "ref_id");
    if (ref.empty()) ref = attr(r.attrs, "ref_id");   // start, logout: an attribute
    if (f == "start") {   // play_id (the game's own count goes on from it), and the common info; a guest too
        const bool known = safe_name(ref);
        std::vector<Record> players = known ? from_lines(read_file("phc-" + ref)) : std::vector<Record>{};
        Record p = players.empty() ? Record{} : players[0];
        p["play_id"] = std::to_string(num(p, "play_id") + 1);
        if (!players.empty()) write_file("phc-" + ref, to_line(p));
        return "<player status=\"0\"><play_id __type=\"s32\">" + esc(attr(p, "play_id")) + "</play_id>" + common(read_file("phases-phc")) +
               "</player>";
    }
    if (!safe_name(ref)) return "<player status=\"0\"/>";
    const std::string file = "phc-" + ref, scores = "phcs-" + ref;
    std::vector<Record> players = from_lines(read_file(file));
    auto record = [&](const Record &p) {
        // g_pm_id: the ref id's last eight digits, as 1234-5678
        const std::string tail = ref.size() >= 8 ? ref.substr(ref.size() - 8) : ref;
        std::map<std::string, std::string> account = kept(p, "account", ACCOUNT);
        account.erase("name");
        account["g_pm_id"] = tail.substr(0, 4) + "-" + tail.substr(4);
        account["name"] = attr(p, "name");
        return "<player status=\"0\"><result __type=\"s8\">0</result>" + part("account", ACCOUNT, account) + part("info", INFO, {}) +
               part("config", CONFIG, kept(p, "config", CONFIG)) + part("option", OPTION, kept(p, "option", OPTION)) +
               items(p, read_file("unlock-phc").rfind("0", 0) != 0) + part("customize", CUSTOMIZE, kept(p, "customize", CUSTOMIZE)) +
               part("netvs", NETVS, {}) + "</player>";
    };
    if (f == "read") {
        if (players.empty()) return "<player status=\"0\"><result __type=\"s8\">2</result></player>";   // none: the game makes one
        return record(players[0]);
    }
    if (f == "new") {
        const Record p = {{"name", child(r.inner, "name")}};
        write_file(file, to_line(p));
        return record(p);
    }
    if (players.empty()) return "<player status=\"0\"/>";
    Record &p = players[0];
    if (f == "read_score") {   // every chart's best, in one answer (con_flg: more to ask for)
        std::string x = "<player status=\"0\"><con_flg __type=\"bool\">0</con_flg>";
        for (const Record &s : from_lines(read_file(scores)))
            x += "<music><music_num __type=\"s16\">" + esc(attr(s, "music")) + "</music_num><sheet_num __type=\"u8\">" +
                 esc(attr(s, "sheet")) + "</sheet_num><score __type=\"s32\">" + esc(attr(s, "score")) + "</score><clear_type __type=\"u8\">" +
                 esc(attr(s, "clear_type")) + "</clear_type><clear_rank __type=\"u8\">" + esc(attr(s, "clear_rank")) +
                 "</clear_rank><cnt __type=\"s16\">" + esc(attr(s, "cnt")) + "</cnt><score_ver __type=\"s32\">0</score_ver>"
                 "<clear_type_ver __type=\"u8\">0</clear_type_ver></music>";
        return x + "</player>";
    }
    if (f == "read_option")   // a song's options: the kept ones
        return "<player status=\"0\">" + part("option", OPTION, kept(p, "option", OPTION)) + "</player>";
    if (f == "write_music") {   // a song played: the chart's best kept, and the options when it asks
        const std::string music = child(r.inner, "music_num"), sheet = child(r.inner, "sheet_num");
        std::vector<Record> all = from_lines(read_file(scores));
        Record *s = nullptr;
        for (Record &e : all)
            if (attr(e, "music") == music && attr(e, "sheet") == sheet) s = &e;
        if (!s) {
            all.push_back({{"music", music}, {"sheet", sheet}, {"score", "0"}, {"clear_type", "0"}, {"clear_rank", "0"}, {"cnt", "0"}});
            s = &all.back();
        }
        for (const char *k : {"score", "clear_type", "clear_rank"}) {
            const long v = std::strtol(child(r.inner, k).c_str(), nullptr, 10);
            if (v > num(*s, k)) (*s)[k] = std::to_string(v);
        }
        (*s)["cnt"] = std::to_string(num(*s, "cnt") + 1);
        std::string t;
        for (const Record &e : all) t += to_line(e);
        write_file(scores, t);
        if (child(r.inner, "is_option_save") == "1") keep(p, "option", OPTION, r.inner);
        p["account.total_play_cnt"] = std::to_string(num(p, "account.total_play_cnt") + 1);
        write_file(file, to_line(p));
        return "<player status=\"0\"/>";
    }
    if (f == "write") {   // the credit's end: the parts it sends kept as they are
        keep(p, "account", ACCOUNT, child(r.inner, "account"));   // (no name, id or play count: kept here)
        keep(p, "config", CONFIG, child(r.inner, "config"));
        keep(p, "option", OPTION, child(r.inner, "option"));
        keep(p, "customize", CUSTOMIZE, child(r.inner, "customize"));
        // the items the game sends (those it gave or changed in the credit), over those kept
        std::map<std::pair<int, int>, std::string> got;
        const std::string had = attr(p, "items");
        for (size_t at = 0; at < had.size();) {
            size_t end = had.find(' ', at);
            if (end == std::string::npos) end = had.size();
            int type = 0, id = 0;
            if (std::sscanf(had.substr(at, end - at).c_str(), "%d:%d:", &type, &id) == 2) got[{type, id}] = had.substr(at, end - at);
            at = end + 1;
        }
        for (size_t at = r.inner.find("<item>"); at != std::string::npos; at = r.inner.find("<item>", at + 1)) {
            const std::string item = r.inner.substr(at, r.inner.find("</item>", at) - at);
            const int type = std::atoi(child(item, "type").c_str()), id = std::atoi(child(item, "id").c_str());
            got[{type, id}] = std::to_string(type) + ":" + std::to_string(id) + ":" + std::to_string(std::atol(child(item, "param").c_str()));
        }
        std::string list;
        for (const auto &[k, v] : got) list += (list.empty() ? "" : " ") + v;
        if (!list.empty()) p["items"] = list;
        write_file(file, to_line(p));
        return "<player status=\"0\"/>";
    }
    return "<player status=\"0\"/>";   // end, logout, ...
}

}  // namespace hk::py
