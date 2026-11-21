// SPDX-License-Identifier: GPL-3.0-or-later
// A mod's settings panel (the Mods dialog's Settings…): a row a [[setting]], its label and a tick
// box, a list or a number box, the default beside it; the actions' keys under them, read-only;
// Defaults, OK, Cancel. It reads and writes the file the game keeps them in (modsettings.h), so
// it works for a mod that isn't on too.
#pragma once

#include <QDialog>

#include "modsettings.h"

class QLabel;

class ModSettingsDialog : public QDialog {
    Q_OBJECT
public:
    ModSettingsDialog(const QString &game, const QString &folder, QWidget *parent = nullptr);
    void accept() override;

private:
    void show_values();
    ModSettings m_model;
    std::vector<QWidget *> m_editors;   // a row's: QCheckBox, QComboBox or QSpinBox
    bool m_ok = false;
};
