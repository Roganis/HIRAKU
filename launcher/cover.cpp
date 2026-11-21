// SPDX-License-Identifier: GPL-3.0-or-later
#include "cover.h"

#include <QGuiApplication>
#include <QVariant>

#include <utility>
#include <QWidget>
#include <QWindow>

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shobjidl.h>
#endif

namespace cover {

static QString g_note;
QString note() { return std::exchange(g_note, QString()); }

QWidget *make(const QRect &geometry) {
    auto *w = new QWidget(nullptr, Qt::Window | Qt::FramelessWindowHint);
    w->setWindowTitle("HIRAKU");
    w->setStyleSheet("background: black");
    w->setAutoFillBackground(true);
    w->setCursor(Qt::BlankCursor);
    w->setAttribute(Qt::WA_ShowWithoutActivating);   // (the keyboard stays the running program's)
    w->setGeometry(geometry);
    return w;
}

#ifdef Q_OS_WIN
namespace {

struct Find {
    DWORD pid;
    HWND self;
    HMONITOR monitor;   // the cover's screen
    HWND lowest = nullptr, lowestHere = nullptr;
};

// The process's lowest visible window, and its lowest on the cover's screen: the cover goes under
// all of the program's windows there (a second screen's own window, the 64-bit PC platform's sub
// screen, isn't under another screen's cover). EnumWindows goes from the top of the order down
BOOL CALLBACK lowest(HWND h, LPARAM p) {
    auto *f = reinterpret_cast<Find *>(p);
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    RECT r;
    if (h == f->self || pid != f->pid || !IsWindowVisible(h) || !GetWindowRect(h, &r) || r.right <= r.left || r.bottom <= r.top) return TRUE;
    f->lowest = h;
    if (MonitorFromWindow(h, MONITOR_DEFAULTTONULL) == f->monitor) f->lowestHere = h;
    return TRUE;
}

constexpr UINT kKeep = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER;

// No fading in (Windows' own animation of a window being shown: a moment of the desktop through it)
void noFade(HWND h) {
    using Set = HRESULT(WINAPI *)(HWND, DWORD, LPCVOID, DWORD);
    static const Set set = [] {
        HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        return dwm ? reinterpret_cast<Set>(reinterpret_cast<void *>(GetProcAddress(dwm, "DwmSetWindowAttribute"))) : nullptr;
    }();
    const BOOL off = TRUE;
    if (set) set(h, 3 /* DWMWA_TRANSITIONS_FORCEDISABLED */, &off, sizeof off);
}

// Full screen as the taskbar sees it: it goes behind the cover while the cover is the foreground
// window (by itself it takes a frameless window the size of the screen for an ordinary one and
// stays over it)
void fullScreen(HWND h) {
    static const CLSID taskbarList = {0x56fdf344, 0xfd6d, 0x11d0, {0x95, 0x8a, 0x00, 0x60, 0x97, 0xc9, 0xa0, 0x90}};
    static const IID taskbarList2 = {0x602d4995, 0xb13a, 0x429b, {0xa6, 0x6e, 0x19, 0x35, 0xe4, 0x4f, 0x43, 0x17}};
    ITaskbarList2 *t = nullptr;
    if (FAILED(CoCreateInstance(taskbarList, nullptr, CLSCTX_INPROC_SERVER, taskbarList2, reinterpret_cast<void **>(&t))) || !t) return;
    if (SUCCEEDED(t->HrInit())) t->MarkFullscreenWindow(h, TRUE);
    t->Release();
}

// Its place in the windows' order (after `after`), over the whole of its screen: Qt keeps a window
// that isn't in its full-screen state off the taskbar's part of the screen
void put(HWND self, HWND after) {
    MONITORINFO m{};
    m.cbSize = sizeof m;
    GetMonitorInfoW(MonitorFromWindow(self, MONITOR_DEFAULTTONEAREST), &m);
    const RECT &r = m.rcMonitor;
    SetWindowPos(self, after, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

}  // namespace

bool place(QWidget *w, gamemenu::Cover at, qint64 pid, bool whole) {
    using gamemenu::Cover;
    if (at == Cover::Down) {
        w->hide();
        return true;
    }
    // (not Qt's full screen: Qt puts a window going full screen at the top of the order)
    HWND self = reinterpret_cast<HWND>(w->winId());
    if (!w->property("hk_cover_marked").toBool()) {   // (once a window)
        noFade(self);
        fullScreen(self);
        w->setProperty("hk_cover_marked", true);
    }
    HWND after = HWND_TOPMOST;
    if (at == Cover::Under) {
        Find f{(DWORD)pid, self, MonitorFromWindow(self, MONITOR_DEFAULTTONEAREST)};
        if (pid > 0) EnumWindows(lowest, reinterpret_cast<LPARAM>(&f));
        if (!f.lowest) return false;   // (no window: it has ended, or has none yet. The black stays as it is)
        if (whole && f.lowestHere) {   // (its window on this screen not yet the whole of it: not yet)
            MONITORINFO m{};
            m.cbSize = sizeof m;
            GetMonitorInfoW(f.monitor, &m);
            bool covers = false;   // (any of its windows from there up)
            for (HWND h = f.lowestHere; h && !covers; h = GetWindow(h, GW_HWNDPREV)) {
                DWORD p = 0;
                RECT r;
                GetWindowThreadProcessId(h, &p);
                covers = p == f.pid && IsWindowVisible(h) && GetWindowRect(h, &r) && EqualRect(&r, &m.rcMonitor);
            }
            if (!covers) return false;
        }
        if (GetWindowLongW(self, GWL_EXSTYLE) & WS_EX_TOPMOST) SetWindowPos(self, HWND_NOTOPMOST, 0, 0, 0, 0, kKeep);
        after = f.lowestHere ? f.lowestHere : f.lowest;
    }
    // in its place first, then shown (and there again, should showing it have moved it): hidden or
    // shown, it is never under the taskbar as it appears, nor over the program it goes under
    put(self, after);
    if (!w->isVisible()) {
        w->show();
        put(self, after);
        // black at once, into the window as Windows keeps it, even where it is hidden under the
        // program: Qt paints a window only once it is seen, and until then Windows shows through it
        // what was there before (the program's window going, its fading out)
        if (HDC dc = GetDC(self)) {
            RECT r;
            GetClientRect(self, &r);
            FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            ReleaseDC(self, dc);
        }
        w->repaint();
    }
    if (at == Cover::Top) {
        // ... and the foreground window, or the taskbar comes up over it (the program leaving hands
        // it over: runtime/host/menukey.h). The next program takes it from the cover
        const BOOL fg = SetForegroundWindow(self);
        RECT r;
        GetWindowRect(self, &r);
        g_note += QString(" fg=%1 rect=%2,%3,%4,%5").arg(fg).arg(r.left).arg(r.top).arg(r.right).arg(r.bottom);
    }
    return true;
}
#else
bool place(QWidget *w, gamemenu::Cover at, qint64 pid, bool whole) {
    (void)pid, (void)whole;
    using gamemenu::Cover;
    if (at == Cover::Down || (at == Cover::Under && QGuiApplication::platformName() == "wayland")) {
        w->hide();   // (Wayland: that is where it goes, nothing to try again)
        return true;
    }
    const bool top = at == Cover::Top;
    if (w->isVisible() && w->windowHandle()) {   // (as it is: Qt's own change of flags would hide it first)
        QWindow *h = w->windowHandle();
        h->setFlag(Qt::WindowStaysOnBottomHint, !top);
        h->setFlag(Qt::WindowStaysOnTopHint, top);
        if (top) h->raise();
        return true;
    }
    w->setWindowFlag(Qt::WindowStaysOnBottomHint, !top);
    w->setWindowFlag(Qt::WindowStaysOnTopHint, top);
    w->showFullScreen();
    w->repaint();
    return true;
}
#endif

}  // namespace cover
