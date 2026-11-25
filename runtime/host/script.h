// SPDX-License-Identifier: GPL-3.0-or-later
// The oracle's input scripts (.his, oracle/README.md), run by the runtime: the
// same file drives MAME (oracle/lua/hiraku.lua) and HIRAKU, keyed by frame.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace hk::script {

// Load a script; returns false (with a message on stderr) on a parse error.
bool load(const std::string &path, const std::string &out_dir);
// Run everything due at this frame (call once per frame, at vertical blank).
// `snap` is called for `snap` actions; returns false when the script says `quit`.
bool frame(uint64_t f, const std::function<void()> &snap);
void close();
// Something happened (any thread): lines `on "NAME" +N action` (or `on "NAME"#K +N`, its K-th time)
// run their action N frames after it. hiraku-pc's: "ea:MODULE.METHOD", each e-AMUSEMENT call
void event(const std::string &name);
// Inputs the core doesn't take (a turntable's or dial's keys, "P1 Turntable +"/"-") go here: the
// window's controls (host/input.cpp) take them as those keys held, at the family's speed
void set_key_input(std::function<bool(const std::string &name, int value)> take);

}  // namespace hk::script
