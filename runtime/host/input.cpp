// SPDX-License-Identifier: GPL-3.0-or-later
#include "input.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <vector>

#include "../shell/core.h"
#include "../shell/cabinet.h"
#include "warp.h"

namespace hk::host {

namespace {

enum class Src { key, button, axis, axis_neg, axis_pos, hat };
struct Binding {
    Src src;
    int joy = 0, index = 0, hat_dir = 0;
    SDL_Keycode key = 0;
    std::string dev = {};  // the controller by name (joy[<dev>]), or "": by number (joy<N>)
    double scale = 1.0;    // an analog axis: its turntable counts times this (negative: reversed)
};

std::map<std::string, std::vector<Binding>> g_bind;   // input name -> bindings
std::map<std::string, int> g_held;                    // input name -> number of bound sources down
std::vector<SDL_Joystick *> g_joys;
std::vector<std::string> g_joy_dev;                   // each one's name as bindings give it ("" once unplugged)
int16_t g_axis_last[16][16];
bool g_axis_seen[16][16];
double g_tt_scale = 1024.0 / 65536.0 * 4;             // turntable counts per axis unit (one wrap = 4 x 256)

bool parse_binding(const std::string &b, Binding &out) {
    if (b.rfind("key:", 0) == 0) {
        // ',' separates bindings, so punctuation keys go by name
        static const std::map<std::string, std::string> alias = {
            {"Comma", ","}, {"Period", "."}, {"Slash", "/"}, {"Semicolon", ";"}, {"Minus", "-"}, {"Equals", "="}};
        std::string k = b.substr(4);
        if (auto a = alias.find(k); a != alias.end()) k = a->second;
        out.src = Src::key;
        out.key = SDL_GetKeyFromName(k.c_str());
        return out.key != SDLK_UNKNOWN;
    }
    // the controller: joy<N> by number (the order they were found), joy[<name>] by name
    int joy = 0, idx;
    std::string dev, r;
    if (b.rfind("joy[", 0) == 0) {
        size_t e = b.find("]:");
        if (e == std::string::npos) return false;
        dev = b.substr(4, e - 4);
        r = b.substr(e + 2);
    } else {
        size_t c = b.find(':');
        if (sscanf(b.c_str(), "joy%d:", &joy) != 1 || c == std::string::npos) return false;
        r = b.substr(c + 1);
    }
    char rest[32] = {};
    if (sscanf(r.c_str(), "button%d", &idx) == 1 && r.rfind("button", 0) == 0) {
        out = {Src::button, joy, idx};
    } else if (sscanf(r.c_str(), "hat%d:%31s", &idx, rest) == 2) {
        std::string d = rest;
        int dir = d == "up" ? SDL_HAT_UP : d == "down" ? SDL_HAT_DOWN : d == "left" ? SDL_HAT_LEFT : d == "right" ? SDL_HAT_RIGHT : 0;
        out = {Src::hat, joy, idx, dir};
        if (!dir) return false;
    } else if (sscanf(r.c_str(), "axis%d%31s", &idx, rest) >= 1) {
        std::string x = rest;
        out = {x == "-" ? Src::axis_neg : x == "+" ? Src::axis_pos : Src::axis, joy, idx};
        if (!x.empty() && x[0] == '*') {   // axis<K>*<scale>: a turntable's speed, negative to reverse it
            char *end = nullptr;
            out.scale = strtod(x.c_str() + 1, &end);
            if (*end || out.scale == 0) return false;
        } else if (!x.empty() && x != "-" && x != "+") {
            return false;
        }
    } else {
        return false;
    }
    out.dev = dev;
    return true;
}

void load_text(const std::string &text, const char *origin) {
    for (auto &en : cab::parse_list_file(text)) {
        std::vector<Binding> list;
        for (auto &b : en.items) {
            Binding bd{};
            if (parse_binding(b, bd)) list.push_back(bd);
            else fprintf(stderr, "%s:%d: cannot parse binding '%s'\n", origin, en.line, b.c_str());
        }
        g_bind[en.name] = list;   // a later definition replaces the default
    }
}

void press(const std::string &name, bool down) {
    int &n = g_held[name];
    int before = n;
    n = std::max(0, n + (down ? 1 : -1));
    if ((before > 0) == (n > 0)) return;
    if (n > 0) warp::pressed();   // a player's: the fast start-up ends (host/warp.h)
    if (name.find("Turntable") != std::string::npos) return;   // handled in input_update
    if (name.find(" Wheel Up") != std::string::npos || name.find(" Wheel Down") != std::string::npos) return;   // likewise
    core().set_input(name, n > 0 ? 1 : 0);
}

// An analog control that takes a position (an axis's, from one end to the other): its highest
// value, as the core takes it; -1: not one (the turntables turn, handled in input_update)
int analog_max(const std::string &name) {
    if (name.size() > 6 && name.compare(name.size() - 6, 6, " Wheel") == 0) return 255;   // Keyboardmania's wheels
    if (name.find("Effect Knob") != std::string::npos) return 3;                            // GuitarFreaks
    if (name.rfind("Volume ", 0) == 0) return 15;                                            // IIDX's effector sliders
    return -1;
}
std::map<std::string, double> g_analog;   // each such control's position (keys move it too)

// Each axis's state as a button (axisN+ / axisN-): an axis whose first report is already past a
// threshold rests there (a trigger, or an adapter's unused axis at one end), so it doesn't count
// as pressed that way until it has come back. Some USB controllers report an axis only once
// something changes: until then SDL gives 0, and the first key pressed brings the resting values.
bool g_axis_known[16][16], g_rest_neg[16][16], g_rest_pos[16][16];
const bool g_input_log = [] {
    const char *e = getenv("HK_INPUT_LOG");
    return e && *e && *e != '0';
}();

int joy_index(SDL_JoystickID id) {
    for (size_t k = 0; k < g_joys.size(); k++)
        if (SDL_GetJoystickID(g_joys[k]) == id) return (int)k;
    return -1;
}

// The joystick a binding means now, or -1 (a named one that isn't plugged in)
int bound_joy(const Binding &b) {
    if (b.dev.empty()) return b.joy;
    for (size_t k = 0; k < g_joy_dev.size(); k++)
        if (g_joy_dev[k] == b.dev) return (int)k;
    return -1;
}

bool from(const Binding &b, SDL_JoystickID id) {
    int j = joy_index(id);
    return j >= 0 && j == bound_joy(b);
}

// Slots stay as they were found (joy<N> keeps its meaning); an unplugged one loses its name, so a
// controller plugged back in gets it again rather than "<name>@2"
void add_joystick(SDL_Joystick *j) {
    std::string name = cab::pad_name(SDL_GetJoystickName(j) ? SDL_GetJoystickName(j) : ""), dev = name;
    int same = 1;
    for (auto &d : g_joy_dev)
        if (d == name || d.rfind(name + "@", 0) == 0) same++;
    if (same > 1) dev += "@" + std::to_string(same);
    g_joys.push_back(j);
    g_joy_dev.push_back(dev);
    fprintf(stderr, "input: joy%zu = joy[%s] (%d buttons, %d axes)\n", g_joys.size() - 1, dev.c_str(), SDL_GetNumJoystickButtons(j),
            SDL_GetNumJoystickAxes(j));
}

}  // namespace

void input_init(const std::string &file) {
    SDL_InitSubSystem(SDL_INIT_JOYSTICK);
    load_text(cab::default_bindings(core().family()), "defaults");
    if (!file.empty()) {
        std::ifstream in(file);
        if (in) load_text(std::string((std::istreambuf_iterator<char>(in)), {}), file.c_str());
        else fprintf(stderr, "input: cannot read %s (using defaults)\n", file.c_str());
    }
    int n = 0;
    SDL_JoystickID *ids = SDL_GetJoysticks(&n);
    for (int k = 0; k < n; k++)
        if (SDL_Joystick *j = SDL_OpenJoystick(ids[k])) add_joystick(j);
    SDL_free(ids);
}

void input_joystick_added(unsigned int id) {
    for (auto *j : g_joys)
        if (SDL_GetJoystickID(j) == id) return;   // already open (enumerated at start)
    if (SDL_Joystick *j = SDL_OpenJoystick(id)) add_joystick(j);
}

void input_joystick_removed(unsigned int id) {
    int k = joy_index(id);
    if (k < 0 || g_joy_dev[k].empty()) return;
    fprintf(stderr, "input: joy%d = joy[%s] unplugged\n", k, g_joy_dev[k].c_str());
    g_joy_dev[k].clear();
}

namespace {
thread_local std::chrono::steady_clock::time_point g_event_time{};   // input_event's, while it runs
}

std::chrono::steady_clock::time_point input_time() {
    return g_event_time != std::chrono::steady_clock::time_point{} ? g_event_time : std::chrono::steady_clock::now();
}

void input_event(const SDL_Event &e) {
    // SDL's event times are its own clock's (SDL_GetTicksNS), taken from the system's input event
    // where it has one: on the steady clock, as long ago as that. One from the future, or older than a
    // quarter of a second (a clock that isn't the same), is taken as now
    const auto now = std::chrono::steady_clock::now();
    const Uint64 ticks = SDL_GetTicksNS();
    const Uint64 ago = e.common.timestamp && e.common.timestamp <= ticks ? ticks - e.common.timestamp : 0;
    g_event_time = ago < 250000000ull ? now - std::chrono::nanoseconds(ago) : now;
    struct Clear {
        ~Clear() { g_event_time = {}; }
    } clear;
    if (e.type == SDL_EVENT_JOYSTICK_AXIS_MOTION) {
        const int j = joy_index(e.jaxis.which), a = e.jaxis.axis;
        if (j >= 0 && j < 16 && a < 16) {
            const int v = e.jaxis.value;
            if (!g_axis_known[j][a]) {
                g_axis_known[j][a] = true;
                g_rest_neg[j][a] = v < -16384, g_rest_pos[j][a] = v > 16384;
                if (g_rest_neg[j][a] || g_rest_pos[j][a])
                    fprintf(stderr, "input: joy%d axis %d first reads %d: it rests there (not pressed as axis%d%c)\n", j, a, v, a,
                            v < 0 ? '-' : '+');
            } else {
                if (v >= -16384) g_rest_neg[j][a] = false;
                if (v <= 16384) g_rest_pos[j][a] = false;
            }
        }
    }
    if (g_input_log && (e.type == SDL_EVENT_JOYSTICK_AXIS_MOTION || e.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN ||
                        e.type == SDL_EVENT_JOYSTICK_BUTTON_UP || e.type == SDL_EVENT_JOYSTICK_HAT_MOTION)) {
        if (e.type == SDL_EVENT_JOYSTICK_AXIS_MOTION)
            fprintf(stderr, "input log: joy%d axis%d %d\n", joy_index(e.jaxis.which), e.jaxis.axis, e.jaxis.value);
        else if (e.type == SDL_EVENT_JOYSTICK_HAT_MOTION)
            fprintf(stderr, "input log: joy%d hat%d 0x%x\n", joy_index(e.jhat.which), e.jhat.hat, e.jhat.value);
        else
            fprintf(stderr, "input log: joy%d button%d %s\n", joy_index(e.jbutton.which), e.jbutton.button,
                    e.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN ? "down" : "up");
    }
    for (auto &[name, list] : g_bind)
        for (auto &b : list) {
            switch (e.type) {
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
                if (b.src == Src::key && !e.key.repeat && e.key.key == b.key) press(name, e.type == SDL_EVENT_KEY_DOWN);
                break;
            case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
            case SDL_EVENT_JOYSTICK_BUTTON_UP:
                if (b.src == Src::button && from(b, e.jbutton.which) && e.jbutton.button == b.index)
                    press(name, e.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN);
                break;
            case SDL_EVENT_JOYSTICK_HAT_MOTION:
                if (b.src == Src::hat && from(b, e.jhat.which) && e.jhat.hat == b.index) {
                    bool down = (e.jhat.value & b.hat_dir) != 0;
                    if (down != (g_held[name] > 0)) press(name, down);
                }
                break;
            case SDL_EVENT_JOYSTICK_AXIS_MOTION:
                if ((b.src == Src::axis_neg || b.src == Src::axis_pos) && from(b, e.jaxis.which) && e.jaxis.axis == b.index) {
                    const int j = joy_index(e.jaxis.which), a = e.jaxis.axis;
                    const bool rests = j >= 0 && j < 16 && a < 16 && (b.src == Src::axis_neg ? g_rest_neg[j][a] : g_rest_pos[j][a]);
                    bool down = !rests && (b.src == Src::axis_neg ? e.jaxis.value < -16384 : e.jaxis.value > 16384);
                    if (down != (g_held[name] > 0)) press(name, down);
                }
                // an analog control's position: the axis from one end to the other (reversed with a
                // negative speed)
                if (b.src == Src::axis && from(b, e.jaxis.which) && e.jaxis.axis == b.index) {
                    if (const int max = analog_max(name); max > 0) {
                        int v = (int)(((long)e.jaxis.value + 32768) * (max + 1) / 65536);
                        v = std::clamp(b.scale < 0 ? max - v : v, 0, max);
                        if (g_analog.count(name) == 0 || (int)std::lround(g_analog[name]) != v) core().set_input(name, v);
                        g_analog[name] = v;
                    }
                }
                break;
            default: break;
            }
        }
}

int input_binding_event(const std::string &binding, const SDL_Event &e) {
    Binding b;
    if (!parse_binding(binding, b)) return 0;
    switch (e.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        return b.src == Src::key && e.key.key == b.key ? (e.type == SDL_EVENT_KEY_DOWN ? 1 : -1) : 0;
    case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
    case SDL_EVENT_JOYSTICK_BUTTON_UP:
        if (b.src == Src::button && from(b, e.jbutton.which) && e.jbutton.button == b.index)
            return e.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN ? 1 : -1;
        return 0;
    case SDL_EVENT_JOYSTICK_HAT_MOTION:
        if (b.src == Src::hat && from(b, e.jhat.which) && e.jhat.hat == b.index) return (e.jhat.value & b.hat_dir) ? 1 : -1;
        return 0;
    case SDL_EVENT_JOYSTICK_AXIS_MOTION:
        if ((b.src == Src::axis_neg || b.src == Src::axis_pos) && from(b, e.jaxis.which) && e.jaxis.axis == b.index)
            return (b.src == Src::axis_neg ? e.jaxis.value < -16384 : e.jaxis.value > 16384) ? 1 : -1;
        return 0;
    default: return 0;
    }
}

void input_update(double ms) {
    // Keyboardmania's wheels on keys or buttons: up and down move it (about MAME's KEYDELTA, 10 a
    // 60 Hz frame); it stays where it's left, as a modulation wheel does
    for (const char *w : {"P1 Wheel", "P2 Wheel"}) {
        const std::string n = w;
        const int dir = (g_held[n + " Up"] > 0 ? 1 : 0) - (g_held[n + " Down"] > 0 ? 1 : 0);
        if (!dir) continue;
        double &pos = g_analog.try_emplace(n, 128.0).first->second;
        const int before = (int)std::lround(pos);
        pos = std::clamp(pos + dir * ms * 0.6, 0.0, 255.0);
        if ((int)std::lround(pos) != before) core().set_input(n, (int)std::lround(pos));
    }
    for (int p = 0; p < 2; p++) {
        std::string base = p == 0 ? "P1 Turntable" : "P2 Turntable";
        // keys: about MAME's KEYDELTA (15 counts per 60 Hz frame)
        static double frac[2] = {0, 0};
        double v = (g_held[base + " +"] > 0 ? 1 : 0) - (g_held[base + " -"] > 0 ? 1 : 0);
        frac[p] += v * ms * 0.9;
        int whole = (int)frac[p];
        frac[p] -= whole;
        if (whole) core().turntable_add(p, whole);
        // analog axis: follow its motion (wrapping, as turntable encoders do)
        for (auto &b : g_bind[base]) {
            int j = bound_joy(b);
            if (b.src != Src::axis || j < 0 || j >= (int)g_joys.size() || j >= 16 || b.index >= 16) continue;
            int16_t a = SDL_GetJoystickAxis(g_joys[j], b.index);
            if (g_axis_seen[j][b.index]) {
                int d = (int)a - g_axis_last[j][b.index];
                if (d > 32768) d -= 65536;
                if (d < -32768) d += 65536;
                if (d) core().turntable_add(p, (int)std::lround(d * g_tt_scale * b.scale));
            }
            g_axis_last[j][b.index] = a;
            g_axis_seen[j][b.index] = true;
        }
    }
}

std::string input_describe() {
    std::string s;
    for (auto &[name, list] : g_bind) {
        s += "  " + name + " =";
        for (auto &b : list) {
            char buf[64];
            const std::string pad = b.dev.empty() ? "joy" + std::to_string(b.joy) : "joy[" + b.dev + "]";
            const char *P = pad.c_str();
            switch (b.src) {
            case Src::key: snprintf(buf, sizeof buf, " key:%s", SDL_GetKeyName(b.key)); break;
            case Src::button: snprintf(buf, sizeof buf, " %s:button%d", P, b.index); break;
            case Src::axis:
                if (b.scale != 1.0) snprintf(buf, sizeof buf, " %s:axis%d*%g", P, b.index, b.scale);
                else snprintf(buf, sizeof buf, " %s:axis%d", P, b.index);
                break;
            case Src::axis_neg: snprintf(buf, sizeof buf, " %s:axis%d-", P, b.index); break;
            case Src::axis_pos: snprintf(buf, sizeof buf, " %s:axis%d+", P, b.index); break;
            case Src::hat:
                snprintf(buf, sizeof buf, " %s:hat%d:%s", P, b.index,
                         b.hat_dir == SDL_HAT_UP ? "up" : b.hat_dir == SDL_HAT_DOWN ? "down" : b.hat_dir == SDL_HAT_LEFT ? "left" : "right");
                break;
            }
            s += buf;
        }
        s += "\n";
    }
    return s;
}

}  // namespace hk::host
