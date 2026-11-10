// SPDX-License-Identifier: GPL-3.0-or-later
// What a problem report (report.h) is cleaned of before the person sees it: where their files are,
// who they are, their cards, their network. Text in, text out (Qt's core only), so it is tested on
// its own: tests/launcher/test_cleaner.cpp.
#pragma once

#include <QMap>
#include <QString>
#include <QStringList>

namespace report {

// What a report replaces, and the cards met so far (their numbers in a report, in turn)
struct Cleaner {
    QStringList folders;          // HIRAKU's own: <hiraku>
    QStringList homes;            // the home folder, as each tool writes it
    QStringList names;            // the user's and the computer's names, in that order
    QMap<QString, int> cards;
    QString clean(const QString &text);
    static Cleaner forThisComputer();
};

}  // namespace report
