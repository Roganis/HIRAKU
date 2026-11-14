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

    const QString choice = dir + "/choice";
    QFile::remove(choice);
    CHECK(gamemenu::readChoice(choice).isEmpty());   // closed with none chosen: no file
    {
        QFile f(choice);
        CHECK(f.open(QIODevice::WriteOnly) && f.write("iidx16\r\n") > 0);
    }
    CHECK(gamemenu::readChoice(choice) == "iidx16");
    QFile::remove(choice);

    if (g_failed) {
        fprintf(stderr, "%d checks failed\n", g_failed);
        return 1;
    }
    printf("ok\n");
    return 0;
}
