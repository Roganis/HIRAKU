// SPDX-License-Identifier: GPL-3.0-or-later
#include "menu.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace hk::menu {

namespace {

std::vector<std::string> fields(const std::string &line) {
    std::vector<std::string> f;
    size_t at = 0;
    for (;;) {
        const size_t tab = line.find('\t', at);
        f.push_back(line.substr(at, tab == std::string::npos ? std::string::npos : tab - at));
        if (tab == std::string::npos) return f;
        at = tab + 1;
    }
}

// "#rrggbb" as 0xrrggbb
bool colour(const std::string &text, uint32_t &rgb) {
    if (text.size() != 7 || text[0] != '#') return false;
    char *end = nullptr;
    const unsigned long v = std::strtoul(text.c_str() + 1, &end, 16);
    if (*end) return false;
    rgb = (uint32_t)v;
    return true;
}

}  // namespace

bool light(uint32_t rgb) { return ((rgb >> 16 & 255) * 2 + (rgb >> 8 & 255) * 5 + (rgb & 255)) / 8 > 128; }

bool List::parse(const std::string &text, std::string &error) {
    shelves.clear();
    current.clear();
    std::istringstream in(text);
    std::string line;
    int n = 0;
    std::map<std::string, uint32_t> colours;   // the shelves', by name (shelf lines)
    while (std::getline(in, line)) {
        n++;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const std::vector<std::string> f = fields(line);
        if (n == 1) {
            if (f[0] != "hiraku-menu-list 1") {
                error = "not a game menu list (its first line isn't \"hiraku-menu-list 1\")";
                return false;
            }
        } else if (f[0] == "current" && f.size() >= 2) {
            current = f[1];
        } else if (f[0] == "shelf" && f.size() >= 3) {
            uint32_t rgb;
            if (!colour(f[2], rgb)) {
                error = "line " + std::to_string(n) + ": a shelf's colour isn't #rrggbb";
                return false;
            }
            colours[f[1]] = rgb;
        } else if (f[0] == "game" && f.size() >= 4 && !f[1].empty()) {
            const std::string &shelf = f[2];
            if (shelves.empty() || shelves.back().name != shelf) shelves.push_back({shelf, {}});
            shelves.back().games.push_back({f[1], f[3], f.size() > 4 ? f[4] : std::string(), f.size() > 5 ? f[5] : std::string()});
        } else {
            error = "line " + std::to_string(n) + ": not \"current ID\", \"game ID SHELF TITLE DETAIL\" or \"shelf NAME #rrggbb\", tabs between";
            return false;
        }
    }
    if (n == 0) {
        error = "empty";
        return false;
    }
    for (Shelf &sh : shelves)
        if (const auto it = colours.find(sh.name); it != colours.end()) sh.colour = it->second, sh.has_colour = true;
    return true;
}

// a look's colours by name
std::map<std::string, uint32_t *> look_colours(Look &l) {
    return {{"ground", &l.ground}, {"foot", &l.foot}, {"ink", &l.ink}, {"dim", &l.dim}, {"line", &l.line}, {"bar", &l.bar}, {"point", &l.point}};
}

bool Look::parse(const std::string &text, std::string &error) {
    Look given;
    std::map<std::string, uint32_t *> names = look_colours(given);
    std::set<std::string> seen;
    std::istringstream in(text);
    std::string item;
    while (std::getline(in, item, ',')) {
        if (item.empty()) continue;
        const size_t eq = item.find('=');
        const std::string name = item.substr(0, eq), value = eq == std::string::npos ? "" : item.substr(eq + 1);
        if (name == "stripe") {
            std::istringstream six(value);
            std::string c;
            given.stripe.clear();
            while (std::getline(six, c, ':')) {
                uint32_t rgb;
                if (!colour(c, rgb)) {
                    error = "stripe: \"" + c + "\" isn't #rrggbb";
                    return false;
                }
                given.stripe.push_back(rgb);
            }
        } else if (name == "round") {
            char *end = nullptr;
            const long r = std::strtol(value.c_str(), &end, 10);
            if (value.empty() || *end || r < 0 || r > 40) {
                error = "round: not a number of pixels (0-40)";
                return false;
            }
            given.round = (int)r;
        } else if (const auto it = names.find(name); it != names.end()) {
            if (!colour(value, *it->second)) {
                error = name + ": \"" + value + "\" isn't #rrggbb";
                return false;
            }
            seen.insert(name);
        } else {
            error = "\"" + name + "\" isn't one of ground, foot, ink, dim, line, bar, point, stripe, round";
            return false;
        }
    }
    // what wasn't given: a light look's on a light ground (the dark one's are the defaults); the
    // foot, the ground (no fade)
    if (seen.count("ground") && light(given.ground)) {
        Look day;
        day.ground = day.foot = 0xffffff, day.ink = 0x232629, day.dim = 0x70777e, day.line = 0xd3d4d5, day.bar = day.point = 0x3daee9;
        for (const auto &[name, at] : look_colours(day))
            if (!seen.count(name)) *names[name] = *at;
    }
    if (!seen.count("foot") && seen.count("ground")) given.foot = given.ground;
    *this = given;
    return true;
}

bool List::load(const std::string &path, std::string &error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot read " + path;
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    if (!parse(ss.str(), error)) {
        error = path + ": " + error;
        return false;
    }
    return true;
}

size_t List::games() const {
    size_t n = 0;
    for (const Shelf &s : shelves) n += s.games.size();
    return n;
}

void State::set(const List &l) {
    list_ = l;
    rows_.assign(list_.shelves.size(), 0);
    shelf_ = 0;
    chosen_ = false;
    dock_at_ = UINT64_MAX;
    for (size_t s = 0; s < list_.shelves.size(); s++)
        for (size_t g = 0; g < list_.shelves[s].games.size(); g++)
            if (list_.shelves[s].games[g].id == list_.current) shelf_ = (int)s, rows_[s] = (int)g;
    shown_row_ = row();
}

const Game *State::selected() const {
    if (list_.shelves.empty()) return nullptr;
    const Shelf &s = list_.shelves[(size_t)shelf_];
    return s.games.empty() ? nullptr : &s.games[(size_t)rows_[(size_t)shelf_]];
}

bool State::act(Act a, uint64_t frame) {
    if (chosen_ || !selected() || a == Act::none) return false;
    dock_at_ = std::min(dock_at_, frame);   // (the logo makes way: logo_at)
    const int shelves = (int)list_.shelves.size();
    const int n = (int)list_.shelves[(size_t)shelf_].games.size();
    int &r = rows_[(size_t)shelf_];
    moved_at_ = frame;
    notice_.clear();   // read, by now
    switch (a) {
    case Act::prev:
    case Act::next: {
        if (n < 2) return false;
        const int to = (r + (a == Act::next ? 1 : n - 1)) % n;
        if (std::abs(to - r) > 1) shown_row_ = to + (a == Act::next ? -1 : 1);   // round the end: it comes in from the edge
        r = to;
        return true;
    }
    case Act::shelf_prev:
    case Act::shelf_next:
        if (shelves < 2) return false;
        shelf_ = (shelf_ + (a == Act::shelf_next ? 1 : shelves - 1)) % shelves;
        shelf_dir_ = a == Act::shelf_next ? 1 : -1;
        shelf_at_ = frame;
        shown_row_ = row();
        return true;
    case Act::start:
        chosen_ = true;
        chosen_at_ = frame;
        return true;
    default:
        return false;
    }
}

void State::animate() {
    const double to = row(), d = to - shown_row_;
    shown_row_ = std::fabs(d) < 0.02 ? to : shown_row_ + d * 0.35;
}

}  // namespace hk::menu
