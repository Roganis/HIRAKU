// SPDX-License-Identifier: GPL-3.0-or-later
// The game menu's text in the launcher's fonts (runtime/menu/font.h): hiraku-menu draws its own
// picture and has no font engine, so the launcher draws each character the menu will need, in
// three sizes, with the fonts this PC has (any alphabet they cover, a title in Japanese too), and
// hands them over as a file.
#pragma once

#include <QByteArray>
#include <QFont>
#include <QImage>
#include <QString>

namespace menufont {

// The sizes' heights in pixels: small (a game's year and system), a list's row, a heading
constexpr int kPixels[3] = {14, 20, 31};

// The file's bytes: ASCII, and every other character of `text`, in each size of `base`'s family.
// Needs a QGuiApplication.
QByteArray build(const QFont &base, const QString &text);

// HIRAKU's name for the menu's top (--logo): the launcher's picture of it (white letters on black)
// without the black around the letters, kLogo pixels high
constexpr int kLogo = 30;
QImage logo(const QImage &picture);

}  // namespace menufont
