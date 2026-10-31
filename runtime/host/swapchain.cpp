// SPDX-License-Identifier: GPL-3.0-or-later
#include "swapchain.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_3.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

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
ID3D11Texture2D *g_shared = nullptr;      // OpenGL's picture, the right way up
HANDLE g_interop = nullptr, g_object = nullptr;
GLuint g_rb = 0, g_out_fbo = 0;           // the shared texture as OpenGL's renderbuffer, and its framebuffer
GLuint g_tex = 0, g_fbo = 0;              // OpenGL's own picture of the window (fbo())
int g_w = 0, g_h = 0;
const char *g_effect = "";
std::string g_adapter;
uint64_t n_presents = 0, n_left_out = 0, n_failed = 0;
uint32_t g_queued = 0;

template <typename T> void release(T *&p) {
    if (p) p->Release();
    p = nullptr;
}

void release_buffers() {
    if (g_object) gl.DXUnregisterObject(g_interop, g_object);
    g_object = nullptr;
    release(g_shared);
    release(g_back);
}

bool make_buffers(int w, int h, std::string &why) {
    if (FAILED(g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&g_back))) {
        why = "no back buffer";
        return false;
    }
    D3D11_TEXTURE2D_DESC d{};
    d.Width = (UINT)w, d.Height = (UINT)h, d.MipLevels = 1, d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &g_shared))) {
        why = "no texture to share";
        return false;
    }
    g_object = gl.DXRegisterObject(g_interop, g_shared, g_rb, GL_RENDERBUFFER, ACCESS_WRITE_DISCARD);
    if (!g_object) {
        why = "OpenGL can't take Direct3D's texture (wglDXRegisterObjectNV)";
        return false;
    }
    gl.BindFramebuffer(GL_FRAMEBUFFER, g_out_fbo);
    gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, g_rb);
    gl.DXLockObjects(g_interop, 1, &g_object);
    const GLenum status = gl.CheckFramebufferStatus(GL_FRAMEBUFFER);
    gl.DXUnlockObjects(g_interop, 1, &g_object);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        why = "OpenGL can't draw into the shared texture";
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
    gl.GenRenderbuffers(1, &g_rb);
    gl.GenFramebuffers(1, &g_out_fbo);
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
    return true;
}

bool on() { return g_on; }
unsigned fbo() { return g_on ? g_fbo : 0; }

void resize(int w, int h) {
    if (!g_on || w <= 0 || h <= 0 || (w == g_w && h == g_h)) return;
    release_buffers();
    std::string why;
    const HRESULT hr = g_swap->ResizeBuffers(0, (UINT)w, (UINT)h, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr) || !make_buffers(w, h, why)) {
        fprintf(stderr, "video: the DXGI swap chain can't take the window's new size (%s): OpenGL's swap from now on\n",
                FAILED(hr) ? "ResizeBuffers" : why.c_str());
        stop();
    }
}

uint32_t last_present() {
    UINT n = 0;
    if (g_swap && FAILED(g_swap->GetLastPresentCount(&n))) n = 0;
    return n;
}

bool shown(Shown &s) {
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
    const uint32_t last = last_present();
    if (!st.PresentCount || st.PresentCount > last || !st.SyncQPCTime.QuadPart) return false;
    s.id = st.PresentCount;
    s.vblank_ns = qpc_ns(st.SyncQPCTime.QuadPart);
    s.since = st.SyncRefreshCount >= st.PresentRefreshCount ? st.SyncRefreshCount - st.PresentRefreshCount : 0;
    s.queued = g_queued = last - st.PresentCount;
    return true;
}

bool present(bool in_turn, bool extra) {
    if (!g_on) return false;
    if (extra) {   // two waiting already (this one would show two refreshes late): left out
        Shown s;
        if (shown(s) && s.queued >= 2) {
            n_left_out++;
            gl.BindFramebuffer(GL_FRAMEBUFFER, g_fbo);
            return false;
        }
    }
    // OpenGL's picture (its rows bottom up) into the shared texture top down, as Direct3D's are
    gl.DXLockObjects(g_interop, 1, &g_object);
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo);
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, g_out_fbo);
    gl.BlitFramebuffer(0, 0, g_w, g_h, 0, g_h, g_w, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    gl.DXUnlockObjects(g_interop, 1, &g_object);   // (OpenGL's drawing done before Direct3D reads it)
    g_ctx->CopyResource(g_back, g_shared);
    // in turn: at the refresh after the last picture's; else (display sync off) at the next refresh,
    // in place of one still waiting for it, as OpenGL's swap did
    const HRESULT hr = g_swap->Present(in_turn ? 1 : 0, 0);
    gl.BindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    if (FAILED(hr)) {
        if (!n_failed++) fprintf(stderr, "video: the DXGI swap chain's present failed (0x%08lx)\n", (unsigned long)hr);
        return false;
    }
    n_presents++;
    return true;
}

void stop() {
    if (g_interop) release_buffers();
    if (g_tex) gl.DeleteTextures(1, &g_tex);
    if (g_fbo) gl.DeleteFramebuffers(1, &g_fbo);
    if (g_out_fbo) gl.DeleteFramebuffers(1, &g_out_fbo);
    if (g_rb) gl.DeleteRenderbuffers(1, &g_rb);
    g_tex = g_fbo = g_out_fbo = g_rb = 0;
    if (g_interop) gl.DXCloseDevice(g_interop);
    g_interop = nullptr;
    release(g_media);
    release(g_swap);
    release(g_ctx);
    release(g_dev);
    g_on = false;
}

std::string describe() {
    if (!g_swap && !n_presents) return {};
    char b[320];
    snprintf(b, sizeof b, "video: pictures presented through a DXGI flip-model swap chain (%s%s%s): %llu presented, %llu in-between "
             "pictures left out (two already waiting)%s",
             g_effect, g_adapter.empty() ? "" : ", on ", g_adapter.c_str(), (unsigned long long)n_presents,
             (unsigned long long)n_left_out, n_failed ? (", " + std::to_string(n_failed) + " presents failed").c_str() : "");
    return b;
}

}  // namespace hk::host::dxgi
