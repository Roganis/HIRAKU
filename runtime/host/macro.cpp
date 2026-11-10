// SPDX-License-Identifier: GPL-3.0-or-later
#include "macro.h"

#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>

#include "../shell/core.h"

namespace hk::macro {

namespace {

struct Step {
    std::string field;
    int value;        // 1 down, 0 up, or an analog value
    uint64_t offset;  // frames from the macro's start
};

struct Macro {
    Info info;
    std::vector<Step> steps;
    uint64_t length = 0;
};

std::vector<Macro> g_macros;
std::vector<Info> g_infos;
std::map<std::string, std::string> g_buttons;   // "3" -> "P1 Button 3"
std::multimap<uint64_t, size_t> g_scheduled;    // frame -> macro (power-on codes)
int g_running = -1;
bool g_pending = false;
uint64_t g_start = 0;
std::function<void(const std::string &)> g_cb;

std::vector<std::string> tokens(const std::string &line) {
    std::vector<std::string> t;
    size_t i = 0;
    while (i < line.size()) {
        char c = line[i];
        if (c == '#') break;
        if (isspace((unsigned char)c)) { i++; continue; }
        if (c == '"') {
            size_t j = line.find('"', i + 1);
            if (j == std::string::npos) throw std::runtime_error("unterminated quote");
            t.push_back(line.substr(i + 1, j - i - 1));
            i = j + 1;
        } else {
            size_t j = line.find_first_of(" \t#", i);
            if (j == std::string::npos) j = line.size();
            t.push_back(line.substr(i, j - i));
            i = j;
        }
    }
    return t;
}

std::string button(const std::string &n) {
    auto it = g_buttons.find(n);
    return it != g_buttons.end() ? it->second : "P1 Button " + n;
}

}  // namespace

namespace {

// Macros from `in` (a macros.txt's text) added to `out`; false on a parse error (printed, as from `path`)
bool parse(std::istream &in, const std::string &path, std::vector<Macro> &out) {
    Macro *m = nullptr;
    uint64_t cursor = 0;
    int tap = 8, gap = 16;
    int n = 0;
    auto press = [&](const std::string &field, int len) {
        m->steps.push_back({field, 1, cursor});
        m->steps.push_back({field, 0, cursor + (uint64_t)len});
        cursor += (uint64_t)std::max(gap, len + 1);
    };
    for (std::string line; std::getline(in, line);) {
        n++;
        try {
            auto t = tokens(line);
            if (t.empty()) continue;
            if (!m) {
                if (t[0] == "button") g_buttons[t.at(1)] = t.at(2);
                else if (t[0] == "macro") {
                    out.push_back({});
                    m = &out.back();
                    m->info.name = t.at(1);
                    cursor = 0;
                    tap = 8;
                    gap = 16;
                } else throw std::runtime_error("expected 'button' or 'macro'");
                continue;
            }
            if (t[0] == "end") {
                m->length = cursor + 1;
                m = nullptr;
            } else if (t[0] == "text") m->info.text = t.at(1);
            else if (t[0] == "at") m->info.at = std::stoll(t.at(1));
            else if (t[0] == "tap") {
                tap = std::stoi(t.at(1));
                if (t.size() > 3 && t[2] == "gap") gap = std::stoi(t[3]);
            } else if (t[0] == "keys") {
                std::vector<std::string> keys;
                if (t.size() == 2 && t[1].find_first_not_of("0123456789") == std::string::npos)
                    for (char c : t[1]) keys.push_back(std::string(1, c));   // one digit each
                else keys.assign(t.begin() + 1, t.end());
                for (auto &k : keys) press(button(k), tap);
            } else if (t[0] == "press") press(t.at(1), t.size() > 2 ? std::stoi(t[2]) : tap);
            else if (t[0] == "hold") m->steps.push_back({t.at(1), 1, cursor}), cursor += 2;
            else if (t[0] == "release") m->steps.push_back({t.at(1), 0, cursor}), cursor += 2;
            else if (t[0] == "set") m->steps.push_back({t.at(1), std::stoi(t.at(2)), cursor}), cursor += 1;
            else if (t[0] == "wait") cursor += (uint64_t)std::stoll(t.at(1));
            else throw std::runtime_error("unknown step '" + t[0] + "'");
        } catch (const std::exception &e) {
            fprintf(stderr, "%s:%d: %s\n", path.c_str(), n, e.what());
            out.clear();
            return false;
        }
    }
    if (m) {
        fprintf(stderr, "%s: macro \"%s\" has no 'end'\n", path.c_str(), m->info.name.c_str());
        out.clear();
        return false;
    }
    return true;
}

std::mutex g_posted_lock;
std::vector<Macro> g_posted;   // post(): run from the next frame

}  // namespace

bool load(const std::string &path) {
    std::ifstream in(path);
    if (!in) return true;   // a game without codes
    if (!parse(in, path, g_macros)) return false;
    for (auto &mm : g_macros) g_infos.push_back(mm.info);
    return true;
}

bool post(const std::string &name, const std::string &steps) {
    std::istringstream in("macro \"" + name + "\"\n" + steps + "\nend\n");
    std::vector<Macro> m;
    if (!parse(in, name, m) || m.size() != 1) return false;
    std::lock_guard<std::mutex> hold(g_posted_lock);
    g_posted.push_back(std::move(m[0]));
    return true;
}

const std::vector<Info> &list() { return g_infos; }

bool trigger(size_t index) {
    if (index >= g_macros.size() || g_running >= 0) return false;
    g_running = (int)index;
    g_pending = true;
    return true;
}

bool schedule(const std::string &spec) {
    std::string name = spec;
    int64_t frame = -1;
    if (auto at = spec.rfind('@'); at != std::string::npos) {
        name = spec.substr(0, at);
        try {
            frame = std::stoll(spec.substr(at + 1));
        } catch (const std::exception &) {
            fprintf(stderr, "macro %s: after @, the frame to play it at\n", spec.c_str());
            return false;
        }
    }
    for (size_t k = 0; k < g_macros.size(); k++)
        if (g_macros[k].info.name == name) {
            if (frame < 0) frame = g_macros[k].info.at;
            if (frame < 0) {
                fprintf(stderr, "macro \"%s\" has no 'at' frame (give NAME@FRAME)\n", name.c_str());
                return false;
            }
            g_scheduled.emplace((uint64_t)frame, k);
            return true;
        }
    fprintf(stderr, "no macro \"%s\" for this game\n", name.c_str());
    return false;
}

void frame(uint64_t f) {
    {
        std::lock_guard<std::mutex> hold(g_posted_lock);
        for (Macro &m : g_posted) {
            g_macros.push_back(std::move(m));
            g_scheduled.emplace(f, g_macros.size() - 1);
        }
        g_posted.clear();
    }
    if (g_running < 0)   // a macro due while another ran starts after it
        if (auto it = g_scheduled.begin(); it != g_scheduled.end() && it->first <= f) {
            g_running = (int)it->second;
            g_pending = true;
            g_scheduled.erase(it);
        }
    if (g_running < 0) return;
    const Macro &m = g_macros[(size_t)g_running];
    if (g_pending) {
        g_pending = false;
        g_start = f;
        fprintf(stderr, "macro: %s\n", m.info.name.c_str());
        if (g_cb) g_cb(m.info.name);
    }
    uint64_t t = f - g_start;
    for (const Step &s : m.steps)
        if (s.offset == t && !core().set_input(s.field, s.value))
            fprintf(stderr, "macro \"%s\": unknown input '%s'\n", m.info.name.c_str(), s.field.c_str());
    if (t >= m.length) {
        g_running = -1;
        if (g_cb) g_cb("");
    }
}

void on_change(std::function<void(const std::string &)> cb) { g_cb = std::move(cb); }

}  // namespace hk::macro
