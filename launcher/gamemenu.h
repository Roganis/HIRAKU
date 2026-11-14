// SPDX-License-Identifier: GPL-3.0-or-later
// The launcher's side of the game menu (runtime/menu/menu.h): the list it hands hiraku-menu, and
// the choice it reads back. Qt's core only, tested on its own (tests/launcher/test_gamemenu.cpp).
#pragma once

#include <QFile>
#include <QList>
#include <QString>

namespace gamemenu {

// The exit code of a game left for the menu (runtime/host/menukey.h, EXIT_CODE)
constexpr int kExitCode = 73;

// A title as the menu's 8x8 font can show it: `latin` if the game's manifest gives one
// (title_latin: a name in another alphabet spelt out), else the title without what isn't ASCII
// ("pop'n music 16 PARTY♪" is "pop'n music 16 PARTY")
inline QString plainTitle(const QString &title, const QString &latin = QString()) {
    if (!latin.isEmpty()) return latin;
    QString out;
    for (const QChar c : title)
        if (c.unicode() >= 32 && c.unicode() < 127) out += c;
    return out.simplified();
}

struct Row {
    QString id, shelf, title, detail;   // detail: "2002  DJ Main"
    QString picture;                    // a BMP file's path, "" if the game has no picture
};

// The list file's text: the rows in the order given (a shelf's games together), and the game the
// menu opens on. Tabs and line ends can't be in a field: they'd be taken for the next one.
inline QByteArray listText(const QList<Row> &rows, const QString &current) {
    const auto field = [](QString s) { return s.replace('\t', ' ').replace('\n', ' ').replace('\r', ' '); };
    QString text = "hiraku-menu-list 1\n";
    if (!current.isEmpty()) text += "current\t" + field(current) + "\n";
    for (const Row &r : rows)
        text += "game\t" + field(r.id) + "\t" + field(r.shelf) + "\t" + field(r.title) + "\t" + field(r.detail) +
                (r.picture.isEmpty() ? QString() : "\t" + field(r.picture)) + "\n";
    return text.toUtf8();
}

// What hiraku-menu wrote: the chosen game's id, "" if it wrote none (the menu was closed)
inline QString readChoice(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll()).section('\n', 0, 0).trimmed();
}

}  // namespace gamemenu
