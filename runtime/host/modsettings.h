// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// A mod's mod.toml as tables (docs/MODS.md), and its [[setting]]s and [[action]]s read from them:
// shared by the mods' loader (host/mods.cpp) and the launcher's settings panel (launcher/
// modsettingsdialog.h), so the two read a mod the same way. Also a setting's value as text (a
// number, or one of its `choices` by name), a `when` condition ("NAME=VALUE"), and a mod's settings
// file (<saves>/mod-settings/<game>/<mod folder>.txt) written again with other lines kept.
// Errors are std::runtime_error, "FILE:LINE: why".
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace hk::modsettings {

// ---- mod.toml: the TOML a mod needs -----------------------------------------------------------
// key = "string" | number (decimal, 0x hex, _ separators) | true | false | ["a", "b"] (on one line),
// # comments, and [[name]] tables. Values keep their text; the readers convert.
struct Table {
    std::string kind;   // "" for the top, else patch / bytes / file / setting...
    int line = 0;
    std::map<std::string, std::string> values;
    std::map<std::string, bool> is_string;
};

inline std::vector<Table> parse(const std::string &text, const std::string &file) {
    std::vector<Table> out(1);
    std::istringstream in(text);
    std::string line;
    int n = 0;
    auto fail = [&](const std::string &why) { throw std::runtime_error(file + ":" + std::to_string(n) + ": " + why); };
    while (std::getline(in, line)) {
        n++;
        // the comment, outside a string
        bool str = false;
        for (size_t i = 0; i < line.size(); i++) {
            if (line[i] == '"' && (i == 0 || line[i - 1] != '\\')) str = !str;
            else if (line[i] == '#' && !str) { line.erase(i); break; }
        }
        size_t a = line.find_first_not_of(" \t\r"), b = line.find_last_not_of(" \t\r");
        if (a == std::string::npos) continue;
        line = line.substr(a, b - a + 1);
        if (line.rfind("[[", 0) == 0) {
            if (line.size() < 4 || line.substr(line.size() - 2) != "]]") fail("a table header is [[name]]");
            Table t;
            t.kind = line.substr(2, line.size() - 4);
            t.line = n;
            out.push_back(t);
            continue;
        }
        if (line[0] == '[')
            fail("only [[patch]], [[text]], [[bytes]], [[file]], [[files]], [[rate]], [[hook]], [[setting]] and [[action]] tables");
        const size_t eq = line.find('=');
        if (eq == std::string::npos) fail("expected key = value");
        std::string key = line.substr(0, eq), val = line.substr(eq + 1);
        key.erase(key.find_last_not_of(" \t") + 1);
        val.erase(0, val.find_first_not_of(" \t"));
        Table &t = out.back();
        if (t.values.count(key)) fail("'" + key + "' given twice");
        if (!val.empty() && val[0] == '"') {
            std::string s;
            size_t i = 1;
            for (; i < val.size() && val[i] != '"'; i++) {
                if (val[i] == '\\' && i + 1 < val.size()) {
                    const char e = val[++i];
                    s += e == 'n' ? '\n' : e == 't' ? '\t' : e;
                } else
                    s += val[i];
            }
            if (i >= val.size()) fail("unterminated string");
            t.values[key] = s;
            t.is_string[key] = true;
        } else {
            if (val.empty()) fail("'" + key + "' has no value");
            t.values[key] = val;
            t.is_string[key] = false;
        }
    }
    return out;
}

// A list of strings on one line (["hook", "settings"]); `example` for the message
inline std::vector<std::string> list(const Table &t, const std::string &key, const std::string &where, const char *example) {
    auto fail = [&](const std::string &why) { throw std::runtime_error(where + ": " + why); };
    std::vector<std::string> out;
    if (t.is_string.at(key)) fail("'" + key + "' is a list: " + example);
    std::string v = t.values.at(key);
    if (v.size() < 2 || v.front() != '[' || v.back() != ']') fail("'" + key + "' is a list on one line: " + example);
    v = v.substr(1, v.size() - 2);
    for (size_t at = 0;;) {
        at = v.find_first_not_of(" \t,", at);
        if (at == std::string::npos) break;
        const size_t close = v[at] == '"' ? v.find('"', at + 1) : std::string::npos;
        if (close == std::string::npos) fail("'" + key + "' is a list of names in quotes: " + example);
        out.push_back(v.substr(at + 1, close - at - 1));
        at = close + 1;
    }
    return out;
}

inline bool same_name(const std::string &a, const std::string &b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    return true;
}

// ---- [[setting]] and [[action]] ---------------------------------------------------------------
struct Setting {
    std::string name, label;            // label: name when none is given
    int64_t min = 0, max = 0, dflt = 0, step = 1;
    std::vector<std::string> choices;   // names for the values 0..n-1 (min and max then implied)
    std::string where;                  // its table's place, for messages
    bool tick() const { return choices.empty() && min == 0 && max == 1; }   // a plain on/off
};
struct Action {
    std::string name, label, binding, setting, what;   // what: "up", "down" or "toggle"
    std::string where;
};

namespace detail {
inline void known(const Table &t, const std::string &where, std::initializer_list<const char *> keys) {
    for (const auto &[k, v] : t.values) {
        bool ok = false;
        for (const char *x : keys) ok |= k == x;
        if (!ok) throw std::runtime_error(where + ": unknown key '" + k + "'");
    }
}
inline std::string str(const Table &t, const std::string &where, const char *k, bool need) {
    auto it = t.values.find(k);
    if (it == t.values.end()) {
        if (need) throw std::runtime_error(where + ": '" + k + "' is missing");
        return {};
    }
    if (!t.is_string.at(k)) throw std::runtime_error(where + ": '" + k + "' is a string (in quotes)");
    return it->second;
}
inline bool whole(const std::string &text, int64_t &v) {
    std::string s;
    for (char c : text)
        if (c != '_') s += c;
    char *end = nullptr;
    errno = 0;
    const long long r = std::strtoll(s.c_str(), &end, 0);
    if (s.empty() || *end || errno) return false;
    v = r;
    return true;
}
inline int64_t inum(const Table &t, const std::string &where, const char *k) {
    auto it = t.values.find(k);
    if (it == t.values.end()) throw std::runtime_error(where + ": '" + k + "' is missing");
    if (t.is_string.at(k)) throw std::runtime_error(where + ": '" + k + "' is a number (no quotes)");
    int64_t v;
    if (!whole(it->second, v)) throw std::runtime_error(where + ": '" + k + "' isn't a whole number");
    return v;
}
inline std::string ident(const Table &t, const std::string &where, const char *k) {   // letters, digits, _ and -
    const std::string s = str(t, where, k, true);
    if (s.empty() || !std::all_of(s.begin(), s.end(), [](char c) { return std::isalnum((unsigned char)c) || c == '_' || c == '-'; }))
        throw std::runtime_error(where + ": '" + k + "' is a name of letters, digits, _ and - (\"" + s + "\")");
    return s;
}
}  // namespace detail

// A value as text: a whole number, or one of the setting's choices by name (any case). false: neither
inline bool value_of(const Setting &s, const std::string &text, int64_t &v) {
    for (size_t i = 0; i < s.choices.size(); i++)
        if (same_name(s.choices[i], text)) {
            v = (int64_t)i;
            return true;
        }
    return detail::whole(text, v);
}

// A value as shown (the box over the game, the log, the launcher): its choice's name, else the
// number (with a + where the setting goes below 0)
inline std::string shown(const Setting &s, int64_t v) {
    if (!s.choices.empty() && v >= 0 && v < (int64_t)s.choices.size()) return s.choices[(size_t)v];
    return (s.min < 0 && v > 0 ? "+" : "") + std::to_string(v);
}

inline Setting setting(const Table &t, const std::string &where) {
    using namespace detail;
    auto fail = [&](const std::string &why) { throw std::runtime_error(where + ": " + why); };
    known(t, where, {"name", "label", "min", "max", "default", "step", "choices"});
    Setting s;
    s.where = where;
    s.name = ident(t, where, "name");
    s.label = str(t, where, "label", false);
    if (s.label.empty()) s.label = s.name;
    if (t.values.count("choices")) {   // named values: 0..n-1
        s.choices = list(t, "choices", where, "[\"Arcade\", \"PS2\"]");
        if (s.choices.size() < 2) fail("'choices' names two values or more");
        for (size_t i = 0; i < s.choices.size(); i++) {
            if (s.choices[i].empty()) fail("'choices' has an empty name");
            for (size_t j = 0; j < i; j++)
                if (same_name(s.choices[i], s.choices[j])) fail("'choices' has \"" + s.choices[i] + "\" twice");
            int64_t n;
            if (whole(s.choices[i], n)) fail("a choice's name isn't a number (\"" + s.choices[i] + "\")");
        }
        if (t.values.count("min") || t.values.count("max")) fail("with 'choices', 'min' and 'max' are theirs (0 to one less than their count)");
        s.min = 0, s.max = (int64_t)s.choices.size() - 1;
        if (!t.values.count("default")) s.dflt = 0;
        else if (t.is_string.at("default")) {
            if (!value_of(s, t.values.at("default"), s.dflt)) fail("'default' is one of 'choices' (\"" + t.values.at("default") + "\" isn't)");
        } else {
            s.dflt = inum(t, where, "default");
        }
    } else {
        s.min = inum(t, where, "min"), s.max = inum(t, where, "max"), s.dflt = inum(t, where, "default");
    }
    s.step = t.values.count("step") ? inum(t, where, "step") : 1;
    if (s.min > s.max) fail("'min' is above 'max'");
    if (s.dflt < s.min || s.dflt > s.max) fail("'default' is from 'min' to 'max'");
    if (s.step <= 0) fail("'step' is 1 or more");
    return s;
}

inline Action action(const Table &t, const std::string &where) {
    using namespace detail;
    known(t, where, {"name", "label", "binding", "setting", "do"});
    Action a;
    a.where = where;
    a.name = ident(t, where, "name");
    a.label = str(t, where, "label", false);
    a.binding = str(t, where, "binding", true);
    a.what = str(t, where, "do", true);
    if (a.what != "up" && a.what != "down" && a.what != "toggle")
        throw std::runtime_error(where + ": 'do' is \"up\", \"down\" (the setting by its step) or \"toggle\" (from its min to its max and back)");
    a.setting = str(t, where, "setting", true);
    return a;
}

// A table's `when`: "NAME=VALUE", NAME one of `settings`, VALUE a number from its min to its max or
// one of its choices. false, with why
inline bool condition(const std::vector<Setting> &settings, const std::string &text, size_t &which, int64_t &value, std::string &why) {
    const size_t eq = text.find('=');
    auto trim = [](std::string s) {
        s.erase(0, s.find_first_not_of(" \t"));
        s.erase(s.find_last_not_of(" \t") + 1);
        return s;
    };
    if (eq == std::string::npos) {
        why = "'when' is \"SETTING=VALUE\" (\"" + text + "\")";
        return false;
    }
    const std::string name = trim(text.substr(0, eq)), v = trim(text.substr(eq + 1));
    for (size_t i = 0; i < settings.size(); i++) {
        if (settings[i].name != name) continue;
        const Setting &s = settings[i];
        if (!value_of(s, v, value) || value < s.min || value > s.max) {
            std::string can;
            if (!s.choices.empty())
                for (const std::string &c : s.choices) can += (can.empty() ? "" : ", ") + c;
            else
                can = std::to_string(s.min) + " to " + std::to_string(s.max);
            why = "'when': " + name + " has no value \"" + v + "\" (" + can + ")";
            return false;
        }
        which = i;
        return true;
    }
    why = "'when': no [[setting]] '" + name + "' in this mod";
    return false;
}

// The mod's settings and actions from its mod.toml's text (the launcher's; the loader reads the
// same tables with the same functions as it goes)
struct Defs {
    std::vector<Setting> settings;
    std::vector<Action> actions;
};
inline Defs read(const std::string &text, const std::string &file) {
    Defs d;
    for (const Table &t : parse(text, file)) {
        const std::string where = file + (t.line ? ":" + std::to_string(t.line) : std::string());
        if (t.kind == "setting") {
            Setting s = setting(t, where);
            for (const Setting &o : d.settings)
                if (o.name == s.name) throw std::runtime_error(where + ": a second setting '" + s.name + "'");
            d.settings.push_back(std::move(s));
        } else if (t.kind == "action") {
            d.actions.push_back(action(t, where));
        }
    }
    for (Action &a : d.actions) {
        bool found = false;
        for (const Setting &s : d.settings) found |= s.name == a.setting;
        if (!found) throw std::runtime_error(a.where + ": no [[setting]] '" + a.setting + "' in this mod");
        if (a.label.empty()) a.label = a.name;
    }
    return d;
}

// ---- the settings file: key=value lines ----------------------------------------------------------
constexpr const char *kFileHead = "# HIRAKU: a mod's settings and its actions' bindings (docs/MODS.md). Edit while the game isn't running\n";

inline std::map<std::string, std::string> values(const std::string &text) {
    std::map<std::string, std::string> kv;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        kv[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return kv;
}

// `text` (the file as it is, or "") with each of `set`'s keys at its new value: in its own line
// where it has one, else added; every other line (comments, actions' bindings, settings the mod
// no longer has) as it was
inline std::string written(const std::string &text, const std::vector<std::pair<std::string, std::string>> &set) {
    std::string out;
    std::vector<bool> done(set.size(), false);
    std::istringstream in(text);
    bool any = false;
    for (std::string line; std::getline(in, line);) {
        any = true;
        std::string plain = line;
        if (!plain.empty() && plain.back() == '\r') plain.pop_back();
        const size_t eq = plain.find('=');
        if (!plain.empty() && plain[0] != '#' && eq != std::string::npos) {
            const std::string key = plain.substr(0, eq);
            bool replaced = false;
            for (size_t i = 0; i < set.size(); i++)
                if (set[i].first == key) {
                    if (!done[i]) out += key + "=" + set[i].second + "\n";
                    done[i] = replaced = true;
                }
            if (replaced) continue;
        }
        out += plain + "\n";
    }
    if (!any) out = kFileHead;
    for (size_t i = 0; i < set.size(); i++)
        if (!done[i]) out += set[i].first + "=" + set[i].second + "\n";
    return out;
}

}  // namespace hk::modsettings
