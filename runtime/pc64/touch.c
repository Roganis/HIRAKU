// SPDX-License-Identifier: GPL-3.0-or-later
// The touch screen (PLAN.md section 32): Wine has no touch injection, so touches are WM_TOUCH
// messages posted to the game's sub screen window, and the game's GetTouchInputInfo answers from
// here, as spice2x's wintouchemu does. Each event has its own handle and data: the game reads
// them a little later, in its frame.
// The mouse on the sub screen's window (or a touch screen, which Wine gives as a mouse) is made the
// game's touches: the window's procedure is wrapped when the game registers it for touch, and its
// left button's down, drags and up become a touch's. The game sees no mouse messages there, and
// its GetClientRect and ScreenToClient on that window answer in its 1280x800 whatever the window's
// size (it may be placed elsewhere and resized: display.c).
#include "pc64.h"

#include <string.h>
#include <windowsx.h>

#define SUB_W 1280
#define SUB_H 800

static HWND g_windows[4];   // registered for touch: "Main Screen", "Sub Screen"
static int g_nwindows;
static TOUCHINPUT g_events[64];
static unsigned g_nevents;
#define HANDLE_BASE 0x48540000u
static int ours(HTOUCHINPUT h) { return ((ULONG_PTR)h & 0xffff0000u) == HANDLE_BASE; }

static BOOL(WINAPI *r_RegisterTouchWindow)(HWND, ULONG);
static BOOL(WINAPI *r_GetTouchInputInfo)(HTOUCHINPUT, UINT, PTOUCHINPUT, int);
static BOOL(WINAPI *r_CloseTouchInputHandle)(HTOUCHINPUT);

static void sub_screen_mouse(HWND w);

static BOOL WINAPI RegisterTouchWindow_(HWND w, ULONG flags) {
    if (g_nwindows < 4) g_windows[g_nwindows++] = w;
    if (r_RegisterTouchWindow) r_RegisterTouchWindow(w, flags);
    char title[64] = "";
    GetWindowTextA(w, title, sizeof title);
    if (strstr(title, "Sub")) sub_screen_mouse(w);
    return TRUE;
}
static BOOL WINAPI GetTouchInputInfo_(HTOUCHINPUT h, UINT n, PTOUCHINPUT in, int size) {
    if (!ours(h)) return r_GetTouchInputInfo(h, n, in, size);
    if (n < 1 || size != sizeof(TOUCHINPUT)) return FALSE;
    in[0] = g_events[(ULONG_PTR)h & 63];
    return TRUE;
}
static BOOL WINAPI CloseTouchInputHandle_(HTOUCHINPUT h) {
    return ours(h) ? TRUE : r_CloseTouchInputHandle(h);
}

static HWND sub_screen(void) {
    for (int i = 0; i < g_nwindows; i++) {
        char title[64] = "";
        GetWindowTextA(g_windows[i], title, sizeof title);
        if (strstr(title, "Sub")) return g_windows[i];
    }
    return NULL;
}
// A touch at (x, y) of the window's client area as it is (TOUCHINPUT has screen coordinates, in
// hundredths of a pixel): the game's ScreenToClient gives it back in its 1280x800
static void event(HWND w, int x, int y, DWORD flags) {
    POINT p = {x, y};
    ClientToScreen(w, &p);
    const unsigned k = g_nevents++ & 63;
    TOUCHINPUT *t = &g_events[k];
    memset(t, 0, sizeof *t);
    t->x = p.x * 100, t->y = p.y * 100;
    t->dwID = 1;
    t->dwFlags = flags | TOUCHEVENTF_PRIMARY;
    t->dwTime = GetTickCount();
    PostMessageA(w, WM_TOUCH, 1, (LPARAM)(ULONG_PTR)(HANDLE_BASE | k));
}

// --- the mouse as the touch screen
static WNDPROC g_sub_proc;
static HWND g_sub;   // the sub screen's window, once the game registered it
static int g_held, g_last_x, g_last_y;
static DWORD g_last_move;

// The mouse's place in the window (lParam), kept inside it (a drag held outside)
static void game_point(HWND w, LPARAM lp, int *x, int *y) {
    RECT rc;
    GetClientRect(w, &rc);
    const int px = GET_X_LPARAM(lp), py = GET_Y_LPARAM(lp);
    *x = px < 0 ? 0 : px >= rc.right ? rc.right - 1 : px;
    *y = py < 0 ? 0 : py >= rc.bottom ? rc.bottom - 1 : py;
}

static LRESULT CALLBACK sub_proc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    int x, y;
    switch (m) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:   // a second press soon after: a touch as well
        game_point(w, lp, &x, &y);
        SetCapture(w);
        g_held = 1, g_last_x = x, g_last_y = y, g_last_move = GetTickCount();
        event(w, x, y, TOUCHEVENTF_DOWN | TOUCHEVENTF_INRANGE);
        return 0;
    case WM_MOUSEMOVE:
        if (!g_held) return 0;
        game_point(w, lp, &x, &y);
        // at most one move every 4 ms: the game reads its events once a frame, from 64 kept here
        if ((x != g_last_x || y != g_last_y) && GetTickCount() - g_last_move >= 4) {
            g_last_x = x, g_last_y = y, g_last_move = GetTickCount();
            event(w, x, y, TOUCHEVENTF_MOVE | TOUCHEVENTF_INRANGE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (g_held) {
            game_point(w, lp, &x, &y);
            g_held = 0;
            event(w, x, y, TOUCHEVENTF_UP);
            ReleaseCapture();
        }
        return 0;
    case WM_CAPTURECHANGED:   // the capture taken away while held (another window, a dialog): let go
        if (g_held && (HWND)lp != w) {
            g_held = 0;
            event(w, g_last_x, g_last_y, TOUCHEVENTF_UP);
        }
        break;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MOUSEWHEEL:
        return 0;
    }
    return CallWindowProcW(g_sub_proc, w, m, wp, lp);
}

static void sub_screen_mouse(HWND w) {
    if (g_sub_proc) return;
    g_sub = w;
    g_sub_proc = (WNDPROC)SetWindowLongPtrW(w, GWLP_WNDPROC, (LONG_PTR)sub_proc);
    RECT r = {0};
    GetWindowRect(w, &r);
    pc64_log("the sub screen's window: %ld,%ld %ldx%ld, style %08lx%s; the mouse on it touches", r.left, r.top, r.right - r.left,
             r.bottom - r.top, (unsigned long)GetWindowLongA(w, GWL_STYLE), IsWindowVisible(w) ? ", shown" : "");
}

// (x, y) of the game's 1280x800 in the window's client area as it is
static void client_point(HWND w, int x, int y, int *cx, int *cy) {
    RECT rc;
    GetClientRect(w, &rc);
    *cx = rc.right > 0 ? x * rc.right / SUB_W : x, *cy = rc.bottom > 0 ? y * rc.bottom / SUB_H : y;
}

// The game's view of the sub screen's window: its 1280x800 whatever the window's size
static BOOL(WINAPI *r_GetClientRect)(HWND, LPRECT);
static BOOL(WINAPI *r_ScreenToClient)(HWND, LPPOINT);
static BOOL WINAPI GetClientRect_(HWND w, LPRECT r) {
    if (!g_sub || w != g_sub || !r) return r_GetClientRect(w, r);
    SetRect(r, 0, 0, SUB_W, SUB_H);
    return TRUE;
}
static BOOL WINAPI ScreenToClient_(HWND w, LPPOINT p) {
    const BOOL ok = r_ScreenToClient(w, p);
    RECT rc;
    if (ok && p && g_sub && w == g_sub && GetClientRect(w, &rc) && rc.right > 0 && rc.bottom > 0)
        p->x = p->x * SUB_W / rc.right, p->y = p->y * SUB_H / rc.bottom;
    return ok;
}

// A click posted to the sub screen's window at (x, y) of its 1280x800 (tests: the mouse's way)
void pc64_click(int x, int y) {
    HWND w = sub_screen();
    if (!w) return;
    int cx, cy;
    client_point(w, x, y, &cx, &cy);
    PostMessageA(w, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(cx, cy));
    Sleep(100);
    PostMessageA(w, WM_LBUTTONUP, 0, MAKELPARAM(cx, cy));
}

void pc64_touch(int x, int y) {
    HWND w = sub_screen();
    if (!w) return;
    client_point(w, x, y, &x, &y);
    event(w, x, y, TOUCHEVENTF_DOWN | TOUCHEVENTF_INRANGE);
    for (int i = 0; i < 4; i++) {
        Sleep(33);
        event(w, x, y, TOUCHEVENTF_MOVE | TOUCHEVENTF_INRANGE);
    }
    Sleep(33);
    event(w, x, y, TOUCHEVENTF_UP);
}

// A drag on the sub screen from (x0, y0) to (x1, y1) of its 1280x800: down, moved in steps over 0.3 s,
// held still a moment (no fling), up
void pc64_drag(int x0, int y0, int x1, int y1) {
    HWND w = sub_screen();
    if (!w) return;
    int x, y;
    client_point(w, x0, y0, &x, &y);
    event(w, x, y, TOUCHEVENTF_DOWN | TOUCHEVENTF_INRANGE);
    for (int i = 1; i <= 12; i++) {
        Sleep(25);
        client_point(w, x0 + (x1 - x0) * i / 12, y0 + (y1 - y0) * i / 12, &x, &y);
        event(w, x, y, TOUCHEVENTF_MOVE | TOUCHEVENTF_INRANGE);
    }
    for (int i = 0; i < 6; i++) {
        Sleep(33);
        event(w, x, y, TOUCHEVENTF_MOVE | TOUCHEVENTF_INRANGE);
    }
    event(w, x, y, TOUCHEVENTF_UP);
}

void pc64_touch_install(HMODULE game) {
    r_RegisterTouchWindow = pc64_import(game, "user32.dll", "RegisterTouchWindow", (void *)RegisterTouchWindow_);
    r_GetTouchInputInfo = pc64_import(game, "user32.dll", "GetTouchInputInfo", (void *)GetTouchInputInfo_);
    r_CloseTouchInputHandle = pc64_import(game, "user32.dll", "CloseTouchInputHandle", (void *)CloseTouchInputHandle_);
    r_GetClientRect = pc64_import(game, "user32.dll", "GetClientRect", (void *)GetClientRect_);
    r_ScreenToClient = pc64_import(game, "user32.dll", "ScreenToClient", (void *)ScreenToClient_);
}
