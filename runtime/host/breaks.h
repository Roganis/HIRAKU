// SPDX-License-Identifier: GPL-3.0-or-later
// Why the sound broke: each break (a pull of the sound device's that found the queue short) sorted
// by what the frame it came in shows, for the exit report (window.cpp) and the slow-frame lines
// (statsview.cpp). The slow-frame lines are one every five seconds, forty a run: a cabinet run's
// hundreds of breaks were mostly not in them, and the study of the IIDX cabinet's logs (2026-11)
// could not tell whether what was left came from the picture path on the machine's thread, from the
// queue's arithmetic or from the machine itself. Every break is counted here, in one class:
//
//   - the frame's own work (the machine's and the window's) was over its time: by the largest part,
//     the window's, or a part of the machine's (named); or with the CPU running slow (Windows says);
//   - the work fit, on display sync's grid, but a burst (the machine's run of a frame, its sound made
//     as it runs: the frame's own or the next one's) started late, so its sound came late: by how
//     late, and what the window spent the time on after the frame's picture was handed over;
//   - the work fit and the bursts started on time: the queue was too short for the device's pulls;
//   - the work fit, without display sync (no grid to be late against);
//   - a frame a PC game drew no picture for (its own report says why).
//
// The classifier is a pure function of the frame's numbers (tests/host/test_breaks.cpp).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#include "../core/prof.h"
#include "pacing.h"

namespace hk::host::breaks {

// Display sync: from a frame's picture handed over (its commit, or queued for the swap chain's
// thread) to the next burst's time
struct Path {
    bool known = false;
    float commit_late_ms = 0;          // the picture handed over this long after its commit time (> 0: not ready in time)
    float room_ms = 0;                 // the schedule's time from then to the next burst
    float took_ms = 0;                 // the window's, until it was done
    float swap_ms = 0;                 // ... of it the swaps' own (the display taking the pictures: OpenGL's swap, the
                                       // DXGI present but for its waits)
    float wait_ms[pacing::NWAIT] = {}; // ... and its waits, by what for
};

// What a frame with breaks in it showed, milliseconds
struct Input {
    float period_ms = 0;
    float window_ms = 0;               // the window's work (its drawing, the swaps in it)
    float machine_ms[prof::NCAT] = {}; // the machine's work by part (prof::PRESENT's is the window's: not counted)
    float other_ms = 0;                // the machine's work outside the named parts
    int cpu_mhz = 0, cpu_nominal_mhz = 0;   // 0: not known (Windows says)
    bool again = false;                // a PC game drew no picture for it (Core::frame_repeated)
    bool paced = false;                // on display sync's grid: late_ms and path are known
    float late_ms = 0;                 // the later of the frame's burst and the next one's: started this long after its time
    Path path;                         // ... and the picture path before that burst
};

constexpr float LATE_MS = 0.5f;   // a burst this late or more is late (a 5 ms queue over a 4 ms buffer spares 1 ms)

enum Class { WINDOW_OVER, MACHINE_OVER, CPU_SLOW, LATE_BURST, ON_TIME, UNPACED, NOT_DRAWN, NCLASS };
// in the exit report: what each class means
inline const char *const CLASS_SAID[NCLASS] = {
    "the frame's own work was over its time, the window's part the largest (its pictures drawn and handed to the display)",
    "the frame's own work was over its time, a part of the machine's the largest",
    "the frame's own work was over its time, with the CPU running below nine tenths of its nominal clock (the power plan)",
    ("the work fit, but a burst (the machine's run of a frame: the frame's own or the next one's) started late against the "
     "display's grid, so its sound came late"),
    "the work fit and the bursts started on time: the queue was too short for the device's pulls",
    "the work fit, without display sync (no grid to be late against: the sound paces the game)",
    "in a frame a PC game drew no picture for (its own report says why)",
};
// in a slow-frame line
inline const char *const CLASS_SHORT[NCLASS] = {"own work over its time, the window's part the largest",
                                                "own work over its time, the machine's part the largest",
                                                "own work over its time, the CPU slow", "the work fit, a burst late",
                                                "the work fit, the bursts on time: the queue too short",
                                                "the work fit, no display sync", "no picture drawn"};

enum Cause { LATE_PICTURE, WOKE_LATE, SWAP, DRAWING, EXTRA, PRESENT, LOCK, OTHER_WAIT, UNKNOWN, NCAUSE };
inline const char *const CAUSE_SAID[NCAUSE] = {
    "the frame's own picture handed over after the next burst's time already",
    "the window done in time, the wait for the burst ending late (the system woke the thread late)",
    "the swaps (the display taking the pictures)",
    "the window's other drawing (the frame's other refreshes' pictures, the overlays)",
    "waits for the other refreshes' times",
    "the display holding a present",
    "waits for the swap chain's thread",
    "other waits",
    "not known (the frame's picture wasn't handed over on the grid)",
};

// The largest part of a frame's work: -1 the window's, 0..NCAT-1 a part of the machine's, NCAT the
// machine's outside the named parts
inline int largest_part(const Input &in) {
    int best = -1;
    float most = in.window_ms;
    for (int c = 0; c < prof::NCAT; c++)
        if (c != prof::PRESENT && in.machine_ms[c] > most) best = c, most = in.machine_ms[c];
    if (in.other_ms > most) best = prof::NCAT;
    return best;
}

inline float work_ms(const Input &in) {
    float w = in.window_ms + in.other_ms;
    for (int c = 0; c < prof::NCAT; c++)
        if (c != prof::PRESENT) w += in.machine_ms[c];
    return w;
}

inline Class classify(const Input &in) {
    if (in.again) return NOT_DRAWN;
    if (work_ms(in) > in.period_ms) {
        if (in.cpu_mhz > 0 && in.cpu_mhz * 10 < in.cpu_nominal_mhz * 9) return CPU_SLOW;
        return largest_part(in) < 0 ? WINDOW_OVER : MACHINE_OVER;
    }
    if (!in.paced) return UNPACED;
    return in.late_ms >= LATE_MS ? LATE_BURST : ON_TIME;
}

// What made a late burst late: what the window spent the most time on between the frame's picture
// handed over and the burst's time (or that it was done in time, or that the picture itself came
// after that time)
inline Cause cause(const Path &p) {
    if (!p.known) return UNKNOWN;
    if (p.room_ms <= 0) return LATE_PICTURE;
    if (p.took_ms <= p.room_ms) return WOKE_LATE;
    float waits = 0, other = 0;
    for (int k = 0; k < pacing::NWAIT; k++) {
        waits += p.wait_ms[k];
        const auto w = (pacing::Wait)k;
        if (w != pacing::Wait::extra && w != pacing::Wait::present && w != pacing::Wait::lock) other += p.wait_ms[k];
    }
    const float each[] = {p.swap_ms, std::max(0.0f, p.took_ms - p.swap_ms - waits), p.wait_ms[(int)pacing::Wait::extra],
                          p.wait_ms[(int)pacing::Wait::present], p.wait_ms[(int)pacing::Wait::lock], other};
    int best = 0;
    for (int i = 1; i < 6; i++)
        if (each[i] > each[best]) best = i;
    return (Cause)(SWAP + best);
}

// A burst's lateness in the report's steps: 0.5-1, 1-2, 2-3, 3-5, 5 ms or more
inline int late_step(float ms) { return ms < 1 ? 0 : ms < 2 ? 1 : ms < 3 ? 2 : ms < 5 ? 3 : 4; }

// The run's breaks by class, and every burst's lateness, for the exit report
struct Summary {
    uint64_t n[NCLASS] = {};
    uint64_t by_part[prof::NCAT + 1] = {};       // MACHINE_OVER: by the largest part (NCAT: outside the named ones)
    uint64_t late_steps[5] = {}, by_cause[NCAUSE] = {};
    double late_sum = 0, late_worst = 0;         // LATE_BURST: how late its burst was
    double low_sum = 0, short_sum = 0;           // ON_TIME: the queue at its lowest as the frame's sound came, and as
    uint64_t low_n = 0, short_n = 0;             // the device found it short
    uint64_t bursts = 0, burst_late[6] = {};     // every burst on the grid: on time, then late_step's
    double burst_worst = 0;

    void burst(float late_ms) {
        bursts++;
        burst_late[late_ms < LATE_MS ? 0 : 1 + late_step(late_ms)]++;
        burst_worst = std::max(burst_worst, (double)late_ms);
    }
    // queue_low_ms, short_queued_ms: < 0 not known
    Class add(const Input &in, int breaks, float queue_low_ms, float short_queued_ms) {
        const Class c = classify(in);
        const uint64_t b = (uint64_t)std::max(breaks, 0);
        n[c] += b;
        if (c == MACHINE_OVER) by_part[std::max(0, largest_part(in))] += b;
        if (c == LATE_BURST) {
            late_steps[late_step(in.late_ms)] += b;
            by_cause[cause(in.path)] += b;
            late_sum += (double)in.late_ms * (double)b, late_worst = std::max(late_worst, (double)in.late_ms);
        }
        if (c == ON_TIME && queue_low_ms >= 0) low_sum += (double)queue_low_ms * (double)b, low_n += b;
        if (c == ON_TIME && short_queued_ms >= 0) short_sum += (double)short_queued_ms * (double)b, short_n += b;
        return c;
    }
    uint64_t counted() const {
        uint64_t s = 0;
        for (uint64_t v : n) s += v;
        return s;
    }
    // total: the run's breaks after its first 10 s; target_ms: the queue's target at the end; device_ms: the device's buffer
    std::string describe(uint64_t total, double target_ms, double device_ms) const {
        std::string out;
        char b[1024];
        total = std::max(total, counted());
        if (!total) {
            out = "audio: no breaks in the sound after the first 10 s";
        } else {
            snprintf(b, sizeof b, "audio: the %llu break%s after the first 10 s, by what the frame each came in shows "
                     "(a burst: the machine's run of a frame, its sound made as it runs):", (unsigned long long)total, total == 1 ? "" : "s");
            out = b;
        }
        static const int ORDER[NCLASS] = {LATE_BURST, ON_TIME, WINDOW_OVER, MACHINE_OVER, CPU_SLOW, UNPACED, NOT_DRAWN};
        for (int c : ORDER) {
            if (!n[c]) continue;
            snprintf(b, sizeof b, "\naudio:   %llu: %s", (unsigned long long)n[c], CLASS_SAID[c]);
            out += b;
            if (c == MACHINE_OVER) {
                std::string parts;
                for (int p = 0; p <= prof::NCAT; p++)
                    if (by_part[p])
                        parts += (parts.empty() ? "" : ", ") + std::string(p == prof::NCAT ? "other" : prof::NAMES[p]) + " " +
                                 std::to_string(by_part[p]);
                out += " (" + parts + ")";
            }
            if (c == LATE_BURST) {
                const uint64_t *s = late_steps;
                snprintf(b, sizeof b, ": 0.5-1 ms late %llu, 1-2 ms %llu, 2-3 ms %llu, 3-5 ms %llu, 5 ms or more %llu (mean %.1f ms, "
                         "the latest %.1f ms); what the window spent most of the time on before it:",
                         (unsigned long long)s[0], (unsigned long long)s[1], (unsigned long long)s[2], (unsigned long long)s[3],
                         (unsigned long long)s[4], late_sum / (double)n[c], late_worst);
                out += b;
                bool first = true;
                for (int k = 0; k < NCAUSE; k++)
                    if (by_cause[k]) {
                        snprintf(b, sizeof b, "%s %s %llu", first ? "" : ",", CAUSE_SAID[k], (unsigned long long)by_cause[k]);
                        out += b, first = false;
                    }
            }
            if (c == ON_TIME) {
                snprintf(b, sizeof b, ": the queue's target %.1f ms (at the end), the device's buffer %.1f ms, %.1f ms %s", target_ms,
                         device_ms, std::fabs(target_ms - device_ms), target_ms >= device_ms ? "to spare" : "less than the buffer");
                out += b;
                if (low_n) snprintf(b, sizeof b, "; in those frames the queue was down to %.1f ms on average as their sound came",
                                    low_sum / (double)low_n), out += b;
                if (short_n) snprintf(b, sizeof b, ", %.1f ms when the device found it short", short_sum / (double)short_n), out += b;
            }
        }
        if (const uint64_t rest = total - counted()) {
            snprintf(b, sizeof b, "\naudio:   %llu: outside the frames at the game's pace (a pause, loading, fast-forward, the "
                     "run's end)", (unsigned long long)rest);
            out += b;
        }
        if (bursts) {
            const uint64_t *s = burst_late;
            snprintf(b, sizeof b, "\nvideo: the bursts on display sync's grid: %llu, %llu of them 0.5 ms or more after their time "
                     "(0.5-1 ms %llu, 1-2 ms %llu, 2-3 ms %llu, 3-5 ms %llu, 5 ms or more %llu), the latest %.1f ms",
                     (unsigned long long)bursts, (unsigned long long)(bursts - s[0]), (unsigned long long)s[1],
                     (unsigned long long)s[2], (unsigned long long)s[3], (unsigned long long)s[4], (unsigned long long)s[5],
                     burst_worst);
            out += b;
        }
        return out;
    }
};

}  // namespace hk::host::breaks
