// SPDX-License-Identifier: GPL-3.0-or-later
#include "controlsdialog.h"

#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QPainter>
#include <QRegularExpression>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyle>
#include <QStandardItemModel>
#include <QToolButton>
#include <QVBoxLayout>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <functional>

#include "capturedialog.h"
#include "config.h"
#include "inputs.h"

namespace {

QString hatDir(int bits) {
    if (bits & SDL_HAT_UP) return "up";
    if (bits & SDL_HAT_DOWN) return "down";
    if (bits & SDL_HAT_LEFT) return "left";
    if (bits & SDL_HAT_RIGHT) return "right";
    return {};
}

// Players by input name: "P1 ..." and "1 Player Start" are player 1's; others (coin, test) shared
int playerOf(const QString &name) {
    if (name.startsWith("P1 ") || name == "1 Player Start") return 1;
    if (name.startsWith("P2 ") || name == "2 Players Start") return 2;
    return 0;
}

// The same input for the other player ("P1 Button 3" -> "P2 Button 3"); shared ones stay
QString asPlayer(const QString &name, int player) {
    int now = playerOf(name);
    if (player == 0 || now == 0 || now == player) return name;
    if (name.endsWith("Start")) return player == 1 ? "1 Player Start" : "2 Players Start";
    return QString("P%1").arg(player) + name.mid(2);
}

// A control's default bindings, one item each ("key:Z", "joy0:button0")
std::vector<std::string> defaultItems(const hk::cab::Control &c) { return hk::cab::parse_list_file(c.name + " = " + c.defaults)[0].items; }

// The controller (or "key") an input comes from, the same whichever way a binding names it
QString padOf(const QString &b) { return bindingPad(canonicalBinding(b)); }

bool isPadNumber(const QString &pad) {
    static const QRegularExpression re(R"(^joy\d+$)");
    return re.match(pad).hasMatch();
}

}  // namespace

// An analog control's axis turning as the game will see it (its speed and direction applied):
// 1024 runtime counts, 16384 axis units at speed 1, make a turn (runtime/host/input.cpp)
class TurntableMeter : public QWidget {
public:
    explicit TurntableMeter(std::function<QString()> binding, QWidget *parent = nullptr)
        : QWidget(parent), m_binding(std::move(binding)) {
        setFixedSize(22, 22);
        setToolTip("The turntable as the game will see it turn: spin it to check the speed and direction");
        connect(&Joysticks::instance(), &Joysticks::axis, this, [this](int j, int a, int v) { onAxis(j, a, v); });
        m_fade.setSingleShot(true);
        connect(&m_fade, &QTimer::timeout, this, [this] {
            m_lit = false;
            update();
        });
    }
    bool hasAxis() const { return !m_binding().isEmpty(); }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QRectF r = QRectF(rect()).adjusted(2, 2, -2, -2);
        p.setPen(QPen(palette().color(QPalette::Mid), 1.5));
        p.setBrush(palette().color(m_lit ? QPalette::Highlight : QPalette::Base));
        p.drawEllipse(r);
        p.translate(r.center());
        p.rotate(m_angle);
        p.setPen(QPen(palette().color(m_lit ? QPalette::HighlightedText : QPalette::Text), 2, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(0, 0), QPointF(0, -r.height() / 2 + 2));
    }
private:
    void onAxis(int j, int a, int v) {
        QString b = m_binding();
        QString here = QString("joy%1:axis%2").arg(j).arg(a);
        if (b.isEmpty() || canonicalBinding(b) != here) return;
        if (m_last.contains(here)) {
            int d = v - m_last[here];
            if (d > 32768) d -= 65536;
            if (d < -32768) d += 65536;
            m_angle = std::fmod(m_angle + d / 16384.0 * 360.0 * axisScale(b), 360.0);
        }
        m_last[here] = v;
        m_lit = true;
        m_fade.start(250);
        update();
    }
    std::function<QString()> m_binding;   // the axis binding shown ("" = none plugged in)
    QHash<QString, int> m_last;
    double m_angle = 0;
    bool m_lit = false;
    QTimer m_fade;
};

namespace {

// The controls of a set: each member family's, once, in the order they first come. One that only
// some members have says which, in its label, or for a whole group of them (Python's 10-key) in
// the group's title.
const std::vector<hk::cab::Control> &setControls(const QString &set) {
    static QHash<QString, std::vector<hk::cab::Control>> cache;
    if (auto it = cache.constFind(set); it != cache.constEnd()) return *it;
    QStringList members = cfg::controlsSetMembers(set);
    std::vector<hk::cab::Control> out;
    QHash<QString, QStringList> has;   // control -> the members that have it
    for (const QString &m : members)
        for (auto &c : hk::cab::controls(m.toStdString())) {
            QString n = QString::fromStdString(c.name);
            if (!has.contains(n)) out.push_back(c);
            has[n] << m;
        }
    QHash<QString, QSet<QString>> groupHas;   // group -> its controls' member lists
    for (auto &c : out) groupHas[QString::fromStdString(c.group)] << has[QString::fromStdString(c.name)].join(",");
    for (auto &c : out) {
        const QStringList &h = has[QString::fromStdString(c.name)];
        if (h.size() == members.size()) continue;
        QStringList boards;
        for (const QString &m : h) boards << cfg::controlsMemberName(m);
        std::string only = " (" + boards.join(", ").toStdString() + " only)";
        if (groupHas[QString::fromStdString(c.group)].size() == 1) c.group += only;
        else c.label += only;
    }
    return cache[set] = out;
}

}  // namespace

void fillCabinetBox(QComboBox *box, bool controlSets) {
    auto *model = new QStandardItemModel(box);
    QFont bold = box->font();
    bold.setBold(true);
    for (auto &[series, fams] : controlSets ? cfg::controlsSeries() : cfg::familySeries()) {
        if (!series.isEmpty()) {
            auto *header = new QStandardItem(series);
            header->setFlags(Qt::NoItemFlags);   // not a choice
            header->setFont(bold);
            model->appendRow(header);
        }
        for (const QString &f : fams) {
            auto *it = new QStandardItem(controlSets ? cfg::controlsSetName(f) : cfg::familyName(f));
            it->setData(f, Qt::UserRole);
            model->appendRow(it);
        }
    }
    box->setModel(model);
    box->setMaxVisibleItems(model->rowCount());
}

ControlsDialog::ControlsDialog(const QString &family, QWidget *parent) : QDialog(parent) {
    setWindowTitle("Controls");
    resize(640, 720);
    auto *v = new QVBoxLayout(this);

    auto *top = new QHBoxLayout;
    top->addWidget(new QLabel("Cabinet:"));
    m_familyBox = new QComboBox;
    fillCabinetBox(m_familyBox, true);
    top->addWidget(m_familyBox, 1);
    v->addLayout(top);
    m_setLabel = new QLabel;
    m_setLabel->setWordWrap(true);
    v->addWidget(m_setLabel);

    m_joyLabel = new QLabel;
    m_joyLabel->setWordWrap(true);
    m_joyLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    v->addWidget(m_joyLabel);
    for (const QString &pad : cfg::settings().value("controls/notNow").toStringList()) m_hidden.insert(pad);
    m_banner = new QLabel;
    m_banner->setWordWrap(true);
    m_banner->setFrameShape(QFrame::StyledPanel);
    m_banner->setMargin(6);
    m_banner->setTextFormat(Qt::RichText);
    connect(m_banner, &QLabel::linkActivated, this, [this](const QString &link) {
        // "walk:<player>", "default:<player>:<joy>", "saved:<player>:<joy>", "hide:<joy>", "tie"
        QStringList p = link.split(':');
        auto &joys = Joysticks::instance();
        QString pad = p.size() > 2 ? joys.pad(p[2].toInt()) : QString();
        if (p[0] == "walk") bindAll(p[1].toInt());
        else if (p[0] == "default" && !pad.isEmpty()) applyLayout(pad, p[1].toInt(), defaultLayout());
        else if (p[0] == "saved" && !pad.isEmpty()) applyLayout(pad, p[1].toInt(), savedLayout(pad));
        else if (p[0] == "hide") {
            m_hidden.insert(joys.pad(p[1].toInt()));
            cfg::settings().setValue("controls/notNow", QStringList(m_hidden.begin(), m_hidden.end()));
        }
        else if (p[0] == "tie") tieToControllers();
        changed();
    });
    v->addWidget(m_banner);
    auto *hint = new QLabel("Click a binding, then press a key or a controller button: it replaces that keyboard's or "
                            "controller's binding there. Its <b>▾</b> menu (or a right-click) adds another, removes one, "
                            "resets it, or sets a turntable's speed. A warning sign marks an input that presses two "
                            "controls in one game. Press anything and its name lights up.");
    hint->setWordWrap(true);
    v->addWidget(hint);
    m_lastLabel = new QLabel("Last input: —");
    m_lastLabel->setTextFormat(Qt::RichText);
    v->addWidget(m_lastLabel);

    m_scroll = new QScrollArea;
    m_scroll->setWidgetResizable(true);
    v->addWidget(m_scroll, 1);

    auto *box = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel | QDialogButtonBox::RestoreDefaults);
    auto *setup = new QPushButton("Set up");
    auto *setupMenuBox = new QMenu(setup);
    connect(setupMenuBox, &QMenu::aboutToShow, this, [this, setupMenuBox] {
        setupMenuBox->clear();
        setupMenu(setupMenuBox);
    });
    setup->setMenu(setupMenuBox);
    setup->setToolTip("Bind everything in turn, a controller's layout (the default, or one you saved), copying "
                      "player 1's controller to player 2");
    box->addButton(setup, QDialogButtonBox::ResetRole);
    auto *tryIt = new QPushButton("Try in a game");
    auto *tryMenu = new QMenu(tryIt);
    connect(tryMenu, &QMenu::aboutToShow, this, [this, tryMenu] {
        tryMenu->clear();
        QList<Game> ready;
        for (const Game &g : m_games)
            if (g.status == Game::Ready && cfg::controlsSet(g.family) == m_family) ready << g;
        std::sort(ready.begin(), ready.end(), [](const Game &a, const Game &b) { return a.title < b.title; });
        if (ready.isEmpty()) tryMenu->addAction("No " + cfg::controlsSetName(m_family) + " game is ready to play")->setEnabled(false);
        for (const Game &g : ready)
            tryMenu->addAction(familyIcon(g.family), g.title, this, [this, id = g.id] {
                if (!save()) return;
                m_tryGame = id;
                accept();
            });
    });
    tryIt->setMenu(tryMenu);
    tryIt->setToolTip("Save, and start a game with these controls: its test menu's I/O check shows each input as it is pressed");
    box->addButton(tryIt, QDialogButtonBox::ResetRole);
    for (auto *ab : box->buttons()) {
        auto *b = qobject_cast<QPushButton *>(ab);
        b->setAutoDefault(false);
        b->setDefault(false);
        b->setFocusPolicy(Qt::NoFocus);   // keys pressed to test bindings must not click them
    }
    v->addWidget(box);
    connect(box, &QDialogButtonBox::accepted, this, [this] {
        if (save()) accept();
    });
    connect(box, &QDialogButtonBox::rejected, this, &ControlsDialog::reject);
    connect(box->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, this, [this] {
        Bindings d;
        for (auto &c : setControls(m_family)) {
            QStringList l;
            auto entries = hk::cab::parse_list_file(c.name + " = " + c.defaults);
            for (auto &it : entries[0].items) l << normalizeBinding(QString::fromStdString(it));
            d[QString::fromStdString(c.name)] = l;
        }
        m_edits[m_family] = d;
        m_dirty.insert(m_family);
        changed();
    });

    connect(m_familyBox, &QComboBox::currentIndexChanged, this, [this] { showFamily(m_familyBox->currentData().toString()); });
    int idx = m_familyBox->findData(cfg::controlsSet(family));
    m_familyBox->setCurrentIndex(idx < 0 ? m_familyBox->findData(cfg::controlsSet("bm3")) : idx);
    showFamily(m_familyBox->currentData().toString());

    // live feedback
    auto &joys = Joysticks::instance();
    connect(&joys, &Joysticks::devicesChanged, this, [this] {
        updateJoystickList();
        changed();   // named bindings now find (or lose) their controllers
    });
    connect(&joys, &Joysticks::button, this, [this](int j, int b, bool down) { source(QString("joy%1:button%2").arg(j).arg(b), down); });
    connect(&joys, &Joysticks::hat, this, [this](int j, int h, int val) {
        QString base = QString("joy%1:hat%2").arg(j).arg(h);
        for (int bit : {SDL_HAT_UP, SDL_HAT_DOWN, SDL_HAT_LEFT, SDL_HAT_RIGHT}) source(base + ":" + hatDir(bit), val & bit);
    });
    connect(&joys, &Joysticks::axis, this, [this](int j, int a, int val) {
        QString base = QString("joy%1:axis%2").arg(j).arg(a);
        // as the game reads it (runtime/host/input.cpp): an axis whose first report is past a
        // threshold rests there, and isn't pressed that way until it has come back
        if (!Joysticks::instance().axisKnown(j, a)) {   // its first report (axisKnown turns after this signal)
            if (val < -16384) m_restNeg << base;
            if (val > 16384) m_restPos << base;
        }
        if (val >= -16384) m_restNeg.remove(base);
        if (val <= 16384) m_restPos.remove(base);
        source(base + "+", val > 16384 && !m_restPos.contains(base));
        source(base + "-", val < -16384 && !m_restNeg.contains(base));
        m_flashUntil[base] = m_clock.elapsed() + 200;
        source(base, true);
    });
    m_clock.start();
    connect(&m_flashTimer, &QTimer::timeout, this, [this] {
        qint64 now = m_clock.elapsed();
        for (auto it = m_flashUntil.begin(); it != m_flashUntil.end();)
            if (it.value() < now) {
                m_active.remove(it.key());
                it = m_flashUntil.erase(it);
                refreshHighlights();
            } else {
                ++it;
            }
    });
    m_flashTimer.start(50);
    updateJoystickList();
    qApp->installEventFilter(this);
}

ControlsDialog::Bindings ControlsDialog::load(const QString &family) const {
    std::string text;
    for (auto &c : setControls(family))
        if (!c.defaults.empty()) text += c.name + " = " + c.defaults + "\n";
    QFile f(cfg::controlsFile(family));
    if (f.open(QIODevice::ReadOnly)) text += f.readAll().toStdString();   // the file's lines replace the defaults
    Bindings b;
    for (auto &c : setControls(family)) b[QString::fromStdString(c.name)] = {};
    for (auto &en : hk::cab::parse_list_file(text)) {
        QStringList l;
        for (auto &it : en.items) l << normalizeBinding(QString::fromStdString(it));
        b[QString::fromStdString(en.name)] = l;
    }
    return b;
}

bool ControlsDialog::save() {
    for (const QString &fam : m_dirty) {
        QDir().mkpath(cfg::configDir() + "/controls");
        QSaveFile f(cfg::controlsFile(fam));
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QMessageBox::warning(this, "Controls", "Cannot write " + f.fileName() + ": " + f.errorString());
            return false;
        }
        QString text = "# HIRAKU controls for " + cfg::controlsSetName(fam) + ", written by the launcher.\n"
                       "# Format: runtime/host/input.h. An input with nothing after '=' is unbound.\n";
        const Bindings &b = m_edits[fam];
        for (auto &c : setControls(fam)) {
            std::vector<std::string> items;
            for (const QString &s : b.value(QString::fromStdString(c.name))) items.push_back(s.toStdString());
            text += QString::fromStdString(hk::cab::format_entry(c.name, items)) + "\n";
        }
        f.write(text.toUtf8());
        if (!f.commit()) {
            QMessageBox::warning(this, "Controls", "Cannot write " + f.fileName() + ": " + f.errorString());
            return false;
        }
    }
    m_dirty.clear();
    return true;
}

void ControlsDialog::reject() {
    if (!m_dirty.isEmpty()) {
        auto r = QMessageBox::question(this, "Controls", "Save the changed controls?",
                                       QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
        if (r == QMessageBox::Cancel) return;
        if (r == QMessageBox::Save && !save()) return;
    }
    QDialog::reject();
}

void ControlsDialog::showFamily(const QString &family) {
    m_family = family;
    if (!m_edits.contains(family)) m_edits[family] = load(family);
    m_rows.clear();
    m_sections.clear();
    auto &defs = setControls(family);
    QStringList sharing = cfg::controlsSetMembers(family);
    m_setLabel->setVisible(sharing.size() > 1);
    if (sharing.size() > 1) {
        QStringList names;
        for (const QString &m : sharing) names << cfg::familyName(m);
        QString last = names.takeLast();
        m_setLabel->setText("Shared by " + names.join(", ") + " and " + last +
                            ": each game uses the controls its cabinet has and ignores the rest.");
    }
    // one section per group, in the order the groups first come (IIDX lists "Cabinet" twice)
    QStringList groups;
    QHash<QString, QVector<int>> members;
    for (int k = 0; k < (int)defs.size(); k++) {
        QString g = QString::fromStdString(defs[k].group);
        if (!members.contains(g)) groups << g;
        members[g] << k;
    }
    // Two columns, each section to the shorter one: Player 1 beside Player 2
    QHash<QString, bool> folded;
    QHash<QString, int> column;
    int rows[2] = {0, 0}, labelWidth[2] = {0, 0};   // one width per column, so its bindings line up
    for (const QString &g : groups) {
        folded[g] = cfg::settings().value("controls/folded/" + g, g.startsWith("e-AMUSEMENT")).toBool();
        int c = column[g] = rows[1] < rows[0] ? 1 : 0;
        rows[c] += 2 + (folded[g] ? 0 : members[g].size());
        for (int k : members[g])
            labelWidth[c] = std::max(labelWidth[c], fontMetrics().horizontalAdvance(QString::fromStdString(defs[k].label)) + 16);
    }

    auto *page = new QWidget;
    auto *cols = new QHBoxLayout(page);
    cols->setSpacing(24);
    QVBoxLayout *col[2];
    for (auto *&c : col) {
        c = new QVBoxLayout;
        c->setSpacing(4);
        cols->addLayout(c, 1);
    }
    QFont bold = font();
    bold.setBold(true);
    for (const QString &g : groups) {
        int si = m_sections.size();
        Section sec;
        sec.group = g;
        sec.header = new QToolButton;
        sec.header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        sec.header->setAutoRaise(true);
        sec.header->setFont(bold);
        sec.header->setFocusPolicy(Qt::NoFocus);
        connect(sec.header, &QToolButton::clicked, this, [this, si] {
            bool fold = !m_sections[si].body->isHidden();
            setFolded(si, fold);
            cfg::settings().setValue("controls/folded/" + m_sections[si].group, fold);
        });
        auto *line = new QFrame;
        line->setFrameShape(QFrame::HLine);
        line->setFrameShadow(QFrame::Sunken);
        auto *head = new QHBoxLayout;
        head->addWidget(sec.header);
        head->addWidget(line, 1);
        sec.body = new QWidget;
        auto *grid = new QGridLayout(sec.body);
        grid->setContentsMargins(8, 0, 0, 8);
        grid->setVerticalSpacing(3);
        grid->setColumnStretch(1, 1);

        int r = 0;
        for (int k : members[g]) {
            const auto &c = defs[k];
            int index = m_rows.size();
            Row row;
            row.control = c;
            row.section = si;
            row.label = new QLabel(QString::fromStdString(c.label));
            row.label->setToolTip(QString::fromStdString(c.name));
            row.label->setMinimumWidth(labelWidth[column[g]]);
            row.button = new QToolButton;
            row.button->setFocusPolicy(Qt::NoFocus);
            row.button->setPopupMode(QToolButton::MenuButtonPopup);
            row.button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            row.button->setMinimumWidth(180);
            row.button->setContextMenuPolicy(Qt::CustomContextMenu);
            auto *menu = new QMenu(row.button);
            connect(menu, &QMenu::aboutToShow, this, [this, index, menu] {
                menu->clear();
                bindingMenu(index, menu);
            });
            row.button->setMenu(menu);
            connect(row.button, &QToolButton::clicked, this, [this, index] { capture(index, false); });
            connect(row.button, &QToolButton::customContextMenuRequested, this, [this, index](const QPoint &) {
                QMenu m;
                bindingMenu(index, &m);
                m.exec(QCursor::pos());
            });
            grid->addWidget(row.label, r, 0);
            grid->addWidget(row.button, r, 1);
            if (c.analog && c.name.find("Turntable") != std::string::npos) {
                // the first of its axes on a controller plugged in (the speed applies to turntables only)
                row.meter = new TurntableMeter([this, index] {
                    for (const QString &b : bindingsOf(index))
                        if (isPadNumber(padOf(b)) && Joysticks::instance().indexOf(padOf(b)) >= 0 && bindingRest(b).startsWith("axis"))
                            return b;
                    return QString();
                });
                QSizePolicy keep = row.meter->sizePolicy();
                keep.setRetainSizeWhenHidden(true);   // the bindings stay put as one comes and goes
                row.meter->setSizePolicy(keep);
                grid->addWidget(row.meter, r, 2);
            }
            r++;
            m_rows.push_back(row);
        }

        auto *box = new QVBoxLayout;
        box->setSpacing(2);
        box->addLayout(head);
        box->addWidget(sec.body);
        m_sections.push_back(sec);
        setFolded(si, folded[g]);
        col[column[g]]->addLayout(box);
    }
    for (auto *c : col) c->addStretch(1);
    m_scroll->setWidget(page);
    changed();

    // wide enough for both columns, as far as the screen allows (measured with the labels' style applied)
    page->ensurePolished();
    for (auto &row : m_rows) row.label->ensurePolished();
    page->layout()->activate();
    int want = page->sizeHint().width() + m_scroll->verticalScrollBar()->sizeHint().width() + 2 * m_scroll->frameWidth() +
               layout()->contentsMargins().left() + layout()->contentsMargins().right();
    if (screen()) want = std::min(want, screen()->availableGeometry().width());
    if (width() < want) resize(want, height());
}

void ControlsDialog::setFolded(int si, bool folded) {
    Section &sec = m_sections[si];
    sec.body->setVisible(!folded);
    sec.header->setArrowType(folded ? Qt::RightArrow : Qt::DownArrow);
    int n = 0;
    for (auto &row : m_rows) n += row.section == si;
    sec.header->setText(folded ? QString("%1  (%2)").arg(sec.group).arg(n) : sec.group);
    sec.header->setToolTip(folded ? "Show these controls" : "Fold these controls away");
}

void ControlsDialog::bindingMenu(int k, QMenu *menu) {
    QString name = QString::fromStdString(m_rows[k].control.name);
    // queued: the binding dialog opens once the menu has closed
    menu->addAction("Add another binding…", this, [this, k] {
        QMetaObject::invokeMethod(this, [this, k] { capture(k, true); }, Qt::QueuedConnection);
    });
    const QStringList l = m_edits[m_family].value(name);
    if (!l.isEmpty()) menu->addSeparator();
    for (const QString &b : l)
        menu->addAction("Remove " + describeBinding(b), this, [this, name, b] {
            m_edits[m_family][name].removeAll(b);
            m_dirty.insert(m_family);
            changed();
        });
    if (l.size() > 1)
        menu->addAction("Remove all", this, [this, name] {
            m_edits[m_family][name].clear();
            m_dirty.insert(m_family);
            changed();
        });
    const auto &c = m_rows[k].control;
    QStringList defaults;
    for (auto &it : defaultItems(c)) defaults << normalizeBinding(QString::fromStdString(it));
    QStringList shown;
    for (const QString &b : defaults) shown << describeBinding(b);
    menu->addSeparator();
    auto *reset = menu->addAction("Reset to default (" + (shown.isEmpty() ? QString("unbound") : shown.join(", ")) + ")", this,
                                  [this, name, defaults] {
                                      m_edits[m_family][name] = defaults;
                                      m_dirty.insert(m_family);
                                      changed();
                                  });
    reset->setEnabled(l != defaults);

    // a turntable turned by buttons (Turntable - / +): how fast while one is held, for its buttons, hat
    // directions and axis directions (a digital turntable, as a PS2 controller's, holds a button while
    // it turns: at the keys' speed a short turn went past several songs); keys stay as they are
    if (!c.analog && c.name.find("Turntable") != std::string::npos) {
        auto held = [](const QString &b) { return !b.startsWith("key:"); };
        QStringList pads;
        for (const QString &b : l)
            if (held(b)) pads << b;
        if (pads.isEmpty()) return;
        const double scale = axisScale(pads[0]);
        auto *speed = menu->addMenu("Speed (buttons)");
        for (double x : {0.1, 0.2, 0.3, 0.5, 0.75, 1.0, 1.5, 2.0}) {
            auto *act = speed->addAction(x == 1.0 ? QString("×1 (as keys turn it)") : QString("×%1").arg(x), this, [this, name, held, x] {
                for (QString &b : m_edits[m_family][name])
                    if (held(b)) b = withAxisScale(b, x);
                m_dirty.insert(m_family);
                changed();
            });
            act->setCheckable(true);
            act->setChecked(std::abs(scale - x) < 1e-6);
        }
        return;
    }
    // a turntable's speed and direction, for each of its axes
    QStringList axes;
    for (const QString &b : l)
        if (bindingRest(b).startsWith("axis") && !bindingRest(b).endsWith('+') && !bindingRest(b).endsWith('-')) axes << b;
    if (!c.analog || c.name.find("Turntable") == std::string::npos || axes.isEmpty()) return;
    double scale = axisScale(axes[0]);
    auto setScale = [this, name](double s) {
        for (QString &b : m_edits[m_family][name])
            if (bindingRest(b).startsWith("axis") && !bindingRest(b).endsWith('+') && !bindingRest(b).endsWith('-')) b = withAxisScale(b, s);
        m_dirty.insert(m_family);
        changed();
    };
    menu->addSeparator();
    auto *rev = menu->addAction("Reverse direction", this, [setScale, scale] { setScale(-scale); });
    rev->setCheckable(true);
    rev->setChecked(scale < 0);
    auto *speed = menu->addMenu("Speed");
    for (double x : {0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0}) {
        auto *a = speed->addAction(x == 1.0 ? QString("×1 (as the controller sends it)") : QString("×%1").arg(x), this,
                                   [setScale, scale, x] { setScale(scale < 0 ? -x : x); });
        a->setCheckable(true);
        a->setChecked(std::abs(std::abs(scale) - x) < 1e-6);
    }
}

void ControlsDialog::refreshRows() {
    // An input bound to two controls of one game presses both there: each says which, per game
    QHash<int, QStringList> clashes;
    for (const QString &m : cfg::controlsSetMembers(m_family)) {
        QSet<std::string> has;
        for (auto &c : hk::cab::controls(m.toStdString())) has.insert(c.name);
        QMap<QString, QVector<int>> users;   // binding -> its rows
        for (int k = 0; k < m_rows.size(); k++)
            if (has.contains(m_rows[k].control.name))
                for (const QString &b : m_edits[m_family].value(QString::fromStdString(m_rows[k].control.name)))
                    users[canonicalBinding(b)] << k;
        for (auto it = users.cbegin(); it != users.cend(); ++it)
            for (int k : *it)
                for (int j : *it)
                    if (j != k) {
                        QString where = cfg::controlsSetMembers(m_family).size() > 1 ? " in " + cfg::familyName(m) : QString();
                        clashes[k] << describeBinding(it.key()) + " also presses " + m_rows[j].label->text() + where;
                    }
    }
    for (int k = 0; k < m_rows.size(); k++) {
        Row &row = m_rows[k];
        const QStringList &l = m_edits[m_family].value(QString::fromStdString(row.control.name));
        QStringList shown;
        for (const QString &b : l) shown << describeBinding(b);
        row.button->setText(shown.isEmpty() ? "—" : shown.join(", "));
        QString tip = l.isEmpty() ? QString("Not bound") : l.join(", ");
        if (clashes.contains(k)) tip += "\n\n" + clashes[k].join("\n");
        row.button->setToolTip(tip + "\n\nClick to bind; the arrow (or a right-click) adds or removes one binding.");
        row.button->setToolButtonStyle(clashes.contains(k) ? Qt::ToolButtonTextBesideIcon : Qt::ToolButtonTextOnly);
        row.button->setIcon(clashes.contains(k) ? style()->standardIcon(QStyle::SP_MessageBoxWarning) : QIcon());
        if (row.meter) row.meter->setVisible(row.meter->hasAxis());
    }
}

void ControlsDialog::capture(int k, bool append) {
    const auto &c = m_rows[k].control;
    m_capturing = true;
    CaptureDialog d(QString::fromStdString(c.label) + "  (" + QString::fromStdString(c.group) + ")", c.analog, this);
    int r = d.exec();
    m_capturing = false;
    m_active.clear();   // the capturing press never reached the highlighter: don't leave it stuck
    if (r == CaptureDialog::Captured && !d.result().isEmpty()) bind(k, d.result(), append, false);
    changed();
}

void ControlsDialog::source(const QString &binding, bool on) {
    if (m_capturing) return;
    bool was = m_active.contains(binding);
    if (on == was) return;
    if (on) m_active.insert(binding);
    else m_active.remove(binding);
    if (on) {
        // by the controller's name, and what it presses here
        QString shown = binding, pad = bindingPad(binding);
        if (QString named = Joysticks::instance().pad(Joysticks::instance().indexOf(pad)); isPadNumber(pad) && !named.isEmpty())
            shown = bindingWith(named, bindingRest(binding));
        QStringList to;
        for (int k = 0; k < m_rows.size(); k++)
            for (const QString &b : bindingsOf(k))
                if (canonicalBinding(b) == binding && !to.contains(m_rows[k].label->text())) to << m_rows[k].label->text();
        m_lastLabel->setText("Last input: <b>" + describeBinding(shown).toHtmlEscaped() + "</b>" +
                             (to.isEmpty() ? QString(" — not bound") : " → " + to.join(", ").toHtmlEscaped()));
    }
    refreshHighlights();
}

void ControlsDialog::refreshHighlights() {
    QVector<bool> sectionLit(m_sections.size(), false);
    for (auto &row : m_rows) {
        bool lit = false;
        for (const QString &b : m_edits[m_family].value(QString::fromStdString(row.control.name)))
            if (m_active.contains(canonicalBinding(b))) lit = true;
        if (lit) sectionLit[row.section] = true;
        row.label->setStyleSheet(lit ? "QLabel { background: palette(highlight); color: palette(highlighted-text); "
                                       "border-radius: 3px; padding: 2px 6px; }"
                                     : "QLabel { padding: 2px 6px; }");
    }
    // a folded section lights its header
    for (int s = 0; s < m_sections.size(); s++)
        m_sections[s].header->setStyleSheet(sectionLit[s] && m_sections[s].body->isHidden()
                                                ? "QToolButton { background: palette(highlight); color: palette(highlighted-text); "
                                                  "border-radius: 3px; }"
                                                : QString());
}

bool ControlsDialog::eventFilter(QObject *o, QEvent *e) {
    // Keys pressed while this dialog is in front test the bindings (and do nothing else)
    if ((e->type() == QEvent::KeyPress || e->type() == QEvent::KeyRelease) && !m_capturing && isActiveWindow() &&
        !QApplication::activePopupWidget()) {   // an open menu keeps its keys
        auto *k = static_cast<QKeyEvent *>(e);
        if (k->key() == Qt::Key_Escape) return QDialog::eventFilter(o, e);
        if (!k->isAutoRepeat()) {
            QString b = bindingForKey(k);
            if (!b.isEmpty()) source(b, e->type() == QEvent::KeyPress);
        }
        return true;
    }
    return QDialog::eventFilter(o, e);
}

void ControlsDialog::updateJoystickList() {
    QStringList n = Joysticks::instance().names();
    m_joyLabel->setText(n.isEmpty() ? "No controllers connected. Plug one in and it appears here."
                                    : "Controllers: " + n.join(" · "));
}

QStringList &ControlsDialog::bindingsOf(int k) { return m_edits[m_family][QString::fromStdString(m_rows[k].control.name)]; }

void ControlsDialog::changed() {
    refreshRows();
    refreshHighlights();
    updateBanner();
}

bool ControlsDialog::bind(int k, const QString &b, bool append, bool move) {
    const QString canon = canonicalBinding(b);
    // the other controls of a game with this one that have the input already
    QVector<int> others;
    QStringList games;
    const QStringList members = cfg::controlsSetMembers(m_family);
    for (const QString &m : members) {
        QSet<std::string> has;
        for (auto &c : hk::cab::controls(m.toStdString())) has.insert(c.name);
        if (!has.contains(m_rows[k].control.name)) continue;
        for (int j = 0; j < m_rows.size(); j++) {
            if (j == k || !has.contains(m_rows[j].control.name)) continue;
            for (const QString &o : bindingsOf(j))
                if (canonicalBinding(o) == canon) {
                    if (!others.contains(j)) others << j;
                    if (!games.contains(cfg::familyName(m))) games << cfg::familyName(m);
                }
        }
    }
    if (!others.isEmpty() && !move) {
        QStringList labels;
        for (int j : others) labels << m_rows[j].label->text();
        QString where = members.size() > 1 ? " in " + games.join(", ") : QString();
        QMessageBox ask(QMessageBox::Question, "Controls",
                        describeBinding(b) + " is already " + labels.join(", ") + where + ".\n\nPressing it would press both.",
                        QMessageBox::NoButton, this);
        auto *moveIt = ask.addButton("Move it here", QMessageBox::AcceptRole);
        auto *both = ask.addButton("Keep both", QMessageBox::ActionRole);
        ask.addButton(QMessageBox::Cancel);
        ask.setDefaultButton(moveIt);
        ask.exec();
        if (ask.clickedButton() != moveIt && ask.clickedButton() != both) return false;
        move = ask.clickedButton() == moveIt;
    }
    if (move)
        for (int j : others) bindingsOf(j).removeIf([&](const QString &o) { return canonicalBinding(o) == canon; });
    QStringList &l = bindingsOf(k);
    const QString pad = padOf(b);
    l.removeIf([&](const QString &o) { return canonicalBinding(o) == canon || (!append && padOf(o) == pad); });
    l << b;
    m_dirty.insert(m_family);
    return true;
}

void ControlsDialog::bindAll(int player) {
    // the controls in the order they are listed: a player's, or all of them but the folded sections'
    QVector<int> order;
    for (int k = 0; k < m_rows.size(); k++) {
        QString name = QString::fromStdString(m_rows[k].control.name);
        if (player ? playerOf(name) == player : !m_sections[m_rows[k].section].body->isHidden()) order << k;
    }
    QVector<int> history;      // the steps answered or skipped, for Back
    QSet<int> spun;            // players whose turntable was bound to an axis in this walk
    int i = 0;
    while (i < order.size()) {
        int k = order[i];
        const auto &c = m_rows[k].control;
        QString name = QString::fromStdString(c.name);
        // a turntable on an axis needs no buttons as well
        if ((name.endsWith("Turntable -") || name.endsWith("Turntable +")) && spun.contains(playerOf(name))) {
            i++;
            continue;
        }
        m_capturing = true;
        CaptureDialog d(m_rows[k].label->text() + "  (" + QString::fromStdString(c.group) + ")", c.analog, this,
                        QString("%1 of %2").arg(i + 1).arg(order.size()), !history.isEmpty());
        int r = d.exec();
        m_capturing = false;
        m_active.clear();
        if (r == CaptureDialog::Stopped) break;
        if (r == CaptureDialog::Back) {
            i = history.takeLast();
            if (m_rows[order[i]].control.analog) spun.remove(playerOf(QString::fromStdString(m_rows[order[i]].control.name)));
            continue;
        }
        if (r == CaptureDialog::Captured && !d.result().isEmpty()) {
            bind(k, d.result(), false, true);   // a walk sets things up anew: an input moves to where it is pressed
            if (c.analog && name.endsWith("Turntable")) spun.insert(playerOf(name));
        }
        history << i++;
        refreshRows();
    }
    changed();
}

ControlsDialog::Layout ControlsDialog::defaultLayout() const {
    // the cabinet's own pad defaults: player 1's controller, joy0
    Layout l;
    for (auto &c : setControls(m_family))
        for (auto &it : defaultItems(c)) {
            QString b = QString::fromStdString(it);
            if (bindingPad(b) == "joy0") l[QString::fromStdString(c.name)] << bindingRest(b);
        }
    return l;
}

ControlsDialog::Layout ControlsDialog::layoutOf(const QString &pad) const {
    const QString cp = padOf(bindingWith(pad, "button0"));
    Layout l;
    int uses[3] = {0, 0, 0};
    const Bindings &all = m_edits[m_family];
    for (auto it = all.cbegin(); it != all.cend(); ++it)
        for (const QString &b : *it)
            if (padOf(b) == cp) {
                l[it.key()] << bindingRest(b);
                uses[playerOf(it.key())]++;
            }
    if (uses[2] <= uses[1]) return l;
    Layout asP1;   // player 2's controller: kept as player 1's, to go to either
    for (auto it = l.cbegin(); it != l.cend(); ++it) asP1[asPlayer(it.key(), 1)] = *it;
    return asP1;
}

namespace {
QString layoutFile(const QString &set, const QString &pad) {
    // one per kind of controller: a second of a name ("@2") shares the first's
    QString model = pad.mid(4, pad.size() - 5);
    if (int at = model.indexOf('@'); at >= 0) model.truncate(at);
    return cfg::configDir() + "/controls/layouts/" + set + "/" + model + ".txt";
}
}  // namespace

ControlsDialog::Layout ControlsDialog::savedLayout(const QString &pad) const {
    Layout l;
    QFile f(layoutFile(m_family, pad));
    if (!f.open(QIODevice::ReadOnly)) return l;
    for (auto &en : hk::cab::parse_list_file(f.readAll().toStdString()))
        for (auto &it : en.items) l[QString::fromStdString(en.name)] << QString::fromStdString(it);
    return l;
}

void ControlsDialog::saveLayout(const QString &pad) {
    Layout l = layoutOf(pad);
    QString path = layoutFile(m_family, pad);
    QDir().mkpath(QFileInfo(path).path());
    QSaveFile f(path);
    QString text = "# " + Joysticks::instance().label(Joysticks::instance().indexOf(pad)) + "'s layout for " +
                   cfg::controlsSetName(m_family) + ", as player 1's, saved by the launcher's Controls dialog.\n";
    for (auto it = l.cbegin(); it != l.cend(); ++it) {
        std::vector<std::string> items;
        for (const QString &r : *it) items.push_back(r.toStdString());
        text += QString::fromStdString(hk::cab::format_entry(it.key().toStdString(), items)) + "\n";
    }
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text) || f.write(text.toUtf8()) < 0 || !f.commit())
        QMessageBox::warning(this, "Controls", "Cannot write " + path + ": " + f.errorString());
}

void ControlsDialog::applyLayout(const QString &pad, int player, const Layout &layout) {
    // the controller now does only this for the player: what it did for either player (and, as player
    // 1's, for the shared inputs) goes
    const QString cp = padOf(bindingWith(pad, "button0"));
    for (int k = 0; k < m_rows.size(); k++) {
        int p = playerOf(QString::fromStdString(m_rows[k].control.name));
        if (p != 0 || player == 1) bindingsOf(k).removeIf([&](const QString &b) { return padOf(b) == cp; });
    }
    for (auto it = layout.cbegin(); it != layout.cend(); ++it) {
        if (player == 2 && playerOf(it.key()) == 0) continue;   // the shared inputs stay player 1's
        QString name = asPlayer(it.key(), player);
        for (int k = 0; k < m_rows.size(); k++)
            if (QString::fromStdString(m_rows[k].control.name) == name)
                for (const QString &rest : *it) bind(k, bindingWith(pad, rest), true, true);
    }
    m_dirty.insert(m_family);
}

void ControlsDialog::tieToControllers() {
    auto &joys = Joysticks::instance();
    for (auto it = m_edits[m_family].begin(); it != m_edits[m_family].end(); ++it)
        for (QString &b : *it)
            if (QString pad = bindingPad(b); isPadNumber(pad) && !joys.pad(joys.indexOf(pad)).isEmpty())
                b = bindingWith(joys.pad(joys.indexOf(pad)), bindingRest(b));
    m_dirty.insert(m_family);
}

void ControlsDialog::setupMenu(QMenu *menu) {
    bool twoPlayers = false;
    for (auto &row : m_rows) twoPlayers |= playerOf(QString::fromStdString(row.control.name)) == 2;
    auto *all = menu->addMenu("Bind all in turn");
    all->addAction("Player 1", this, [this] { bindAll(1); });
    if (twoPlayers) all->addAction("Player 2", this, [this] { bindAll(2); });
    all->addAction("Everything shown", this, [this] { bindAll(0); });

    menu->addSection("Controllers");
    auto &joys = Joysticks::instance();
    if (joys.count() == 0) menu->addAction("No controllers connected")->setEnabled(false);
    // the controller player 1 plays with: the one most of player 1's bindings are on
    QHash<QString, int> p1pads;
    for (int k = 0; k < m_rows.size(); k++)
        if (playerOf(QString::fromStdString(m_rows[k].control.name)) == 1)
            for (const QString &b : bindingsOf(k))
                if (bindingPad(b) != "key") p1pads[bindingPad(b)]++;
    QString p1pad;
    for (auto it = p1pads.cbegin(); it != p1pads.cend(); ++it)
        if (p1pad.isEmpty() || *it > p1pads[p1pad]) p1pad = it.key();
    for (int j = 0; j < joys.count(); j++) {
        QString pad = joys.pad(j);
        auto *sub = menu->addMenu(joys.label(j));
        for (int p = 1; p <= (twoPlayers ? 2 : 1); p++)
            sub->addAction(QString("Default layout for player %1").arg(p), this, [this, pad, p] {
                applyLayout(pad, p, defaultLayout());
                changed();
            });
        Layout saved = savedLayout(pad);
        for (int p = 1; p <= (twoPlayers ? 2 : 1) && !saved.isEmpty(); p++)
            sub->addAction(QString("Your saved layout for player %1").arg(p), this, [this, pad, p, saved] {
                applyLayout(pad, p, saved);
                changed();
            });
        sub->addSeparator();
        auto *save = sub->addAction("Save its layout", this, [this, pad] { saveLayout(pad); });
        save->setEnabled(!layoutOf(pad).isEmpty());
        save->setToolTip("Keep what this controller does here, to use again for either player (or on another PC: "
                         "controls/layouts in the settings folder)");
        if (twoPlayers) {
            auto *copy = sub->addAction("Copy player 1's controller here, as player 2", this, [this, pad, p1pad] {
                Layout l = layoutOf(p1pad), mine;
                for (auto it = l.cbegin(); it != l.cend(); ++it)
                    if (playerOf(it.key()) == 1) mine[it.key()] = *it;
                applyLayout(pad, 2, mine);
                changed();
            });
            copy->setEnabled(!p1pad.isEmpty() && padOf(bindingWith(p1pad, "button0")) != padOf(bindingWith(pad, "button0")));
        }
    }
    bool numbered = false;
    for (auto &l : m_edits[m_family])
        for (const QString &b : l) numbered |= isPadNumber(bindingPad(b));
    menu->addSeparator();
    auto *tie = menu->addAction("Tie pad numbers to the controllers now connected", this, [this] {
        tieToControllers();
        changed();
    });
    tie->setEnabled(numbered && joys.count() > 0);
}

void ControlsDialog::updateBanner() {
    auto &joys = Joysticks::instance();
    bool twoPlayers = false;
    for (auto &row : m_rows) twoPlayers |= playerOf(QString::fromStdString(row.control.name)) == 2;
    QSet<QString> used;
    bool numbered = false;
    for (auto &l : m_edits[m_family])
        for (const QString &b : l) {
            used.insert(padOf(b));
            numbered |= isPadNumber(bindingPad(b));
        }
    QStringList lines;
    for (int j = 0; j < joys.count(); j++) {
        if (used.contains(QString("joy%1").arg(j)) || m_hidden.contains(joys.pad(j))) continue;
        auto link = [](const QString &href, const QString &text) { return "<a href=\"" + href + "\">" + text + "</a>"; };
        QStringList acts;
        bool saved = !savedLayout(joys.pad(j)).isEmpty();
        for (int p = 1; p <= (twoPlayers ? 2 : 1); p++) {
            QString who = twoPlayers ? QString(" for player %1").arg(p) : QString();
            if (saved) acts << link(QString("saved:%1:%2").arg(p).arg(j), "your saved layout" + who);
            acts << link(QString("default:%1:%2").arg(p).arg(j), "the default layout" + who);
        }
        acts << link("walk:1", twoPlayers ? "bind player 1 in turn" : "bind it in turn");
        acts << link(QString("hide:%1").arg(j), "not now");
        lines << "<b>" + joys.label(j).toHtmlEscaped() + "</b> isn't set up for " + cfg::controlsSetName(m_family).toHtmlEscaped() +
                     ": " + acts.join(" · ");
    }
    if (numbered && joys.count() > 0)
        lines << "Some bindings name a controller by its number, which follows the order they were plugged in. "
                 "<a href=\"tie\">Tie them to the controllers now connected</a>.";
    m_banner->setText(lines.join("<br>"));
    m_banner->setVisible(!lines.isEmpty());
}

QString ControlsDialog::testHint() const {
    QStringList keys;
    for (const QString &b : m_edits.value(m_family).value("Test")) keys << describeBinding(b);
    return "press Test (" + (keys.isEmpty() ? QString("not bound") : keys.join(", ")) +
           ") for its test menu: the I/O check there shows each input as it is pressed.";
}
