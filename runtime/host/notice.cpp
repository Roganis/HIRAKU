// SPDX-License-Identifier: GPL-3.0-or-later
#include "notice.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

#include "../shell/sys.h"
#include "../shell/userdir.h"
#include "lang/fit.h"
#include "lang/lang.h"

namespace hk::host::notice {

namespace {

using Clock = std::chrono::steady_clock;
constexpr double kWithin = 15, kShow = 6, kFade = 1;   // seconds
constexpr long kFresh = 6 * 3600;

std::mutex g_mu;
bool g_newer = false;
std::string g_current, g_latest;
Clock::time_point g_start, g_shown;
enum class State { off, waiting, done } g_state = State::off;   // the update check's

// the banners to show, in turn: the first is the one on screen once it has its picture
struct Item {
    std::vector<std::string> lines;
    double seconds;
    uint32_t edge;
    ui::Icon icon = ui::Icon::info;
};
std::vector<Item> g_queue;
std::vector<Item> g_hints;   // hint()'s, under g_mu, for banner() to queue
bool g_showing = false;
std::vector<uint32_t> g_pixels;
int g_w = 0, g_h = 0;
double g_scale = 1;
unsigned g_id = 0;

std::string read_all(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

// a value of update.py's flat JSON: "key": "text", a number, true or false
std::string field(const std::string &json, const std::string &key) {
    size_t p = json.find("\"" + key + "\"");
    if (p == std::string::npos || (p = json.find(':', p)) == std::string::npos) return {};
    p = json.find_first_not_of(" \t", p + 1);
    if (p == std::string::npos) return {};
    if (json[p] == '"') {
        const size_t e = json.find('"', p + 1);
        return e == std::string::npos ? std::string() : json.substr(p + 1, e - p - 1);
    }
    const size_t e = json.find_first_of(",}", p);
    return json.substr(p, e - p);
}

// a menu entry's words in a sentence: without its Alt letter ("&Help", "ヘルプ(&H)") or its "…"
std::string menu_words(std::string s) {
    if (const size_t p = s.find("(&"); p != std::string::npos && p + 3 < s.size() + 1 && s.find(')', p) == p + 3) s.erase(p, 4);
    for (size_t p; (p = s.find('&')) != std::string::npos;) s.erase(p, 1);
    for (const char *end : {"\xe2\x80\xa6", "..."})
        if (s.size() >= strlen(end) && s.compare(s.size() - strlen(end), strlen(end), end) == 0) s.resize(s.size() - strlen(end));
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

// the launcher's settings (QSettings' ini): [updates] check=false turns the check off
bool turned_off(const std::string &root) {
    std::istringstream in(read_all(hk::user_config_dir(root) + "/launcher.ini"));
    std::string line, section;
    while (std::getline(in, line)) {
        line = trim(line);
        if (!line.empty() && line[0] == '[') section = line;
        else if (section == "[updates]" && line == "check=false") return true;
    }
    return false;
}

// the answer in update-check.json, if it is about this version: false if there's none
bool take(const std::string &json, const std::string &current, long max_age) {
    if (json.empty() || field(json, "current") != current) return false;
    const long checked = atol(field(json, "checked").c_str());
    if (max_age && (checked <= 0 || (long)time(nullptr) - checked > max_age)) return false;
    std::lock_guard<std::mutex> lk(g_mu);
    g_newer = field(json, "newer") == "true";
    g_latest = field(json, "latest");
    if (!g_latest.empty() && (g_latest[0] == 'v' || g_latest[0] == 'V')) g_latest.erase(0, 1);
    return true;
}

// a banner's picture: in the launcher's look (host/uibox.h), or its lines in SDL's debug font on a
// dark box with a coloured edge, drawn by SDL's software renderer
void draw_picture(const Item &it) {
    // the lines as the log has them (English) shown in the interface's language where the
    // launcher's fonts can draw it (ui::said: a text marked HK_N, or lang::later's)
    std::vector<std::string> lines;
    for (const std::string &l : it.lines) lines.push_back(ui::said("boxes", l, true));
    if (ui::styled()) {
        ui::Image im;
        ui::notice_box(im, it.icon, lines);
        g_pixels = std::move(im.px);
        g_w = im.w, g_h = im.h, g_scale = im.scale;
        g_id++;
        return;
    }
    g_scale = 1;
    // (each line a paragraph: broken at 56 of the 8x8 font's characters)
    {
        std::vector<std::string> broken;
        for (const std::string &l : lines)
            for (const std::string &b : hk::lang::wrap(l, 56 * 8, [](uint32_t) { return 8; })) broken.push_back(b);
        lines = std::move(broken);
    }
    if (lines.empty()) return;
    size_t cols = 0;
    for (const std::string &l : lines) cols = std::max(cols, l.size());
    const int pad = 6, w = (int)cols * 8 + 2 * pad, h = (int)lines.size() * 12 - 4 + 2 * pad;
    g_pixels.clear();
    SDL_Surface *s = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_ARGB8888);
    if (!s) return;
    if (SDL_Renderer *r = SDL_CreateSoftwareRenderer(s)) {
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(r, (uint8_t)(it.edge >> 16), (uint8_t)(it.edge >> 8), (uint8_t)it.edge, 235);
        SDL_RenderClear(r);
        SDL_SetRenderDrawColor(r, 16, 16, 20, 225);
        const SDL_FRect in{1, 1, (float)w - 2, (float)h - 2};
        SDL_RenderFillRect(r, &in);
        for (size_t k = 0; k < lines.size(); k++) {
            const uint8_t c = k ? 200 : 255;
            SDL_SetRenderDrawColor(r, c, c, c, 255);
            SDL_RenderDebugText(r, (float)pad, (float)(pad + 12 * k), lines[k].c_str());
        }
        SDL_RenderPresent(r);
        SDL_DestroyRenderer(r);
        g_pixels.resize((size_t)w * h);
        for (int y = 0; y < h; y++)
            memcpy(&g_pixels[(size_t)y * w], (const uint8_t *)s->pixels + (size_t)y * s->pitch, (size_t)w * 4);
        g_w = w;
        g_h = h;
        g_id++;
    }
    SDL_DestroySurface(s);
}

}  // namespace

void start(const std::string &root) {
    ui::styled();   // (the launcher's look and fonts read now, while their files are sure to be there)
    if (getenv("HK_LAUNCHER") || (getenv("HK_UPDATE_NOTICE") && !strcmp(getenv("HK_UPDATE_NOTICE"), "0"))) return;
    g_current = trim(read_all(root + "/VERSION"));
    const std::string script = root + "/tools/update.py";
    if (g_current.empty() || !std::ifstream(script) || turned_off(root)) return;
    g_start = Clock::now();
    g_state = State::waiting;
    const std::string cache = hk::user_data_dir(root) + "/update-check.json";
    if (take(read_all(cache), g_current, kFresh)) return;
#ifdef _WIN32
    const std::string py = root + "/python/python.exe";
#else
    std::string py = root + "/python/bin/python3";
    if (!std::ifstream(py)) py = "/usr/bin/python3";
#endif
    // in the background: the game starts meanwhile (and the process may end before it does)
    std::thread([py, script, cache, current = g_current] {
        if (hk::sys::run_quiet({py, script, "check", "--save", cache}) == 0) take(read_all(cache), current, 0);
    }).detach();
}

void warn(const std::vector<std::string> &lines, double seconds) {
    if (lines.empty()) return;
    for (const std::string &l : lines) fprintf(stderr, "%s\n", hk::lang::english(l).c_str());
    // before the update's banner, unless one is on screen already
    g_queue.insert(g_queue.begin() + (g_showing ? 1 : 0), Item{lines, seconds, 0xd03030, ui::Icon::alert});
}

void hint(const std::vector<std::string> &lines, double seconds, ui::Icon icon) {
    if (lines.empty()) return;
    std::lock_guard<std::mutex> lk(g_mu);
    g_hints.push_back(Item{lines, seconds, 0xe8a018, icon});
}

bool banner(Banner &b) {
    const auto now = Clock::now();
    {
        std::lock_guard<std::mutex> lk(g_mu);
        for (Item &i : g_hints) g_queue.push_back(std::move(i));
        g_hints.clear();
    }
    auto secs = [&](Clock::time_point t) { return std::chrono::duration<double>(now - t).count(); };
    if (g_state == State::waiting) {
        bool newer;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            newer = g_newer;
        }
        if (secs(g_start) > kWithin) g_state = State::done;   // too late: the game is under way
        else if (newer) {
            g_state = State::done;
            fprintf(stderr, "HIRAKU %s is out (this is %s): update it from the launcher\n", g_latest.c_str(), g_current.c_str());
            // the launcher's own menu words, as its menu has them in the player's language (the
            // same catalogue: they never disagree); in English without the launcher's fonts
            std::string where = "Help > Check for updates";
            if (ui::styled()) where = menu_words(hk::lang::tr("MainWindow", "&Help")) + " > " + menu_words(hk::lang::tr("MainWindow", "Check for updates…"));
            // TR: %1 is the launcher's menu entry, "Help > Check for updates"
            const std::string update = ui::styled() ? hk::lang::arg(hk::lang::tr("boxes", "Update it from the launcher: %1."), {where})
                                                    : "Update it from the launcher: " + where + ".";
            // TR: %1 the new version, %2 the one running ("0.15.0")
            g_queue.push_back(Item{{hk::lang::later(HK_N("boxes", "HIRAKU %1 is out (this is %2)."), {g_latest, g_current}), update},
                                   kShow, 0xe86c18, ui::Icon::update});
        }
    }
    while (!g_queue.empty()) {
        if (!g_showing) {
            draw_picture(g_queue.front());
            if (g_pixels.empty()) {
                g_queue.erase(g_queue.begin());
                continue;
            }
            g_showing = true;
            g_shown = now;
        }
        const double t = secs(g_shown), len = g_queue.front().seconds;
        if (t < len) {
            b = {&g_pixels, g_w, g_h, (float)std::min(1.0, (len - t) / kFade), g_id, g_scale};
            return true;
        }
        g_queue.erase(g_queue.begin());
        g_showing = false;
    }
    return false;
}

}  // namespace hk::host::notice
