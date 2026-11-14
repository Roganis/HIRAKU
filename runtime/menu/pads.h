// SPDX-License-Identifier: GPL-3.0-or-later
// A plain gamepad in the game menu: a controller that is nobody's cabinet controls (the family's
// bindings file doesn't name it) and that SDL knows the layout of. The menu reads it as what it is,
// whatever the family's default bindings would make of its buttons: the d-pad or the left stick
// chooses (up, down) and changes shelf (left, right), A or Start plays.
//
// What a pad holds when it is first seen does nothing until it has been let go (a PlayStation
// pop'n controller through an adapter holds three directions for good).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "menu.h"

struct SDL_Gamepad;

namespace hk::menu {

// The family name of a gamepad's own legend (legend("gamepad"))
inline constexpr const char *kPadFamily = "gamepad";

class Pads {
public:
    ~Pads();
    // skip(id): this controller is the cabinet's (left to the bindings). taken(id): the menu reads
    // it from now on, so the bindings must not (host/input.h, input_joystick_leave)
    void set(std::function<bool(uint32_t)> skip, std::function<void(uint32_t)> taken);
    // Once a frame: each control's state as press(name, act, down), down or not, every time (the
    // caller keeps what is held). True if a pad's control went down in this call.
    bool poll(const std::function<void(const std::string &, Act, bool)> &press);
    size_t count() const { return pads_.size(); }

private:
    struct Pad {
        uint32_t id;
        SDL_Gamepad *pad;
        unsigned rest;   // held when first seen: nothing until let go
        unsigned down;
    };
    void scan();
    std::vector<Pad> pads_;
    std::function<bool(uint32_t)> skip_;
    std::function<void(uint32_t)> taken_;
    uint64_t polls_ = 0;
    bool started_ = false;
};

}  // namespace hk::menu
