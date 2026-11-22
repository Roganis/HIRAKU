// SPDX-License-Identifier: GPL-3.0-or-later
// hiraku-midi: the launcher's MIDI player (launcher/music.h starts it and talks to it).
//
//   hiraku-midi [--synth auto|card|font|windows] [--rom DIR]... [--soundfont FILE] [--volume 0-100]
//               [--device NAME] [--play FILE [--seconds S]]
//
// It plays standard MIDI files (runtime/midi/smf.h) through one of:
//   card     Keyboardmania's synth card, an embedded Yamaha MU100B (dev/xt446), when a --rom folder
//            has its files (a Keyboardmania set's rom/); it starts by running 6 s of the card's own
//            start-up, silently
//   font     a General MIDI SoundFont (.sf2) through TinySoundFont: --soundfont, else $HK_SOUNDFONT,
//            else the first one in /usr/share/soundfonts or /usr/share/sounds/sf2
//   windows  Windows' own synth (Microsoft GS Wavetable Synth), through the MIDI mapper
// auto: the first of these there is. The sound goes to the system's default output (--device: an
// output by its name, Windows), never a card alone: the games may want it while the music waits.
//
// Commands on stdin, one a line:
//   play SECONDS PATH   that file, from that time (a UTF-8 path, the rest of the line)
//   pause / resume / stop
//   volume N            0-100
//   quit                (as is the end of stdin: the launcher went)
// What it says on stdout, one a line:
//   synth KIND TEXT     once, when it is ready (KIND card, font or windows)
//   playing LENGTH TITLE   a song begins (TITLE: its name in the file, "" when it has none)
//   at SECONDS          where the song is, twice a second (what is heard: the queued sound counted)
//   levels HEX16        the 16 parts' levels as the song has them, a hex digit each (0-f: a note's
//                       velocity times the part's volume and expression, falling away after it), 20
//                       times a second while any is up; then one of all 0. Not scaled by the player's
//                       volume, which the launcher knows (launcher/musicvolume.h: its meters scale them)
//   ended               the song is over
//   error TEXT          a file it couldn't play (then ended), or no synth at all (then it exits, 3)
// --play: one file, the same as "play" on stdin, then quits once it ends (for trying it by hand).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <array>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>

#include "dev/xt446.h"
#include "midi/smf.h"
#include "midi/part_volume.h"

#define TSF_IMPLEMENTATION
#include "tsf.h"

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#endif

namespace {

constexpr int kRate = 44100;

void say(const std::string &line) {
    std::fputs((line + "\n").c_str(), stdout);
    std::fflush(stdout);
}

std::filesystem::path upath(const std::string &s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); }

// ---- the synths --------------------------------------------------------------

struct Synth {
    virtual ~Synth() = default;
    virtual void send(const uint8_t *b, size_t n) = 0;   // one message, or one SysEx
    virtual void render(float *lr, int frames) { std::fill(lr, lr + 2 * frames, 0.0f); }   // written, not added
    virtual bool renders() const { return true; }        // false: it plays on its own (Windows' synth)
    virtual void set_volume(float) {}                    // (those that don't render; the others are scaled here)
    virtual double line_seconds(size_t) const { return 0; }   // how long n bytes take to reach it
    // a song's start: everything off and back to General MIDI's start
    void reset() {
        static const uint8_t gm_on[] = {0xf0, 0x7e, 0x7f, 0x09, 0x01, 0xf7};
        silence();
        send(gm_on, sizeof gm_on);
    }
    // sound off now, on every channel (a pause, a stop, the next song)
    void silence() {
        for (uint8_t ch = 0; ch < 16; ch++)
            for (uint8_t cc : {64, 120, 123, 121}) {
                const uint8_t m[3] = {(uint8_t)(0xb0 | ch), cc, 0};
                send(m, 3);
            }
    }
};

// Keyboardmania's XT446 (an MU100B), its MIDI in at 31250 baud
struct CardSynth : Synth {
    hk::Xt446 card;
    void send(const uint8_t *b, size_t n) override {
        for (size_t k = 0; k < n; k++) card.midi(b[k]);
    }
    // (it comes out about 12 dB below a SoundFont's General MIDI level: brought up to it)
    void render(float *lr, int frames) override {
        card.take(lr, frames);
        for (int k = 0; k < 2 * frames; k++) lr[k] *= 3.0f;
    }
    double line_seconds(size_t n) const override { return (double)n * 320e-6; }
};

// a SoundFont through TinySoundFont, as a General MIDI module: channel 10 drums (XG's drum banks too)
struct FontSynth : Synth {
    tsf *f = nullptr;
    bool drums[16] = {};
    ~FontSynth() override {
        if (f) tsf_close(f);
    }
    void program(int ch, int p) { tsf_channel_set_presetnumber(f, ch, p, drums[ch]); }
    void gm_reset() {
        for (int ch = 0; ch < 16; ch++) {
            drums[ch] = ch == 9;
            tsf_channel_sounds_off_all(f, ch);
            tsf_channel_midi_control(f, ch, 121, 0);
            tsf_channel_set_bank(f, ch, 0);
            program(ch, 0);
            tsf_channel_set_pitchrange(f, ch, 2.0f);
        }
    }
    void send(const uint8_t *b, size_t n) override {
        if (n && b[0] == 0xf0) {
            // General MIDI on (7E 7F 09 01), GS reset (41 .. 42 12 40 00 7F), XG on (43 1x 4C 00 00 7E): a fresh start
            const bool gm = n >= 5 && b[1] == 0x7e && b[3] == 0x09 && b[4] == 0x01;
            const bool gs = n >= 9 && b[1] == 0x41 && b[3] == 0x42 && b[4] == 0x12 && b[5] == 0x40 && b[6] == 0x00 && b[7] == 0x7f;
            const bool xg = n >= 8 && b[1] == 0x43 && (b[2] & 0xf0) == 0x10 && b[3] == 0x4c && b[4] == 0 && b[5] == 0 && b[6] == 0x7e;
            if (gm || gs || xg) gm_reset();
            return;
        }
        if (n < 2 || b[0] < 0x80 || b[0] >= 0xf0) return;
        const int ch = b[0] & 0x0f, d1 = n > 2 ? b[2] : 0;
        switch (b[0] & 0xf0) {
        case 0x80: tsf_channel_note_off(f, ch, b[1]); break;
        case 0x90:
            if (d1) tsf_channel_note_on(f, ch, b[1], d1 / 127.0f);
            else tsf_channel_note_off(f, ch, b[1]);
            break;
        case 0xb0:
            // bank select: a GM SoundFont has bank 0 and the drums (128), so the banks themselves are left
            // out; MSB 126 and 127 are XG's drum kits, and channel 10 stays drums (GM, GS)
            if (b[1] == 0) drums[ch] = d1 >= 126 || ch == 9;
            else if (b[1] != 32) tsf_channel_midi_control(f, ch, b[1], d1);
            break;
        case 0xc0: program(ch, b[1]); break;
        case 0xe0: tsf_channel_set_pitchwheel(f, ch, b[1] | d1 << 7); break;
        }
    }
    void render(float *lr, int frames) override { tsf_render_float(f, lr, frames, 0); }
};

#ifdef _WIN32
// Windows' own General MIDI synth: messages straight to it, as their times come
struct WinSynth : Synth {
    HMIDIOUT h = nullptr;
    hk::midi::PartVolume parts;   // the player's volume, in each part's own (part_volume.h)
    void short_message(uint8_t a, uint8_t b, uint8_t c) { midiOutShortMsg(h, (DWORD)a | (DWORD)b << 8 | (DWORD)c << 16); }
    void send_parts() {
        for (int ch = 0; ch < 16; ch++) short_message((uint8_t)(0xb0 | ch), 7, parts.part(ch));
    }
    ~WinSynth() override {
        if (h) midiOutReset(h), midiOutClose(h);
    }
    bool renders() const override { return false; }
    void send(const uint8_t *b, size_t n) override {
        if (!n) return;
        if (b[0] == 0xf0) {
            std::vector<char> data(b, b + n);
            MIDIHDR hdr{};
            hdr.lpData = data.data();
            hdr.dwBufferLength = hdr.dwBytesRecorded = (DWORD)n;
            if (midiOutPrepareHeader(h, &hdr, sizeof hdr) == MMSYSERR_NOERROR) {
                if (midiOutLongMsg(h, &hdr, sizeof hdr) == MMSYSERR_NOERROR)
                    for (int k = 0; k < 1000 && !(hdr.dwFlags & MHDR_DONE); k++) Sleep(1);
                midiOutUnprepareHeader(h, &hdr, sizeof hdr);
            }
            // (the synth's parts are back at 100; sent again whether it went or not, never left at the song's)
            if (parts.afresh(b, n)) send_parts();
            return;
        }
        if (uint8_t v; parts.volume(b, n, v)) return short_message(b[0], 7, v);
        short_message(b[0], n > 1 ? b[1] : 0, n > 2 ? b[2] : 0);
    }
    // (not midiOutSetVolume: through the MIDI mapper on Windows 10 it changed nothing)
    void set_volume(float v) override {
        parts.set(v);
        send_parts();
    }
};
#endif

std::string find_font(const std::string &given) {
    if (!given.empty()) return given;
    if (const char *e = std::getenv("HK_SOUNDFONT"); e && *e) return e;
    for (const char *d : {"/usr/share/soundfonts", "/usr/share/sounds/sf2"}) {
        std::error_code ec;
        std::vector<std::string> found;
        for (auto &f : std::filesystem::directory_iterator(d, ec)) {
            std::string x = f.path().extension().string();
            std::transform(x.begin(), x.end(), x.begin(), ::tolower);
            if (x == ".sf2") found.push_back(f.path().string());
        }
        std::sort(found.begin(), found.end());
        if (!found.empty()) return found.front();
    }
    return "";
}

// the synth asked for, or the first there is; what it is, for "synth"
std::unique_ptr<Synth> open_synth(const std::string &want, const std::vector<std::string> &roms, const std::string &font_given,
                                  std::string &kind, std::string &what, std::string &why) {
    if (want == "auto" || want == "card") {
        for (const std::string &dir : roms) {
            auto c = std::make_unique<CardSynth>();
            std::string err;
            if (c->card.load(dir, err)) {
                // the card's firmware starting up, before it listens (as the game gives it time to): notes
                // before about 4.5 s of it are lost. 6 s, about a second here (it runs about 6x real time)
                std::vector<float> scratch(2 * 4410);
                for (int k = 0; k < 60; k++) c->card.take(scratch.data(), 4410);
                kind = "card", what = "Keyboardmania's synth card (Yamaha MU100B)";
                return c;
            }
            why += (why.empty() ? "" : "; ") + dir + ": " + err;
        }
        if (roms.empty()) why = "no Keyboardmania synth card files";
        if (want == "card") return nullptr;
    }
    if (want == "auto" || want == "font") {
        const std::string path = find_font(font_given);
        if (!path.empty()) {
            std::ifstream in(upath(path), std::ios::binary);
            std::vector<char> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            auto s = std::make_unique<FontSynth>();
            if (!data.empty()) s->f = tsf_load_memory(data.data(), (int)data.size());
            if (s->f) {
                tsf_set_output(s->f, TSF_STEREO_INTERLEAVED, kRate, 0);
                s->gm_reset();
                kind = "font", what = "SoundFont " + upath(path).filename().string();
                return s;
            }
            why += (why.empty() ? "" : "; ") + path + ": not a SoundFont this player reads";
        } else {
            why += (why.empty() ? "" : "; ") + std::string("no SoundFont");
        }
        if (want == "font") return nullptr;
    }
#ifdef _WIN32
    if (want == "auto" || want == "windows") {
        auto s = std::make_unique<WinSynth>();
        if (midiOutOpen(&s->h, MIDI_MAPPER, 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR) {
            midiOutSetVolume(s->h, 0xffffffff);   // (the device's own volume full, where an earlier HIRAKU turned it down)
            kind = "windows", what = "Windows' own synth";
            return s;
        }
        why += (why.empty() ? "" : "; ") + std::string("Windows' synth can't be opened");
    }
#endif
    return nullptr;
}

// ---- the player ---------------------------------------------------------------

struct Command {
    enum Kind { Play, Pause, Resume, Stop, Volume, Quit } kind = Quit;
    double value = 0;
    std::string path;
    Command() = default;
    Command(Kind k, double v = 0, std::string p = {}) : kind(k), value(v), path(std::move(p)) {}
};

std::mutex g_mu;
std::condition_variable g_cv;
std::deque<Command> g_commands;

void post(Command c) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_commands.push_back(std::move(c));
    }
    g_cv.notify_one();
}

bool parse_command(const std::string &line, Command &c) {
    std::string word = line.substr(0, line.find(' '));
    const std::string rest = line.size() > word.size() ? line.substr(word.size() + 1) : "";
    if (word == "play") {
        const size_t sp = rest.find(' ');
        if (sp == std::string::npos) return false;
        c = Command(Command::Play, std::atof(rest.substr(0, sp).c_str()), rest.substr(sp + 1));
        while (!c.path.empty() && (c.path.back() == '\r' || c.path.back() == '\n')) c.path.pop_back();
        return true;
    }
    if (word == "pause") return c = Command(Command::Pause), true;
    if (word == "resume") return c = Command(Command::Resume), true;
    if (word == "stop") return c = Command(Command::Stop), true;
    if (word == "volume") return c = Command(Command::Volume, std::atof(rest.c_str())), true;
    if (word == "quit") return c = Command(Command::Quit), true;
    return false;
}

float gain_of(double volume) {   // 0-100, heard as about even steps
    const double v = std::clamp(volume, 0.0, 100.0) / 100.0;
    return (float)(v * v);
}

class Player {
public:
    Player(Synth &s, SDL_AudioStream *out, double volume, bool quit_at_end)
        : synth_(s), out_(out), gain_(gain_of(volume)), quit_at_end_(quit_at_end) {
        synth_.set_volume(gain_);
        volume_.fill(100), expression_.fill(127);
    }

    void run() {
        std::vector<float> buf;
        for (;;) {
            if (!take_commands()) return;
            if (!song_ || paused_) {
                wait(std::chrono::milliseconds(50));
                continue;
            }
            if (out_) {
                // keep about 0.1 s queued: sound from now on, at most that late to a pause
                const int target = kRate / 10;
                int queued = std::max(0, SDL_GetAudioStreamQueued(out_)) / 8;
                while (song_ && queued < target) {
                    int n = 256;
                    dispatch_due();
                    if (next_ < song_->events.size()) n = (int)std::clamp<int64_t>(sample_of(song_->events[next_].time) - pos_, 1, n);
                    buf.resize(2 * (size_t)n);
                    synth_.render(buf.data(), n);
                    for (float &x : buf) x *= gain_;
                    SDL_PutAudioStreamData(out_, buf.data(), (int)(buf.size() * sizeof(float)));
                    pos_ += n;
                    queued += n;
                    if (next_ >= song_->events.size() && (double)pos_ / kRate > song_->length + 2.0) end_song();
                }
                report((double)pos_ / kRate - (double)queued / kRate);
                levels(pos_ - queued);
                wait(std::chrono::milliseconds(10));
            } else {   // a synth that plays by itself: its messages as the clock reaches them
                pos_ = (int64_t)std::llround(clock_seconds() * kRate);
                dispatch_due();
                if (next_ >= song_->events.size() && (double)pos_ / kRate > song_->length + 2.0) end_song();
                report((double)pos_ / kRate);
                levels(pos_);
                wait(std::chrono::milliseconds(1));
            }
        }
    }

private:
    int64_t sample_of(double t) const { return (int64_t)std::llround(t * kRate); }

    double clock_seconds() const {
        return start_at_ + std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
    }

    void dispatch_due() {
        while (song_ && next_ < song_->events.size() && sample_of(song_->events[next_].time) <= pos_) {
            const auto &e = song_->events[next_++];
            synth_.send(e.bytes.data(), e.bytes.size());
            track(e.bytes, sample_of(e.time));
        }
    }

    // ---- the parts' levels (the launcher's visualizer, like the MU100's own part meters) ----
    // A note on: its velocity times the part's volume and expression, at the sample it is heard;
    // the level falls from there. The messages go to the synth ahead of the sound (the queue), so
    // the notes wait in `onsets_` until the sound reaches them.
    void track(const std::vector<uint8_t> &b, int64_t sample) {
        if (b.size() < 3 || b[0] < 0x80 || b[0] >= 0xf0) return;
        const int ch = b[0] & 0x0f;
        if ((b[0] & 0xf0) == 0x90 && b[2]) {
            const float level = b[2] / 127.0f * std::min(1.0f, volume_[ch] / 100.0f) * (expression_[ch] / 127.0f);
            onsets_[ch].push_back({sample, level});
        } else if ((b[0] & 0xf0) == 0xb0 && b[1] == 7) {
            volume_[ch] = b[2];
        } else if ((b[0] & 0xf0) == 0xb0 && b[1] == 11) {
            expression_[ch] = b[2];
        } else if ((b[0] & 0xf0) == 0xb0 && b[1] == 121) {
            expression_[ch] = 127;
        }
    }

    void clear_levels() {
        for (int ch = 0; ch < 16; ch++) onsets_[ch].clear(), level_[ch] = 0, volume_[ch] = 100, expression_[ch] = 127;
        if (levels_up_) say("levels 0000000000000000"), levels_up_ = false;
    }

    void levels(int64_t heard) {
        if (std::chrono::steady_clock::now() - last_levels_ < std::chrono::milliseconds(50)) return;
        last_levels_ = std::chrono::steady_clock::now();
        char line[7 + 16 + 1] = "levels ";
        bool any = false;
        for (int ch = 0; ch < 16; ch++) {
            auto &q = onsets_[ch];
            while (!q.empty() && q.front().first <= heard) {
                level_[ch] = std::max(level_[ch] * (float)std::exp(-(double)(q.front().first - at_[ch]) / (0.35 * kRate)),
                                      q.front().second);
                at_[ch] = q.front().first;
                q.pop_front();
            }
            const float now = heard > at_[ch] ? level_[ch] * (float)std::exp(-(double)(heard - at_[ch]) / (0.35 * kRate)) : level_[ch];
            const int d = std::clamp((int)std::lround(now * 15), 0, 15);
            line[7 + ch] = "0123456789abcdef"[d];
            any |= d > 0;
        }
        if (any || levels_up_) say(line);
        levels_up_ = any;
    }

    void report(double at) {
        if (at < 0 || !song_) return;
        if (std::chrono::steady_clock::now() - last_report_ < std::chrono::milliseconds(500)) return;
        last_report_ = std::chrono::steady_clock::now();
        char line[64];
        std::snprintf(line, sizeof line, "at %.1f", at);
        say(line);
    }

    void end_song() {
        song_.reset();
        clear_levels();
        say("ended");
        if (quit_at_end_) quit_ = true;
    }

    void wait(std::chrono::milliseconds d) {
        std::unique_lock<std::mutex> lk(g_mu);
        g_cv.wait_for(lk, d, [] { return !g_commands.empty(); });
    }

    void start(const std::string &path, double at) {
        synth_.reset();
        clear_levels();
        if (out_) SDL_ClearAudioStream(out_);
        auto song = std::make_unique<hk::midi::Song>();
        std::string err;
        if (!hk::midi::load(path, *song, err)) {
            say("error " + upath(path).filename().string() + ": " + err);
            song_.reset();
            say("ended");
            if (quit_at_end_) quit_ = true;
            return;
        }
        at = std::clamp(at, 0.0, song->length);
        size_t bytes = 16 * 4 * 3 + 6;   // (reset's)
        for (auto &m : hk::midi::setup_before(*song, at, next_)) {
            synth_.send(m.data(), m.size());
            track(m, 0);
            bytes += m.size();
        }
        // a moment for the reset and the set-up to reach the synth (the card: down its MIDI line)
        // before the first note: the song's clock starts that much before `at`
        const double lead = 0.15 + synth_.line_seconds(bytes);
        pos_ = sample_of(at - lead);
        start_at_ = at - lead;
        started_ = std::chrono::steady_clock::now();
        song_ = std::move(song);
        paused_ = false;
        char line[64];
        std::snprintf(line, sizeof line, "playing %.1f ", song_->length);
        say(line + song_->title);
    }

    // false: quit
    bool take_commands() {
        for (;;) {
            Command c;
            {
                std::lock_guard<std::mutex> lk(g_mu);
                if (g_commands.empty()) return !quit_;
                c = std::move(g_commands.front());
                g_commands.pop_front();
            }
            switch (c.kind) {
            case Command::Play: start(c.path, c.value); break;
            case Command::Pause:
                if (song_ && !paused_) {
                    paused_ = true;
                    paused_at_ = clock_seconds();
                    synth_.silence();
                    if (out_) SDL_ClearAudioStream(out_);
                    for (auto &q : onsets_) q.clear();
                    for (float &l : level_) l = 0;
                    if (levels_up_) say("levels 0000000000000000"), levels_up_ = false;
                }
                break;
            case Command::Resume:
                if (song_ && paused_) {
                    paused_ = false;
                    // (Windows' synth: its clock from where it was; the others render on from pos_)
                    start_at_ = paused_at_;
                    started_ = std::chrono::steady_clock::now();
                }
                break;
            case Command::Stop:
                if (song_) synth_.silence(), song_.reset();
                clear_levels();
                if (out_) SDL_ClearAudioStream(out_);
                break;
            case Command::Volume:
                gain_ = gain_of(c.value);
                synth_.set_volume(gain_);
                break;
            case Command::Quit: return false;
            }
            if (quit_) return false;
        }
    }

    Synth &synth_;
    SDL_AudioStream *out_;
    float gain_;
    bool quit_at_end_, quit_ = false;
    std::unique_ptr<hk::midi::Song> song_;
    size_t next_ = 0;          // the next event to send
    int64_t pos_ = 0;          // the song's clock in samples (rendered so far)
    bool paused_ = false;
    double start_at_ = 0, paused_at_ = 0;   // (the clock of a synth that plays by itself)
    std::chrono::steady_clock::time_point started_, last_report_, last_levels_;
    std::array<std::deque<std::pair<int64_t, float>>, 16> onsets_;   // notes on, not heard yet
    std::array<float, 16> level_{};                                     // each part's last note's level ...
    std::array<int64_t, 16> at_{};                                      // ... from this sample
    std::array<uint8_t, 16> volume_, expression_;
    bool levels_up_ = false;                                            // the last levels said weren't all 0
};

}  // namespace

int main(int argc, char **argv) {
    std::string want = "auto", font, device, play;
    std::vector<std::string> roms;
    double volume = 70, seconds = 0;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--synth") want = next();
        else if (a == "--rom") roms.push_back(next());
        else if (a == "--soundfont") font = next();
        else if (a == "--volume") volume = std::atof(next().c_str());
        else if (a == "--device") device = next();
        else if (a == "--play") play = next();
        else if (a == "--seconds") seconds = std::atof(next().c_str());
        else if (a == "--help" || a == "-h") {
            std::fputs("usage: hiraku-midi [--synth auto|card|font|windows] [--rom DIR]... [--soundfont FILE]\n"
                       "                   [--volume 0-100] [--device NAME] [--play FILE [--seconds S]]\n"
                       "Commands on stdin: play SECONDS PATH, pause, resume, stop, volume N, quit\n",
                       stdout);
            return 0;
        } else {
            std::fprintf(stderr, "hiraku-midi: unknown option %s\n", a.c_str());
            return 2;
        }
    }
    if (want != "auto" && want != "card" && want != "font" && want != "windows") {
        std::fprintf(stderr, "hiraku-midi: --synth is auto, card, font or windows\n");
        return 2;
    }

    std::string kind, what, why;
    std::unique_ptr<Synth> synth = open_synth(want, roms, font, kind, what, why);
    if (!synth) {
        say("error no synth to play MIDI with: " + why);
        return 3;
    }

    SDL_AudioStream *out = nullptr;
    if (synth->renders()) {
        SDL_SetHint(SDL_HINT_APP_NAME, "HIRAKU");
        SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_NAME, "Music");
        if (!SDL_Init(SDL_INIT_AUDIO)) {
            say(std::string("error no sound output: ") + SDL_GetError());
            return 3;
        }
        const SDL_AudioSpec spec{SDL_AUDIO_F32, 2, kRate};
        if (!device.empty()) {
            int n = 0;
            SDL_AudioDeviceID *ids = SDL_GetAudioPlaybackDevices(&n);
            for (int k = 0; ids && k < n && !out; k++)
                if (const char *name = SDL_GetAudioDeviceName(ids[k]); name && device == name)
                    out = SDL_OpenAudioDeviceStream(ids[k], &spec, nullptr, nullptr);
            SDL_free(ids);
        }
        if (!out) out = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (!out) {
            say(std::string("error no sound output: ") + SDL_GetError());
            SDL_Quit();
            return 3;
        }
        SDL_ResumeAudioStreamDevice(out);
    }
#ifdef _WIN32
    timeBeginPeriod(1);   // (the 1 ms waits between Windows' synth's messages: not 15.6)
#endif
    say("synth " + kind + " " + what);

    if (!play.empty()) post(Command(Command::Play, seconds, play));
    else
        std::thread([] {   // the commands; the end of stdin (the launcher gone) is a quit
            std::string line;
            while (std::getline(std::cin, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                Command c;
                if (parse_command(line, c)) post(std::move(c));
            }
            post(Command(Command::Quit));
        }).detach();

    Player(*synth, out, volume, !play.empty()).run();

    synth.reset();
    if (out) SDL_DestroyAudioStream(out), SDL_Quit();
#ifdef _WIN32
    timeEndPeriod(1);
#endif
    std::fflush(stdout);
    std::_Exit(0);   // (the stdin thread may still be in its read)
}
