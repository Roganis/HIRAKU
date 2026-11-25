// Diagnostic logging of one run, each behind its own environment switch, all off by default, and
// with a switch off nothing is hooked (the run is as without this file; tests):
//   HK_PC64_WIN_LOG=1    who ends the run — PostQuitMessage, ExitProcess, FreeLibraryAndExitThread,
//                       DestroyWindow, each with the caller as module+offset — and the messages the
//                       game's windows receive (WM_CLOSE, WM_DESTROY, WM_QUIT, WM_ACTIVATEAPP,
//                       WM_KILLFOCUS, with wParam), plus WM_QUIT as PeekMessage/PostThreadMessage
//                       give it
//   HK_PC64_THREAD_LOG=1 the threads started (CreateThread: the start routine as module+offset)
//                       and ended (their exit code), and ExitThread's caller and code
//   HK_PC64_DISP_LOG=1   the monitors and display devices the game enumerates (EnumDisplayDevices,
//                       EnumDisplayMonitors, EnumDisplaySettings) and what it is told of them
//                       (the Direct3D side is logged in display.c)
//   HK_PC64_NAME_LOG=1   avs2-core's string compare (ordinal 229), as the game and its AFP modules call
//                       it: calls a second, the busiest call sites (module+offset) and a sample of the
//                       strings compared, every 5 seconds (the music select's names looked up by
//                       the hundred thousand a frame, 2026-11-23)
//   HK_PC64_PROFILE=FILE:HZ:LO:HI  the main thread's call chains, sampled HZ times a second of wall
//                       time while the main screen's pictures are LO..HI, into FILE (below)
#include "pc64.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int on(const char *name) {
    const char *v = getenv(name);
    return v && *v && *v != '0';
}
static int g_win, g_thread, g_disp;

// an address as module+offset, on its own line
static void where(const char *lead, const void *at) {
    HMODULE m = NULL;
    char path[MAX_PATH] = "?";
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, at, &m);
    if (m) GetModuleFileNameA(m, path, sizeof path);
    const char *b = strrchr(path, '\\');
    pc64_log("%s%s+%llx", lead, b ? b + 1 : path, (unsigned long long)((const char *)at - (const char *)m));
}
// the hook's caller (the game's call site)
static void caller(void) {
    void *frames[2];
    if (CaptureStackBackTrace(2, 2, frames, NULL)) where("  called by ", frames[0]);
}

// hook a module's import quietly (a diagnostic: a module that doesn't import it is just not
// logged — and must not clear the real target another module's hook found)
#define HOOKQ(m, dll, name, fn, keep) do { void *o_ = pc64_import(m, dll, name, (void *)(fn)); \
    if (o_ && keep) *(void **)(keep) = o_; } while (0)

// --- HK_PC64_WIN_LOG: who ends the run, and the messages the game's windows receive
static void(WINAPI *r_ExitProcess)(UINT);
static void(WINAPI *r_FreeLibraryAndExitThread)(HMODULE, UINT);
static void(WINAPI *r_ExitThread)(DWORD);
static void WINAPI ExitProcess_(UINT code) {
    pc64_log("win: ExitProcess(%lu)", (unsigned long)code);
    caller();
    pc64_frame_log_exit();   // HK_PC64_FRAME_LOG: the exit line and thread table before the process goes
    r_ExitProcess(code);
}
static void WINAPI FreeLibraryAndExitThread_(HMODULE m, UINT code) {
    pc64_log("win: FreeLibraryAndExitThread(%p, %lu)", (void *)m, (unsigned long)code);
    caller();
    r_FreeLibraryAndExitThread(m, code);
}
static void WINAPI ExitThread_(DWORD code) {
    pc64_log("thread: ExitThread(%lu)", (unsigned long)code);
    caller();
    r_ExitThread(code);
}
static BOOL(WINAPI *r_PostQuitMessage)(int);
static void WINAPI PostQuitMessage_(int code) {
    pc64_log("win: PostQuitMessage(%d)", code);
    caller();
    r_PostQuitMessage(code);
}
static BOOL(WINAPI *r_PostThreadMessageA)(DWORD, UINT, WPARAM, LPARAM);
static BOOL WINAPI PostThreadMessageA_(DWORD tid, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_QUIT) {
        pc64_log("win: PostThreadMessage WM_QUIT to thread %lu", (unsigned long)tid);
        caller();
    }
    return r_PostThreadMessageA(tid, msg, wp, lp);
}
static BOOL(WINAPI *r_DestroyWindow)(HWND);
static BOOL WINAPI DestroyWindow_(HWND w) {
    char title[64] = "";
    GetWindowTextA(w, title, sizeof title);
    pc64_log("win: DestroyWindow(%p \"%s\")", (void *)w, title);
    caller();
    return r_DestroyWindow(w);
}
static BOOL(WINAPI *r_PeekMessageA)(LPMSG, HWND, UINT, UINT, UINT);
static BOOL WINAPI PeekMessageA_(LPMSG m, HWND w, UINT fmin, UINT fmax, UINT rem) {
    const BOOL r = r_PeekMessageA(m, w, fmin, fmax, rem);
    if (r && m && m->message == WM_QUIT) pc64_log("win: PeekMessage -> WM_QUIT wParam %llx", (unsigned long long)m->wParam);
    return r;
}

// the game's window procedures, wrapped to log the messages that matter
static struct { char cls[64]; WNDPROC orig; } g_classes[32];
static int g_ncls;
static LRESULT CALLBACK wndproc_(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    char cls[64] = "";
    GetClassNameA(w, cls, sizeof cls);
    WNDPROC orig = NULL;
    for (int i = 0; i < g_ncls; i++)
        if (!strcmp(g_classes[i].cls, cls)) orig = g_classes[i].orig;
    if (!orig) orig = DefWindowProcA;
    if (msg == WM_CLOSE || msg == WM_DESTROY || msg == WM_QUIT || msg == WM_ACTIVATEAPP || msg == WM_KILLFOCUS) {
        char title[64] = "";
        GetWindowTextA(w, title, sizeof title);
        pc64_log("win: msg %04x on window %p of class \"%s\" (\"%s\") wParam %llx lParam %llx", msg, (void *)w, cls, title,
                 (unsigned long long)wp, (unsigned long long)lp);
    }
    return CallWindowProcA(orig, w, msg, wp, lp);
}
static ATOM(WINAPI *r_RegisterClassExA)(const WNDCLASSEXA *);
static ATOM WINAPI RegisterClassExA_(const WNDCLASSEXA *wc) {
    if (wc && wc->lpfnWndProc && wc->lpszClassName && g_ncls < (int)(sizeof g_classes / sizeof g_classes[0])) {
        WNDCLASSEXA copy = *wc;
        snprintf(g_classes[g_ncls].cls, sizeof g_classes[0].cls, "%s", wc->lpszClassName);
        g_classes[g_ncls].orig = wc->lpfnWndProc;
        g_ncls++;
        copy.lpfnWndProc = wndproc_;
        pc64_log("win: class \"%s\" registered; its window proc wrapped for the message log", wc->lpszClassName);
        return r_RegisterClassExA(&copy);
    }
    return r_RegisterClassExA(wc);
}
static ATOM(WINAPI *r_RegisterClassA)(const WNDCLASSA *);
static ATOM WINAPI RegisterClassA_(const WNDCLASSA *wc) {
    if (wc && wc->lpfnWndProc && wc->lpszClassName && g_ncls < (int)(sizeof g_classes / sizeof g_classes[0])) {
        WNDCLASSA copy = *wc;
        snprintf(g_classes[g_ncls].cls, sizeof g_classes[0].cls, "%s", wc->lpszClassName);
        g_classes[g_ncls].orig = wc->lpfnWndProc;
        g_ncls++;
        copy.lpfnWndProc = wndproc_;
        return r_RegisterClassA(&copy);
    }
    return r_RegisterClassA(wc);
}

// --- HK_PC64_THREAD_LOG: the threads started and ended
typedef DWORD(WINAPI *thread_fn)(void *);
struct tramp { thread_fn fn; void *arg; };
static DWORD WINAPI thread_tramp_(void *p) {
    struct tramp *t = p;
    const DWORD id = GetCurrentThreadId();
    pc64_log("thread: started, id %lu, routine:", (unsigned long)id);
    where("  ", t->fn);
    const DWORD r = t->fn(t->arg);
    pc64_log("thread: ended, id %lu, exit code %lu", (unsigned long)id, (unsigned long)r);
    free(t);
    return r;
}
static HANDLE(WINAPI *r_CreateThread)(LPSECURITY_ATTRIBUTES, SIZE_T, thread_fn, void *, DWORD, LPDWORD);
static HANDLE WINAPI CreateThread_(LPSECURITY_ATTRIBUTES sa, SIZE_T stack, thread_fn fn, void *arg, DWORD flags, LPDWORD id) {
    if (fn) {
        struct tramp *t = malloc(sizeof *t);
        t->fn = fn, t->arg = arg;
        return r_CreateThread(sa, stack, thread_tramp_, t, flags, id);
    }
    return r_CreateThread(sa, stack, fn, arg, flags, id);
}

// --- HK_PC64_DISP_LOG: the monitors and display devices as the game enumerates them
static BOOL(WINAPI *r_EnumDisplayDevicesA)(LPCSTR, DWORD, DISPLAY_DEVICEA *, DWORD);
static BOOL WINAPI EnumDisplayDevicesA_(LPCSTR dev, DWORD i, DISPLAY_DEVICEA *dd, DWORD flags) {
    const BOOL r = r_EnumDisplayDevicesA(dev, i, dd, flags);
    if (r) pc64_log("disp: display device %lu \"%s\" \"%s\" flags %lx", (unsigned long)i, dd->DeviceName, dd->DeviceString,
                    (unsigned long)dd->StateFlags);
    else pc64_log("disp: display device %lu of \"%s\": none", (unsigned long)i, dev ? dev : "(all)");
    return r;
}
static MONITORENUMPROC g_mon_cb;
static BOOL CALLBACK mon_cb_(HMONITOR m, HDC dc, LPRECT r, LPARAM lp) {
    MONITORINFO mi;
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    if (GetMonitorInfoA(m, &mi))
        pc64_log("disp: monitor %ld,%ld %ldx%ld%s", mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top, (mi.dwFlags & MONITORINFOF_PRIMARY) ? " (primary)" : "");
    return g_mon_cb ? g_mon_cb(m, dc, r, lp) : TRUE;
}
static BOOL(WINAPI *r_EnumDisplayMonitors)(HDC, LPCRECT, MONITORENUMPROC, LPARAM);
static BOOL WINAPI EnumDisplayMonitors_(HDC dc, LPCRECT clip, MONITORENUMPROC cb, LPARAM lp) {
    if (cb) {
        g_mon_cb = cb;
        const BOOL r = r_EnumDisplayMonitors(dc, clip, mon_cb_, lp);
        g_mon_cb = NULL;
        return r;
    }
    return r_EnumDisplayMonitors(dc, clip, cb, lp);
}
static LONG(WINAPI *r_EnumDisplaySettingsA)(LPCSTR, DWORD, DEVMODEA *);
static LONG WINAPI EnumDisplaySettingsA_(LPCSTR dev, DWORD mode, DEVMODEA *dm) {
    const LONG r = r_EnumDisplaySettingsA(dev, mode, dm);
    if (r && g_disp)
        pc64_log("disp: mode %lu of \"%s\": %ux%u at %u Hz, %u bpp", (unsigned long)mode, dev ? dev : "(primary)",
                 (unsigned)dm->dmPelsWidth, (unsigned)dm->dmPelsHeight, (unsigned)dm->dmDisplayFrequency, (unsigned)dm->dmBitsPerPel);
    return r;
}

// --- HK_PC64_NAME_LOG: the string compare's calls, by call site, with a sample of what they compare
typedef int (*cmp_t)(const char *, const char *);
static cmp_t r_cmp;
#define NL_SITES 256
#define NL_PAIRS 512
static struct { const void *at; unsigned long long n; } g_nl_site[NL_SITES];
static struct { const void *at; char a[48], b[48]; unsigned n; unsigned h; } g_nl_pair[NL_PAIRS];
static unsigned long long g_nl_calls, g_nl_from;
static ULONGLONG g_nl_t0;
static unsigned nl_hash(const char *s, unsigned h) {
    for (int i = 0; s && s[i] && i < 47; i++) h = h * 31 + (unsigned char)s[i];
    return h;
}
static void nl_report(ULONGLONG now) {
    const double sec = (now - g_nl_t0) / 1000.0;
    pc64_log("names: %.0f compares a second", (g_nl_calls - g_nl_from) / sec);
    for (int k = 0; k < 10; k++) {   // the busiest sites, taken out one by one
        int best = -1;
        for (int i = 0; i < NL_SITES; i++)
            if (g_nl_site[i].n && (best < 0 || g_nl_site[i].n > g_nl_site[best].n)) best = i;
        if (best < 0) break;
        char lead[64];
        snprintf(lead, sizeof lead, "names:   %.0f/s at ", g_nl_site[best].n / sec);
        where(lead, g_nl_site[best].at);
        g_nl_site[best].n = 0;
    }
    for (int k = 0; k < 16; k++) {   // ... and the strings most often compared (1 call in 64 kept)
        int best = -1;
        for (int i = 0; i < NL_PAIRS; i++)
            if (g_nl_pair[i].n && (best < 0 || g_nl_pair[i].n > g_nl_pair[best].n)) best = i;
        if (best < 0) break;
        char lead[200];
        snprintf(lead, sizeof lead, "names:   ~%.0f/s \"%s\" vs \"%s\" at ", 64.0 * g_nl_pair[best].n / sec, g_nl_pair[best].a,
                 g_nl_pair[best].b);
        where(lead, g_nl_pair[best].at);
        g_nl_pair[best].n = 0;
    }
    memset(g_nl_site, 0, sizeof g_nl_site), memset(g_nl_pair, 0, sizeof g_nl_pair);
    g_nl_from = g_nl_calls, g_nl_t0 = now;
}
static int cmp_(const char *a, const char *b) {
    const void *at = __builtin_return_address(0);
    const unsigned long long n = ++g_nl_calls;
    unsigned i = (unsigned)((uintptr_t)at >> 1) % NL_SITES;
    for (unsigned k = 0; k < NL_SITES; k++, i = (i + 1) % NL_SITES)
        if (!g_nl_site[i].at || g_nl_site[i].at == at) {
            g_nl_site[i].at = at, g_nl_site[i].n++;
            break;
        }
    if ((n & 63) == 0 && a && b) {
        const unsigned h = nl_hash(b, nl_hash(a, (unsigned)(uintptr_t)at));
        unsigned j = h % NL_PAIRS;
        for (unsigned k = 0; k < 16; k++, j = (j + 1) % NL_PAIRS) {
            if (!g_nl_pair[j].n) {
                g_nl_pair[j].at = at, g_nl_pair[j].h = h, g_nl_pair[j].n = 1;
                snprintf(g_nl_pair[j].a, sizeof g_nl_pair[j].a, "%s", a);
                snprintf(g_nl_pair[j].b, sizeof g_nl_pair[j].b, "%s", b);
                break;
            }
            if (g_nl_pair[j].h == h) {
                g_nl_pair[j].n++;
                break;
            }
        }
    }
    if ((n & 0xffff) == 0) {
        const ULONGLONG now = GetTickCount64();
        if (!g_nl_t0) g_nl_t0 = now;
        else if (now - g_nl_t0 >= 5000) nl_report(now);
    }
    return r_cmp(a, b);
}

// --- HK_PC64_PROFILE=FILE:HZ:LO:HI — the main thread sampled HZ times a second of wall time while
// the main screen's picture count is LO..HI: each sample is the thread stopped, its call chain
// walked by the modules' own unwind tables (no locks taken while it is stopped: the tables are
// found once, before sampling), and written as "f<picture> <address> <caller> <caller's caller>..."
// (hexadecimal, absolute), then the loaded modules as "m <base> <size> <path>". A wall-time
// profile: a sample in a wait shows the wait and who waits.
#include <tlhelp32.h>
#define PF_MODS 160
#define PF_DEPTH 24
static struct { uintptr_t base, end; const RUNTIME_FUNCTION *fn; DWORD n; } g_pf_mod[PF_MODS];
static int g_pf_nmod;
static HANDLE g_pf_main;
static char g_pf_path[MAX_PATH];
static unsigned g_pf_hz = 500, g_pf_lo, g_pf_hi = 0xffffffffu;

static void pf_modules(FILE *out) {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    MODULEENTRY32 me;
    me.dwSize = sizeof me;
    g_pf_nmod = 0;
    for (BOOL ok = s != INVALID_HANDLE_VALUE && Module32First(s, &me); ok; ok = Module32Next(s, &me)) {
        const uint8_t *b = me.modBaseAddr;
        if (out) fprintf(out, "m %llx %lx %s\n", (unsigned long long)(uintptr_t)b, (unsigned long)me.modBaseSize, me.szExePath);
        const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(b + ((const IMAGE_DOS_HEADER *)b)->e_lfanew);
        const IMAGE_DATA_DIRECTORY *d = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        if (g_pf_nmod < PF_MODS && d->VirtualAddress && d->Size) {
            g_pf_mod[g_pf_nmod].base = (uintptr_t)b;
            g_pf_mod[g_pf_nmod].end = (uintptr_t)b + me.modBaseSize;
            g_pf_mod[g_pf_nmod].fn = (const RUNTIME_FUNCTION *)(b + d->VirtualAddress);
            g_pf_mod[g_pf_nmod].n = d->Size / sizeof(RUNTIME_FUNCTION);
            g_pf_nmod++;
        }
    }
    if (s != INVALID_HANDLE_VALUE) CloseHandle(s);
}
static const RUNTIME_FUNCTION *pf_lookup(uintptr_t pc, uintptr_t *base) {
    for (int i = 0; i < g_pf_nmod; i++) {
        if (pc < g_pf_mod[i].base || pc >= g_pf_mod[i].end) continue;
        const DWORD rva = (DWORD)(pc - g_pf_mod[i].base);
        DWORD lo = 0, hi = g_pf_mod[i].n;
        *base = g_pf_mod[i].base;
        while (lo < hi) {
            const DWORD mid = (lo + hi) / 2;
            const RUNTIME_FUNCTION *f = &g_pf_mod[i].fn[mid];
            if (rva < f->BeginAddress) hi = mid;
            else if (rva >= f->EndAddress) lo = mid + 1;
            else return f;
        }
        return NULL;
    }
    *base = 0;
    return NULL;
}
static int pf_in_module(uintptr_t pc) {
    for (int i = 0; i < g_pf_nmod; i++)
        if (pc >= g_pf_mod[i].base && pc < g_pf_mod[i].end) return 1;
    return 0;
}
static DWORD WINAPI pf_thread(void *arg) {
    (void)arg;
    while (pc64_frames < g_pf_lo) Sleep(50);
    FILE *out = fopen(g_pf_path, "w");
    if (!out) { pc64_log("inspect: HK_PC64_PROFILE: can't write %s", g_pf_path); return 0; }
    pf_modules(out);
    pc64_log("inspect: HK_PC64_PROFILE: sampling from picture %u (%d modules with unwind tables)", pc64_frames, g_pf_nmod);
    unsigned long long n = 0;
    const DWORD gap = g_pf_hz >= 1000 ? 1 : 1000 / g_pf_hz;   // (Wine's Sleep keeps to the millisecond)
    while (pc64_frames < g_pf_hi) {
        Sleep(gap);
        uintptr_t chain[PF_DEPTH];
        int depth = 0;
        const unsigned f = pc64_frames;
        if (SuspendThread(g_pf_main) == (DWORD)-1) break;
        CONTEXT c;
        memset(&c, 0, sizeof c);
        c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        if (GetThreadContext(g_pf_main, &c)) {
            for (; depth < PF_DEPTH && c.Rip; depth++) {
                chain[depth] = (uintptr_t)c.Rip;
                uintptr_t base;
                const RUNTIME_FUNCTION *fe = pf_lookup((uintptr_t)c.Rip, &base);
                if (fe) {
                    void *hd;
                    ULONG64 est;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, (PRUNTIME_FUNCTION)fe, &c, &hd, &est, NULL);
                } else if (depth == 0 || pf_in_module((uintptr_t)c.Rip)) {   // a leaf without a table entry
                    c.Rip = *(const ULONG64 *)c.Rsp;
                    c.Rsp += 8;
                } else
                    break;   // outside every module: the walk can't go on
                if (c.Rsp & 7) break;
            }
        }
        ResumeThread(g_pf_main);
        fprintf(out, "f%u", f);
        for (int i = 0; i < depth; i++) fprintf(out, " %llx", (unsigned long long)chain[i]);
        fputc('\n', out);
        n++;
    }
    pf_modules(out);
    fclose(out);
    pc64_log("inspect: HK_PC64_PROFILE: %llu samples to picture %u", n, pc64_frames);
    return 0;
}
static void pf_install(void) {
    const char *s = getenv("HK_PC64_PROFILE");
    if (!s || strlen(s) < 3) return;
    const char *c1 = strchr(s + 2, ':');   // (a drive letter's colon is not a separator)
    snprintf(g_pf_path, sizeof g_pf_path, "%.*s", c1 ? (int)(c1 - s) : (int)strlen(s), s);
    if (c1) {
        g_pf_hz = (unsigned)atoi(c1 + 1);
        const char *c2 = strchr(c1 + 1, ':');
        if (c2) {
            g_pf_lo = (unsigned)atol(c2 + 1);
            const char *c3 = strchr(c2 + 1, ':');
            if (c3) g_pf_hi = (unsigned)atol(c3 + 1);
        }
    }
    if (!g_pf_hz) g_pf_hz = 500;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_pf_main,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    CloseHandle(CreateThread(NULL, 0, pf_thread, NULL, 0, NULL));
    pc64_log("inspect: HK_PC64_PROFILE on: %s, %u a second, pictures %u..%u", g_pf_path, g_pf_hz, g_pf_lo, g_pf_hi);
}

void pc64_inspect_install(HMODULE game) {
    pf_install();
    g_win = on("HK_PC64_WIN_LOG");
    g_thread = on("HK_PC64_THREAD_LOG");
    g_disp = on("HK_PC64_DISP_LOG");
    const int g_frame = on("HK_PC64_FRAME_LOG");   // only ExitProcess is hooked for it (link.c's exit table)
    if (!g_win && !g_thread && !g_disp && !g_frame && !on("HK_PC64_NAME_LOG")) return;
    HMODULE avs = GetModuleHandleA("avs2-core.dll");
    HMODULE mods[2] = { game, avs };
    for (int i = 0; i < 2; i++) {
        HMODULE m = mods[i];
        if (!m) continue;
        if (g_win) {
            HOOKQ(m, "user32.dll", "PostQuitMessage", PostQuitMessage_, &r_PostQuitMessage);
            HOOKQ(m, "user32.dll", "PostThreadMessageA", PostThreadMessageA_, &r_PostThreadMessageA);
            HOOKQ(m, "user32.dll", "DestroyWindow", DestroyWindow_, &r_DestroyWindow);
            HOOKQ(m, "user32.dll", "PeekMessageA", PeekMessageA_, &r_PeekMessageA);
            HOOKQ(m, "user32.dll", "RegisterClassExA", RegisterClassExA_, &r_RegisterClassExA);
            HOOKQ(m, "user32.dll", "RegisterClassA", RegisterClassA_, &r_RegisterClassA);
            HOOKQ(m, "kernel32.dll", "ExitProcess", ExitProcess_, &r_ExitProcess);
            HOOKQ(m, "kernel32.dll", "FreeLibraryAndExitThread", FreeLibraryAndExitThread_, &r_FreeLibraryAndExitThread);
        }
        if (g_frame)
            HOOKQ(m, "kernel32.dll", "ExitProcess", ExitProcess_, &r_ExitProcess);
        if (g_thread) {
            HOOKQ(m, "kernel32.dll", "CreateThread", CreateThread_, &r_CreateThread);
            HOOKQ(m, "kernel32.dll", "ExitThread", ExitThread_, &r_ExitThread);
        }
        if (g_disp) {
            HOOKQ(m, "user32.dll", "EnumDisplayDevicesA", EnumDisplayDevicesA_, &r_EnumDisplayDevicesA);
            HOOKQ(m, "user32.dll", "EnumDisplayMonitors", EnumDisplayMonitors_, &r_EnumDisplayMonitors);
            HOOKQ(m, "user32.dll", "EnumDisplaySettingsA", EnumDisplaySettingsA_, &r_EnumDisplaySettingsA);
        }
    }
    if (on("HK_PC64_NAME_LOG")) {   // (its own: the modules that import the compare)
        const char *names[] = { NULL, "afp-core.dll", "afp-utils.dll", "afp-utils2.dll" };
        int n = 0;
        for (int i = 0; i < 4; i++) {
            HMODULE m = i ? GetModuleHandleA(names[i]) : game;
            void *o = m ? pc64_import_ordinal(m, "avs2-core.dll", 229, (void *)cmp_) : NULL;
            if (o) r_cmp = (cmp_t)o, n++;
        }
        pc64_log("inspect: HK_PC64_NAME_LOG on (%d modules' string compares)", n);
    }
    if (g_win) pc64_log("inspect: HK_PC64_WIN_LOG on");
    if (g_thread) pc64_log("inspect: HK_PC64_THREAD_LOG on");
    if (g_disp) pc64_log("inspect: HK_PC64_DISP_LOG on (the Direct3D side in display.c)");
}
