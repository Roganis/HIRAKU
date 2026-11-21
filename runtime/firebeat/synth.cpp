// SPDX-License-Identifier: GPL-3.0-or-later
#include "synth.h"

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <vector>

#include "dev/xt446.h"

#ifdef HK_WITH_FLUIDSYNTH
#include <fluidsynth.h>
#endif

namespace hk::synth {

namespace {
std::unique_ptr<Xt446> g_card;   // the real card, when its ROMs are there

// The card on a thread of its own, so that it runs beside the game's CPU on
// another core. The board's commands (render n more samples, a MIDI byte) reach
// it in the same order as before, so every byte lands on the same sample and
// the output is bit-identical. The game never reads anything back from the
// card; the board collects the samples once a frame (collect()).
class Worker {
public:
    explicit Worker(Xt446 &card) : card_(card), th_([this] { loop(); }) {}
    ~Worker() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            stop_ = true;
        }
        cv_.notify_one();
        th_.join();
    }
    void post(bool midi, uint32_t v) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            q_.push_back({midi, v});
            if (!midi) requested_ += v;
        }
        cv_.notify_one();
    }
    // every sample asked for so far, interleaved stereo, in order
    void wait_all(std::vector<float> &out) {
        std::unique_lock<std::mutex> lk(mu_);
        done_cv_.wait(lk, [&] { return q_.empty() && !busy_; });
        out.swap(out_);
        out_.clear();
    }
    // the samples made so far, added to `out` (no wait)
    void take_ready(std::vector<float> &out) {
        std::lock_guard<std::mutex> lk(mu_);
        out.insert(out.end(), out_.begin(), out_.end());
        out_.clear();
    }
    uint64_t requested() const { return requested_; }

private:
    struct Cmd { bool midi; uint32_t v; };
    void loop() {
        std::vector<float> buf;
        std::unique_lock<std::mutex> lk(mu_);
        for (;;) {
            cv_.wait(lk, [&] { return stop_ || !q_.empty(); });
            if (q_.empty()) return;
            const Cmd c = q_.front();
            q_.pop_front();
            busy_ = true;
            lk.unlock();
            if (c.midi) card_.midi((uint8_t)c.v);
            else {
                buf.resize(2 * (size_t)c.v);
                card_.take(buf.data(), (int)c.v);
            }
            lk.lock();
            if (!c.midi) out_.insert(out_.end(), buf.begin(), buf.end());
            busy_ = false;
            if (q_.empty()) done_cv_.notify_all();
        }
    }
    Xt446 &card_;
    std::mutex mu_;
    std::condition_variable cv_, done_cv_;
    std::deque<Cmd> q_;
    std::vector<float> out_;
    uint64_t requested_ = 0;
    bool stop_ = false, busy_ = false;
    std::thread th_;   // last: starts once the rest is set up
};
std::unique_ptr<Worker> g_worker;
std::vector<float> g_collected;
std::vector<float> g_ready;   // made, taken by collect_ready, not yet added (past the board's n)
}

namespace fluid {
#ifdef HK_WITH_FLUIDSYNTH
namespace {

fluid_settings_t *g_settings = nullptr;
fluid_synth_t *g_synth = nullptr;
std::string g_font;
std::vector<float> g_l, g_r;

// MIDI parser: running status, channel messages, SysEx
uint8_t g_status = 0;
uint8_t g_data[2];
int g_ndata = 0;
std::vector<uint8_t> g_sysex;
bool g_in_sysex = false;

int data_bytes(uint8_t status) {
    switch (status & 0xf0) {
    case 0xc0: case 0xd0: return 1;   // program change, channel pressure
    default: return 2;
    }
}

void channel_message(uint8_t st, uint8_t d0, uint8_t d1) {
    int ch = st & 0x0f;
    switch (st & 0xf0) {
    case 0x80: fluid_synth_noteoff(g_synth, ch, d0); break;
    case 0x90: if (d1) fluid_synth_noteon(g_synth, ch, d0, d1); else fluid_synth_noteoff(g_synth, ch, d0); break;
    case 0xa0: fluid_synth_key_pressure(g_synth, ch, d0, d1); break;
    case 0xb0:
        // The game speaks XG: bank LSB picks voice variations, MSB 127 a drum kit.
        // A GM SoundFont has bank 0 and percussion bank 128 only: map to those.
        if (d0 == 0x00) {
            bool drums = d1 == 127;
            fluid_synth_set_channel_type(g_synth, ch, drums ? CHANNEL_TYPE_DRUM : CHANNEL_TYPE_MELODIC);
            fluid_synth_bank_select(g_synth, ch, drums ? 128 : 0);
        } else if (d0 != 0x20) {
            fluid_synth_cc(g_synth, ch, d0, d1);
        }
        break;
    case 0xc0: fluid_synth_program_change(g_synth, ch, d0); break;
    case 0xd0: fluid_synth_channel_pressure(g_synth, ch, d0); break;
    case 0xe0: fluid_synth_pitch_bend(g_synth, ch, d0 | d1 << 7); break;
    }
}

std::string find_font(const std::string &path, const std::string &user_dir) {
    if (!path.empty()) return path;
    if (const char *e = getenv("HK_SOUNDFONT"); e && *e) return e;
    std::vector<std::string> dirs = {"/usr/share/soundfonts", "/usr/share/sounds/sf2", user_dir};
    for (auto &d : dirs) {
        std::error_code ec;
        for (auto &f : std::filesystem::directory_iterator(d, ec))
            if (f.path().extension() == ".sf2" || f.path().extension() == ".SF2") return f.path().string();
    }
    return "";
}

}  // namespace

bool init(const std::string &path, const std::string &user_dir) {
    g_font = find_font(path, user_dir);
    if (g_font.empty()) {
        fprintf(stderr, "synth: no SoundFont (install one, e.g. soundfont-fluid, or pass --soundfont FILE): keyboard notes are silent\n");
        return false;
    }
    // HIRAKU only renders samples: register no audio drivers, so FluidSynth doesn't
    // probe ALSA/SDL/... at startup (that printed a screenful of ALSA errors)
    static const char *no_drivers[] = {nullptr};
    fluid_audio_driver_register(no_drivers);
    g_settings = new_fluid_settings();
    fluid_settings_setnum(g_settings, "synth.sample-rate", 44100.0);
    fluid_settings_setint(g_settings, "synth.polyphony", 128);
    double gain = 0.25;                     // sits under the CD/YMZ mix (0.6 clipped it); HK_SYNTH_GAIN overrides
    if (const char *e = getenv("HK_SYNTH_GAIN"); e && *e) gain = atof(e);
    fluid_settings_setnum(g_settings, "synth.gain", gain);
    g_synth = new_fluid_synth(g_settings);
    if (fluid_synth_sfload(g_synth, g_font.c_str(), 1) == FLUID_FAILED) {
        fprintf(stderr, "synth: cannot load SoundFont %s\n", g_font.c_str());
        delete_fluid_synth(g_synth);
        g_synth = nullptr;
        return false;
    }
    fprintf(stderr, "synth: XT446 stand-in, FluidSynth with %s\n", g_font.c_str());
    return true;
}

bool active() { return g_synth != nullptr; }

void midi(uint8_t b) {
    if (!g_synth) return;
    if (b >= 0xf8) return;                          // real-time (clock, active sense): no state
    if (b == 0xf0) { g_in_sysex = true; g_sysex.clear(); return; }
    if (g_in_sysex) {
        if (b == 0xf7) {
            g_in_sysex = false;
            if (!g_sysex.empty()) fluid_synth_sysex(g_synth, (const char *)g_sysex.data(), (int)g_sysex.size(), nullptr, nullptr, nullptr, 0);
        } else if (b < 0x80) {
            g_sysex.push_back(b);
        } else {
            g_in_sysex = false;                     // aborted by a status byte: fall through
        }
        if (b < 0x80 || b == 0xf7) return;
    }
    if (b & 0x80) {
        if (b >= 0xf0) { g_status = 0; return; }    // system common: clears running status
        g_status = b;
        g_ndata = 0;
        return;
    }
    if (!g_status) return;
    g_data[g_ndata++] = b;
    if (g_ndata == data_bytes(g_status)) {
        channel_message(g_status, g_data[0], g_ndata > 1 ? g_data[1] : 0);
        g_ndata = 0;                                // running status: same status, new data
    }
}

void render(float *lr, int frames, float gain) {
    if (!g_synth || frames <= 0) return;
    g_l.assign((size_t)frames, 0.0f);
    g_r.assign((size_t)frames, 0.0f);
    fluid_synth_write_float(g_synth, frames, g_l.data(), 0, 1, g_r.data(), 0, 1);
    for (int k = 0; k < frames; k++) {
        lr[2 * k] += g_l[k] * gain;
        lr[2 * k + 1] += g_r[k] * gain;
    }
}

std::string describe() { return g_synth ? "FluidSynth, " + g_font : "silent"; }

#else

bool init(const std::string &, const std::string &) {
    fprintf(stderr, "synth: built without FluidSynth: keyboard notes are silent\n");
    return false;
}
bool active() { return false; }
void midi(uint8_t) {}
void render(float *, int, float) {}
std::string describe() { return "silent (no FluidSynth)"; }

#endif
}  // namespace fluid

bool init(const std::string &rom_dir, const std::string &soundfont, bool stand_in, const std::string &user_dir) {
    if (!stand_in) {
        auto card = std::make_unique<Xt446>();
        std::string err;
        if (card->load(rom_dir, err)) {
            g_card = std::move(card);
            fprintf(stderr, "synth: XT446 (MU100B firmware on the H8S/2655 + SWP30)\n");
            if (!getenv("HK_XT446_INLINE")) g_worker = std::make_unique<Worker>(*g_card);
            return true;
        }
        fprintf(stderr, "synth: no XT446 ROMs (%s in %s): using the SoundFont stand-in\n", err.c_str(), rom_dir.c_str());
    }
    return fluid::init(soundfont, user_dir);
}

bool active() { return g_card || fluid::active(); }

void midi(uint8_t b) {
    if (g_worker) g_worker->post(true, b);
    else if (g_card) g_card->midi(b);
    else fluid::midi(b);
}

void render(float *lr, int frames, float gain) {
    if (g_card) g_card->render(lr, frames, gain);
    else fluid::render(lr, frames, gain);
}

bool deferred() { return g_worker != nullptr; }

void request(int frames) {
    if (g_worker && frames > 0) g_worker->post(false, (uint32_t)frames);
}

void collect(float *lr, int frames, float gain) {
    if (!g_worker) return;
    g_worker->wait_all(g_collected);
    // (what collect_ready took out and left for later comes first)
    if (!g_ready.empty()) {
        g_ready.insert(g_ready.end(), g_collected.begin(), g_collected.end());
        g_collected.swap(g_ready);
        g_ready.clear();
    }
    // the board collects exactly what it requested since the last collect
    const size_t n = std::min(g_collected.size(), 2 * (size_t)frames);
    for (size_t k = 0; k < n; k++) lr[k] += g_collected[k] * gain;
}

int collect_ready(float *lr, int frames, float gain) {
    if (!g_worker) return 0;
    g_worker->take_ready(g_ready);
    const size_t n = std::min(g_ready.size(), 2 * (size_t)frames);
    for (size_t k = 0; k < n; k++) lr[k] += g_ready[k] * gain;   // (the same sums as collect's)
    g_ready.erase(g_ready.begin(), g_ready.begin() + (std::ptrdiff_t)n);
    return (int)(n / 2);
}

void stop() { g_worker.reset(); }

std::string describe() {
    if (g_card) {
        char buf[128];
        double emu = (double)g_card->samples() / 44100.0;
        snprintf(buf, sizeof buf, "XT446, %.1f s in %.1f s of host time", emu, g_card->cpu_seconds());
        return buf;
    }
    return fluid::describe();
}

}  // namespace hk::synth
