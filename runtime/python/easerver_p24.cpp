// SPDX-License-Identifier: GPL-3.0-or-later
// The last 32-bit PC pop'n (M39, popn22.dll: runtime/pc32/, PLAN.md section 35) on the server
// (easerver.h): its modules info24 and player24, the protocol pop'n 24 on use (bemaniutils, public
// domain, answers pop'n 24-27 the same way).
//
// The game reads each part of an answer into a struct through AVS2's struct reader
// (property_psmap_import, libavs-win32 2.15's XCd229cc000005) with a descriptor of its own: 16-byte
// entries (a type byte, an array's 0x41 above its values'; a flags byte, 1: may be left out; the
// struct offset; the size, or an array's count; the node's name; a default). The parts' fields below
// are the game's, in its order, from those descriptors; a new player's values after bemaniutils.
#include "python/easerver_parts.h"

#include <algorithm>
#include <cstdlib>

namespace hk::py {

namespace {

const Field ACCOUNT[] = {
    {"g_pm_id", "str", 0, nullptr},
    {"name", "str", 0, nullptr},
    {"tutorial", "s16", 0, "0"},
    {"area_id", "s16", 0, "0"},
    {"use_navi", "s16", 0, "0"},
    {"read_news", "s16", 0, "0"},
    {"staff", "s8", 0, "0"},   // may be left out
    {"item_type", "s16", 0, "0"},   // may be left out
    {"item_id", "s16", 0, "0"},   // may be left out
    {"is_conv", "s8", 0, "0"},
    {"active_fr_num", "u8", 0, "0"},
    {"total_play_cnt", "s16", 0, "0"},
    {"today_play_cnt", "s16", 0, "0"},
    {"consecutive_days", "s16", 0, "0"},
    {"total_days", "s16", 0, "0"},
    {"interval_day", "s16", 0, "0"},
    {"my_best", "s16", 20, "-1"},
    {"latest_music", "s16", 10, "-1"},
    {"nice", "s16", 100, "-1"},
    {"favorite_chara", "s16", 20, "-1"},
    {"special_area", "s16", 8, "-1"},   // may be left out
    {"chocolate_charalist", "s16", 5, "-1"},   // may be left out
    {"chocolate_sp_chara", "s32", 0, "-1"},   // may be left out
    {"chocolate_pass_cnt", "s32", 0, "0"},   // may be left out
    {"chocolate_hon_cnt", "s32", 0, "0"},   // may be left out
    {"teacher_setting", "s16", 10, "-1"},   // may be left out
    {"license_data", "s16", 20, "-1"},   // may be left out
    {"welcom_pack", "bool", 0, "0"},   // may be left out
    {"ranking_node", "s32", 0, "0"},
    {"chara_ranking_kind_id", "s32", 0, "0"},
    {"navi_evolution_flg", "s8", 0, "0"},   // may be left out
    {"ranking_news_last_no", "s32", 0, "0"},
    {"power_point", "s32", 0, "0"},
    {"player_point", "s32", 0, "300"},
    {"power_point_list", "s32", 20, "-1"},
    {"sp_riddles_id", "s16", 0, "-1"},   // may be left out
    {"option_tuto", "bool", 0, "0"},   // may be left out
    {"sc_news_no", "s32", 0, "-1"},   // may be left out
};
const Field INFO[] = {
    {"ep", "u16", 0, "0"},
};
const Field CONFIG[] = {
    {"mode", "u8", 0, "0"},
    {"chara", "s16", 0, "-1"},
    {"music", "s16", 0, "-1"},
    {"sheet", "u8", 0, "0"},
    {"category", "s8", 0, "-1"},
    {"sub_category", "s8", 0, "-1"},
    {"chara_category", "s8", 0, "-1"},
    {"course_id", "s16", 0, "-1"},
    {"course_folder", "s8", 0, "-1"},
    {"ms_banner_disp", "s8", 0, "-1"},
    {"ms_down_info", "s8", 0, "-1"},
    {"ms_side_info", "s8", 0, "-1"},
    {"ms_raise_type", "s8", 0, "-1"},
    {"ms_rnd_type", "s8", 0, "-1"},
    {"banner_sort", "s8", 0, "-1"},
};
const Field OPTION[] = {
    {"hispeed", "s16", 0, "10"},
    {"popkun", "u8", 0, "0"},
    {"hidden", "bool", 0, "0"},
    {"hidden_rate", "s16", 0, "0"},
    {"sudden", "bool", 0, "0"},
    {"sudden_rate", "s16", 0, "0"},
    {"randmir", "s8", 0, "0"},
    {"gauge_type", "s8", 0, "0"},
    {"ojama_0", "u8", 0, "0"},
    {"ojama_1", "u8", 0, "0"},
    {"forever_0", "bool", 0, "0"},
    {"forever_1", "bool", 0, "0"},
    {"full_setting", "bool", 0, "0"},
    {"judge", "u8", 0, "0"},
    {"guide_se", "s8", 0, "0"},
    {"guide_se_vol", "u8", 0, "3"},   // may be left out
    {"lift", "bool", 0, "0"},
    {"lift_rate", "s16", 0, "0"},
};
const Field CUSTOMIZE[] = {
    {"effect_left", "u16", 0, "0"},
    {"effect_center", "u16", 0, "0"},
    {"effect_right", "u16", 0, "0"},
    {"hukidashi", "u16", 0, "0"},
    {"comment_1", "u16", 0, "0"},
    {"comment_2", "u16", 0, "0"},
};
const Field CUSTOM_CATE[] = {
    {"valid", "s8", 0, "0"},
    {"lv_min", "s8", 0, "-1"},
    {"lv_max", "s8", 0, "-1"},
    {"medal_min", "s8", 0, "-1"},
    {"medal_max", "s8", 0, "-1"},
    {"friend_no", "s8", 0, "-1"},
    {"score_flg", "s8", 0, "-1"},
};
const Field EVENT2021[] = {
    {"point", "u32", 0, "0"},   // may be left out
    {"step", "u8", 0, "0"},   // may be left out
    {"quest_point", "u32", 8, "0"},   // may be left out
    {"step_nos", "u8", 0, "0"},   // may be left out
    {"quest_point_nos", "u32", 13, "0"},   // may be left out
};
// netvs: every field may be left out, but set_recommend's default is broken (as the 64-bit game's:
// its "pointer" is 1), so the arrays are given; the six dialog strings left to their default
const Field NETVS[] = {
    {"record", "s16", 6, "0"},          {"ojama_condition", "s8", 74, "0"}, {"set_ojama", "s8", 3, "0"},
    {"set_recommend", "s8", 3, "0"},    {"netvs_play_cnt", "u32", 0, "0"},
};

// event_p28, its burger shop's state (the 0x10387c78 descriptor), as write sends it; given back without
// current_order (the orders it names come as <order> lists the game reads after it, 0x10387cc8, which
// write doesn't send: an order it names that isn't there is "out of range", and the game faulted after
// the next stage, popn22.dll+0xca719; left out, it picks its orders anew)
const Field EVENT_P28[] = {
    {"burger_first_play", "bool", 0, "0"}, {"burger_daily_bonus", "bool", 0, "0"},
    {"current_order", "s16", 3, "0"},      {"neko_daily_bonus", "bool", 0, "0"},
};
const Field EVENT_P28_BACK[] = {
    {"burger_first_play", "bool", 0, "0"}, {"burger_daily_bonus", "bool", 0, "0"}, {"neko_daily_bonus", "bool", 0, "0"},
};

// A list of numbers kept as "a b c": the first n, padded with -1, as an s16 array's values
std::string first_n(const std::string &list, size_t n) {
    std::string out;
    size_t at = 0, count = 0;
    while (count < n) {
        while (at < list.size() && list[at] == ' ') at++;
        const size_t e = list.find(' ', at);
        const std::string v = at < list.size() ? list.substr(at, e == std::string::npos ? std::string::npos : e - at) : "-1";
        out += (count ? " " : "") + v;
        count++;
        at = e == std::string::npos ? list.size() : e;
    }
    return out;
}

// The common info, info24.common's and player24.start's: the areas (open, no end), the chocolate
// event's five, the shop's goods (bemaniutils' prices)
std::string common_info() {
    std::string x;
    for (int i = 1; i <= 16; i++)
        x += "<area><area_id __type=\"s16\">" + std::to_string(i) + "</area_id><end_date __type=\"u64\">0</end_date>"
             "<medal_id __type=\"s16\">" + std::to_string(i) + "</medal_id><is_limit __type=\"bool\">0</is_limit></area>";
    for (int i = 0; i < 5; i++)
        x += "<choco><choco_id __type=\"s16\">" + std::to_string(i) + "</choco_id><param __type=\"s32\">-1</param></choco>";
    for (int i = 0; i < 98; i++) {
        const int price = i < 15 ? 30 : i < 30 ? 40 : i < 45 ? 60 : i < 60 ? 80 : 200;
        x += "<goods><item_id __type=\"s32\">" + std::to_string(i + 1) + "</item_id><item_type __type=\"s16\">3</item_type>"
             "<price __type=\"s32\">" + std::to_string(price) + "</price><goods_type __type=\"s16\">0</goods_type></goods>";
    }
    return x;
}

}  // namespace

std::string EamuseServer::answer_p24(const Request &r) {
    const std::string &m = r.module, &f = r.method;
    if (m == "info24") return f == "common" ? "<info24 status=\"0\">" + common_info() + "</info24>" : "<info24 status=\"0\"/>";
    std::string ref = child(r.inner, "ref_id");
    if (ref.empty()) ref = attr(r.attrs, "ref_id");
    if (f == "start") {   // a credit's start (with or without a player): its play id, and the common info
        Record count = from_lines(read_file("p24-plays")).empty() ? Record{} : from_lines(read_file("p24-plays"))[0];
        count["n"] = std::to_string(num(count, "n") + 1);
        write_file("p24-plays", to_line(count));
        return "<player24 status=\"0\"><play_id __type=\"s32\">" + attr(count, "n") + "</play_id>" + common_info() + "</player24>";
    }
    if (!safe_name(ref)) return "<player24 status=\"0\"/>";
    const std::string file = "p24-" + ref;
    std::vector<Record> players = from_lines(read_file(file));
    auto record = [&](const Record &p) {
        // g_pm_id: the ref id's last eight digits, as 1234-5678
        const std::string tail = ref.size() >= 8 ? ref.substr(ref.size() - 8) : ref;
        std::map<std::string, std::string> account = kept(p, "account", ACCOUNT);
        account["g_pm_id"] = tail.substr(0, 4) + "-" + tail.substr(4);
        account["name"] = attr(p, "name");
        // the plays with this one (the game expects it from 1: with 0 on a first credit it plays a
        // broken tutorial stage on the next, as bemaniutils notes)
        account["total_play_cnt"] = std::to_string(num(p, "account.total_play_cnt") + 1);
        // what the game leaves to the server: the latest songs played, the most played (by the scores' counts)
        account["latest_music"] = first_n(attr(p, "latest"), 10);
        std::vector<std::pair<long, std::string>> plays;
        for (const Record &e : from_lines(read_file("p24s-" + ref))) {
            bool merged = false;
            for (auto &[n, music] : plays)
                if (music == attr(e, "music")) n += num(e, "cnt"), merged = true;
            if (!merged) plays.push_back({num(e, "cnt"), attr(e, "music")});
        }
        std::stable_sort(plays.begin(), plays.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
        std::string best;
        for (const auto &[n, music] : plays) best += (best.empty() ? "" : " ") + music;
        account["my_best"] = first_n(best, 20);
        // the characters' friendship (chara_param), as the game wrote them: "id:friendship" each
        std::string charas;
        for (size_t at = 0; at < attr(p, "charas").size();) {
            const std::string c = attr(p, "charas");
            size_t e = c.find(' ', at);
            if (e == std::string::npos) e = c.size();
            const std::string one = c.substr(at, e - at);
            if (const size_t colon = one.find(':'); colon != std::string::npos)
                charas += "<chara_param><chara_id __type=\"u16\">" + esc(one.substr(0, colon)) + "</chara_id><friendship __type=\"u16\">" +
                          esc(one.substr(colon + 1)) + "</friendship></chara_param>";
            at = e + 1;
        }
        return "<player24 status=\"0\"><result __type=\"s8\">0</result>" + part("account", ACCOUNT, account) +
               part("info", INFO, kept(p, "info", INFO)) + part("config", CONFIG, kept(p, "config", CONFIG)) +
               part("option", OPTION, kept(p, "option", OPTION)) + part("customize", CUSTOMIZE, kept(p, "customize", CUSTOMIZE)) +
               part("netvs", NETVS, {}) + part("custom_cate", CUSTOM_CATE, {}) + part("event2021", EVENT2021, kept(p, "event2021", EVENT2021)) +
               charas + (p.count("event_p28.burger_first_play") ? part("event_p28", EVENT_P28_BACK, kept(p, "event_p28", EVENT_P28_BACK)) : "") +
               "</player24>";
    };
    if (f == "read") {   // 2: no player (the game asks for a name, then sends new)
        if (players.empty()) return "<player24 status=\"0\"><result __type=\"s8\">2</result></player24>";
        return record(players[0]);
    }
    if (f == "new") {
        const Record p = {{"name", child(r.inner, "name")}};
        write_file(file, to_line(p));
        return record(p);
    }
    if (players.empty()) return "<player24 status=\"0\"/>";
    Record &p = players[0];
    const std::string scores = "p24s-" + ref;
    if (f == "read_option")   // a song's options: the kept ones
        return "<player24 status=\"0\">" + part("option", OPTION, kept(p, "option", OPTION)) + "</player24>";
    if (f == "read_score") {   // every chart's best (bemaniutils' answer)
        std::string x = "<player24 status=\"0\">";
        for (const Record &e : from_lines(read_file(scores)))
            x += "<music><music_num __type=\"s16\">" + esc(attr(e, "music")) + "</music_num><sheet_num __type=\"u8\">" +
                 esc(attr(e, "sheet")) + "</sheet_num><score __type=\"s32\">" + esc(attr(e, "score")) + "</score><clear_type __type=\"u8\">" +
                 esc(attr(e, "clear_type")) + "</clear_type><clear_rank __type=\"u8\">" + esc(attr(e, "clear_rank")) +
                 "</clear_rank><cnt __type=\"s16\">" + esc(attr(e, "cnt")) + "</cnt></music>";
        return x + "</player24>";
    }
    if (f == "write_music") {   // a song played: the chart's best kept, and the options when it asks
        const std::string music = child(r.inner, "music_num"), sheet = child(r.inner, "sheet_num");
        std::vector<Record> all = from_lines(read_file(scores));
        Record *e = nullptr;
        for (Record &x : all)
            if (attr(x, "music") == music && attr(x, "sheet") == sheet) e = &x;
        if (!e) {
            all.push_back({{"music", music}, {"sheet", sheet}, {"score", "0"}, {"clear_type", "0"}, {"clear_rank", "0"}, {"cnt", "0"}});
            e = &all.back();
        }
        for (const char *k : {"score", "clear_type", "clear_rank"}) {
            const long v = std::strtol(child(r.inner, k).c_str(), nullptr, 10);
            if (v > num(*e, k)) (*e)[k] = std::to_string(v);
        }
        (*e)["cnt"] = std::to_string(num(*e, "cnt") + 1);
        std::string t;
        for (const Record &x : all) t += to_line(x);
        write_file(scores, t);
        if (child(r.inner, "is_option_save") == "1") keep(p, "option", OPTION, r.inner);
        write_file(file, to_line(p));
        return "<player24 status=\"0\"/>";
    }
    if (f == "write") {   // the credit's end: the parts it sends kept as they are, and the play counted
        keep(p, "account", ACCOUNT, child(r.inner, "account"));
        keep(p, "info", INFO, child(r.inner, "info"));
        keep(p, "config", CONFIG, child(r.inner, "config"));
        keep(p, "option", OPTION, child(r.inner, "option"));
        keep(p, "customize", CUSTOMIZE, child(r.inner, "customize"));
        keep(p, "event2021", EVENT2021, child(r.inner, "event2021"));
        keep(p, "event_p28", EVENT_P28, child(r.inner, "event_p28"));
        p["account.total_play_cnt"] = std::to_string(num(p, "account.total_play_cnt") + 1);
        // the stages' songs, newest first, ahead of the latest kept (each once); each chara_param's friendship
        std::string latest = attr(p, "latest"), charas = attr(p, "charas");
        for (size_t at = r.inner.find("<stage>"); at != std::string::npos; at = r.inner.find("<stage>", at + 1)) {
            const std::string music = child(r.inner.substr(at), "no");
            if (music.empty()) continue;
            std::string rest;
            for (size_t b = 0; b < latest.size();) {
                size_t e = latest.find(' ', b);
                if (e == std::string::npos) e = latest.size();
                if (const std::string m = latest.substr(b, e - b); !m.empty() && m != music) rest += (rest.empty() ? "" : " ") + m;
                b = e + 1;
            }
            latest = music + (rest.empty() ? "" : " " + rest);
        }
        p["latest"] = latest;
        for (size_t at = r.inner.find("<chara_param>"); at != std::string::npos; at = r.inner.find("<chara_param>", at + 1)) {
            const std::string one = r.inner.substr(at);
            const std::string id = child(one, "chara_id"), friendship = child(one, "friendship");
            std::string kept_list;   // the others, then this one
            for (size_t b = 0; b < charas.size();) {
                size_t e = charas.find(' ', b);
                if (e == std::string::npos) e = charas.size();
                const std::string c = charas.substr(b, e - b);
                if (!c.empty() && c.substr(0, c.find(':')) != id) kept_list += (kept_list.empty() ? "" : " ") + c;
                b = e + 1;
            }
            charas = kept_list + (kept_list.empty() ? "" : " ") + id + ":" + friendship;
        }
        p["charas"] = charas;
        write_file(file, to_line(p));
        return "<player24 status=\"0\"/>";
    }
    return "<player24 status=\"0\"/>";   // logout, buy, friend, update_ranking, ...
}

}  // namespace hk::py
