// SPDX-License-Identifier: GPL-3.0-or-later
// The loader's log, and the C++ exceptions the game throws (with where: they name what it lacked,
// as std::out_of_range named pcb.boot's region, PLAN.md section 32), and its faults (an access
// violation, ...: under Wine an unhandled one waits in its debugger, the game stopped and silent)
#include "pc64.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

FILE *pc64_log_file(void) {
    static FILE *f;
    if (!f) {
        const char *path = getenv("HK_PC64_LOG");
        f = path ? fopen(path, "a") : NULL;
        if (!f) f = stderr;
    }
    return f;
}

void pc64_log(const char *fmt, ...) {
    FILE *f = pc64_log_file();
    va_list a;
    va_start(a, fmt);
    fputs("hiraku-pc64: ", f);
    vfprintf(f, fmt, a);
    fputc('\n', f);
    va_end(a);
    fflush(f);
}

// An address as module+offset
static void where(const char *what, const void *at) {
    HMODULE m = NULL;
    char path[MAX_PATH] = "?";
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, at, &m);
    if (m) GetModuleFileNameA(m, path, sizeof path);
    const char *b = strrchr(path, '\\');
    pc64_log("  %s%s+%llx", what, b ? b + 1 : path, (unsigned long long)((const char *)at - (const char *)m));
}
static void stack(void) {
    void *frames[16];
    USHORT n = CaptureStackBackTrace(1, 16, frames, NULL);
    for (USHORT i = 0; i < n; i++) where("", frames[i]);
}

// A fault (the first few): where, and for an access violation the address read or written
static void on_fault(PEXCEPTION_POINTERS ep) {
    static volatile LONG said;
    EXCEPTION_RECORD *r = ep->ExceptionRecord;
    if (InterlockedIncrement(&said) > 8) return;
    pc64_log("the game faulted: %08lx (thread %lu)", (unsigned long)r->ExceptionCode, GetCurrentThreadId());
    where("at ", r->ExceptionAddress);
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2)
        pc64_log("  %s %p", r->ExceptionInformation[0] == 1 ? "writing" : r->ExceptionInformation[0] == 8 ? "running" : "reading",
                 (void *)r->ExceptionInformation[1]);
    pc64_log("  rsp %p", (void *)ep->ContextRecord->Rsp);
    const ULONG_PTR *sp = (const ULONG_PTR *)ep->ContextRecord->Rsp;   // return addresses on the stack, as found
    for (int i = 0, n = 0; i < 64 && n < 12; i++) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void *)sp[i], &mi, sizeof mi) || !(mi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE | PAGE_EXECUTE_READWRITE))) continue;
        where("  ", (void *)sp[i]);
        n++;
    }
}

// MSVC's C++ throw (0xe06d7363): the thrown type is in its ThrowInfo's first CatchableType's
// TypeDescriptor; std::exception's what() is its vtable's second slot. The stack as module offsets.
static LONG CALLBACK on_throw(PEXCEPTION_POINTERS ep) {
    EXCEPTION_RECORD *r = ep->ExceptionRecord;
    switch (r->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_PRIV_INSTRUCTION:
    case 0xc0000409:   // a fail-fast (the stack's cookie, an invalid parameter)
        on_fault(ep);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (r->ExceptionCode != 0xe06d7363 || r->NumberParameters < 4) return EXCEPTION_CONTINUE_SEARCH;
    ULONG_PTR obj = r->ExceptionInformation[1], info = r->ExceptionInformation[2], base = r->ExceptionInformation[3];
    const char *name = "?";
    if (info && base) {
        const int *types = (const int *)(base + *(const int *)(info + 12));
        if (types[0] > 0) name = (const char *)(base + *(const int *)(base + types[1] + 4) + 16);
    }
    pc64_log("the game threw %s", name);
    if (obj && strstr(name, "exception")) {
        const char *(*what)(void *) = (const char *(*)(void *))(*(void ***)obj)[1];
        pc64_log("  what(): %s", what((void *)obj));
    }
    stack();
    return EXCEPTION_CONTINUE_SEARCH;
}

void pc64_faults_install(void) {
    AddVectoredExceptionHandler(1, on_throw);
}

// HK_PC64_TRACE_STRUCTS (tests): the game's searches for answer nodes (avs2-core's XCgsqzn00000a1,
// ordinal 162: a child by name) and its struct reads (XCgsqzn00000b2, ordinal 179: a node into a
// struct by a descriptor in popn.dll), each with what came of it: which part of an answer it
// doesn't take (PLAN.md section 32, the player's record)
static void *(*r_search)(void *, void *, const char *);
static char (*r_struct)(void *, void *, void *, const void *);
static void *search_(void *ctx, void *node, const char *name) {
    void *r = r_search(ctx, node, name);
    pc64_log("answer: node %s%s", name ? name : "?", r ? "" : ": none");
    return r;
}
static char struct_(void *ctx, void *node, void *dst, const void *desc) {
    const char r = r_struct(ctx, node, dst, desc);
    HMODULE m = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, desc, &m);
    pc64_log("answer: struct %llx: %s", (unsigned long long)((const char *)desc - (const char *)m + 0x180000000ull), r ? "read" : "FAILED");
    return r;
}
void pc64_trace_install(HMODULE game) {
    if (!getenv("HK_PC64_TRACE_STRUCTS")) return;
    r_search = pc64_import_ordinal(game, "avs2-core.dll", 162, (void *)search_);
    r_struct = pc64_import_ordinal(game, "avs2-core.dll", 179, (void *)struct_);
    pc64_log("tracing the answers' nodes and structs");
}

// HK_PC64_PEEK (tests): "SECONDS;WHAT;WHAT..." logs the game's memory every SECONDS, each WHAT an
// address as popn.dll's (0x180...), then * (the pointer there) and +N in turn, then :SIZE, as
// "0x180fe6270*+0xc:64" (a global's object, 64 bytes from its +0xc)
static const char *g_peek;
static HMODULE g_peek_game;
static DWORD WINAPI peek_run(void *arg) {
    (void)arg;
    const int every = atoi(g_peek);
    for (;;) {
        Sleep((DWORD)(every > 0 ? every : 5) * 1000);
        for (const char *w = strchr(g_peek, ';'); w; w = strchr(w, ';')) {
            char *at;
            unsigned long long a = strtoull(++w, &at, 0) - 0x180000000ull + (unsigned long long)(uintptr_t)g_peek_game;
            int ok = 1;
            while (ok && (*at == '*' || *at == '+')) {
                if (*at++ == '*') ok = !IsBadReadPtr((void *)(uintptr_t)a, 8) && (a = *(unsigned long long *)(uintptr_t)a) != 0;
                else a += strtoull(at, &at, 0);
            }
            const unsigned size = *at == ':' ? (unsigned)strtoul(at + 1, NULL, 0) : 16;
            char hex[3 * 256 + 1] = "";
            for (unsigned i = 0; ok && i < size && i < 256; i++) {
                if (IsBadReadPtr((void *)(uintptr_t)(a + i), 1)) break;
                sprintf(hex + 3 * i, "%02x ", ((const unsigned char *)(uintptr_t)a)[i]);
            }
            pc64_log("peek %.*s: %s", (int)strcspn(w, ";"), w, ok ? hex : "(no pointer)");
        }
    }
    return 0;
}
void pc64_peek_install(HMODULE game) {
    if (!(g_peek = getenv("HK_PC64_PEEK"))) return;
    g_peek_game = game;
    CloseHandle(CreateThread(NULL, 0, peek_run, NULL, 0, NULL));
}

// HK_PC64_TRACE_D3DX (tests): the game's D3DX textures, effects and shaders, each with its result
// (d3dx9_43, Wine's own under Wine): what didn't come out as the game meant it
#include <d3d9.h>
typedef HRESULT(WINAPI *tex_mem_ex_t)(IDirect3DDevice9 *, const void *, UINT, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, DWORD, DWORD,
                                      D3DCOLOR, void *, PALETTEENTRY *, IDirect3DTexture9 **);
typedef HRESULT(WINAPI *tex_mem_t)(IDirect3DDevice9 *, const void *, UINT, IDirect3DTexture9 **);
typedef HRESULT(WINAPI *effect_t)(IDirect3DDevice9 *, const void *, UINT, const void *, void *, DWORD, void *, void **, void **);
typedef HRESULT(WINAPI *compile_t)(const char *, UINT, const void *, void *, const char *, const char *, DWORD, void **, void **, void **);
static tex_mem_ex_t r_tex_mem_ex;
static tex_mem_t r_tex_mem;
static effect_t r_effect;
static compile_t r_compile;
static void tex_said(const char *what, HRESULT h, IDirect3DTexture9 *t, UINT size, DWORD filter, D3DFORMAT asked) {
    D3DSURFACE_DESC d = {0};
    if (SUCCEEDED(h) && t) t->lpVtbl->GetLevelDesc(t, 0, &d);
    const unsigned char *b = NULL;
    (void)b;
    pc64_log("d3dx: %s (%u bytes, format asked %u, filter %#lx): %08lx, %ux%u format %u, %lu levels", what, size, (unsigned)asked,
             (unsigned long)filter, (unsigned long)h, d.Width, d.Height, (unsigned)d.Format, SUCCEEDED(h) && t ? (unsigned long)t->lpVtbl->GetLevelCount(t) : 0ul);
}
static HRESULT WINAPI tex_mem_ex_(IDirect3DDevice9 *dev, const void *src, UINT size, UINT w, UINT hgt, UINT mips, DWORD usage, D3DFORMAT fmt,
                                  D3DPOOL pool, DWORD filter, DWORD mipfilter, D3DCOLOR key, void *info, PALETTEENTRY *pal, IDirect3DTexture9 **out) {
    const HRESULT h = r_tex_mem_ex(dev, src, size, w, hgt, mips, usage, fmt, pool, filter, mipfilter, key, info, pal, out);
    char what[96];
    snprintf(what, sizeof what, "texture %ux%u, %u levels, key %08lx, magic %.4s", w, hgt, mips, (unsigned long)key, (const char *)src);
    tex_said(what, h, out ? *out : NULL, size, filter, fmt);
    return h;
}
static HRESULT WINAPI tex_mem_(IDirect3DDevice9 *dev, const void *src, UINT size, IDirect3DTexture9 **out) {
    const HRESULT h = r_tex_mem(dev, src, size, out);
    char what[64];
    snprintf(what, sizeof what, "texture, magic %.4s", (const char *)src);
    tex_said(what, h, out ? *out : NULL, size, 0, D3DFMT_UNKNOWN);
    return h;
}
static HRESULT WINAPI effect_(IDirect3DDevice9 *dev, const void *src, UINT size, const void *defs, void *inc, DWORD flags, void *pool, void **fx,
                              void **errors) {
    const HRESULT h = r_effect(dev, src, size, defs, inc, flags, pool, fx, errors);
    pc64_log("d3dx: effect (%u bytes, flags %#lx): %08lx", size, (unsigned long)flags, (unsigned long)h);
    return h;
}
// sin() and cos() in a pixel shader for ps_2_x: Wine's HLSL compiler (vkd3d-shader) makes them its
// sincos instruction, which it can't write for those targets, and fails the shader (one of the
// game's effects then draws as white squares). Retried with both as a polynomial: the angle into
// [-pi, pi) (frac), then sin's odd series to x^11 (at most 5e-4 off)
static const char HK_SIN[] =
    "float hk_sin(float x) { float t = frac(x * 0.15915494 + 0.5) * 6.2831853 - 3.1415927; float t2 = t * t;\n"
    "  return -t * (1.0 + t2 * (-0.16666667 + t2 * (0.0083333333 + t2 * (-0.00019841270 + t2 * (2.7557319e-6 + t2 * -2.5052108e-8))))); }\n"
    "float2 hk_sin(float2 x) { return float2(hk_sin(x.x), hk_sin(x.y)); }\n"
    "float3 hk_sin(float3 x) { return float3(hk_sin(x.x), hk_sin(x.y), hk_sin(x.z)); }\n"
    "float4 hk_sin(float4 x) { return float4(hk_sin(x.x), hk_sin(x.y), hk_sin(x.z), hk_sin(x.w)); }\n"
    "float hk_cos(float x) { return hk_sin(x + 1.5707963); }\n"
    "float2 hk_cos(float2 x) { return hk_sin(x + 1.5707963); }\n"
    "float3 hk_cos(float3 x) { return hk_sin(x + 1.5707963); }\n"
    "float4 hk_cos(float4 x) { return hk_sin(x + 1.5707963); }\n";
static char *without_sincos(const char *src, UINT len, UINT *out_len) {
    char *out = malloc(sizeof HK_SIN + 2 * (size_t)len + 1), *o = out;
    memcpy(o, HK_SIN, sizeof HK_SIN - 1), o += sizeof HK_SIN - 1;
    for (UINT i = 0; i < len; i++) {
        const int word_start = i == 0 || !(isalnum((unsigned char)src[i - 1]) || src[i - 1] == '_');
        if (word_start && i + 3 < len && (!strncmp(src + i, "sin", 3) || !strncmp(src + i, "cos", 3))) {
            UINT j = i + 3;
            while (j < len && (src[j] == ' ' || src[j] == '\t')) j++;
            if (j < len && src[j] == '(') {
                memcpy(o, "hk_", 3), o += 3;
            }
        }
        *o++ = src[i];
    }
    *out_len = (UINT)(o - out);
    return out;
}
static HRESULT WINAPI compile_(const char *src, UINT len, const void *defs, void *inc, const char *fn, const char *profile, DWORD flags,
                               void **shader, void **errors, void **consts) {
    HRESULT h = r_compile(src, len, defs, inc, fn, profile, flags, shader, errors, consts);
    if (FAILED(h) && errors && *errors && src) {
        typedef struct { struct { void *qi, *addref; ULONG(WINAPI *release)(void *); void *(WINAPI *ptr)(void *); DWORD(WINAPI *size)(void *); } *v; } buf_t;
        buf_t *e = (buf_t *)*errors;
        const char *text = (const char *)e->v->ptr(e);
        if (text && strstr(text, "sincos")) {
            UINT n;
            char *fixed = without_sincos(src, len, &n);
            e->v->release(e);
            *errors = NULL;
            h = r_compile(fixed, n, defs, inc, fn, profile, flags, shader, errors, consts);
            free(fixed);
            pc64_log("d3dx: shader %s %s without sincos (Wine's compiler can't): %08lx", fn ? fn : "?", profile ? profile : "?", (unsigned long)h);
        }
    }
    if (!getenv("HK_PC64_TRACE_D3DX")) return h;
    pc64_log("d3dx: shader %s %s (%u bytes): %08lx", fn ? fn : "?", profile ? profile : "?", len, (unsigned long)h);
    if (FAILED(h)) {   // why, and its source (the game's: for the log only)
        typedef struct { struct { void *qi, *addref, *release; void *(WINAPI *ptr)(void *); DWORD(WINAPI *size)(void *); } *v; } buf_t;
        if (errors && *errors) pc64_log("d3dx: compiler: %.*s", (int)((buf_t *)*errors)->v->size(*errors), (const char *)((buf_t *)*errors)->v->ptr(*errors));
        FILE *f = pc64_log_file();
        fprintf(f, "--- %s source ---\n%.*s\n--- end ---\n", fn ? fn : "?", (int)len, src);
        fflush(f);
    }
    return h;
}
void pc64_trace_d3dx_install(HMODULE game) {
    r_compile = (compile_t)pc64_import(game, "d3dx9_43.dll", "D3DXCompileShader", (void *)compile_);   // always: sin, cos
    if (!getenv("HK_PC64_TRACE_D3DX")) return;
    r_tex_mem_ex = (tex_mem_ex_t)pc64_import(game, "d3dx9_43.dll", "D3DXCreateTextureFromFileInMemoryEx", (void *)tex_mem_ex_);
    r_tex_mem = (tex_mem_t)pc64_import(game, "d3dx9_43.dll", "D3DXCreateTextureFromFileInMemory", (void *)tex_mem_);
    r_effect = (effect_t)pc64_import(game, "d3dx9_43.dll", "D3DXCreateEffect", (void *)effect_);
    pc64_log("tracing D3DX");
}
