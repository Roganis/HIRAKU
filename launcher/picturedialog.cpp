// SPDX-License-Identifier: GPL-3.0-or-later
#include "picturedialog.h"

#include "theme.h"
#include "../third_party/mdi/icons.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QLocale>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSlider>
#include <QTabWidget>
#include <QTextStream>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

#include "config.h"
#include "language.h"

namespace picture {

QString configDir() { return cfg::configDir() + "/picture"; }
QString configDir(const QString &game) { return configDir() + "/games/" + game; }

QString globalMode() {
    auto &s = cfg::settings();
    QString m = s.value("video/picture").toString();
    if (m.isEmpty()) m = s.value("video/crt", false).toBool() ? "light" : "off";   // the first CRT setting
    return m;
}

QString mode(const QString &game) {
    QString m = cfg::settings().value("picture/" + game).toString();
    return m.isEmpty() ? globalMode() : m;
}

}  // namespace picture

namespace {

// `name = value` lines
QMap<QString, double> readValues(const QString &path) {
    QMap<QString, double> out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return out;
    QTextStream in(&f);
    while (!in.atEnd()) {
        QString line = in.readLine();
        int hash = line.indexOf('#');
        if (hash >= 0) line.truncate(hash);
        int eq = line.indexOf('=');
        if (eq < 0) continue;
        bool ok = false;
        double v = line.mid(eq + 1).trimmed().remove('"').toDouble(&ok);
        if (ok) out[line.left(eq).trimmed()] = v;
    }
    return out;
}

int decimals(double step) {
    for (int d = 0; d < 4; d++) {
        double x = step * std::pow(10.0, d);
        if (std::fabs(x - std::round(x)) < 1e-6) return d;
    }
    return 4;
}

bool same(double a, double b) { return std::fabs(a - b) < 1e-6; }

// TR: the picture's modes; the shaders' own names (crt-royale, ...) stay as they are
const char *const MODES[][2] = {{"off", QT_TRANSLATE_NOOP("PictureDialog", "Off: sharp pixels")},
                                {"light", QT_TRANSLATE_NOOP("PictureDialog", "Light: crt-guest-advanced-fast")},
                                {"full", QT_TRANSLATE_NOOP("PictureDialog", "Full: crt-royale")},
                                {"lola", QT_TRANSLATE_NOOP("PictureDialog", "Analog: Lola Retro")}};

QString modeText(const char *english) { return language::tr("PictureDialog", english); }

// The tab of a mode's shader (off: the first)
int tabOf(const QString &m) { return m == "full" ? 1 : m == "lola" ? 2 : 0; }

// The game's monitor, kHz, as its core says (runtime/shell/core.h, monitor_khz): DJ Main 24,
// Firebeat (no core key) 31, the rest 15; 0 without a game. The shaders' hiraku-<k>khz.params.
int monitorKhz(const QString &game) {
    if (game.isEmpty()) return 0;
    QFile f(cfg::rootDir() + "/games/" + game + "/game.toml");
    QString core;
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&f);
        static const QRegularExpression re("^core\\s*=\\s*\"([^\"]*)\"");
        while (!in.atEnd())
            if (auto m = re.match(in.readLine()); m.hasMatch()) core = m.captured(1);
    }
    return core == "djmain" ? 24 : core.isEmpty() || core == "firebeat" ? 31 : 15;
}

QString modeLabel(const QString &m) {
    for (auto &x : MODES)
        if (m == x[0]) return modeText(x[1]);
    return m;
}

}  // namespace

PictureDialog::PictureDialog(const QString &game, const QString &gameTitle, bool onGame, QWidget *parent)
    : QDialog(parent), m_game(game), m_gameTitle(gameTitle), m_onGame(onGame && !game.isEmpty()) {
    setWindowTitle(tr("Picture"));
    resize(720, 640);
    auto *v = new QVBoxLayout(this);

    auto *top = new QFormLayout;
    m_scope = new QComboBox;
    m_scope->addItem(tr("All games"));
    if (!m_game.isEmpty()) m_scope->addItem(tr("%1 only").arg(m_gameTitle));   // TR: %1 is the game's title
    m_scope->setCurrentIndex(m_onGame ? 1 : 0);
    top->addRow(tr("Settings for:"), m_scope);
    m_mode = new QComboBox;
    top->addRow(tr("Picture:"), m_mode);
    v->addLayout(top);
    auto *note = new QLabel(tr("A running game follows the sliders at once. In the game, F10 cycles off, light, full and analog."));
    note->setWordWrap(true);
    v->addWidget(note);

    m_globalMode = picture::globalMode();
    m_gameMode = m_game.isEmpty() ? QString() : cfg::settings().value("picture/" + m_game).toString();

    for (auto [name, title] : {std::pair{"crt-guest-advanced-fast", "Light: crt-guest-advanced-fast"},
                               std::pair{"crt-royale", "Full: crt-royale"}, std::pair{"lola-retro", "Analog: Lola Retro"}}) {
        Shader sh;
        sh.name = name, sh.title = modeText(title);
        m_shaders.push_back(sh);
    }
    m_values.resize(m_shaders.size());
    m_rows.resize(m_shaders.size());
    m_tabs = new QTabWidget;
    for (int s = 0; s < m_shaders.size(); s++) {
        Shader &sh = m_shaders[s];
        QString dir = cfg::rootDir() + "/shaders/" + sh.name;
        QFile f(dir + "/parameters.txt");
        QString section;
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream in(&f);
            while (!in.atEnd()) {
                QStringList c = in.readLine().split('\t');
                if (c.size() < 6 || c[0].startsWith('#')) continue;
                Param p;
                p.name = c[0];
                p.def = c[1].toDouble();
                p.min = c[2].toDouble();
                p.max = c[3].toDouble();
                p.step = c[4].toDouble();
                p.label = c[5].trimmed();
                if (p.name.startsWith("bogus") || p.label.startsWith('[') || p.label.startsWith("---")) {   // guest's, Lola's section headers
                    section = p.label;
                    section.remove(QRegularExpression("[\\[\\]:]|^-+|-+$"));
                    section = section.trimmed();
                    continue;
                }
                if (p.name.startsWith("info") || p.label.startsWith("INFO") || same(p.min, p.max)) continue;
                int dash = p.label.indexOf(" - ");   // royale's "Mask - Type"
                if (dash > 0) {
                    p.section = p.label.left(dash).trimmed();
                    p.label = p.label.mid(dash + 3).trimmed();
                } else {
                    p.section = section.isEmpty() ? tr("General") : section;   // TR: the shader settings' first group (the others are the shader's own English names)
                }
                sh.base[p.name] = p.def;
                sh.params.push_back(p);
            }
        }
        // HIRAKU's defaults, then those for the game's monitor (as the game reads them)
        QStringList layers = {dir + "/hiraku.params"};
        if (int khz = monitorKhz(m_game)) layers << dir + QString("/hiraku-%1khz.params").arg(khz);
        for (const QString &l : layers) {
            auto hiraku = readValues(l);
            for (auto it = hiraku.begin(); it != hiraku.end(); ++it)
                if (sh.base.contains(it.key())) sh.base[it.key()] = it.value();
        }
        // looks: the shader's presets that differ only in their values (looks/*.params, the
        // first line's comment their name)
        for (const QFileInfo &fi : QDir(dir + "/looks").entryInfoList({"*.params"}, QDir::Files, QDir::Name)) {
            Look lk;
            QFile lf(fi.filePath());
            if (lf.open(QIODevice::ReadOnly | QIODevice::Text)) lk.title = QString::fromUtf8(lf.readLine()).remove('#').trimmed();
            if (lk.title.isEmpty()) lk.title = fi.baseName();
            lk.values = readValues(fi.filePath());
            sh.looks.push_back(lk);
        }
        // what Cancel puts back
        QStringList files = {file(s, false)};
        if (!m_game.isEmpty()) files << file(s, true);
        for (const QString &path : files) {
            QFile b(path);
            m_existed[path] = b.open(QIODevice::ReadOnly);
            m_backup[path] = m_existed[path] ? b.readAll() : QByteArray();
        }
        auto *t = new QTimer(this);
        t->setSingleShot(true);
        t->setInterval(150);
        connect(t, &QTimer::timeout, this, [this, s] { save(s); });
        m_saveTimers.push_back(t);
        buildPage(s);
        m_tabs->addTab(sh.page, sh.title);
    }
    v->addWidget(m_tabs, 1);

    connect(m_scope, &QComboBox::currentIndexChanged, this, [this](int i) {
        for (int s = 0; s < m_shaders.size(); s++)
            if (m_saveTimers[s]->isActive()) {
                m_saveTimers[s]->stop();
                save(s);
            }
        m_onGame = i == 1;
        load();
    });
    connect(m_mode, &QComboBox::currentIndexChanged, this, [this](int) {
        if (m_loading) return;
        QString m = m_mode->currentData().toString();
        if (m_onGame) m_gameMode = m;
        else m_globalMode = m;
        QString shown = m_onGame && m.isEmpty() ? m_globalMode : m;
        m_tabs->setCurrentIndex(tabOf(shown));
    });

    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, this, &PictureDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &PictureDialog::reject);
    v->addWidget(bb);
    load();
}

QString PictureDialog::file(int s, bool game) const {
    return (game ? picture::configDir(m_game) : picture::configDir()) + "/" + m_shaders[s].name + ".params";
}

QMap<QString, double> PictureDialog::inherited(int s) const {
    QMap<QString, double> v = m_shaders[s].base;
    if (m_onGame) {
        auto g = readValues(file(s, false));
        for (auto it = g.begin(); it != g.end(); ++it)
            if (v.contains(it.key())) v[it.key()] = it.value();
    }
    return v;
}

void PictureDialog::buildPage(int s) {
    Shader &sh = m_shaders[s];
    sh.page = new QWidget;
    auto *v = new QVBoxLayout(sh.page);
    auto *bar = new QHBoxLayout;
    auto *filter = new QLineEdit;
    filter->setPlaceholderText(tr("Find a setting"));
    filter->setClearButtonEnabled(true);
    bar->addWidget(filter, 1);
    auto *resetAll = new QPushButton(tr("Reset all"));
    resetAll->setAutoDefault(false);
    bar->addWidget(resetAll);
    v->addLayout(bar);
    if (!sh.looks.isEmpty()) {   // a look sets all its values at once; the sliders then fine-tune it
        auto *lr = new QHBoxLayout;
        lr->addWidget(new QLabel(tr("Look:")));   // TR: a shader's preset (a tube, a cable and a set-up)
        sh.look = new QComboBox;
        sh.look->addItem(tr("Own settings"), -1);
        for (int k = 0; k < sh.looks.size(); k++) sh.look->addItem(sh.looks[k].title, k);
        sh.look->setToolTip(tr("The shader's presets: each a tube, a cable and a set-up. Curvature and the board's timing stay as they are."));
        lr->addWidget(sh.look, 1);
        v->addLayout(lr);
        connect(sh.look, &QComboBox::currentIndexChanged, this, [this, s](int) {
            if (m_loading) return;
            Shader &sh = m_shaders[s];
            int k = sh.look->currentData().toInt();
            if (k < 0) return;
            for (int i = 0; i < sh.params.size(); i++)
                if (sh.looks[k].values.contains(sh.params[i].name)) setValue(s, i, sh.looks[k].values[sh.params[i].name], false);
        });
    }

    auto *inner = new QWidget;
    auto *g = new QGridLayout(inner);
    g->setColumnStretch(1, 1);
    int r = 0;
    QString section;
    QVector<QLabel *> headers;
    QVector<QVector<QWidget *>> rowWidgets;
    for (int i = 0; i < sh.params.size(); i++) {
        const Param &p = sh.params[i];
        if (p.section != section) {
            section = p.section;
            auto *h = new QLabel("<b>" + section.toHtmlEscaped() + "</b>");
            h->setContentsMargins(0, r ? 10 : 0, 0, 2);
            g->addWidget(h, r++, 0, 1, 4);
            headers.push_back(h);
        }
        Row row;
        row.label = new QLabel(p.label);
        row.label->setToolTip(p.name);
        row.slider = new QSlider(Qt::Horizontal);
        double step = p.step > 0 ? p.step : (p.max - p.min) / 100.0;
        row.slider->setRange(0, std::max(1, (int)std::lround((p.max - p.min) / step)));
        row.spin = new QDoubleSpinBox;
        row.spin->setDecimals(decimals(step));
        row.spin->setRange(std::min(p.min, p.max), std::max(p.min, p.max));
        row.spin->setSingleStep(step);
        row.spin->setMinimumWidth(80);
        row.reset = new QToolButton;
        row.reset->setIcon(theme::icon(mdi::undo));
        row.reset->setText("↺");
        row.reset->setAutoRaise(true);
        g->addWidget(row.label, r, 0);
        g->addWidget(row.slider, r, 1);
        g->addWidget(row.spin, r, 2);
        g->addWidget(row.reset, r, 3);
        r++;
        connect(row.slider, &QSlider::valueChanged, this, [this, s, i, step, min = p.min](int pos) {
            if (!m_loading) setValue(s, i, min + pos * step, true);
        });
        connect(row.spin, &QDoubleSpinBox::valueChanged, this, [this, s, i](double x) {
            if (!m_loading) setValue(s, i, x, false);
        });
        connect(row.reset, &QToolButton::clicked, this, [this, s, i] {
            setValue(s, i, inherited(s).value(m_shaders[s].params[i].name), false);
        });
        m_rows[s].push_back(row);
        rowWidgets.push_back({row.label, row.slider, row.spin, row.reset});
    }
    g->setRowStretch(r, 1);
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setWidget(inner);
    v->addWidget(scroll, 1);

    connect(filter, &QLineEdit::textChanged, this, [this, s, headers, rowWidgets](const QString &t) {
        for (int i = 0; i < rowWidgets.size(); i++) {
            const Param &p = m_shaders[s].params[i];
            bool show = t.isEmpty() || p.label.contains(t, Qt::CaseInsensitive) || p.name.contains(t, Qt::CaseInsensitive) ||
                        p.section.contains(t, Qt::CaseInsensitive);
            for (QWidget *w : rowWidgets[i]) w->setVisible(show);
        }
        for (QLabel *h : headers) h->setVisible(t.isEmpty());
    });
    connect(resetAll, &QPushButton::clicked, this, [this, s] {
        auto inh = inherited(s);
        for (int i = 0; i < m_shaders[s].params.size(); i++) setValue(s, i, inh.value(m_shaders[s].params[i].name), false);
    });
}

void PictureDialog::setValue(int s, int i, double v, bool fromSlider) {
    const Param &p = m_shaders[s].params[i];
    Row &row = m_rows[s][i];
    m_values[s][p.name] = v;
    m_loading = true;
    if (!fromSlider) {
        double step = p.step > 0 ? p.step : (p.max - p.min) / 100.0;
        row.slider->setValue((int)std::lround((v - p.min) / step));
    }
    row.spin->setValue(v);
    m_loading = false;
    double inh = inherited(s).value(p.name);
    QFont f = row.label->font();
    f.setBold(!same(v, inh));
    row.label->setFont(f);
    row.reset->setEnabled(!same(v, inh));
    // TR: %1 is the value it goes back to
    row.reset->setToolTip((m_onGame ? tr("Back to %1 (all games)") : tr("Back to %1 (the default)")).arg(QLocale().toString(inh, 'g', 6)));
    showLook(s);
    m_saveTimers[s]->start();
}

// The look the values are, or "Own settings"
void PictureDialog::showLook(int s) {
    Shader &sh = m_shaders[s];
    if (!sh.look) return;
    int found = -1;
    for (int k = 0; k < sh.looks.size() && found < 0; k++) {
        bool all = true;
        for (auto it = sh.looks[k].values.begin(); it != sh.looks[k].values.end() && all; ++it)
            all = !m_values[s].contains(it.key()) || same(m_values[s][it.key()], it.value());
        if (all) found = k;
    }
    const bool was = m_loading;
    m_loading = true;
    sh.look->setCurrentIndex(std::max(0, sh.look->findData(found)));
    m_loading = was;
}

void PictureDialog::load() {
    m_loading = true;
    m_mode->clear();
    if (m_onGame) m_mode->addItem(tr("Same as all games (%1)").arg(modeLabel(m_globalMode)), QString());   // TR: %1 is a picture mode, "Off: sharp pixels"
    for (auto &x : MODES) m_mode->addItem(modeText(x[1]), QString(x[0]));
    QString cur = m_onGame ? m_gameMode : m_globalMode;
    m_mode->setCurrentIndex(std::max(0, m_mode->findData(cur)));
    QString shown = m_onGame && cur.isEmpty() ? m_globalMode : cur;
    m_tabs->setCurrentIndex(tabOf(shown));
    m_loading = false;
    for (int s = 0; s < m_shaders.size(); s++) {
        auto inh = inherited(s);
        auto own = readValues(file(s, m_onGame));
        m_values[s] = inh;
        for (auto it = own.begin(); it != own.end(); ++it)
            if (inh.contains(it.key())) m_values[s][it.key()] = it.value();
        for (int i = 0; i < m_shaders[s].params.size(); i++) setValue(s, i, m_values[s][m_shaders[s].params[i].name], false);
        m_saveTimers[s]->stop();   // loading is not a change
    }
}

void PictureDialog::save(int s) {
    QString path = file(s, m_onGame);
    auto inh = inherited(s);
    QString out;
    for (const Param &p : m_shaders[s].params) {
        double v = m_values[s].value(p.name, inh.value(p.name));
        if (!same(v, inh.value(p.name))) out += QString("%1 = %2\n").arg(p.name).arg(v, 0, 'g', 8);
    }
    // keep values this dialog doesn't show (written by hand) as they are
    auto prev = readValues(path);
    for (auto it = prev.begin(); it != prev.end(); ++it)
        if (!inh.contains(it.key())) out += QString("%1 = %2\n").arg(it.key()).arg(it.value(), 0, 'g', 8);
    if (out.isEmpty()) {
        QFile::remove(path);
        return;
    }
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) return;
    f.write(("# " + m_shaders[s].name + ", " + (m_onGame ? m_gameTitle : QString("all games")) +
             ": what differs from " + (m_onGame ? "all games" : "the defaults") + " (HIRAKU launcher, Picture)\n" + out)
                .toUtf8());
}

void PictureDialog::accept() {
    for (int s = 0; s < m_shaders.size(); s++)
        if (m_saveTimers[s]->isActive()) {
            m_saveTimers[s]->stop();
            save(s);
        }
    auto &st = cfg::settings();
    st.setValue("video/picture", m_globalMode);
    if (!m_game.isEmpty()) {
        if (m_gameMode.isEmpty()) st.remove("picture/" + m_game);
        else st.setValue("picture/" + m_game, m_gameMode);
    }
    QDialog::accept();
}

void PictureDialog::reject() {
    for (auto *t : m_saveTimers) t->stop();
    for (auto it = m_backup.begin(); it != m_backup.end(); ++it) {
        if (!m_existed[it.key()]) {
            QFile::remove(it.key());
            continue;
        }
        QFile f(it.key());
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(it.value());
    }
    QDialog::reject();
}
