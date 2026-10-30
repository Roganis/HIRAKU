// SPDX-License-Identifier: GPL-3.0-or-later
// The BIO2's layout is spice2x's (games/iidx/bi2a.cpp, iidx.cpp: GPLv3, the sp2xdev team and the
// SpiceTools contributors).
#include "bio2.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <regex>
#include <thread>
#include <vector>

#include "../shell/cabinet.h"
#include "../shell/core.h"
#include "acio.h"
#include "warp.h"

namespace hk::host {

namespace {

constexpr size_t OUT = 0x30, IN = 0x2e;

std::string g_path;
std::atomic<bool> g_running{false};
std::thread g_thread;
std::mutex g_mutex;
uint8_t g_out[OUT] = {};   // the lamps, from bio2_frame
uint8_t g_in[IN] = {};     // the controls, as the board last sent them
bool g_have_in = false;

void sleep_ms(int ms) {
    for (int t = 0; t < ms && g_running; t += 50) std::this_thread::sleep_for(std::chrono::milliseconds(std::min(50, ms - t)));
}

void run() {
    AcioPort port(g_path);
    bool told = false;
    while (g_running) {
        if (!port.open()) {
            if (!told) fprintf(stderr, "bio2 %s: cannot open it (%s); trying again every 5 s\n", g_path.c_str(), strerror(errno));
            told = true;
            sleep_ms(5000);
            continue;
        }
        told = false;
        // the start-up as beatmania IIDX 27 does it (docs/BIO2-BI2A.md): a break and aa echoes at
        // 115200, the node's address, its firmware version, then (below) start, init and watchdog
        AcioPort::Message m;
        port.tag_zero(true);
        uint8_t node = 1;
        if (!port.handshake_bio2([] { return g_running.load(); }) || !port.ask_one(0, 0x0001, {0x00}, m, 500, 3) ||
            !(node = m.payload.empty() || !m.payload[0] ? 1 : m.payload[0]) || !port.ask_one(node, 0x0002, {}, m, 500, 3)) {
            fprintf(stderr, "bio2 %s: no answer as an ACIO board\n", g_path.c_str());
            port.close();
            sleep_ms(5000);
            continue;
        }
        const std::string model = m.payload.size() >= 12 ? std::string((const char *)&m.payload[8], 4) : "?";
        if (model != "BI2A") {
            fprintf(stderr, "bio2 %s: the board says it is \"%s\", not BI2A (a BIO2 with the BI2X firmware of IIDX 27 and "
                            "later speaks ACIO2, which HIRAKU doesn't yet)\n", g_path.c_str(), model.c_str());
            port.close();
            sleep_ms(30000);
            continue;
        }
        fprintf(stderr, "bio2 %s: BI2A %u.%u.%u: the cabinet's controls and lamps\n", g_path.c_str(), m.payload[5], m.payload[6],
                m.payload[7]);
        // start (command mode), init (0x2d: IIDX's, which wakes the sub-I/O; SDVX sends 0x3b), the
        // watchdog (x 100 ms, big-endian: 0 = off, as IIDX 27)
        bool ok = port.ask_one(node, 0x0003, {}, m, 500, 3) && port.ask_one(node, 0x0100, {0x2d}, m, 500, 3) &&
                  port.ask_one(node, 0x0120, {0x00, 0x00}, m, 500, 3);
        while (ok && g_running) {
            std::vector<uint8_t> out;
            {
                std::lock_guard<std::mutex> l(g_mutex);
                out.assign(g_out, g_out + OUT);
            }
            ok = port.ask_one(node, 0x0152, out, m, 20, 3);   // IIDX 27: 20 ms, three tries
            if (ok && m.payload.size() >= IN) {
                std::lock_guard<std::mutex> l(g_mutex);
                memcpy(g_in, m.payload.data(), IN);
                g_have_in = true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (g_running) fprintf(stderr, "bio2 %s: lost; connecting again\n", g_path.c_str());
        port.close();
        sleep_ms(2000);
    }
}

bool bit(const uint8_t *in, int n) { return in[n / 8] >> (n % 8) & 1; }

// The board's buttons (bit, input)
struct Button {
    int bit;
    std::string name;
};
std::vector<Button> buttons() {
    std::vector<Button> b = {{10, "Service"}, {11, "Test"}, {76, "Effect"}, {77, "VEFX"}, {78, "2 Players Start"}, {79, "1 Player Start"}};
    for (int k = 0; k < 7; k++) {
        b.push_back({151 + 16 * k, "P1 Button " + std::to_string(k + 1)});
        b.push_back({263 + 16 * k, "P2 Button " + std::to_string(k + 1)});
    }
    return b;
}

// Where a lamp of the game's goes: its byte of the board's 0x30, or -1
int lamp_byte(const std::string &name) {
    static const std::regex key(R"(P([12]) (Key|Button) Lamp ([1-7]))"), spot(R"(Spotlight ([1-8]))");
    std::smatch m;
    if (std::regex_match(name, m, key)) return 0x07 + (m[1] == "2" ? 7 : 0) + std::stoi(m[3]) - 1;
    if (std::regex_match(name, m, spot)) {
        const int k = std::stoi(m[1]);
        return k <= 4 ? 0x20 + k - 1 : 0x25 + k - 5;
    }
    if (name == "P1 Start Lamp") return 0x03;
    if (name == "P2 Start Lamp") return 0x04;
    if (name == "VEFX Lamp") return 0x05;
    if (name == "Effect Lamp") return 0x06;
    if (name.compare(0, 4, "Neon") == 0) return 0x24;
    return -1;
}

}  // namespace

bool bio2_option(const std::string &name, const std::function<std::string()> &val) {
    if (name != "--bio2") return false;
    g_path = val();
    return true;
}

std::string bio2_help() {
    return "  --bio2 PATH          beatmania IIDX's BIO2 I/O board (BI2A firmware) on its serial port: the cabinet's\n"
           "                       controls and lamps\n";
}

void bio2_start() {
    if (g_path.empty()) return;
    g_running = true;
    g_thread = std::thread(run);
    atexit(bio2_stop);   // before the thread's state goes (the game leaves through exit())
}

void bio2_stop() {
    g_running = false;
    if (g_thread.joinable()) g_thread.join();
}

void bio2_frame(hk::Core &core) {
    if (!g_running) return;
    // the lamps out
    static std::string family;
    static std::vector<int> where;   // each of cab::lamps(family)'s byte, or -1
    if (family != core.family()) {
        family = core.family();
        where.clear();
        for (auto &l : cab::lamps(family)) where.push_back(lamp_byte(l.name));
    }
    uint8_t out[OUT] = {};
    const auto &lit = core.lamp_state();
    for (size_t i = 0; i < where.size() && i < lit.size(); i++)
        if (where[i] >= 0 && lit[i]) out[where[i]] = 0x80;   // bit 7, as IIDX 27 writes a lit lamp
    const std::string text = core.display_text();
    if (!text.empty())
        for (size_t k = 0; k < 9; k++) out[0x17 + k] = k < text.size() ? (uint8_t)text[k] : ' ';
    uint8_t in[IN];
    {
        std::lock_guard<std::mutex> l(g_mutex);
        memcpy(g_out, out, OUT);
        if (!g_have_in) return;
        memcpy(in, g_in, IN);
    }

    // the controls in: what changed since the last frame
    static uint8_t last[IN];
    static bool first = true;
    if (first) {   // where the turntables, sliders and coin count start from; the buttons from up
        memcpy(last, in, IN);
        for (auto &b : buttons()) last[b.bit / 8] &= (uint8_t)~(1 << b.bit % 8);
        for (int s : {0, 2, 4, 6, 7}) last[s] ^= 0xf0;   // so each slider is set once
        first = false;
    }
    static const std::vector<Button> all = buttons();
    for (auto &b : all)
        if (bit(in, b.bit) != bit(last, b.bit)) {
            core.set_input(b.name, bit(in, b.bit));
            if (bit(in, b.bit)) warp::pressed();   // the fast start-up ends (host/warp.h)
        }
    for (int p = 0; p < 2; p++)
        if (int8_t d = (int8_t)(in[16 + p] - last[16 + p])) core.turntable_add(p, d * 4);   // 256 a turn, to 1024
    const int sliders[5] = {0, 2, 4, 6, 7};
    for (int k = 0; k < 5; k++)
        if ((in[sliders[k]] ^ last[sliders[k]]) & 0xf0) core.set_input("Volume " + std::to_string(k + 1), in[sliders[k]] >> 4);
    // coins: the count goes up by one a coin; each a press of a few frames
    static int coins = 0, coin_frames = 0;
    if (uint8_t d = (uint8_t)((in[8] - last[8]) & 0x3f); d && d < 16) coins += d;   // a 6-bit count (IIDX 27)
    if (coin_frames) {
        if (--coin_frames == 2) core.set_input("Coin 1", 0);
    } else if (coins) {
        coins--;
        core.set_input("Coin 1", 1);
        warp::pressed();
        coin_frames = 4;   // 2 down, 2 up
    }
    memcpy(last, in, IN);
}

}  // namespace hk::host
