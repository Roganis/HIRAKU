// SPDX-License-Identifier: GPL-3.0-or-later
#include "warp.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

namespace hk::host::warp {

namespace {

using Clock = std::chrono::steady_clock;
constexpr double kRedraw = 0.25, kFade = 1.0;   // seconds of wall time

double g_until = 0;
bool g_started = false;
double g_v0 = 0, g_vlast = 0, g_vmark = 0;   // the machine's time: at the start, the last frame, the last redraw
Clock::time_point g_t0, g_tmark, g_tend;
double g_speed = 0;
bool g_ended = false;
std::string g_report;

std::vector<uint32_t> g_pixels;
int g_w = 0, g_h = 0;
unsigned g_id = 0;

double secs(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double>(b - a).count(); }

// the box: its lines in SDL's debug font and a bar of how far the start-up is, on a dark box with a
// blue edge (as host/notice.h's banners are drawn)
void draw(double done, bool ended) {
    char speed[64];
    if (ended) snprintf(speed, sizeof speed, "Done: the game runs at its normal speed");
    else if (g_speed > 0) snprintf(speed, sizeof speed, "Speeding through the start-up: %.1fx", g_speed);
    else snprintf(speed, sizeof speed, "Speeding through the start-up");
    const char *lines[] = {"FAST START-UP", speed, "Press a game button for normal speed"};
    const int pad = 8, cols = 42, w = cols * 8 + 2 * pad;
    const int bar_y = pad + 2 * 12 + 4, bar_h = 10, h = bar_y + bar_h + 8 + 8 + pad;
    SDL_Surface *s = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_ARGB8888);
    if (!s) return;
    if (SDL_Renderer *r = SDL_CreateSoftwareRenderer(s)) {
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(r, 0x30, 0x80, 0xe0, 240);
        SDL_RenderClear(r);
        SDL_SetRenderDrawColor(r, 14, 16, 24, 230);
        const SDL_FRect in{2, 2, (float)w - 4, (float)h - 4};
        SDL_RenderFillRect(r, &in);
        auto text = [&](int y, const char *t, uint8_t c) {
            SDL_SetRenderDrawColor(r, c, c, c, 255);
            SDL_RenderDebugText(r, (float)(w - 8 * (int)strlen(t)) / 2, (float)y, t);
        };
        text(pad, lines[0], 255);
        text(pad + 12, lines[1], 210);
        const SDL_FRect frame{(float)pad, (float)bar_y, (float)(w - 2 * pad), (float)bar_h};
        SDL_SetRenderDrawColor(r, 60, 66, 84, 255);
        SDL_RenderFillRect(r, &frame);
        const SDL_FRect fill{(float)pad + 1, (float)bar_y + 1, (float)(w - 2 * pad - 2) * (float)std::clamp(done, 0.0, 1.0),
                             (float)bar_h - 2};
        SDL_SetRenderDrawColor(r, 0x40, 0xa0, 0xff, 255);
        SDL_RenderFillRect(r, &fill);
        if (!ended) text(bar_y + bar_h + 8, lines[2], 160);
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

void end(const char *why) {
    g_on = false;
    g_pressed = false;
    g_ended = true;
    g_tend = Clock::now();
    const double wall = secs(g_t0, g_tend), game = g_vlast - g_v0;
    char line[160];
    snprintf(line, sizeof line, "fast start-up: %.1f s of the game's time in %.1f s (%.1fx), %s", game, wall,
             wall > 0 ? game / wall : 0.0, why);
    g_report = line;
    fprintf(stderr, "%s\n", line);
    draw(1, true);
}

}  // namespace

void set(double until) {
    g_until = until;
    g_pressed = g_started = g_ended = false;
    g_speed = 0;
    g_on = until > 0;
}

bool frame(double vsec) {
    if (!g_on) return false;
    const auto now = Clock::now();
    if (!g_started) {
        g_started = true;
        g_v0 = g_vmark = g_vlast = vsec;
        g_t0 = g_tmark = now;
        if (vsec >= g_until) {   // nothing left of it (a core that starts later on: Firebeat's skipped BIOS)
            g_on = false;
            return false;
        }
        draw(0, false);
    }
    g_vlast = vsec;
    if (g_pressed) {
        end("a button pressed");
        return false;
    }
    if (vsec >= g_until) {
        end("ready");
        return false;
    }
    if (secs(g_tmark, now) >= kRedraw) {
        const double s = (vsec - g_vmark) / secs(g_tmark, now);
        g_speed = g_speed > 0 ? 0.5 * g_speed + 0.5 * s : s;
        g_vmark = vsec;
        g_tmark = now;
        draw((vsec - g_v0) / (g_until - g_v0), false);
    }
    return true;
}

bool panel(Panel &p) {
    if (g_pixels.empty() || (!g_on && !g_ended)) return false;
    float alpha = 1;
    if (g_ended) {
        const double t = secs(g_tend, Clock::now());
        if (t >= kFade) {
            g_pixels.clear();
            return false;
        }
        alpha = (float)(1 - t / kFade);
    }
    p = {&g_pixels, g_w, g_h, alpha, g_id};
    return true;
}

const char *report() { return g_report.c_str(); }

}  // namespace hk::host::warp
