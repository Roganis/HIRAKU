// SPDX-License-Identifier: GPL-3.0-or-later
// In-between pictures for the Direct3D 9 games (smooth motion, docs/SMOOTH.md; link v11), as d3d8.c
// does for the Direct3D 8 ones: while hiraku-pc asks for them, each frame's drawing is recorded (the
// states set, the textures, each DrawPrimitiveUP's vertices: beatmania IIDX GOLD and later draw
// everything so, quads in screen space with no shaders), its quads listed with its picture, and while
// the game waits for its next frame the frame is drawn again into a render target of its own with the
// quads hiraku-pc moves (the notes) shifted, between the frame's starting and ending states (state
// blocks), and read back. Of that picture only the moved quads' places are taken: the rest is the
// frame's own (the movie's texture is the decoder's, written meanwhile). A frame drawn in a way this
// doesn't record gets none, said once in pc.log.
//
// HK_PC_DRAWLOG=FIRST,COUNT: each draw of those pictures in pc.log (how a game draws).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define COBJMACROS
#include <d3d9.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cabinet.h"

enum { OP_RS, OP_TSS, OP_SS, OP_TEX, OP_FVF, OP_TRANSFORM, OP_VIEWPORT, OP_MATERIAL, OP_LIGHT, OP_LIGHT_ON, OP_CLEAR, OP_SCISSOR,
       OP_BEGIN, OP_END, OP_DPUP };
struct op {
    int kind;
    DWORD a, b, c;
    size_t at;   // its data in the pool
    UINT size;
    int quad;     // OP_DPUP: its first quad in the frame's, or -1
    int nquads;   // ... how many (a list's quads drawn in one call), each `per` vertices
    UINT per;
};
static struct {
    int on, broken, ready;
    struct op *ops;
    size_t n, cap;
    uint8_t *pool;
    size_t used, room;
    IDirect3DBaseTexture9 *held[256];
    int nheld;
    IDirect3DBaseTexture9 *tex0;
    DWORD fvf;
    int decl, vs, ps;   // a vertex declaration (not SetFVF's), a vertex or a pixel shader in effect
    int quads;
} g_rec;
static IDirect3DStateBlock9 *g_sb_start, *g_sb_end;
static IDirect3DSurface9 *g_rt, *g_sys;   // where pictures are drawn again, and read back from
static UINT g_rt_w, g_rt_h;
static D3DFORMAT g_rt_fmt;
static IDirect3DDevice9 *g_dev;
static int g_replaying;
static unsigned g_frame, g_log_first = ~0u, g_log_count;

static int logging(void) { return g_frame >= g_log_first && g_frame < g_log_first + g_log_count; }
static int recording(void) { return g_rec.on && !g_rec.broken && !g_replaying; }
static void cant(const char *why) {
    static char said[16][32];
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
static size_t keep(const void *p, size_t n) {
    if (g_rec.used + n > g_rec.room) {
        size_t r = g_rec.room ? g_rec.room : 65536;
        while (r < g_rec.used + n) r *= 2;
        uint8_t *q = (uint8_t *)realloc(g_rec.pool, r);
        if (!q) {
            g_rec.broken = 1;
            return 0;
        }
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
        if (!o) {
            g_rec.broken = 1;
            return NULL;
        }
        g_rec.ops = o, g_rec.cap = cap;
    }
    struct op *o = &g_rec.ops[g_rec.n++];
    o->kind = kind, o->a = a, o->b = b, o->c = c, o->at = 0, o->size = 0, o->quad = -1, o->nquads = 0, o->per = 0;
    return o;
}
static void drop_textures(void) {
    for (int i = 0; i < g_rec.nheld; i++) IDirect3DBaseTexture9_Release(g_rec.held[i]);
    g_rec.nheld = 0;
}

typedef HRESULT(WINAPI *RS9_t)(IDirect3DDevice9 *, D3DRENDERSTATETYPE, DWORD);
typedef HRESULT(WINAPI *TSS9_t)(IDirect3DDevice9 *, DWORD, D3DTEXTURESTAGESTATETYPE, DWORD);
typedef HRESULT(WINAPI *SS9_t)(IDirect3DDevice9 *, DWORD, D3DSAMPLERSTATETYPE, DWORD);
typedef HRESULT(WINAPI *ST9_t)(IDirect3DDevice9 *, DWORD, IDirect3DBaseTexture9 *);
typedef HRESULT(WINAPI *SFVF9_t)(IDirect3DDevice9 *, DWORD);
typedef HRESULT(WINAPI *STR9_t)(IDirect3DDevice9 *, D3DTRANSFORMSTATETYPE, const D3DMATRIX *);
typedef HRESULT(WINAPI *SVP9_t)(IDirect3DDevice9 *, const D3DVIEWPORT9 *);
typedef HRESULT(WINAPI *SM9_t)(IDirect3DDevice9 *, const D3DMATERIAL9 *);
typedef HRESULT(WINAPI *SL9_t)(IDirect3DDevice9 *, DWORD, const D3DLIGHT9 *);
typedef HRESULT(WINAPI *LE9_t)(IDirect3DDevice9 *, DWORD, BOOL);
typedef HRESULT(WINAPI *CL9_t)(IDirect3DDevice9 *, DWORD, const D3DRECT *, DWORD, D3DCOLOR, float, DWORD);
typedef HRESULT(WINAPI *SSR9_t)(IDirect3DDevice9 *, const RECT *);
typedef HRESULT(WINAPI *BS9_t)(IDirect3DDevice9 *);
typedef HRESULT(WINAPI *DPUP9_t)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, const void *, UINT);
typedef HRESULT(WINAPI *DP9_t)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT);
typedef HRESULT(WINAPI *DIP9_t)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
typedef HRESULT(WINAPI *DIPUP9_t)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void *, D3DFORMAT, const void *, UINT);
typedef HRESULT(WINAPI *SRT9_t)(IDirect3DDevice9 *, DWORD, IDirect3DSurface9 *);
typedef HRESULT(WINAPI *SDS9_t)(IDirect3DDevice9 *, IDirect3DSurface9 *);
typedef HRESULT(WINAPI *SVD9_t)(IDirect3DDevice9 *, IDirect3DVertexDeclaration9 *);
typedef HRESULT(WINAPI *SVS9_t)(IDirect3DDevice9 *, IDirect3DVertexShader9 *);
typedef HRESULT(WINAPI *SPS9_t)(IDirect3DDevice9 *, IDirect3DPixelShader9 *);
typedef HRESULT(WINAPI *SR9_t)(IDirect3DDevice9 *, IDirect3DSurface9 *, const RECT *, IDirect3DSurface9 *, const RECT *, D3DTEXTUREFILTERTYPE);
static RS9_t g_RS; static TSS9_t g_TSS; static SS9_t g_SS; static ST9_t g_ST; static SFVF9_t g_SFVF; static STR9_t g_STR;
static SVP9_t g_SVP; static SM9_t g_SM; static SL9_t g_SL; static LE9_t g_LE; static CL9_t g_CL; static SSR9_t g_SSR;
static BS9_t g_BS, g_ES; static DPUP9_t g_DPUP; static DP9_t g_DP; static DIP9_t g_DIP; static DIPUP9_t g_DIPUP;
static SRT9_t g_SRT; static SDS9_t g_SDS; static SVD9_t g_SVD; static SVS9_t g_SVS; static SPS9_t g_SPS; static SR9_t g_SR;

static HRESULT WINAPI my_RS(IDirect3DDevice9 *d, D3DRENDERSTATETYPE s, DWORD v) {
    if (recording()) add(OP_RS, s, v, 0);
    return g_RS(d, s, v);
}
static HRESULT WINAPI my_TSS(IDirect3DDevice9 *d, DWORD st, D3DTEXTURESTAGESTATETYPE s, DWORD v) {
    if (recording()) add(OP_TSS, st, s, v);
    return g_TSS(d, st, s, v);
}
static HRESULT WINAPI my_SS(IDirect3DDevice9 *d, DWORD st, D3DSAMPLERSTATETYPE s, DWORD v) {
    if (recording()) add(OP_SS, st, s, v);
    return g_SS(d, st, s, v);
}
static HRESULT WINAPI my_ST(IDirect3DDevice9 *d, DWORD st, IDirect3DBaseTexture9 *t) {
    if (!g_replaying && st == 0) g_rec.tex0 = t;
    if (recording()) {
        if (t) {
            if (g_rec.nheld == (int)(sizeof g_rec.held / sizeof *g_rec.held)) cant("more than 256 textures");
            else IDirect3DBaseTexture9_AddRef(t), g_rec.held[g_rec.nheld++] = t;
        }
        add(OP_TEX, st, (DWORD)(uintptr_t)t, 0);
    }
    return g_ST(d, st, t);
}
static HRESULT WINAPI my_SFVF(IDirect3DDevice9 *d, DWORD f) {
    if (!g_replaying) g_rec.fvf = f, g_rec.decl = 0;   // (SetFVF sets a declaration of its own)
    if (recording()) add(OP_FVF, f, 0, 0);
    return g_SFVF(d, f);
}
static HRESULT WINAPI my_STR(IDirect3DDevice9 *d, D3DTRANSFORMSTATETYPE t, const D3DMATRIX *m) {
    if (recording() && m) {
        struct op *o = add(OP_TRANSFORM, t, 0, 0);
        if (o) o->at = keep(m, sizeof *m);
    }
    return g_STR(d, t, m);
}
static HRESULT WINAPI my_SVP(IDirect3DDevice9 *d, const D3DVIEWPORT9 *v) {
    if (recording() && v) {
        struct op *o = add(OP_VIEWPORT, 0, 0, 0);
        if (o) o->at = keep(v, sizeof *v);
    }
    return g_SVP(d, v);
}
static HRESULT WINAPI my_SM(IDirect3DDevice9 *d, const D3DMATERIAL9 *m) {
    if (recording() && m) {
        struct op *o = add(OP_MATERIAL, 0, 0, 0);
        if (o) o->at = keep(m, sizeof *m);
    }
    return g_SM(d, m);
}
static HRESULT WINAPI my_SL(IDirect3DDevice9 *d, DWORD i, const D3DLIGHT9 *l) {
    if (recording() && l) {
        struct op *o = add(OP_LIGHT, i, 0, 0);
        if (o) o->at = keep(l, sizeof *l);
    }
    return g_SL(d, i, l);
}
static HRESULT WINAPI my_LE(IDirect3DDevice9 *d, DWORD i, BOOL on) {
    if (recording()) add(OP_LIGHT_ON, i, on, 0);
    return g_LE(d, i, on);
}
static HRESULT WINAPI my_CL(IDirect3DDevice9 *d, DWORD n, const D3DRECT *r, DWORD flags, D3DCOLOR c, float z, DWORD s) {
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
static HRESULT WINAPI my_SSR(IDirect3DDevice9 *d, const RECT *r) {
    if (recording() && r) {
        struct op *o = add(OP_SCISSOR, 0, 0, 0);
        if (o) o->at = keep(r, sizeof *r);
    }
    return g_SSR(d, r);
}
static HRESULT WINAPI my_BS(IDirect3DDevice9 *d) {
    if (recording()) add(OP_BEGIN, 0, 0, 0);
    return g_BS(d);
}
static HRESULT WINAPI my_ES(IDirect3DDevice9 *d) {
    if (recording()) add(OP_END, 0, 0, 0);
    return g_ES(d);
}
static void log_draw(const char *what, const void *v, UINT stride) {
    const float *f = (const float *)v;
    hk_log("draw %u: %s fvf %#lx tex %p stride %u v0 %.1f %.1f %.3f %.3f", g_frame, what, (unsigned long)g_rec.fvf,
           (void *)g_rec.tex0, stride, f[0], f[1], f[2], f[3]);
}
static HRESULT WINAPI my_DPUP(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT n, const void *v, UINT stride) {
    if (logging() && v) {
        char w[48];
        snprintf(w, sizeof w, "DPUP t%d n%u", (int)t, n);
        log_draw(w, v, stride);
    }
    if (recording() && v) {
        if (g_rec.decl) cant("a vertex declaration");
        if (g_rec.vs) cant("a vertex shader");
        if (g_rec.ps) cant("a pixel shader");
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
    return g_DPUP(d, t, n, v, stride);
}
// what isn't recorded: the frame can't be drawn again
static HRESULT WINAPI my_DP(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT a, UINT n) {
    if (logging()) hk_log("draw %u: DP t%d n%u fvf %#lx tex %p", g_frame, (int)t, n, (unsigned long)g_rec.fvf, (void *)g_rec.tex0);
    if (recording()) cant("DrawPrimitive");
    return g_DP(d, t, a, n);
}
static HRESULT WINAPI my_DIP(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, INT base, UINT a, UINT b, UINT c, UINT n) {
    if (logging()) hk_log("draw %u: DIP t%d n%u fvf %#lx tex %p", g_frame, (int)t, n, (unsigned long)g_rec.fvf, (void *)g_rec.tex0);
    if (recording()) cant("DrawIndexedPrimitive");
    return g_DIP(d, t, base, a, b, c, n);
}
static HRESULT WINAPI my_DIPUP(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT a, UINT b, UINT n, const void *ix, D3DFORMAT f, const void *v, UINT s) {
    if (logging()) hk_log("draw %u: DIPUP t%d n%u", g_frame, (int)t, n);
    if (recording()) cant("DrawIndexedPrimitiveUP");
    return g_DIPUP(d, t, a, b, n, ix, f, v, s);
}
static HRESULT WINAPI my_SRT(IDirect3DDevice9 *d, DWORD i, IDirect3DSurface9 *s) {
    if (logging() && !g_replaying) hk_log("draw %u: SetRenderTarget %lu %p", g_frame, (unsigned long)i, (void *)s);
    if (recording()) cant("SetRenderTarget");
    return g_SRT(d, i, s);
}
static HRESULT WINAPI my_SDS(IDirect3DDevice9 *d, IDirect3DSurface9 *s) {
    if (recording()) cant("SetDepthStencilSurface");
    return g_SDS(d, s);
}
static HRESULT WINAPI my_SVD(IDirect3DDevice9 *d, IDirect3DVertexDeclaration9 *v) {
    if (logging() && !g_replaying) hk_log("draw %u: SetVertexDeclaration %p", g_frame, (void *)v);
    if (!g_replaying) g_rec.decl = v != NULL;
    return g_SVD(d, v);
}
static HRESULT WINAPI my_SVS(IDirect3DDevice9 *d, IDirect3DVertexShader9 *s) {
    if (logging() && !g_replaying) hk_log("draw %u: SetVertexShader %p", g_frame, (void *)s);
    if (!g_replaying) g_rec.vs = s != NULL;
    return g_SVS(d, s);
}
static HRESULT WINAPI my_SPS(IDirect3DDevice9 *d, IDirect3DPixelShader9 *s) {
    if (logging() && !g_replaying) hk_log("draw %u: SetPixelShader %p", g_frame, (void *)s);
    if (!g_replaying) g_rec.ps = s != NULL;
    return g_SPS(d, s);
}
static HRESULT WINAPI my_SR(IDirect3DDevice9 *d, IDirect3DSurface9 *s, const RECT *r, IDirect3DSurface9 *t, const RECT *tr, D3DTEXTUREFILTERTYPE f) {
    if (logging() && !g_replaying) hk_log("draw %u: StretchRect %p -> %p", g_frame, (void *)s, (void *)t);
    if (recording()) cant("StretchRect");
    return g_SR(d, s, r, t, tr, f);
}

static void hook_all(IDirect3DDevice9 *dev) {
    IDirect3DDevice9Vtbl *vt = (IDirect3DDevice9Vtbl *)dev->lpVtbl;
    void *was;
#define HOOK(slot, fn, var, type)                                     \
    if ((void *)vt->slot != (void *)fn && (was = hk_patch_slot((void **)&vt->slot, (void *)fn))) var = (type)was
    HOOK(SetRenderState, my_RS, g_RS, RS9_t);
    HOOK(SetTextureStageState, my_TSS, g_TSS, TSS9_t);
    HOOK(SetSamplerState, my_SS, g_SS, SS9_t);
    HOOK(SetTexture, my_ST, g_ST, ST9_t);
    HOOK(SetFVF, my_SFVF, g_SFVF, SFVF9_t);
    HOOK(SetTransform, my_STR, g_STR, STR9_t);
    HOOK(SetViewport, my_SVP, g_SVP, SVP9_t);
    HOOK(SetMaterial, my_SM, g_SM, SM9_t);
    HOOK(SetLight, my_SL, g_SL, SL9_t);
    HOOK(LightEnable, my_LE, g_LE, LE9_t);
    HOOK(Clear, my_CL, g_CL, CL9_t);
    HOOK(SetScissorRect, my_SSR, g_SSR, SSR9_t);
    HOOK(BeginScene, my_BS, g_BS, BS9_t);
    HOOK(EndScene, my_ES, g_ES, BS9_t);
    HOOK(DrawPrimitiveUP, my_DPUP, g_DPUP, DPUP9_t);
    HOOK(DrawPrimitive, my_DP, g_DP, DP9_t);
    HOOK(DrawIndexedPrimitive, my_DIP, g_DIP, DIP9_t);
    HOOK(DrawIndexedPrimitiveUP, my_DIPUP, g_DIPUP, DIPUP9_t);
    HOOK(SetRenderTarget, my_SRT, g_SRT, SRT9_t);
    HOOK(SetDepthStencilSurface, my_SDS, g_SDS, SDS9_t);
    HOOK(SetVertexDeclaration, my_SVD, g_SVD, SVD9_t);
    HOOK(SetVertexShader, my_SVS, g_SVS, SVS9_t);
    HOOK(SetPixelShader, my_SPS, g_SPS, SPS9_t);
    HOOK(StretchRect, my_SR, g_SR, SR9_t);
#undef HOOK
}
// all of them still the device's (Windows' Direct3D 9 put another table's Present in place once,
// GitHub issue 4): put back where not, and that frame not recorded
static int hooks_in(IDirect3DDevice9 *dev) {
    const IDirect3DDevice9Vtbl *vt = (const IDirect3DDevice9Vtbl *)dev->lpVtbl;
    if ((void *)vt->DrawPrimitiveUP == (void *)my_DPUP && (void *)vt->SetTexture == (void *)my_ST &&
        (void *)vt->SetRenderState == (void *)my_RS && (void *)vt->SetFVF == (void *)my_SFVF)
        return 1;
    hook_all(dev);
    return 0;
}

// The frame drawn again, its quads moved as hiraku-pc says, into the link's in-between pixels
static void draw_between(void) {
    struct hk_pc_link *l = hk_link();
    const uint32_t req = l->between_req;
    int ok = 0;
    IDirect3DDevice9 *d = g_dev;
    IDirect3DSurface9 *bb = NULL, *was = NULL, *z = NULL;
    if (d && g_rec.ready && SUCCEEDED(IDirect3DDevice9_GetRenderTarget(d, 0, &bb))) {
        D3DSURFACE_DESC desc;
        IDirect3DSurface9_GetDesc(bb, &desc);
        if (g_rt && (g_rt_w != desc.Width || g_rt_h != desc.Height || g_rt_fmt != desc.Format)) {
            IDirect3DSurface9_Release(g_rt), g_rt = NULL;
            if (g_sys) IDirect3DSurface9_Release(g_sys), g_sys = NULL;
        }
        if (!g_rt && SUCCEEDED(IDirect3DDevice9_CreateRenderTarget(d, desc.Width, desc.Height, desc.Format, D3DMULTISAMPLE_NONE, 0,
                                                                     FALSE, &g_rt, NULL))) {
            g_rt_w = desc.Width, g_rt_h = desc.Height, g_rt_fmt = desc.Format;
            if (FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(d, desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &g_sys, NULL)))
                g_sys = NULL;
        }
        was = bb;
    }
    if (was && g_rt && g_sys) {
        if (FAILED(IDirect3DDevice9_GetDepthStencilSurface(d, &z))) z = NULL;
        const int16_t(*move)[2] = (const int16_t(*)[2])((char *)l + HK_PC_QUAD_MOVE);
        static uint8_t *verts;
        static size_t room;
        g_replaying = 1;
        g_SRT(d, 0, g_rt);
        IDirect3DStateBlock9_Apply(g_sb_start);
        for (size_t i = 0; i < g_rec.n; i++) {
            const struct op *o = &g_rec.ops[i];
            const uint8_t *data = g_rec.pool + o->at;
            switch (o->kind) {
            case OP_RS: g_RS(d, (D3DRENDERSTATETYPE)o->a, o->b); break;
            case OP_TSS: g_TSS(d, o->a, (D3DTEXTURESTAGESTATETYPE)o->b, o->c); break;
            case OP_SS: g_SS(d, o->a, (D3DSAMPLERSTATETYPE)o->b, o->c); break;
            case OP_TEX: g_ST(d, o->a, (IDirect3DBaseTexture9 *)(uintptr_t)o->b); break;
            case OP_FVF: g_SFVF(d, o->a); break;
            case OP_TRANSFORM: g_STR(d, (D3DTRANSFORMSTATETYPE)o->a, (const D3DMATRIX *)data); break;
            case OP_VIEWPORT: g_SVP(d, (const D3DVIEWPORT9 *)data); break;
            case OP_MATERIAL: g_SM(d, (const D3DMATERIAL9 *)data); break;
            case OP_LIGHT: g_SL(d, o->a, (const D3DLIGHT9 *)data); break;
            case OP_LIGHT_ON: g_LE(d, o->a, (BOOL)o->b); break;
            case OP_SCISSOR: g_SSR(d, (const RECT *)data); break;
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
        g_SRT(d, 0, was);
        IDirect3DStateBlock9_Apply(g_sb_end);
        g_replaying = 0;
        // the frame's own picture, and of the one drawn again only where a quad was moved from and to
        D3DLOCKED_RECT lr;
        static uint32_t *again;
        static size_t again_room;
        const size_t px = (size_t)g_rt_w * g_rt_h;
        if (again_room < px) {
            uint32_t *a = (uint32_t *)realloc(again, px * 4);
            if (a) again = a, again_room = px;
        }
        if (again_room >= px && l->frame_w == g_rt_w && l->frame_h == g_rt_h &&
            SUCCEEDED(IDirect3DDevice9_GetRenderTargetData(d, g_rt, g_sys)) &&
            SUCCEEDED(IDirect3DSurface9_LockRect(g_sys, &lr, NULL, D3DLOCK_READONLY))) {
            hk_convert_pixels(again, lr.pBits, lr.Pitch, g_rt_w, g_rt_h, (int)g_rt_fmt);
            IDirect3DSurface9_UnlockRect(g_sys);
            uint32_t *out = (uint32_t *)((char *)l + HK_PC_BETWEEN);
            memcpy(out, (const char *)l + HK_PC_PIXELS, px * 4);
            const struct hk_pc_quad *q = (const struct hk_pc_quad *)((const char *)l + HK_PC_QUAD_LIST);
            for (int i = 0; i < g_rec.quads; i++) {
                const int mx = move[i][0], my = move[i][1];
                if (!mx && !my) continue;
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
        if (z) IDirect3DSurface9_Release(z);
    }
    if (was) IDirect3DSurface9_Release(was);
    l->between_ok = ok;
    MemoryBarrier();
    l->between_done = req;
}

void hk_d3d9_hooks(IDirect3DDevice9 *dev) {
    const char *e = getenv("HK_PC_DRAWLOG");
    if (e) sscanf(e, "%u,%u", &g_log_first, &g_log_count);
    if (!hk_host_shows()) return;
    if (e) hk_log("hiraku-pc: HK_PC_DRAWLOG %u,%u (Direct3D 9)", g_log_first, g_log_count);
    hook_all(dev);
    hk_between = draw_between;
}

// At Present, the picture read back (captured): the frame's quads with it, if it can be drawn again
void hk_d3d9_frame_end(IDirect3DDevice9 *d, int captured) {
    if (logging()) hk_log("draw %u: Present", g_frame);
    g_frame++;
    struct hk_pc_link *l = hk_link();
    g_rec.ready = captured && g_rec.on && !g_rec.broken && g_sb_end && SUCCEEDED(IDirect3DStateBlock9_Capture(g_sb_end));
    l->quads_n = g_rec.ready ? (uint32_t)g_rec.quads : 0;
    l->quads_seq = l->frame_seq + (captured ? 1 : 0);
    g_dev = d;
}

// After it (the game's next frame): a new recording, from the state the device has now
void hk_d3d9_frame_start(IDirect3DDevice9 *d) {
    drop_textures();
    g_rec.n = 0, g_rec.used = 0, g_rec.broken = 0, g_rec.quads = 0, g_rec.ready = 0;
    g_rec.on = hk_link()->smooth && hk_host_shows() && hooks_in(d) && g_RS && g_TSS && g_SS && g_ST && g_SFVF && g_STR && g_SVP &&
               g_SM && g_SL && g_LE && g_CL && g_SSR && g_BS && g_ES && g_DPUP && g_SRT;
    if (!g_rec.on) return;
    if (!g_sb_start && FAILED(IDirect3DDevice9_CreateStateBlock(d, D3DSBT_ALL, &g_sb_start))) g_sb_start = NULL;
    if (!g_sb_end && FAILED(IDirect3DDevice9_CreateStateBlock(d, D3DSBT_ALL, &g_sb_end))) g_sb_end = NULL;
    if (!g_sb_start || !g_sb_end || FAILED(IDirect3DStateBlock9_Capture(g_sb_start))) g_rec.on = 0;
}

// Before a Reset: what it doesn't let live (the default pool's render target, the state blocks)
void hk_d3d9_reset(void) {
    if (g_sb_start) IDirect3DStateBlock9_Release(g_sb_start), g_sb_start = NULL;
    if (g_sb_end) IDirect3DStateBlock9_Release(g_sb_end), g_sb_end = NULL;
    if (g_rt) IDirect3DSurface9_Release(g_rt), g_rt = NULL;
    if (g_sys) IDirect3DSurface9_Release(g_sys), g_sys = NULL;
    drop_textures();
    g_rec.on = g_rec.ready = 0;
}
