// SPDX-License-Identifier: GPL-3.0-or-later
// The launcher's side of the game menu (runtime/menu/menu.h): the list it hands hiraku-menu, and
// the choice it reads back. Qt's core only, tested on its own (tests/launcher/test_gamemenu.cpp).
#pragma once

#include <QFile>
#include <QList>
#include <QMap>
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

// The list file's text: the rows in the order given (a shelf's games together), the game the
// menu opens on, and the shelves' colours by name ("#rrggbb": the series', drawn when the menu has
// the launcher's look). Tabs and line ends can't be in a field: they'd be taken for the next one.
inline QByteArray listText(const QList<Row> &rows, const QString &current, const QMap<QString, QString> &shelfColours = {}) {
    const auto field = [](QString s) { return s.replace('\t', ' ').replace('\n', ' ').replace('\r', ' '); };
    QString text = "hiraku-menu-list 1\n";
    if (!current.isEmpty()) text += "current\t" + field(current) + "\n";
    for (auto it = shelfColours.cbegin(); it != shelfColours.cend(); ++it) text += "shelf\t" + field(it.key()) + "\t" + field(it.value()) + "\n";
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

// The lines a program says to the launcher about its window (runtime/main.cpp, with HK_LAUNCHER=1),
// and the ones a game and the menu say as they leave for the next program
constexpr const char *kHoldBegun = "game menu: hold begun";       // 5, 7 and 3 held: the box shows
constexpr const char *kHoldLetGo = "game menu: hold let go";      // ... and let go before leaving
constexpr const char *kLeaving = "game menu: asked for";          // the hold lasted: the game ends
constexpr const char *kChosen = "menu: chosen ";                  // the menu ends for a game
constexpr const char *kFirstPicture = "window: first picture";    // the window is up, its picture on it

// Where the black cover goes over the screens (MainWindow::cover), as a program gives way to the next
// one in full screen (a game to the game menu, the menu to a game). The cover is up BEFORE the old
// window goes and down only once the next one has a picture, so nothing else is seen in between:
// - Down: not shown.
// - Under: shown right under the running program's window (Windows: placed after it; X11: kept
//   below), painted and out of sight, ready for the moment that window goes: during a hold of the
//   way out, and all through the menu loop (the menu, a game chosen in it), where any ending of the
//   program (its own, Escape, an error) shows black.
// - Top: above everything, the taskbar too, from a program's leaving until the next one's first
//   picture is under it (the next program's window opens and goes full screen under the black).
// Outside the loop a game started from the launcher's window has no cover, except under it during a hold.
enum class Cover { Down, Under, Top };

class CoverPlan {
public:
    // A program was started: in full screen or not, and in the menu loop (the menu itself, a game
    // chosen in it) or from the launcher's window
    Cover started(bool full, bool loop) {
        full_ = full, loop_ = loop;
        if (!full_) at_ = Cover::Down;   // (a cover up for a window that isn't full screen: off)
        else if (loop_ && at_ == Cover::Down) at_ = Cover::Top;   // (the menu at the start: black till it shows)
        return at_;
    }
    // A line of the running program's output
    Cover said(const QString &line) {
        if (line.startsWith(kHoldBegun)) {
            if (full_ && at_ == Cover::Down) at_ = Cover::Under;
        } else if (line.startsWith(kHoldLetGo)) {
            if (at_ == Cover::Under && !loop_) at_ = Cover::Down;
        } else if (line.startsWith(kLeaving) || line.startsWith(kChosen)) {
            if (full_) at_ = Cover::Top;
        } else if (line.startsWith(kFirstPicture)) {
            if (at_ == Cover::Top) at_ = loop_ ? Cover::Under : Cover::Down;
        }
        return at_;
    }
    // The program ended: `next` a program comes after it (the menu, or the game chosen in it)
    Cover ended(bool next) {
        at_ = next && (full_ || at_ != Cover::Down) ? Cover::Top : Cover::Down;
        return at_;
    }
    // The next program has said nothing of its picture for long (an old one, or one that never
    // draws): the black gives way as if it had
    Cover timedOut() { return said(kFirstPicture); }
    // The launcher's own window is shown again, or the next program couldn't start
    Cover back() { return at_ = Cover::Down; }
    // Where the cover went, against the plan: under a program that has no window yet (its start
    // slow, the fallback before it) it stays as it was, and is put there again and again till it
    // can be (MainWindow::coverTo's timer), or the plan has moved on
    void placed(bool ok) { pending_ = !ok && at_ == Cover::Under; }
    bool pending() const { return pending_ && at_ == Cover::Under; }
    Cover at() const { return at_; }
    bool full() const { return full_; }   // the last program started was in full screen

private:
    Cover at_ = Cover::Down;
    bool full_ = false, loop_ = false, pending_ = false;
};

}  // namespace gamemenu
