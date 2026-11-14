// SPDX-License-Identifier: GPL-3.0-or-later
#include "menu.h"

#include <cmath>
#include <fstream>
#include <sstream>

namespace hk::menu {

namespace {

// A field as the 8x8 font can show it: ASCII, a '?' for each character that isn't
std::string ascii(const std::string &s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        const unsigned char c = (unsigned char)s[i];
        if (c >= 32 && c < 127) out += (char)c;
        else if (c >= 0xc0 || c < 32) out += '?';   // (a UTF-8 character's first byte: its others add nothing)
    }
    return out;
}

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

}  // namespace

bool List::parse(const std::string &text, std::string &error) {
    shelves.clear();
    current.clear();
    std::istringstream in(text);
    std::string line;
    int n = 0;
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
        } else if (f[0] == "game" && f.size() >= 4 && !f[1].empty()) {
            const std::string shelf = ascii(f[2]);
            if (shelves.empty() || shelves.back().name != shelf) shelves.push_back({shelf, {}});
            shelves.back().games.push_back({f[1], ascii(f[3]), f.size() > 4 ? ascii(f[4]) : std::string(), f.size() > 5 ? f[5] : std::string()});
        } else {
            error = "line " + std::to_string(n) + ": not \"current ID\" or \"game ID SHELF TITLE DETAIL\", tabs between";
            return false;
        }
    }
    if (n == 0) {
        error = "empty";
        return false;
    }
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
    const int shelves = (int)list_.shelves.size();
    const int n = (int)list_.shelves[(size_t)shelf_].games.size();
    int &r = rows_[(size_t)shelf_];
    moved_at_ = frame;
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
