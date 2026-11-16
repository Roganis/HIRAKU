// SPDX-License-Identifier: GPL-3.0-or-later
// The game menu's text in the launcher's fonts (runtime/menu/font.h): hiraku-menu draws its own
// picture and has no font engine, so the launcher draws each character the menu will need, in
// three sizes, with the fonts this PC has (any alphabet they cover, a title in Japanese too), and
// hands them over as a file.
#pragma once

#include <QByteArray>
#include <QRectF>

#include <functional>

class QPainter;
#include <QFont>
#include <QImage>
#include <QString>

namespace menufont {

// The sizes' heights in pixels: small (a game's year and system), a list's row, a heading
constexpr int kPixels[3] = {14, 20, 31};

// The sizes a file is drawn at, and their weights
struct Sizes {
    int pixels[3];
    QFont::Weight weight[3];
};
constexpr Sizes kMenu{{kPixels[0], kPixels[1], kPixels[2]}, {QFont::Normal, QFont::DemiBold, QFont::Bold}};
// a game's boxes (runtime/host/uibox.h): a line, a title, a heading at a 360-line screen's size,
// times its scale
constexpr Sizes kBoxes{{10, 13, 18}, {QFont::Normal, QFont::Bold, QFont::Black}};
Sizes times(const Sizes &s, double scale);

// A character the caller draws itself instead of the font (an icon), filling `box`, in white
using Painter = std::function<void(QPainter &p, const QRectF &box, char32_t c)>;

// The file's bytes: ASCII, and every other character of `text`, in each size of `base`'s family;
// and `drawn`, by `paint`, each as wide as its size's line is high. Needs a QGuiApplication.
QByteArray build(const QFont &base, const QString &text, const Sizes &sizes = kMenu, const QList<char32_t> &drawn = {},
                 const Painter &paint = {});

// HIRAKU's name for the menu's top (--logo): the launcher's picture of it (white letters on black)
// without the black around the letters, kLogo pixels high
constexpr int kLogo = 30;
QImage logo(const QImage &picture, int height = kLogo);

}  // namespace menufont
