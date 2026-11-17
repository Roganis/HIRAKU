// SPDX-License-Identifier: GPL-3.0-or-later
// The games as cards (launcher/gamecards.h): a shelf for all the games and one a series; a game
// picked in the list opens its shelf without saying it was picked (the list is where it was); a
// shelf opened by the player picks its first game; a shelf whose games a search hides gives way to
// all of them; and the cards come and go with the games shown.
//
//   test_gamecards DIR    (DIR: where its settings are kept, on Windows Qt's test folder)
#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QListWidget>
#include <QScrollArea>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QVBoxLayout>

#include <cstdio>

#include "config.h"
#include "gamecards.h"

static int g_failed = 0;
#define CHECK(x) \
    do { \
        if (!(x)) g_failed++, fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
    } while (0)

static Card card(const char *id, const char *series, bool ready = true) {
    Card c;
    c.id = id;
    c.title = id;
    c.series = series;
    c.family = "popn";
    c.ready = ready;
    return c;
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
#ifndef _WIN32
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("XDG_CONFIG_HOME", argv[1]);        // (ui/shelf: not in the player's settings)
#endif
    QApplication app(argc, argv);
#ifdef _WIN32
    QStandardPaths::setTestModeEnabled(true);   // (... there: Qt's test folder)
#endif
    cfg::settings().remove("ui/shelf");         // (nor a shelf the last run left open)
    GameCards w;
    QList<QListWidget *> lists = w.findChildren<QListWidget *>();
    CHECK(lists.size() == 2);
    if (lists.size() != 2) return 1;
    QListWidget *shelves = lists[0], *grid = lists[1];
    QStringList picked;   // what the cards said was picked
    QObject::connect(&w, &GameCards::picked, [&](const QString &id) { picked << id; });

    const QList<Card> all{card("bm1", "beatmania"), card("bm2", "beatmania", false), card("pn1", "pop'n music"),
                          card("pn2", "pop'n music"), card("pn3", "pop'n music")};
    w.setCards(all);
    CHECK(shelves->count() == 3);   // all, beatmania, pop'n music
    CHECK(shelves->currentRow() == 0);
    CHECK(grid->count() == 5);

    // a game picked in the list: its card, nothing said back
    w.select("pn2");
    CHECK(grid->currentRow() == 3);
    CHECK(picked.isEmpty());

    // the player opens pop'n music: its games, and the one picked stays
    shelves->setCurrentRow(2);
    CHECK(grid->count() == 3);
    CHECK(grid->currentRow() == 1);
    CHECK(picked.isEmpty());   // (pn2 already was)

    // ... then beatmania: its first game is picked, and said
    shelves->setCurrentRow(1);
    CHECK(grid->count() == 2);
    CHECK(grid->currentRow() == 0);
    CHECK(picked.size() == 1 && picked.last() == "bm1");

    // a game of another series picked in the list: its shelf opens
    picked.clear();
    w.select("pn3");
    CHECK(shelves->currentRow() == 2);
    CHECK(grid->count() == 3 && grid->currentRow() == 2);
    CHECK(picked.isEmpty());

    // a card picked: said
    grid->setCurrentRow(0);
    CHECK(picked.size() == 1 && picked.last() == "pn1");

    // a search that leaves no pop'n music: all the games' shelf, not an empty one
    w.setCards({all[0], all[1]});
    CHECK(shelves->count() == 2);
    CHECK(shelves->currentRow() == 0);
    CHECK(grid->count() == 2);
    // ... and back: the shelf stays all (the player's choice was let go)
    w.setCards(all);
    CHECK(shelves->currentRow() == 0);
    CHECK(grid->count() == 5);

    // nothing picked in the list: no card picked
    w.select("pn1");
    w.select(QString());
    CHECK(grid->currentRow() < 0 || !grid->currentItem()->isSelected());

    // no games at all
    w.setCards({});
    CHECK(shelves->count() == 1 && grid->count() == 0);


    // a narrow window: the shelves as a drop-down; widened while the list's page showed, the cards
    // page back with its shelves (Qt gives a hidden page its new size as it is shown, before it is
    // visible: compared with isVisible(), the change was taken for done)
    {
        QWidget top;
        auto *stack = new QStackedWidget;
        auto *cards = new GameCards;
        stack->addWidget(cards);
        stack->addWidget(new QWidget);   // (the list's page)
        (new QVBoxLayout(&top))->addWidget(stack);
        cards->setCards(all);
        QListWidget *sh = cards->findChildren<QListWidget *>().value(0);
        QComboBox *pick = cards->findChild<QComboBox *>();
        CHECK(sh && pick);
        if (!sh || !pick) return 1;
        top.resize(800, 600);
        top.show();
        app.processEvents();
        CHECK(sh->isHidden() && !pick->isHidden());
        stack->setCurrentIndex(1);
        app.processEvents();
        top.resize(1000, 600);
        app.processEvents();
        stack->setCurrentIndex(0);
        app.processEvents();
        CHECK(!sh->isHidden() && pick->isHidden());
        CHECK(sh->isVisible());
        // ... and narrowed again while hidden
        stack->setCurrentIndex(1);
        app.processEvents();
        top.resize(800, 600);
        app.processEvents();
        stack->setCurrentIndex(0);
        app.processEvents();
        CHECK(sh->isHidden() && !pick->isHidden());
    }

    // the chosen game's panel: its picture gone in a window under 600 lines high and back over it,
    // the window's width the same (the panel's own size changes only with its height then)
    {
        QWidget top;
        auto *head = new GameHead;
        auto *scroll = new QScrollArea;   // (as the launcher's details; its bar always there: the same width)
        scroll->setWidgetResizable(true);
        scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
        scroll->setWidget(head);
        (new QVBoxLayout(&top))->addWidget(scroll);
        Head h;
        h.card = all[2];
        h.line = "pop'n music";
        head->set(h);
        top.resize(700, 800);
        top.show();
        app.processEvents();
        QList<QWidget *> kids = head->findChildren<QWidget *>(Qt::FindDirectChildrenOnly);
        QWidget *art = kids.value(0);
        CHECK(art);
        if (!art) return 1;
        const int tall = art->height();
        top.resize(700, 500);
        app.processEvents();
        const int low = art->height();
        CHECK(low < tall - 200);   // (the picture: 3/4 of the panel's width)
        top.resize(700, 800);
        app.processEvents();
        CHECK(art->height() == tall);
    }

    if (g_failed) fprintf(stderr, "%d failed\n", g_failed);
    return g_failed ? 1 : 0;
}
