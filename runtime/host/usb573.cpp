// SPDX-License-Identifier: GPL-3.0-or-later
// The reports are the board's own (USB_2_573's firmware/core/u573_proto.h, docs/PROTOCOL.md).
#include "usb573.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

#include "../shell/core.h"
#include "../shell/wired573.h"
#include "menukey.h"
#include "warp.h"

namespace hk::host {

namespace usb573 {

namespace {

constexpr uint8_t IN_MAGIC = 0x73, OUT_MAGIC = 0x53;
constexpr uint8_t CMD_KEEPALIVE = 0, CMD_OUTPUTS = 1, CMD_CONFIG = 2, CMD_RESET = 3;
constexpr uint8_t STATUS_OVERFLOW = 0x02;
constexpr int OUT_DATA = 8, ENTRY = 8;   // where a report's data starts; an entry's bytes
constexpr uint32_t DELAY_UNIT_US = 10;

std::array<uint8_t, REPORT> command(uint8_t cmd) {
    std::array<uint8_t, REPORT> r{};
    r[0] = OUT_MAGIC;
    r[1] = cmd;
    return r;
}

void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }

}  // namespace

bool parse_in(const uint8_t *r, size_t n, In &in) {
    if (n < 26 || r[0] != IN_MAGIC) return false;
    in.version = r[1];
    in.status = r[2];
    in.switches = (uint32_t)r[4] | (uint32_t)r[5] << 8 | (uint32_t)r[6] << 16 | (uint32_t)r[7] << 24;
    memcpy(in.analog, r + 8, 4);
    in.applied = (uint16_t)(r[12] | r[13] << 8);
    in.queued = r[14];
    in.capacity = r[15];
    return true;
}

std::array<uint8_t, REPORT> outputs_report(const Entry *entries, int n, bool flush) {
    auto r = command(CMD_OUTPUTS);
    n = std::clamp(n, 0, ENTRIES_MAX);
    r[2] = (uint8_t)n;
    r[3] = flush ? 1 : 0;
    for (int k = 0; k < n; k++) {
        uint8_t *e = &r[OUT_DATA + k * ENTRY];
        put16(e, std::min<uint32_t>((entries[k].delay_us + DELAY_UNIT_US - 1) / DELAY_UNIT_US, 0xffff));
        const uint64_t state = entries[k].state & wired573::OUT_ALL;
        for (int b = 0; b < 6; b++) e[2 + b] = (uint8_t)(state >> (8 * b));
    }
    return r;
}

std::array<uint8_t, REPORT> keepalive_report() { return command(CMD_KEEPALIVE); }
std::array<uint8_t, REPORT> reset_report() { return command(CMD_RESET); }

std::array<uint8_t, REPORT> config_report(uint16_t debounce_us, uint16_t failsafe_ms, uint16_t counter_ms) {
    auto r = command(CMD_CONFIG);
    put16(&r[OUT_DATA], debounce_us);
    put16(&r[OUT_DATA + 2], failsafe_ms);
    put16(&r[OUT_DATA + 4], counter_ms);
    return r;
}

}  // namespace usb573

namespace {

using namespace usb573;
using Clock = std::chrono::steady_clock;

constexpr uint8_t PROTOCOL = 1;
constexpr size_t BACKLOG = 32;        // this many output states waiting: the board is behind, lamps' states merge
constexpr size_t PENDING_MAX = 16384; // ... and beyond this the oldest go (a board that is away for long)

std::string g_where;   // --usb573: "" not asked for
int g_debounce_us = -1;
std::unique_ptr<Link> g_link;
std::thread g_thread;
std::atomic<bool> g_running{false};
bool g_attached = false;   // the machine has a 573's connectors and the board's thread runs
bool g_refused = false;    // --usb573 was given to a machine without them
const std::vector<struct Named> *g_named = nullptr;   // the game menu in a 573's cabinet: its controls by name
bool g_ddr = false;        // a DDR cabinet: stage boards on A4, A5 and B4, B5
const bool g_log = getenv("HK_USB573_LOG") != nullptr;

struct Pending {
    Entry e;
    uint8_t stage = 0;   // bit p: the entry moves stage p's clock or data
};

std::mutex g_mutex;
// ---- under g_mutex ----
bool g_connected = false;
uint32_t g_switches = 0, g_closed = 0;   // as the board last said; closed at some time since the last frame
uint8_t g_analog[4] = {};
std::deque<Pending> g_pending;       // the outputs' changes not yet handed to the board
// The entries are numbered from 1 as they are queued: g_taken have left g_pending (handed to a
// board, or dropped), g_done are on a board's pins (or went with a board that left)
uint64_t g_taken = 0, g_done = 0;
uint64_t g_state = 0;                // the outputs as the last entry leaves them
bool g_stage_spoken = false;         // a stage edge has been queued
uint64_t g_stage_at = 0;             // ... the machine's time of the last one
uint64_t g_stage_entry[2] = {};      // the last entry to move stage p's lines (0: the game hasn't spoken to it)
bool g_stage_heard[2] = {};          // ... is on the board's pins
Clock::time_point g_stage_heard_at[2];
// ---- the core's thread ----
uint32_t g_last = 0;                 // the switches as the machine was last given them

void log_report(const char *what, const uint8_t *r, int n) {
    if (!g_log) return;
    fprintf(stderr, "usb573 %s", what);
    for (int k = 0; k < n && k < 32; k++) fprintf(stderr, " %02x", r[k]);
    fprintf(stderr, "\n");
}

bool send(const std::array<uint8_t, REPORT> &r) {
    log_report("out", r.data(), r[1] == 1 ? 8 + r[2] * 8 : 8);
    return g_link->write(r.data(), REPORT);
}

void wait(std::chrono::milliseconds t) {
    for (const auto until = Clock::now() + t; g_running && Clock::now() < until;) std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

// Under g_mutex: which stages' boards have the game's every edge
void stages_heard(Clock::time_point now) {
    for (int p = 0; p < 2; p++) {
        const bool heard = g_stage_entry[p] && g_done >= g_stage_entry[p];
        if (heard && !g_stage_heard[p]) g_stage_heard_at[p] = now;
        g_stage_heard[p] = heard;
    }
}

// A board, from its reset until it is gone or the game ends
void session() {
    uint8_t r[REPORT];
    In in;
    // its first report: that it is one, and where its count of entries stands. The outputs are left
    // as they are until the game's own state goes out (no reset: a DDR stage's lines would move)
    bool found = false;
    for (const auto until = Clock::now() + std::chrono::milliseconds(500); g_running && !found && Clock::now() < until;) {
        const int n = g_link->read(r, sizeof r, 20);
        if (n < 0) return;
        found = n > 0 && parse_in(r, (size_t)n, in);
    }
    if (!g_running) return;
    if (!found || in.version != PROTOCOL) {
        if (found) fprintf(stderr, "usb573: the board speaks protocol %d, this HIRAKU %d: one of them needs an update\n", in.version, PROTOCOL);
        else fprintf(stderr, "usb573: the device doesn't answer as a USB_2_573 board\n");
        wait(std::chrono::seconds(5));
        return;
    }
    if (g_debounce_us >= 0 && !send(config_report((uint16_t)g_debounce_us, 1000, 1000))) return;
    fprintf(stderr, "usb573: the cabinet's board is connected\n");

    uint64_t base;   // the entries before this board's first
    {
        std::lock_guard<std::mutex> l(g_mutex);
        base = g_done = g_taken;
        g_connected = true;
        // what waited for a board goes to this one; with nothing waiting, how the outputs stand
        if (g_pending.empty()) g_pending.push_back({{g_state, 0}, 0});
        stages_heard(Clock::now());
    }
    uint16_t last_applied = in.applied;
    uint64_t applied = 0, sent = 0;   // this board's, from here on
    int capacity = in.capacity ? in.capacity : 64;
    bool overflow_said = (in.status & STATUS_OVERFLOW) != 0;   // (left from whoever drove it before)
    bool first = true;
    auto written = Clock::now();
    while (g_running) {
        const int n = g_link->read(r, sizeof r, 10);
        if (n < 0) return;
        const auto now = Clock::now();
        if (n > 0 && parse_in(r, (size_t)n, in)) {
            log_report("in ", r, 26);
            applied += (uint16_t)(in.applied - last_applied);
            last_applied = in.applied;
            if (in.capacity) capacity = in.capacity;
            if ((in.status & STATUS_OVERFLOW) && !overflow_said) {   // (never, with the count kept below)
                fprintf(stderr, "usb573: the board dropped output states (its queue was full)\n");
                overflow_said = true;
            }
            std::lock_guard<std::mutex> l(g_mutex);
            g_switches = in.switches & wired573::CABINET;
            g_closed |= g_switches;
            memcpy(g_analog, in.analog, 4);
            g_done = base + applied;
            stages_heard(now);
        }
        // the outputs: as many entries as the board has room for, a report's worth at a time
        Entry batch[ENTRIES_MAX];
        int count = 0;
        {
            std::lock_guard<std::mutex> l(g_mutex);
            const int64_t room = capacity - (int64_t)(sent - applied);
            while (count < ENTRIES_MAX && count < room && !g_pending.empty()) {
                batch[count++] = g_pending.front().e;
                g_pending.pop_front();
                g_taken++;
            }
        }
        if (count) {
            if (!send(outputs_report(batch, count, first))) return;   // (the first drops what another program left queued)
            first = false;
            sent += (uint64_t)count;
            written = now;
        } else if (now - written > std::chrono::milliseconds(200)) {
            if (!send(keepalive_report())) return;   // (its failsafe: a second without a word and it turns everything off)
            written = now;
        }
    }
    send(reset_report());   // the game ends: nothing stays lit
}

void run() {
    bool said = false;
    while (g_running) {
        std::string why;
        if (!g_link->open(g_where, why)) {
            if (!said || !why.empty())
                fprintf(stderr, "usb573: %s (looked for every second; the game runs without it meanwhile)\n",
                        why.empty() ? "no USB_2_573 board found" : why.c_str());
            said = true;
            wait(std::chrono::seconds(why.empty() ? 1 : 10));
            continue;
        }
        said = false;
        session();
        g_link->close();
        {
            std::lock_guard<std::mutex> l(g_mutex);
            const bool was = g_connected;
            g_connected = false;
            g_switches = g_closed = 0;
            g_done = g_taken;   // what it was handed went with it
            stages_heard(Clock::now());
            if (was && g_running) fprintf(stderr, "usb573: the cabinet's board is gone\n");
        }
        if (g_running) wait(std::chrono::milliseconds(500));
    }
}

// The game menu in a System 573 cabinet (runtime/menu/: the cabinet's family, no 573): the
// cabinet's switches by the names its controls have (shell/cabinet.cpp), as the 573's ports have
// them (sys573/board.cpp, set_input). A control is held while any of its switches is closed (a
// DDR Solo panel's two sensors, a conga pad's two).
struct Named {
    uint32_t switches;
    const char *name;
};
const std::vector<Named> *named_controls(const std::string &family) {
    using namespace wired573;
    static const std::map<std::string, std::vector<Named>> all = {
        {"ddr",
         {{P1_LEFT, "P1 Left"}, {P1_RIGHT, "P1 Right"}, {P1_UP, "P1 Up"}, {P1_DOWN, "P1 Down"},
          {P2_LEFT, "P2 Left"}, {P2_RIGHT, "P2 Right"}, {P2_UP, "P2 Up"}, {P2_DOWN, "P2 Down"},
          {P1_B1, "P1 Button 1"}, {P1_B2, "P1 Button 2"}, {P1_B3, "P1 Button 3"},
          {P2_B1, "P2 Button 1"}, {P2_B2, "P2 Button 2"}, {P2_B3, "P2 Button 3"},
          {P1_START, "1 Player Start"}, {P2_START, "2 Players Start"}}},
        {"ddrsolo",
         {{P1_LEFT | P1_B2, "P1 Left"}, {P1_RIGHT | P2_B2, "P1 Right"}, {P1_UP | P2_B1, "P1 Up"}, {P1_DOWN | P1_B3, "P1 Down"},
          {P2_LEFT | P1_B1, "P1 Up-Left"}, {P2_RIGHT | P2_B3, "P1 Up-Right"},
          {P1_B5, "P1 Select L"}, {P2_B5, "P1 Select R"}, {P1_START, "1 Player Start"}}},
        {"dmx",
         {{P1_UP, "P1 Select L"}, {P1_DOWN, "P1 Select R"}, {P1_B1, "P1 Upper Left"}, {P1_B2, "P1 Upper Right"},
          {P1_B4 | P1_LEFT, "P1 Lower Left"}, {P1_B5 | P1_RIGHT, "P1 Lower Right"},
          {P2_UP, "P2 Select L"}, {P2_DOWN, "P2 Select R"}, {P2_B1, "P2 Upper Left"}, {P2_B2, "P2 Upper Right"},
          {P2_B4 | P2_LEFT, "P2 Lower Left"}, {P2_B5 | P2_RIGHT, "P2 Lower Right"},
          {P1_START, "1 Player Start"}, {P2_START, "2 Players Start"}}},
        {"mambo",
         {{P1_LEFT, "Select Left"}, {P1_RIGHT, "Select Right"}, {P1_START, "1 Player Start"},
          {P1_B2 | P2_START, "Left Pad 1"}, {P1_B5 | P1_B1, "Left Pad 2"}, {P1_B4 | P1_B3, "Left Pad 3"},
          {P2_UP | P1_DOWN, "Centre Pad 1"}, {P2_DOWN | P2_LEFT, "Centre Pad 2"}, {P1_UP | P2_RIGHT, "Centre Pad 3"},
          {P2_B2 | COIN2, "Right Pad 1"}, {P2_B5 | P2_B1, "Right Pad 2"}, {P2_B4 | P2_B3, "Right Pad 3"}}},
        {"drmn",
         {{P1_LEFT, "High Tom"}, {P1_RIGHT, "Low Tom"}, {P1_UP, "Hi-Hat"}, {P1_DOWN, "Snare"},
          {P1_B1, "Cymbal"}, {P1_B3, "Bass Drum"}, {P2_LEFT, "Select L"}, {P2_RIGHT, "Select R"}, {P1_START, "1 Player Start"}}},
        {"gtrf",
         {{P1_B1, "P1 Button R"}, {P1_B2, "P1 Button G"}, {P1_B3, "P1 Button B"}, {P1_UP, "P1 Pick"}, {P1_DOWN, "P1 Wailing"},
          {P2_B1, "P2 Button R"}, {P2_B2, "P2 Button G"}, {P2_B3, "P2 Button B"}, {P2_UP, "P2 Pick"}, {P2_DOWN, "P2 Wailing"},
          {P1_START, "1 Player Start"}, {P2_START, "2 Players Start"}}},
    };
    const auto f = all.find(family);
    return f != all.end() ? &f->second : nullptr;
}

// Under g_mutex: one change of the outputs, queued for the board
void queue(const wired573::Output &c) {
    const uint64_t diff = c.state ^ g_state;
    uint8_t stage = 0;
    if (g_ddr) stage = (uint8_t)((diff & wired573::OUT_STAGE1 ? 1 : 0) | (diff & wired573::OUT_STAGE2 ? 2 : 0));
    Entry e{c.state, 0};
    if (stage) {
        // a stage's edges keep the game's own pace, within what its board surely follows
        const uint64_t gap = g_stage_spoken && c.at_us > g_stage_at ? c.at_us - g_stage_at : 0;
        e.delay_us = (uint32_t)std::clamp<uint64_t>(gap, STAGE_GAP_MIN, STAGE_GAP_MAX);
        g_stage_spoken = true;
        g_stage_at = c.at_us;
    }
    g_state = c.state;
    // a lamp's state that waits gives way to a newer one when the board is behind, or away; a
    // stage's edges never do
    if (!stage && !g_pending.empty() && !g_pending.back().stage && (g_pending.size() >= BACKLOG || !g_connected)) {
        g_pending.back().e.state = c.state;
        return;
    }
    if (g_pending.size() >= PENDING_MAX) g_pending.pop_front(), g_taken++;
    g_pending.push_back({e, stage});
    for (int p = 0; p < 2; p++)
        if (stage >> p & 1) g_stage_entry[p] = g_taken + g_pending.size(), g_stage_heard[p] = false;
}

}  // namespace

bool usb573_option(const std::string &name, const std::function<std::string()> &val) {
    if (name == "--usb573") {
        g_where = val();
        if (g_where == "off" || g_where == "none") g_where.clear();
    } else if (name == "--usb573-debounce") {
        g_debounce_us = std::clamp(std::stoi(val()), 0, 65535);
    } else {
        return false;
    }
    return true;
}

std::string usb573_help() {
    return "  --usb573 auto|present|PATH  a System 573 cabinet on a USB_2_573 board (System 573 games, with\n"
           "                    --window): the cabinet's switches as the JAMMA edge has them, the I/O board's lights,\n"
           "                    EXT-OUT and the coin counters, a DDR cabinet's stages (docs/USB573.md). auto: the\n"
           "                    board, looked for every second until it is there; present: only if it is plugged in\n"
           "                    when the game starts; PATH: one board's HID path. off: none\n"
           "  --usb573-debounce US  how long the board leaves a switch alone after it changed, in microseconds\n"
           "                    (the board's own: 2000; 0: none)\n";
}

void usb573_start(hk::Core &core) {
    if (g_where.empty() || g_attached) return;
    g_named = nullptr;
    if (!core.wired_attach(true)) {
        // not a 573: the game menu in a 573's cabinet takes the cabinet's switches by their names
        g_named = named_controls(core.family());
        if (!g_named) {
            fprintf(stderr, "usb573: this machine isn't a System 573: the board is left alone\n");
            g_refused = true;
            return;
        }
    }
    if (!g_link) {
        fprintf(stderr, "usb573: this build can't reach USB devices (no SDL)\n");
        if (!g_named) core.wired_attach(false);
        return;
    }
    if (g_where == "present") {   // only a board that is there now: nothing is looked for afterwards
        std::string why;
        if (!g_link->open("auto", why)) {
            if (!why.empty()) fprintf(stderr, "usb573: %s\n", why.c_str());
            if (!g_named) core.wired_attach(false);
            g_refused = true;
            return;
        }
        g_link->close();
        g_where = "auto";
    }
    g_ddr = core.family() == "ddr" && !g_named;
    {
        std::lock_guard<std::mutex> l(g_mutex);
        g_connected = false;
        g_switches = g_closed = g_last = 0;
        g_pending.clear();
        g_taken = g_done = g_state = g_stage_at = 0;
        g_stage_spoken = false;
        for (int p = 0; p < 2; p++) g_stage_entry[p] = 0, g_stage_heard[p] = false;
    }
    g_attached = true;
    g_running = true;
    g_thread = std::thread(run);
    atexit(usb573_stop);   // before the thread's state goes (the game leaves through exit())
}

void usb573_stop() {
    if (!g_attached) return;
    g_running = false;
    if (g_thread.joinable()) g_thread.join();
    g_attached = false;
    g_named = nullptr;
    std::lock_guard<std::mutex> l(g_mutex);
    g_connected = false;
    g_pending.clear();
}

void usb573_lamps(hk::Core &core) {
    if (!g_attached || g_named) return;   // (the menu's lamps are names: not on the board yet)
    static std::vector<wired573::Output> changes;
    changes.clear();
    core.wired_outputs(changes);
    if (changes.empty()) return;
    std::lock_guard<std::mutex> l(g_mutex);
    for (const auto &c : changes) queue(c);
}

void usb573_frame(hk::Core &core) {
    if (!g_attached) return;
    uint32_t sw;
    uint8_t analog[4];
    bool connected, panels[2];
    {
        std::lock_guard<std::mutex> l(g_mutex);
        const auto now = Clock::now();
        connected = g_connected;
        // a switch that closed and opened again within the frame (a drum pad's pulse) still counts
        sw = connected ? g_switches | g_closed : 0;
        g_closed = 0;
        memcpy(analog, g_analog, 4);
        for (int p = 0; p < 2; p++)
            panels[p] = g_stage_heard[p] && now - g_stage_heard_at[p] >= std::chrono::duration<double>(STAGE_SETTLE);
    }
    // a DDR stage's lines are its panels only once its board has heard the game out; until then
    // (and while more of the game's edges are on their way) they are that board's own signals
    if (g_ddr) {
        if (!panels[0]) sw &= ~(uint32_t)wired573::P1_STAGE;
        if (!panels[1]) sw &= ~(uint32_t)wired573::P2_STAGE;
    }
    if (g_named) {
        // A DDR stage's board that no game has spoken to since the cabinet was turned on holds
        // right and up: not its panels then (nor are they for anyone standing on both)
        if (core.family() == "ddr")
            for (const uint32_t stage : {(uint32_t)wired573::P1_STAGE, (uint32_t)wired573::P2_STAGE}) {
                const uint32_t held = (wired573::P1_RIGHT | wired573::P1_UP | wired573::P2_RIGHT | wired573::P2_UP) & stage;
                if ((sw & held) == held) sw &= ~stage;
            }
        for (const Named &n : *g_named) {
            const bool now = sw & n.switches, was = g_last & n.switches;
            if (now != was) core.set_input(n.name, now);
        }
        for (const auto &[bit, name] : {std::pair<uint32_t, const char *>{wired573::COIN1, "Coin 1"}, {wired573::TEST, "Test"}, {wired573::SERVICE, "Service"}})
            if ((sw ^ g_last) & bit) core.set_input(name, sw & bit ? 1 : 0);
    } else {
        core.wired_inputs(sw, connected ? analog : nullptr);
    }

    const uint32_t changed = sw ^ g_last;
    if (changed & wired573::TEST) menukey::input("Test", sw & wired573::TEST ? 1 : 0);   // (both held: the way out to the game menu)
    if (changed & wired573::SERVICE) menukey::input("Service", sw & wired573::SERVICE ? 1 : 0);
    if (changed & sw) warp::pressed();   // the fast start-up ends (host/warp.h)
    g_last = sw;
}

bool usb573_owns(uint16_t vid, uint16_t pid, const char *product) {
    // (from the option on, not from the board's thread: the controllers are opened before it starts)
    return !g_where.empty() && !g_refused && vid == VID && pid == PID && product && !strncmp(product, PRODUCT, strlen(PRODUCT));
}

namespace usb573 {

void use_link(std::unique_ptr<Link> link) { g_link = std::move(link); }

}  // namespace usb573

}  // namespace hk::host
