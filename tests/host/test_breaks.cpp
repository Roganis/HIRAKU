// SPDX-License-Identifier: GPL-3.0-or-later
// The breaks in the sound sorted by what their frame shows (runtime/host/breaks.h), from made-up
// frames: each class, what made a late burst late, the exit report's counts adding up to the run's
// breaks; and the log's line of test switches (runtime/shell/switches.h), a secret's value never in it.
//
//   test_breaks    exit 0 when all hold
#include <cstdio>
#include <cstring>
#include <string>

#include "host/breaks.h"
#include "shell/switches.h"

using namespace hk::host;
using namespace hk::host::breaks;

static int g_failed = 0;
#define CHECK(x) \
    do { \
        if (!(x)) g_failed++, fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
    } while (0)

namespace {

// a frame of a 60 Hz game: 4 ms of the machine's work (cpu), 1 ms of the window's, on display sync's
// grid with its bursts on time
Input fits() {
    Input in;
    in.period_ms = 16.7f;
    in.machine_ms[hk::prof::CPU] = 4.0f;
    in.window_ms = 1.0f;
    in.paced = true;
    return in;
}

// the picture path before a burst: handed over with `room` ms to the next burst's time, done after `took`
Path path(float room, float took, float swap = 0, float present = 0, float lock = 0) {
    Path p;
    p.known = true;
    p.room_ms = room, p.took_ms = took, p.swap_ms = swap;
    p.wait_ms[(int)pacing::Wait::present] = present;
    p.wait_ms[(int)pacing::Wait::lock] = lock;
    return p;
}

bool has(const std::string &s, const char *what) {
    if (s.find(what) != std::string::npos) return true;
    fprintf(stderr, "not in the text: \"%s\"\n%s\n", what, s.c_str());
    return false;
}

}  // namespace

int main() {
    // the work over the frame's time: by its largest part
    {
        Input in = fits();
        in.window_ms = 14.0f;   // 18 ms of 16.7: the window's the largest
        CHECK(classify(in) == WINDOW_OVER);
        in.window_ms = 1.0f, in.machine_ms[hk::prof::GPU] = 13.0f;   // the machine's GPU part
        CHECK(classify(in) == MACHINE_OVER);
        CHECK(largest_part(in) == hk::prof::GPU);
        in.machine_ms[hk::prof::GPU] = 0, in.other_ms = 13.0f;   // outside the named parts
        CHECK(largest_part(in) == hk::prof::NCAT);
        in.machine_ms[hk::prof::PRESENT] = 50.0f;   // (the window's own part: not the machine's)
        CHECK(largest_part(in) == hk::prof::NCAT);
        in.cpu_mhz = 800, in.cpu_nominal_mhz = 3000;   // with the CPU slowed down
        CHECK(classify(in) == CPU_SLOW);
        in.cpu_mhz = 2950;
        CHECK(classify(in) == MACHINE_OVER);
    }
    // the work fit: on time, late, no grid, no picture
    {
        Input in = fits();
        CHECK(classify(in) == ON_TIME);
        in.late_ms = 0.4f;
        CHECK(classify(in) == ON_TIME);
        in.late_ms = 1.8f;
        CHECK(classify(in) == LATE_BURST);
        in.paced = false;
        CHECK(classify(in) == UNPACED);
        in.again = true;
        CHECK(classify(in) == NOT_DRAWN);
        in = fits();
        in.cpu_mhz = 800, in.cpu_nominal_mhz = 3000;   // a slow CPU whose work still fit: not its fault
        CHECK(classify(in) == ON_TIME);
    }
    // what made a late burst late
    CHECK(cause(Path{}) == UNKNOWN);
    CHECK(cause(path(-0.5f, 3.0f)) == LATE_PICTURE);
    CHECK(cause(path(9.0f, 8.0f)) == WOKE_LATE);
    CHECK(cause(path(9.0f, 11.0f, 8.4f)) == SWAP);          // Linux: a swap that blocked
    CHECK(cause(path(9.0f, 11.0f, 0.5f, 8.2f)) == PRESENT); // Windows: the display held a present
    CHECK(cause(path(9.0f, 11.0f, 0.5f, 0.0f, 6.0f)) == LOCK);
    CHECK(cause(path(9.0f, 11.0f, 0.5f, 1.0f)) == DRAWING); // the other refreshes' pictures drawn
    {
        Path p = path(9.0f, 11.0f, 0.5f);
        p.wait_ms[(int)pacing::Wait::extra] = 7.0f;
        CHECK(cause(p) == EXTRA);
        p.wait_ms[(int)pacing::Wait::extra] = 0, p.wait_ms[(int)pacing::Wait::sound] = 9.0f;
        CHECK(cause(p) == OTHER_WAIT);
    }
    // the report: every break in one class, the rest outside the frames
    {
        Summary s;
        Input late = fits();
        late.late_ms = 1.5f, late.path = path(9.0f, 10.5f, 8.0f);
        CHECK(s.add(late, 3, 0.2f, 0.1f) == LATE_BURST);
        late.late_ms = 6.0f, late.path = path(9.0f, 15.0f, 0.2f, 12.0f);
        s.add(late, 1, -1, -1);
        Input on = fits();
        s.add(on, 2, 0.4f, 0.0f);
        Input over = fits();
        over.machine_ms[hk::prof::CPU] = 20.0f;
        s.add(over, 1, -1, -1);
        for (int i = 0; i < 99; i++) s.burst(0.1f);
        s.burst(1.5f);
        CHECK(s.counted() == 7);
        CHECK(s.n[LATE_BURST] == 4 && s.n[ON_TIME] == 2 && s.n[MACHINE_OVER] == 1);
        CHECK(s.late_steps[1] == 3 && s.late_steps[4] == 1);
        CHECK(s.by_cause[SWAP] == 3 && s.by_cause[PRESENT] == 1);
        CHECK(s.by_part[hk::prof::CPU] == 1);
        const std::string d = s.describe(10, 5.0, 4.0);
        printf("%s\n", d.c_str());
        CHECK(has(d, "the 10 breaks after the first 10 s"));
        CHECK(has(d, "audio:   4: the work fit, but a burst"));
        CHECK(has(d, "0.5-1 ms late 0, 1-2 ms 3, 2-3 ms 0, 3-5 ms 0, 5 ms or more 1 (mean 2.6 ms, the latest 6.0 ms)"));
        CHECK(has(d, "the swaps (the display taking the pictures) 3, the display holding a present 1"));
        CHECK(has(d, "audio:   2: the work fit and the bursts started on time"));
        CHECK(has(d, "the queue's target 5.0 ms (at the end), the device's buffer 4.0 ms, 1.0 ms to spare"));
        CHECK(has(d, "down to 0.4 ms on average as their sound came, 0.0 ms when the device found it short"));
        CHECK(has(d, "audio:   1: the frame's own work was over its time, a part of the machine's the largest (cpu 1)"));
        CHECK(has(d, "audio:   3: outside the frames at the game's pace"));
        CHECK(has(d, "the bursts on display sync's grid: 100, 1 of them 0.5 ms or more after their time"));
        CHECK(Summary{}.describe(0, 5, 4) == "audio: no breaks in the sound after the first 10 s");
        CHECK(has(s.describe(2, 5, 4), "the 7 breaks"));   // (never fewer than counted)
    }
    // the test switches: HIRAKU's, SDL's and the recorded code's said, in order; a secret's value never
    {
        const char *env[] = {"PATH=/usr/bin", "HK_PACING_STALL=60,8", "HK_EA_KEY=0123456789abcdef", "SDL_VIDEODRIVER=offscreen",
                             "WAIT_SPIN_MICROSECONDS=50", "HK_CARD_FILE=x", "HK_TEST_PIN=1234", "HK_SOME_TOKEN=t", "HK_EMPTY=",
                             "HK_SPACED=a b", "HOME=/home/someone", nullptr};
        const std::string l = hk::switches_line(env);
        printf("%s\n", l.c_str());
        CHECK(l == "environment: test switches set: HK_CARD_FILE (its value not shown), HK_EA_KEY (its value not shown), "
                   "HK_EMPTY=\"\", HK_PACING_STALL=60,8, HK_SOME_TOKEN (its value not shown), HK_SPACED=\"a b\", "
                   "HK_TEST_PIN (its value not shown), SDL_VIDEODRIVER=offscreen, WAIT_SPIN_MICROSECONDS=50");
        CHECK(l.find("0123456789abcdef") == std::string::npos && l.find("1234") == std::string::npos && l.find("PATH") == std::string::npos);
        const char *none[] = {"PATH=/usr/bin", nullptr};
        CHECK(hk::switches_line(none).find("no test switches set") != std::string::npos);
        const std::string long_sdl = "SDL_HINT_X=" + std::string(60, 'a');
        const char *longer[] = {long_sdl.c_str(), nullptr};
        CHECK(hk::switches_line(longer) == "environment: test switches set: SDL_HINT_X (60 characters)");
    }
    printf(g_failed ? "%d checks failed\n" : "ok\n", g_failed);
    return g_failed ? 1 : 0;
}
