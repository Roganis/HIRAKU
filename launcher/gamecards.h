// SPDX-License-Identifier: GPL-3.0-or-later
// The games as cards (View > Cards, or the switch beside the search): shelves down the left (all
// the games, then a series each), and the open shelf's games as cards, each with the picture of its
// title screen the game menu keeps (cfg::menuPicturesDir), or until there is one a stand-in in its
// series' colour.
//
// The list (mainwindow.cpp) stays what the launcher acts on: a card picked picks its game there,
// and the list's search and "games not added yet" decide which cards there are.
#pragma once

#include <QList>
#include <QString>
#include <QWidget>

class QComboBox;
class QGridLayout;
class QLabel;
class QListWidget;
class QPushButton;

struct Card {
    QString id, title;
    QString detail;    // under the title: "2004 · Konami Python"
    QString series, family;
    QString status;    // said on the card: a game not ready's, or a ready one's when its next start is special; "" none
    bool ready = false, notable = false;
    QString picture;   // its title screen's file, "" none yet
};

class GameCards : public QWidget {
    Q_OBJECT
public:
    explicit GameCards(QWidget *parent = nullptr);
    void setCards(const QList<Card> &cards);   // the games shown, in the list's order
    void select(const QString &id);            // its card picked (its shelf opened, if another is); signals nothing
    void themeChanged();
    void focusCards();
    const Card *card(int row) const;           // the open shelf's row'th (the cards' painter)
protected:
    bool eventFilter(QObject *o, QEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;   // a small window (640x480): the shelves as a drop-down
signals:
    void picked(const QString &id);
    void activated(const QString &id);         // double-clicked: play it
    void menuAsked(const QString &id, const QPoint &global);
private:
    void fillShelves();
    void showShelf(bool pickFirst);
    void fitGrid();
    QListWidget *m_shelves, *m_grid;
    QComboBox *m_pick;    // the shelves in a small window, in their list's place (the same rows)
    QLabel *m_head, *m_sub;
    QList<Card> m_all;
    QList<int> m_shown;   // m_all's cards on the open shelf
    QString m_shelf;      // its series, "" all the games (ui/shelf)
    QString m_current;    // the game picked
    bool m_quiet = false;
};

// The chosen game, large, over its details in the cards view: its picture, its title and a line
// (maker, year, system, status), the mods on as chips, Play in its series' colour, and its Controls,
// Unlocks and Mods
struct Head {
    Card card;
    QString line;          // "Konami · 2004 · Konami Python · Ready"
    QStringList mods;      // the mods on, by name
    bool canPlay = false;  // ready, and no game running
    bool unlocks = false;  // it has unlocks or codes (UnlocksDialog::available)
};

class GameHead : public QWidget {
    Q_OBJECT
public:
    explicit GameHead(QWidget *parent = nullptr);
    void set(const Head &h);
    void setPlayable(bool on);   // (a game started or ended)
    void themeChanged();
signals:
    void play();
    void controls();
    void unlocks();
    void mods();
protected:
    void resizeEvent(QResizeEvent *e) override;
private:
    QWidget *m_art;   // the picture, title, line and chips, painted (gamecards.cpp: HeadArt)
    QPushButton *m_play, *m_controls, *m_unlocks, *m_mods;
    QGridLayout *m_buttons;   // Controls, Unlocks and Mods: in a row, or two rows in a narrow panel (resizeEvent)
    bool m_twoRows = false;
    Head m_h;
};
