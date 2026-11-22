// SPDX-License-Identifier: GPL-3.0-or-later
#include "menukey.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

#include <cwchar>
#endif

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
constexpr int kBits = 9;
constexpr unsigned kChords[] = {0x007, 0x038, 0x0c0, 0x100};
constexpr double kShownAfter = 0.3;   // seconds of hold before the box shows

std::atomic<bool> g_on{false}, g_touched{false};
std::mutex g_lock;
double g_hold = 2.0;
unsigned g_held = 0;
int g_src[kBits] = {-1, -1, -1, -1, -1, -1, -1, -1, -1};   // the reader each held bit came from, -1: none
double g_seen[2] = {-1e300, -1e300};                        // each reader's last reading
bool g_broken = false;   // a key of a whole chord let go since the last frame
double g_since = -1;     // when a chord was first seen whole (frame()'s time); -1: none is
bool g_fired = false;
// for the report
unsigned g_begun = 0, g_quiet = 0;
std::vector<double> g_let_go;   // the holds let go: how long each had lasted

bool whole(unsigned held) {
    for (unsigned c : kChords)
        if ((held & c) == c) return true;
    return false;
}

// The held bits a reader gave whose reader has had no fresh reading for STALE_SECONDS
unsigned stale(double now_seconds) {
    unsigned m = 0;
    for (int i = 0; i < kBits; i++)
        if ((g_held >> i & 1) && g_src[i] >= 0 && now_seconds - g_seen[g_src[i]] > STALE_SECONDS) m |= 1u << i;
    return m;
}

void drop(double now_seconds, bool quiet) {
    if (g_since >= 0 && !g_fired) {
        g_let_go.push_back(now_seconds - g_since);
        if (quiet) g_quiet++;
    }
    g_since = -1;
}

}  // namespace

void set(bool on, double hold_seconds) {
    std::lock_guard<std::mutex> l(g_lock);
    g_hold = hold_seconds > 0.2 ? hold_seconds : 0.2;
    g_held = 0;
    for (int &s : g_src) s = -1;
    g_seen[0] = g_seen[1] = -1e300;
    g_broken = false;
    g_since = -1;
    g_fired = false;
    g_begun = g_quiet = 0;
    g_let_go.clear();
    g_on = on;
}

bool on() { return g_on; }

bool touched() { return g_touched; }

void input(const std::string &name, int value, int reader) {
    if (value) g_touched = true;
    if (!g_on) return;
    for (int i = 0; i < kBits; i++)
        if (name == kWatched[i].name) {
            const unsigned bit = kWatched[i].bit;
            std::lock_guard<std::mutex> l(g_lock);
            if (value) {
                g_held |= bit;
                g_src[i] = reader == 0 || reader == 1 ? reader : -1;
            } else {
                // a key of a whole chord let go: that hold is over, whatever the next frame sees
                if (whole(g_held) && !whole(g_held & ~bit)) g_broken = true;
                g_held &= ~bit;
                g_src[i] = -1;
            }
            return;
        }
}

void reading(int reader, double at_seconds) {
    if (reader != 0 && reader != 1) return;
    std::lock_guard<std::mutex> l(g_lock);
    if (at_seconds > g_seen[reader]) g_seen[reader] = at_seconds;
}

bool frame(double now_seconds) {
    if (!g_on) return false;
    std::lock_guard<std::mutex> l(g_lock);
    const unsigned held = g_held & ~stale(now_seconds);
    if (g_broken || !whole(held)) {
        drop(now_seconds, !g_broken && whole(g_held));   // (whole but for a reader gone quiet)
        g_broken = false;
        if (!whole(held)) return false;
    }
    if (g_since < 0) {
        g_since = now_seconds;
        g_begun++;
    }
    if (g_fired || now_seconds - g_since < g_hold) return false;
    g_fired = true;
    return true;
}

double progress(double now_seconds) {
    if (!g_on) return 0;
    std::lock_guard<std::mutex> l(g_lock);
    if (g_since < 0 || g_broken || now_seconds - g_since < kShownAfter) return 0;
    if (!whole(g_held & ~stale(now_seconds))) return 0;
    const double p = (now_seconds - g_since) / g_hold;
    return p > 1 ? 1 : p;
}

double now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

#ifdef _WIN32
namespace {
DWORD parent_pid() {
    DWORD self = GetCurrentProcessId(), parent = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof pe;
    for (BOOL more = Process32FirstW(snap, &pe); more; more = Process32NextW(snap, &pe))
        if (pe.th32ProcessID == self) parent = pe.th32ParentProcessID;
    CloseHandle(snap);
    return parent;
}

struct Cover {
    DWORD pid;
    HWND found;
};

// This program's own windows go at once, with no fading out: Windows draws a window's fading over
// every other, the launcher's black too, with whatever picture it last had of it
BOOL CALLBACK no_fade(HWND h, LPARAM) {
    using Set = HRESULT(WINAPI *)(HWND, DWORD, LPCVOID, DWORD);
    static const Set set = [] {
        HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        return dwm ? reinterpret_cast<Set>(reinterpret_cast<void *>(GetProcAddress(dwm, "DwmSetWindowAttribute"))) : nullptr;
    }();
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    const BOOL off = TRUE;
    if (set && pid == GetCurrentProcessId()) set(h, 3 /* DWMWA_TRANSITIONS_FORCEDISABLED */, &off, sizeof off);
    return TRUE;
}

// The launcher's black cover: its visible window named "HIRAKU" (launcher/cover.h)
BOOL CALLBACK find_cover(HWND h, LPARAM p) {
    auto *c = reinterpret_cast<Cover *>(p);
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    wchar_t title[16];
    if (pid != c->pid || !IsWindowVisible(h) || GetWindowTextW(h, title, 16) != 6 || wcscmp(title, L"HIRAKU")) return TRUE;
    c->found = h;
    return FALSE;
}
}  // namespace
#endif

void window_up_for_launcher() {
#ifdef _WIN32
    if (getenv("HK_LAUNCHER")) EnumWindows(no_fade, 0);
#endif
}

void hand_screen_to_launcher() {
#ifdef _WIN32
    if (!getenv("HK_LAUNCHER")) return;
    // the launcher (the parent) may take the foreground; and its cover, when it has one up, takes it
    // now, while this program still has it: Windows would otherwise give it to some other window when
    // this one goes, the taskbar coming up over the black
    Cover c{parent_pid(), nullptr};
    if (c.pid) AllowSetForegroundWindow(c.pid);
    EnumWindows(no_fade, 0);
    if (c.pid) EnumWindows(find_cover, reinterpret_cast<LPARAM>(&c));
    if (c.found) SetForegroundWindow(c.found);
#endif
}

std::string report() {
    if (!g_on) return "";
    std::lock_guard<std::mutex> l(g_lock);
    char b[512];
    size_t n = (size_t)snprintf(b, sizeof b, "game menu: %u hold%s begun, %zu let go", g_begun, g_begun == 1 ? "" : "s",
                                g_let_go.size());
    for (size_t i = 0; i < g_let_go.size() && i < 8 && n < 400; i++)
        n += (size_t)snprintf(b + n, sizeof b - n, "%s%.2f s", i ? ", " : " (after ", g_let_go[i]);
    if (!g_let_go.empty()) n += (size_t)snprintf(b + n, sizeof b - n, "%s)", g_let_go.size() > 8 ? ", ..." : "");
    if (g_quiet) n += (size_t)snprintf(b + n, sizeof b - n, ", %u of them by a reader gone quiet", g_quiet);
    snprintf(b + n, sizeof b - n, ", %s", g_fired ? "1 completed" : "none completed");
    return b;
}

}  // namespace hk::host::menukey
