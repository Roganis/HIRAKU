// SPDX-License-Identifier: GPL-3.0-or-later
// Host controls (runtime/host/input.h) with a controller SDL makes up: an input bound to a key and
// to a hat direction or an axis's, held by one, isn't let go by the other; a controller unplugged
// and plugged back in is the same joy<N>, and what it held is let go; a controller left out of the
// bindings (the game menu's plain gamepad) presses nothing. IIDX's effector sliders stepped by
// keys: a press a step from where the slider is, held more after a moment, kept within 0-15. A
// turntable on an axis: a wrap of the axis is a turn (1024 counts), a step of an 8-bit controller's
// one of the game's (4 counts), each across the wrap too (255 to 0, a single unit of SDL's), either
// way and at another speed, and what's under a count isn't lost. A 16-bit axis is read as before,
// once a value of its isn't one an 8-bit axis gives (host/axisturn.h).
//
//   test_bindings      exit 0 when all hold (0 with a note where SDL has no joysticks to make up)
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "host/axisturn.h"
#include "host/input.h"
#include "shell/core.h"

namespace {

// a machine that keeps what its inputs were last set to
class Machine final : public hk::Core {
public:
    std::map<std::string, int> inputs;
    int turned[2] = {0, 0};
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
    int input_position(const std::string &name) const override {
        if (name.rfind("Volume ", 0) != 0) return -1;
        auto it = inputs.find(name);
        return it == inputs.end() ? 8 : it->second;   // (as Twinkle's: in the middle at the start)
    }
    void turntable_add(int p, int d) override { turned[p] += d; }
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
    d.naxes = 4, d.nbuttons = 4, d.nhats = 1;
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
                           "P1 Button 4 = joy0:axis-1+, joy0:button-3\n"   // (refused: no such axis or button)
                           "P1 Turntable = joy0:axis0, joy0:axis2*0.75\n"
                           "P2 Turntable = joy0:axis1*0.3\n"
                           "Volume 2 Up = key:U\n"
                           "Volume 2 Down = key:J\n";
    hk::host::input_init(file);
    auto &in = g_machine.inputs;

    // an effector slider on two keys: a step a press, from where it is
    key(SDLK_U, true);
    hk::host::input_update(1);
    check(in["Volume 2"] == 9 && !in.count("Volume 2 Up"), "the slider's up key: a step up from the middle");
    hk::host::input_update(250);
    check(in["Volume 2"] == 9, "held a quarter of a second: no more yet");
    hk::host::input_update(60);
    check(in["Volume 2"] == 10, "held past 0.3 s: another step");
    hk::host::input_update(160);
    check(in["Volume 2"] == 12, "... and one each 0.08 s after");
    key(SDLK_U, false);
    hk::host::input_update(500);
    check(in["Volume 2"] == 12, "let go: it stays");
    key(SDLK_J, true);
    hk::host::input_update(1);
    check(in["Volume 2"] == 11, "the down key: a step down");
    key(SDLK_J, false);
    hk::host::input_update(1);
    in["Volume 2"] = 15;   // (moved by something else: an axis, a cabinet board)
    key(SDLK_U, true);
    hk::host::input_update(1);
    check(in["Volume 2"] == 15, "at the top: stays there");
    key(SDLK_U, false);
    hk::host::input_update(1);
    key(SDLK_J, true);
    hk::host::input_update(1);
    hk::host::input_update(5000);
    check(in["Volume 2"] == 0, "held down long: at the bottom, no further");
    key(SDLK_J, false);
    hk::host::input_update(1);

    // the rest with a controller SDL makes up (opened as one plugged in after the start)
    SDL_JoystickID pad = plug();
    if (!pad) {
        printf("SDL can't make a controller up here (%s): the rest not run\n", SDL_GetError());
        std::remove(file.c_str());
        SDL_Quit();
        printf(failed ? "%d failed\n" : "all hold\n", failed);
        return failed ? 1 : 0;
    }
    hk::host::input_joystick_added(pad);

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

    // a controller someone else reads (the game menu's plain gamepad): the bindings leave it alone
    check(hk::host::input_joystick_in_file(pad), "the bindings file names joy0: it is the cabinet's");
    {
        const SDL_JoystickID other = plug();
        hk::host::input_joystick_added(other);
        check(other && !hk::host::input_joystick_in_file(other), "a second controller the file doesn't name: nobody's");
        SDL_DetachVirtualJoystick(other);
        hk::host::input_joystick_removed(other);
    }
    button(pad, 2, true);
    hk::host::input_joystick_leave(pad, true);
    check(in["P1 Button 3"] == 0, "left out with a button held: released");
    button(pad, 2, false);
    button(pad, 2, true);
    hat(pad, SDL_HAT_LEFT);
    check(in["P1 Button 3"] == 0 && in["P1 Button 1"] == 0, "... and what it does is nobody's press");
    button(pad, 2, false);
    hat(pad, SDL_HAT_CENTERED);
    hk::host::input_joystick_leave(pad, false);
    button(pad, 2, true);
    check(in["P1 Button 3"] == 1, "taken back: its button pressed again");
    button(pad, 2, false);

    // a turntable on an axis, as an IIDX controller's 8-bit position (SDL: 257 units a step)
    {
        SDL_Joystick *js = SDL_OpenJoystick(pad);
        auto at = [&](int step) {
            for (int a : {0, 1}) SDL_SetJoystickVirtualAxis(js, a, (Sint16)(-32768 + (step & 255) * 257));
            SDL_UpdateJoysticks();
            hk::host::input_update(1);
        };
        SDL_SetJoystickVirtualAxis(js, 2, -32768);   // (P1's second axis stays at an 8-bit step meanwhile)
        at(0);   // (from where the tests before left it: a whole number of counts away)
        g_machine.turned[0] = g_machine.turned[1] = 0;
        at(1);
        check(g_machine.turned[0] == 4, "a step of the controller's: a step of the game's (4 counts)");
        at(0);
        check(g_machine.turned[0] == 0, "a step back: one back, not four");
        g_machine.turned[1] = 0;
        for (int k = 1; k <= 256; k++) at(k);
        check(g_machine.turned[0] == 1024, "a whole wrap of the axis, past its end: one turn (1024 counts)");
        check(g_machine.turned[1] >= 306 && g_machine.turned[1] <= 308, "at x0.3: 0.3 of a turn, nothing lost under a count");

        // slowly through the wrap, a step a poll: each one 4 counts, the one from 255 to 0 too
        bool each = true;
        at(250);
        for (int k = 251; k <= 260; k++) {
            const int was = g_machine.turned[0];
            at(k);
            each &= g_machine.turned[0] - was == 4;
        }
        check(each, "a step a poll through the wrap (254, 255, 0, 1): 4 counts each");
        each = true;
        for (int k = 259; k >= 250; k--) {
            const int was = g_machine.turned[0];
            at(k);
            each &= g_machine.turned[0] - was == -4;
        }
        check(each, "... and back through it (1, 0, 255, 254): -4 each");
        // at x0.75 (P1's second axis): 3 counts a step, across the wrap as anywhere
        g_machine.turned[0] = 0;
        each = true;
        for (int k = 250; k <= 262; k++) {
            const int was = g_machine.turned[0];
            SDL_SetJoystickVirtualAxis(js, 2, (Sint16)(-32768 + (k & 255) * 257));
            SDL_UpdateJoysticks();
            hk::host::input_update(1);
            each &= g_machine.turned[0] - was == (k == 250 ? -18 : 3);   // (first from 0 to 250: 6 steps back)
        }
        check(each, "at x0.75: 3 counts a step, through the wrap too");
        SDL_CloseJoystick(js);
    }

    // the reading itself (host/axisturn.h): 65536ths of a turn
    {
        hk::host::AxisTurn t;
        check(t.turn(-32768 + 254 * 257) == 0, "the first value: where the axis is, no movement");
        check(t.turn(-32768 + 255 * 257) == 256 && t.turn(-32768) == 256 && t.turn(-32768 + 257) == 256,
              "8-bit: 254, 255, 0, 1: a 256th of a turn each");
        check(t.turn(-32768) == -256 && t.turn(32767) == -256, "... and back");
        int sum = 0;
        for (int k = 1; k <= 512; k++) sum += t.turn((Sint16)(-32768 + ((255 + k) & 255) * 257));
        check(sum == 2 * 65536, "two whole 8-bit turns: two turns exactly");

        // a 16-bit axis, its first value not an 8-bit one: as before (the difference, wrapped)
        auto before = [](int from, int to) {
            int d = to - from;
            if (d > 32768) d -= 65536;
            if (d < -32768) d += 65536;
            return d;
        };
        const int wide[] = {1000, 1500, 30000, -30000, -32768, 32767, 32000, -100, 12850 - 32768, -32768 + 257 * 3, 5};
        hk::host::AxisTurn w;
        bool same = w.turn((Sint16)wide[0]) == 0;
        for (size_t k = 1; k < std::size(wide); k++) same &= w.turn((Sint16)wide[k]) == before(wide[k - 1], wide[k]);
        check(same, "16-bit: each movement as before, across its wrap too, values of 257 steps among them");

        // ... and one whose first values happen to be 8-bit ones: read as 8-bit until the first that
        // isn't (the same scale within 0.4 %), then as before, with no jump at the change
        hk::host::AxisTurn e;
        const int early[] = {-32768 + 257 * 10, -32768 + 257 * 12, -32768 + 257 * 11};
        bool ok = e.turn((Sint16)early[0]) == 0;
        ok &= e.turn((Sint16)early[1]) == 512 && e.turn((Sint16)early[2]) == -256;
        const int later[] = {-32768 + 257 * 11 + 100, 0, 32767, -32768, -32000};
        int from = early[2];
        for (int v : later) ok &= e.turn((Sint16)v) == before(from, v), from = v;
        ok &= e.turn((Sint16)(-32768 + 257 * 20)) == before(from, -32768 + 257 * 20);   // (16 bits for good)
        check(ok, "16-bit, its first values 8-bit ones: as 8-bit until then, as before after, no jump");
    }

    std::remove(file.c_str());
    SDL_Quit();
    printf(failed ? "%d failed\n" : "all hold\n", failed);
    return failed ? 1 : 0;
}
