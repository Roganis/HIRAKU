// SPDX-License-Identifier: GPL-3.0-or-later
#include "menuview.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstring>

#include "menukey.h"

namespace hk::host::menuview {

namespace {

std::vector<uint32_t> g_pixels;
int g_w = 0, g_h = 0, g_step = -1;
unsigned g_id = 0;
constexpr int kSteps = 64;   // the bar's steps: a redraw each

// the box: its lines in SDL's debug font and the bar, on a dark box with an amber edge (as
// host/warp.h's is drawn)
void draw(int step) {
    const char *lines[] = {"GAME MENU", "Keep holding to leave this game"};
    const int pad = 8, cols = 34, w = cols * 8 + 2 * pad;
    const int bar_y = pad + 2 * 12 + 4, bar_h = 10, h = bar_y + bar_h + pad;
    SDL_Surface *s = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_ARGB8888);
    if (!s) return;
    if (SDL_Renderer *r = SDL_CreateSoftwareRenderer(s)) {
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(r, 0xf0, 0xa0, 0x20, 240);
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
        const SDL_FRect fill{(float)pad + 1, (float)bar_y + 1, (float)(w - 2 * pad - 2) * (float)step / kSteps, (float)bar_h - 2};
        SDL_SetRenderDrawColor(r, 0xff, 0xb8, 0x30, 255);
        SDL_RenderFillRect(r, &fill);
        SDL_RenderPresent(r);
        SDL_DestroyRenderer(r);
        g_pixels.resize((size_t)w * h);
        for (int y = 0; y < h; y++)
            memcpy(&g_pixels[(size_t)y * w], (const uint8_t *)s->pixels + (size_t)y * s->pitch, (size_t)w * 4);
        g_w = w;
        g_h = h;
        g_step = step;
        g_id++;
    }
    SDL_DestroySurface(s);
}

}  // namespace

bool panel(Panel &p) {
    const double done = menukey::progress(menukey::now());
    if (done <= 0) return false;
    const int step = std::clamp((int)(done * kSteps), 0, kSteps);
    if (step != g_step || g_pixels.empty()) draw(step);
    if (g_pixels.empty()) return false;
    p = {&g_pixels, g_w, g_h, g_id};
    return true;
}

}  // namespace hk::host::menuview
