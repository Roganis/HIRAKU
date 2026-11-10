// SPDX-License-Identifier: GPL-3.0-or-later
// Host controls (runtime/host/input.h) with a controller SDL makes up: an input bound to a key and
// to a hat direction or an axis's, held by one, isn't let go by the other; a controller unplugged
// and plugged back in is the same joy<N>, and what it held is let go.
//
//   test_bindings      exit 0 when all hold (0 with a note where SDL has no joysticks to make up)
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "host/input.h"
#include "shell/core.h"

namespace {

// a machine that keeps what its inputs were last set to
class Machine final : public hk::Core {
public:
    std::map<std::string, int> inputs;
    bool init(const Options &) override { return true; }
    const std::string &family() const override { static const std::string f = "popn"; return f; }
    void set_frame_hook(FrameHook) override {}
    void set_audio_sink(AudioSink) override {}
    void set_on_fatal(std::function<void()>) override {}
    [[noreturn]] void run() override { abort(); }
    void shutdown() override {}
    void report() override {}
    double seconds() const override { return 0; }
    const uint8_t *mem(uint32_t, uint32_t) const override { return nullptr; }
    uint64_t frame_count() const override { return 0; }
    bool set_input(const std::string &name, int value) override { inputs[name] = value; return true; }
    void turntable_add(int, int) override {}
    const std::vector<uint8_t> &lamp_state() const override { static const std::vector<uint8_t> none; return none; }
    uint64_t lamp_changes() const override { return 0; }
};
Machine g_machine;

int failed = 0;
void check(bool ok, const char *what) {
    printf("%s%s\n", ok ? "ok    " : "FAIL  ", what);
    failed += !ok;
}

SDL_JoystickID plug() {
    SDL_VirtualJoystickDesc d;
    SDL_INIT_INTERFACE(&d);
    d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    d.naxes = 2, d.nbuttons = 4, d.nhats = 1;
    d.name = "HIRAKU test pad";
    return SDL_AttachVirtualJoystick(&d);
}

void key(SDL_Keycode k, bool down) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.key = k;
    hk::host::input_event(e);
}
void hat(SDL_JoystickID id, Uint8 value) {
    SDL_Event e{};
    e.type = SDL_EVENT_JOYSTICK_HAT_MOTION;
    e.jhat.which = id, e.jhat.hat = 0, e.jhat.value = value;
    hk::host::input_event(e);
}
void axis(SDL_JoystickID id, int a, Sint16 value) {
    SDL_Event e{};
    e.type = SDL_EVENT_JOYSTICK_AXIS_MOTION;
    e.jaxis.which = id, e.jaxis.axis = (Uint8)a, e.jaxis.value = value;
    hk::host::input_event(e);
}
void button(SDL_JoystickID id, int b, bool down) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_JOYSTICK_BUTTON_DOWN : SDL_EVENT_JOYSTICK_BUTTON_UP;
    e.jbutton.which = id, e.jbutton.button = (Uint8)b, e.jbutton.down = down;
    hk::host::input_event(e);
}

}  // namespace

hk::Core &hk::core() { return g_machine; }

int main() {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    if (!SDL_Init(SDL_INIT_JOYSTICK)) {
        printf("no SDL joysticks here (%s): not run\n", SDL_GetError());
        return 0;
    }
    const std::string file = (std::string(SDL_GetBasePath() ? SDL_GetBasePath() : "./")) + "test_bindings.txt";
    std::ofstream(file) << "P1 Button 1 = key:Z, joy0:hat0:left\n"
                           "P1 Button 2 = key:X, joy0:axis1+\n"
                           "P1 Button 3 = joy0:button2\n"
                           "P1 Button 4 = joy0:axis-1+, joy0:button-3\n";   // (refused: no such axis or button)
    SDL_JoystickID pad = plug();
    if (!pad) {
        printf("SDL can't make a controller up here (%s): not run\n", SDL_GetError());
        return 0;
    }
    hk::host::input_init(file);
    auto &in = g_machine.inputs;

    // a key and a hat direction on one input
    key(SDLK_Z, true);
    check(in["P1 Button 1"] == 1, "a key held: pressed");
    hat(pad, SDL_HAT_UP);
    check(in["P1 Button 1"] == 1, "the hat moved another way, the key still held: still pressed");
    hat(pad, SDL_HAT_LEFT);
    hat(pad, SDL_HAT_CENTERED);
    check(in["P1 Button 1"] == 1, "the hat pressed that way and let go, the key still held: still pressed");
    key(SDLK_Z, false);
    check(in["P1 Button 1"] == 0, "the key let go: released");
    hat(pad, SDL_HAT_LEFT);
    check(in["P1 Button 1"] == 1, "the hat alone: pressed");
    hat(pad, SDL_HAT_CENTERED);
    check(in["P1 Button 1"] == 0, "the hat let go: released");

    // a key and an axis direction on one input
    axis(pad, 1, 0);   // (its first report: at rest in the middle)
    key(SDLK_X, true);
    axis(pad, 1, 300);
    axis(pad, 1, -200);
    check(in["P1 Button 2"] == 1, "an axis's noise around the middle, a key held: still pressed");
    key(SDLK_X, false);
    axis(pad, 1, 30000);
    check(in["P1 Button 2"] == 1, "the axis pushed: pressed");
    axis(pad, 1, 0);
    check(in["P1 Button 2"] == 0, "the axis back: released");

    // unplugged with a button held, and back
    button(pad, 2, true);
    check(in["P1 Button 3"] == 1, "a button: pressed");
    SDL_DetachVirtualJoystick(pad);
    hk::host::input_joystick_removed(pad);
    check(in["P1 Button 3"] == 0, "the controller unplugged with it held: released");
    pad = plug();
    hk::host::input_joystick_added(pad);
    button(pad, 2, true);
    check(in["P1 Button 3"] == 1, "plugged back in: joy0 again, its button pressed");
    button(pad, 2, false);
    check(in["P1 Button 3"] == 0, "... and released");

    std::remove(file.c_str());
    SDL_Quit();
    printf(failed ? "%d failed\n" : "all hold\n", failed);
    return failed ? 1 : 0;
}
