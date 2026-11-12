// SPDX-License-Identifier: GPL-3.0-or-later
// Windows: the DXGI swap chain's queue of a frame's other pictures (runtime/host/swapchain.cpp) and
// display pacing's accounting of them (pacing.cpp), run as the window runs them (window.cpp's
// window_present and window_next, smooth motion on) against a pretend Direct3D device, swap chain,
// display and OpenGL (fake_d3d.h): no graphics card is needed, so it runs under Wine.
//
//   test_swapchain queue|wait [HZ [WORK_MS [FRAMES [strict]]]]
//
// queue: the pictures queued for the swap chain's own thread; wait: HK_DXGI_NO_QUEUE, each waited
// for on the game's thread, as before. A frame's emulation (WORK_MS, default 6) comes after its
// picture, as on Twinkle: longer than a refresh at 240 Hz (HZ's default), so `wait` isn't ready in
// time for most frames and `queue` is.
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
    const bool strict = argc > 5 && !strcmp(argv[5], "strict");
    const bool queue = mode == "queue";
    _putenv(queue ? "HK_DXGI_NO_QUEUE=" : "HK_DXGI_NO_QUEUE=1");
    setvbuf(stdout, nullptr, _IONBF, 0);

    fake_display(hz, 1'500'000);
    std::string why;
    if (!dxgi::start(nullptr, 640, 480, why)) {
        printf("FAILED: no swap chain on the pretend device: %s\n", why.c_str());
        return 1;
    }
    check(dxgi::queues() == queue, "queues() isn't what HK_DXGI_NO_QUEUE says");
    pacing::init(nullptr, true);
    pacing::set_smooth_motion(true);

    const DWORD game_thread = GetCurrentThreadId();
    const double period = 1.0 / 59.94;
    std::set<long long> own;   // the frames' own pictures (the others: their other refreshes')
    long long stamp = 0, extras = 0;
    bool sync = false;
    for (int f = 0; f < frames; f++) {
        // window_next: until the frame's burst, then the last of its emulation
        if (sync) pacing::wait_next();
        pacing::pump();
        busy(0.3);
        // window_present
        sync = pacing::frame(f ? period : 0.0);
        if (sync && pacing::refreshes_per_frame() == 0) continue;
        fake_drawn = ++stamp;
        own.insert(stamp);
        if (sync) pacing::wait_commit(), pacing::commit();
        else pacing::observe();
        dxgi::present(sync);
        const int k = sync ? pacing::refreshes_per_frame() : 1;
        if (sync) pacing::committed(false);
        for (int i = 1; i < k; i++) {   // the frame's other refreshes: a picture each
            busy(0.2);
            fake_drawn = ++stamp;
            extras++;
            if (!pacing::commit_extra(i)) dxgi::present(true, true);
        }
        // half way: the window's size changes with pictures still queued
        if (f == frames / 2) dxgi::resize(800, 600);
        // back at the machine: most of the frame's emulation comes after its picture
        busy(work_ms);
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
        // queued: the frames' own by the game's thread, the others by the swap chain's
        if (queue ? mine != (own.count(log[i].stamp) > 0) : !mine) wrong_thread++;
    }
    long long refresh;   // every refresh a picture: each present shown a refresh after the last
    fake_last_vblank_ns(&refresh);
    for (int i = 1; i < n; i++)
        if (log[i].vblank_ns - log[i - 1].vblank_ns == refresh) a_refresh_on++;
    check(out_of_order == 0, "a picture was presented before one drawn earlier");
    check(wrong_thread == 0, "a picture was presented by the wrong thread");
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
    check(shown + gone <= committed && shown + gone >= committed - 16, "the other refreshes' pictures don't add up");
    check((long long)own.size() + extras - gone == n, "the presents aren't the pictures drawn less those left out");
    check(presented <= (long long)own.size() && presented >= (long long)own.size() - 16, "the frames presented aren't the frames drawn");
    if (queue) check(by_thread == extras - gone, "the swap chain's own thread didn't present the other refreshes' pictures");

    printf("%s at %.0f Hz, %.1f ms a frame's emulation: %d frames, %lld not ready in time, %lld held their refreshes; "
           "%lld other pictures, %lld shown, %lld late, %lld left out or given up; %d of %d presents a refresh after the last\n",
           mode.c_str(), hz, work_ms, frames, not_ready, held, extras, shown, late, gone, a_refresh_on, n - 1);
    if (strict && queue) {
        check(not_ready * 20 <= frames, "strict: more than 5 % of the frames weren't ready in time");
        check(held * 10 >= presented * 9, "strict: under 90 % of the frames held their refreshes");
        check(late * 20 <= shown, "strict: more than 5 % of the other pictures came a refresh late");
        check(a_refresh_on * 10 >= (n - 1) * 9, "strict: under 90 % of the presents came a refresh after the last");
    }
    pacing::shutdown();
    dxgi::stop();
    printf(g_failed ? "%d checks failed\n" : "ok\n", g_failed);
    return g_failed ? 1 : 0;
}
