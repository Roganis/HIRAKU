// SPDX-License-Identifier: GPL-3.0-or-later
#include "swapchain.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// (in this order: Microsoft's d3d11.h brings d3d10_1.h, which refuses to come after d3d10.h)
#include <d3d11.h>
#include <d3d10.h>   // ID3D10Multithread (mingw's d3d11.h doesn't bring it)
#include <dxgi1_3.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../core/prof.h"

namespace hk::host::dxgi {

namespace {

// WGL_NV_DX_interop2's calls (wglext.h's names)
constexpr GLenum ACCESS_WRITE_DISCARD = 0x0002;   // WGL_ACCESS_WRITE_DISCARD_NV
using ExtensionsFn = const char *(WINAPI *)(HDC);
using OpenDeviceFn = HANDLE(WINAPI *)(void *);
using CloseDeviceFn = BOOL(WINAPI *)(HANDLE);
using RegisterFn = HANDLE(WINAPI *)(HANDLE, void *, GLuint, GLenum, GLenum);
using UnregisterFn = BOOL(WINAPI *)(HANDLE, HANDLE);
using LockFn = BOOL(WINAPI *)(HANDLE, GLint, HANDLE *);

struct Gl {
    decltype(&::glGetString) GetString;
    PFNGLGENFRAMEBUFFERSPROC GenFramebuffers;
    PFNGLDELETEFRAMEBUFFERSPROC DeleteFramebuffers;
    PFNGLBINDFRAMEBUFFERPROC BindFramebuffer;
    PFNGLFRAMEBUFFERRENDERBUFFERPROC FramebufferRenderbuffer;
    PFNGLFRAMEBUFFERTEXTURE2DPROC FramebufferTexture2D;
    PFNGLCHECKFRAMEBUFFERSTATUSPROC CheckFramebufferStatus;
    PFNGLBLITFRAMEBUFFERPROC BlitFramebuffer;
    PFNGLGENRENDERBUFFERSPROC GenRenderbuffers;
    PFNGLDELETERENDERBUFFERSPROC DeleteRenderbuffers;
    decltype(&::glGenTextures) GenTextures;
    decltype(&::glDeleteTextures) DeleteTextures;
    decltype(&::glBindTexture) BindTexture;
    decltype(&::glTexImage2D) TexImage2D;
    decltype(&::glTexParameteri) TexParameteri;
    OpenDeviceFn DXOpenDevice;
    CloseDeviceFn DXCloseDevice;
    RegisterFn DXRegisterObject;
    UnregisterFn DXUnregisterObject;
    LockFn DXLockObjects, DXUnlockObjects;
} gl;

template <typename F> bool get(F &f, const char *name) {
    f = reinterpret_cast<F>(SDL_GL_GetProcAddress(name));
    return f != nullptr;
}

bool g_on = false;
HWND g_hwnd = nullptr;
ID3D11Device *g_dev = nullptr;
ID3D11DeviceContext *g_ctx = nullptr;
IDXGISwapChain1 *g_swap = nullptr;
IDXGISwapChainMedia *g_media = nullptr;   // its statistics with how the picture went out (composed or not)
ID3D11Texture2D *g_back = nullptr;        // the swap chain's back buffer (buffer 0: always the next to show)
HANDLE g_interop = nullptr;
// A picture of OpenGL's, the right way up: a Direct3D texture shared with it. [0] is present()'s (the
// frame's own picture); the others are queue()'s, one for each picture waiting for its time, made
// when first needed
struct Slot {
    ID3D11Texture2D *tex = nullptr;
    HANDLE object = nullptr;              // ... as the sharing knows it
    GLuint rb = 0, out_tex = 0, fbo = 0;  // ... as OpenGL's (renderbuffer or texture), its framebuffer
    bool waiting = false;                 // queued, Direct3D's until presented (under q.mu)
};
constexpr int SLOTS = 1 + 12;
Slot g_slot[SLOTS];
bool g_as_texture = false, g_shared_flag = false;   // how OpenGL took [0]'s (make_buffers' way that worked): the others' too
const char *g_shared_as = "";
GLuint g_tex = 0, g_fbo = 0;              // OpenGL's own picture of the window (fbo())
int g_w = 0, g_h = 0;
const char *g_effect = "";
std::string g_adapter;
uint64_t n_presents = 0, n_left_out = 0, n_failed = 0;
uint32_t g_queued = 0;

// The pictures queued for their times (queue()), and the thread that presents them. Direct3D only
// on that thread; q.mu is held around every call of the swap chain's and every use of the back
// buffer, on either thread. What the thread uses is never freed (as dwm.cpp's: a process that exits
// without stop() would free it under the running thread)
struct Job {
    int slot;
    int64_t at;        // when to present it (prof::now_ns()'s clock)
    uint32_t ticket;
};
struct Queue {
    std::mutex mu;
    std::condition_variable wake;         // a picture queued, or quit
    HANDLE idle = CreateEventW(nullptr, TRUE, TRUE, nullptr);   // set while none is queued
    std::deque<Job> jobs;
    std::vector<Went> went;               // went()'s
    std::vector<Shown> seen;              // seen()'s
    std::thread *thread = nullptr;
    bool quit = false, gone = false;      // the thread told to end; ended
};
Queue &q = *new Queue;

// q.mu for the window's thread (the game's), which never waits blind for the queue's: DXGI may send
// the window a message from a present on another thread (Microsoft's "Multithreading and DXGI": a
// mode change), answered only when the window's thread asks for its messages; waiting for that
// present here would stop both. So while it waits, the messages sent to it are answered (none of
// the posted ones are taken: those are SDL's to read).
// settled: taken once no picture is left queued
void answer_sent() {
    MSG m;
    PeekMessageW(&m, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
}
struct Locked {
    explicit Locked(bool settled = false) {
        for (;;) {
            for (int n = 0; !q.mu.try_lock(); n++) {   // (held for some tens of microseconds a picture)
                if (n) answer_sent();
                if (n < 200) SwitchToThread();
                else Sleep(1);
            }
            if (!settled || q.jobs.empty()) return;
            q.mu.unlock();
            if (MsgWaitForMultipleObjectsEx(1, &q.idle, 50, QS_SENDMESSAGE, 0) == WAIT_OBJECT_0 + 1) answer_sent();
        }
    }
    ~Locked() { q.mu.unlock(); }
    Locked(const Locked &) = delete;
    Locked &operator=(const Locked &) = delete;
};
bool g_queues = false;                    // queue() is of use (start(): not with HK_DXGI_NO_QUEUE; off when a texture can't be made)
uint64_t n_queued = 0, n_queue_late = 0, n_no_room = 0, n_dropped = 0;
int64_t g_queue_late_sum = 0, g_queue_late_worst = 0;

template <typename T> void release(T *&p) {
    if (p) p->Release();
    p = nullptr;
}

// The pictures still queued, given up (the buffers are going: a resize, the end). Under q.mu
void drop_queued() {
    for (const Job &j : q.jobs) {
        g_slot[j.slot].waiting = false;
        q.went.push_back({j.ticket, 0});
        n_dropped++;
    }
    q.jobs.clear();
    SetEvent(q.idle);
}

void release_buffers() {
    Locked l;
    drop_queued();
    for (Slot &s : g_slot) {
        if (s.object) gl.DXUnregisterObject(g_interop, s.object);
        s.object = nullptr;
        release(s.tex);
    }
    release(g_back);
}

// The shared texture, made and given to OpenGL one of the ways WGL_NV_DX_interop2 allows: as a
// texture or a renderbuffer, the texture made with Direct3D's sharing flag or not. Drivers differ in
// which they take (a GTX 1650's said its framebuffer was incomplete with the renderbuffer, flag
// off), so each is tried until OpenGL can draw into it; why says how each went
bool share(Slot &s, int w, int h, bool texture, bool shared_flag, std::string &tried) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = (UINT)w, d.Height = (UINT)h, d.MipLevels = 1, d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    d.MiscFlags = shared_flag ? D3D11_RESOURCE_MISC_SHARED : 0;
    char b[160];
    const char *way = texture ? (shared_flag ? "texture, shared" : "texture") : (shared_flag ? "renderbuffer, shared" : "renderbuffer");
    const HRESULT hr = g_dev->CreateTexture2D(&d, nullptr, &s.tex);
    if (FAILED(hr)) {
        snprintf(b, sizeof b, "%s%s: no texture (0x%08lx)", tried.empty() ? "" : "; ", way, (unsigned long)hr);
        tried += b;
        return false;
    }
    s.object = gl.DXRegisterObject(g_interop, s.tex, texture ? s.out_tex : s.rb, texture ? GL_TEXTURE_2D : GL_RENDERBUFFER,
                                   ACCESS_WRITE_DISCARD);
    if (!s.object) {
        snprintf(b, sizeof b, "%s%s: not registered (error 0x%08lx)", tried.empty() ? "" : "; ", way, (unsigned long)GetLastError());
        tried += b;
        release(s.tex);
        return false;
    }
    const BOOL locked = gl.DXLockObjects(g_interop, 1, &s.object);
    gl.BindFramebuffer(GL_FRAMEBUFFER, s.fbo);
    if (texture) gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s.out_tex, 0);
    else gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, s.rb);
    const GLenum status = gl.CheckFramebufferStatus(GL_FRAMEBUFFER);
    if (locked) gl.DXUnlockObjects(g_interop, 1, &s.object);
    if (locked && status == GL_FRAMEBUFFER_COMPLETE) {
        g_shared_as = way, g_as_texture = texture, g_shared_flag = shared_flag;
        return true;
    }
    snprintf(b, sizeof b, "%s%s: %s, framebuffer status 0x%04x", tried.empty() ? "" : "; ", way, locked ? "locked" : "not locked",
             (unsigned)status);
    tried += b;
    if (texture) gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    else gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, 0);
    gl.DXUnregisterObject(g_interop, s.object);
    s.object = nullptr;
    release(s.tex);
    return false;
}

// OpenGL's names for a picture's texture (kept over a resize: the texture is shared with them again)
void name(Slot &s) {
    if (s.fbo) return;
    gl.GenRenderbuffers(1, &s.rb);
    gl.GenTextures(1, &s.out_tex);
    gl.GenFramebuffers(1, &s.fbo);
}

// OpenGL's picture (fbo(): its rows bottom up) into a shared texture top down, as Direct3D's are
void copy_out(Slot &s) {
    gl.DXLockObjects(g_interop, 1, &s.object);
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo);
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, s.fbo);
    gl.BlitFramebuffer(0, 0, g_w, g_h, 0, g_h, g_w, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    gl.DXUnlockObjects(g_interop, 1, &s.object);   // (OpenGL's drawing done before Direct3D reads it)
    gl.BindFramebuffer(GL_FRAMEBUFFER, g_fbo);
}

bool make_buffers(int w, int h, std::string &why) {
    if (FAILED(g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&g_back))) {
        why = "no back buffer";
        return false;
    }
    std::string tried;
    Slot &s = g_slot[0];
    if (!share(s, w, h, true, false, tried) && !share(s, w, h, false, false, tried) && !share(s, w, h, true, true, tried) &&
        !share(s, w, h, false, true, tried)) {
        why = "OpenGL can't draw into the shared texture (" + tried + ")";
        return false;
    }
    gl.BindTexture(GL_TEXTURE_2D, g_tex);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    gl.BindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_tex, 0);
    if (gl.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        why = "no framebuffer for the picture";
        return false;
    }
    g_w = w, g_h = h;
    return true;
}

// The graphics card OpenGL draws on (its vendor: the same card's adapter), else the default one
IDXGIAdapter1 *adapter_for_gl(IDXGIFactory1 *f) {
    const char *v = (const char *)gl.GetString(GL_VENDOR);
    const std::string vendor = v ? v : "";
    UINT id = vendor.find("NVIDIA") != std::string::npos                                              ? 0x10de
              : vendor.find("AMD") != std::string::npos || vendor.find("ATI") != std::string::npos ? 0x1002
              : vendor.find("Intel") != std::string::npos                                          ? 0x8086
                                                                                                    : 0;
    IDXGIAdapter1 *a = nullptr;
    for (UINT i = 0; id && f->EnumAdapters1(i, &a) == S_OK; i++) {
        DXGI_ADAPTER_DESC1 d{};
        if (a->GetDesc1(&d) == S_OK && d.VendorId == id && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) return a;
        release(a);
    }
    return nullptr;
}

// QPC counts to prof::now_ns()'s clock
int64_t qpc_ns(int64_t qpc) {
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (int64_t)prof::now_ns() - (int64_t)((double)(now.QuadPart - qpc) * 1e9 / (double)f.QuadPart);
}

// last_present() and shown(), under q.mu
uint32_t last_present_locked() {
    UINT n = 0;
    if (g_swap && FAILED(g_swap->GetLastPresentCount(&n))) n = 0;
    return n;
}

bool shown_locked(Shown &s) {
    if (!g_on) return false;
    DXGI_FRAME_STATISTICS st{};
    s.direct = false;
    if (g_media) {
        DXGI_FRAME_STATISTICS_MEDIA m{};
        if (FAILED(g_media->GetFrameStatisticsMedia(&m))) return false;
        st.PresentCount = m.PresentCount, st.PresentRefreshCount = m.PresentRefreshCount;
        st.SyncRefreshCount = m.SyncRefreshCount, st.SyncQPCTime = m.SyncQPCTime;
        s.direct = m.CompositionMode == DXGI_FRAME_PRESENTATION_MODE_OVERLAY || m.CompositionMode == DXGI_FRAME_PRESENTATION_MODE_NONE;
    } else if (FAILED(g_swap->GetFrameStatistics(&st))) {
        return false;
    }
    const uint32_t last = last_present_locked();
    if (!st.PresentCount || st.PresentCount > last || !st.SyncQPCTime.QuadPart) return false;
    s.id = st.PresentCount;
    s.vblank_ns = qpc_ns(st.SyncQPCTime.QuadPart);
    s.since = st.SyncRefreshCount >= st.PresentRefreshCount ? st.SyncRefreshCount - st.PresentRefreshCount : 0;
    s.queued = g_queued = last - st.PresentCount;
    return true;
}

void present_failed(HRESULT hr) {
    if (!n_failed++) fprintf(stderr, "video: the DXGI swap chain's present failed (0x%08lx)\n", (unsigned long)hr);
}

// The queue's thread: each picture at its time, in the order queued. Before each, the frame
// statistics are read (the picture before it has had its refresh by then) and kept for seen(): the
// game's thread, back at the machine, isn't there to read them
void presenter() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);   // (it sleeps, but for some tens of microseconds a picture)
    std::unique_lock<std::mutex> l(q.mu);
    for (;;) {
        q.wake.wait(l, [] { return q.quit || !q.jobs.empty(); });
        if (q.quit) break;
        if (const int64_t wait = q.jobs.front().at - (int64_t)prof::now_ns(); wait > 0) {
            l.unlock();   // (the picture may be dropped meanwhile: looked at again)
            SDL_DelayPrecise((Uint64)std::min<int64_t>(wait, 50'000'000));
            l.lock();
            continue;
        }
        const Job j = q.jobs.front();
        q.jobs.pop_front();
        Slot &s = g_slot[j.slot];
        uint32_t id = 0;
        Shown st;
        const bool have = shown_locked(st);
        if (have && (q.seen.empty() || q.seen.back().id != st.id)) {
            if (q.seen.size() >= 64) q.seen.erase(q.seen.begin());
            q.seen.push_back(st);
        }
        if (have && st.queued >= 2) {   // two waiting already (this one would show two refreshes late): left out
            n_left_out++;
        } else {
            const int64_t late = (int64_t)prof::now_ns() - j.at;
            g_ctx->CopyResource(g_back, s.tex);
            const HRESULT hr = g_swap->Present(1, 0);   // at the refresh after the last picture's
            if (FAILED(hr)) {
                present_failed(hr);
            } else {
                id = last_present_locked();
                n_presents++, n_queued++;
                g_queue_late_sum += late, g_queue_late_worst = std::max(g_queue_late_worst, late);
                if (late > 1'000'000) n_queue_late++;
            }
        }
        s.waiting = false;
        if (q.went.size() >= 256) q.went.erase(q.went.begin());
        q.went.push_back({j.ticket, id});
        if (q.jobs.empty()) SetEvent(q.idle);
    }
    q.gone = true;
    l.unlock();
    SDL_CleanupTLS();   // (SDL's timer of this thread's, for its waits)
}

void stop_presenter() {
    std::thread *t = nullptr;
    {
        Locked l;
        drop_queued();
        t = q.thread, q.thread = nullptr;
        q.quit = t != nullptr;
    }
    if (!t) return;
    q.wake.notify_all();
    for (bool gone = false; !gone; Sleep(gone ? 0 : 1)) {   // (out of any present first: Locked)
        Locked l;
        gone = q.gone;
    }
    t->join();
    delete t;
    Locked l;
    q.quit = q.gone = false;
}

}  // namespace

bool start(void *hwnd, int w, int h, std::string &why) {
    if (getenv("HK_NO_DXGI")) {
        why = "HK_NO_DXGI is set";
        return false;
    }
    if (w <= 0 || h <= 0) {
        why = "the window has no size";
        return false;
    }
    g_hwnd = (HWND)hwnd;
    ExtensionsFn extensions = nullptr;
    get(extensions, "wglGetExtensionsStringARB");
    HDC dc = GetDC(g_hwnd);
    const char *ext = extensions ? extensions(dc) : nullptr;
    const bool interop = ext && strstr(ext, "WGL_NV_DX_interop2");
    ReleaseDC(g_hwnd, dc);
    if (!interop) {
        why = "the graphics driver can't share OpenGL's pictures with Direct3D (no WGL_NV_DX_interop2)";
        return false;
    }
    if (!get(gl.GetString, "glGetString") || !get(gl.GenFramebuffers, "glGenFramebuffers") ||
        !get(gl.DeleteFramebuffers, "glDeleteFramebuffers") || !get(gl.BindFramebuffer, "glBindFramebuffer") ||
        !get(gl.FramebufferRenderbuffer, "glFramebufferRenderbuffer") || !get(gl.FramebufferTexture2D, "glFramebufferTexture2D") ||
        !get(gl.CheckFramebufferStatus, "glCheckFramebufferStatus") || !get(gl.BlitFramebuffer, "glBlitFramebuffer") ||
        !get(gl.GenRenderbuffers, "glGenRenderbuffers") || !get(gl.DeleteRenderbuffers, "glDeleteRenderbuffers") ||
        !get(gl.GenTextures, "glGenTextures") || !get(gl.DeleteTextures, "glDeleteTextures") ||
        !get(gl.BindTexture, "glBindTexture") || !get(gl.TexImage2D, "glTexImage2D") || !get(gl.TexParameteri, "glTexParameteri") ||
        !get(gl.DXOpenDevice, "wglDXOpenDeviceNV") || !get(gl.DXCloseDevice, "wglDXCloseDeviceNV") ||
        !get(gl.DXRegisterObject, "wglDXRegisterObjectNV") || !get(gl.DXUnregisterObject, "wglDXUnregisterObjectNV") ||
        !get(gl.DXLockObjects, "wglDXLockObjectsNV") || !get(gl.DXUnlockObjects, "wglDXUnlockObjectsNV")) {
        why = "OpenGL lacks a call the sharing needs";
        return false;
    }
    IDXGIFactory2 *factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory2), (void **)&factory))) {
        why = "no DXGI 1.2 (Windows 8 or later)";
        return false;
    }
    IDXGIAdapter1 *adapter = adapter_for_gl(factory);
    if (adapter) {
        DXGI_ADAPTER_DESC1 d{};
        adapter->GetDesc1(&d);
        char name[128];
        WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof name, nullptr, nullptr);
        g_adapter = name;
    }
    const HRESULT made = D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                           D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &g_dev, nullptr, &g_ctx);
    release(adapter);
    if (FAILED(made)) {
        release(factory);
        why = "no Direct3D 11 device";
        return false;
    }
    // the device's context is used from two threads once pictures are queued (the game's thread and
    // the driver's sharing on it, the queue's): Direct3D keeps its calls apart itself then
    ID3D10Multithread *mt = nullptr;
    const bool guarded = SUCCEEDED(g_ctx->QueryInterface(__uuidof(ID3D10Multithread), (void **)&mt));
    if (guarded) mt->SetMultithreadProtected(TRUE), release(mt);
    g_interop = gl.DXOpenDevice(g_dev);
    if (!g_interop) {
        release(factory);
        stop();
        why = "the graphics driver won't share OpenGL's pictures with Direct3D (wglDXOpenDeviceNV)";
        return false;
    }
    DXGI_SWAP_CHAIN_DESC1 d{};
    d.Width = (UINT)w, d.Height = (UINT)h;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 3;
    d.Scaling = DXGI_SCALING_STRETCH;
    d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;   // Windows 10; 8 and 8.1 have the sequential one
    g_effect = "flip discard";
    HRESULT hr = factory->CreateSwapChainForHwnd(g_dev, g_hwnd, &d, nullptr, nullptr, &g_swap);
    if (FAILED(hr)) {
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        g_effect = "flip sequential";
        hr = factory->CreateSwapChainForHwnd(g_dev, g_hwnd, &d, nullptr, nullptr, &g_swap);
    }
    if (SUCCEEDED(hr)) factory->MakeWindowAssociation(g_hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    release(factory);
    if (FAILED(hr)) {
        char b[96];
        snprintf(b, sizeof b, "no flip-model swap chain on the window (0x%08lx)", (unsigned long)hr);
        stop();
        why = b;
        return false;
    }
    // presents queued ahead of the screen: two at most (display pacing keeps it to one, present()
    // leaves extra pictures out past that)
    IDXGIDevice1 *dd = nullptr;
    if (SUCCEEDED(g_dev->QueryInterface(__uuidof(IDXGIDevice1), (void **)&dd))) dd->SetMaximumFrameLatency(2), release(dd);
    if (FAILED(g_swap->QueryInterface(__uuidof(IDXGISwapChainMedia), (void **)&g_media))) g_media = nullptr;
    name(g_slot[0]);
    gl.GenFramebuffers(1, &g_fbo);
    gl.GenTextures(1, &g_tex);
    gl.BindTexture(GL_TEXTURE_2D, g_tex);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    if (!make_buffers(w, h, why)) {
        stop();
        return false;
    }
    g_on = true;
    g_queues = guarded && !getenv("HK_DXGI_NO_QUEUE");
    return true;
}

bool on() { return g_on; }
unsigned fbo() { return g_on ? g_fbo : 0; }

void resize(int w, int h) {
    if (!g_on || w <= 0 || h <= 0 || (w == g_w && h == g_h)) return;
    release_buffers();   // (the pictures still queued are given up)
    std::string why;
    HRESULT hr;
    {
        Locked l;
        hr = g_swap->ResizeBuffers(0, (UINT)w, (UINT)h, DXGI_FORMAT_UNKNOWN, 0);
    }
    if (FAILED(hr) || !make_buffers(w, h, why)) {
        fprintf(stderr, "video: the DXGI swap chain can't take the window's new size (%s): OpenGL's swap from now on\n",
                FAILED(hr) ? "ResizeBuffers" : why.c_str());
        stop();
    }
}

uint32_t last_present() {
    Locked l;
    return last_present_locked();
}

bool shown(Shown &s) {
    Locked l;
    return shown_locked(s);
}

void settle() {
    if (g_on) Locked l(true);
}

bool present(bool in_turn, bool extra) {
    if (!g_on) return false;
    {
        Locked l(true);   // the pictures queued before it first
        if (extra) {   // two waiting already (this one would show two refreshes late): left out
            Shown s;
            if (shown_locked(s) && s.queued >= 2) {
                n_left_out++;
                gl.BindFramebuffer(GL_FRAMEBUFFER, g_fbo);
                return false;
            }
        }
    }
    copy_out(g_slot[0]);
    Locked l;
    g_ctx->CopyResource(g_back, g_slot[0].tex);
    // in turn: at the refresh after the last picture's; else (display sync off) at the next refresh,
    // in place of one still waiting for it, as OpenGL's swap did
    const HRESULT hr = g_swap->Present(in_turn ? 1 : 0, 0);
    if (FAILED(hr)) {
        present_failed(hr);
        return false;
    }
    n_presents++;
    return true;
}

bool queues() { return g_on && g_queues; }

bool queue(int64_t at_ns, uint32_t ticket) {
    if (!queues()) return false;
    int n = 1;
    {
        Locked l;
        while (n < SLOTS && g_slot[n].waiting) n++;
    }
    if (n == SLOTS) {   // (its thread far behind: a present that waited)
        n_no_room++;
        return false;
    }
    Slot &s = g_slot[n];
    if (!s.tex) {   // its texture, shared as the frame's own is
        std::string tried;
        name(s);
        const bool made = share(s, g_w, g_h, g_as_texture, g_shared_flag, tried);
        gl.BindFramebuffer(GL_FRAMEBUFFER, g_fbo);
        if (!made) {
            fprintf(stderr, "video: no more textures to share with OpenGL (%s): a frame's other pictures wait for their "
                            "times on the game's thread from now on\n", tried.c_str());
            g_queues = false;
            return false;
        }
    }
    copy_out(s);
    Locked l;
    if (!q.thread) q.thread = new std::thread(presenter);
    s.waiting = true;
    // (a time far off would hold the frame's own picture after it: never more than a few frames)
    q.jobs.push_back({n, std::min(at_ns, (int64_t)prof::now_ns() + 250'000'000), ticket});
    ResetEvent(q.idle);
    q.wake.notify_one();
    return true;
}

size_t went(Went *out, size_t max) {
    Locked l;
    const size_t n = std::min(max, q.went.size());
    std::copy(q.went.begin(), q.went.begin() + (long)n, out);
    q.went.erase(q.went.begin(), q.went.begin() + (long)n);
    return n;
}

size_t seen(Shown *out, size_t max) {
    Locked l;
    const size_t n = std::min(max, q.seen.size());
    std::copy(q.seen.begin(), q.seen.begin() + (long)n, out);
    q.seen.erase(q.seen.begin(), q.seen.begin() + (long)n);
    return n;
}

void stop() {
    stop_presenter();
    if (g_interop) release_buffers();
    if (g_tex) gl.DeleteTextures(1, &g_tex);
    if (g_fbo) gl.DeleteFramebuffers(1, &g_fbo);
    for (Slot &s : g_slot) {
        if (s.fbo) gl.DeleteFramebuffers(1, &s.fbo);
        if (s.rb) gl.DeleteRenderbuffers(1, &s.rb);
        if (s.out_tex) gl.DeleteTextures(1, &s.out_tex);
        s.fbo = s.rb = s.out_tex = 0;
    }
    g_tex = g_fbo = 0;
    if (g_interop) gl.DXCloseDevice(g_interop);
    g_interop = nullptr;
    release(g_media);
    release(g_swap);
    release(g_ctx);
    release(g_dev);
    g_on = g_queues = false;
}

std::string describe() {
    if (!g_swap && !n_presents) return {};
    char b[400];
    snprintf(b, sizeof b, "video: pictures presented through a DXGI flip-model swap chain (%s%s%s; OpenGL's picture shared as a %s): "
             "%llu presented, %llu in-between pictures left out (two already waiting)%s",
             g_effect, g_adapter.empty() ? "" : ", on ", g_adapter.c_str(), g_shared_as, (unsigned long long)n_presents,
             (unsigned long long)n_left_out, n_failed ? (", " + std::to_string(n_failed) + " presents failed").c_str() : "");
    std::string out = b;
    // the queue's thread: how near their times its pictures went
    Locked l;
    if (n_queued || n_no_room || n_dropped) {
        snprintf(b, sizeof b, "\nvideo: of them %llu in-between pictures presented at their times by the swap chain's own thread "
                 "(%.2f ms after on average, %llu over 1 ms, the worst %.1f ms); %llu more left out with no texture free, "
                 "%llu given up (a resize, the end)",
                 (unsigned long long)n_queued, n_queued ? (double)g_queue_late_sum / (double)n_queued / 1e6 : 0.0,
                 (unsigned long long)n_queue_late, (double)g_queue_late_worst / 1e6, (unsigned long long)n_no_room,
                 (unsigned long long)n_dropped);
        out += b;
    }
    return out;
}

}  // namespace hk::host::dxgi
