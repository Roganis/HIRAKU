// SPDX-License-Identifier: GPL-3.0-or-later
// The cabinet's two screens on one adapter (PLAN.md section 32), after spice2x's d3d9 backend and
// games/popn/popn.cpp (GPL-3.0):
//   - The game picks its screens by their connectors (QueryDisplayConfig): DisplayPort #2 the main
//     (1920x1080), HDMI #0 the sub screen (1280x800, the touch screen). The PC's primary monitor is
//     reported as the first, and a monitor that isn't there as the second.
//   - It asks Direct3D 9Ex for an adapter group of two: a second adapter is reported in the first's
//     group, and the group device is made a windowed one-head device plus an additional swap chain
//     for the sub screen (GetSwapChain(1), GetBackBuffer(1, ...)).
//   - The main screen is reported at HK_PC64_MAIN_HZ (the cabinet's 120 by default) and the sub
//     screen at 60, as the cabinet's are.
// The game presents each screen through its own swap chain. HK_PC64_SNAPS=DIR:N writes both
// screens' pictures every N presents to DIR as BMPs (tests).
#define COBJMACROS
#include "pc64.h"

#include <d3d9.h>
#include <stdlib.h>
#include <string.h>

#define SUB_W 1280
#define SUB_H 800

volatile unsigned pc64_frames, pc64_sub_frames;
static IDirect3DSwapChain9 *g_sub;
static IDirect3DDevice9Ex *g_dev;
static IDirect3D9ExVtbl r3d;
static IDirect3DDevice9ExVtbl rdev;
static IDirect3DSwapChain9ExVtbl rsc;

static UINT main_hz(void) {
    static UINT hz;
    if (!hz) hz = getenv("HK_PC64_MAIN_HZ") ? (UINT)atoi(getenv("HK_PC64_MAIN_HZ")) : 120;
    return hz;
}
static void fix_hz(UINT *hz) {   // the PC's 60 Hz main screen reported at the cabinet's rate
    if (*hz >= 59 && *hz <= 61) *hz = main_hz();
}
static void sub_mode(D3DDISPLAYMODE *m) {
    m->Width = SUB_W, m->Height = SUB_H, m->RefreshRate = 60, m->Format = D3DFMT_X8R8G8B8;
}
static void sub_mode_ex(D3DDISPLAYMODEEX *m) {
    D3DDISPLAYMODE d;
    sub_mode(&d);
    m->Width = d.Width, m->Height = d.Height, m->RefreshRate = d.RefreshRate, m->Format = d.Format;
    m->ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
}

// --- IDirect3D9Ex: a second adapter, in the first's group
static UINT WINAPI GetAdapterCount_(IDirect3D9Ex *d) {
    UINT n = r3d.GetAdapterCount(d);
    return n < 2 ? 2 : n;
}
static HRESULT WINAPI GetAdapterIdentifier_(IDirect3D9Ex *d, UINT a, DWORD f, D3DADAPTER_IDENTIFIER9 *id) {
    HRESULT r = r3d.GetAdapterIdentifier(d, a == 1 ? 0 : a, f, id);
    if (a == 1 && SUCCEEDED(r)) strcpy(id->DeviceName, "\\\\.\\DISPLAY_HK_SUB");
    return r;
}
static UINT WINAPI GetAdapterModeCount_(IDirect3D9Ex *d, UINT a, D3DFORMAT f) {
    return a == 1 ? 1 : r3d.GetAdapterModeCount(d, a, f);
}
static HRESULT WINAPI EnumAdapterModes_(IDirect3D9Ex *d, UINT a, D3DFORMAT f, UINT i, D3DDISPLAYMODE *m) {
    if (a == 1) {
        if (i) return D3DERR_INVALIDCALL;
        sub_mode(m);
        return D3D_OK;
    }
    HRESULT r = r3d.EnumAdapterModes(d, a, f, i, m);
    if (SUCCEEDED(r)) fix_hz(&m->RefreshRate);
    return r;
}
static HRESULT WINAPI GetAdapterDisplayMode_(IDirect3D9Ex *d, UINT a, D3DDISPLAYMODE *m) {
    if (a == 1) {
        sub_mode(m);
        return D3D_OK;
    }
    HRESULT r = r3d.GetAdapterDisplayMode(d, a, m);
    if (SUCCEEDED(r)) fix_hz(&m->RefreshRate);
    return r;
}
static HRESULT WINAPI CheckDeviceType_(IDirect3D9Ex *d, UINT a, D3DDEVTYPE t, D3DFORMAT f, D3DFORMAT b, BOOL w) {
    return r3d.CheckDeviceType(d, a == 1 ? 0 : a, t, f, b, w);
}
static HRESULT WINAPI CheckDeviceFormat_(IDirect3D9Ex *d, UINT a, D3DDEVTYPE t, D3DFORMAT f, DWORD u, D3DRESOURCETYPE r, D3DFORMAT c) {
    return r3d.CheckDeviceFormat(d, a == 1 ? 0 : a, t, f, u, r, c);
}
static HRESULT WINAPI GetDeviceCaps_(IDirect3D9Ex *d, UINT a, D3DDEVTYPE t, D3DCAPS9 *c) {
    HRESULT r = r3d.GetDeviceCaps(d, a == 1 ? 0 : a, t, c);
    if (SUCCEEDED(r)) {
        c->NumberOfAdaptersInGroup = a == 0 ? 2 : 0;
        c->MasterAdapterOrdinal = 0;
        c->AdapterOrdinalInGroup = a == 1;
        c->AdapterOrdinal = a;
    }
    return r;
}
static HMONITOR WINAPI GetAdapterMonitor_(IDirect3D9Ex *d, UINT a) {
    return r3d.GetAdapterMonitor(d, a == 1 ? 0 : a);
}
static UINT WINAPI GetAdapterModeCountEx_(IDirect3D9Ex *d, UINT a, const D3DDISPLAYMODEFILTER *f) {
    return a == 1 ? 1 : r3d.GetAdapterModeCountEx(d, a, f);
}
static HRESULT WINAPI EnumAdapterModesEx_(IDirect3D9Ex *d, UINT a, const D3DDISPLAYMODEFILTER *f, UINT i, D3DDISPLAYMODEEX *m) {
    if (a == 1) {
        if (i) return D3DERR_INVALIDCALL;
        sub_mode_ex(m);
        return D3D_OK;
    }
    HRESULT r = r3d.EnumAdapterModesEx(d, a, f, i, m);
    if (SUCCEEDED(r)) fix_hz(&m->RefreshRate);
    return r;
}
static HRESULT WINAPI GetAdapterDisplayModeEx_(IDirect3D9Ex *d, UINT a, D3DDISPLAYMODEEX *m, D3DDISPLAYROTATION *rot) {
    if (a == 1) {
        sub_mode_ex(m);
        if (rot) *rot = D3DDISPLAYROTATION_IDENTITY;
        return D3D_OK;
    }
    HRESULT r = r3d.GetAdapterDisplayModeEx(d, a, m, rot);
    if (SUCCEEDED(r)) fix_hz(&m->RefreshRate);
    return r;
}
static HRESULT WINAPI GetAdapterLUID_(IDirect3D9Ex *d, UINT a, LUID *l) {
    return r3d.GetAdapterLUID(d, a == 1 ? 0 : a, l);
}

// --- the device: the sub screen's swap chain as the group's second
static HRESULT WINAPI GetSwapChain_(IDirect3DDevice9Ex *d, UINT i, IDirect3DSwapChain9 **s) {
    if (i == 1 && g_sub) {
        IDirect3DSwapChain9_AddRef(g_sub);
        *s = g_sub;
        return D3D_OK;
    }
    return rdev.GetSwapChain(d, i, s);
}
static UINT WINAPI GetNumberOfSwapChains_(IDirect3DDevice9Ex *d) {
    return rdev.GetNumberOfSwapChains(d) + (g_sub != NULL);
}
static HRESULT WINAPI GetBackBuffer_(IDirect3DDevice9Ex *d, UINT sc, UINT i, D3DBACKBUFFER_TYPE t, IDirect3DSurface9 **s) {
    if (sc == 1 && g_sub) return IDirect3DSwapChain9_GetBackBuffer(g_sub, i, t, s);
    return rdev.GetBackBuffer(d, sc, i, t, s);
}
static void windowed(D3DPRESENT_PARAMETERS *p) {
    p->Windowed = TRUE;
    p->FullScreen_RefreshRateInHz = 0;
    if (!p->SwapEffect) p->SwapEffect = D3DSWAPEFFECT_DISCARD;
}
// the stages' coordinate sets and the vertices' (the missing sets, below), as Direct3D starts them
static int g_tc_sets = 8, g_tc_shader;
static DWORD g_tc_index[8];
static void tc_reset(void) {
    for (int s = 0; s < 8; s++) g_tc_index[s] = (DWORD)s;
    g_tc_sets = 8, g_tc_shader = 0;
}
static HRESULT WINAPI Reset_(IDirect3DDevice9Ex *d, D3DPRESENT_PARAMETERS *p) {
    windowed(p);
    tc_reset();
    return rdev.Reset(d, p);
}
static HRESULT WINAPI ResetEx_(IDirect3DDevice9Ex *d, D3DPRESENT_PARAMETERS *p, D3DDISPLAYMODEEX *m) {
    (void)m;
    windowed(p);
    tc_reset();
    return rdev.ResetEx(d, p, NULL);
}

// --- pictures: counted, and written now and then for the tests
static void write_bmp(IDirect3DSurface9 *s, const char *path) {
    D3DSURFACE_DESC d;
    IDirect3DSurface9_GetDesc(s, &d);
    IDirect3DSurface9 *sys = NULL;
    if (FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface((IDirect3DDevice9 *)g_dev, d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, NULL)))
        return;
    D3DLOCKED_RECT r;
    if (SUCCEEDED(IDirect3DDevice9_GetRenderTargetData((IDirect3DDevice9 *)g_dev, s, sys)) &&
        SUCCEEDED(IDirect3DSurface9_LockRect(sys, &r, NULL, D3DLOCK_READONLY))) {
        FILE *f = fopen(path, "wb");
        if (f) {
            BITMAPFILEHEADER fh = {0x4d42, 54 + d.Width * d.Height * 4, 0, 0, 54};
            BITMAPINFOHEADER ih = {40, (LONG)d.Width, -(LONG)d.Height, 1, 32, BI_RGB, 0, 0, 0, 0, 0};
            fwrite(&fh, sizeof fh, 1, f);
            fwrite(&ih, sizeof ih, 1, f);
            for (UINT y = 0; y < d.Height; y++) fwrite((BYTE *)r.pBits + y * r.Pitch, 4, d.Width, f);
            fclose(f);
        }
        IDirect3DSurface9_UnlockRect(sys);
    }
    IDirect3DSurface9_Release(sys);
}
static void snapshot(void) {
    static char dir[MAX_PATH];
    static int every = -1;
    if (every < 0) {
        const char *s = getenv("HK_PC64_SNAPS"), *colon = s ? strrchr(s, ':') : NULL;
        every = 0;
        if (colon && colon > s + 1) {   // a drive letter's colon isn't the separator
            snprintf(dir, sizeof dir, "%.*s", (int)(colon - s), s);
            every = atoi(colon + 1);
        }
    }
    if (!every || pc64_frames % every) return;
    char path[MAX_PATH + 32];
    IDirect3DSurface9 *bb;
    if (SUCCEEDED(rdev.GetBackBuffer(g_dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) {
        snprintf(path, sizeof path, "%s\\snap-%06u-main.bmp", dir, pc64_frames);
        write_bmp(bb, path);
        IDirect3DSurface9_Release(bb);
    }
    if (g_sub && SUCCEEDED(IDirect3DSwapChain9_GetBackBuffer(g_sub, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) {
        snprintf(path, sizeof path, "%s\\snap-%06u-sub.bmp", dir, pc64_frames);
        write_bmp(bb, path);
        IDirect3DSurface9_Release(bb);
    }
}
// HK_PC64_SNAPS (tests): the game's 480-line layer (854x480, drawn first, then stretched to the
// main screen) with the snapshots
static HRESULT WINAPI StretchRect_(IDirect3DDevice9Ex *d, IDirect3DSurface9 *src, const RECT *sr, IDirect3DSurface9 *dst, const RECT *dr,
                                   D3DTEXTUREFILTERTYPE f) {
    static int every = -1;
    if (every < 0) {
        const char *e = getenv("HK_PC64_SNAPS"), *colon = e ? strrchr(e, ':') : NULL;
        every = colon && colon > e + 1 ? atoi(colon + 1) : 0;
    }
    D3DSURFACE_DESC sd;
    if (every && src && pc64_frames % every == 0 && SUCCEEDED(IDirect3DSurface9_GetDesc(src, &sd)) && sd.Height == 480) {
        const char *e = getenv("HK_PC64_SNAPS");
        char path[MAX_PATH + 40];
        snprintf(path, sizeof path, "%.*s\\snap-%06u-%ux480.bmp", (int)(strrchr(e, ':') - e), e, pc64_frames, sd.Width);
        write_bmp(src, path);
    }
    return rdev.StretchRect(d, src, sr, dst, dr, f);
}

// The main screen's picture into hiraku-pc's link (HIRAKU's window shows it): read back through a
// surface in system memory, X8R8G8B8 (the desktop's, windowed)
// The single-screen mode's picture (crt.c): the main screen's middle 4:3 scaled down on the GPU in
// two linear steps (1440x1080 to 960x720 to 640x480: each 1.5 times, no pixel skipped), read back
// The touch screen's part a menu shows over it (crt.c's panel): copied at the touch screen's presents
// at the size it's shown (a GPU stretch, linear), read back with the main screen's picture and put
// on it, framed, the item marked. Its whole picture at half its size (640x400) read back for crt.c's
// menus to look at, 10 times a second while they want it.
static IDirect3DSurface9 *g_sub_small, *g_sub_readback, *g_panel, *g_panel_readback;
static struct pc64_panel g_panel_at;   // the panel's place as last copied
static void keep_sub(IDirect3DSwapChain9 *s) {
    struct pc64_panel q;
    const int panel = pc64_crt() && pc64_crt_panel(&q);
    if (!pc64_crt() || (!panel && !pc64_crt_wants_sub())) return;
    IDirect3DSurface9 *bb = NULL;
    if (FAILED(IDirect3DSwapChain9_GetBackBuffer(s, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
    D3DSURFACE_DESC d;
    IDirect3DSurface9_GetDesc(bb, &d);
    IDirect3DDevice9 *dev = (IDirect3DDevice9 *)g_dev;
    if (!g_sub_small &&
        (FAILED(IDirect3DDevice9_CreateRenderTarget(dev, d.Width / 2, d.Height / 2, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &g_sub_small, NULL)) ||
         FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, d.Width / 2, d.Height / 2, d.Format, D3DPOOL_SYSTEMMEM, &g_sub_readback, NULL))))
        g_sub_small = NULL;
    if (g_sub_small) rdev.StretchRect(g_dev, bb, NULL, g_sub_small, NULL, D3DTEXF_LINEAR);
    if (panel && !g_panel &&
        (FAILED(IDirect3DDevice9_CreateRenderTarget(dev, 640, 480, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &g_panel, NULL)) ||
         FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, 640, 480, d.Format, D3DPOOL_SYSTEMMEM, &g_panel_readback, NULL))))
        g_panel = NULL;
    if (panel && g_panel && d.Width == 1280 && d.Height == 800 && q.dw > 0 && q.dh > 0 && q.dw <= 640 && q.dh <= 480) {
        const RECT from = {q.sx, q.sy, q.sx + q.sw, q.sy + q.sh}, to = {0, 0, q.dw, q.dh};
        if (SUCCEEDED(rdev.StretchRect(g_dev, bb, &from, g_panel, &to, D3DTEXF_LINEAR))) g_panel_at = q;
    }
    IDirect3DSurface9_Release(bb);
}

// The touch screen's picture, not shown, read back for crt.c's menus to look at (10 times a second)
static void sub_for_menus(void) {
    static uint32_t px[640 * 400];
    static DWORD at;
    if (GetTickCount() - at < 100) return;
    at = GetTickCount();
    D3DSURFACE_DESC sd;
    D3DLOCKED_RECT lr;
    if (!g_sub_small || FAILED(IDirect3DSurface9_GetDesc(g_sub_small, &sd)) || sd.Width != 640 || sd.Height > 400 ||
        FAILED(IDirect3DDevice9_GetRenderTargetData((IDirect3DDevice9 *)g_dev, g_sub_small, g_sub_readback)) ||
        FAILED(IDirect3DSurface9_LockRect(g_sub_readback, &lr, NULL, D3DLOCK_READONLY)))
        return;
    for (UINT y = 0; y < sd.Height; y++) {
        const uint32_t *p = (const uint32_t *)((const uint8_t *)lr.pBits + (size_t)y * lr.Pitch);
        for (UINT x = 0; x < 640; x++) px[y * 640 + x] = p[x] & 0xffffff;
    }
    IDirect3DSurface9_UnlockRect(g_sub_readback);
    pc64_crt_sub_picture(px, 640, (int)sd.Height);
}

// A frame of width n around (x, y, w, h) on the 640x480, inside it
static void frame(uint32_t *out, int x, int y, int w, int h, int n, uint32_t colour) {
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            if ((i < x + n || i >= x + w - n || j < y + n || j >= y + h - n) && i >= 0 && j >= 0 && i < 640 && j < 480)
                out[j * 640 + i] = colour;
}

// The panel put on the picture: framed in dark blue and white (as the game's panels), its item marked
// in yellow and red, blinking twice a second
static void put_panel(uint32_t *out) {
    struct pc64_panel now;
    if (!g_panel || !pc64_crt_panel(&now)) return;
    const struct pc64_panel q = g_panel_at;
    if (q.sx != now.sx || q.sy != now.sy || q.dw != now.dw || q.dh != now.dh) return;   // (not copied yet)
    D3DLOCKED_RECT lr;
    if (FAILED(IDirect3DDevice9_GetRenderTargetData((IDirect3DDevice9 *)g_dev, g_panel, g_panel_readback)) ||
        FAILED(IDirect3DSurface9_LockRect(g_panel_readback, &lr, NULL, D3DLOCK_READONLY)))
        return;
    for (int y = 0; y < q.dh && q.dy + y < 480; y++) {
        const uint32_t *p = (const uint32_t *)((const uint8_t *)lr.pBits + (size_t)y * lr.Pitch);
        uint32_t *o = out + (size_t)(q.dy + y) * 640 + q.dx;
        for (int x = 0; x < q.dw && q.dx + x < 640; x++) o[x] = p[x] & 0xffffff;
    }
    IDirect3DSurface9_UnlockRect(g_panel_readback);
    frame(out, q.dx - 5, q.dy - 5, q.dw + 10, q.dh + 10, 3, 0x172a8c);
    frame(out, q.dx - 2, q.dy - 2, q.dw + 4, q.dh + 4, 2, 0xffffff);
    if (now.hw > 0) {   // the item: its place on the touch screen to the panel's
        const int x = q.dx + (now.hx - q.sx) * q.dw / q.sw, y = q.dy + (now.hy - q.sy) * q.dh / q.sh;
        const int w = now.hw * q.dw / q.sw, h = now.hh * q.dh / q.sh;
        const int on = GetTickCount() / 250 % 2;
        frame(out, x - 1, y - 1, w + 2, h + 2, 4, on ? 0xffe000 : 0xff3050);
        frame(out, x - 2, y - 2, w + 4, h + 4, 1, 0x000000);
    }
}

static int to_link_crt(IDirect3DSurface9 *bb, const D3DSURFACE_DESC *d, uint32_t *out) {
    enum { W = 640, H = 480 };
    static IDirect3DSurface9 *half, *small, *readback;
    if (pc64_crt_wants_sub()) sub_for_menus();
    if (!readback) {
        IDirect3DDevice9 *dev = (IDirect3DDevice9 *)g_dev;
        if (FAILED(IDirect3DDevice9_CreateRenderTarget(dev, W * 3 / 2, H * 3 / 2, d->Format, D3DMULTISAMPLE_NONE, 0, FALSE, &half, NULL)) ||
            FAILED(IDirect3DDevice9_CreateRenderTarget(dev, W, H, d->Format, D3DMULTISAMPLE_NONE, 0, FALSE, &small, NULL)) ||
            FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, W, H, d->Format, D3DPOOL_SYSTEMMEM, &readback, NULL)))
            return 0;
        pc64_log("the single-screen mode: the main screen's middle 4:3 at %ux%u to HIRAKU's window", W, H);
    }
    if (pc64_link_not_shown()) {   // counted, not read back
        pc64_link_picture(W, H);
        return 1;
    }
    const UINT cw = d->Height * 4 / 3;
    const RECT crop = {(LONG)(d->Width - cw) / 2, 0, (LONG)(d->Width + cw) / 2, (LONG)d->Height};
    D3DLOCKED_RECT lr;
    if (FAILED(rdev.StretchRect(g_dev, bb, &crop, half, NULL, D3DTEXF_LINEAR)) ||
        FAILED(rdev.StretchRect(g_dev, half, NULL, small, NULL, D3DTEXF_LINEAR)) ||
        FAILED(IDirect3DDevice9_GetRenderTargetData((IDirect3DDevice9 *)g_dev, small, readback)) ||
        FAILED(IDirect3DSurface9_LockRect(readback, &lr, NULL, D3DLOCK_READONLY)))
        return 0;
    for (UINT y = 0; y < H; y++) {
        const uint32_t *p = (const uint32_t *)((const uint8_t *)lr.pBits + (size_t)y * lr.Pitch);
        uint32_t *o = out + (size_t)y * W;
        for (UINT x = 0; x < W; x++) o[x] = p[x] & 0xffffff;
    }
    IDirect3DSurface9_UnlockRect(readback);
    put_panel(out);
    pc64_link_picture(W, H);
    return 1;
}

static int to_link(IDirect3DSwapChain9 *s) {
    static IDirect3DSurface9 *readback;
    static UINT rw, rh;
    uint32_t *out = pc64_link_pixels();
    IDirect3DSurface9 *bb = NULL;
    if (!out || FAILED(IDirect3DSwapChain9_GetBackBuffer(s, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return 0;
    D3DSURFACE_DESC d;
    IDirect3DSurface9_GetDesc(bb, &d);
    if (pc64_crt()) {
        const int done = to_link_crt(bb, &d, out);
        IDirect3DSurface9_Release(bb);
        return done;
    }
    int ok = 0;
    if (readback && rw == d.Width && rh == d.Height && pc64_link_not_shown()) {   // counted, not read back
        IDirect3DSurface9_Release(bb);
        pc64_link_picture(d.Width, d.Height);
        return 1;
    }
    if (d.Width <= 1920 && d.Height <= 1200 && (d.Format == D3DFMT_X8R8G8B8 || d.Format == D3DFMT_A8R8G8B8)) {
        if (!readback || rw != d.Width || rh != d.Height) {
            if (readback) IDirect3DSurface9_Release(readback);
            readback = NULL;
            if (SUCCEEDED(IDirect3DDevice9_CreateOffscreenPlainSurface((IDirect3DDevice9 *)g_dev, d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM,
                                                                       &readback, NULL)))
                rw = d.Width, rh = d.Height, pc64_log("the main screen goes to HIRAKU's window (%ux%u)", d.Width, d.Height);
        }
        D3DLOCKED_RECT lr;
        if (readback && SUCCEEDED(IDirect3DDevice9_GetRenderTargetData((IDirect3DDevice9 *)g_dev, bb, readback)) &&
            SUCCEEDED(IDirect3DSurface9_LockRect(readback, &lr, NULL, D3DLOCK_READONLY))) {
            for (UINT y = 0; y < d.Height; y++) {
                const uint32_t *p = (const uint32_t *)((const uint8_t *)lr.pBits + (size_t)y * lr.Pitch);
                uint32_t *o = out + (size_t)y * d.Width;
                for (UINT x = 0; x < d.Width; x++) o[x] = p[x] & 0xffffff;
            }
            IDirect3DSurface9_UnlockRect(readback);
            ok = 1;
        }
    }
    IDirect3DSurface9_Release(bb);
    if (ok) pc64_link_picture(d.Width, d.Height);
    return ok;
}

static HRESULT WINAPI SwapChainPresent_(IDirect3DSwapChain9Ex *s, const RECT *a, const RECT *b, HWND w, const RGNDATA *r, DWORD f) {
    if ((void *)s == (void *)g_sub) {
        pc64_sub_frames++;
        keep_sub((IDirect3DSwapChain9 *)s);
    } else {
        pc64_frames++;
        snapshot();
        if (pc64_host_shows() && to_link((IDirect3DSwapChain9 *)s)) return D3D_OK;   // not shown by the game's window
    }
    return rsc.Present(s, a, b, w, r, f);
}

// --- the windows
// The game's main screen window stays hidden while HIRAKU's window shows its pictures. The sub
// screen's goes where the PC has room for it, wherever the game puts it (it places it below the
// main screen it was told of, a place that isn't on the PC):
//   - HK_PC64_SUB=X,Y,W,H: there, without a frame (W and H its picture's size);
//   - else on a monitor other than the primary, filling it without a frame, as the cabinet's;
//   - else a window with a frame, resizable, at most half the screen's width and height, in the
//     work area's lower right corner.
// While HIRAKU's window shows the main screen it keeps the keyboard: the sub screen's window doesn't
// take it when touched (WS_EX_NOACTIVATE), and the game's SetForegroundWindow is ignored.
static BOOL(WINAPI *r_ShowWindow)(HWND, int);
static BOOL(WINAPI *r_MoveWindow)(HWND, int, int, int, int, BOOL);

static int is_window(HWND w, const char *which) {
    char title[64] = "";
    GetWindowTextA(w, title, sizeof title);
    return !strcmp(title, which);
}
static BOOL CALLBACK other_monitor(HMONITOR m, HDC dc, LPRECT r, LPARAM out) {
    (void)dc, (void)r;
    MONITORINFO mi;
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoA(m, &mi) || (mi.dwFlags & MONITORINFOF_PRIMARY)) return TRUE;
    *(RECT *)out = mi.rcMonitor;
    return FALSE;
}
static void place_sub(HWND w) {
    static int said;
    RECT at = {0};
    int bare = 1, x, y, cw, ch;
    const char *env = getenv("HK_PC64_SUB");
    if (env && sscanf(env, "%d,%d,%d,%d", &x, &y, &cw, &ch) == 4 && cw > 0 && ch > 0) SetRect(&at, x, y, x + cw, y + ch);
    else EnumDisplayMonitors(NULL, NULL, other_monitor, (LPARAM)&at);
    if (at.right <= at.left) {
        MONITORINFO mi;
        memset(&mi, 0, sizeof mi);
        mi.cbSize = sizeof mi;
        GetMonitorInfoA(MonitorFromPoint((POINT){0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
        const RECT wa = mi.rcWork;
        const int ww = wa.right - wa.left, wh = wa.bottom - wa.top;
        cw = SUB_W, ch = SUB_H;
        if (cw > ww / 2) cw = ww / 2, ch = cw * SUB_H / SUB_W;
        if (ch > wh / 2) ch = wh / 2, cw = ch * SUB_W / SUB_H;
        bare = 0;
        const DWORD style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
        SetRect(&at, 0, 0, cw, ch);
        AdjustWindowRect(&at, style, FALSE);
        OffsetRect(&at, wa.right - at.right, wa.bottom - at.bottom);
        SetWindowLongPtrA(w, GWL_STYLE, (LONG_PTR)style);
    }
    if (bare) SetWindowLongPtrA(w, GWL_STYLE, (LONG_PTR)(WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS));
    if (pc64_host_shows()) SetWindowLongPtrA(w, GWL_EXSTYLE, GetWindowLongPtrA(w, GWL_EXSTYLE) | WS_EX_NOACTIVATE);
    SetWindowPos(w, NULL, at.left, at.top, at.right - at.left, at.bottom - at.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    if (!said++)
        pc64_log("the sub screen's window at %ld,%ld %ldx%ld%s", at.left, at.top, at.right - at.left, at.bottom - at.top,
                 bare ? ", no frame" : ", a frame");
}
static BOOL WINAPI ShowWindow_(HWND w, int how) {
    if (pc64_host_shows() && is_window(w, "Main Screen")) how = SW_HIDE;
    if (pc64_crt() && is_window(w, "Sub Screen")) return r_ShowWindow(w, SW_HIDE);   // no touch screen: never shown
    const BOOL r = r_ShowWindow(w, how);
    if (how != SW_HIDE && is_window(w, "Sub Screen")) place_sub(w);
    return r;
}
static BOOL WINAPI MoveWindow_(HWND w, int x, int y, int cw, int ch, BOOL paint) {
    if (is_window(w, "Sub Screen") && !pc64_crt()) {
        place_sub(w);
        return TRUE;
    }
    return r_MoveWindow(w, x, y, cw, ch, paint);
}
static BOOL(WINAPI *r_SetForegroundWindow)(HWND);
static BOOL WINAPI SetForegroundWindow_(HWND w) {
    return pc64_host_shows() ? TRUE : r_SetForegroundWindow(w);
}
static HRESULT WINAPI Present_(IDirect3DDevice9Ex *d, const RECT *a, const RECT *b, HWND w, const RGNDATA *r) {
    pc64_frames++;
    snapshot();
    return rdev.Present(d, a, b, w, r);
}
static HRESULT WINAPI PresentEx_(IDirect3DDevice9Ex *d, const RECT *a, const RECT *b, HWND w, const RGNDATA *r, DWORD f) {
    pc64_frames++;
    snapshot();
    return rdev.PresentEx(d, a, b, w, r, f);
}


// A texture stage reading a coordinate set its vertices haven't. afp-core's multiply pass (the music
// select's chart levels) reads a 2x2 white texture at stage 1 through set 1 of vertices that carry
// one set: Direct3D 9 gives the missing set a constant, the white keeps what is under each digit as it
// is; DXVK's coordinates for it sample black (a black square around each digit). Such a stage reads
// set 0 for the draw instead (the texture it reads is one colour all over). Under Wine only.
static HRESULT WINAPI SetFVF_(IDirect3DDevice9Ex *d, DWORD fvf) {
    g_tc_sets = (int)((fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT);
    return rdev.SetFVF(d, fvf);
}
static HRESULT WINAPI SetVertexDeclaration_(IDirect3DDevice9Ex *d, IDirect3DVertexDeclaration9 *decl) {
    D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH + 1];
    UINT n = MAXD3DDECLLENGTH + 1;
    g_tc_sets = 8;
    if (decl && SUCCEEDED(IDirect3DVertexDeclaration9_GetDeclaration(decl, el, &n))) {
        g_tc_sets = 0;
        for (UINT i = 0; i < n && el[i].Stream != 0xff; i++)
            if (el[i].Usage == D3DDECLUSAGE_TEXCOORD && el[i].UsageIndex + 1 > g_tc_sets) g_tc_sets = el[i].UsageIndex + 1;
    }
    return rdev.SetVertexDeclaration(d, decl);
}
static HRESULT WINAPI SetVertexShader_(IDirect3DDevice9Ex *d, IDirect3DVertexShader9 *vs) {
    g_tc_shader = vs != NULL;
    return rdev.SetVertexShader(d, vs);
}
static HRESULT WINAPI SetTextureStageState_(IDirect3DDevice9Ex *d, DWORD stage, D3DTEXTURESTAGESTATETYPE t, DWORD v) {
    if (t == D3DTSS_TEXCOORDINDEX && stage < 8) g_tc_index[stage] = v;
    return rdev.SetTextureStageState(d, stage, t, v);
}
// The stages that read a missing set, pointed at set 0 (a mask: put back after the draw)
static unsigned tc_before(IDirect3DDevice9Ex *d) {
    unsigned moved = 0;
    if (g_tc_shader) return 0;   // a vertex shader's outputs are its own
    for (int s = 0; s < 8; s++)
        if (g_tc_index[s] >> 16 == 0 && (int)(g_tc_index[s] & 0xffff) >= g_tc_sets) rdev.SetTextureStageState(d, s, D3DTSS_TEXCOORDINDEX, 0), moved |= 1u << s;
    return moved;
}
static void tc_after(IDirect3DDevice9Ex *d, unsigned moved) {
    for (int s = 0; moved; s++, moved >>= 1)
        if (moved & 1) rdev.SetTextureStageState(d, s, D3DTSS_TEXCOORDINDEX, g_tc_index[s]);
}
static HRESULT WINAPI DrawPrimitive_(IDirect3DDevice9Ex *d, D3DPRIMITIVETYPE t, UINT a, UINT b) {
    const unsigned m = tc_before(d);
    const HRESULT r = rdev.DrawPrimitive(d, t, a, b);
    tc_after(d, m);
    return r;
}
static HRESULT WINAPI DrawIndexedPrimitive_(IDirect3DDevice9Ex *d, D3DPRIMITIVETYPE t, INT a, UINT b, UINT c, UINT e, UINT f) {
    const unsigned m = tc_before(d);
    const HRESULT r = rdev.DrawIndexedPrimitive(d, t, a, b, c, e, f);
    tc_after(d, m);
    return r;
}
static HRESULT WINAPI DrawPrimitiveUP_(IDirect3DDevice9Ex *d, D3DPRIMITIVETYPE t, UINT a, const void *v, UINT s) {
    const unsigned m = tc_before(d);
    const HRESULT r = rdev.DrawPrimitiveUP(d, t, a, v, s);
    tc_after(d, m);
    return r;
}
static HRESULT WINAPI DrawIndexedPrimitiveUP_(IDirect3DDevice9Ex *d, D3DPRIMITIVETYPE t, UINT a, UINT b, UINT c, const void *i, D3DFORMAT f, const void *v,
                                              UINT s) {
    const unsigned m = tc_before(d);
    const HRESULT r = rdev.DrawIndexedPrimitiveUP(d, t, a, b, c, i, f, v, s);
    tc_after(d, m);
    return r;
}
static HRESULT WINAPI CreateDeviceEx_(IDirect3D9Ex *d, UINT a, D3DDEVTYPE t, HWND focus, DWORD flags, D3DPRESENT_PARAMETERS *pp,
                                      D3DDISPLAYMODEEX *modes, IDirect3DDevice9Ex **dev) {
    (void)modes;
    const int group = (flags & D3DCREATE_ADAPTERGROUP_DEVICE) != 0;
    D3DPRESENT_PARAMETERS sub = group ? pp[1] : (D3DPRESENT_PARAMETERS){0};
    pc64_log("Direct3D: main %ux%u at %u Hz%s", pp[0].BackBufferWidth, pp[0].BackBufferHeight, pp[0].FullScreen_RefreshRateInHz,
             group ? ", and the sub screen" : "");
    windowed(&pp[0]);
    HRESULT r = r3d.CreateDeviceEx(d, a, t, focus ? focus : pp[0].hDeviceWindow, flags & ~D3DCREATE_ADAPTERGROUP_DEVICE, pp, NULL, dev);
    if (FAILED(r)) {
        pc64_log("Direct3D: CreateDeviceEx failed (%lx)", r);
        return r;
    }
    if (!g_dev) {
        g_dev = *dev;
        IDirect3DDevice9ExVtbl *v = (*dev)->lpVtbl;
        rdev = *v;
        pc64_patch(&v->GetSwapChain, GetSwapChain_);
        pc64_patch(&v->GetNumberOfSwapChains, GetNumberOfSwapChains_);
        pc64_patch(&v->GetBackBuffer, GetBackBuffer_);
        pc64_patch(&v->Reset, Reset_);
        pc64_patch(&v->ResetEx, ResetEx_);
        pc64_patch(&v->Present, Present_);
        pc64_patch(&v->StretchRect, StretchRect_);
        pc64_patch(&v->PresentEx, PresentEx_);
        HMODULE nt = GetModuleHandleA("ntdll.dll");
        if (nt && GetProcAddress(nt, "wine_get_version")) {   // the missing coordinate sets (above)
            pc64_patch(&v->SetFVF, SetFVF_);
            pc64_patch(&v->SetVertexDeclaration, SetVertexDeclaration_);
            pc64_patch(&v->SetVertexShader, SetVertexShader_);
            pc64_patch(&v->SetTextureStageState, SetTextureStageState_);
            pc64_patch(&v->DrawPrimitive, DrawPrimitive_);
            pc64_patch(&v->DrawIndexedPrimitive, DrawIndexedPrimitive_);
            pc64_patch(&v->DrawPrimitiveUP, DrawPrimitiveUP_);
            pc64_patch(&v->DrawIndexedPrimitiveUP, DrawIndexedPrimitiveUP_);
            tc_reset();
        }
        IDirect3DSwapChain9 *sc;
        if (SUCCEEDED(rdev.GetSwapChain(*dev, 0, &sc))) {
            IDirect3DSwapChain9ExVtbl *sv = (IDirect3DSwapChain9ExVtbl *)sc->lpVtbl;
            rsc = *sv;
            pc64_patch(&sv->Present, SwapChainPresent_);
            IDirect3DSwapChain9_Release(sc);
        }
    }
    if (group) {
        windowed(&sub);
        if (FAILED(IDirect3DDevice9Ex_CreateAdditionalSwapChain(*dev, &sub, &g_sub))) pc64_log("Direct3D: no swap chain for the sub screen");
    }
    return r;
}
static HRESULT WINAPI CreateDevice_(IDirect3D9Ex *d, UINT a, D3DDEVTYPE t, HWND focus, DWORD flags, D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **dev) {
    windowed(pp);
    return r3d.CreateDevice(d, a, t, focus, flags & ~D3DCREATE_ADAPTERGROUP_DEVICE, pp, dev);
}

static HRESULT(WINAPI *r_Direct3DCreate9Ex)(UINT, IDirect3D9Ex **);
static HRESULT WINAPI Direct3DCreate9Ex_(UINT sdk, IDirect3D9Ex **out) {
    HRESULT r = r_Direct3DCreate9Ex(sdk, out);
    if (SUCCEEDED(r) && !r3d.CreateDeviceEx) {
        IDirect3D9ExVtbl *v = (*out)->lpVtbl;
        r3d = *v;
        pc64_patch(&v->GetAdapterCount, GetAdapterCount_);
        pc64_patch(&v->GetAdapterIdentifier, GetAdapterIdentifier_);
        pc64_patch(&v->GetAdapterModeCount, GetAdapterModeCount_);
        pc64_patch(&v->EnumAdapterModes, EnumAdapterModes_);
        pc64_patch(&v->GetAdapterDisplayMode, GetAdapterDisplayMode_);
        pc64_patch(&v->CheckDeviceType, CheckDeviceType_);
        pc64_patch(&v->CheckDeviceFormat, CheckDeviceFormat_);
        pc64_patch(&v->GetDeviceCaps, GetDeviceCaps_);
        pc64_patch(&v->GetAdapterMonitor, GetAdapterMonitor_);
        pc64_patch(&v->CreateDevice, CreateDevice_);
        pc64_patch(&v->GetAdapterModeCountEx, GetAdapterModeCountEx_);
        pc64_patch(&v->EnumAdapterModesEx, EnumAdapterModesEx_);
        pc64_patch(&v->GetAdapterDisplayModeEx, GetAdapterDisplayModeEx_);
        pc64_patch(&v->CreateDeviceEx, CreateDeviceEx_);
        pc64_patch(&v->GetAdapterLUID, GetAdapterLUID_);
    }
    return r;
}

// --- the monitors' connectors: one more path (the sub screen, at -999999,-999999, as spice2x's)
static LONG(WINAPI *r_GetDisplayConfigBufferSizes)(UINT32, UINT32 *, UINT32 *);
static LONG(WINAPI *r_QueryDisplayConfig)(UINT32, UINT32 *, DISPLAYCONFIG_PATH_INFO *, UINT32 *, DISPLAYCONFIG_MODE_INFO *, DISPLAYCONFIG_TOPOLOGY_ID *);
static LONG(WINAPI *r_DisplayConfigGetDeviceInfo)(DISPLAYCONFIG_DEVICE_INFO_HEADER *);
static UINT32 g_paths, g_modes;   // the PC's own

static LONG WINAPI GetDisplayConfigBufferSizes_(UINT32 f, UINT32 *np, UINT32 *nm) {
    LONG r = r_GetDisplayConfigBufferSizes(f, np, nm);
    if (r == ERROR_SUCCESS) {
        g_paths = *np, g_modes = *nm;
        *np += 1, *nm += 2;
    }
    return r;
}
static void cabinet_rate(DISPLAYCONFIG_RATIONAL *r) {
    if (r->Denominator && r->Numerator / r->Denominator >= 59 && r->Numerator / r->Denominator <= 61)
        r->Numerator = main_hz() * 1000, r->Denominator = 1000;
}
static LONG WINAPI QueryDisplayConfig_(UINT32 f, UINT32 *np, DISPLAYCONFIG_PATH_INFO *paths, UINT32 *nm, DISPLAYCONFIG_MODE_INFO *modes,
                                       DISPLAYCONFIG_TOPOLOGY_ID *topo) {
    UINT32 p = g_paths, m = g_modes;
    LONG r = r_QueryDisplayConfig(f, &p, paths, &m, modes, topo);
    if (r != ERROR_SUCCESS) return r;
    for (UINT32 i = 0; i < p; i++) cabinet_rate(&paths[i].targetInfo.refreshRate);
    for (UINT32 i = 0; i < m; i++)
        if (modes[i].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_TARGET) cabinet_rate(&modes[i].targetMode.targetVideoSignalInfo.vSyncFreq);
    DISPLAYCONFIG_PATH_INFO *path = &paths[p];
    memset(path, 0, sizeof *path);
    path->flags = DISPLAYCONFIG_PATH_ACTIVE;
    path->sourceInfo.adapterId.HighPart = -1, path->sourceInfo.adapterId.LowPart = (DWORD)-1;
    path->sourceInfo.id = (UINT32)-1;
    path->sourceInfo.modeInfoIdx = m;
    path->sourceInfo.statusFlags = DISPLAYCONFIG_SOURCE_IN_USE;
    path->targetInfo.adapterId = path->sourceInfo.adapterId;
    path->targetInfo.id = (UINT32)-2;
    path->targetInfo.modeInfoIdx = m + 1;
    path->targetInfo.outputTechnology = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
    path->targetInfo.rotation = DISPLAYCONFIG_ROTATION_IDENTITY;
    path->targetInfo.scaling = DISPLAYCONFIG_SCALING_IDENTITY;
    path->targetInfo.refreshRate.Numerator = 60000, path->targetInfo.refreshRate.Denominator = 1000;
    path->targetInfo.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
    path->targetInfo.targetAvailable = TRUE;
    path->targetInfo.statusFlags = DISPLAYCONFIG_TARGET_IN_USE;
    DISPLAYCONFIG_MODE_INFO *source = &modes[m], *target = &modes[m + 1];
    memset(source, 0, sizeof *source);
    source->infoType = DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;
    source->id = (UINT32)-1;
    source->adapterId = path->sourceInfo.adapterId;
    source->sourceMode.width = SUB_W, source->sourceMode.height = SUB_H;
    source->sourceMode.pixelFormat = DISPLAYCONFIG_PIXELFORMAT_32BPP;
    source->sourceMode.position.x = -999999, source->sourceMode.position.y = -999999;
    memset(target, 0, sizeof *target);
    target->infoType = DISPLAYCONFIG_MODE_INFO_TYPE_TARGET;
    target->id = (UINT32)-2;
    target->adapterId = path->sourceInfo.adapterId;
    *np = p + 1, *nm = m + 2;
    return r;
}
static LONG WINAPI DisplayConfigGetDeviceInfo_(DISPLAYCONFIG_DEVICE_INFO_HEADER *q) {
    if (q && (q->id == (UINT32)-1 || q->id == (UINT32)-2) && q->adapterId.HighPart == -1) {   // the sub screen
        if (q->type == DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME) {
            DISPLAYCONFIG_TARGET_DEVICE_NAME *t = (DISPLAYCONFIG_TARGET_DEVICE_NAME *)q;
            t->flags.value = 0;
            t->outputTechnology = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
            t->connectorInstance = 0;
            wcscpy(t->monitorFriendlyDeviceName, L"HIRAKU sub screen");
            wcscpy(t->monitorDevicePath, L"\\\\?\\HirakuSubScreen");
        } else if (q->type == DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME) {
            wcscpy(((DISPLAYCONFIG_SOURCE_DEVICE_NAME *)q)->viewGdiDeviceName, L"\\\\.\\DISPLAY_HK_SUB");
        }
        return ERROR_SUCCESS;
    }
    LONG r = r_DisplayConfigGetDeviceInfo(q);
    if (r == ERROR_SUCCESS && q->type == DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME) {   // the PC's: the main screen
        DISPLAYCONFIG_TARGET_DEVICE_NAME *t = (DISPLAYCONFIG_TARGET_DEVICE_NAME *)q;
        t->outputTechnology = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EXTERNAL;
        t->connectorInstance = 2;
    }
    return r;
}

void pc64_display_install(HMODULE game) {
    r_Direct3DCreate9Ex = pc64_import(game, "d3d9.dll", "Direct3DCreate9Ex", (void *)Direct3DCreate9Ex_);
    r_GetDisplayConfigBufferSizes = pc64_import(game, "user32.dll", "GetDisplayConfigBufferSizes", (void *)GetDisplayConfigBufferSizes_);
    r_QueryDisplayConfig = pc64_import(game, "user32.dll", "QueryDisplayConfig", (void *)QueryDisplayConfig_);
    r_DisplayConfigGetDeviceInfo = pc64_import(game, "user32.dll", "DisplayConfigGetDeviceInfo", (void *)DisplayConfigGetDeviceInfo_);
    r_ShowWindow = pc64_import(game, "user32.dll", "ShowWindow", (void *)ShowWindow_);
    r_MoveWindow = pc64_import(game, "user32.dll", "MoveWindow", (void *)MoveWindow_);
    r_SetForegroundWindow = pc64_import(game, "user32.dll", "SetForegroundWindow", (void *)SetForegroundWindow_);
    if (!r_Direct3DCreate9Ex || !r_QueryDisplayConfig) pc64_log("the game doesn't import Direct3DCreate9Ex/QueryDisplayConfig: not the 64-bit PC pop'n?");
}
