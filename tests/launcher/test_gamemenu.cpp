// SPDX-License-Identifier: GPL-3.0-or-later
// The launcher's side of the game menu (launcher/gamemenu.h): the titles as the menu's font can
// show them, the list file's text, and the choice read back.
// test_gamemenu SCRATCH_DIR
#include <QDir>
#include <QFile>

#include <cstdio>

#include "gamemenu.h"

static int g_failed = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) g_failed++, fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); \
    } while (0)

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    const QString dir = QString::fromLocal8Bit(argv[1]) + "/gamemenu";
    QDir().mkpath(dir);

    CHECK(gamemenu::plainTitle("beatmania IIDX 14 GOLD") == "beatmania IIDX 14 GOLD");
    CHECK(gamemenu::plainTitle(QString::fromUtf8("pop'n music 16 PARTY\xe2\x99\xaa")) == "pop'n music 16 PARTY");
    CHECK(gamemenu::plainTitle(QString::fromUtf8("pop'n music 18 \xe3\x81\x9b\xe3\x82\x93"), "pop'n music 18 Sengoku") == "pop'n music 18 Sengoku");
    CHECK(gamemenu::plainTitle(QString::fromUtf8("A \xe2\x98\x86 B")) == "A B");   // no two spaces where a sign was

    const QList<gamemenu::Row> rows = {{"bm7thmix", "beatmania", "beatmania 7th MIX", "2002  DJ Main", ""},
                                       {"odd", "a\tshelf", "a title\nover two lines", "", ""},
                                       {"shown", "s", "t", "d", "/a folder/shown.bmp"}};
    const QByteArray text = gamemenu::listText(rows, "bm7thmix");
    CHECK(text == "hiraku-menu-list 1\ncurrent\tbm7thmix\n"
                  "game\tbm7thmix\tbeatmania\tbeatmania 7th MIX\t2002  DJ Main\n"
                  "game\todd\ta shelf\ta title over two lines\t\n"
                  "game\tshown\ts\tt\td\t/a folder/shown.bmp\n");   // a picture's path, where there is one
    CHECK(gamemenu::listText({}, "") == "hiraku-menu-list 1\n");
    // the shelves' colours, before the games
    CHECK(gamemenu::listText(rows.mid(0, 1), "", {{"beatmania", "#4a7bff"}, {"a\tb", "#000000"}}) ==
          "hiraku-menu-list 1\nshelf\ta b\t#000000\nshelf\tbeatmania\t#4a7bff\n"
          "game\tbm7thmix\tbeatmania\tbeatmania 7th MIX\t2002  DJ Main\n");

    const QString choice = dir + "/choice";
    QFile::remove(choice);
    CHECK(gamemenu::readChoice(choice).isEmpty());   // closed with none chosen: no file
    {
        QFile f(choice);
        CHECK(f.open(QIODevice::WriteOnly) && f.write("iidx16\r\n") > 0);
    }
    CHECK(gamemenu::readChoice(choice) == "iidx16");
    QFile::remove(choice);

    // The black cover between full-screen programs (gamemenu::CoverPlan, MainWindow::cover)
    using gamemenu::Cover;
    {
        gamemenu::CoverPlan p;   // a game started from the launcher's window, in full screen
        CHECK(p.started(true, false) == Cover::Down);   // no black under a game started by mouse
        CHECK(p.said("some line of the game's") == Cover::Down);
        CHECK(p.said("game menu: hold begun") == Cover::Under);   // the hold: ready under it
        CHECK(p.said("game menu: hold let go") == Cover::Down);   // ... let go: away
        CHECK(p.said("game menu: hold begun") == Cover::Under);
        CHECK(p.said("game menu: asked for from the cabinet's controls") == Cover::Top);
        CHECK(p.ended(true) == Cover::Top);                // the game gone: the menu comes
        CHECK(p.started(true, true) == Cover::Top);        // the menu started under the black
        CHECK(p.said("window: first picture") == Cover::Under);   // its picture up: under it
        CHECK(p.said("menu: chosen bm1stmix") == Cover::Top);
        CHECK(p.ended(true) == Cover::Top);
        CHECK(p.started(true, true) == Cover::Top);        // the chosen game
        CHECK(p.timedOut() == Cover::Under);               // a game that never says its picture is up
        CHECK(p.said("game menu: hold begun") == Cover::Under);
        CHECK(p.said("game menu: hold let go") == Cover::Under);   // (in the loop it stays under)
        CHECK(p.ended(true) == Cover::Top);                // ended by Escape: back to the menu, black between
        CHECK(p.started(true, true) == Cover::Top);
        CHECK(p.back() == Cover::Down);                    // the menu couldn't start / closed: the launcher's window
    }
    {
        gamemenu::CoverPlan p;   // under a program with no window yet: tried again till it is placed
        CHECK(p.started(true, true) == Cover::Top);
        p.placed(true);
        CHECK(!p.pending());                               // (on top: always placed)
        CHECK(p.timedOut() == Cover::Under);               // the fallback, the program's window not there yet
        p.placed(false);
        CHECK(p.pending());                                // ... the black stays as it was, and is tried again
        CHECK(p.said("window: first picture") == Cover::Under);
        CHECK(p.pending());                                // (the line changes nothing: still the timer's)
        p.placed(true);
        CHECK(!p.pending());                               // placed at last
        p.placed(false);
        CHECK(p.said("menu: chosen x") == Cover::Top);
        CHECK(!p.pending());                               // the plan moved on: nothing to try again
        CHECK(p.ended(true) == Cover::Top);
        CHECK(p.back() == Cover::Down);
        CHECK(!p.pending());
    }
    {
        gamemenu::CoverPlan p;   // a game in a window: no cover at all
        CHECK(p.started(false, false) == Cover::Down);
        CHECK(p.said("game menu: hold begun") == Cover::Down);
        CHECK(p.said("game menu: asked for from the cabinet's controls") == Cover::Down);
        CHECK(p.ended(true) == Cover::Down);
        CHECK(p.started(false, true) == Cover::Down);      // nor the menu in a window
    }
    {
        gamemenu::CoverPlan p;   // the menu at the launcher's start (Settings > General), full screen
        CHECK(p.started(true, true) == Cover::Top);
        CHECK(p.timedOut() == Cover::Under);               // a menu that never said it is up: the black gives way
        CHECK(p.ended(false) == Cover::Down);              // closed with nothing chosen: the launcher's window
    }
    {
        gamemenu::CoverPlan p;   // a game chosen in the menu that couldn't start
        CHECK(p.started(true, true) == Cover::Top);
        CHECK(p.said("menu: chosen x") == Cover::Top);
        CHECK(p.ended(true) == Cover::Top);
        CHECK(p.back() == Cover::Down);
        CHECK(p.started(true, false) == Cover::Down);      // the next game by mouse: no cover
    }

    if (g_failed) {
        fprintf(stderr, "%d checks failed\n", g_failed);
        return 1;
    }
    printf("ok\n");
    return 0;
}
