// SPDX-License-Identifier: GPL-3.0-or-later
// hk::sys::sleep_for and sleep_until: a wait lasts what was asked, and not much more (and
// hk::sys::Wake, a wait another thread ends: wake_checks). What they
// are there for (shell/sys.h): the Windows build's std::this_thread::sleep_for waits not at all
// under a millisecond and 15.6 ms for two. The first is caught here under Wine too; the second
// shows on Windows itself only (the program is run there by hand: the last lines are for that).
#include <algorithm>
#include <atomic>
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

// hk::sys::Wake: a wait ends at the other thread's signal, or at its time
static void wake_checks() {
    hk::sys::Wake w;
    {   // nobody signals: the time, and not much more
        double least = 0;
        const double two = middle([&] { CHECK("no signal: the wait says so", !w.wait(2'000'000)); }, &least);
        printf("Wake: a 2 ms wait nobody ends lasts %.0f us (the shortest %.0f)\n", two, least);
        CHECK("... and lasts its time", least >= 2000);
        CHECK("... not a 15.6 ms tick", two < 6000);
        const double quarter = middle([&] { w.wait(250'000); }, &least);
        printf("Wake: a 0.25 ms wait nobody ends lasts %.0f us (the shortest %.0f)\n", quarter, least);
        CHECK("a quarter of a millisecond is waited for", least >= 250);
        CHECK("... and not for milliseconds", quarter < 2500);
    }
    {   // signalled before the wait: kept, once
        w.signal();
        w.signal();
        const auto a = Clock::now();
        CHECK("a signal before the wait ends it", w.wait(1'000'000'000));
        CHECK("... at once", Clock::now() - a < std::chrono::milliseconds(100));
        CHECK("... once, however often it was signalled", !w.wait(1'000'000));
        CHECK("a wait for no time only says what there is", !w.wait(0));
        w.signal();
        CHECK("...", w.wait(0));
    }
    {   // signalled from another thread every 3 ms, as a sound device takes its sound: each one
        // ends a wait, soon after it
        std::atomic<bool> stop{false};
        std::atomic<int64_t> at{0};   // the last signal's time (ns on the steady clock); 0: taken
        std::atomic<int> sent{0};
        std::thread other([&] {
            while (!stop) {
                hk::sys::sleep_for(std::chrono::milliseconds(3));
                at = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
                sent++;
                w.signal();
            }
        });
        std::vector<double> late;
        int woken = 0, timed_out = 0;
        const auto until = Clock::now() + std::chrono::milliseconds(600);
        while (Clock::now() < until) {
            if (!w.wait(50'000'000)) {
                timed_out++;
                continue;
            }
            woken++;
            const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
            if (const int64_t t = at.exchange(0)) late.push_back((double)(now - t) / 1000.0);
        }
        stop = true;
        other.join();
        std::sort(late.begin(), late.end());
        const double mid = late.empty() ? 1e9 : late[late.size() / 2];
        printf("Wake: %d signals 3 ms apart ended %d waits (%d ran out); the wait ended %.0f us after its signal (the middle one)\n",
               sent.load(), woken, timed_out, mid);
        CHECK("no wait ran its 50 ms out", timed_out == 0);
        CHECK("nearly every signal ended a wait", woken >= sent - 3 && woken <= sent + 1);
        CHECK("... within a millisecond", mid < 1000);
    }
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
    wake_checks();
    // for comparison, the standard wait this takes the place of (nothing checked: it is as it is)
    const double std_tenth = middle([] { std::this_thread::sleep_for(us(100)); });
    const double std_two = middle([] { std::this_thread::sleep_for(std::chrono::milliseconds(2)); });
    printf("std::this_thread::sleep_for here: 100 us lasts %.0f us, 2 ms lasts %.0f us\n", std_tenth, std_two);
    hk::sys::full_speed();   // (it must at least not fail to link or fault)
    printf(g_failed ? "%d FAILED\n" : "sleep ok\n", g_failed);
    return g_failed ? 1 : 0;
}
