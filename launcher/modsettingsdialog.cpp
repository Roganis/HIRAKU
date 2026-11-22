// SPDX-License-Identifier: GPL-3.0-or-later
#include "modsettingsdialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QVBoxLayout>

#include <climits>

#include "config.h"
#include "modsdialog.h"

ModSettingsDialog::ModSettingsDialog(const QString &game, const QString &folder, QWidget *parent) : QDialog(parent) {
    const QString dir = cfg::modPath(game, folder);
    const ModsDialog::Info info = ModsDialog::read(dir);
    // TR: %1 is the mod's name (its author's)
    setWindowTitle(tr("Settings: %1").arg(info.name));
    QString err;
    m_ok = m_model.load(dir, cfg::modSettingsFile(game, folder), &err);
    auto *body = new QWidget;
    auto *v = new QVBoxLayout(body);
    auto *about = new QLabel(m_ok ? tr("The mod's settings, kept for the next time the game starts with it on (the game reads "
                                       "them when it starts; the keys below change some while playing).")
                                  // TR: %1 is why (the mod file reader's message, in English)
                                  : tr("This mod's mod.toml can't be read, so the game wouldn't start with it:\n%1").arg(err));
    about->setWordWrap(true);
    v->addWidget(about);
    if (m_ok && !m_model.empty()) {
        auto *group = new QGroupBox(info.name);
        auto *form = new QFormLayout(group);
        for (const ModSettings::Row &r : m_model.rows()) {
            QWidget *editor = nullptr;
            const QString dflt = ModSettings::shown(r, r.setting.dflt);
            switch (r.kind) {
            case ModSettings::Kind::tick: {
                auto *box = new QCheckBox(tr("(default: %1)").arg(dflt));   // TR: %1 is the default value ("on", "off", a number or a named value)
                editor = box;
                break;
            }
            case ModSettings::Kind::list: {
                auto *box = new QComboBox;
                for (size_t i = 0; i < r.setting.choices.size(); i++)
                    box->addItem((int64_t)i == r.setting.dflt ? tr("%1 (default)").arg(QString::fromStdString(r.setting.choices[i]))
                                                              : QString::fromStdString(r.setting.choices[i]));   // TR: %1 is one of the mod's named values
                editor = box;
                break;
            }
            case ModSettings::Kind::number: {
                auto *box = new QSpinBox;
                box->setRange((int)std::max<int64_t>(r.setting.min, INT_MIN), (int)std::min<int64_t>(r.setting.max, INT_MAX));
                box->setSingleStep((int)std::min<int64_t>(r.setting.step, INT_MAX));
                // TR: %1 and %2 are the setting's lowest and highest value, %3 its default
                box->setToolTip(tr("%1 to %2, default %3").arg(r.setting.min).arg(r.setting.max).arg(dflt));
                auto *line = new QWidget;
                auto *h = new QHBoxLayout(line);
                h->setContentsMargins(0, 0, 0, 0);
                h->addWidget(box);
                auto *note = new QLabel(tr("%1 to %2 (default: %3)").arg(r.setting.min).arg(r.setting.max).arg(dflt));
                note->setEnabled(false);
                h->addWidget(note, 1);
                m_editors.push_back(box);
                form->addRow(tr("%1:").arg(QString::fromStdString(r.setting.label)), line);
                continue;
            }
            }
            m_editors.push_back(editor);
            // TR: a setting's name (the mod author's, as written) as a form's label
            form->addRow(tr("%1:").arg(QString::fromStdString(r.setting.label)), editor);
        }
        v->addWidget(group);
        if (const QStringList keys = m_model.bindings(); !keys.isEmpty()) {
            auto *k = new QLabel(tr("Keys while playing (the bindings in the settings file):") + "\n" + keys.join("\n"));
            k->setWordWrap(true);
            k->setTextInteractionFlags(Qt::TextSelectableByMouse);
            v->addWidget(k);
        }
    } else if (m_ok) {
        v->addWidget(new QLabel(tr("This mod has no settings.")));
    }
    v->addStretch(1);
    auto *scroll = new QScrollArea;   // (many settings: it scrolls on a small screen, fit.h)
    scroll->setWidget(body);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    auto *defaults = bb->addButton(tr("Defaults"), QDialogButtonBox::ResetRole);
    defaults->setEnabled(m_ok && !m_model.empty());
    bb->button(QDialogButtonBox::Ok)->setEnabled(m_ok);
    connect(defaults, &QPushButton::clicked, this, [this] {
        m_model.defaults();
        show_values();
    });
    connect(bb, &QDialogButtonBox::accepted, this, &ModSettingsDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto *outer = new QVBoxLayout(this);
    outer->addWidget(scroll);
    outer->addWidget(bb);
    show_values();
    resize(std::max(460, body->sizeHint().width() + 40), body->sizeHint().height() + 60);
}

void ModSettingsDialog::show_values() {
    for (size_t i = 0; i < m_editors.size() && i < m_model.rows().size(); i++) {
        const int64_t v = m_model.rows()[i].value;
        if (auto *c = qobject_cast<QCheckBox *>(m_editors[i])) c->setChecked(v != 0);
        else if (auto *b = qobject_cast<QComboBox *>(m_editors[i])) b->setCurrentIndex((int)v);
        else if (auto *s = qobject_cast<QSpinBox *>(m_editors[i])) s->setValue((int)v);
    }
}

void ModSettingsDialog::accept() {
    if (!m_ok) return QDialog::reject();
    for (size_t i = 0; i < m_editors.size() && i < m_model.rows().size(); i++) {
        if (auto *c = qobject_cast<QCheckBox *>(m_editors[i])) m_model.set(i, c->isChecked() ? 1 : 0);
        else if (auto *b = qobject_cast<QComboBox *>(m_editors[i])) m_model.set(i, b->currentIndex());
        else if (auto *s = qobject_cast<QSpinBox *>(m_editors[i])) m_model.set(i, s->value());
    }
    QString err;
    if (!m_model.empty() && !m_model.save(&err)) {
        QMessageBox::warning(this, "HIRAKU", tr("The settings couldn't be kept:\n%1").arg(err));   // TR: %1 is the file and the system's reason
        return;
    }
    QDialog::accept();
}
