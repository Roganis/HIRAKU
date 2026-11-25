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
// The game presents each screen through its own swap chain. HK_PC64_SNAPS=DIR:N (or DIR:N:LO:HI,
// frames LO..HI only) writes both screens' pictures every N presents to DIR as BMPs (tests).
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
static int g_disp_log;   // HK_PC64_DISP_LOG: what the game learns of the screens, and the mode it asks
                         // Direct3D for, logged (tests)
static int say_disp(void) {   // the enumeration logs are capped
    static int n;
    return g_disp_log && n++ < 400;
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
    if (g_disp_log) pc64_log("disp: the PC tells of %u adapter(s); answered %u", n, n < 2 ? 2 : n);
    return n < 2 ? 2 : n;
}
static HRESULT WINAPI GetAdapterIdentifier_(IDirect3D9Ex *d, UINT a, DWORD f, D3DADAPTER_IDENTIFIER9 *id) {
    HRESULT r = r3d.GetAdapterIdentifier(d, a == 1 ? 0 : a, f, id);
    if (a == 1 && SUCCEEDED(r)) strcpy(id->DeviceName, "\\\\.\\DISPLAY_HK_SUB");
    return r;
}
static UINT WINAPI GetAdapterModeCount_(IDirect3D9Ex *d, UINT a, D3DFORMAT f) {
    UINT n = a == 1 ? 1 : r3d.GetAdapterModeCount(d, a, f);
    if (say_disp()) pc64_log("disp: adapter %u format %u: %u mode(s)", a, (unsigned)f, n);
    return n;
}
static HRESULT WINAPI EnumAdapterModes_(IDirect3D9Ex *d, UINT a, D3DFORMAT f, UINT i, D3DDISPLAYMODE *m) {
    if (a == 1) {
        if (i) return D3DERR_INVALIDCALL;
        sub_mode(m);
        return D3D_OK;
    }
    HRESULT r = r3d.EnumAdapterModes(d, a, f, i, m);
    if (SUCCEEDED(r)) fix_hz(&m->RefreshRate);
    if (say_disp() && SUCCEEDED(r)) pc64_log("disp: adapter %u format %u mode %u: %ux%u at %u Hz format %u", a, (unsigned)f, i,
                                             m->Width, m->Height, m->RefreshRate, (unsigned)m->Format);
    return r;
}
static HRESULT WINAPI GetAdapterDisplayMode_(IDirect3D9Ex *d, UINT a, D3DDISPLAYMODE *m) {
    if (a == 1) {
        sub_mode(m);
        return D3D_OK;
    }
    HRESULT r = r3d.GetAdapterDisplayMode(d, a, m);
    if (SUCCEEDED(r)) fix_hz(&m->RefreshRate);
    if (g_disp_log && SUCCEEDED(r)) pc64_log("disp: adapter %u's display mode: %ux%u at %u Hz format %u", a, m->Width, m->Height, m->RefreshRate, (unsigned)m->Format);
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
    UINT n = a == 1 ? 1 : r3d.GetAdapterModeCountEx(d, a, f);
    if (say_disp()) pc64_log("disp: adapter %u format %u (Ex): %u mode(s)", a, (unsigned)f->Format, n);
    return n;
}
static HRESULT WINAPI EnumAdapterModesEx_(IDirect3D9Ex *d, UINT a, const D3DDISPLAYMODEFILTER *f, UINT i, D3DDISPLAYMODEEX *m) {
    if (a == 1) {
        if (i) return D3DERR_INVALIDCALL;
        sub_mode_ex(m);
        return D3D_OK;
    }
    HRESULT r = r3d.EnumAdapterModesEx(d, a, f, i, m);
    if (SUCCEEDED(r)) fix_hz(&m->RefreshRate);
    if (say_disp() && SUCCEEDED(r)) pc64_log("disp: adapter %u format %u mode %u (Ex): %ux%u at %u Hz format %u", a, (unsigned)f->Format, i,
                                              m->Width, m->Height, m->RefreshRate, (unsigned)m->Format);
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
    if (g_disp_log && SUCCEEDED(r)) pc64_log("disp: adapter %u's display mode (Ex): %ux%u at %u Hz format %u", a, m->Width, m->Height, m->RefreshRate, (unsigned)m->Format);
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
    if (g_disp_log)
        pc64_log("disp: Reset %ux%u format %u windowed %d swap %u at %u Hz interval %lx",
                 p->BackBufferWidth, p->BackBufferHeight, (unsigned)p->BackBufferFormat, p->Windowed,
                 (unsigned)p->SwapEffect, p->FullScreen_RefreshRateInHz, (unsigned long)p->PresentationInterval);
    windowed(p);
    tc_reset();
    return rdev.Reset(d, p);
}
static HRESULT WINAPI ResetEx_(IDirect3DDevice9Ex *d, D3DPRESENT_PARAMETERS *p, D3DDISPLAYMODEEX *m) {
    (void)m;
    if (g_disp_log)
        pc64_log("disp: ResetEx %ux%u format %u windowed %d swap %u at %u Hz interval %lx",
                 p->BackBufferWidth, p->BackBufferHeight, (unsigned)p->BackBufferFormat, p->Windowed,
                 (unsigned)p->SwapEffect, p->FullScreen_RefreshRateInHz, (unsigned long)p->PresentationInterval);
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
// HK_PC64_SNAPS (tests): "DIR:N" or "DIR:N:LO:HI" — pictures every N presents, frames LO..HI only
// (a drive letter's colon is not a separator)
static struct { char dir[MAX_PATH]; int every; unsigned lo, hi; int done; } g_snap;
static void snaps_parse(void) {
    if (g_snap.done) return;
    g_snap.done = 1;
    g_snap.every = 0;
    g_snap.lo = 0;
    g_snap.hi = 0xffffffffu;
    const char *s = getenv("HK_PC64_SNAPS");
    if (!s) return;
    const char *c1 = strchr(s + 2, ':');
    if (!c1) return;
    snprintf(g_snap.dir, sizeof g_snap.dir, "%.*s", (int)(c1 - s), s);
    g_snap.every = atoi(c1 + 1);
    const char *c2 = strchr(c1 + 1, ':');
    if (c2) {
        g_snap.lo = (unsigned)atol(c2 + 1);
        const char *c3 = strchr(c2 + 1, ':');
        if (c3) g_snap.hi = (unsigned)atol(c3 + 1);
    }
}
static int snaps_on(unsigned f) {
    return g_snap.every && f % (unsigned)g_snap.every == 0 && f >= g_snap.lo && f <= g_snap.hi;
}
static void snapshot(void) {
    snaps_parse();
    if (!snaps_on(pc64_frames)) return;
    char path[MAX_PATH + 32];
    IDirect3DSurface9 *bb;
    if (SUCCEEDED(rdev.GetBackBuffer(g_dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) {
        snprintf(path, sizeof path, "%s\\snap-%06u-main.bmp", g_snap.dir, pc64_frames);
        write_bmp(bb, path);
        IDirect3DSurface9_Release(bb);
    }
    if (g_sub && SUCCEEDED(IDirect3DSwapChain9_GetBackBuffer(g_sub, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) {
        snprintf(path, sizeof path, "%s\\snap-%06u-sub.bmp", g_snap.dir, pc64_frames);
        write_bmp(bb, path);
        IDirect3DSurface9_Release(bb);
    }
}
// HK_PC64_SNAPS (tests): the game's 480-line layer (854x480, drawn first, then stretched to the
// main screen) with the snapshots
static HRESULT WINAPI StretchRect_(IDirect3DDevice9Ex *d, IDirect3DSurface9 *src, const RECT *sr, IDirect3DSurface9 *dst, const RECT *dr,
                                   D3DTEXTUREFILTERTYPE f) {
    snaps_parse();
    D3DSURFACE_DESC sd;
    if (snaps_on(pc64_frames) && src && SUCCEEDED(IDirect3DSurface9_GetDesc(src, &sd)) && sd.Height == 480) {
        char path[MAX_PATH + 40];
        snprintf(path, sizeof path, "%s\\snap-%06u-%ux480.bmp", g_snap.dir, pc64_frames, sd.Width);
        write_bmp(src, path);
    }
    return rdev.StretchRect(d, src, sr, dst, dr, f);
}

// The main screen's picture into hiraku-pc's link (HIRAKU's window shows it): read back through a
// surface in system memory, X8R8G8B8 (the desktop's, windowed)
// The single-screen mode's picture (crt.c): the main screen's middle 4:3 scaled down on the GPU in
// two linear steps, 1440x1080 to 1280x960 to 640x480: the second an exact half (each pixel the mean
// of four, none counted twice or skipped), the first a ninth less. Small text comes out a little
// crisper than through 960x720 (two steps of 1.5, each pixel a quarter and three quarters of two).
// Read back
// The touch screen's parts a menu shows over it (crt.c's panels): copied at the touch screen's
// presents at the size and the place they're shown (a GPU stretch, linear, into a picture of the
// 640x480's size), read back with the main screen's picture and put on it, framed, the item marked.
// Its whole picture at half its size (640x400) read back for crt.c's menus to look at, 10 times a
// second while they want it.
static IDirect3DSurface9 *g_sub_small, *g_sub_readback, *g_panel, *g_panel_readback;
static struct pc64_panel g_panel_at[PC64_PANELS];   // the panels' places as last copied
static int g_panels_kept;
static void keep_sub(IDirect3DSwapChain9 *s) {
    struct pc64_panel q[PC64_PANELS];
    const int panel = pc64_crt() ? pc64_crt_panels(q) : 0;
    if (!panel) g_panels_kept = 0;
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
    if (panel && g_panel && d.Width == 1280 && d.Height == 800) {
        int kept = 0;
        for (; kept < panel; kept++) {
            const struct pc64_panel *p = &q[kept];
            if (p->sx < 0 || p->sy < 0 || p->sw <= 0 || p->sh <= 0 || p->sx + p->sw > 1280 || p->sy + p->sh > 800 ||
                p->dx < 0 || p->dy < 0 || p->dw <= 0 || p->dh <= 0 || p->dx + p->dw > 640 || p->dy + p->dh > 480)
                break;
            const RECT from = {p->sx, p->sy, p->sx + p->sw, p->sy + p->sh}, to = {p->dx, p->dy, p->dx + p->dw, p->dy + p->dh};
            if (FAILED(rdev.StretchRect(g_dev, bb, &from, g_panel, &to, D3DTEXF_LINEAR))) break;
            g_panel_at[kept] = *p;
        }
        g_panels_kept = kept;
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

// The panels put on the picture: each framed in dark blue and white (as the game's panels), its item
// marked in yellow and red, blinking twice a second; the place marked on the main screen's own
// picture the same way. A panel whose part of the touch screen has just changed (the next option's
// column) shows the last one copied until the next is.
static void mark(uint32_t *out, int x, int y, int w, int h) {
    const int on = GetTickCount() / 250 % 2;
    frame(out, x - 1, y - 1, w + 2, h + 2, 4, on ? 0xffe000 : 0xff3050);
    frame(out, x - 2, y - 2, w + 4, h + 4, 1, 0x000000);
}
static void put_panel(uint32_t *out) {
    struct pc64_panel now[PC64_PANELS];
    int shown = g_panel ? pc64_crt_panels(now) : 0, at[4];
    if (shown > g_panels_kept) shown = g_panels_kept;
    for (int i = 0; i < shown; i++) {   // (not copied yet: none)
        const struct pc64_panel *q = &g_panel_at[i];
        if (q->dx != now[i].dx || q->dy != now[i].dy || q->dw != now[i].dw || q->dh != now[i].dh) shown = 0;
    }
    D3DLOCKED_RECT lr;
    if (shown && SUCCEEDED(IDirect3DDevice9_GetRenderTargetData((IDirect3DDevice9 *)g_dev, g_panel, g_panel_readback)) &&
        SUCCEEDED(IDirect3DSurface9_LockRect(g_panel_readback, &lr, NULL, D3DLOCK_READONLY))) {
        for (int i = 0; i < shown; i++) {
            const struct pc64_panel q = g_panel_at[i];
            for (int y = 0; y < q.dh; y++) {
                const uint32_t *p = (const uint32_t *)((const uint8_t *)lr.pBits + (size_t)(q.dy + y) * lr.Pitch) + q.dx;
                uint32_t *o = out + (size_t)(q.dy + y) * 640 + q.dx;
                for (int x = 0; x < q.dw; x++) o[x] = p[x] & 0xffffff;
            }
        }
        IDirect3DSurface9_UnlockRect(g_panel_readback);
        for (int i = 0; i < shown; i++) {
            const struct pc64_panel q = g_panel_at[i];
            frame(out, q.dx - 5, q.dy - 5, q.dw + 10, q.dh + 10, 3, 0x172a8c);
            frame(out, q.dx - 2, q.dy - 2, q.dw + 4, q.dh + 4, 2, 0xffffff);
            if (now[i].hw > 0)   // the item: its place on the touch screen to the panel's
                mark(out, q.dx + (now[i].hx - q.sx) * q.dw / q.sw, q.dy + (now[i].hy - q.sy) * q.dh / q.sh, now[i].hw * q.dw / q.sw,
                     now[i].hh * q.dh / q.sh);
        }
    }
    if (pc64_crt_mark(at)) mark(out, at[0], at[1], at[2], at[3]);
}

static int to_link_crt(IDirect3DSurface9 *bb, const D3DSURFACE_DESC *d, uint32_t *out) {
    enum { W = 640, H = 480 };
    static IDirect3DSurface9 *half, *small, *readback;
    if (pc64_crt_wants_sub()) sub_for_menus();
    if (!readback) {
        IDirect3DDevice9 *dev = (IDirect3DDevice9 *)g_dev;
        if (FAILED(IDirect3DDevice9_CreateRenderTarget(dev, W * 2, H * 2, d->Format, D3DMULTISAMPLE_NONE, 0, FALSE, &half, NULL)) ||
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
        LARGE_INTEGER rb0;   // HK_PC64_FRAME_LOG: the read-back (GPU surface to system memory, then
        int64_t rb_us = 0;   // the per-pixel copy into the link) takes this long
        if (getenv("HK_PC64_FRAME_LOG") && QueryPerformanceCounter(&rb0)) {} else rb0.QuadPart = 0;
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
        if (rb0.QuadPart) {
            LARGE_INTEGER q, f;
            if (QueryPerformanceCounter(&q) && QueryPerformanceFrequency(&f))
                rb_us = (q.QuadPart - rb0.QuadPart) * 1000000 / f.QuadPart;
            pc64_note_readback((uint32_t)rb_us);
        }
    }
    IDirect3DSurface9_Release(bb);
    if (ok) pc64_link_picture(d.Width, d.Height);
    return ok;
}

static uint32_t span_us(int64_t q0) {   // HK_PC64_FRAME_LOG: a span around a real call, in us
    LARGE_INTEGER q, f;
    if (!pc64_frame_log_on()) return 0;
    if (!QueryPerformanceCounter(&q) || !QueryPerformanceFrequency(&f)) return 0;
    return (uint32_t)((q.QuadPart - q0) * 1000000 / f.QuadPart);
}
static int64_t now_qpc(void) {
    LARGE_INTEGER q;
    return QueryPerformanceCounter(&q) ? q.QuadPart : 0;
}
static HRESULT WINAPI SwapChainPresent_(IDirect3DSwapChain9Ex *s, const RECT *a, const RECT *b, HWND w, const RGNDATA *r, DWORD f) {
    const int64_t t0 = now_qpc();
    if ((void *)s == (void *)g_sub) {
        pc64_sub_frames++;
        keep_sub((IDirect3DSwapChain9 *)s);
    } else {
        pc64_frames++;
        snapshot();
        if (pc64_host_shows() && to_link((IDirect3DSwapChain9 *)s)) { pc64_note_present_span(span_us(t0)); return D3D_OK; }   // not shown by the game's window
    }
    const HRESULT hr = rsc.Present(s, a, b, w, r, f);
    pc64_note_present_span(span_us(t0));
    return hr;
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
    const int64_t t0 = now_qpc();
    pc64_frames++;
    snapshot();
    const HRESULT hr = rdev.Present(d, a, b, w, r);
    pc64_note_present_span(span_us(t0));
    return hr;
}
static HRESULT WINAPI PresentEx_(IDirect3DDevice9Ex *d, const RECT *a, const RECT *b, HWND w, const RGNDATA *r, DWORD f) {
    const int64_t t0 = now_qpc();
    pc64_frames++;
    snapshot();
    const HRESULT hr = rdev.PresentEx(d, a, b, w, r, f);
    pc64_note_present_span(span_us(t0));
    return hr;
}
// HK_PC64_FRAME_LOG: the device's WaitForVBlank and GetRasterStatus, timed if the game calls them
static HRESULT WINAPI WaitForVBlank_(IDirect3DDevice9Ex *d, UINT i) {
    const int64_t t0 = now_qpc();
    const HRESULT hr = rdev.WaitForVBlank(d, i);
    pc64_note_devwait(0, span_us(t0));
    return hr;
}
static HRESULT WINAPI GetRasterStatus_(IDirect3DDevice9Ex *d, UINT i, D3DRASTER_STATUS *s) {
    const int64_t t0 = now_qpc();
    const HRESULT hr = rdev.GetRasterStatus(d, i, s);
    pc64_note_devwait(1, span_us(t0));
    return hr;
}
static HRESULT WINAPI SwapGetRasterStatus_(IDirect3DSwapChain9Ex *s, D3DRASTER_STATUS *st) {
    const int64_t t0 = now_qpc();
    const HRESULT hr = rsc.GetRasterStatus(s, st);
    pc64_note_devwait(1, span_us(t0));
    return hr;
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
    pc64_log("Direct3D: main %ux%u at %u Hz, interval %lx%s", pp[0].BackBufferWidth, pp[0].BackBufferHeight,
             pp[0].FullScreen_RefreshRateInHz, (unsigned long)pp[0].PresentationInterval, group ? ", and the sub screen" : "");
    if (g_disp_log) {
        pc64_log("disp: CreateDeviceEx adapter %u type %u focus %p flags %lx: main %ux%u format %u windowed %d swap %u at %u Hz interval %lx",
                 a, (unsigned)t, (void *)focus, (unsigned long)flags, pp[0].BackBufferWidth, pp[0].BackBufferHeight,
                 (unsigned)pp[0].BackBufferFormat, pp[0].Windowed, (unsigned)pp[0].SwapEffect,
                 pp[0].FullScreen_RefreshRateInHz, (unsigned long)pp[0].PresentationInterval);
        if (group) pc64_log("disp:   sub %ux%u format %u windowed %d at %u Hz interval %lx", sub.BackBufferWidth, sub.BackBufferHeight,
                            (unsigned)sub.BackBufferFormat, sub.Windowed, sub.FullScreen_RefreshRateInHz, (unsigned long)sub.PresentationInterval);
    }
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
        pc64_patch(&v->WaitForVBlank, WaitForVBlank_);   // HK_PC64_FRAME_LOG timings (pass-through)
        pc64_patch(&v->GetRasterStatus, GetRasterStatus_);
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
            pc64_patch(&sv->GetRasterStatus, SwapGetRasterStatus_);
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
    if (g_disp_log)
        pc64_log("disp: CreateDevice adapter %u type %u focus %p flags %lx: %ux%u format %u windowed %d at %u Hz interval %lx", a, (unsigned)t,
                 (void *)focus, (unsigned long)flags, pp->BackBufferWidth, pp->BackBufferHeight, (unsigned)pp->BackBufferFormat,
                 pp->Windowed, pp->FullScreen_RefreshRateInHz, (unsigned long)pp->PresentationInterval);
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
        if (g_disp_log) pc64_log("disp: the PC has %u display path(s), %u mode(s)", g_paths, g_modes);
        *np += 1, *nm += 2;
    }
    return r;
}
// a target's friendly name, through the real DisplayConfigGetDeviceInfo
static void target_name(LUID adapter, UINT32 id, char *out, size_t n) {
    out[0] = '?', out[1] = 0;
    if (!r_DisplayConfigGetDeviceInfo) return;
    DISPLAYCONFIG_TARGET_DEVICE_NAME q;
    memset(&q, 0, sizeof q);
    q.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    q.header.size = sizeof q;
    q.header.adapterId = adapter;
    q.header.id = id;
    if (r_DisplayConfigGetDeviceInfo(&q.header) == ERROR_SUCCESS)
        WideCharToMultiByte(CP_UTF8, 0, q.monitorFriendlyDeviceName, -1, out, (int)n, NULL, NULL);
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
    if (g_disp_log)
        for (UINT32 i = 0; i < p; i++) {
            char name[128];
            target_name(paths[i].targetInfo.adapterId, paths[i].targetInfo.id, name, sizeof name);
            UINT32 mi = paths[i].targetInfo.modeInfoIdx;
            if (mi < m && modes[mi].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_TARGET)
                pc64_log("disp: path %u: \"%s\" %ux%u at %u/%u Hz%s", i, name,
                         modes[mi].targetMode.targetVideoSignalInfo.activeSize.cx, modes[mi].targetMode.targetVideoSignalInfo.activeSize.cy,
                         modes[mi].targetMode.targetVideoSignalInfo.vSyncFreq.Numerator, modes[mi].targetMode.targetVideoSignalInfo.vSyncFreq.Denominator,
                         (paths[i].targetInfo.statusFlags & DISPLAYCONFIG_TARGET_IN_USE) ? " (in use)" : "");
            else
                pc64_log("disp: path %u: \"%s\"", i, name);
        }
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
    const char *dl = getenv("HK_PC64_DISP_LOG");
    g_disp_log = dl && *dl && *dl != '0';
    r_Direct3DCreate9Ex = pc64_import(game, "d3d9.dll", "Direct3DCreate9Ex", (void *)Direct3DCreate9Ex_);
    r_GetDisplayConfigBufferSizes = pc64_import(game, "user32.dll", "GetDisplayConfigBufferSizes", (void *)GetDisplayConfigBufferSizes_);
    r_QueryDisplayConfig = pc64_import(game, "user32.dll", "QueryDisplayConfig", (void *)QueryDisplayConfig_);
    r_DisplayConfigGetDeviceInfo = pc64_import(game, "user32.dll", "DisplayConfigGetDeviceInfo", (void *)DisplayConfigGetDeviceInfo_);
    r_ShowWindow = pc64_import(game, "user32.dll", "ShowWindow", (void *)ShowWindow_);
    r_MoveWindow = pc64_import(game, "user32.dll", "MoveWindow", (void *)MoveWindow_);
    r_SetForegroundWindow = pc64_import(game, "user32.dll", "SetForegroundWindow", (void *)SetForegroundWindow_);
    if (!r_Direct3DCreate9Ex || !r_QueryDisplayConfig) pc64_log("the game doesn't import Direct3DCreate9Ex/QueryDisplayConfig: not the 64-bit PC pop'n?");
}

// --- The AFP layouts' pictures looked up by name (a 64-bit PC IIDX's music select, 2026-11-23)
// The game's layout library (afp-utils.dll) finds a picture in a texture list by comparing its name
// with every entry's, one by one (avs2-core's string compare). The music select asks for its list
// bars' picture dozens of times a frame through a list of thousands: 27 million compares a second,
// half of the game's main thread, 11 ms frames for a 120 Hz game. The search is answered here the
// same way (the first entry of that name, or none), with the answers kept: a kept entry is checked
// (the list, its size and the entry's name the same) before it is given; "none" is looked for again
// every 16th time. HK_PC64_AFP_NAMES=0: the library's own search. Found by its code's shape; not
// found, nothing changes (a log line).
typedef int (*afp_cmp_t)(const char *, const char *);
static void **g_afp_cmp_slot;   // afp-utils' import of the compare (a diagnostic hook may sit in it)
#define AFP_SLOTS 2048
static struct afp_kept { const void *list, *base; unsigned h; int idx; WORD n, uses; char name[52]; } g_afp_kept[AFP_SLOTS];
static volatile LONG g_afp_lock;
static volatile LONG64 g_afp_calls, g_afp_kept_answers;

static BYTE *afp_scan(BYTE *list, const char *name) {
    const afp_cmp_t cmp = (afp_cmp_t)*g_afp_cmp_slot;
    for (unsigned i = 0; i < *(WORD *)(list + 0xe); i++) {
        BYTE *e = *(BYTE **)(list + 0x40) + 0x20 * (size_t)i;
        if (cmp(*(const char **)(e + 0x18), name) == 0) return e;
    }
    return NULL;
}
// HK_PC64_AFP_LOG=1 (tests): every 10 s the names searched most since the last, each with its
// list's size, where it was found (or "none") and how many searches went through the whole list
#define AFP_STATS 256
static int g_afp_log;
static struct { const void *list; char name[52]; unsigned n_list; int idx; unsigned calls, scans; } g_afp_stat[AFP_STATS];
static ULONGLONG g_afp_stat_t0;
static void afp_stat(const void *list, const char *name, unsigned n_list, int idx, int scanned) {
    unsigned h = 2166136261u ^ (unsigned)((uintptr_t)list >> 4);
    for (const char *c = name; *c; c++) h = (h ^ (BYTE)*c) * 16777619u;
    for (unsigned k = 0, i = h % AFP_STATS; k < 8; k++, i = (i + 1) % AFP_STATS) {
        if (!g_afp_stat[i].calls) {
            g_afp_stat[i].list = list;
            snprintf(g_afp_stat[i].name, sizeof g_afp_stat[i].name, "%s", name);
        } else if (g_afp_stat[i].list != list || strcmp(g_afp_stat[i].name, name))
            continue;
        g_afp_stat[i].n_list = n_list, g_afp_stat[i].idx = idx, g_afp_stat[i].calls++, g_afp_stat[i].scans += scanned;
        break;
    }
    const ULONGLONG now = GetTickCount64();
    if (!g_afp_stat_t0) g_afp_stat_t0 = now;
    if (now - g_afp_stat_t0 < 10000) return;
    g_afp_stat_t0 = now;
    for (int r = 0; r < 12; r++) {   // the busiest twelve, then cleared
        int best = -1;
        for (int i = 0; i < AFP_STATS; i++)
            if (g_afp_stat[i].calls && (best < 0 || g_afp_stat[i].calls > g_afp_stat[best].calls)) best = i;
        if (best < 0) break;
        pc64_log("afp names f%u: \"%s\" list %p of %u: %u searches, %u through the list, %s%d", pc64_frames, g_afp_stat[best].name,
                 g_afp_stat[best].list, g_afp_stat[best].n_list, g_afp_stat[best].calls, g_afp_stat[best].scans,
                 g_afp_stat[best].idx >= 0 ? "found at " : "none ", g_afp_stat[best].idx);
        g_afp_stat[best].calls = 0;
    }
    memset(g_afp_stat, 0, sizeof g_afp_stat);
}
static BYTE *afp_find_(BYTE *list, const char *name) {
    if (!list) return NULL;
    const LONG64 n_calls = InterlockedIncrement64(&g_afp_calls);
    if (n_calls == 1 << 20 || n_calls == 1 << 24)
        pc64_log("afp names: %lld searches, %lld answered from those kept", (long long)n_calls, (long long)g_afp_kept_answers);
    size_t len = 0;
    unsigned h = 2166136261u ^ (unsigned)((uintptr_t)list >> 4);
    for (; name && name[len] && len < sizeof g_afp_kept[0].name; len++) h = (h ^ (BYTE)name[len]) * 16777619u;
    if (!name || len >= sizeof g_afp_kept[0].name || InterlockedCompareExchange(&g_afp_lock, 1, 0) != 0)
        return afp_scan(list, name);   // (a long name, or another thread in the table: as the library does)
    BYTE *const base = *(BYTE **)(list + 0x40);
    const WORD n = *(WORD *)(list + 0xe);
    struct afp_kept *k = &g_afp_kept[h % AFP_SLOTS];
    BYTE *r;
    if (k->list == list && k->base == base && k->n == n && k->h == h && !strcmp(k->name, name) &&
        (k->idx >= 0 ? ((afp_cmp_t)*g_afp_cmp_slot)(*(const char **)(base + 0x20 * (size_t)k->idx + 0x18), name) == 0
                     : (++k->uses & 15) != 0)) {
        r = k->idx >= 0 ? base + 0x20 * (size_t)k->idx : NULL;
        g_afp_kept_answers++;
        if (g_afp_log) afp_stat(list, name, n, k->idx, 0);
    } else {
        r = afp_scan(list, name);
        k->list = list, k->base = base, k->n = n, k->h = h, k->uses = 0;
        k->idx = r ? (int)((r - base) / 0x20) : -1;
        memcpy(k->name, name, len + 1);
        if (g_afp_log) afp_stat(list, name, n, k->idx, 1);
    }
    InterlockedExchange(&g_afp_lock, 0);
    return r;
}

void pc64_afp_names_install(void) {
    const char *v = getenv("HK_PC64_AFP_NAMES");
    if (v && *v == '0') {
        pc64_log("afp names: the layouts' own search (HK_PC64_AFP_NAMES=0)");
        return;
    }
    const char *lg = getenv("HK_PC64_AFP_LOG");
    g_afp_log = lg && *lg == '1';
    HMODULE m = GetModuleHandleA("afp-utils.dll");
    if (!m) return;
    // the search's code (afp-utils of 2026: list in rcx, name in rdx; the compare's import at +80)
    static const BYTE shape[] = {
        0x48, 0x89, 0x74, 0x24, 0x20, 0x41, 0x54, 0x48, 0x83, 0xec, 0x20, 0x4c, 0x8b, 0xe2, 0x48, 0x8b, 0xf1, 0x48, 0x85, 0xc9,
        0x75, 0x0e, 0x33, 0xc0, 0x48, 0x8b, 0x74, 0x24, 0x48, 0x48, 0x83, 0xc4, 0x20, 0x41, 0x5c, 0xc3, 0x48, 0x89, 0x5c, 0x24,
        0x30, 0x33, 0xdb, 0x48, 0x89, 0x6c, 0x24, 0x38, 0x48, 0x89, 0x7c, 0x24, 0x40, 0x66, 0x3b, 0x59, 0x0e, 0x73, 0x2d, 0x8b,
        0xfb, 0x0f, 0x1f, 0x00, 0x48, 0x8b, 0x46, 0x40, 0x49, 0x8b, 0xd4, 0x48, 0x8b, 0x4c, 0x07, 0x18, 0x48, 0x8d, 0x2c, 0x07,
        0xff, 0x15, 0, 0, 0, 0, 0x85, 0xc0, 0x74, 0x2b, 0x0f, 0xb7, 0x46, 0x0e, 0xff, 0xc3, 0x48, 0x83, 0xc7, 0x20,
        0x3b, 0xd8, 0x7c, 0xd8, 0x33, 0xc0, 0x48, 0x8b, 0x6c, 0x24, 0x38, 0x48, 0x8b, 0x5c, 0x24, 0x30, 0x48, 0x8b, 0x7c, 0x24,
        0x40, 0x48, 0x8b, 0x74, 0x24, 0x48, 0x48, 0x83, 0xc4, 0x20, 0x41, 0x5c, 0xc3, 0x48, 0x8b, 0xc5, 0xeb, 0xe0};
    BYTE *base = (BYTE *)m;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    BYTE *at = NULL;
    int found = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, s++) {
        if (!(s->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        BYTE *p = base + s->VirtualAddress, *end = p + s->Misc.VirtualSize;
        for (; p + sizeof shape <= end; p++) {
            size_t j = 0;
            while (j < sizeof shape && (p[j] == shape[j] || (j >= 82 && j < 86))) j++;
            if (j == sizeof shape) at = p, found++;
        }
    }
    if (found != 1) {
        pc64_log("afp names: the layouts' search not found (%d places of its shape): their own search", found);
        return;
    }
    g_afp_cmp_slot = (void **)(at + 86 + *(int32_t *)(at + 82));
    // its first 14 bytes (whole instructions: mov [rsp+20],rsi; push r12; sub rsp,20; mov r12,rdx) -> jmp [rip]; here
    BYTE jump[14] = {0xff, 0x25, 0, 0, 0, 0};
    void *to = (void *)afp_find_;
    memcpy(jump + 6, &to, 8);
    DWORD old;
    VirtualProtect(at, sizeof jump, PAGE_EXECUTE_READWRITE, &old);
    memcpy(at, jump, sizeof jump);
    VirtualProtect(at, sizeof jump, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, sizeof jump);
    pc64_log("afp names: the layouts' picture search at afp-utils.dll+%llx answered with its answers kept (HK_PC64_AFP_NAMES=0: off)",
             (unsigned long long)(at - base));
}
