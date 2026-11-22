// SPDX-License-Identifier: GPL-3.0-or-later
#include "dipsdialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QTextStream>
#include <QVBoxLayout>

#include "config.h"

QList<DipsDialog::Switch> DipsDialog::read(const QString &game) {
    QList<Switch> out;
    QFile f(cfg::rootDir() + "/games/" + game + "/dips.txt");
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return out;
    QTextStream in(&f);
    while (!in.atEnd()) {
        const QString line = in.readLine();
        if (line.startsWith('#') || line.trimmed().isEmpty()) continue;
        if (line.startsWith(' ')) {   // a setting of the switch above: "  0x01 1P 5C / Continue 5C"
            const QString t = line.trimmed();
            const int sp = t.indexOf(' ');
            if (out.isEmpty() || sp < 0) continue;
            out.last().settings.append({t.left(sp).toInt(nullptr, 0), t.mid(sp + 1)});
            continue;
        }
        // "DSW1 0x0f 0x0f SW1:8,7,6,5 Coinage"
        const QStringList p = line.split(' ');
        if (p.size() < 5) continue;
        Switch s;
        s.bank = p[0];
        s.mask = p[1].toInt(nullptr, 0);
        s.preset = p[2].toInt(nullptr, 0);
        s.where = p[3];
        s.name = p.mid(4).join(' ');
        out.append(s);
    }
    return out;
}

bool DipsDialog::available(const QString &game) { return !read(game).isEmpty(); }

QMap<QString, int> DipsDialog::banks(const QString &game) {
    QMap<QString, int> b;
    for (const Switch &s : read(game)) b[s.bank] = (b.value(s.bank, 0xff) & ~s.mask) | s.preset;
    for (const QString &kv : cfg::settings().value("dips/" + game).toString().split(',', Qt::SkipEmptyParts)) {
        const int eq = kv.indexOf('=');
        if (eq > 0 && b.contains(kv.left(eq))) b[kv.left(eq)] = kv.mid(eq + 1).toInt(nullptr, 0) & 0xff;
    }
    return b;
}

QStringList DipsDialog::arguments(const QString &game) {
    QStringList a;
    for (const QString &kv : cfg::settings().value("dips/" + game).toString().split(',', Qt::SkipEmptyParts))
        a << "--dip" << kv;
    return a;
}

QString DipsDialog::summary(const QString &game) {
    if (!cfg::settings().contains("dips/" + game)) return {};
    const auto b = banks(game);
    QStringList out;
    for (const Switch &s : read(game)) {
        const int v = b.value(s.bank, 0xff) & s.mask;
        if (v == s.preset) continue;
        QString name = QString("0x%1").arg(v, 2, 16, QChar('0'));
        for (const auto &[value, label] : s.settings)
            if (value == v) name = label;
        out << s.name + ": " + name;
    }
    return out.join(", ");
}

DipsDialog::DipsDialog(const QString &game, const QString &title, QWidget *parent) : QDialog(parent), m_game(game) {
    // TR: %1 is the game's title
    setWindowTitle(tr("DIP switches: %1").arg(title));
    m_switches = read(game);
    const auto b = banks(game);
    auto *body = new QWidget;
    auto *v = new QVBoxLayout(body);
    auto *about = new QLabel(tr("The switches on the game's board, which the operator sets: the coinage (Free Play), "
                                "difficulty, demo sounds, and some games' hidden modes. Named as MAME names them. The game "
                                "reads them when it starts: a change takes effect at the next start."));
    about->setWordWrap(true);
    v->addWidget(about);
    QMap<QString, QFormLayout *> forms;
    for (const Switch &s : m_switches) {
        if (!forms.contains(s.bank)) {
            auto *g = new QGroupBox(s.bank + " (" + s.where.section(':', 0, 0) + ")");
            forms[s.bank] = new QFormLayout(g);
            v->addWidget(g);
        }
        auto *box = new QComboBox;
        for (const auto &[value, label] : s.settings) box->addItem(value == s.preset ? tr("%1 (default)").arg(label) : label, value);   // TR: %1 is a switch's setting as MAME names it (English)
        box->setCurrentIndex(std::max(0, box->findData(b.value(s.bank, 0xff) & s.mask)));
        box->setToolTip(s.where);
        forms[s.bank]->addRow((s.name == "Unknown" ? s.name + " (" + s.where + ")" : s.name) + ":", box);
        m_boxes.append(box);
    }
    v->addStretch(1);
    auto *scroll = new QScrollArea;   // a game has up to 17: it scrolls on a small screen (fit.h)
    scroll->setWidget(body);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    auto *defaults = bb->addButton(tr("Defaults"), QDialogButtonBox::ResetRole);
    connect(defaults, &QPushButton::clicked, this, [this] {
        for (int i = 0; i < m_switches.size(); i++)
            m_boxes[i]->setCurrentIndex(std::max(0, m_boxes[i]->findData(m_switches[i].preset)));
    });
    connect(bb, &QDialogButtonBox::accepted, this, &DipsDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto *outer = new QVBoxLayout(this);
    outer->addWidget(scroll);
    outer->addWidget(bb);
    resize(std::max(480, body->sizeHint().width() + 40), body->sizeHint().height() + 60);
}

void DipsDialog::accept() {
    QMap<QString, int> byte, preset;
    for (int i = 0; i < m_switches.size(); i++) {
        const Switch &s = m_switches[i];
        byte[s.bank] = (byte.value(s.bank, 0xff) & ~s.mask) | (m_boxes[i]->currentData().toInt() & s.mask);
        preset[s.bank] = (preset.value(s.bank, 0xff) & ~s.mask) | s.preset;
    }
    QStringList kept;   // the banks that differ from MAME's defaults
    for (auto it = byte.begin(); it != byte.end(); ++it)
        if (it.value() != preset.value(it.key())) kept << QString("%1=0x%2").arg(it.key()).arg(it.value(), 2, 16, QChar('0'));
    if (kept.isEmpty()) cfg::settings().remove("dips/" + m_game);
    else cfg::settings().setValue("dips/" + m_game, kept.join(','));
    QDialog::accept();
}
