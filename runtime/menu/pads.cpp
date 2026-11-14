// SPDX-License-Identifier: GPL-3.0-or-later
#include "pads.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>

namespace hk::menu {

namespace {

struct Control {
    const char *name;
    Act act;
};
constexpr Control kControls[] = {{"up", Act::prev}, {"down", Act::next}, {"left", Act::shelf_prev}, {"right", Act::shelf_next}, {"play", Act::start}};
constexpr int kStick = 16000;   // half way out

unsigned read(SDL_Gamepad *p) {
    const int x = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTX), y = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTY);
    unsigned bits = 0;
    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_UP) || y < -kStick) bits |= 1;
    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_DOWN) || y > kStick) bits |= 2;
    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_LEFT) || x < -kStick) bits |= 4;
    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || x > kStick) bits |= 8;
    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_SOUTH) || SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_START)) bits |= 16;
    return bits;
}

}  // namespace

Pads::~Pads() {
    for (Pad &p : pads_) SDL_CloseGamepad(p.pad);
}

void Pads::set(std::function<bool(uint32_t)> skip, std::function<void(uint32_t)> taken) { skip_ = std::move(skip), taken_ = std::move(taken); }

// the pads plugged in now: new ones opened, gone ones closed
void Pads::scan() {
    std::erase_if(pads_, [](Pad &p) {
        if (SDL_GamepadConnected(p.pad)) return false;
        SDL_CloseGamepad(p.pad);
        return true;
    });
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    for (int k = 0; ids && k < n; k++) {
        const uint32_t id = ids[k];
        if (std::any_of(pads_.begin(), pads_.end(), [id](const Pad &p) { return p.id == id; }) || (skip_ && skip_(id))) continue;
        if (SDL_Gamepad *pad = SDL_OpenGamepad(id)) {
            pads_.push_back({id, pad, read(pad), 0});
            if (taken_) taken_(id);
            const char *name = SDL_GetGamepadName(pad);
            printf("menu: gamepad %s: its d-pad or stick chooses, A or Start plays\n", name ? name : "(no name)");
        }
    }
    SDL_free(ids);
}

bool Pads::poll(const std::function<void(const std::string &, Act, bool)> &press) {
    if (!started_) {
        started_ = true;
        SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    }
    if (polls_++ % 60 == 0) scan();
    bool pressed = false;
    for (Pad &p : pads_) {
        const unsigned now = read(p.pad);
        p.rest &= now;   // let go: a control like any other from now on
        const unsigned down = now & ~p.rest;
        pressed |= (down & ~p.down) != 0;
        p.down = down;
        for (int k = 0; k < 5; k++) press("\x03pad" + std::to_string(p.id) + " " + kControls[k].name, kControls[k].act, down >> k & 1);
    }
    return pressed;
}

}  // namespace hk::menu
