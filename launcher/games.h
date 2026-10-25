// SPDX-License-Identifier: GPL-3.0-or-later
// The supported games (games/*/game.toml) and how far each is from playable.
#pragma once

#include <QIcon>
#include <QList>
#include <QString>

struct Game {
    QString id, title, family, mame;
    QString core;                        // firebeat (default), djmain, sys573
    QString year, maker;                 // from the manifest's first comment line
    QString firstBoot;                   // what a new cabinet needs in test mode
    bool installCassette = false;        // System 573: first boots with an install cassette (ddr3mj)
    bool hidden = false;                 // the AVS2 PC games ([pc] loader = "pc64", "pc32"): listed once unlocked
    QString loader;                      // [pc] loader: "pc64", "pc32", or none
    // Unavailable: a package (no source beside it) without a binary for the game: Firebeat and
    // Konami Python games in a release (PLAN.md section 28)
    enum Status { Ready, NotBuilt, NotInstalled, Unavailable } status = NotInstalled;
    QString exe;                         // hiraku-<id>: in binDir, or build/cmake; else the core's
    bool coreBinary = false;             // exe is the core's own (hiraku-sys573, ...): it takes --game <id>
    QString statusText() const;
    QString howTo() const;               // what to do to make it Ready
};

QList<Game> scanGames();
QIcon familyIcon(const QString &family);

// Where a game sits in the launcher's list: its series (pop'n music), then the system it runs on
// (DJ Main, Firebeat, Konami Python...); the orders are the series' and the systems' own, by date
struct Shelf {
    QString series, system;
    int seriesOrder = 0, systemOrder = 0;
};
Shelf shelf(const Game &g);
