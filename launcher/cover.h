// SPDX-License-Identifier: GPL-3.0-or-later
// The black cover's place among the screen's windows: gamemenu::CoverPlan says where it goes,
// MainWindow::cover puts each screen's there by this. Apart from mainwindow.cpp for Windows' calls.
#pragma once

#include <QString>
#include <QtGlobal>

#include "gamemenu.h"

class QRect;
class QWidget;

namespace cover {

// Puts `w` (a top-level window, black, one screen's size; made by make()) where `at` says, never
// taking the keyboard from the program running:
// - Top: shown above every window, the taskbar too.
// - Under: shown right under the windows of process `pid`. Windows: placed after its lowest window
//   on the cover's screen (else its lowest anywhere) before it is shown; X11: kept below the others. Wayland, where a program
//   can't say where its window goes: not shown (it could end up over the program).
// - Down: hidden.
// false: Under, and it couldn't go there yet (Windows: `pid` has no window, it has ended or has none
// yet, or not one the size of the screen while `whole`): the cover is left as it was
// `whole`: Under only once the program's window on the cover's screen covers all of it (Windows: a
// window opening full screen is first smaller, and the taskbar comes up over it if the black has
// gone under it by then); false: once there is one at all
bool place(QWidget *w, gamemenu::Cover at, qint64 pid, bool whole = false);
// A cover for the screen at `geometry` (black, no frame, no pointer)
QWidget *make(const QRect &geometry);
QString note();   // (HK_COVER_LOG: what Windows answered, since the last call)

}  // namespace cover
