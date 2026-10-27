// SPDX-License-Identifier: GPL-3.0-or-later
#include "unlocksdialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSettings>
#include <QTextStream>
#include <QVBoxLayout>

#include "config.h"

static QString gameFile(const QString &game, const QString &name) { return cfg::rootDir() + "/games/" + game + "/" + name; }

bool UnlocksDialog::available(const QString &game) {
    return QFileInfo::exists(gameFile(game, "unlocks.toml")) || QFileInfo::exists(gameFile(game, "macros.txt"));
}

// The names, texts and `at` lines of macros.txt; the game itself reads the steps.
QList<UnlocksDialog::Macro> UnlocksDialog::macros(const QString &game) {
    QList<Macro> list;
    QFile f(gameFile(game, "macros.txt"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return list;
    static const QRegularExpression line(R"re(^\s*(\w+)\s*(?:"([^"]*)"|(\S+))?)re");
    QTextStream in(&f);
    bool inside = false;
    while (!in.atEnd()) {
        auto m = line.match(in.readLine());
        if (!m.hasMatch()) continue;
        QString key = m.captured(1), arg = m.captured(2).isEmpty() ? m.captured(3) : m.captured(2);
        if (key == "macro") {
            list.push_back({arg, {}, false});
            inside = true;
        } else if (!inside) continue;
        else if (key == "end") inside = false;
        else if (key == "text") list.back().text = arg;
        else if (key == "at") list.back().atStart = true;
    }
    return list;
}

QStringList UnlocksDialog::macroArguments(const QString &game) {
    QStringList on = cfg::settings().value("macros/" + game).toStringList(), a;
    for (const Macro &m : macros(game))
        if (m.atStart && on.contains(m.name)) a << "--macro" << m.name;
    return a;
}

void UnlocksDialog::addMacros(QVBoxLayout *v) {
    QList<Macro> list = macros(m_game);
    if (list.isEmpty()) return;
    auto *box = new QGroupBox("Codes, typed for you");
    auto *bv = new QVBoxLayout(box);
    auto *intro = new QLabel("<i>In the game window, Ctrl+1 to Ctrl+9 type the codes below at the moment you press them "
                             "(the window title shows it while it types). The codes last a credit, or until power-off; "
                             "a code marked so can be typed at every start by itself.</i>");
    intro->setWordWrap(true);
    bv->addWidget(intro);
    for (int i = 0; i < list.size(); i++) {
        const Macro &m = list[i];
        QString key = i < 9 ? QString("Ctrl+%1").arg(i + 1) : QString("(no key)");
        auto *name = new QLabel("<b>" + key + "</b>&nbsp;&nbsp;" + m.name.toHtmlEscaped());
        bv->addWidget(name);
        auto *t = new QLabel(m.text);
        t->setWordWrap(true);
        t->setContentsMargins(24, 0, 0, m.atStart ? 0 : 6);
        bv->addWidget(t);
        if (m.atStart) {
            auto *c = new QCheckBox("Type it at every start");
            c->setChecked(cfg::settings().value("macros/" + m_game).toStringList().contains(m.name));
            c->setContentsMargins(24, 0, 0, 6);
            connect(c, &QCheckBox::toggled, this, [game = m_game, n = m.name](bool on) {
                QStringList l = cfg::settings().value("macros/" + game).toStringList();
                l.removeAll(n);
                if (on) l << n;
                cfg::settings().setValue("macros/" + game, l);
            });
            bv->addWidget(c);
        }
    }
    v->addWidget(box);
}

UnlocksDialog::UnlocksDialog(const QString &game, const QString &title, bool running, QWidget *parent)
    : QDialog(parent), m_game(game), m_title(title), m_running(running) {
    setWindowTitle("Unlocks: " + title);
    resize(620, 560);
    auto *v = new QVBoxLayout(this);
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *inner = new QWidget;
    m_body = new QVBoxLayout(inner);
    scroll->setWidget(inner);
    v->addWidget(scroll, 1);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    v->addWidget(bb);
    rebuild();
}

// With mods on, the game plays on a save of their own (cfg::saveDir), made from a copy of the clean
// game's at the first start with them: until then the clean one is the one they'll start from.
QString UnlocksDialog::save() const {
    const QString s = cfg::saveDir(m_game);
    return QFileInfo::exists(s) ? s : cfg::nvramDir(m_game);
}

QJsonObject UnlocksDialog::run(const QStringList &extra, QString *err) {
    const QStringList py = cfg::python();
    if (py.isEmpty()) {
        *err = "tools/unlock.py needs Python 3 (python.org)";
        return {};
    }
    QProcess p;
    p.start(py[0], py.mid(1) + QStringList{cfg::rootDir() + "/tools/unlock.py", m_game, "--json", "--nvram", save()} + extra);
    if (!p.waitForFinished(15000)) {
        *err = "tools/unlock.py did not answer (is Python 3 installed?)";
        return {};
    }
    QJsonObject o = QJsonDocument::fromJson(p.readAllStandardOutput()).object();
    if (o.isEmpty()) *err = QString::fromUtf8(p.readAllStandardError()).trimmed();
    else if (o.contains("error")) *err = o["error"].toString();
    return o;
}

void UnlocksDialog::rebuild() {
    if (m_content) m_content->deleteLater();
    m_content = new QWidget;
    auto *v = new QVBoxLayout(m_content);
    v->setContentsMargins(0, 0, 0, 0);
    m_body->insertWidget(0, m_content);
    QString err;
    QJsonObject st = QFileInfo::exists(gameFile(m_game, "unlocks.toml")) ? run({}, &err) : QJsonObject{};
    const QStringList mods = cfg::modsOn(m_game);
    QString with;
    if (!mods.isEmpty())
        with = QString(save() == cfg::saveDir(m_game) ? "<br>the one with the mods on now: %1"
                                                      : "<br>the mods on now (%1) get a copy of it at their first start")
                   .arg(mods.join(", ").toHtmlEscaped());
    auto *where = new QLabel("Save: <tt>" + save().toHtmlEscaped() + "</tt>" + with);
    where->setTextInteractionFlags(Qt::TextSelectableByMouse);
    where->setToolTip(save());
    where->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);   // a long path is cut, not the dialog widened
    v->addWidget(where);
    if (!err.isEmpty()) {
        auto *e = new QLabel(err);
        e->setTextFormat(Qt::PlainText);   // (a traceback's "<module>" isn't a tag)
        e->setWordWrap(true);
        v->addWidget(e);
    }
    if (m_running && !st.isEmpty()) {
        auto *r = new QLabel("<b>Stop the game first</b>: it writes its save back when it exits.");
        r->setWordWrap(true);
        v->addWidget(r);
    }
    for (const QJsonValue &uv : st["unlocks"].toArray()) {
        QJsonObject u = uv.toObject();
        if (u["kind"].toString() == "switches") {   // independent switches (pop'n 6): one checkbox each
            auto *box = new QGroupBox(u["title"].toString());
            auto *bv = new QVBoxLayout(box);
            QJsonObject cur = u["current"].toObject();
            QList<QPair<QString, QCheckBox *>> checks;
            for (const QJsonValue &sv : u["switch"].toArray()) {
                QJsonObject sw = sv.toObject();
                QString id = sw["id"].toString();
                auto *c = new QCheckBox(sw["name"].toString() + "   (code " + sw["code"].toString() + ")");
                c->setChecked(cur[id].toObject()["on"].toBool());
                c->setEnabled(!u["current"].isNull());
                bv->addWidget(c);
                auto *t = new QLabel(sw["text"].toString());
                t->setWordWrap(true);
                t->setContentsMargins(24, 0, 0, 6);
                bv->addWidget(t);
                checks.push_back({id, c});
            }
            if (u["current"].isNull()) {
                auto *none = new QLabel("No save yet: start the game once (with its first-boot setup), quit, then come back.");
                none->setWordWrap(true);
                bv->addWidget(none);
            }
            auto *how = new QLabel("<i>" + u["how"].toString().toHtmlEscaped() + " Apply writes what the game writes then.</i>");
            how->setWordWrap(true);
            bv->addWidget(how);
            auto *apply = new QPushButton("Apply");
            apply->setEnabled(!m_running && !u["current"].isNull());
            bv->addWidget(apply, 0, Qt::AlignRight);
            connect(apply, &QPushButton::clicked, this, [this, checks] {
                QStringList args;
                for (const auto &[id, c] : checks) args << id + "=" + (c->isChecked() ? "1" : "0");
                QString e;
                run(args, &e);
                if (!e.isEmpty()) QMessageBox::warning(this, m_title, e);
                rebuild();
            });
            v->addWidget(box);
            continue;
        }
        auto *box = new QGroupBox(u["title"].toString());
        auto *bv = new QVBoxLayout(box);
        QJsonValue cur = u["current"];
        auto *combo = new QComboBox;
        QJsonArray levels = u["level"].toArray();
        for (const QJsonValue &lv : levels) {
            QJsonObject l = lv.toObject();
            QString code = l["code"].toString();
            combo->addItem(l["name"].toString() + (code.isEmpty() ? "" : "   (code " + code + ")"), l["value"].toInt());
        }
        if (!cur.isNull()) combo->setCurrentIndex(std::max(0, combo->findData(cur.toInt())));
        auto *text = new QLabel;
        text->setWordWrap(true);
        auto showText = [text, levels, combo] {
            for (const QJsonValue &lv : levels)
                if (lv.toObject()["value"].toInt() == combo->currentData().toInt()) text->setText(lv.toObject()["text"].toString());
        };
        showText();
        connect(combo, &QComboBox::currentIndexChanged, this, showText);
        auto *now = new QLabel(cur.isNull() ? QString("No save yet: start the game once (with its first-boot setup), quit, then come back.")
                                            : "Now: <b>" + combo->itemText(combo->findData(cur.toInt())).section("   (", 0, 0).toHtmlEscaped() + "</b>");
        now->setWordWrap(true);
        bv->addWidget(now);
        bv->addWidget(combo);
        bv->addWidget(text);
        auto *how = new QLabel("<i>" + u["how"].toString().toHtmlEscaped() + " Apply writes what the game writes then.</i>");
        how->setWordWrap(true);
        bv->addWidget(how);
        auto *apply = new QPushButton("Apply");
        apply->setEnabled(!m_running && !cur.isNull());
        bv->addWidget(apply, 0, Qt::AlignRight);
        QString id = u["id"].toString();
        connect(apply, &QPushButton::clicked, this, [this, id, combo] {
            QString e;
            run({id + "=" + QString::number(combo->currentData().toInt())}, &e);
            if (!e.isEmpty()) QMessageBox::warning(this, m_title, e);
            rebuild();
        });
        v->addWidget(box);
    }
    for (const QJsonValue &pv : st["progress"].toArray()) {
        QJsonObject p = pv.toObject();
        auto *box = new QGroupBox(p["title"].toString() + ": event progress");
        auto *bv = new QVBoxLayout(box);
        auto *t = new QLabel(p["text"].toString());
        t->setWordWrap(true);
        t->setTextInteractionFlags(Qt::TextSelectableByMouse);
        bv->addWidget(t);
        if (p.contains("choices") && !p["state"].isNull()) {   // an event the tool can set (pop'n 10's wishes)
            auto *combo = new QComboBox;
            for (const QJsonValue &cv : p["choices"].toArray())
                combo->addItem(cv.toObject()["name"].toString(), cv.toObject()["value"].toString());
            auto *row = new QHBoxLayout;
            row->addWidget(combo, 1);
            auto *apply = new QPushButton("Apply");
            apply->setEnabled(!m_running);
            row->addWidget(apply);
            bv->addLayout(row);
            QString id = p["id"].toString();
            connect(apply, &QPushButton::clicked, this, [this, id, combo] {
                QString e;
                run({id + "=" + combo->currentData().toString()}, &e);
                if (!e.isEmpty()) QMessageBox::warning(this, m_title, e);
                rebuild();
            });
        }
        v->addWidget(box);
    }
    for (const QJsonValue &nv : st["notes"].toArray()) {
        QJsonObject n = nv.toObject();
        auto *box = new QGroupBox(n["title"].toString());
        auto *bv = new QVBoxLayout(box);
        auto *t = new QLabel(n["text"].toString().trimmed());
        t->setWordWrap(true);
        t->setTextInteractionFlags(Qt::TextSelectableByMouse);
        bv->addWidget(t);
        v->addWidget(box);
    }
    addMacros(v);
    v->addStretch(1);
}
