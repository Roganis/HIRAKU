/* SPDX-License-Identifier: GPL-3.0-or-later */
/* fake_d3d.h's pretend device, swap chain, display and OpenGL. Only what swapchain.cpp calls is
   there: any other method is a null in its table. */
#define COBJMACROS
#define CINTERFACE
#define INITGUID
#include <windows.h>
#include <d3d10.h>
#include <d3d11.h>
#include <dxgi1_3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fake_d3d.h"

volatile long long fake_drawn = 0;
volatile long fake_errors = 0;

static void error(const char *what) {
    InterlockedIncrement(&fake_errors);
    fprintf(stderr, "pretend Direct3D: %s\n", what);
}

/* --- the display --------------------------------------------------------------------------- */

static long long g_period = 1000000000ll / 240, g_compose = 1500000;
static CRITICAL_SECTION g_cs;
static int g_cs_made = 0;

void fake_display(double hz, long long compose_ns) {
    g_period = (long long)(1e9 / hz + 0.5);
    g_compose = compose_ns;
}

long long fake_now_ns(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (long long)((double)t.QuadPart * 1e9 / (double)f.QuadPart);
}

static long long ns_to_qpc(long long ns) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return (long long)((double)ns * (double)f.QuadPart / 1e9);
}

long long fake_last_vblank_ns(long long *period_ns) {
    if (period_ns) *period_ns = g_period;
    return fake_now_ns() / g_period * g_period;
}

/* --- textures ------------------------------------------------------------------------------ */

typedef struct {
    ID3D11Texture2D iface;
    LONG refs;
    long long stamp;   /* the picture in it */
    int gl_locked;     /* OpenGL holds it (wglDXLockObjectsNV) */
} Tex;

static HRESULT STDMETHODCALLTYPE tex_qi(ID3D11Texture2D *t, REFIID riid, void **out) {
    (void)t, (void)riid;
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE tex_addref(ID3D11Texture2D *t) { return (ULONG)InterlockedIncrement(&((Tex *)t)->refs); }
static ULONG STDMETHODCALLTYPE tex_release(ID3D11Texture2D *t) {
    const LONG n = InterlockedDecrement(&((Tex *)t)->refs);
    if (!n && ((Tex *)t)->gl_locked) error("a texture freed while OpenGL held it");
    return (ULONG)n;   /* (never freed: a few dozen in a run) */
}
static ID3D11Texture2DVtbl tex_vtbl;

static Tex *tex_new(void) {
    Tex *t = (Tex *)calloc(1, sizeof *t);
    tex_vtbl.QueryInterface = tex_qi, tex_vtbl.AddRef = tex_addref, tex_vtbl.Release = tex_release;
    t->iface.lpVtbl = &tex_vtbl;
    t->refs = 1;
    t->stamp = -1;
    return t;
}

static Tex g_back_tex;   /* the swap chain's back buffer */

/* --- who is in Direct3D -------------------------------------------------------------------- */

static volatile LONG g_inside = 0, g_protected = 0;
static DWORD g_first_thread = 0;

static void enter(void) {
    if (InterlockedIncrement(&g_inside) != 1) error("two threads in Direct3D at once");
    if (GetCurrentThreadId() != g_first_thread && !g_protected)
        error("a second thread in Direct3D without the device's multithread protection");
}
static void leave(void) { InterlockedDecrement(&g_inside); }

/* --- the swap chain ------------------------------------------------------------------------ */

#define LOG_MAX (1 << 17)
static struct fake_present g_log[LOG_MAX];
static int g_presents = 0;
static long long g_last_shown_vblank = 0;   /* the vblank (its number) the last present is shown at */

int fake_presents(const struct fake_present **log) {
    *log = g_log;
    return g_presents;
}

static HRESULT STDMETHODCALLTYPE swap_qi(IDXGISwapChain1 *s, REFIID riid, void **out) {
    (void)s, (void)riid;   /* (no IDXGISwapChainMedia: the plain statistics) */
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE swap_release(IDXGISwapChain1 *s) {
    (void)s;
    return 0;
}
static HRESULT STDMETHODCALLTYPE swap_get_buffer(IDXGISwapChain1 *s, UINT n, REFIID riid, void **out) {
    (void)s, (void)n, (void)riid;
    enter();
    InterlockedIncrement(&g_back_tex.refs);
    *out = &g_back_tex;
    leave();
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE swap_resize(IDXGISwapChain1 *s, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags) {
    HRESULT hr = S_OK;
    (void)s, (void)n, (void)w, (void)h, (void)f, (void)flags;
    enter();
    if (g_back_tex.refs) error("the swap chain's buffers resized with the back buffer still held"), hr = DXGI_ERROR_INVALID_CALL;
    leave();
    return hr;
}
/* A picture is shown at the first vblank it is in time for (presented g_compose before it), and
   after the last picture's: one a refresh, in the order presented. With three waiting, Present
   waits for the first of them to show, as a flip-model swap chain's does at its latency limit */
static HRESULT STDMETHODCALLTYPE swap_present(IDXGISwapChain1 *s, UINT sync, UINT flags) {
    long long now, v;
    (void)s, (void)flags;
    enter();
    for (;;) {
        now = fake_now_ns();
        if (!sync || g_last_shown_vblank - now / g_period < 3) break;
        Sleep(1);
    }
    v = (now + g_compose + g_period - 1) / g_period;
    EnterCriticalSection(&g_cs);
    if (sync && v <= g_last_shown_vblank) v = g_last_shown_vblank + 1;
    if (v > g_last_shown_vblank) g_last_shown_vblank = v;
    if (g_presents < LOG_MAX) {
        struct fake_present *p = &g_log[g_presents];
        p->id = (unsigned)g_presents + 1;
        p->stamp = g_back_tex.stamp;
        p->at_ns = now;
        p->vblank_ns = v * g_period;
        p->thread = GetCurrentThreadId();
        g_presents++;
    }
    LeaveCriticalSection(&g_cs);
    leave();
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE swap_last_present(IDXGISwapChain1 *s, UINT *n) {
    (void)s;
    enter();
    *n = (UINT)g_presents;
    leave();
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE swap_statistics(IDXGISwapChain1 *s, DXGI_FRAME_STATISTICS *st) {
    const long long now = fake_now_ns(), current = now / g_period;
    int i;
    (void)s;
    enter();
    memset(st, 0, sizeof *st);
    EnterCriticalSection(&g_cs);
    for (i = g_presents - 1; i >= 0 && g_log[i].vblank_ns > current * g_period; i--) {}
    if (i >= 0) {
        st->PresentCount = g_log[i].id;
        st->PresentRefreshCount = (UINT)(g_log[i].vblank_ns / g_period);
        st->SyncRefreshCount = (UINT)current;
        st->SyncQPCTime.QuadPart = ns_to_qpc(current * g_period);
    }
    LeaveCriticalSection(&g_cs);
    leave();
    return S_OK;
}
static IDXGISwapChain1Vtbl swap_vtbl;
static IDXGISwapChain1 g_swap = {&swap_vtbl};

/* --- the device, its context, the factory -------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE mt_qi(ID3D10Multithread *m, REFIID riid, void **out) {
    (void)m, (void)riid;
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE mt_release(ID3D10Multithread *m) {
    (void)m;
    return 0;
}
static WINBOOL STDMETHODCALLTYPE mt_set(ID3D10Multithread *m, WINBOOL on) {
    (void)m;
    return (WINBOOL)InterlockedExchange(&g_protected, on ? 1 : 0);
}
static ID3D10MultithreadVtbl mt_vtbl;
static ID3D10Multithread g_mt = {&mt_vtbl};

static HRESULT STDMETHODCALLTYPE ctx_qi(ID3D11DeviceContext *c, REFIID riid, void **out) {
    (void)c;
    *out = IsEqualGUID(riid, &IID_ID3D10Multithread) ? &g_mt : NULL;
    return *out ? S_OK : E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE ctx_release(ID3D11DeviceContext *c) {
    (void)c;
    return 0;
}
static void STDMETHODCALLTYPE ctx_copy(ID3D11DeviceContext *c, ID3D11Resource *dst, ID3D11Resource *src) {
    (void)c;
    enter();
    if (((Tex *)src)->gl_locked) error("Direct3D copied from a texture OpenGL still held");
    ((Tex *)dst)->stamp = ((Tex *)src)->stamp;
    leave();
}
static ID3D11DeviceContextVtbl ctx_vtbl;
static ID3D11DeviceContext g_ctx = {&ctx_vtbl};

static HRESULT STDMETHODCALLTYPE dev_qi(ID3D11Device *d, REFIID riid, void **out) {
    (void)d, (void)riid;   /* (no IDXGIDevice1: the frame latency stays the default) */
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE dev_release(ID3D11Device *d) {
    (void)d;
    return 0;
}
static HRESULT STDMETHODCALLTYPE dev_texture(ID3D11Device *d, const D3D11_TEXTURE2D_DESC *desc, const D3D11_SUBRESOURCE_DATA *data,
                                             ID3D11Texture2D **out) {
    (void)d, (void)desc, (void)data;
    *out = &tex_new()->iface;
    return S_OK;
}
static ID3D11DeviceVtbl dev_vtbl;
static ID3D11Device g_dev = {&dev_vtbl};

static ULONG STDMETHODCALLTYPE factory_release(IDXGIFactory2 *f) {
    (void)f;
    return 0;
}
static HRESULT STDMETHODCALLTYPE factory_swap(IDXGIFactory2 *f, IUnknown *dev, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1 *desc,
                                              const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *full, IDXGIOutput *output, IDXGISwapChain1 **out) {
    (void)f, (void)dev, (void)hwnd, (void)desc, (void)full, (void)output;
    *out = &g_swap;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE factory_associate(IDXGIFactory2 *f, HWND hwnd, UINT flags) {
    (void)f, (void)hwnd, (void)flags;
    return S_OK;
}
static IDXGIFactory2Vtbl factory_vtbl;
static IDXGIFactory2 g_factory = {&factory_vtbl};

static void make(void) {
    if (g_cs_made) return;
    g_cs_made = 1;
    InitializeCriticalSection(&g_cs);
    swap_vtbl.QueryInterface = swap_qi, swap_vtbl.Release = swap_release, swap_vtbl.GetBuffer = swap_get_buffer;
    swap_vtbl.ResizeBuffers = swap_resize, swap_vtbl.Present = swap_present, swap_vtbl.GetLastPresentCount = swap_last_present;
    swap_vtbl.GetFrameStatistics = swap_statistics;
    mt_vtbl.QueryInterface = mt_qi, mt_vtbl.Release = mt_release, mt_vtbl.SetMultithreadProtected = mt_set;
    ctx_vtbl.QueryInterface = ctx_qi, ctx_vtbl.Release = ctx_release, ctx_vtbl.CopyResource = ctx_copy;
    dev_vtbl.QueryInterface = dev_qi, dev_vtbl.Release = dev_release, dev_vtbl.CreateTexture2D = dev_texture;
    factory_vtbl.Release = factory_release, factory_vtbl.CreateSwapChainForHwnd = factory_swap;
    factory_vtbl.MakeWindowAssociation = factory_associate;
    tex_vtbl.QueryInterface = tex_qi, tex_vtbl.AddRef = tex_addref, tex_vtbl.Release = tex_release;
    g_back_tex.iface.lpVtbl = &tex_vtbl;
    g_back_tex.stamp = -1;
}

/* (the system's own, in d3d11.dll and dxgi.dll, aren't linked) */
HRESULT WINAPI D3D11CreateDevice(IDXGIAdapter *adapter, D3D_DRIVER_TYPE type, HMODULE software, UINT flags, const D3D_FEATURE_LEVEL *levels,
                                 UINT n_levels, UINT sdk, ID3D11Device **dev, D3D_FEATURE_LEVEL *level, ID3D11DeviceContext **ctx) {
    (void)adapter, (void)type, (void)software, (void)flags, (void)levels, (void)n_levels, (void)sdk, (void)level;
    make();
    g_first_thread = GetCurrentThreadId();
    *dev = &g_dev, *ctx = &g_ctx;
    return S_OK;
}

HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void **factory) {
    (void)riid;
    make();
    *factory = &g_factory;
    return S_OK;
}

/* --- OpenGL -------------------------------------------------------------------------------- */

#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_FRAMEBUFFER 0x8D40
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define NAMES 256

typedef struct {
    Tex *tex;
    unsigned name, type;
    int used;
} Shared;   /* a texture registered with the sharing */
static Shared g_shared[NAMES];
static unsigned g_next_name = 1, g_draw_fbo = 0;
static struct {
    unsigned name, type;
} g_attached[NAMES];   /* each framebuffer's colour attachment */

static const unsigned char *WINAPI gl_get_string(unsigned what) {
    (void)what;
    return (const unsigned char *)"Pretend";
}
static const char *WINAPI wgl_extensions(HDC dc) {
    (void)dc;
    return "WGL_ARB_extensions_string WGL_NV_DX_interop WGL_NV_DX_interop2";
}
static void WINAPI gl_gen(int n, unsigned *out) {
    while (n-- > 0) *out++ = g_next_name++;
}
static void WINAPI gl_delete(int n, const unsigned *names) { (void)n, (void)names; }
static void WINAPI gl_bind_framebuffer(unsigned target, unsigned fbo) {
    if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) g_draw_fbo = fbo;
}
static void WINAPI gl_framebuffer_renderbuffer(unsigned target, unsigned attachment, unsigned type, unsigned name) {
    (void)target, (void)attachment;
    if (g_draw_fbo < NAMES) g_attached[g_draw_fbo].name = name, g_attached[g_draw_fbo].type = type;
}
static void WINAPI gl_framebuffer_texture(unsigned target, unsigned attachment, unsigned type, unsigned name, int level) {
    (void)level;
    gl_framebuffer_renderbuffer(target, attachment, type, name);
}
static unsigned WINAPI gl_check(unsigned target) {
    (void)target;
    return GL_FRAMEBUFFER_COMPLETE;
}
/* the picture drawn (fake_drawn) into the draw framebuffer's texture, when that is a shared one */
static void WINAPI gl_blit(int a, int b, int c, int d, int e, int f, int g, int h, unsigned mask, unsigned filter) {
    int i;
    (void)a, (void)b, (void)c, (void)d, (void)e, (void)f, (void)g, (void)h, (void)mask, (void)filter;
    if (g_draw_fbo >= NAMES) return;
    for (i = 0; i < NAMES; i++)
        if (g_shared[i].used && g_shared[i].name == g_attached[g_draw_fbo].name && g_shared[i].type == g_attached[g_draw_fbo].type) {
            if (!g_shared[i].tex->gl_locked) error("OpenGL drew into a shared texture it hadn't locked");
            g_shared[i].tex->stamp = fake_drawn;
        }
}
static void WINAPI gl_bind_texture(unsigned target, unsigned name) { (void)target, (void)name; }
static void WINAPI gl_tex_image(unsigned target, int level, int format, int w, int h, int border, unsigned f, unsigned type, const void *p) {
    (void)target, (void)level, (void)format, (void)w, (void)h, (void)border, (void)f, (void)type, (void)p;
}
static void WINAPI gl_tex_parameter(unsigned target, unsigned name, int value) { (void)target, (void)name, (void)value; }

static HANDLE WINAPI dx_open(void *dev) {
    (void)dev;
    return (HANDLE)(INT_PTR)1;
}
static BOOL WINAPI dx_close(HANDLE dev) {
    (void)dev;
    return TRUE;
}
static HANDLE WINAPI dx_register(HANDLE dev, void *tex, unsigned name, unsigned type, unsigned access) {
    int i;
    (void)dev, (void)access;
    for (i = 0; i < NAMES && g_shared[i].used; i++) {}
    if (i == NAMES) return NULL;
    g_shared[i].tex = (Tex *)tex, g_shared[i].name = name, g_shared[i].type = type, g_shared[i].used = 1;
    return &g_shared[i];
}
static BOOL WINAPI dx_unregister(HANDLE dev, HANDLE object) {
    Shared *s = (Shared *)object;
    (void)dev;
    if (s->tex->gl_locked) error("a shared texture unregistered while OpenGL held it");
    s->used = 0;
    return TRUE;
}
static BOOL WINAPI dx_lock(HANDLE dev, int n, HANDLE *objects) {
    (void)dev;
    while (n-- > 0) {
        Tex *t = ((Shared *)objects[n])->tex;
        if (t->gl_locked) error("a shared texture locked twice");
        t->gl_locked = 1;
    }
    return TRUE;
}
static BOOL WINAPI dx_unlock(HANDLE dev, int n, HANDLE *objects) {
    (void)dev;
    while (n-- > 0) ((Shared *)objects[n])->tex->gl_locked = 0;
    return TRUE;
}

void *fake_get_proc(const char *name) {
    static const struct {
        const char *name;
        void *f;
    } table[] = {
        {"glGetString", (void *)gl_get_string},
        {"wglGetExtensionsStringARB", (void *)wgl_extensions},
        {"glGenFramebuffers", (void *)gl_gen},
        {"glGenRenderbuffers", (void *)gl_gen},
        {"glGenTextures", (void *)gl_gen},
        {"glDeleteFramebuffers", (void *)gl_delete},
        {"glDeleteRenderbuffers", (void *)gl_delete},
        {"glDeleteTextures", (void *)gl_delete},
        {"glBindFramebuffer", (void *)gl_bind_framebuffer},
        {"glFramebufferRenderbuffer", (void *)gl_framebuffer_renderbuffer},
        {"glFramebufferTexture2D", (void *)gl_framebuffer_texture},
        {"glCheckFramebufferStatus", (void *)gl_check},
        {"glBlitFramebuffer", (void *)gl_blit},
        {"glBindTexture", (void *)gl_bind_texture},
        {"glTexImage2D", (void *)gl_tex_image},
        {"glTexParameteri", (void *)gl_tex_parameter},
        {"wglDXOpenDeviceNV", (void *)dx_open},
        {"wglDXCloseDeviceNV", (void *)dx_close},
        {"wglDXRegisterObjectNV", (void *)dx_register},
        {"wglDXUnregisterObjectNV", (void *)dx_unregister},
        {"wglDXLockObjectsNV", (void *)dx_lock},
        {"wglDXUnlockObjectsNV", (void *)dx_unlock},
    };
    size_t i;
    for (i = 0; i < sizeof table / sizeof table[0]; i++)
        if (!strcmp(table[i].name, name)) return table[i].f;
    return NULL;
}
