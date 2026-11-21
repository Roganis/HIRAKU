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
#include <QMap>
#include <QScrollBar>
#include <QScrollArea>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyleFactory>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdio>

#include "config.h"
#include "gamecards.h"
#include "theme.h"

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

// the grid's layout as it is drawn: each card's place, the scroll bars, the cells' width
static QString layoutOf(QListWidget *g) {
    QString s = QString("vp %1 cell %2 v%3 h%4 |").arg(g->viewport()->width()).arg(g->gridSize().width())
                    .arg(g->verticalScrollBar()->isVisible()).arg(g->horizontalScrollBar()->isVisible());
    for (int k = 0; k < g->count(); k++) {
        const QRect r = g->visualItemRect(g->item(k));
        s += QString(" %1,%2").arg(r.x()).arg(r.y());
    }
    return s;
}

// the cards across each row, top to bottom
static QList<int> rowsOf(QListWidget *g) {
    QMap<int, int> rows;
    for (int k = 0; k < g->count(); k++) rows[g->visualItemRect(g->item(k)).y() + g->verticalScrollBar()->value()]++;
    return rows.values();
}

// Seven cards on one shelf at widths 700..1500 and heights about two rows': each size's layout
// stops changing, then its rows are full but the last, no card is cut off at the right, and there's
// no scroll bar across
static void sweep(QApplication &app, const char *look) {
    QList<Card> seven;
    for (int k = 0; k < 7; k++) seven << card(qPrintable(QString("g%1").arg(k)), k < 4 ? "beatmania" : "pop'n music");
    cfg::settings().remove("ui/shelf");   // (all the games' shelf)
    GameCards w;
    w.setCards(seven);
    QListWidget *g = w.findChildren<QListWidget *>().value(1);
    CHECK(g && g->count() == 7);
    CHECK(g);
    if (!g) return;
    w.resize(1200, 900);
    w.show();
    for (int k = 0; k < 10; k++) app.processEvents();
    // the window's height for a grid this high (the lines over the grid: the window's less the grid's)
    const int cellH = g->gridSize().height(), over = w.height() - g->height();
    QList<int> heights{500, 600, 700, 800};
    for (int d : {1, cellH / 8, cellH / 4, cellH / 2, 3 * cellH / 4, cellH - 8, cellH - 1}) heights << over + 2 * cellH + d;
    QList<int> widths;
    for (int x = 700; x <= 1500; x += 7) widths << x;
    int bad = 0, sizes = 0;
    for (int hgt : heights)
        for (int wid : widths) {
            sizes++;
            w.resize(wid, hgt);
            QString last = layoutOf(g);
            int lastChange = -1;
            QStringList seen;   // the layouts in the last turns
            const int turns = 40;
            for (int t = 0; t < turns; t++) {
                app.processEvents();
                const QString now = layoutOf(g);
                if (now != last) lastChange = t, last = now;
                if (t >= turns - 10 && !seen.contains(now)) seen << now;
            }
            if (lastChange >= turns - 10) {
                if (bad++ < 12) {
                    fprintf(stderr, "FAIL %s %dx%d never settles (grid %dx%d):\n", look, wid, hgt, g->width(), g->height());
                    for (const QString &s : seen) fprintf(stderr, "    %s\n", qPrintable(s));
                }
                continue;
            }
            const QList<int> rows = rowsOf(g);
            bool even = true;
            for (int r = 0; r + 1 < rows.size(); r++) even = even && rows[r] == rows[0];
            if (!rows.isEmpty()) even = even && rows.last() <= rows[0];
            int right = 0;
            for (int k = 0; k < g->count(); k++) right = std::max(right, g->visualItemRect(g->item(k)).right());
            const bool cut = right >= g->viewport()->width(), across = g->horizontalScrollBar()->isVisible();
            if (!even || cut || across) {
                if (bad++ < 12) {
                    QStringList rs;
                    for (int n : rows) rs << QString::number(n);
                    fprintf(stderr, "FAIL %s %dx%d rows %s, right edge %d of %d, scroll bar across %d\n", look, wid, hgt,
                            qPrintable(rs.join('+')), right, g->viewport()->width(), across);
                }
            }
        }
    if (bad) fprintf(stderr, "%s: %d of %d sizes bad\n", look, bad, sizes);
    else fprintf(stderr, "%s: %d sizes settle\n", look, sizes);
    g_failed += bad;
}

// The chosen game's panel over a line of details, in a scroll area as the launcher's Details panel
// (mainwindow.cpp), at widths 400..700 and heights 600..760: each size settles; then its chips
// changed, the picture's panel takes their height
static void panelSweep(QApplication &app) {
    QWidget top;
    auto *head = new GameHead;
    auto *box = new QWidget;
    auto *bv = new QVBoxLayout(box);
    bv->setContentsMargins(0, 0, 0, 0);
    bv->setSpacing(0);
    bv->addWidget(head);
    auto *line = new QLabel("details");
    line->setWordWrap(true);
    bv->addWidget(line, 1);
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(box);
    auto *tv = new QVBoxLayout(&top);
    tv->setContentsMargins(0, 0, 0, 0);
    tv->addWidget(scroll);
    Head h;
    h.card = card("pn1", "pop'n music");
    h.line = "pop'n music";
    head->set(h);
    top.resize(500, 800);
    top.show();
    for (int k = 0; k < 10; k++) app.processEvents();
    int bad = 0, sizes = 0;
    for (int wid = 400; wid <= 700; wid += 4)
        for (int hgt = 600; hgt <= 760; hgt += 4) {
            sizes++;
            top.resize(wid, hgt);
            QString last;
            int lastChange = -1;
            QStringList seen;
            const int turns = 40;
            for (int t = 0; t < turns; t++) {
                app.processEvents();
                const QString now = QString("panel %1x%2 bar %3").arg(box->width()).arg(box->height())
                                        .arg(scroll->verticalScrollBar()->isVisible());
                if (now != last) lastChange = t, last = now;
                if (t >= turns - 10 && !seen.contains(now)) seen << now;
            }
            if (lastChange >= turns - 10 && bad++ < 12)
                fprintf(stderr, "FAIL panel %dx%d never settles: %s\n", wid, hgt, qPrintable(seen.join(" / ")));
        }
    if (bad) fprintf(stderr, "panel: %d of %d sizes never settle\n", bad, sizes);
    else fprintf(stderr, "panel: %d sizes settle\n", sizes);
    g_failed += bad;
    // its chips changed: the picture's panel as high as they need, Play under them
    QWidget *art = head->findChildren<QWidget *>(Qt::FindDirectChildrenOnly).value(0);
    QWidget *play = head->findChildren<QWidget *>(Qt::FindDirectChildrenOnly).value(1);
    CHECK(art && play);
    if (!art || !play) return;
    for (QSize at : {QSize(400, 900), QSize(700, 640)}) {
        h.mods.clear();
        head->set(h);
        top.resize(at);
        for (int k = 0; k < 10; k++) app.processEvents();
        h.mods = QStringList{"one mod", "another mod", "a third mod with a longer name", "four", "five", "six"};
        head->set(h);
        for (int k = 0; k < 10; k++) app.processEvents();
        CHECK(art->height() == art->heightForWidth(art->width()));
        CHECK(play->y() > art->geometry().bottom());
        CHECK(box->height() >= box->heightForWidth(box->width()));
    }
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

    // the cards' columns at every size, in a style whose scroll bar takes room: they settle (they
    // went 4 and 3 across in turn, the scroll bar on and off, when a height held two rows and not three)
    if (QStyleFactory::keys().contains("Fusion", Qt::CaseInsensitive)) {
        // (first the style the launcher starts with, when another: on Windows the system's)
        const QString own = QApplication::style()->name();
        if (own.compare("fusion", Qt::CaseInsensitive) && own.compare("windows", Qt::CaseInsensitive))
            sweep(app, qPrintable(own));
        QApplication::setStyle("Fusion");
        sweep(app, "Fusion");
        if (QStyleFactory::keys().contains("Windows", Qt::CaseInsensitive)) {
            QApplication::setStyle("Windows");
            sweep(app, "Windows");
        }
        theme::apply(theme::Pop);   // (Fusion and the Pop sheet's own scroll bar)
        sweep(app, "Pop");
        // ... and the chosen game's panel in a scroll area as the launcher's Details (its bar as
        // needed): it settles too (its picture, 3/4 of its width, was a bar's width shorter with the
        // bar, and the bar went on and off), and takes the height its chips need when they change
        qApp->setStyleSheet(QString());
        QApplication::setStyle("Fusion");
        panelSweep(app);
    }

    if (g_failed) fprintf(stderr, "%d failed\n", g_failed);
    return g_failed ? 1 : 0;
}
