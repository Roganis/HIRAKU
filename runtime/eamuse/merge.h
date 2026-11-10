// SPDX-License-Identifier: GPL-3.0-or-later
// One game's e-AMUSEMENT folder moved into the folder all the pop'n games share (pop'n 9-14 in the
// saves' default places, runtime/python/core.cpp: a player carried from one game to the next is
// found there). A file the shared folder lacks moves over; the card counters keep the larger; the
// card and KONAMI ID lists gain what they lack. Anything else already there stays, and the old
// folder is kept, renamed.
//
// The players of pop'n 13 and 14 are kept by ref id, a count that began at 1 in each folder: two
// folders' players have the same numbers without being the same players. So a folder's ref ids are
// made the shared folder's before its files move:
//   a card the shared folder knows    its player there is this card's: the folder's files for it
//                                     take that ref id
//   a card it doesn't, whose ref id   a new ref id, from the count
//   another card has there
// in the folder's card list, its files' names (pdata-F14-REF, pscore-F14-REF) and its rankings'
// entries (who=REF). Without this a card read its other game's player from someone else's file.
// A player's file under a ref id that no card of the folder has stays in the old folder if a card
// of the shared folder has that ref id.
#pragma once
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "shell/sys.h"

namespace hk::ea {

namespace merging {

struct Field {
    std::string key, value;
    bool plain = false;   // no "=" in it: written back as it was
};
using Line = std::vector<Field>;   // a record's fields, in the file's order

inline std::vector<Line> lines(const std::string &text) {
    std::vector<Line> out;
    std::istringstream in(text);
    for (std::string l; std::getline(in, l);) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (l.empty()) continue;
        Line line;
        for (size_t at = 0; at <= l.size();) {
            size_t end = l.find('\t', at);
            if (end == std::string::npos) end = l.size();
            const std::string field = l.substr(at, end - at);
            const size_t eq = field.find('=');
            if (eq == std::string::npos) line.push_back({field, "", true});
            else line.push_back({field.substr(0, eq), field.substr(eq + 1), false});
            at = end + 1;
        }
        out.push_back(line);
    }
    return out;
}

inline std::string text(const std::vector<Line> &all) {
    std::string out;
    for (const Line &line : all) {
        for (size_t i = 0; i < line.size(); i++) out += (i ? "\t" : "") + line[i].key + (line[i].plain ? "" : "=" + line[i].value);
        out += "\n";
    }
    return out;
}

inline std::string get(const Line &line, const char *key) {
    for (const Field &f : line)
        if (f.key == key && !f.plain) return f.value;
    return "";
}

inline void set(Line &line, const char *key, const std::string &value) {
    for (Field &f : line)
        if (f.key == key && !f.plain) f.value = value;
}

// A ref id's count ("48524B" and ten hex digits, easerver_popn.cpp), 0 for anything else
inline unsigned long long ref_count(const std::string &ref) {
    return ref.size() == 16 && ref.compare(0, 6, "48524B") == 0 ? std::strtoull(ref.c_str() + 6, nullptr, 16) : 0;
}

}  // namespace merging

inline void merge_folder(const std::string &from, const std::string &to) {
    namespace fs = std::filesystem;
    using namespace merging;
    std::error_code ec;
    if (!fs::is_directory(from, ec)) return;
    sys::make_dirs(to);
    auto read = [](const fs::path &p) {
        std::ifstream f(p, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    };
    auto write = [](const fs::path &p, const std::string &d) {   // whole or not at all
        if (sys::write_file(p.string(), d)) return true;
        fprintf(stderr, "e-AMUSEMENT: cannot write %s: the one from before is kept\n", p.string().c_str());
        return false;
    };
    const fs::path src = from, dst = to;

    // The cards: which of the folder's ref ids become which
    std::string have = read(dst / "cards");
    std::vector<Line> ours = lines(have), theirs = lines(read(src / "cards")), gained;
    unsigned long long next = std::max({std::strtoull(read(dst / "refid").c_str(), nullptr, 10),
                                        std::strtoull(read(src / "refid").c_str(), nullptr, 10), 1ULL});
    std::map<std::string, std::string> ref_of;   // the shared folder's cards
    std::set<std::string> used;
    for (const Line &c : ours) {
        ref_of.emplace(get(c, "cardid"), get(c, "refid"));
        used.insert(get(c, "refid"));
        next = std::max(next, ref_count(get(c, "refid")) + 1);
    }
    for (const Line &c : theirs) next = std::max(next, ref_count(get(c, "refid")) + 1);
    std::map<std::string, std::string> becomes;   // the folder's ref id -> the shared folder's
    for (const Line &c : theirs)   // the cards both know, first: their players are the shared folder's
        if (const auto it = ref_of.find(get(c, "cardid")); it != ref_of.end() && !get(c, "refid").empty())
            becomes.emplace(get(c, "refid"), it->second);
    for (Line c : theirs) {
        const std::string card = get(c, "cardid"), ref = get(c, "refid");
        if (card.empty() || ref.empty() || ref_of.count(card)) continue;
        std::string now = ref;
        if (const auto it = becomes.find(ref); it != becomes.end()) {
            now = it->second;
        } else if (used.count(ref)) {
            char made[17];
            snprintf(made, sizeof made, "48524B%010llX", next++);
            now = made;
        }
        becomes.emplace(ref, now);
        used.insert(now);
        ref_of.emplace(card, now);
        set(c, "refid", now);
        gained.push_back(c);
    }
    size_t changed = 0;
    for (const auto &[was, now] : becomes) changed += was != now;
    auto renamed = [&](const std::string &name) {   // NAME-REF as the shared folder has the player
        const size_t dash = name.rfind('-');
        if (dash == std::string::npos) return name;
        const auto it = becomes.find(name.substr(dash + 1));
        return it == becomes.end() ? name : name.substr(0, dash + 1) + it->second;
    };

    bool left = false;
    std::vector<fs::path> files;
    for (const fs::directory_entry &e : fs::directory_iterator(src, ec)) files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (const fs::path &file : files) {
        const std::string name = file.filename().string();
        if (name == "cards") {   // (the folder's own stays in it, as it was)
            if (!have.empty() && have.back() != '\n') have += "\n";
            if (!write(dst / "cards", have + text(gained))) left = true;
        } else if (name == "refid") {   // (below: the count after both folders' cards)
        } else if (name == "numbering" && fs::exists(dst / name, ec)) {
            const unsigned long long a = std::strtoull(read(file).c_str(), nullptr, 10), b = std::strtoull(read(dst / name).c_str(), nullptr, 10);
            write(dst / name, std::to_string(std::max(a, b)) + "\n");
        } else if (name == "users" && fs::exists(dst / name, ec)) {
            std::string list = read(dst / name), add;
            if (!list.empty() && list.back() != '\n') list += "\n";
            std::istringstream in(read(file));
            for (std::string l; std::getline(in, l);)   // the lines it lacks (whole lines)
                if (!l.empty() && ("\n" + list + add).find("\n" + l + "\n") == std::string::npos) add += l + "\n";
            write(dst / name, list + add);
        } else if (const std::string who = name.substr(name.rfind('-') + 1); ref_count(who) && !becomes.count(who) && used.count(who)) {
            // (a player's file under a ref id none of the folder's cards has, and a card has there: nobody's to gain)
            left = true;
            fprintf(stderr, "e-AMUSEMENT: %s in %s is under a number none of its cards has: it stays there\n", name.c_str(), from.c_str());
        } else if (const fs::path there = dst / renamed(name); fs::exists(there, ec)) {
            left = true;
            fprintf(stderr, "e-AMUSEMENT: %s is in both %s and %s; the shared one is kept\n", there.filename().string().c_str(), from.c_str(), to.c_str());
        } else if (changed && name.find("rank") != std::string::npos) {   // a ranking: its entries' players
            std::vector<Line> entries = lines(read(file));
            for (Line &e : entries)
                if (const auto it = becomes.find(get(e, "who")); it != becomes.end()) set(e, "who", it->second);
            if (!write(there, text(entries))) left = true;
        } else {
            fs::rename(file, there, ec);
            if (ec) left = true;
        }
    }
    // the next new card's ref id: after every one of both folders' cards
    if ((fs::exists(src / "refid", ec) || !gained.empty()) && std::strtoull(read(dst / "refid").c_str(), nullptr, 10) != next)
        if (!write(dst / "refid", std::to_string(next) + "\n")) left = true;
    fs::rename(src, from + ".moved", ec);
    printf("e-AMUSEMENT: this game's data moved to %s, which the pop'n games share%s\n", to.c_str(),
           left ? " (what couldn't move stays in eamuse.moved)" : "");
    if (changed)
        printf("e-AMUSEMENT: %zu of its players had another's number there: each keeps its own card, under the shared folder's number\n", changed);
}

}  // namespace hk::ea
