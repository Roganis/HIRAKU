// SPDX-License-Identifier: GPL-3.0-or-later
// A game's mods (docs/MODS.md): the folders in <saves>/mods/<game>, each with its mod.toml; the
// ones on, in order. The game then keeps its saves apart (cfg::saveDir).
#pragma once

#include <QDialog>

class QLabel;
class QListWidget;
class QPushButton;

class ModsDialog : public QDialog {
    Q_OBJECT
public:
    ModsDialog(const QString &game, const QString &title, QWidget *parent = nullptr);
    void accept() override;

    // a mod's mod.toml, its top lines (name, version, description, game)
    struct Info { QString folder, name, version, description, game; };
    static Info read(const QString &dir);
    // the game's mods that are on, for its page: "name, name"
    static QString summary(const QString &game);

private:
    void fill();
    void move(int by);
    void selected(int row);   // its description and settings below the list, Settings… for it
    QString m_game;
    QListWidget *m_list;
    QLabel *m_about;
    QPushButton *m_settings;   // the chosen mod's settings panel (modsettingsdialog.h)
};
