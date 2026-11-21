// SPDX-License-Identifier: GPL-3.0-or-later
// Windows: the DXGI swap chain's queue of a frame's other pictures (runtime/host/swapchain.cpp) and
// display pacing's accounting of them (pacing.cpp), run as the window runs them (window.cpp's
// window_present and window_next, smooth motion on) against a pretend Direct3D device, swap chain,
// display and OpenGL (fake_d3d.h): no graphics card is needed, so it runs under Wine.
//
//   test_swapchain queue|own|wait [HZ [WORK_MS [FRAMES [strict]]]]
//
// queue: the pictures queued for the swap chain's own thread, the frames' own too; own:
// HK_DXGI_QUEUE_OWN=0, the frames' own waited for and presented by the game's thread, the others
// queued; wait: HK_DXGI_NO_QUEUE, each waited for on the game's thread. A frame's emulation (WORK_MS, default 6) comes after its
// picture, as on Twinkle: longer than a refresh at 240 Hz (HZ's default), so `wait` isn't ready in
// time for most frames and `queue` is.
//
// A window the display stops showing for a while (Alt+Tab to another, covered, minimized), each way
// fake_d3d.h's fake_cover has, then shown again: covered-jump, covered-lost, covered-live, occluded;
// stall: one present held half a second; disjoint: a statistics reading that fails now and then. The
// pictures must be presented again soon after the window is shown again, and the game's thread never
// wait long. Flags after FRAMES: nosync (display sync off: the game's thread presents, in place of the
// one waiting), nosmooth (no smooth motion: at 60 Hz, a frame's own picture alone).
// composed: a composited desktop's way, each picture shown a refresh after the first it is in time
// for and the statistics naming a vblank a little after it, then a stall: few pictures may be left out.
// snapshot: the same, but the statistics brought up to date only when a present is made (a Windows 11
// VM's way): pictures must go on being presented, never more than a few left out in a row.
//
// Fails on what doesn't depend on the PC's speed: something the pretend device doesn't allow, a
// picture presented out of order or by the wrong thread, the report's counts not adding up.
// strict: also on the timing (frames not ready, pictures off their refreshes), for a quiet PC.
#include <windows.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "core/prof.h"
#include "host/dwm.h"
#include "host/pacing.h"
#include "host/swapchain.h"

#include "fake_d3d.h"

using namespace hk;
using namespace hk::host;

// OpenGL's calls are the pretend ones (SDL's own isn't linked in its place)
extern "C" SDL_FunctionPointer SDLCALL SDL_GL_GetProcAddress(const char *proc) { return (SDL_FunctionPointer)fake_get_proc(proc); }

// the pretend display's vblanks (dwm.cpp isn't linked)
namespace hk::host::dwm {
void start(void *) {}
bool vblank(int64_t &last_ns, int64_t &period_ns) {
    long long period;
    const long long v = fake_last_vblank_ns(&period);
    last_ns = (int64_t)prof::now_ns() - (fake_now_ns() - v);
    period_ns = period;
    return true;
}
bool kernel() { return true; }
std::string report() { return {}; }
}  // namespace hk::host::dwm

namespace {

int g_failed = 0;
void check(bool ok, const char *what) {
    if (ok) return;
    g_failed++;
    printf("FAILED: %s\n", what);
}

// the machine at work for so long
void busy(double ms) {
    const uint64_t until = prof::now_ns() + (uint64_t)(ms * 1e6);
    while (prof::now_ns() < until) {}
}

// the number before `after` in a report ("12 not ready in time"); -1 if it isn't there
long long number(const std::string &text, const char *after) {
    const size_t at = text.find(after);
    if (at == std::string::npos || !at) return -1;
    size_t from = at;
    while (from > 0 && isdigit((unsigned char)text[from - 1])) from--;
    return from == at ? -1 : atoll(text.c_str() + from);
}

}  // namespace

int main(int argc, char **argv) {
    const std::string mode = argc > 1 ? argv[1] : "queue";
    const double hz = argc > 2 ? atof(argv[2]) : 240.0;
    const double work_ms = argc > 3 ? atof(argv[3]) : 6.0;
    const int frames = argc > 4 ? atoi(argv[4]) : 600;
    bool strict = false, nosync = false, nosmooth = false;
    for (int i = 5; i < argc; i++) {
        strict |= !strcmp(argv[i], "strict");
        nosync |= !strcmp(argv[i], "nosync");
        nosmooth |= !strcmp(argv[i], "nosmooth");
    }
    // the window away from the screen for a while (fake_cover), or the display's other ways
    const int cover = mode == "covered-jump"   ? FAKE_COVER_JUMP
                      : mode == "covered-lost" ? FAKE_COVER_LOST
                      : mode == "covered-live" ? FAKE_COVER_LIVE
                      : mode == "occluded"     ? FAKE_OCCLUDED
                                               : FAKE_SHOWN;
    const bool stall = mode == "stall", disjoint = mode == "disjoint", snapshot = mode == "snapshot";
    const bool composed = mode == "composed" || snapshot;
    const bool away = cover != FAKE_SHOWN || stall || disjoint;
    // "even": a 58 Hz game, the frame's work done before its pictures are shown (as the window does at
    // the game's own speed for a core whose picture comes first): the work must end at the game's even
    // pace, not on the display's refreshes. "uneven": the same with HK_PACING_EVEN=0 and the pictures
    // first, as it was, to see the difference (nothing checked)
    // "tight": beatmania III on the IIDX cabinet: a 59.95 Hz game on 120 Hz, 5 ms of work and 1.7 ms
    // of this thread for each picture's drawing, so that the budget, a refresh and the second
    // picture's drawing are more than a frame: the work must still end at the game's even pace.
    // "tight-own": the same with HK_DXGI_QUEUE_OWN=0, as it was, to see the difference (nothing checked)
    const bool tight = mode == "tight", tight_own = mode == "tight-own";
    const bool even = mode == "even" || tight, uneven = mode == "uneven", as_was = tight_own;
    const bool queue = mode == "queue" || mode == "own" || even || uneven || as_was || away || composed;
    const bool own_queued = queue && mode != "own" && !tight_own;   // the frames' own pictures queued too
    _putenv(queue ? "HK_DXGI_NO_QUEUE=" : "HK_DXGI_NO_QUEUE=1");
    if (queue && !own_queued) _putenv("HK_DXGI_QUEUE_OWN=0");
    if (uneven) _putenv("HK_PACING_EVEN=0");
    const double draw_ms = tight || tight_own ? 1.7 : 0.0;   // this thread held by a picture's drawing
    setvbuf(stdout, nullptr, _IONBF, 0);

    fake_display(hz, 1'500'000);
    std::string why;
    if (!dxgi::start(nullptr, 640, 480, why)) {
        printf("FAILED: no swap chain on the pretend device: %s\n", why.c_str());
        return 1;
    }
    check(dxgi::queues() == queue, "queues() isn't what HK_DXGI_NO_QUEUE says");
    pacing::init(nullptr, !nosync);
    pacing::set_smooth_motion(!nosmooth);
    if (composed) fake_composed(1, snapshot ? 0 : 2'000'000);
    if (snapshot) fake_statistics_at_present(1);
    // (away) the window away from frame cover_from to cover_to; the game's thread's longest stretch
    // from a frame's start (its burst) to its pictures handed over
    const int cover_from = frames / 5, cover_to = frames / 5 + std::min(240, frames / 4);
    long long t_uncover = 0, stamp_uncover = 0, left_at_stall = -1;
    double longest_ms = 0;
    int longest_at = -1;

    const DWORD game_thread = GetCurrentThreadId();
    const double period = tight || tight_own ? 1.0 / 59.95 : even || uneven ? 1.0 / 58.0 : 1.0 / 59.94;
    std::vector<double> work_done;   // when each frame's work ended, ms, less its even place
    std::set<long long> own;   // the frames' own pictures (the others: their other refreshes')
    long long stamp = 0, extras = 0;
    bool sync = false;
    for (int f = 0; f < frames; f++) {
        // window_next: until the frame's burst, then the last of its emulation
        if (sync) pacing::wait_next();
        const uint64_t presenting = prof::now_ns();   // (away) from here to the frame's pictures handed over
        if (away && f == cover_from) {
            if (stall) fake_present_stall(500);
            else if (cover != FAKE_SHOWN) fake_cover(cover);
            if (stall || disjoint) t_uncover = fake_now_ns(), stamp_uncover = stamp;
        }
        if (cover != FAKE_SHOWN && f == cover_to) fake_cover(FAKE_SHOWN), t_uncover = fake_now_ns(), stamp_uncover = stamp;
        if (disjoint && f >= cover_from && f % 40 == 0) fake_statistics_fail(1);
        if (composed && f == frames / 2 + 50) {   // a stall of the game's
            left_at_stall = number(dxgi::describe(), " in-between pictures left out");
            busy(100);
        }
        pacing::pump();
        busy(0.3);
        // window_present
        sync = pacing::frame(f ? period : 0.0);
        if (sync && pacing::refreshes_per_frame() == 0) continue;
        if (even || as_was) busy(work_ms);   // the frame's work, its pictures kept for after it
        if ((even || uneven || as_was) && sync && f > frames / 4) {
            if (uneven) busy(0.1);   // (as it was: the work after the pictures, timed below)
            else work_done.push_back((double)prof::now_ns() / 1e6 - (double)f * period * 1e3);
        }
        busy(draw_ms);
        fake_drawn = ++stamp;
        own.insert(stamp);
        // (window.cpp's show_frame: queued for its commit time where it can be, else waited for)
        if (!(sync && pacing::commit_queued())) {
            if (sync) pacing::wait_commit(), pacing::commit();
            else pacing::observe();
            dxgi::present(sync);
        }
        const int k = sync ? pacing::refreshes_per_frame() : 1;
        if (sync) pacing::committed(false);
        for (int i = 1; i < k; i++) {   // the frame's other refreshes: a picture each
            busy(0.2 + draw_ms);
            fake_drawn = ++stamp;
            extras++;
            if (!pacing::commit_extra(i)) dxgi::present(true, true);
        }
        if (const double ms = (double)(prof::now_ns() - presenting) / 1e6; ms > longest_ms) longest_ms = ms, longest_at = f;
        // half way: the window's size changes with pictures still queued
        if (f == frames / 2) dxgi::resize(800, 600);
        // back at the machine: most of the frame's emulation comes after its picture
        if (!even && !as_was) busy(work_ms);
        if (uneven && sync && f > frames / 4) work_done.push_back((double)prof::now_ns() / 1e6 - (double)f * period * 1e3);
    }
    dxgi::settle();
    Sleep(100);
    pacing::pump();
    const std::string paced = pacing::describe(), chain = dxgi::describe();
    printf("%s\n%s\n", paced.c_str(), chain.c_str());

    // what the pretend device saw
    check(fake_errors == 0, "the pretend Direct3D device was used in a way a real one doesn't allow");
    const fake_present *log;
    const int n = fake_presents(&log);
    int out_of_order = 0, wrong_thread = 0, a_refresh_on = 0, by_thread = 0;
    for (int i = 0; i < n; i++) {
        if (i && log[i].stamp <= log[i - 1].stamp) out_of_order++;
        const bool mine = log[i].thread == game_thread;
        if (!mine) by_thread++;
        // queued: the others by the swap chain's thread, and the frames' own too (own_queued; but
        // for one that found no texture free) or by the game's thread
        const bool is_own = own.count(log[i].stamp) > 0;
        if (!queue ? !mine : own_queued ? mine && !is_own : mine != is_own) wrong_thread++;
    }
    long long refresh;   // every refresh a picture: each present shown a refresh after the last
    fake_last_vblank_ns(&refresh);
    for (int i = 1; i < n; i++)
        if (log[i].vblank_ns - log[i - 1].vblank_ns == refresh) a_refresh_on++;
    check(out_of_order == 0, "a picture was presented before one drawn earlier");
    check(wrong_thread == 0, "a picture was presented by the wrong thread");
    if (away || composed) {   // (the counts below are for a display that shows every picture)
        const long long left_out = number(chain, " in-between pictures left out");
        if (away) {
            // the first picture drawn after the window was shown again, and how soon one was presented;
            // and of the pictures drawn after it, how many were presented (but the last few, on their way)
            long long first_ms = -1, after = 0;
            for (int i = 0; i < n; i++)
                if (log[i].stamp > stamp_uncover) {
                    if (first_ms < 0) first_ms = (log[i].at_ns - t_uncover) / 1'000'000;
                    after++;
                }
            const long long drawn = stamp - stamp_uncover;
            printf("%s%s%s: the window away from frame %d to %d; the first picture after it presented %lld ms after, %lld of the %lld "
                   "drawn after it presented, %lld left out in all; the game's thread's longest presenting %.1f ms (frame %d)\n",
                   mode.c_str(), nosync ? " nosync" : "", nosmooth ? " nosmooth" : "", cover_from, stall || disjoint ? cover_from : cover_to,
                   first_ms, after, drawn, left_out, longest_ms, longest_at);
            check(first_ms >= 0 && (stall || first_ms <= 100), "away: no picture presented within 100 ms of the window being shown again");
            check(after * 10 >= (drawn - 16) * 8, "away: under 80 % of the pictures drawn after the window was shown again were presented");
            // (stall: the game's thread waits with the held present, its own or, for the lock the
            // swap chain's thread keeps over its presents, that thread's: said above, not checked)
            check(stall || longest_ms < 100, "away: the game's thread was held 100 ms or more presenting a frame");
        } else {
            // the most pictures left out in a row (but the last few, on their way), and the last presented
            long long run = 0, last = n ? log[n - 1].stamp : 0;
            for (int i = 1; i < n; i++) run = std::max(run, log[i].stamp - log[i - 1].stamp - 1);
            run = std::max(run, stamp - last - 16);
            printf("%s: %lld pictures left out of %lld drawn in %d frames (%lld before the stall at frame %d), at most %lld in a row\n",
                   mode.c_str(), left_out, stamp, frames, left_at_stall, frames / 2 + 50, run);
            check(left_out >= 0 && left_out * 20 <= stamp, "composed: more than 5 % of the pictures were left out");
            check(run <= 4, "composed: more than 4 pictures left out in a row");
        }
        pacing::shutdown();
        dxgi::stop();
        printf(g_failed ? "%d checks failed\n" : "ok\n", g_failed);
        return g_failed ? 1 : 0;
    }
    check(n > frames, "fewer presents than frames");

    // the report's counts
    const long long presented = number(paced, " presented:"), held = number(paced, " held their"),
                    not_ready = number(paced, " not ready in time"), committed = number(paced, " pictures committed"),
                    shown = number(paced, " shown ("), late = number(paced, " of them a refresh or more late"),
                    left_out = number(chain, " in-between pictures left out"), no_room = number(chain, " more left out with no texture free"),
                    given_up = number(chain, " given up"), chain_presented = number(chain, " presented, ");
    const long long gone = std::max(0ll, left_out) + std::max(0ll, no_room) + std::max(0ll, given_up);
    check(presented >= 0 && held >= 0 && not_ready >= 0 && committed >= 0 && shown >= 0 && left_out >= 0 && chain_presented >= 0,
          "a count is missing from the report");
    check(chain_presented == n, "the swap chain's count of presents isn't the device's");
    check(committed == extras, "the other refreshes' pictures committed aren't those drawn");
    // every one of them shown, left out or given up, but for the last few still on their way
    // (own_queued: a frame's own picture can be left out too, so `gone` isn't the others' alone)
    check(shown + (own_queued ? 0 : gone) <= committed && shown + gone >= committed - 16, "the other refreshes' pictures don't add up");
    check((long long)own.size() + extras - gone == n, "the presents aren't the pictures drawn less those left out");
    check(presented <= (long long)own.size() && presented >= (long long)own.size() - 16 - (own_queued ? gone : 0),
          "the frames presented aren't the frames drawn");
    if (queue && !own_queued) check(by_thread == extras - gone, "the swap chain's own thread didn't present the other refreshes' pictures");
    if (own_queued) check(by_thread >= n - 16, "the swap chain's own thread didn't present the pictures");

    printf("%s at %.0f Hz, %.1f ms a frame's emulation: %d frames, %lld not ready in time, %lld held their refreshes; "
           "%lld other pictures, %lld shown, %lld late, %lld left out or given up; %d of %d presents a refresh after the last\n",
           mode.c_str(), hz, work_ms, frames, not_ready, held, extras, shown, late, gone, a_refresh_on, n - 1);
    if (strict && queue) {
        check(not_ready * 20 <= frames, "strict: more than 5 % of the frames weren't ready in time");
        check(held * 10 >= presented * 9, "strict: under 90 % of the frames held their refreshes");
        check(late * 20 <= shown, "strict: more than 5 % of the other pictures came a refresh late");
        check(a_refresh_on * 10 >= (n - 1) * 9, "strict: under 90 % of the presents came a refresh after the last");
    }
    if (even || uneven || as_was) {
        // each frame's work ended where the game's clock puts it, give or take: all but the outer
        // twentieths within 3 ms of each other (on the refreshes, they spread over one: 8 ms at 120 Hz)
        std::sort(work_done.begin(), work_done.end());
        const size_t n = work_done.size();
        const double spread = n > 40 ? work_done[n - 1 - n / 20] - work_done[n / 20] : 1e9;
        printf("%s: the frames' work ended within %.2f ms of the game's even pace (%zu frames, the outer twentieths left out)\n",
               mode.c_str(), spread, n);
        if (even) check(spread < (tight ? 1.5 : 3.0), "the frames' work doesn't end at the game's even pace");
    }
    pacing::shutdown();
    dxgi::stop();
    printf(g_failed ? "%d checks failed\n" : "ok\n", g_failed);
    return g_failed ? 1 : 0;
}
