// SPDX-License-Identifier: GPL-3.0-or-later
// The launcher's looks: the system's own (as before), or "Pop", HIRAKU's own colours - six bright
// ones (blue, green, yellow, orange, red, purple) on warm paper, or on a night-blue ground ("Pop
// night"). View > Look picks one; it's kept in ui/theme (HK_THEME=system|pop|night for a try).
//
// Pop is the Fusion style, a palette and a style sheet (theme.cpp); what the launcher paints itself
// (the HIRAKU word in the toolbar, the games' badges, the music's meters, the statuses) asks here.
#pragma once

#include <QColor>
#include <QIcon>
#include <QPixmap>
#include <QRectF>
#include <QString>

class QPainter;

namespace theme {

enum Look { System, Pop, Night };

Look current();
void apply(Look look);        // the whole application, at once (dialogs open later follow it)
void applySaved();            // at start-up: ui/theme, or HK_THEME
QString key(Look look);       // "system", "pop", "night" (ui/theme)
bool pop();                   // Pop or Pop night

QColor rainbow(int k);        // the six colours, k = 0..5 (blue .. purple)
QColor along(double t);       // between them, t = 0..1 (the music's meters)
QColor ready();               // a game's status: ready to play
QColor notable();             // ... ready, with something to know (first boot)
QColor badge(const QString &family, QColor fallback);   // a series' badge colour, in Pop

// a plain sign, drawn in one colour: "play", "pause", "stop", "prev", "next", "speaker"
QIcon glyph(const QString &what, QColor colour);

// HIRAKU in big letters, each its own colour, on a slanted bar: the toolbar's logo in Pop
QPixmap wordmark(int height, qreal dpr);

// an icon's 24x24 path (third_party/mdi/icons.h: absolute M, L, C, Z) filled into box
void drawIcon(QPainter *p, const char *path, const QRectF &box, QColor colour);

// An icon from its MDI path (third_party/mdi/icons.h) in this look: in the text's colour, or, with
// `tile` 0..5 in Pop, white on a rounded square of that one of the six (the toolbar's own); a
// paler one when disabled. The desktop's icon theme isn't asked: every look has the same icons.
QIcon icon(const char *path, int tile = -1);
// ... kept on an action or a button (a QAction or QAbstractButton), and made again by restyle for
// the look it is then
void mark(QObject *o, const char *path, int tile = -1);
void restyle(QObject *root);   // every one marked under root (and root)

// a style sheet for the check boxes under the widget it's given to: each a switch, on in this
// colour (the settings' pages, launcher/settingsdialog.cpp)
QString switches(QColor on, QColor off);

// a colour, `amount` of the way to another (0: the first, 1: the other)
QColor mix(QColor a, QColor b, double amount);
QColor onYellow();
// a series' colour where it is seen against `ground`: one too close to it (IIDX's near-black on a
// dark list) made lighter or darker until it shows
QColor against(QColor c, QColor ground);
// the game menu's look (runtime/menu/menu.h, --look): this look's colours, as "name=#rrggbb,..."
QString menuLook();            // what is drawn on the yellow in Pop (a light glyph there is lost)

}  // namespace theme
