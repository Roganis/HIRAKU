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

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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


// --- In-between pictures (smooth motion, docs/SMOOTH.md; link v11). The PC IIDX games count
// everything in frames (PLAN.md section 36), so a display faster than the game gets pictures of
// HIRAKU's: the game's last frame drawn again with its notes moved on. While hiraku-pc asks for them
// (the link's smooth), each frame's drawing is recorded: the states set, the textures, and each
// DrawPrimitiveUP's vertices (the IIDX games draw everything so: quads in screen space, XYZRHW). Its
// quads go to hiraku-pc with the picture, which pairs them with the frame before's and says how far
// each moves (only what goes with the scroll: core.cpp). While the game waits for its next frame,
// each such picture is drawn by replaying the frame into a render target of its own, between the
// frame's starting state and its ending one (state blocks), the moved quads shifted, and read back.
// A frame that draws in a way this doesn't record (vertex buffers, another render target, shaders'
// constants) gets none: its refreshes show it as it is.
enum { OP_RS, OP_TSS, OP_TEX, OP_VS, OP_TRANSFORM, OP_VIEWPORT, OP_MATERIAL, OP_LIGHT, OP_LIGHT_ON, OP_CLEAR, OP_BEGIN, OP_END, OP_DPUP };
struct op {
    int kind;
    DWORD a, b, c;
    size_t at;    // its data in g_pool
    UINT size;
    int quad;     // OP_DPUP: its first quad in the frame's, or -1
    int nquads;   // ... how many (a list's quads drawn in one call), each `per` vertices
    UINT per;
};
static struct {
    int on;        // recording this frame
    int broken;    // it drew in a way not recorded
    struct op *ops;
    size_t n, cap;
    uint8_t *pool;
    size_t used, room;
    IDirect3DBaseTexture8 *held[256];   // the textures recorded (kept till the next frame)
    int nheld;
    IDirect3DBaseTexture8 *tex0;        // stage 0's texture now (the quads' identity)
    DWORD fvf;     // the vertex shader set: an FVF, or a shader's handle (odd)
    int ps;        // a pixel shader in effect
    int quads;
    int ready;     // a whole frame recorded, the states around it captured
} g_rec;
static DWORD g_sb_start, g_sb_end;      // state blocks: the frame's start, its end
static IDirect3DSurface8 *g_rt;         // where pictures are drawn again
static UINT g_rt_w, g_rt_h;
static D3DFORMAT g_rt_fmt;
static IDirect3DDevice8 *g_dev8;
static int g_replaying;

static size_t keep(const void *p, size_t n) {
    if (g_rec.used + n > g_rec.room) {
        size_t r = g_rec.room ? g_rec.room : 65536;
        while (r < g_rec.used + n) r *= 2;
        uint8_t *q = (uint8_t *)realloc(g_rec.pool, r);
        if (!q) { g_rec.broken = 1; return 0; }
        g_rec.pool = q, g_rec.room = r;
    }
    const size_t at = g_rec.used;
    if (p) memcpy(g_rec.pool + at, p, n);
    g_rec.used += n;
    return at;
}
static struct op *add(int kind, DWORD a, DWORD b, DWORD c) {
    if (g_rec.n == g_rec.cap) {
        const size_t cap = g_rec.cap ? g_rec.cap * 2 : 1024;
        struct op *o = (struct op *)realloc(g_rec.ops, cap * sizeof *o);
        if (!o) { g_rec.broken = 1; return NULL; }
        g_rec.ops = o, g_rec.cap = cap;
    }
    struct op *o = &g_rec.ops[g_rec.n++];
    o->kind = kind, o->a = a, o->b = b, o->c = c, o->at = 0, o->size = 0, o->quad = -1, o->nquads = 0, o->per = 0;
    return o;
}
static int recording(void) { return g_rec.on && !g_rec.broken && !g_replaying; }
// HK_PC_DRAWLOG=FIRST,COUNT: each draw of those pictures in pc.log (how a game draws)
static unsigned g_frame, g_log_first = ~0u, g_log_count;
static int logging(void) { return !g_replaying && g_frame >= g_log_first && g_frame < g_log_first + g_log_count; }
// a frame that can't be drawn again: why, said once a kind
static void cant(const char *why) {
    static char said[16][24];
    g_rec.broken = 1;
    for (int i = 0; i < 16; i++) {
        if (!said[i][0]) {
            snprintf(said[i], sizeof said[i], "%s", why);
            hk_log("hiraku-pc: smooth motion: a picture the game draws with %s isn't drawn again (no in-between pictures)", why);
            return;
        }
        if (!strcmp(said[i], why)) return;
    }
}
static void drop_textures(void) {
    for (int i = 0; i < g_rec.nheld; i++) IDirect3DBaseTexture8_Release(g_rec.held[i]);
    g_rec.nheld = 0;
}
// a new frame's recording, at the state the device has now (after the last Present)
static void rec_start(IDirect3DDevice8 *d) {
    drop_textures();
    g_rec.n = 0, g_rec.used = 0, g_rec.broken = 0, g_rec.quads = 0, g_rec.ready = 0;
    g_rec.on = hk_link()->smooth && hk_host_shows();
    if (!g_rec.on) return;
    if (!g_sb_start && FAILED(IDirect3DDevice8_CreateStateBlock(d, D3DSBT_ALL, &g_sb_start))) g_sb_start = 0;
    if (!g_sb_end && FAILED(IDirect3DDevice8_CreateStateBlock(d, D3DSBT_ALL, &g_sb_end))) g_sb_end = 0;
    if (!g_sb_start || !g_sb_end || FAILED(IDirect3DDevice8_CaptureStateBlock(d, g_sb_start))) g_rec.on = 0;
}
static void state_blocks_gone(IDirect3DDevice8 *d) {   // before a Reset, which they don't outlive
    if (g_sb_start) IDirect3DDevice8_DeleteStateBlock(d, g_sb_start);
    if (g_sb_end) IDirect3DDevice8_DeleteStateBlock(d, g_sb_end);
    g_sb_start = g_sb_end = 0;
    if (g_rt) IDirect3DSurface8_Release(g_rt), g_rt = NULL;
    drop_textures();
    g_rec.on = g_rec.ready = 0;
}

typedef HRESULT(WINAPI *SetLight8_t)(IDirect3DDevice8 *, DWORD, const D3DLIGHT8 *);
static SetLight8_t g_SetLight;
typedef HRESULT(WINAPI *RS8_t)(IDirect3DDevice8 *, D3DRENDERSTATETYPE, DWORD);
typedef HRESULT(WINAPI *TSS8_t)(IDirect3DDevice8 *, DWORD, D3DTEXTURESTAGESTATETYPE, DWORD);
typedef HRESULT(WINAPI *ST8_t)(IDirect3DDevice8 *, DWORD, IDirect3DBaseTexture8 *);
typedef HRESULT(WINAPI *SVS8_t)(IDirect3DDevice8 *, DWORD);
typedef HRESULT(WINAPI *STR8_t)(IDirect3DDevice8 *, D3DTRANSFORMSTATETYPE, const D3DMATRIX *);
typedef HRESULT(WINAPI *SVP8_t)(IDirect3DDevice8 *, const D3DVIEWPORT8 *);
typedef HRESULT(WINAPI *SM8_t)(IDirect3DDevice8 *, const D3DMATERIAL8 *);
typedef HRESULT(WINAPI *LE8_t)(IDirect3DDevice8 *, DWORD, BOOL);
typedef HRESULT(WINAPI *CL8_t)(IDirect3DDevice8 *, DWORD, const D3DRECT *, DWORD, D3DCOLOR, float, DWORD);
typedef HRESULT(WINAPI *BS8_t)(IDirect3DDevice8 *);
typedef HRESULT(WINAPI *DPUP8_t)(IDirect3DDevice8 *, D3DPRIMITIVETYPE, UINT, const void *, UINT);
typedef HRESULT(WINAPI *DP8_t)(IDirect3DDevice8 *, D3DPRIMITIVETYPE, UINT, UINT);
typedef HRESULT(WINAPI *DIP8_t)(IDirect3DDevice8 *, D3DPRIMITIVETYPE, UINT, UINT, UINT, UINT);
typedef HRESULT(WINAPI *DIPUP8_t)(IDirect3DDevice8 *, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void *, D3DFORMAT, const void *, UINT);
typedef HRESULT(WINAPI *SRT8_t)(IDirect3DDevice8 *, IDirect3DSurface8 *, IDirect3DSurface8 *);
typedef HRESULT(WINAPI *SPS8_t)(IDirect3DDevice8 *, DWORD);
typedef HRESULT(WINAPI *SC8_t)(IDirect3DDevice8 *, DWORD, const void *, DWORD);
typedef HRESULT(WINAPI *CR8_t)(IDirect3DDevice8 *, IDirect3DSurface8 *, const RECT *, UINT, IDirect3DSurface8 *, const POINT *);
static RS8_t g_RS; static TSS8_t g_TSS; static ST8_t g_ST; static SVS8_t g_SVS; static STR8_t g_STR; static SVP8_t g_SVP;
static SM8_t g_SM; static LE8_t g_LE; static CL8_t g_CL; static BS8_t g_BS, g_ES; static DPUP8_t g_DPUP; static DP8_t g_DP;
static DIP8_t g_DIP; static DIPUP8_t g_DIPUP; static SRT8_t g_SRT; static SPS8_t g_SPS; static SC8_t g_SVSC, g_SPSC; static CR8_t g_CR;

static HRESULT WINAPI my_RS(IDirect3DDevice8 *d, D3DRENDERSTATETYPE s, DWORD v) {
    if (recording()) add(OP_RS, s, v, 0);
    return g_RS(d, s, v);
}
static HRESULT WINAPI my_TSS(IDirect3DDevice8 *d, DWORD st, D3DTEXTURESTAGESTATETYPE s, DWORD v) {
    if (recording()) add(OP_TSS, st, s, v);
    return g_TSS(d, st, s, v);
}
static HRESULT WINAPI my_ST(IDirect3DDevice8 *d, DWORD st, IDirect3DBaseTexture8 *t) {
    if (!g_replaying && st == 0) g_rec.tex0 = t;
    if (recording()) {
        if (t) {
            if (g_rec.nheld == (int)(sizeof g_rec.held / sizeof *g_rec.held)) cant("more than 256 textures");
            else IDirect3DBaseTexture8_AddRef(t), g_rec.held[g_rec.nheld++] = t;
        }
        add(OP_TEX, st, (DWORD)(uintptr_t)t, 0);
    }
    return g_ST(d, st, t);
}
static HRESULT WINAPI my_SVS(IDirect3DDevice8 *d, DWORD h) {
    if (!g_replaying) g_rec.fvf = h;
    if (recording()) {
        if (h & 1) cant("a vertex shader");   // the game's own (a handle, not an FVF; also checked at each draw)
        add(OP_VS, h, 0, 0);
    }
    return g_SVS(d, h);
}
static HRESULT WINAPI my_STR(IDirect3DDevice8 *d, D3DTRANSFORMSTATETYPE t, const D3DMATRIX *m) {
    if (recording() && m) {
        struct op *o = add(OP_TRANSFORM, t, 0, 0);
        if (o) o->at = keep(m, sizeof *m);
    }
    return g_STR(d, t, m);
}
static HRESULT WINAPI my_SVP(IDirect3DDevice8 *d, const D3DVIEWPORT8 *v) {
    if (recording() && v) {
        struct op *o = add(OP_VIEWPORT, 0, 0, 0);
        if (o) o->at = keep(v, sizeof *v);
    }
    return g_SVP(d, v);
}
static HRESULT WINAPI my_SM(IDirect3DDevice8 *d, const D3DMATERIAL8 *m) {
    if (recording() && m) {
        struct op *o = add(OP_MATERIAL, 0, 0, 0);
        if (o) o->at = keep(m, sizeof *m);
    }
    return g_SM(d, m);
}
static HRESULT WINAPI my_LE(IDirect3DDevice8 *d, DWORD i, BOOL on) {
    if (recording()) add(OP_LIGHT_ON, i, on, 0);
    return g_LE(d, i, on);
}
static HRESULT WINAPI my_CL(IDirect3DDevice8 *d, DWORD n, const D3DRECT *r, DWORD flags, D3DCOLOR c, float z, DWORD s) {
    if (recording()) {
        struct op *o = add(OP_CLEAR, n, flags, c);
        if (o) {
            o->at = keep(&z, sizeof z);
            keep(&s, sizeof s);
            if (r && n) keep(r, n * sizeof *r);
            else o->a = 0;
        }
    }
    return g_CL(d, n, r, flags, c, z, s);
}
static HRESULT WINAPI my_BS(IDirect3DDevice8 *d) {
    if (recording()) add(OP_BEGIN, 0, 0, 0);
    return g_BS(d);
}
static HRESULT WINAPI my_ES(IDirect3DDevice8 *d) {
    if (recording()) add(OP_END, 0, 0, 0);
    return g_ES(d);
}
static void record_draw(D3DPRIMITIVETYPE t, UINT n, const void *v, UINT stride) {
    if (logging()) {
        const float *f = (const float *)v;
        hk_log("draw %u: t%d n%u fvf %#lx tex %p stride %u v0 %.1f %.1f", g_frame, (int)t, n, (unsigned long)g_rec.fvf, (void *)g_rec.tex0, stride, f[0], f[1]);
    }
    if (!recording()) return;
    if (g_rec.fvf & 1) cant("a vertex shader");
    if (g_rec.ps) cant("a pixel shader");
    if (!recording()) return;
    {
        const UINT verts = t == D3DPT_TRIANGLESTRIP || t == D3DPT_TRIANGLEFAN ? n + 2 : t == D3DPT_TRIANGLELIST ? 3 * n
                         : t == D3DPT_LINESTRIP ? n + 1 : t == D3DPT_LINELIST ? 2 * n : n;
        struct op *o = add(OP_DPUP, t, n, stride);
        if (o) {
            o->size = verts * stride;
            o->at = keep(v, o->size);
            // quads in screen space, each one of the frame's objects for hiraku-pc: a draw of two
            // triangles, or a list's triangles two by two (beatmania IIDX 10th style draws its notes
            // so, a list of them in one call)
            const int rhw = (g_rec.fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW;
            UINT per = 0;
            if (n == 2 && verts <= 6) per = verts;
            else if (t == D3DPT_TRIANGLELIST && n % 2 == 0) per = 6;
            if (rhw && per && stride >= 16) {
                for (UINT g = 0; g * per < verts && g_rec.quads < (int)HK_PC_QUADS; g++) {
                    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
                    for (UINT i = g * per; i < (g + 1) * per && i < verts; i++) {
                        const float *p = (const float *)((const uint8_t *)v + (size_t)i * stride);
                        if (p[0] < x0) x0 = p[0];
                        if (p[0] > x1) x1 = p[0];
                        if (p[1] < y0) y0 = p[1];
                        if (p[1] > y1) y1 = p[1];
                    }
                    if (!(x0 > -32000 && x1 < 32000 && y0 > -32000 && y1 < 32000)) break;
                    struct hk_pc_quad *q = (struct hk_pc_quad *)((char *)hk_link() + HK_PC_QUAD_LIST) + g_rec.quads;
                    q->tex = (uint32_t)(uintptr_t)g_rec.tex0;
                    q->x = (int16_t)floorf(x0 + 0.5f), q->y = (int16_t)floorf(y0 + 0.5f);
                    q->w = (int16_t)floorf(x1 - x0 + 0.5f), q->h = (int16_t)floorf(y1 - y0 + 0.5f);
                    if (o->quad < 0) o->quad = g_rec.quads, o->per = per;
                    o->nquads++, g_rec.quads++;
                }
            }
        }
    }
}
static UINT verts_of(D3DPRIMITIVETYPE t, UINT n) {
    return t == D3DPT_TRIANGLESTRIP || t == D3DPT_TRIANGLEFAN ? n + 2 : t == D3DPT_TRIANGLELIST ? 3 * n : t == D3DPT_LINESTRIP ? n + 1 : t == D3DPT_LINELIST ? 2 * n : n;
}
static HRESULT WINAPI my_DPUP(IDirect3DDevice8 *d, D3DPRIMITIVETYPE t, UINT n, const void *v, UINT stride) {
    if ((recording() || logging()) && v) record_draw(t, n, v, stride);
    return g_DPUP(d, t, n, v, stride);
}
// what isn't recorded: the frame can't be drawn again
// A vertex buffer's draw: its vertices read and recorded as DrawPrimitiveUP's (drawn again so; the
// frame's end state puts the stream back). A buffer that can't be read (write-only): not recorded
static IDirect3DVertexBuffer8 *g_vb0;
static UINT g_vb0_stride;
static HRESULT WINAPI my_DP(IDirect3DDevice8 *d, D3DPRIMITIVETYPE t, UINT a, UINT n) {
    if (recording() || logging()) {
        BYTE *p = NULL;
        const UINT size = verts_of(t, n) * g_vb0_stride;
        if (g_vb0 && g_vb0_stride && SUCCEEDED(IDirect3DVertexBuffer8_Lock(g_vb0, a * g_vb0_stride, size, &p, D3DLOCK_READONLY))) {
            record_draw(t, n, p, g_vb0_stride);
            IDirect3DVertexBuffer8_Unlock(g_vb0);
        } else {
            cant("DrawPrimitive from a buffer it can't read");
        }
    }
    return g_DP(d, t, a, n);
}
typedef HRESULT(WINAPI *SSS8_t)(IDirect3DDevice8 *, UINT, IDirect3DVertexBuffer8 *, UINT);
static SSS8_t g_SSS;
static HRESULT WINAPI my_SSS(IDirect3DDevice8 *d, UINT i, IDirect3DVertexBuffer8 *vb, UINT stride) {
    if (!g_replaying && i == 0) g_vb0 = vb, g_vb0_stride = stride;
    return g_SSS(d, i, vb, stride);
}
static HRESULT WINAPI my_DIP(IDirect3DDevice8 *d, D3DPRIMITIVETYPE t, UINT a, UINT b, UINT c, UINT n) {
    if (recording()) cant("DrawIndexedPrimitive");
    return g_DIP(d, t, a, b, c, n);
}
static HRESULT WINAPI my_DIPUP(IDirect3DDevice8 *d, D3DPRIMITIVETYPE t, UINT a, UINT b, UINT n, const void *ix, D3DFORMAT f, const void *v, UINT s) {
    if (recording()) cant("DrawIndexedPrimitiveUP");
    return g_DIPUP(d, t, a, b, n, ix, f, v, s);
}
static HRESULT WINAPI my_SRT(IDirect3DDevice8 *d, IDirect3DSurface8 *rt, IDirect3DSurface8 *z) {
    if (recording()) cant("SetRenderTarget");
    return g_SRT(d, rt, z);
}
static HRESULT WINAPI my_SPS(IDirect3DDevice8 *d, DWORD h) {
    if (!g_replaying) g_rec.ps = h != 0;
    return g_SPS(d, h);
}
static HRESULT WINAPI my_SVSC(IDirect3DDevice8 *d, DWORD r, const void *c, DWORD n) {
    if (recording()) cant("SetVertexShaderConstant");
    return g_SVSC(d, r, c, n);
}
static HRESULT WINAPI my_SPSC(IDirect3DDevice8 *d, DWORD r, const void *c, DWORD n) {
    if (recording()) cant("SetPixelShaderConstant");
    return g_SPSC(d, r, c, n);
}
static HRESULT WINAPI my_CR(IDirect3DDevice8 *d, IDirect3DSurface8 *s, const RECT *r, UINT n, IDirect3DSurface8 *t, const POINT *p) {
    if (recording()) cant("CopyRects");
    return g_CR(d, s, r, n, t, p);
}

// The frame drawn again, its quads moved as hiraku-pc says, into the link's in-between pixels
static void draw_between(void) {
    struct hk_pc_link *l = hk_link();
    const uint32_t req = l->between_req;
    int ok = 0;
    IDirect3DDevice8 *d = g_dev8;
    if (d && g_rec.ready) {
        IDirect3DSurface8 *bb = NULL, *was = NULL, *z = NULL;
        D3DSURFACE_DESC desc;
        if (SUCCEEDED(IDirect3DDevice8_GetBackBuffer(d, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) {
            IDirect3DSurface8_GetDesc(bb, &desc);
            IDirect3DSurface8_Release(bb);
            if (g_rt && (g_rt_w != desc.Width || g_rt_h != desc.Height || g_rt_fmt != desc.Format)) IDirect3DSurface8_Release(g_rt), g_rt = NULL;
            if (!g_rt && SUCCEEDED(IDirect3DDevice8_CreateRenderTarget(d, desc.Width, desc.Height, desc.Format, D3DMULTISAMPLE_NONE, TRUE, &g_rt)))
                g_rt_w = desc.Width, g_rt_h = desc.Height, g_rt_fmt = desc.Format;
        }
        if (g_rt && SUCCEEDED(IDirect3DDevice8_GetRenderTarget(d, &was))) {
            if (FAILED(IDirect3DDevice8_GetDepthStencilSurface(d, &z))) z = NULL;
            const int16_t(*move)[2] = (const int16_t(*)[2])((char *)l + HK_PC_QUAD_MOVE);
            g_replaying = 1;
            g_SRT(d, g_rt, z);
            IDirect3DDevice8_ApplyStateBlock(d, g_sb_start);
            static uint8_t *verts;
            static size_t room;
            for (size_t i = 0; i < g_rec.n; i++) {
                const struct op *o = &g_rec.ops[i];
                const uint8_t *data = g_rec.pool + o->at;
                switch (o->kind) {
                case OP_RS: g_RS(d, (D3DRENDERSTATETYPE)o->a, o->b); break;
                case OP_TSS: g_TSS(d, o->a, (D3DTEXTURESTAGESTATETYPE)o->b, o->c); break;
                case OP_TEX: g_ST(d, o->a, (IDirect3DBaseTexture8 *)(uintptr_t)o->b); break;
                case OP_VS: g_SVS(d, o->a); break;
                case OP_TRANSFORM: g_STR(d, (D3DTRANSFORMSTATETYPE)o->a, (const D3DMATRIX *)data); break;
                case OP_VIEWPORT: g_SVP(d, (const D3DVIEWPORT8 *)data); break;
                case OP_MATERIAL: g_SM(d, (const D3DMATERIAL8 *)data); break;
                case OP_LIGHT: g_SetLight(d, o->a, (const D3DLIGHT8 *)data); break;
                case OP_LIGHT_ON: g_LE(d, o->a, (BOOL)o->b); break;
                case OP_CLEAR: {
                    float z0;
                    DWORD s;
                    memcpy(&z0, data, sizeof z0);
                    memcpy(&s, data + 4, sizeof s);
                    g_CL(d, o->a, o->a ? (const D3DRECT *)(data + 8) : NULL, o->b, o->c, z0, s);
                    break;
                }
                case OP_BEGIN: g_BS(d); break;
                case OP_END: g_ES(d); break;
                case OP_DPUP: {
                    const void *v = data;
                    int moved = 0;
                    for (int g = 0; g < o->nquads; g++) moved |= move[o->quad + g][0] || move[o->quad + g][1];
                    if (moved) {
                        if (room < o->size) {
                            uint8_t *q = (uint8_t *)realloc(verts, o->size);
                            if (!q) break;
                            verts = q, room = o->size;
                        }
                        memcpy(verts, data, o->size);
                        for (UINT k = 0, i = 0; k + o->c <= o->size; k += o->c, i++) {
                            const UINT g = i / o->per;
                            if ((int)g >= o->nquads) break;
                            float *p = (float *)(verts + k);
                            p[0] += move[o->quad + g][0], p[1] += move[o->quad + g][1];
                        }
                        v = verts;
                    }
                    g_DPUP(d, (D3DPRIMITIVETYPE)o->a, o->b, v, o->c);
                    break;
                }
                }
            }
            g_SRT(d, was, z);
            IDirect3DDevice8_ApplyStateBlock(d, g_sb_end);
            g_replaying = 0;
            IDirect3DSurface8_Release(was);
            if (z) IDirect3DSurface8_Release(z);
            // The frame's own picture, and of the one drawn again only where a quad was moved from and
            // to: nothing else can differ (the movie's texture is the decoder's, written meanwhile)
            D3DLOCKED_RECT lr;
            static uint32_t *again;
            static size_t again_room;
            const size_t px = (size_t)g_rt_w * g_rt_h;
            if (again_room < px) {
                uint32_t *a = (uint32_t *)realloc(again, px * 4);
                if (a) again = a, again_room = px;
            }
            if (again_room >= px && l->frame_w == g_rt_w && l->frame_h == g_rt_h &&
                SUCCEEDED(IDirect3DSurface8_LockRect(g_rt, &lr, NULL, D3DLOCK_READONLY))) {
                hk_convert_pixels(again, lr.pBits, lr.Pitch, g_rt_w, g_rt_h, (int)g_rt_fmt);
                IDirect3DSurface8_UnlockRect(g_rt);
                uint32_t *out = (uint32_t *)((char *)l + HK_PC_BETWEEN);
                memcpy(out, (const char *)l + HK_PC_PIXELS, px * 4);
                const struct hk_pc_quad *q = (const struct hk_pc_quad *)((const char *)l + HK_PC_QUAD_LIST);
                for (int i = 0; i < g_rec.quads; i++) {
                    const int mx = move[i][0], my = move[i][1];
                    if (!mx && !my) continue;
                    // the place before and after, a pixel more each way (the edges' filtering)
                    int x0 = q[i].x + (mx < 0 ? mx : 0) - 1, x1 = q[i].x + q[i].w + (mx > 0 ? mx : 0) + 1;
                    int y0 = q[i].y + (my < 0 ? my : 0) - 1, y1 = q[i].y + q[i].h + (my > 0 ? my : 0) + 1;
                    if (x0 < 0) x0 = 0;
                    if (y0 < 0) y0 = 0;
                    if (x1 > (int)g_rt_w) x1 = (int)g_rt_w;
                    if (y1 > (int)g_rt_h) y1 = (int)g_rt_h;
                    for (int y = y0; y < y1; y++)
                        memcpy(out + (size_t)y * g_rt_w + x0, again + (size_t)y * g_rt_w + x0, (size_t)(x1 - x0) * 4);
                }
                ok = 1;
            }
        }
    }
    l->between_ok = ok;
    MemoryBarrier();
    l->between_done = req;
}

static HRESULT WINAPI my_Present(IDirect3DDevice8 *d, const RECT *src, const RECT *dst, HWND wnd, const RGNDATA *dirty) {
    hk_present_begin();
    if (!hk_host_shows()) return g_Present(d, src, dst, wnd, dirty);
    struct hk_pc_link *l = hk_link();
    const int captured = capture(d);
    // the frame's quads with its picture, if it can be drawn again (its end state captured)
    if (logging()) hk_log("draw %u: Present", g_frame);
    g_frame++;
    g_rec.ready = captured && g_rec.on && !g_rec.broken && SUCCEEDED(IDirect3DDevice8_CaptureStateBlock(d, g_sb_end));
    l->quads_n = g_rec.ready ? (uint32_t)g_rec.quads : 0;
    l->quads_seq = l->frame_seq + (captured ? 1 : 0);
    g_dev8 = d;
    hk_picture_drawn(captured);   // (in-between pictures drawn while it waits)
    rec_start(d);
    return D3D_OK;
}

// A light's alpha: Direct3D lights a vertex with the material's diffuse alpha and ignores the lights'
// own, but Wine multiplies the light's in. DistorteD's light has diffuse alpha 0 and its MODE SELECT
// blends by the lit alpha (source alpha, zero), so under Wine the whole screen came out black
// (bemanitools' iidx13_fix_song_select_bg turns lighting off instead, losing the shading). Each light
// is handed on with alpha 1: no change where Direct3D's rule holds, the cabinet's look where it doesn't.

static HRESULT WINAPI my_SetLight(IDirect3DDevice8 *d, DWORD i, const D3DLIGHT8 *l) {
    if (!l) return g_SetLight(d, i, l);
    D3DLIGHT8 c = *l;
    c.Diffuse.a = c.Ambient.a = c.Specular.a = 1.0f;
    if (recording()) {
        struct op *o = add(OP_LIGHT, i, 0, 0);
        if (o) o->at = keep(&c, sizeof c);
    }
    return g_SetLight(d, i, &c);
}

static HRESULT WINAPI my_Reset(IDirect3DDevice8 *d, D3DPRESENT_PARAMETERS *pp) {
    windowed(pp);
    state_blocks_gone(d);
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
        // in-between pictures (smooth motion): the frame's drawing recorded while hiraku-pc asks
#define HOOK(slot, fn, var, type) if ((was = hk_patch_slot((void **)&vt->slot, (void *)fn))) var = (type)was
        HOOK(SetRenderState, my_RS, g_RS, RS8_t);
        HOOK(SetTextureStageState, my_TSS, g_TSS, TSS8_t);
        HOOK(SetTexture, my_ST, g_ST, ST8_t);
        HOOK(SetVertexShader, my_SVS, g_SVS, SVS8_t);
        HOOK(SetTransform, my_STR, g_STR, STR8_t);
        HOOK(SetViewport, my_SVP, g_SVP, SVP8_t);
        HOOK(SetMaterial, my_SM, g_SM, SM8_t);
        HOOK(LightEnable, my_LE, g_LE, LE8_t);
        HOOK(Clear, my_CL, g_CL, CL8_t);
        HOOK(BeginScene, my_BS, g_BS, BS8_t);
        HOOK(EndScene, my_ES, g_ES, BS8_t);
        HOOK(DrawPrimitiveUP, my_DPUP, g_DPUP, DPUP8_t);
        HOOK(DrawPrimitive, my_DP, g_DP, DP8_t);
        HOOK(SetStreamSource, my_SSS, g_SSS, SSS8_t);
        HOOK(DrawIndexedPrimitive, my_DIP, g_DIP, DIP8_t);
        HOOK(DrawIndexedPrimitiveUP, my_DIPUP, g_DIPUP, DIPUP8_t);
        HOOK(SetRenderTarget, my_SRT, g_SRT, SRT8_t);
        HOOK(SetPixelShader, my_SPS, g_SPS, SPS8_t);
        HOOK(SetVertexShaderConstant, my_SVSC, g_SVSC, SC8_t);
        HOOK(SetPixelShaderConstant, my_SPSC, g_SPSC, SC8_t);
        HOOK(CopyRects, my_CR, g_CR, CR8_t);
#undef HOOK
        hk_between = draw_between;
        const char *e = getenv("HK_PC_DRAWLOG");
        if (e && sscanf(e, "%u,%u", &g_log_first, &g_log_count) == 2) hk_log("hiraku-pc: HK_PC_DRAWLOG %u,%u (Direct3D 8)", g_log_first, g_log_count);
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
