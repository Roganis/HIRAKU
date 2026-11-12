// SPDX-License-Identifier: GPL-3.0-or-later
// Whether the sound's way out still takes the sound, and what to do when it doesn't.
//
// The game's sound goes into a queue that a device empties, and where the sound paces the game
// (host/window.cpp: the machine waits while more than the target is queued) a device that stops
// emptying it stops the game: a USB card pulled out (ALSA: every call answers "no such device",
// for good), a device Windows takes back from exclusive mode, an ASIO driver whose settings were
// changed in its control panel (it asks to be restarted, and many stop calling back until they
// are). The window looks here every time it reads the controls, with the count of the device's
// pulls from the queue: no pull over two seconds of looking, or the back end saying its device
// is gone, and the way out is to be opened again; one that stops again within ten seconds of
// being opened gives way to a lesser one (the system's default output, then none: the game
// then runs by the clock, without sound).
//
// Time here is the time looked at, a quarter second a look at most: a PC that slept, or a game
// stopped in a debugger, hasn't seen its device take nothing for as long.
#pragma once

#include <algorithm>
#include <cstdint>

namespace hk::host {

class SoundWatch {
public:
    enum class Do {
        nothing,
        again,    // open the same way out again
        lesser,   // open the next lesser one
    };
    static constexpr uint64_t kLook = 250'000'000;         // a look counts for this long at most
    static constexpr uint64_t kStill = 2'000'000'000;      // no pull for this long: stopped
    static constexpr uint64_t kSettled = 10'000'000'000;   // stopped sooner after opening: a lesser one
    static constexpr uint64_t kAsked = 500'000'000;        // a driver's request waits this long (they come in bursts)
    static constexpr int kRestarts = 5;                    // requests granted in kRestartsOver; one more: a lesser one
    static constexpr uint64_t kRestartsOver = 30'000'000'000;

    // A way out was opened just now, the device's pulls so far `pulls`
    void opened(uint64_t now_ns, uint64_t pulls) {
        looked_ = now_ns, opened_ = clock_, pulls_ = pulls, still_ = 0, asked_ = false, why_ = "";
    }
    // gone: the back end says its device is no more. restart: its driver asks to be started again
    Do look(uint64_t now_ns, uint64_t pulls, bool gone, bool restart) {
        const uint64_t dt = std::min(now_ns - looked_, kLook);
        looked_ = now_ns, clock_ += dt;
        if (pulls != pulls_) pulls_ = pulls, still_ = 0;
        else still_ += dt;
        if (gone || still_ >= kStill) {
            why_ = gone ? "the device is gone" : "it took no sound for two seconds";
            return clock_ - opened_ < kSettled ? Do::lesser : Do::again;
        }
        if (!restart) return asked_ = false, Do::nothing;
        if (!asked_) asked_ = true, asked_at_ = clock_;
        if (clock_ - asked_at_ < kAsked) return Do::nothing;
        why_ = "its driver asked to be started again";
        if (restarts_ && clock_ - restarts_from_ > kRestartsOver) restarts_ = 0;
        if (!restarts_) restarts_from_ = clock_;
        if (++restarts_ > kRestarts) {
            why_ = "its driver keeps asking to be started again";
            return restarts_ = 0, Do::lesser;
        }
        return Do::again;
    }
    // Why the last look said to open one (for the log)
    const char *why() const { return why_; }

private:
    uint64_t looked_ = 0;          // the last look (the caller's clock)
    uint64_t clock_ = 0;           // time looked at, summed
    uint64_t opened_ = 0;          // ... when the way out was opened
    uint64_t pulls_ = 0, still_ = 0;
    bool asked_ = false;
    uint64_t asked_at_ = 0;
    int restarts_ = 0;
    uint64_t restarts_from_ = 0;
    const char *why_ = "";
};

}  // namespace hk::host
