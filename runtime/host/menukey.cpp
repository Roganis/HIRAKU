// SPDX-License-Identifier: GPL-3.0-or-later
#include "menukey.h"

#include <atomic>
#include <chrono>
#include <mutex>

namespace hk::host::menukey {

namespace {

// What is watched: a bit each. A hold is all the bits of one of the chords.
struct Watched {
    const char *name;
    unsigned bit;
};
constexpr Watched kWatched[] = {
    {"Keypad 5", 1u << 0},    {"Keypad 7", 1u << 1},    {"Keypad 3", 1u << 2},
    {"P2 Keypad 5", 1u << 3}, {"P2 Keypad 7", 1u << 4}, {"P2 Keypad 3", 1u << 5},
    {"Test", 1u << 6},        {"Service", 1u << 7},     {OWN_KEY, 1u << 8},
};
constexpr unsigned kChords[] = {0x007, 0x038, 0x0c0, 0x100};
constexpr double kShownAfter = 0.3;   // seconds of hold before the box shows

std::atomic<bool> g_on{false};
std::mutex g_lock;
double g_hold = 2.0;
unsigned g_held = 0;
double g_since = -1;   // when a chord was first seen whole (frame()'s time); -1: none is
bool g_fired = false;

bool whole(unsigned held) {
    for (unsigned c : kChords)
        if ((held & c) == c) return true;
    return false;
}

}  // namespace

void set(bool on, double hold_seconds) {
    std::lock_guard<std::mutex> l(g_lock);
    g_hold = hold_seconds > 0.2 ? hold_seconds : 0.2;
    g_held = 0;
    g_since = -1;
    g_fired = false;
    g_on = on;
}

bool on() { return g_on; }

void input(const std::string &name, int value) {
    if (!g_on) return;
    for (const Watched &w : kWatched)
        if (name == w.name) {
            std::lock_guard<std::mutex> l(g_lock);
            if (value) g_held |= w.bit;
            else g_held &= ~w.bit;
            return;
        }
}

bool frame(double now_seconds) {
    if (!g_on) return false;
    std::lock_guard<std::mutex> l(g_lock);
    if (!whole(g_held)) {
        g_since = -1;
        return false;
    }
    if (g_since < 0) g_since = now_seconds;
    if (g_fired || now_seconds - g_since < g_hold) return false;
    g_fired = true;
    return true;
}

double progress(double now_seconds) {
    if (!g_on) return 0;
    std::lock_guard<std::mutex> l(g_lock);
    if (g_since < 0 || now_seconds - g_since < kShownAfter) return 0;
    const double p = (now_seconds - g_since) / g_hold;
    return p > 1 ? 1 : p;
}

double now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace hk::host::menukey
