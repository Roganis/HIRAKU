// SPDX-License-Identifier: GPL-3.0-or-later
// Host controls: keyboard and joysticks (IIDX-style controllers present as
// plain joysticks) bound to the cabinet's inputs by name.
//
// Bindings file (one per line, '#' comments):
//     <input> = <binding>[, <binding>...]
//   input:    a MAME input name ("P1 Button 1", "1 Player Start", "Coin 1",
//             "Test", "Service", "P1 Foot", "Effect 3"), or a turntable:
//             "P1 Turntable" (an analog axis), "P1 Turntable -" / "+" (buttons)
//   binding:  key:<SDL key name>        e.g. key:Z, key:Left Shift, key:F2
//             <pad>:button<K>           a controller's button K
//             <pad>:axis<K>             analog axis (turntables)
//             <pad>:axis<K>*<S>         the same, turning S times as fast (negative: reversed)
//             <pad>:axis<K>-, +         an axis direction used as a button
//             <pad>:hat<K>:<up|down|left|right>
//   pad:      joy<N>                    controller N (0-based), in the order they were found
//             joy[<name>]               the controller of that name (cab::pad_name: "DAO FPS"),
//                                       wherever it is plugged in; "<name>@2" a second one
// Defaults for the game's family are built in (runtime/shell/cabinet.cpp); a
// file only needs the changes.
#pragma once

#include <chrono>
#include <string>

union SDL_Event;

namespace hk::host {

void input_init(const std::string &bindings_file);   // "" = defaults only
void input_event(const SDL_Event &e);
// One binding (a bindings file's spelling: "key:F8", "joy[DAO FPS]:button3", "joy0:hat0 up",
// "joy0:axis1+") against an event: 1 it's pressed (a key's auto-repeat too), -1 released, 0 not this
// binding (host/shaderview.h's controls)
int input_binding_event(const std::string &binding, const SDL_Event &e);
void input_joystick_added(unsigned int instance_id);   // hot-plugged controller
void input_joystick_removed(unsigned int instance_id); // unplugged
void input_update(double elapsed_ms);                 // turntable keys, analog axes
std::string input_describe();                         // the active bindings, for --help
// When the input the core is being given (Core::set_input) happened, on the steady clock: the
// controller's or keyboard's own event time, so a press between two frames keeps its place
// (hiraku-pc's --press-time judges by it); now for inputs that come without one
std::chrono::steady_clock::time_point input_time();
// A core has asked for one: its inputs count by their times, so the window looks for new ones
// often while it waits (four times a millisecond), which is where a polled controller's time comes from
bool input_times_used();

}  // namespace hk::host
