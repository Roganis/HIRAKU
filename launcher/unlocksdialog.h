// SPDX-License-Identifier: GPL-3.0-or-later
// A game's unlocks (games/<id>/unlocks.toml), as tools/unlock.py reads and writes them in the
// game's save: pick a level, Apply. The per-credit codes are shown for doing it by hand, and the
// game's code macros (games/<id>/macros.txt, runtime/host/macro.h) with their Ctrl+N keys.
#pragma once

#include <QDialog>
#include <QJsonObject>
#include <QList>

class QComboBox;
class QLabel;
class QVBoxLayout;

class UnlocksDialog : public QDialog {
    Q_OBJECT
public:
    // running: the game is running now (its save would be overwritten when it exits)
    UnlocksDialog(const QString &game, const QString &title, bool running, QWidget *parent = nullptr);
    static bool available(const QString &game);   // games/<id>/unlocks.toml or macros.txt exists

    struct Macro {
        QString name, text;
        bool atStart = false;   // has an `at` frame: can run by itself at every start
    };
    static QList<Macro> macros(const QString &game);
    // The --macro arguments for the macros the player set to run at every start.
    static QStringList macroArguments(const QString &game);
private:
    void addMacros(class QVBoxLayout *v);

    QString save() const;   // the save Play uses with the mods on now
    QJsonObject run(const QStringList &extra, QString *err);
    void rebuild();

    QString m_game, m_title;
    bool m_running;
    QVBoxLayout *m_body;
    QWidget *m_content = nullptr;
};
