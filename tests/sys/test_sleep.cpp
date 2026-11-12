// SPDX-License-Identifier: GPL-3.0-or-later
// hk::sys::sleep_for and sleep_until: a wait lasts what was asked, and not much more. What they
// are there for (shell/sys.h): the Windows build's std::this_thread::sleep_for waits not at all
// under a millisecond and 15.6 ms for two. The first is caught here under Wine too; the second
// shows on Windows itself only (the program is run there by hand: the last lines are for that).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "shell/sys.h"

using Clock = std::chrono::steady_clock;
using us = std::chrono::microseconds;

static int g_failed = 0;
#define CHECK(what, ok) \
    do { \
        if (!(ok)) printf("FAILED: %s (line %d)\n", what, __LINE__), g_failed++; \
    } while (0)

// The middle one of 41 waits' lengths (the PC may be busy with other tests: the middle one holds), in
// microseconds; `least` the shortest
template <class F>
static double middle(F wait, double *least = nullptr) {
    std::vector<double> v;
    for (int i = 0; i < 41; i++) {
        const auto a = Clock::now();
        wait();
        v.push_back(std::chrono::duration<double, std::micro>(Clock::now() - a).count());
    }
    std::sort(v.begin(), v.end());
    if (least) *least = v[0];
    return v[20];
}

static void checks(const char *where) {
    double least = 0;
    const double tenth = middle([] { hk::sys::sleep_for(us(100)); }, &least);
    printf("%s: sleep_for(100 us) lasts %.0f us (the shortest %.0f)\n", where, tenth, least);
    CHECK("a tenth of a millisecond is waited for", least >= 100);
    CHECK("... and not for milliseconds", tenth < 2500);

    const double two = middle([] { hk::sys::sleep_for(std::chrono::milliseconds(2)); }, &least);
    printf("%s: sleep_for(2 ms) lasts %.0f us (the shortest %.0f)\n", where, two, least);
    CHECK("two milliseconds are waited for", least >= 2000);
    CHECK("... and not for a 15.6 ms tick", two < 6000);

    const double until = middle([] { hk::sys::sleep_until(Clock::now() + us(2900)); }, &least);
    printf("%s: sleep_until(2.9 ms on) lasts %.0f us (the shortest %.0f)\n", where, until, least);
    CHECK("a time is waited for", least >= 2900);
    CHECK("... and not a 15.6 ms tick", until < 7000);
}

int main() {
    {   // nothing to wait for: back at once
        const auto a = Clock::now();
        for (int i = 0; i < 1000; i++) {
            hk::sys::sleep_for(us(0));
            hk::sys::sleep_for(us(-5));
            hk::sys::sleep_until(Clock::now() - std::chrono::seconds(1));
        }
        CHECK("a wait for nothing, or for a time gone by, doesn't wait", Clock::now() - a < std::chrono::milliseconds(200));
    }
    checks("the first thread");
    std::thread([] { checks("another thread") ; }).join();   // (each thread has a timer of its own on Windows)
    {   // a long one isn't cut short
        const auto a = Clock::now();
        hk::sys::sleep_for(std::chrono::milliseconds(120));
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - a).count();
        CHECK("120 ms last 120 ms", ms >= 120 && ms < 200);
    }
    // for comparison, the standard wait this takes the place of (nothing checked: it is as it is)
    const double std_tenth = middle([] { std::this_thread::sleep_for(us(100)); });
    const double std_two = middle([] { std::this_thread::sleep_for(std::chrono::milliseconds(2)); });
    printf("std::this_thread::sleep_for here: 100 us lasts %.0f us, 2 ms lasts %.0f us\n", std_tenth, std_two);
    hk::sys::full_speed();   // (it must at least not fail to link or fault)
    printf(g_failed ? "%d FAILED\n" : "sleep ok\n", g_failed);
    return g_failed ? 1 : 0;
}
