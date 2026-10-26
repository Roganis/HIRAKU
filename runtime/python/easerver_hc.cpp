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

#include <cstdlib>

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

}  // namespace

std::string EamuseServer::answer_hc(const Request &r) {
    const std::string &f = r.method;
    std::string ref = child(r.inner, "ref_id");
    if (ref.empty()) ref = attr(r.attrs, "ref_id");   // start, logout: an attribute
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
               part("customize", CUSTOMIZE, kept(p, "customize", CUSTOMIZE)) + part("netvs", NETVS, {}) + "</player>";
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
    if (f == "start") {   // play_id (the game's own count goes on from it)
        p["play_id"] = std::to_string(num(p, "play_id") + 1);
        write_file(file, to_line(p));
        return "<player status=\"0\"><play_id __type=\"s32\">" + esc(attr(p, "play_id")) + "</play_id></player>";
    }
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
        write_file(file, to_line(p));
        return "<player status=\"0\"/>";
    }
    return "<player status=\"0\"/>";   // end, logout, ...
}

}  // namespace hk::py
