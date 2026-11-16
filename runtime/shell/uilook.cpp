// SPDX-License-Identifier: GPL-3.0-or-later
#include "uilook.h"

#include <cstdlib>
#include <map>
#include <set>
#include <sstream>

namespace hk::ui {

bool light(uint32_t rgb) { return ((rgb >> 16 & 255) * 2 + (rgb >> 8 & 255) * 5 + (rgb & 255)) / 8 > 128; }

bool colour(const std::string &text, uint32_t &rgb) {
    if (text.size() != 7 || text[0] != '#') return false;
    char *end = nullptr;
    const unsigned long v = std::strtoul(text.c_str() + 1, &end, 16);
    if (*end) return false;
    rgb = (uint32_t)v;
    return true;
}

uint32_t mix(uint32_t a, uint32_t b, int t) {
    uint32_t out = 0;
    for (int sh = 0; sh < 24; sh += 8) out |= (uint32_t)(((int)(a >> sh & 255) * (256 - t) + (int)(b >> sh & 255) * t) >> 8) << sh;
    return out;
}

namespace {

// a look's colours by name
std::map<std::string, uint32_t *> look_colours(Look &l) {
    return {{"ground", &l.ground}, {"foot", &l.foot}, {"ink", &l.ink}, {"dim", &l.dim}, {"line", &l.line}, {"bar", &l.bar}, {"point", &l.point}};
}

}  // namespace

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

}  // namespace hk::ui
