// SPDX-License-Identifier: GPL-3.0-or-later
// The game list: pick a game, play it; controls, lights and settings.
#pragma once

#include <QHash>
#include <QJsonObject>
#include <QMainWindow>
#include <QProcess>

#include "games.h"

class QAction;
class QDockWidget;
class QFile;
class QLabel;
class QLineEdit;
class QMenu;
class QStackedWidget;
class QPlainTextEdit;
class QTreeWidget;
class QTreeWidgetItem;
class UpdateCheck;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();
    void play(const QString &id);
    // ingest a game's files (tools/ingest.py): ROM sets, CHDs, images, folders
    void addGameFiles(const QStringList &paths);
    void setEcho(bool on) { m_echo = on; }   // the log on stderr too (hiraku --add from a terminal)
protected:
    void closeEvent(QCloseEvent *e) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dropEvent(QDropEvent *e) override;
    bool eventFilter(QObject *o, QEvent *e) override;   // the keys typed in the window (secretKeys)
private:
    void secretKeys(int key);   // ↑↑↓↓←→←→ B A: the AVS2 PC games listed, or hidden again
    void refresh();
    void applyFilter();
    void updateActions();
    void showDetails();
    const Game *selected() const;
    QList<QTreeWidgetItem *> gameItems() const;   // the list's games (not their series' and systems' rows)
    QTreeWidgetItem *itemFor(const QString &id) const;
    void selectGame(QTreeWidgetItem *it);         // ... with its series opened to show it
    const Game *find(const QString &id) const;
    void nvidiaNotice(const Game &g);
    bool wineMissing(const Game &g);    // a PC game on Linux without Wine: told how to get it   // the PC games on Windows: NVIDIA's present method, told (nvidia.h)
    QStringList arguments(const Game &g) const;
    void stop();
    void contextMenu(const QPoint &pos);
    void fillGameMenu(QMenu *menu, const Game &g);   // the game's menu: the Game menu, and right-click
    void refreshStatuses();                          // the Status column (first boot, mods...)
    void gameChanged();                              // ... and the details, after a change to the game's setup
    void showEmpty();                                // no game added: the list's place says how to add one
    void exitReport(const QString &title, int code, QProcess::ExitStatus st);   // why a game stopped, and its log
    void controls(const QString &family);   // the Controls dialog, which can end by starting a game to try them
    void log(const QString &text);

    QList<Game> m_games;
    QTreeWidget *m_list;
    QStackedWidget *m_listStack;   // the list, or the page shown while no game is added
    QWidget *m_detailsPane;        // the details beside the list (in m_detailsDock; hidden with that page)
    QStringList m_collapsed;     // the series and systems the player closed (ui/collapsed)
    bool m_arranging = false;    // the list being filled or filtered: not the player opening a series
    int m_secret = 0;            // how far into secretKeys' sequence the last keys were
    QLabel *m_details;
    QLineEdit *m_filter;
    QPlainTextEdit *m_log;
    QFile *m_runLog = nullptr;   // the running game's log file (cfg::logsDir): what the log pane gets
    QString m_lastRunLog;        // ... its path, kept after the game ends
    QString m_runOutput;         // the running game's output (its tail), for why it stopped
    void openRunLog(const QString &game);
    void closeRunLog();
    QDockWidget *m_logDock;
    QDockWidget *m_detailsDock;    // the game's details, beside the list
    static constexpr int kLayout = 1;   // saveState's version: 1, the details and log beside the list
    QAction *m_play, *m_stop, *m_controls, *m_lights, *m_showAll;
    QProcess m_proc;
    QProcess m_ingest;                // tools/ingest.py, one batch at a time
    QList<QStringList> m_ingestQueue;
    QString m_ingestOutput;
    void startIngest();
    QString askPlace(const QStringList &paths);   // move, stay, "" (no question: a BIOS or keys), or "cancel"
    // the details' list of the game's files (tools/checklist.py): in the background, kept by game
    QProcess m_files;
    QString m_filesGame;              // the game it runs for
    bool m_filesBig = false;          // ... hashing the big files too
    QHash<QString, QJsonObject> m_fileLists;
    void checkFiles(const QString &id, bool big);
    QString filesHtml(const Game &g);
    QString m_running;          // game id, while one runs
    // updates (updater.h): the bar that says a new version is out, the release it names
    void checkForUpdate(bool asked);
    void offerUpdate();
    UpdateCheck *m_check = nullptr;
    QWidget *m_updateBar = nullptr;
    QLabel *m_updateText = nullptr;
    QJsonObject m_release;
    bool m_checkAsked = false;
    bool m_updateBegun = false;
    bool m_stopping = false;
    bool m_echo = false;
};
