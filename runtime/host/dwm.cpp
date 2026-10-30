// SPDX-License-Identifier: GPL-3.0-or-later
#include "dwm.h"

#include <windows.h>
#include <dwmapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../core/prof.h"

namespace hk::host::dwm {

namespace {

// DWM's own estimate: its last vblank and its refresh period (the whole desktop's, 8.1 on)
bool dwm_timing(int64_t &last_ns, int64_t &period_ns) {
    using Timing = HRESULT(WINAPI *)(HWND, DWM_TIMING_INFO *);
    static const Timing timing = [] {
        HMODULE m = LoadLibraryW(L"dwmapi.dll");
        return m ? reinterpret_cast<Timing>(reinterpret_cast<void *>(GetProcAddress(m, "DwmGetCompositionTimingInfo")))
                 : nullptr;
    }();
    if (!timing) return false;
    DWM_TIMING_INFO ti{};
    ti.cbSize = sizeof ti;
    if (timing(nullptr, &ti) != S_OK || !ti.qpcRefreshPeriod || !ti.qpcVBlank) return false;
    LARGE_INTEGER f, now;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&now);
    const int64_t now_ns = (int64_t)prof::now_ns();
    const double ns_per_count = 1e9 / (double)f.QuadPart;
    last_ns = now_ns - (int64_t)((double)(now.QuadPart - (int64_t)ti.qpcVBlank) * ns_per_count);
    period_ns = (int64_t)((double)ti.qpcRefreshPeriod * ns_per_count + 0.5);
    return period_ns > 0;
}

// The display's own vblanks, from the graphics kernel (D3DKMTWaitForVerticalBlankEvent, gdi32's, as
// Chromium uses it): a thread wakes at each of the window's monitor's vblanks and notes when. DWM's
// estimate wandered (a cabinet's 120 Hz read as 119.39 to 120.64 Hz over a run), and each frame's
// period with it; the line through the last few hundred vblanks' times is steady, and a late wake
// (the thread scheduled a little after the vblank) is left out of it.
struct OpenFromHdc {
    HDC hdc;
    UINT adapter;
    LUID luid;
    UINT source;
};
struct WaitVblank {
    UINT adapter, device, source;
};
struct CloseAdapter {
    UINT adapter;
};
using OpenFn = LONG(APIENTRY *)(OpenFromHdc *);
using WaitFn = LONG(APIENTRY *)(const WaitVblank *);
using CloseFn = LONG(APIENTRY *)(const CloseAdapter *);

constexpr size_t KEEP = 240;   // the vblanks the line goes through (2 s at 120 Hz)
constexpr size_t READY = 60;   // ... and those needed before it's used

std::mutex g_mu;
std::wstring g_device;               // the monitor's (\\.\DISPLAY1), set by start(); changed: opened again
unsigned g_generation = 0;
std::vector<std::pair<int64_t, int64_t>> g_seen;   // (the vblank's number, when it was seen, ns)
int64_t g_line_at = 0, g_line_period = 0;          // the line's latest vblank and period
std::atomic<bool> g_line{false}, g_started{false};
std::string g_name = "DWM's vblank";
int64_t g_lo = 0, g_hi = 0;                         // the line's periods over the run

// the least-squares line through the vblanks seen, then again without those seen later than it by
// more than twice the spread (late wakes)
void fit() {
    const size_t n = g_seen.size();
    if (n < READY) return;
    std::vector<bool> use(n, true);
    double a = 0, b = 0;
    const double k0 = (double)g_seen[0].first, t0 = (double)g_seen[0].second;
    for (int pass = 0; pass < 2; pass++) {
        double sk = 0, st = 0, skk = 0, skt = 0, m = 0;
        for (size_t i = 0; i < n; i++) {
            if (!use[i]) continue;
            const double k = (double)g_seen[i].first - k0, t = (double)g_seen[i].second - t0;
            sk += k, st += t, skk += k * k, skt += k * t, m++;
        }
        const double d = m * skk - sk * sk;
        if (m < READY / 2 || d <= 0) return;
        b = (m * skt - sk * st) / d;
        a = (st - b * sk) / m;   // (relative to k0, t0)
        double ss = 0;
        for (size_t i = 0; i < n; i++) {
            const double r = ((double)g_seen[i].second - t0) - (a + b * ((double)g_seen[i].first - k0));
            ss += r * r;
        }
        const double sd = std::sqrt(ss / (double)n);
        for (size_t i = 0; i < n; i++)
            use[i] = ((double)g_seen[i].second - t0) - (a + b * ((double)g_seen[i].first - k0)) <= 2 * sd;
    }
    if (b <= 0) return;
    g_line_period = (int64_t)std::llround(b);
    g_line_at = (int64_t)std::llround(t0 + a + b * ((double)g_seen.back().first - k0));
    g_lo = g_lo ? std::min(g_lo, g_line_period) : g_line_period, g_hi = std::max(g_hi, g_line_period);
    g_line = true;
}

void tracker() {
    HMODULE gdi = LoadLibraryW(L"gdi32.dll");
    const auto open = gdi ? reinterpret_cast<OpenFn>(reinterpret_cast<void *>(GetProcAddress(gdi, "D3DKMTOpenAdapterFromHdc"))) : nullptr;
    const auto wait = gdi ? reinterpret_cast<WaitFn>(reinterpret_cast<void *>(GetProcAddress(gdi, "D3DKMTWaitForVerticalBlankEvent"))) : nullptr;
    const auto close = gdi ? reinterpret_cast<CloseFn>(reinterpret_cast<void *>(GetProcAddress(gdi, "D3DKMTCloseAdapter"))) : nullptr;
    if (!open || !wait || !close) {
        fprintf(stderr, "display sync: no graphics kernel's vblank here (DWM's estimate instead)\n");
        return;
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    unsigned generation = ~0u;
    WaitVblank w{};
    bool opened = false, said = false;
    int64_t number = 0;
    for (;;) {
        std::wstring device;
        {
            std::lock_guard<std::mutex> l(g_mu);
            if (generation != g_generation) {   // another monitor: its own vblanks
                if (opened) {
                    CloseAdapter c{w.adapter};
                    close(&c);
                    opened = false;
                }
                generation = g_generation;
                g_seen.clear();
                g_line = false;
            }
            device = g_device;
        }
        if (!opened) {
            HDC dc = CreateDCW(nullptr, device.empty() ? L"DISPLAY" : device.c_str(), nullptr, nullptr);
            OpenFromHdc o{};
            o.hdc = dc;
            if (dc && open(&o) == 0) {
                w = WaitVblank{o.adapter, 0, o.source};
                opened = true;
            }
            if (dc) DeleteDC(dc);
            if (!opened) {
                if (!said) fprintf(stderr, "display sync: the graphics kernel's vblank isn't there (DWM's estimate instead)\n");
                said = true;
                Sleep(1000);
                continue;
            }
        }
        if (wait(&w) != 0) {   // the monitor gone, or its mode changed: opened again
            CloseAdapter c{w.adapter};
            close(&c);
            opened = false;
            Sleep(100);
            continue;
        }
        const int64_t t = (int64_t)prof::now_ns();
        std::lock_guard<std::mutex> l(g_mu);
        if (generation != g_generation) continue;
        if (!g_seen.empty()) {   // its number: the vblanks since the last (one missed: two)
            const int64_t gap = t - g_seen.back().second, period = g_line ? g_line_period : gap;
            number = g_seen.back().first + std::max<int64_t>(1, period > 0 ? std::llround((double)gap / (double)period) : 1);
        }
        g_seen.emplace_back(number, t);
        if (g_seen.size() > KEEP) g_seen.erase(g_seen.begin());
        fit();
        if (g_line && g_name[0] == 'D') g_name = "the display's vblank, from the graphics kernel";
    }
}

}  // namespace

void start(void *hwnd) {
    std::wstring device;
    if (HMONITOR mon = hwnd ? MonitorFromWindow((HWND)hwnd, MONITOR_DEFAULTTONEAREST) : nullptr) {
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof mi;
        if (GetMonitorInfoW(mon, &mi)) device = mi.szDevice;
    }
    {
        std::lock_guard<std::mutex> l(g_mu);
        if (device != g_device || !g_started) g_device = device, g_generation++;
    }
    if (!g_started.exchange(true)) std::thread(tracker).detach();
}

bool vblank(int64_t &last_ns, int64_t &period_ns) {
    if (g_line) {
        std::lock_guard<std::mutex> l(g_mu);
        if (g_line) {
            period_ns = g_line_period;
            last_ns = g_line_at;   // carried on to the latest due by now
            const int64_t now = (int64_t)prof::now_ns();
            if (now > last_ns) last_ns += (now - last_ns) / period_ns * period_ns;
            return period_ns > 0;
        }
    }
    return dwm_timing(last_ns, period_ns);
}

std::string report() {
    std::lock_guard<std::mutex> l(g_mu);
    if (!g_lo) return "";
    char b[160];
    snprintf(b, sizeof b, "%s: %.3f to %.3f Hz over the run", g_name.c_str(), 1e9 / (double)g_hi, 1e9 / (double)g_lo);
    return b;
}

bool kernel() { return g_line; }

}  // namespace hk::host::dwm
