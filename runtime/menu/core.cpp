// SPDX-License-Identifier: GPL-3.0-or-later
// hiraku-menu: the game menu (menu.h) as a core of the shell. It is given the cabinet's family
// (--family: the game just left's), so the shell binds that cabinet's controls, drives its lamps
// and I/O boards and reads its readers exactly as for that game; what a control does here is
// menu/nav.cpp's. 60 frames a second, a frame's sound with each.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>

#ifdef HK_WITH_SDL
#include <SDL3/SDL.h>
#endif

#include "../shell/cabinet.h"
#include "../shell/core.h"
#include "menu.h"

namespace hk {

namespace {

constexpr int kRate = 44100, kFrameSamples = kRate / 60;
constexpr uint64_t kQuiet = 20;         // frames at the start when presses do nothing (what was held on the way in)
constexpr uint64_t kRepeatAfter = 24, kRepeatEvery = 6;   // a held control repeats its move
constexpr uint64_t kStartFrames = 36;   // from the choice to the end: its flash and its sound
constexpr int kTurnStep = 16;           // turntable counts a move (a sixteenth of a turn)
constexpr uint64_t kTurnEvery = 5;      // ... and at most a move every so many frames

class MenuCore final : public Core {
public:
    bool option(const std::string &name, const std::function<std::string()> &value) override {
        if (name == "--family") family_ = value();
        else if (name == "--list") list_path_ = value();
        else if (name == "--choice") choice_path_ = value();
        else if (name == "--wide") wide_ = true;
        else return false;
        return true;
    }
    std::string options_help() const override {
        return "  --list FILE       the games to choose from (the launcher writes it: menu/menu.h)\n"
               "  --choice FILE     where the chosen game's id is written\n"
               "  --family NAME     the cabinet whose controls drive the menu (iidx, popn, ddr...: a\n"
               "                    game.toml's family; default iidx)\n"
               "  --wide            a 16:9 picture (848x480) instead of 4:3 (640x480)\n";
    }

    bool init(const Options &o) override {
        if (list_path_.empty()) {
            fprintf(stderr, "hiraku-menu: no --list FILE (HIRAKU's launcher starts the game menu with one)\n");
            return false;
        }
        menu::List list;
        std::string error;
        if (!list.load(list_path_, error)) {
            fprintf(stderr, "hiraku-menu: %s\n", error.c_str());
            return false;
        }
        state_.set(list);
        window_ = o.window;
        w_ = wide_ ? 848 : 640;
        frame_px_.assign((size_t)w_ * h_, 0);
        for (const cab::Control &c : cab::controls(family_)) known_.insert(c.name);
        // the lamps of the controls that do something: lit, and blinking where they play
        const std::vector<cab::Lamp> &lamps = cab::lamps(family_);
        lamp_mode_.assign(lamps.size(), 0);
        for (const menu::Rule &r : menu::rules(family_)) {
            const std::string lamp = menu::lamp_for(family_, r.control);
            for (size_t i = 0; i < lamps.size(); i++)
                if (!lamp.empty() && lamps[i].name == lamp) lamp_mode_[i] = (uint8_t)std::max<int>(lamp_mode_[i], r.act == menu::Act::start ? 2 : 1);
        }
        lamps_.assign(lamps.size(), 0);
        // the keyboard's arrows and Enter (and its keypad's 8, 2, 4, 6) work in every family, but
        // for those the family's own controls are on by default
        for (const cab::Control &c : cab::controls(family_))
            for (const cab::Entry &e : cab::parse_list_file(c.name + " = " + c.defaults))
                for (const std::string &b : e.items)
                    if (b.rfind("key:", 0) == 0) family_keys_.insert(b.substr(4));
        printf("hiraku menu: %zu games on %zu shelves, the %s cabinet's controls\n", list.games(), list.shelves.size(), family_.c_str());
        return true;
    }

    const std::string &family() const override { return family_; }
    int monitor_khz() const override { return 31; }
    double display_aspect() const override { return wide_ ? 16.0 / 9.0 : 4.0 / 3.0; }
    double native_rate() const override { return 60.0; }
    void set_frame_hook(FrameHook h) override { hook_ = std::move(h); }
    void set_audio_sink(AudioSink s) override { sink_ = std::move(s); }
    void set_on_fatal(std::function<void()> f) override { on_fatal_ = std::move(f); }
    bool audio_per_frame() const override { return true; }
    int audio_rate() const override { return kRate; }
    void shutdown() override {}
    void report() override { printf("menu: %llu frames\n", (unsigned long long)frame_); }
    double seconds() const override { return (double)frame_ / 60.0; }
    const uint8_t *mem(uint32_t, uint32_t) const override { return nullptr; }
    uint64_t frame_count() const override { return frame_; }
    const std::vector<uint8_t> &lamp_state() const override { return lamps_; }
    uint64_t lamp_changes() const override { return lamp_changes_; }
    std::string display_text() const override { return ticker_; }

    bool set_input(const std::string &name, int value) override {
        const menu::Act a = menu::action(family_, name);
        if (name.find(" Wheel") != std::string::npos && name.find(" Wheel ") == std::string::npos) {   // Keyboardmania's, a position
            wheel_[name] = value;
            return true;
        }
        if (a == menu::Act::none) return known_.count(name) != 0;
        press(name, a, value != 0);
        return true;
    }
    void turntable_add(int, int delta) override {
        if (menu::turntable(family_)) turned_ = std::clamp(turned_ + delta, -3 * kTurnStep, 3 * kTurnStep);
    }

    [[noreturn]] void run() override {
        std::vector<float> sound((size_t)kFrameSamples * 2);
        for (;;) {
            keyboard();
            repeats();
            turns();
            state_.animate();
            menu::draw(state_, family_, frame_, frame_px_.data(), w_, h_);
            lamps();
            mix(sound.data());
            if (sink_) sink_(sound.data(), kFrameSamples);
            if (hook_) hook_(frame_, frame_px_.data(), w_, h_);
            if (const menu::Game *g = state_.chosen(); g && frame_ - state_.chosen_at() >= kStartFrames) leave(*g);
            frame_++;
        }
    }

private:
    // --- the controls ----------------------------------------------------------
    void act(menu::Act a) {
        if (frame_ < kQuiet || !state_.act(a, frame_)) return;
        switch (a) {
        case menu::Act::prev: beep(1320, 0.035, 0.10); break;
        case menu::Act::next: beep(1100, 0.035, 0.10); break;
        case menu::Act::shelf_prev:
        case menu::Act::shelf_next: beep(660, 0.05, 0.12), beep(990, 0.05, 0.12, 0.05); break;
        case menu::Act::start: beep(660, 0.09, 0.14), beep(880, 0.09, 0.14, 0.09), beep(1320, 0.30, 0.14, 0.18); break;
        default: break;
        }
    }
    void press(const std::string &name, menu::Act a, bool down) {
        if (!down) {
            held_.erase(name);
            return;
        }
        if (held_.count(name)) return;
        held_[name] = {a, frame_};
        act(a);
    }
    // a control held on a move goes on moving
    void repeats() {
        for (const auto &[name, h] : held_) {
            if (h.act != menu::Act::prev && h.act != menu::Act::next) continue;
            const uint64_t t = frame_ - h.since;
            if (t >= kRepeatAfter && (t - kRepeatAfter) % kRepeatEvery == 0) act(h.act);
        }
        // Keyboardmania's wheel, pushed up or down from its middle: likewise
        for (const auto &[name, pos] : wheel_) {
            const int d = pos - 128;
            if (std::abs(d) > 40 && frame_ % 8 == 0) act(d > 0 ? menu::Act::prev : menu::Act::next);
        }
    }
    // the turntables: a move for each step turned, no faster than the list can be read
    void turns() {
        if (turn_wait_) turn_wait_--;
        if (turn_wait_ || std::abs(turned_) < kTurnStep) return;
        const int dir = turned_ > 0 ? 1 : -1;
        turned_ -= dir * kTurnStep;
        turn_wait_ = kTurnEvery;
        act(dir > 0 ? menu::Act::next : menu::Act::prev);
    }
    // the keyboard itself, whatever the family binds: arrows and Enter, the keypad's 8, 2, 4, 6
    void keyboard() {
#ifdef HK_WITH_SDL
        if (!window_) return;
        struct Key {
            SDL_Scancode code;
            const char *binding;   // its name in a bindings file: left to the family if it uses it
            menu::Act act;
        };
        static const Key keys[] = {
            {SDL_SCANCODE_UP, "Up", menu::Act::prev},           {SDL_SCANCODE_DOWN, "Down", menu::Act::next},
            {SDL_SCANCODE_LEFT, "Left", menu::Act::shelf_prev}, {SDL_SCANCODE_RIGHT, "Right", menu::Act::shelf_next},
            {SDL_SCANCODE_RETURN, "Return", menu::Act::start},  {SDL_SCANCODE_KP_ENTER, "Keypad Enter", menu::Act::start},
            {SDL_SCANCODE_KP_8, "Keypad 8", menu::Act::prev},   {SDL_SCANCODE_KP_2, "Keypad 2", menu::Act::next},
            {SDL_SCANCODE_KP_4, "Keypad 4", menu::Act::shelf_prev}, {SDL_SCANCODE_KP_6, "Keypad 6", menu::Act::shelf_next},
        };
        int n = 0;
        const bool *down = SDL_GetKeyboardState(&n);
        if (!down) return;
        for (const Key &k : keys)
            if ((int)k.code < n && !family_keys_.count(k.binding)) press(std::string("\x02key ") + k.binding, k.act, down[k.code]);
#endif
    }

    // --- lamps and the ticker ----------------------------------------------------
    void lamps() {
        const bool blink = frame_ / 20 % 2 == 0;
        bool changed = false;
        for (size_t i = 0; i < lamps_.size(); i++) {
            const uint8_t lit = lamp_mode_[i] == 1 || (lamp_mode_[i] == 2 && blink);
            changed |= lit != lamps_[i];
            lamps_[i] = lit;
        }
        // the chosen game's name on a cabinet's text display, nine characters at a time
        std::string text;
        if (const menu::Game *g = state_.selected()) {
            text = g->title;
            if (text.size() > 9) {
                const std::string loop = text + "   ";
                const size_t at = (size_t)((frame_ - state_.moved_at()) / 12) % loop.size();
                text = (loop + loop).substr(at, 9);
            }
            for (char &ch : text) ch = (char)toupper((unsigned char)ch);
        }
        changed |= text != ticker_;
        ticker_ = text;
        if (changed) lamp_changes_++;
    }

    // --- sound: short tones, made here (nothing recorded) -----------------------
    struct Tone {
        double hz, phase = 0;
        int left, total;
        float gain;
        int wait;   // samples before it starts
    };
    void beep(double hz, double seconds, float gain, double after = 0) {
        const int n = (int)(seconds * kRate);
        tones_.push_back({hz, 0, n, n, gain, (int)(after * kRate)});
    }
    void mix(float *lr) {
        for (int i = 0; i < kFrameSamples; i++) {
            float v = 0;
            for (Tone &t : tones_) {
                if (t.wait > 0) {
                    t.wait--;
                    continue;
                }
                if (t.left <= 0) continue;
                const double env = std::min(1.0, (double)(t.total - t.left) / 60.0) * std::min(1.0, (double)t.left / (t.total * 0.6));
                v += t.gain * (float)(env * std::sin(t.phase));
                t.phase += 6.283185307179586 * t.hz / kRate;
                t.left--;
            }
            lr[2 * i] = lr[2 * i + 1] = v;
        }
        std::erase_if(tones_, [](const Tone &t) { return t.wait <= 0 && t.left <= 0; });
    }

    // --- the way out: the choice said, then the program's end ------------------
    [[noreturn]] void leave(const menu::Game &g) {
        printf("menu: chosen %s\n", g.id.c_str());
        if (!choice_path_.empty()) {
            if (FILE *f = fopen(choice_path_.c_str(), "w")) {
                fprintf(f, "%s\n", g.id.c_str());
                fclose(f);
            } else {
                fprintf(stderr, "hiraku-menu: cannot write %s\n", choice_path_.c_str());
            }
        }
        if (on_fatal_) on_fatal_();
        exit(0);
    }

    std::string family_ = "iidx", list_path_, choice_path_;
    bool wide_ = false, window_ = false;
    int w_ = 640, h_ = 480;
    menu::State state_;
    std::vector<uint32_t> frame_px_;
    uint64_t frame_ = 0;
    FrameHook hook_;
    AudioSink sink_;
    std::function<void()> on_fatal_;
    std::set<std::string> known_, family_keys_;
    struct Held {
        menu::Act act;
        uint64_t since;
    };
    std::map<std::string, Held> held_;
    std::map<std::string, int> wheel_;
    int turned_ = 0;
    uint64_t turn_wait_ = 0;
    std::vector<uint8_t> lamps_, lamp_mode_;
    uint64_t lamp_changes_ = 0;
    std::string ticker_;
    std::vector<Tone> tones_;
};

}  // namespace

Core &core() {
    static MenuCore c;
    return c;
}

}  // namespace hk
