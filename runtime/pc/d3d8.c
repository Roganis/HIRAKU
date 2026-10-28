// SPDX-License-Identifier: GPL-3.0-or-later
// The game's Direct3D 8 (beatmania IIDX 9th style, PLAN.md section 33), as cabinet.c takes Direct3D
// 9's: its pictures in HIRAKU's window. The device is made in a window (hidden), its back buffer
// lockable, so each Present reads the picture straight from it into the link (Direct3D 8 has no
// GetRenderTargetData); the game then waits for hiraku-pc's next frame, as the Direct3D 9 games do.
// Only while HIRAKU's window shows the game (cabinet.c's hook_for).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define COBJMACROS
#include <d3d8.h>

#include "cabinet.h"

typedef HRESULT(WINAPI *CreateDevice8_t)(IDirect3D8 *, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *, IDirect3DDevice8 **);
typedef HRESULT(WINAPI *Reset8_t)(IDirect3DDevice8 *, D3DPRESENT_PARAMETERS *);
typedef HRESULT(WINAPI *Present8_t)(IDirect3DDevice8 *, const RECT *, const RECT *, HWND, const RGNDATA *);
static CreateDevice8_t g_CreateDevice;
static Reset8_t g_Reset;
static Present8_t g_Present;
static D3DFORMAT g_desktop = D3DFMT_X8R8G8B8;

// In a window, at the desktop's format (Direct3D 8 wants it named), the back buffer lockable and
// copied (not flipped) at Present, which never waits for the display
static void windowed(D3DPRESENT_PARAMETERS *pp) {
    pp->Windowed = TRUE;
    pp->FullScreen_RefreshRateInHz = 0;
    pp->FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;
    pp->BackBufferFormat = g_desktop;
    pp->BackBufferCount = 1;
    pp->MultiSampleType = D3DMULTISAMPLE_NONE;
    pp->SwapEffect = D3DSWAPEFFECT_COPY;
    pp->Flags |= D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
}

// The picture just drawn, from the back buffer into the link's pixels
static int capture(IDirect3DDevice8 *d) {
    static int said;
    IDirect3DSurface8 *bb = NULL;
    if (FAILED(IDirect3DDevice8_GetBackBuffer(d, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return 0;
    D3DSURFACE_DESC desc;
    IDirect3DSurface8_GetDesc(bb, &desc);
    D3DLOCKED_RECT lr;
    int ok = 0;
    if (desc.Width <= HK_PC_MAX_W && desc.Height <= HK_PC_MAX_H &&
        SUCCEEDED(IDirect3DSurface8_LockRect(bb, &lr, NULL, D3DLOCK_READONLY))) {
        hk_put_pixels(lr.pBits, lr.Pitch, desc.Width, desc.Height, (int)desc.Format);
        IDirect3DSurface8_UnlockRect(bb);
        if (!said++) hk_log("hiraku-pc: the pictures go to HIRAKU's window (Direct3D 8, %ux%u, format %d)", desc.Width, desc.Height, (int)desc.Format);
        ok = 1;
    } else if (said >= 0) {
        hk_log("hiraku-pc: can't read the pictures back (Direct3D 8, %ux%u, format %d)", desc.Width, desc.Height, (int)desc.Format);
        said = -1;
    }
    IDirect3DSurface8_Release(bb);
    return ok;
}

static HRESULT WINAPI my_Present(IDirect3DDevice8 *d, const RECT *src, const RECT *dst, HWND wnd, const RGNDATA *dirty) {
    hk_present_begin();
    if (!hk_host_shows()) return g_Present(d, src, dst, wnd, dirty);
    hk_picture_drawn(capture(d));
    return D3D_OK;
}

// A light's alpha: Direct3D lights a vertex with the material's diffuse alpha and ignores the lights'
// own, but Wine multiplies the light's in. DistorteD's light has diffuse alpha 0 and its MODE SELECT
// blends by the lit alpha (source alpha, zero), so under Wine the whole screen came out black
// (bemanitools' iidx13_fix_song_select_bg turns lighting off instead, losing the shading). Each light
// is handed on with alpha 1: no change where Direct3D's rule holds, the cabinet's look where it doesn't.
typedef HRESULT(WINAPI *SetLight8_t)(IDirect3DDevice8 *, DWORD, const D3DLIGHT8 *);
static SetLight8_t g_SetLight;

static HRESULT WINAPI my_SetLight(IDirect3DDevice8 *d, DWORD i, const D3DLIGHT8 *l) {
    if (!l) return g_SetLight(d, i, l);
    D3DLIGHT8 c = *l;
    c.Diffuse.a = c.Ambient.a = c.Specular.a = 1.0f;
    return g_SetLight(d, i, &c);
}

static HRESULT WINAPI my_Reset(IDirect3DDevice8 *d, D3DPRESENT_PARAMETERS *pp) {
    windowed(pp);
    return g_Reset(d, pp);
}

static HRESULT WINAPI my_CreateDevice(IDirect3D8 *self, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                                      D3DPRESENT_PARAMETERS *pp, IDirect3DDevice8 **dev) {
    hk_log("hiraku-pc: the game's Direct3D 8: %ux%u, %s, %u Hz, format %d", pp->BackBufferWidth, pp->BackBufferHeight,
           pp->Windowed ? "windowed" : "full screen", pp->FullScreen_RefreshRateInHz, (int)pp->BackBufferFormat);
    D3DDISPLAYMODE mode;
    if (SUCCEEDED(IDirect3D8_GetAdapterDisplayMode(self, adapter, &mode))) g_desktop = mode.Format;
    windowed(pp);
    hk_place_window(pp->hDeviceWindow ? pp->hDeviceWindow : focus);
    const HRESULT r = g_CreateDevice(self, adapter, type, focus, flags, pp, dev);
    if (r == D3D_OK && dev && *dev) {
        IDirect3DDevice8Vtbl *vt = (IDirect3DDevice8Vtbl *)(*dev)->lpVtbl;
        void *was = hk_patch_slot((void **)&vt->Reset, (void *)my_Reset);
        if (was) g_Reset = (Reset8_t)was;
        was = hk_patch_slot((void **)&vt->Present, (void *)my_Present);
        if (was) g_Present = (Present8_t)was;
        was = hk_patch_slot((void **)&vt->SetLight, (void *)my_SetLight);
        if (was) g_SetLight = (SetLight8_t)was;
    } else {
        hk_log("hiraku-pc: CreateDevice (Direct3D 8) failed (%#lx)", (unsigned long)r);
    }
    return r;
}

typedef IDirect3D8 *(WINAPI *Direct3DCreate8_t)(UINT);

void *__stdcall hk_Direct3DCreate8(unsigned sdk) {
    static Direct3DCreate8_t real;
    if (!real) real = (Direct3DCreate8_t)(void *)GetProcAddress(LoadLibraryA("d3d8.dll"), "Direct3DCreate8");
    IDirect3D8 *d = real ? real(sdk) : NULL;
    if (d) {
        void *was = hk_patch_slot((void **)&d->lpVtbl->CreateDevice, (void *)my_CreateDevice);
        if (was) g_CreateDevice = (CreateDevice8_t)was;
    }
    return d;
}
