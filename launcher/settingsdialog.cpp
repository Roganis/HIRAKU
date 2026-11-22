// SPDX-License-Identifier: GPL-3.0-or-later
#include "settingsdialog.h"

#include <algorithm>

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QRadioButton>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSettings>
#include <QSpinBox>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyle>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QVBoxLayout>

#ifdef _WIN32
#include <SDL3/SDL.h>
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "capturedialog.h"
#include "config.h"
#include "powerplan.h"
#include "inputs.h"
#include "language.h"
#include "theme.h"
#include "updater.h"
#include "../third_party/mdi/icons.h"

#include <QFile>
#include <QMap>
#include <QTextStream>
#include "picturedialog.h"

namespace {

// A line edit with a "Browse…" button
// (no title or filter: a SoundFont's)
QWidget *withBrowse(QLineEdit *e, bool directory, QWidget *parent, QString title = QString(), QString filter = QString()) {
    if (title.isEmpty()) title = SettingsDialog::tr("Choose a SoundFont");
    // TR: a file dialog's filter: what is in brackets stays as it is; ";;" separates two of them
    if (filter.isEmpty()) filter = SettingsDialog::tr("SoundFonts (*.sf2 *.sf3);;All files (*)");
    auto *w = new QWidget(parent);
    auto *h = new QHBoxLayout(w);
    h->setContentsMargins(0, 0, 0, 0);
    h->addWidget(e, 1);
    auto *b = new QPushButton(SettingsDialog::tr("Browse…"));
    b->setAutoDefault(false);
    h->addWidget(b);
    QObject::connect(b, &QPushButton::clicked, w, [e, directory, parent, title, filter] {
        QString p = directory ? QFileDialog::getExistingDirectory(parent, SettingsDialog::tr("Choose a folder"), e->text())
                              : QFileDialog::getOpenFileName(parent, title, e->text(), filter);
        if (!p.isEmpty()) e->setText(p);
    });
    return w;
}

// A card number line with a "Pick" button, as spicecfg's: the cards HIRAKU already has (each
// game's save folder keeps them as cards/NUMBER.bin), or a new number.
QWidget *withPick(QLineEdit *e, QWidget *parent) {
    auto *w = new QWidget(parent);
    auto *h = new QHBoxLayout(w);
    h->setContentsMargins(0, 0, 0, 0);
    h->addWidget(e, 1);
    // TR: a button beside a card number: a menu of the cards HIRAKU has, or a new number
    auto *b = new QPushButton(SettingsDialog::tr("Pick"));
    b->setAutoDefault(false);
    h->addWidget(b);
    QObject::connect(b, &QPushButton::clicked, w, [e, b] {
        QMenu menu;
        QDir base(QFileInfo(cfg::nvramDir("x")).path());
        QStringList seen;
        for (const QString &game : base.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
            for (const QFileInfo &f : QDir(base.filePath(game) + "/cards").entryInfoList({"*.bin"}, QDir::Files, QDir::Name)) {
                const QString n = f.completeBaseName();
                if (seen.contains(n)) continue;
                seen << n;
                menu.addAction(n + "  (" + game + ")", [e, n] { e->setText(n); });
            }
        if (!seen.isEmpty()) menu.addSeparator();
        menu.addAction(SettingsDialog::tr("New card number"), [e] {
            QString n = "E004";   // as an e-AMUSEMENT PASS: E004 and 12 hex digits
            for (int i = 0; i < 12; i++) n += QString::number(QRandomGenerator::global()->bounded(16), 16).toUpper();
            e->setText(n);
        });
        menu.addAction(SettingsDialog::tr("None"), [e] { e->clear(); });
        menu.exec(b->mapToGlobal(QPoint(0, b->height())));
    });
    return w;
}

}  // namespace

#ifdef _WIN32
// Windows' output devices by the names Windows shows (SDL's WASAPI list): what `--audio-device`
// opens, shared or, with `--audio-exclusive`, alone.
static QStringList windowsPlaybackDevices() {
    QStringList out;
    const bool was = SDL_WasInit(SDL_INIT_AUDIO);
    if (!was && !SDL_InitSubSystem(SDL_INIT_AUDIO)) return out;
    int n = 0;
    if (SDL_AudioDeviceID *ids = SDL_GetAudioPlaybackDevices(&n)) {
        for (int k = 0; k < n; k++)
            if (const char *name = SDL_GetAudioDeviceName(ids[k])) out << QString::fromUtf8(name);
        SDL_free(ids);
    }
    if (!was) SDL_QuitSubSystem(SDL_INIT_AUDIO);
    return out;
}

// The COM ports Windows has now ("COM3"), by number, from HKLM\HARDWARE\DEVICEMAP\SERIALCOMM. Read
// with the registry's own calls: its value names ("\Device\VCP0") have backslashes, which
// QSettings takes for subkeys, so through it the list came out empty.
static QStringList windowsComPorts() {
    QStringList out;
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &key) != ERROR_SUCCESS) return out;
    for (DWORD i = 0;; i++) {
        wchar_t name[256], data[64];
        DWORD nlen = 256, dlen = sizeof data, type = 0;
        const LONG r = RegEnumValueW(key, i, name, &nlen, nullptr, &type, reinterpret_cast<BYTE *>(data), &dlen);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r != ERROR_SUCCESS || type != REG_SZ) continue;
        const QString port = QString::fromWCharArray(data, (int)(dlen / sizeof(wchar_t))).remove(QChar(0)).trimmed();
        if (!port.isEmpty() && !out.contains(port)) out << port;
    }
    RegCloseKey(key);
    std::sort(out.begin(), out.end(), [](const QString &a, const QString &b) { return a.mid(3).toInt() < b.mid(3).toInt(); });
    return out;
}

// the installed ASIO drivers by name (runtime/host/asio.h: what `--audio-asio` opens)
static QStringList asioDrivers() {
    return QSettings("HKEY_LOCAL_MACHINE\\SOFTWARE\\ASIO", QSettings::Registry64Format).childGroups();
}
#else
// The sound cards' playback devices, as ALSA names ("hw:CARD=Generic,DEV=0") and labels, from
// /proc/asound: what `--audio-device` opens alone, with no sound server in between.
static QList<QPair<QString, QString>> alsaPlaybackDevices() {
    QMap<int, QPair<QString, QString>> cards;   // number -> (id, name)
    QFile cf("/proc/asound/cards");
    if (cf.open(QIODevice::ReadOnly)) {
        static const QRegularExpression line(R"(^\s*(\d+)\s+\[([^\]]+)\]:\s*[^-]*-\s*(.*)$)");
        for (const QString &l : QTextStream(&cf).readAll().split('\n'))
            if (auto m = line.match(l); m.hasMatch()) cards[m.captured(1).toInt()] = {m.captured(2).trimmed(), m.captured(3).trimmed()};
    }
    QList<QPair<QString, QString>> out;
    QFile pf("/proc/asound/pcm");
    if (pf.open(QIODevice::ReadOnly)) {
        static const QRegularExpression line(R"(^(\d+)-(\d+):\s*([^:]*):[^:]*:(.*)$)");
        for (const QString &l : QTextStream(&pf).readAll().split('\n')) {
            auto m = line.match(l);
            if (!m.hasMatch() || !m.captured(4).contains("playback")) continue;
            const int card = m.captured(1).toInt(), dev = m.captured(2).toInt();
            if (!cards.contains(card)) continue;
            const QString name = QString("hw:CARD=%1,DEV=%2").arg(cards[card].first).arg(dev);
            out.append({name, QString("%1: %2 (%3)").arg(cards[card].second, m.captured(3).trimmed(), name)});
        }
    }
    return out;
}
#endif

// The serial ports there are now, for the card readers and the BIO2 board
static QStringList serialPorts() {
    QStringList ports;
#ifdef Q_OS_WIN
    ports = windowsComPorts();
#else
    // a port's /dev/serial/by-id name stays the same whichever USB socket it is in
    for (const QFileInfo &f : QDir("/dev/serial/by-id").entryInfoList(QDir::System | QDir::Files)) ports << f.absoluteFilePath();
    for (const QString &f : QDir("/dev").entryList({"ttyUSB*", "ttyACM*"}, QDir::System)) ports << "/dev/" + f;
#endif
    return ports;
}

// The sections down the side: each its colour (Pop: its icon's square, the page's underline and
// switches) and its icon (third_party/mdi)
namespace {

struct Section {
    const char *name, *icon;
    int colour;   // theme::rainbow
};
// (names in English, shown translated: SettingsDialog::tr)
const Section kSections[] = {{QT_TRANSLATE_NOOP("SettingsDialog", "Display"), mdi::monitor, 0},
                             {QT_TRANSLATE_NOOP("SettingsDialog", "Audio"), mdi::volume_high, 1},
                             {"e-AMUSEMENT", mdi::web, 2},
                             {QT_TRANSLATE_NOOP("SettingsDialog", "Cabinet I/O"), mdi::connection, 3},
                             {QT_TRANSLATE_NOOP("SettingsDialog", "General"), mdi::tune_variant, 4},
                             {QT_TRANSLATE_NOOP("SettingsDialog", "Look"), mdi::palette, 5}};

// a section's colour: its own in Pop, the system's highlight otherwise
QColor sectionColour(const Section &sec, const QPalette &pal) {
    return theme::pop() ? theme::rainbow(sec.colour) : pal.color(QPalette::Highlight);
}

// the list's rows: its icon (Pop: white on a square of its colour) and name, the open one on a
// soft band of its colour
class SectionPainter : public QStyledItemDelegate {
public:
    SectionPainter(QColor ground, QObject *parent) : QStyledItemDelegate(parent), m_ground(ground) {}
    bool compact = false;   // the icons alone (a narrow dialog: a small screen)
    QSize sizeHint(const QStyleOptionViewItem &o, const QModelIndex &) const override {
        int widest = 0;   // (the longest name, in the language shown)
        for (const Section &sec : kSections) widest = std::max(widest, o.fontMetrics.horizontalAdvance(SettingsDialog::tr(sec.name)));
        return QSize(compact ? 58 : widest + 70, std::max(38, o.fontMetrics.height() + 18));
    }
    void paint(QPainter *p, const QStyleOptionViewItem &o, const QModelIndex &i) const override {
        const Section &sec = kSections[i.row()];
        const QPalette &pal = o.palette;
        const QColor ground = m_ground;   // (the list's own is see-through)
        const QColor ink = pal.color(QPalette::WindowText), c = sectionColour(sec, pal);
        const bool on = o.state & QStyle::State_Selected, hover = o.state & QStyle::State_MouseOver;
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const QRectF row = QRectF(o.rect).adjusted(8, 3, -8, -3);
        if (on || hover) {
            p->setPen(Qt::NoPen);
            p->setBrush(theme::mix(c, ground, on ? (theme::pop() ? 0.78 : 0.55) : 0.9));
            p->drawRoundedRect(row, 12, 12);
        }
        const qreal mid = row.center().y();
        if (theme::pop()) {
            const QRectF sq(row.left() + 8, mid - 13, 26, 26);
            p->setPen(Qt::NoPen);
            p->setBrush(c);
            p->drawRoundedRect(sq, 8, 8);
            theme::drawIcon(p, sec.icon, sq.adjusted(4, 4, -4, -4), sec.colour == 2 ? theme::onYellow() : QColor(Qt::white));
        } else {
            theme::drawIcon(p, sec.icon, QRectF(row.left() + 9, mid - 12, 24, 24), ink);
        }
        if (compact) {
            p->restore();
            return;
        }
        QFont f = o.font;
        f.setWeight(on ? QFont::Bold : QFont::Medium);
        p->setFont(f);
        p->setPen(on ? ink : theme::mix(ink, ground, 0.25));
        p->drawText(row.adjusted(44, 0, -4, 0), Qt::AlignVCenter | Qt::AlignLeft, SettingsDialog::tr(sec.name));
        p->restore();
    }
private:
    QColor m_ground;   // the side's colour, under the list
};

}  // namespace

SettingsDialog::SettingsDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle(tr("Settings"));
    auto &s = cfg::settings();
    const bool pop = theme::pop();
    const QPalette pal = palette();
    // the sections down the side, the open one's page beside them: its name, underlined in its
    // colour, over its groups, which scroll (a small screen: 640x480 too, launcher/fit.h); OK and
    // Cancel stay below
    auto *nav = new QWidget;
    nav->setObjectName("hkSettingsNav");
    nav->setAutoFillBackground(true);
    QPalette np = nav->palette();
    np.setColor(QPalette::Window, pop ? pal.color(QPalette::Base) : theme::mix(pal.color(QPalette::Window), pal.color(QPalette::WindowText), 0.04));
    nav->setPalette(np);
    auto *nl = new QVBoxLayout(nav);
    nl->setContentsMargins(4, 16, 4, 12);
    auto *title = new QLabel(tr("Settings"));
    QFont tf = title->font();
    tf.setPointSizeF(tf.pointSizeF() * 1.6);
    tf.setWeight(QFont::Black);
    title->setFont(tf);
    title->setContentsMargins(14, 0, 0, 8);
    nl->addWidget(title);
    m_navTitle = title;
    auto *list = new QListWidget;
    m_sections = list;
    list->setObjectName("hkSections");
    m_painter = new SectionPainter(np.color(QPalette::Window), list);
    list->setItemDelegate(m_painter);
    list->setFrameShape(QFrame::NoFrame);
    list->setMouseTracking(true);
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->viewport()->setAutoFillBackground(false);
    list->setStyleSheet("QListWidget#hkSections, QListWidget#hkSections::item { background: transparent; border: none; }");
    for (const Section &sec : kSections) list->addItem(tr(sec.name));
    nl->addWidget(list, 1);
    list->setFixedWidth(list->sizeHintForColumn(0) + 8);

    auto *heading = new QLabel;
    QFont hf = heading->font();
    hf.setPointSizeF(hf.pointSizeF() * 2);
    hf.setWeight(QFont::Black);
    heading->setFont(hf);
    auto *underline = new QFrame;
    underline->setFixedSize(44, 5);
    auto *pages = new QStackedWidget;
    QList<QWidget *> bodies;
    auto page = [&](const Section &sec, const QString &about = QString()) {
        auto *body = new QWidget;
        auto *l = new QVBoxLayout(body);
        l->setContentsMargins(0, 4, 8, 4);
        l->setSpacing(12);
        if (!about.isEmpty()) {
            auto *note = new QLabel(about);
            note->setWordWrap(true);
            l->addWidget(note);
        }
        // its check boxes as switches in its colour
        body->setStyleSheet(theme::switches(sectionColour(sec, pal),
                                            pop ? (theme::current() == theme::Night ? pal.color(QPalette::Mid) : QColor("#d6cce6"))
                                                : theme::mix(pal.color(QPalette::Base), pal.color(QPalette::Text), 0.4)));
        auto *scroll = new QScrollArea;
        scroll->setWidget(body);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        pages->addWidget(scroll);
        bodies << body;
        return l;
    };
    displayPage(page(kSections[0]));
    audioPage(page(kSections[1]));
    // as spicecfg's Network page, with its option names, so a spice2x setup carries over
    eamusePage(page(kSections[2], tr("For the Konami Python games (pop'n music 9-14, DrumMania V and V2), the PC games "
                                     "(pop'n music 15-18, beatmania IIDX) and GuitarFreaks 11th and DrumMania 10th with "
                                     "their network unit (Game menu), with spice2x's option names. The game's e-AMUSEMENT "
                                     "must be ON in its test mode.")));
    cabinetPage(page(kSections[3]));
    generalPage(page(kSections[4]));
    lookPage(page(kSections[5]));

    // in Pop, each group a card on the page's paper, its name inside it
    if (pop)
        setStyleSheet(QString("QGroupBox { background: %1; border: 1px solid %2; border-radius: 14px; margin-top: 0; "
                              "padding: 30px 8px 6px 8px; font-weight: 700; }\n"
                              "QGroupBox::title { subcontrol-origin: padding; subcontrol-position: top left; left: 16px; "
                              "top: 11px; padding: 0; color: %3; }\n")
                          .arg(pal.color(QPalette::Base).name(), pal.color(QPalette::Mid).name(), pal.color(QPalette::WindowText).name()));

    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, this, &SettingsDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    // number boxes as wide as their numbers, not the dialog
    for (QSpinBox *b : {m_scale, m_volume, m_audioMs}) b->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    QSize most;
    for (QWidget *body : bodies) {
        static_cast<QVBoxLayout *>(body->layout())->addStretch(1);   // the groups at the top of their page
        // long lists (sound devices, readers) shrink to the dialog instead of widening it
        for (QComboBox *c : body->findChildren<QComboBox *>()) {
            c->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
            c->setMinimumContentsLength(16);
        }
        most = most.expandedTo(body->sizeHint());
    }
    auto open = [=](int k) {
        pages->setCurrentIndex(k);
        heading->setText(tr(kSections[k].name));
        underline->setStyleSheet(QString("background: %1; border-radius: 2px;").arg(sectionColour(kSections[k], pal).name()));
    };
    connect(list, &QListWidget::currentRowChanged, this, [open](int k) {
        if (k < 0) return;
        open(k);
        cfg::settings().setValue("ui/settingsTab", k);
    });
    list->setCurrentRow(std::clamp(s.value("ui/settingsTab", 0).toInt(), 0, list->count() - 1));
    open(list->currentRow());

    auto *right = new QVBoxLayout;
    right->setContentsMargins(24, 14, 18, 14);
    right->setSpacing(0);
    right->addWidget(heading);
    right->addSpacing(2);
    right->addWidget(underline);
    right->addSpacing(14);
    right->addWidget(pages, 1);
    right->addSpacing(10);
    right->addWidget(bb);
    auto *outer = new QHBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    outer->addWidget(nav);
    outer->addLayout(right, 1);
    // as the largest page, as the screen allows (fit.h shrinks it)
    resize(nav->sizeHint().width() + most.width() + style()->pixelMetric(QStyle::PM_ScrollBarExtent) + 60,
           most.height() + heading->sizeHint().height() + 110);
}

// The Look page: the system's own style, or Pop or Pop night (as View > Look)
void SettingsDialog::lookPage(QVBoxLayout *v) {
    auto *box = new QGroupBox(tr("The launcher's look"));
    auto *l = new QVBoxLayout(box);
    m_look = new QButtonGroup(this);
    // TR: "Pop" and "Pop night" are the looks' names (View > Look)
    for (auto [look, text] : {std::pair{theme::System, QT_TRANSLATE_NOOP("SettingsDialog", "The system's: your desktop's own style and colours, its night mode too")},
                              {theme::Pop, QT_TRANSLATE_NOOP("SettingsDialog", "Pop: HIRAKU's six colours on warm paper")},
                              {theme::Night, QT_TRANSLATE_NOOP("SettingsDialog", "Pop night: the six colours on night blue")}}) {
        auto *r = new QRadioButton(tr(text));
        r->setChecked(theme::current() == look);
        m_look->addButton(r, look);
        l->addWidget(r);
    }
    auto *note = new QLabel(tr("Also in View > Look. It changes when you press OK."));
    note->setProperty("hkMuted", true);
    l->addWidget(note);
    v->addWidget(box);
}

theme::Look SettingsDialog::look() const { return theme::Look(m_look->checkedId()); }

// A narrow dialog (a small screen: 640x480) keeps its pages' width: the sections as icons alone
void SettingsDialog::resizeEvent(QResizeEvent *e) {
    QDialog::resizeEvent(e);
    auto *painter = static_cast<SectionPainter *>(m_painter);
    const bool compact = width() < 860;
    if (compact == painter->compact) return;
    painter->compact = compact;
    m_navTitle->setVisible(!compact);
    for (int k = 0; k < m_sections->count(); k++) m_sections->item(k)->setToolTip(compact ? tr(kSections[k].name) : QString());
    m_sections->setFixedWidth(m_sections->sizeHintForColumn(0) + 8);
    m_sections->doItemsLayout();
}

// The Display page: the window, frames and picture, the shader controls
void SettingsDialog::displayPage(QVBoxLayout *v) {
    auto &s = cfg::settings();
    auto *video = new QGroupBox(tr("Window"));
    auto *fv = new QFormLayout(video);
    m_scale = new QSpinBox;
    m_scale->setRange(1, 6);
    m_scale->setSuffix("×");
    m_scale->setValue(s.value("video/scale", 2).toInt());
    fv->addRow(tr("Window size:"), m_scale);
    m_fullscreen = new QCheckBox(tr("Start in fullscreen (F11 or Alt+Enter toggles)"));
    m_fullscreen->setChecked(s.value("video/fullscreen", false).toBool());
    fv->addRow("", m_fullscreen);
    m_splitScreens = new QCheckBox(tr("Keyboardmania: each player's monitor in its own window (two displays)"));
    m_splitScreens->setChecked(s.value("video/splitScreens", false).toBool());
    m_splitScreens->setToolTip(tr("The cabinet has a monitor for each player. Player 2's window opens on the next display; "
                                  "with fullscreen, each fills its own. F11 toggles the window with the keyboard."));
    fv->addRow("", m_splitScreens);
    v->addWidget(video);
    auto *frames = new QGroupBox(tr("Frames and picture"));
    fv = new QFormLayout(frames);
    m_displaySync = new QCheckBox(tr("Smooth frames on a 60, 120 or 240 Hz display"));
    m_displaySync->setChecked(s.value("video/displaySync", true).toBool());
    m_displaySync->setToolTip(tr("The games run at their own rates (pop'n 58.49 Hz, beatmania III 59.94 Hz), which a "
                                 "fixed-refresh display can't show evenly. On: each frame stays up the same number of "
                                 "refreshes, and the game runs at the display's rate (pop'n 2.6 % faster, its sound "
                                 "with it). Off: the exact speed, for a variable-refresh (FreeSync, G-Sync) display."));
    fv->addRow("", m_displaySync);
    // DJ Main, Firebeat, Twinkle and System 573 games, with smooth frames on (Windows, or Wayland
    // with the fifo protocol: KWin 6.7); docs/SMOOTH.md
    m_motion = new QComboBox;
    m_motion->addItem(tr("Each frame held for its refreshes"), "held");
    m_motion->addItem(tr("Smooth motion in songs: every refresh a picture of its own moment while a song plays"), "smooth");
    m_motion->addItem(tr("Smooth motion everywhere: on every screen"), "smooth-everywhere");
    m_motion->addItem(tr("CRT beam: each refresh the part of the tube the beam sweeps (120 Hz and up)"), "beam");
    // (smooth motion as it was saved, on: in songs)
    m_motion->setCurrentIndex(m_motion->findData(s.value("video/crtBeam", false).toBool() ? "beam"
                                                 : !s.value("video/smoothMotion", false).toBool() ? "held"
                                                 : s.value("video/smoothMotionWhere", "songs").toString() == "everywhere"
                                                     ? "smooth-everywhere"
                                                     : "smooth"));
    m_motion->setToolTip(tr("DJ Main, Firebeat, Twinkle and System 573 games, and the PC beatmania IIDX games, with smooth "
                            "frames on, on a display faster than the game. Smooth motion: the game at its exact speed on any "
                            "display (60, 144, 240 Hz), the notes and other moving objects carried on along their move at "
                            "each refresh (the PC IIDX games: the notes only, the rest as the game drew it). In songs: only "
                            "while a song is played, the menus each frame held for its refreshes, for the games that say "
                            "when a song plays (beatmania III, most Firebeat and DJ Main games: docs/SMOOTH.md); the others get it "
                            "everywhere until they do. CRT beam "
                            "(Blur Busters' CRT Beam Simulator): each refresh lights the part of the tube the electron "
                            "beam sweeps then and lets the rest fade, for a CRT's motion clarity; best at 240 Hz, SDR, "
                            "with the monitor's ABL/APL and its own strobing off. Windows, or Wayland with the fifo "
                            "protocol (KWin 6.7)."));
    fv->addRow(tr("Motion:"), m_motion);
    m_beamGamma = new QDoubleSpinBox;
    m_beamGamma->setRange(1.8, 2.8);
    m_beamGamma->setSingleStep(0.1);
    m_beamGamma->setDecimals(2);
    m_beamGamma->setValue(s.value("video/crtBeamGamma", 2.2).toDouble());
    m_beamGamma->setToolTip(tr("Your display's gamma (2.2 for most; 2.4 if set so). A mismatch shows as horizontal bands "
                               "and washed-out colours."));
    m_beamGain = new QDoubleSpinBox;
    m_beamGain->setRange(0.3, 1.0);
    m_beamGain->setSingleStep(0.05);
    m_beamGain->setDecimals(2);
    m_beamGain->setValue(s.value("video/crtBeamGain", 0.7).toDouble());
    m_beamGain->setToolTip(tr("Brightness against motion blur: lower is sharper and darker. 0.7 at 240 Hz, about 0.5 at 120 Hz."));
    auto *beamRow = new QWidget;
    auto *bh = new QHBoxLayout(beamRow);
    bh->setContentsMargins(0, 0, 0, 0);
    bh->addWidget(new QLabel(tr("Display gamma")));
    bh->addWidget(m_beamGamma);
    bh->addSpacing(12);
    bh->addWidget(new QLabel(tr("Brightness")));
    bh->addWidget(m_beamGain);
    bh->addStretch(1);
    fv->addRow(tr("CRT beam:"), beamRow);
    // motion and the beam need smooth frames (each frame held for whole refreshes) on
    auto beamShown = [this, beamRow] {
        m_motion->setEnabled(m_displaySync->isChecked());
        beamRow->setEnabled(m_displaySync->isChecked() && m_motion->currentData().toString() == "beam");
    };
    connect(m_motion, &QComboBox::currentIndexChanged, this, beamShown);
    connect(m_displaySync, &QCheckBox::toggled, this, beamShown);
    beamShown();
    m_statsOverlay = new QComboBox;   // runtime/host/statsview.h
    m_statsOverlay->addItem(tr("Off (F7 in the game shows it)"), "off");
    m_statsOverlay->addItem(tr("One line: frame rate, a frame's work, processor, sound, late frames"), "line");
    m_statsOverlay->addItem(tr("Panel: with a graph of the frames, the sound and the display sync"), "panel");
    m_statsOverlay->addItem(tr("Everything: with latencies, threads, the graphics card, the core's numbers"), "full");
    m_statsOverlay->setCurrentIndex(std::max(0, m_statsOverlay->findData(s.value("video/statsOverlay", "off").toString())));
    m_statsOverlay->setToolTip(tr("How the game runs on this PC, over its picture: whether each frame held its place on the "
                                  "display and the sound stayed whole, and where a slow frame's time went. In the game, F7 "
                                  "goes through the levels, the mouse drags the panel anywhere on the picture, and Shift+F7 "
                                  "puts it in the next corner. It changes nothing in the game, and no snapshot has it."));
    fv->addRow(tr("Performance overlay:"), m_statsOverlay);
    m_picture = new QComboBox;
    m_picture->addItem(tr("Off: sharp pixels"), "off");
    m_picture->addItem(tr("CRT light: crt-guest-advanced-fast"), "light");
    m_picture->addItem(tr("CRT full: crt-royale"), "full");
    m_picture->addItem(tr("CRT analog: Lola Retro (the heaviest)"), "lola");
    m_picture->setCurrentIndex(std::max(0, m_picture->findData(picture::globalMode())));
    m_picture->setToolTip(tr("For all games; a game can have its own (Picture, in the game's menu). In the game, F10 cycles them."));
    auto *pic = new QWidget;
    auto *ph = new QHBoxLayout(pic);
    ph->setContentsMargins(0, 0, 0, 0);
    ph->addWidget(m_picture, 1);
    auto *adjust = new QPushButton(tr("Adjust…"));
    adjust->setAutoDefault(false);
    ph->addWidget(adjust);
    connect(adjust, &QPushButton::clicked, this, [this] {
        cfg::settings().setValue("video/picture", m_picture->currentData().toString());
        PictureDialog d(QString(), QString(), false, this);
        if (d.exec() == QDialog::Accepted) m_picture->setCurrentIndex(std::max(0, m_picture->findData(picture::globalMode())));
    });
    fv->addRow(tr("Picture:"), pic);
    v->addWidget(frames);
    // F6 in the game: the game to a video file (runtime/host/record.h, docs/PLAY.md "Recording video")
    auto *rec = new QGroupBox(tr("Recording (F6 in the game)"));
    auto *fr = new QFormLayout(rec);
    m_recFormat = new QComboBox;
    m_recFormat->addItem(tr("Exact, lossless (.mkv: every pixel and sound as the game made them; large)"), "ffv1");
    m_recFormat->addItem(tr("For sharing, H.264 (.mp4: plays everywhere, about a sixth of the size)"), "h264");
    m_recFormat->setCurrentIndex(std::max(0, m_recFormat->findData(s.value("record/format", "ffv1").toString())));
    m_recFormat->setToolTip(tr("Exact recordings play in VLC, mpv and FFmpeg's other players and editors, and can be made "
                               "into the other kind afterwards (File > Convert recordings for sharing). Recordings go to "
                               "the recordings folder in the saves' folder."));
    fr->addRow(tr("Recording:"), m_recFormat);
    m_recEncoder = new QComboBox;
    m_recEncoder->addItem(tr("Automatic: the graphics card's when it can, else the processor"), "auto");
    m_recEncoder->addItem(tr("Graphics card (NVIDIA, AMD, Intel, or Windows' own)"), "gpu");
    m_recEncoder->addItem(tr("Processor (x264)"), "x264");
    m_recEncoder->setCurrentIndex(std::max(0, m_recEncoder->findData(s.value("record/encoder", "auto").toString())));
    m_recEncoder->setToolTip(tr("Who compresses the video. A graphics card's encoder costs the game nearly nothing; x264 "
                                "takes processor time but works on every PC. The game's log says which was taken and why."));
    fr->addRow(tr("Encoder:"), m_recEncoder);
    m_recScale = new QComboBox;
    m_recScale->addItem(tr("Automatic: enlarged to about 1440 lines by a graphics card, the game's own by the processor"), 0);
    m_recScale->addItem(tr("The game's own size"), 1);
    for (int k = 2; k <= 4; k++) m_recScale->addItem(QString("%1×").arg(k), k);
    m_recScale->setCurrentIndex(std::max(0, m_recScale->findData(s.value("record/scale", 0).toInt())));
    m_recScale->setToolTip(tr("H.264 keeps colour at half the resolution, which smears pixel art at the game's own size: "
                              "enlarged by whole pixels first, it stays sharp. A graphics card's encoder does that for "
                              "nothing; on the processor it takes several times the work, so Automatic keeps the game's "
                              "own size there (most games are 480 lines or fewer). A size chosen here is used either way."));
    fr->addRow(tr("Size:"), m_recScale);
    auto recShown = [this] {
        const bool h264 = m_recFormat->currentData().toString() == "h264";
        m_recEncoder->setEnabled(h264);
        m_recScale->setEnabled(h264);
    };
    connect(m_recFormat, &QComboBox::currentIndexChanged, this, recShown);
    recShown();
    v->addWidget(rec);
    auto *py = new QGroupBox(tr("Konami Python games (pop'n music 9-14, DrumMania V and V2)"));
    auto *fy = new QFormLayout(py);
    m_pyRenderer = new QComboBox;
    // (not OpenGL: PCSX2's needs a window of its own, and the core draws without one)
    m_pyRenderer->addItem(tr("Vulkan (the default)"), "vk");
#ifdef Q_OS_WIN
    m_pyRenderer->addItem("Direct3D 11", "dx11");
    m_pyRenderer->addItem("Direct3D 12", "dx12");
#endif
    m_pyRenderer->addItem(tr("Software: the CPU draws (the most CPU)"), "sw");
    m_pyRenderer->setCurrentIndex(std::max(0, m_pyRenderer->findData(s.value("video/pythonRenderer", "vk").toString())));
#ifdef Q_OS_WIN
    m_pyRenderer->setToolTip(tr("How PCSX2's GS draws the PS2's pictures (--renderer). Vulkan takes the least CPU. If these "
                                "games crash or show nothing on this PC, its graphics driver may not get on with Vulkan: "
                                "try Direct3D 11, then Software (which still shows its picture through Direct3D)."));
#else
    m_pyRenderer->setToolTip(tr("How PCSX2's GS draws the PS2's pictures (--renderer). Vulkan takes the least CPU. If these "
                                "games crash or show nothing on this PC, try Software (it still needs a Vulkan driver to "
                                "show its picture)."));
#endif
    fy->addRow(tr("Graphics:"), m_pyRenderer);
    v->addWidget(py);
    // the shader's settings in the game (runtime/host/shaderview.h): a key or a controller's
    // button for each of its controls, as in Controls
    auto *sk = new QGroupBox(tr("The shader's settings while playing (F8)"));
    auto *gk = new QGridLayout(sk);
    auto *skNote = new QLabel(tr("A panel beside the picture: pick a setting, change it, see it at once, save it. A key or a "
                                 "controller's button for each (with the keyboard, Shift does ten steps, or saves for this game)."));
    skNote->setWordWrap(true);
    gk->addWidget(skNote, 0, 0, 1, 3);
    QList<QPair<QString, QPushButton *>> skButtons;
    auto shown = [](const QString &b) { return b.isEmpty() ? tr("none") : describeBinding(b); };   // TR: nothing set (a key; below, a board)
    int row = 1;
    for (const cfg::ShaderKey &k : cfg::shaderKeys()) {
        const QString action = k.action, label = language::tr("cfg", k.label);
        m_shaderKeys[action] = s.value("shaderKeys/" + action, QString(k.binding)).toString();
        auto *b = new QPushButton(shown(m_shaderKeys[action]));
        b->setAutoDefault(false);
        b->setToolTip(tr("Click, then press the key or the controller's button"));
        connect(b, &QPushButton::clicked, this, [this, b, action, label, shown] {
            // TR: %1 is what the key does ("Previous setting"), in the box that waits for the key
            CaptureDialog d(tr("%1  (the shader's settings, F8)").arg(label), false, this);
            if (d.exec() != CaptureDialog::Captured || d.result().isEmpty()) return;
            m_shaderKeys[action] = d.result();
            b->setText(shown(d.result()));
        });
        auto *none = new QToolButton;
        none->setText("✕");
        none->setAutoRaise(true);
        none->setToolTip(tr("No control for it"));
        connect(none, &QToolButton::clicked, this, [this, b, action, shown] {
            m_shaderKeys[action].clear();
            b->setText(shown(QString()));
        });
        gk->addWidget(new QLabel(tr("%1:").arg(label)), row, 0);   // TR: a label before its key: "Previous setting:"
        gk->addWidget(b, row, 1);
        gk->addWidget(none, row, 2);
        skButtons.append({action, b});
        row++;
    }
    auto *skDefaults = new QPushButton(tr("Defaults"));
    skDefaults->setAutoDefault(false);
    connect(skDefaults, &QPushButton::clicked, this, [this, skButtons, shown] {
        for (const cfg::ShaderKey &k : cfg::shaderKeys()) m_shaderKeys[k.action] = k.binding;
        for (const auto &[action, b] : skButtons) b->setText(shown(m_shaderKeys[action]));
    });
    gk->addWidget(skDefaults, row, 1, Qt::AlignLeft);
    gk->setColumnStretch(1, 1);
    v->addWidget(sk);
}

// The Audio page: the output, its timing, Keyboardmania's SoundFont
void SettingsDialog::audioPage(QVBoxLayout *v) {
    auto &s = cfg::settings();
    auto *audio = new QGroupBox(tr("Output"));
    auto *fa = new QFormLayout(audio);
    auto *latency = new QGroupBox(tr("Latency"));
    auto *fq = new QFormLayout(latency);
    m_audioMs = new QSpinBox;
    m_audioMs->setRange(2, 30);
    m_audioMs->setSuffix(" ms");
    m_audioMs->setValue(s.value("audio/ms", 8).toInt());
    m_audioMs->setToolTip(tr("The sound's lead over the device, and its least: it grows by itself while frames keep "
                             "coming late on this PC, and comes back down when they stop (docs/PLAY.md, Latency)."));
    m_audioFixed = new QCheckBox(tr("Fixed: never more (a rare break in the sound instead)"));
    m_audioFixed->setChecked(s.value("audio/fixed", false).toBool());
    m_audioFixed->setToolTip(tr("The queue stays at this setting all game long: the least delay on every sound, and "
                                "a click whenever a frame comes later than it covers."));
    m_volume = new QSpinBox;
    m_volume->setRange(-20, 20);
    m_volume->setSuffix(" dB");
    m_volume->setValue(s.value("audio/volumeDb", 0).toInt());
    m_volume->setToolTip(tr("Louder or quieter than the machine makes it, for every game (6 dB: twice as loud). "
                            "Some games, Keyboardmania among them, are quiet next to others; as in MAME."));
    fq->addRow(tr("Audio queue:"), m_audioMs);
    fq->addRow("", m_audioFixed);
    m_priority = new QCheckBox(tr("Ahead of other programs (higher priority, and GameMode if installed)"));
    m_priority->setChecked(s.value("system/priority", true).toBool());
    m_priority->setToolTip(tr("The game's threads run ahead of other programs (a browser, a build), so a busy PC "
                              "doesn't delay its frames and sound; the sound server and the desktop keep theirs. "
                              "A sound card of its own (Output) is written by a real-time thread either way."));
    fq->addRow("", m_priority);
    if (powerplan::available()) {   // Windows (launcher/powerplan.h)
        m_powerPlan = new QCheckBox(tr("Windows' High performance power plan while a game runs"));
        m_powerPlan->setChecked(s.value("system/highPerformance", false).toBool());
        m_powerPlan->setToolTip(tr("Windows' Balanced plan lets the processor slow down while a game waits for its next frame, and "
                                   "on some PCs frames then come late (a game's log says so: \"Windows let the CPU slow down\"). "
                                   "With this on, HIRAKU changes to the High performance plan when a game starts and back to "
                                   "yours when its window is back. The PC uses more power meanwhile. A plan you set yourself "
                                   "is left as it is."));
        fq->addRow("", m_powerPlan);
    }
    m_audioDevice = new QComboBox;
    const QString device = s.value("audio/device").toString();
#ifdef _WIN32
    m_audioDevice->addItem(tr("Windows' default output"), QString());
    for (const QString &name : windowsPlaybackDevices()) m_audioDevice->addItem(name, name);
    for (const QString &name : asioDrivers()) m_audioDevice->addItem("ASIO: " + name, "asio:" + name);
    // TR: a sound device saved earlier that this PC hasn't now
    if (!device.isEmpty() && m_audioDevice->findData(device) < 0) m_audioDevice->addItem(tr("%1 (not found now)").arg(device), device);
    m_audioDevice->setToolTip(tr("The sound device the game plays on. ASIO: the card through its own ASIO driver, with "
                                 "no Windows audio in between; its buffer is the one set in the driver's control panel "
                                 "unless Device buffer below says otherwise. If the driver won't start, the game uses "
                                 "Windows' default output."));
    fa->addRow(tr("Output:"), m_audioDevice);
    m_audioExclusive = new QCheckBox(tr("Exclusive (WASAPI): the device alone, lowest latency"));
    m_audioExclusive->setChecked(s.value("audio/exclusive", false).toBool());
    m_audioExclusive->setToolTip(tr("The game has the device to itself in WASAPI exclusive mode, with no Windows mixer in "
                                    "between and its buffer as small as the device allows (Device buffer below); other "
                                    "programs are silent on it meanwhile. If the device refuses (another program has it "
                                    "alone, or Windows' sound settings don't allow exclusive mode), the game plays shared."));
    fa->addRow("", m_audioExclusive);
    auto exclusiveFor = [this] { m_audioExclusive->setEnabled(!m_audioDevice->currentData().toString().startsWith("asio:")); };
    connect(m_audioDevice, &QComboBox::currentIndexChanged, this, exclusiveFor);
#else
    m_audioDevice->addItem(tr("The system's default (the sound server)"), QString());
    // TR: a sound card opened alone (no sound server in between): %1 is its name
    for (const auto &[name, label] : alsaPlaybackDevices()) m_audioDevice->addItem(tr("Alone: %1").arg(label), name);
    if (!device.isEmpty() && m_audioDevice->findData(device) < 0) m_audioDevice->addItem(tr("Alone: %1").arg(device), device);
    m_audioDevice->setToolTip(tr("A sound card alone: the game opens it through ALSA with no sound server in between, for "
                                 "the lowest latency; nothing else plays on it meanwhile. If it's busy (the sound server lets "
                                 "go of a card a few seconds after the last sound on it), the game uses the default instead."));
    fa->addRow(tr("Output:"), m_audioDevice);
#endif
    m_audioDevice->setCurrentIndex(std::max(0, m_audioDevice->findData(device)));
#ifdef _WIN32
    exclusiveFor();
#endif
    m_audioBuffer = new QComboBox;
    for (int f : {0, 32, 48, 64, 96, 128, 192, 256, 512})
        // TR: sound frames (one a sample for each speaker), and the time they take at 48 kHz
        m_audioBuffer->addItem(f ? language::trn("SettingsDialog", "%n frame (%1 ms)", "%n frames (%1 ms)", f)
                                       .arg(QLocale().toString(f * 1000.0 / 48000, 'f', 1))
#ifdef _WIN32
                                 : tr("Default (128 frames, 2.7 ms; ASIO: the driver's own)"), f);
#else
                                 : tr("Default (128 frames, 2.7 ms)"), f);
#endif
    m_audioBuffer->setCurrentIndex(std::max(0, m_audioBuffer->findData(s.value("audio/buffer", 0).toInt())));
    m_audioBuffer->setToolTip(tr("The sound device's buffer (the sound server's quantum; on Windows in exclusive mode the device's own; with a Linux card alone, its "
                                 "period: HIRAKU keeps two of them queued on the card, four on a USB card, which needs them). Smaller is "
                                 "sooner, until it crackles; the audio queue above tunes itself to it. Times at 48 kHz."));
    fa->addRow(tr("Device buffer:"), m_audioBuffer);
    fa->addRow(tr("Volume:"), m_volume);
    m_evenVolume = new QCheckBox(tr("Every game about as loud as the others"));
    m_evenVolume->setChecked(s.value("audio/evenVolume", true).toBool());
    m_evenVolume->setToolTip(tr("The machines make their sound at very different levels: System 573 and Twinkle games "
                                "play about 8 dB louder than DJ Main's. On: a trim for each family of games brings their "
                                "songs to about the same loudness (System 573 -5 dB, Twinkle -4, PC IIDX -4, PC pop'n -1, "
                                "Firebeat 0, Python +1, DJ Main +3), and the volume above on top. Off: each machine's own "
                                "level (--even-volume off; docs/PLAY.md, Sound levels)."));
    fa->addRow("", m_evenVolume);
    m_fineSound = new QCheckBox(tr("Sound every millisecond on System 573 and Twinkle (not MAME's)"));
    m_fineSound->setChecked(s.value("audio/soundEveryMs", true).toBool());
    m_fineSound->setToolTip(tr("MAME hands these machines' sound on 50 times a second, which adds up to 20 ms of "
                               "latency, uneven from moment to moment. Every millisecond is lower and steadier. On "
                               "Twinkle (beatmania IIDX) the keysounds then start when the game triggers them, so the "
                               "sound no longer matches MAME's. On by default; off is MAME's timing (--mame-sound; "
                               "docs/PLAY.md, Latency)."));
    fq->addRow("", m_fineSound);
    m_pressTime = new QCheckBox(tr("Presses judged when they happened (pop'n music 15-18, IIDX 9th style to EMPRESS)"));
    m_pressTime->setChecked(s.value("game/pressTime", false).toBool());
    m_pressTime->setToolTip(tr("The game judges a press at the frame it reads the buttons in, up to a frame (17 ms) after "
                               "it happened, as on its cabinet. On: each press is judged at the moment the controller "
                               "gave it. IIDX judges in whole frames: there, each press counts in the frame it happened "
                               "in, however late the game read it (--press-time; docs/PLAY.md, PC)."));
    fq->addRow("", m_pressTime);
    m_judgeOffset = new QSpinBox;
    m_judgeOffset->setRange(-200, 200);
    m_judgeOffset->setSuffix(" ms");
    m_judgeOffset->setValue(s.value("game/judgeOffset", 0).toInt());
    m_judgeOffset->setToolTip(tr("pop'n music 15-18, IIDX 9th style to EMPRESS: each press judged this much earlier than it came "
                                 "(--judge-offset), for a TV or a sound system that shows or plays late, so presses made in "
                                 "time with it count as in time. Positive when the notes look late; pop'n judges in 4 ms "
                                 "steps, IIDX in frames (each press moved by whole frames from its own time: part of a "
                                 "frame on average, with presses judged when they happened)."));
    fq->addRow(tr("Judgement offset:"), m_judgeOffset);
    v->addWidget(audio);
    v->addWidget(latency);
    auto *kbm = new QGroupBox("Keyboardmania");
    auto *fk = new QFormLayout(kbm);
    m_soundfont = new QLineEdit(s.value("audio/soundfont").toString());
    m_soundfont->setPlaceholderText(tr("default: the system's General MIDI SoundFont"));
    m_soundfont->setToolTip(tr("The instruments of Keyboardmania's MIDI out (its keyboards' sound when a song plays them)."));
    fk->addRow(tr("SoundFont:"), withBrowse(m_soundfont, false, this));
    v->addWidget(kbm);
    // the launcher's music (launcher/music.h, runtime/midi/player.cpp)
    auto *music = new QGroupBox(tr("Music in the launcher"));
    auto *fm = new QFormLayout(music);
    music->setToolTip(tr("A small MIDI player, the Music panel under the details and the log (View > Music). It waits "
                         "while a game or the game menu runs, and goes on afterwards."));
    m_musicFolder = new QLineEdit(s.value("music/folder").toString());
    m_musicFolder->setPlaceholderText(QDir::toNativeSeparators(cfg::musicDir()));
    m_musicFolder->setToolTip(tr("Where its MIDI files are (.mid, .midi, .kar, .rmi), its folders too. MIDI files dropped "
                                 "on the launcher's window play as well."));
    fm->addRow(tr("Folder:"), withBrowse(m_musicFolder, true, this));
    m_musicSynth = new QComboBox;
    m_musicSynth->addItem(tr("The first there is"), "auto");
    m_musicSynth->addItem(tr("Keyboardmania's synth card"), "card");
    m_musicSynth->addItem(tr("A SoundFont"), "font");
#ifdef Q_OS_WIN
    m_musicSynth->addItem(tr("Windows' own synth"), "windows");
#endif
    m_musicSynth->setCurrentIndex(std::max(0, m_musicSynth->findData(s.value("music/synth", "auto").toString())));
#ifdef Q_OS_WIN
    m_musicSynth->setToolTip(tr("What plays the notes. Keyboardmania's synth card: the Yamaha MU100B in its cabinet, "
                                "emulated, once a Keyboardmania game is added (about a quarter of a CPU core while it "
                                "plays). A SoundFont: the one below, else Keyboardmania's above, else the system's. "
                                "Windows' own synth: Microsoft GS Wavetable Synth, always there. The first there is: in "
                                "that order."));
#else
    m_musicSynth->setToolTip(tr("What plays the notes. Keyboardmania's synth card: the Yamaha MU100B in its cabinet, "
                                "emulated, once a Keyboardmania game is added (about a quarter of a CPU core while it "
                                "plays). A SoundFont: the one below, else Keyboardmania's above, else the system's. "
                                "The first there is: in that order."));
#endif
    fm->addRow(tr("Synth:"), m_musicSynth);
    m_musicFont = new QLineEdit(s.value("music/soundfont").toString());
    m_musicFont->setPlaceholderText(tr("default: Keyboardmania's above"));
    m_musicFont->setToolTip(tr("A General MIDI SoundFont (.sf2) for the music."));
    fm->addRow(tr("SoundFont:"), withBrowse(m_musicFont, false, this, tr("Choose a SoundFont"), tr("SoundFonts (*.sf2);;All files (*)")));
    m_musicShuffle = new QCheckBox(tr("Shuffle"));
    m_musicShuffle->setChecked(s.value("music/shuffle", true).toBool());
    fm->addRow(m_musicShuffle);
    v->addWidget(music);
}

// The e-AMUSEMENT page: the server, the PCBID and cards, the card readers
void SettingsDialog::eamusePage(QVBoxLayout *v) {
    auto &s = cfg::settings();
    auto *ea = new QGroupBox(tr("Server"));
    auto *fe = new QFormLayout(ea);
    m_eaUrl = new QLineEdit(s.value("eamuse/url", s.value("eamuse/server")).toString());
    m_eaUrl->setPlaceholderText(tr("empty: the server inside HIRAKU (-ea)"));
    m_eaUrl->setToolTip(tr("-url. Empty: the server inside HIRAKU, which keeps its data in the game's save folder. "
                           "host:port or http://host:port/path: a server on the network, such as hiraku-eamuse on another "
                           "PC (docs/EAMUSE3.md). The game's e-AMUSEMENT must be ON in test mode."));
    fe->addRow(tr("EA Service URL:"), m_eaUrl);
    m_smartea = new QCheckBox(tr("The server inside HIRAKU when that one doesn't answer (-smartea)"));
    m_smartea->setChecked(s.value("eamuse/smartea", false).toBool());
    fe->addRow("", m_smartea);
    const QRegularExpression hex20("[0-9A-Fa-f]{0,20}"), hex16("[0-9A-Fa-f]{0,16}");
    m_pcbid = new QLineEdit(s.value("eamuse/pcbid").toString());
    m_pcbid->setValidator(new QRegularExpressionValidator(hex20, m_pcbid));
    m_pcbid->setPlaceholderText(tr("the game's own (pop'n 9: 01401401000100045FEF)"));
    m_pcbid->setToolTip(tr("-p: the cabinet's PCBID, 20 hex digits, for a server that only takes the machines it knows.\n"
                           "HIRAKU's own server (hiraku-eamuse) makes one with --new-cabinet and then answers only the cabinets it lists."));
    fe->addRow(tr("PCBID:"), m_pcbid);
    m_eaKey = new QLineEdit(s.value("eamuse/key").toString());
    m_eaKey->setEchoMode(QLineEdit::PasswordEchoOnEdit);
    m_eaKey->setPlaceholderText(tr("none (a server without a key)"));
    m_eaKey->setToolTip(tr("The key of a hiraku-eamuse that has one (its --new-key prints it; -eakey on a command line).\n"
                           "With it, all that this cabinet and the server say goes over the network sealed, and the "
                           "server answers only cabinets that have it. A key on both sides, or on neither."));
    fe->addRow(tr("Server key:"), m_eaKey);
    v->addWidget(ea);
    auto *cards = new QGroupBox(tr("Cards"));
    fe = new QFormLayout(cards);
    for (int n = 0; n < 2; n++) {
        m_card[n] = new QLineEdit(s.value(QString("eamuse/card%1").arg(n)).toString());
        m_card[n]->setValidator(new QRegularExpressionValidator(hex16, m_card[n]));
        m_card[n]->setPlaceholderText(n ? tr("card1.txt (pop'n has one reader: not used)") : tr("card0.txt, or the save folder's card.bin"));
        // TR: %1 is 0 or 1 (the option -card0 and the file card0.txt)
        m_card[n]->setToolTip(tr("-card%1: the card's number, 16 hex digits (E004...), the same as in spice2x, so a "
                                 "server knows the player. Empty: card%1.txt in the save folder, if there is one; "
                                 "else the save folder's card.bin, numbered by the server.")
                                  .arg(n) +
                              (n ? " " + tr("pop'n has one card reader, player 1's.") : QString()));
        // TR: the spice2x option's name (spicecfg's Network page): %1 is 1 or 2
        fe->addRow(tr("Player %1 Card:").arg(n + 1), withPick(m_card[n], this));
    }
    m_autocard = new QComboBox;
    for (const char *v : {"off", "p1", "p2", "both"}) m_autocard->addItem(v, v);
    m_autocard->setCurrentIndex(std::max(0, m_autocard->findData(s.value("eamuse/autocard", "off").toString())));
    m_autocard->setToolTip(tr("-autocard: the card is put in whenever the reader waits for one, so Insert needn't be pressed."));
    fe->addRow(tr("Auto Card Insert:"), m_autocard);
    m_autopin = new QCheckBox(tr("Type the card's PIN (pop'n music 14-18)"));
    m_autopin->setChecked(s.value("eamuse/autopin", false).toBool());
    m_autopin->setToolTip(tr("-autopin: when the game asks for a known card's PIN, HIRAKU types it on the 10-key, as the "
                             "server inside HIRAKU keeps it (not with a server on the network)."));
    fe->addRow("", m_autopin);
    v->addWidget(cards);

    // real readers, as spice2x's -reader, -scard and -cardio (runtime/host/readers.h)
    auto *rd = new QGroupBox(tr("Card readers (pop'n music 9-18, GuitarFreaks, DrumMania)"));
    auto *fr = new QFormLayout(rd);
    const QStringList ports = serialPorts();
    for (int n = 0; n < 2; n++) {
        m_reader[n] = new QComboBox;
        m_reader[n]->setEditable(true);
        m_reader[n]->addItem("");
        m_reader[n]->addItems(ports);
        m_reader[n]->setCurrentText(s.value(QString("readers/serial%1").arg(n)).toString());
#ifdef Q_OS_WIN
        m_reader[n]->lineEdit()->setPlaceholderText(tr("none (or type its port: COM3)"));
#else
        m_reader[n]->lineEdit()->setPlaceholderText(tr("none", "no card reader"));
#endif
        m_reader[n]->setToolTip(tr("-reader: a Konami e-AMUSEMENT reader with its 10-key on a serial port (a USB-serial "
                                   "adapter). A card held to it is the player's card in the game; its 10-key is the "
                                   "game's (00 is Enter where the game has no 00)."));
        fr->addRow(tr("Player %1 serial reader:").arg(n + 1), m_reader[n]);
    }
    m_scard = new QCheckBox(tr("NFC readers (ACR122U and the like) through PC/SC (-scard): the first is player 1's"));
    m_scard->setChecked(s.value("readers/scard", false).toBool());
#ifdef Q_OS_WIN
    m_scard->setToolTip(tr("Through Windows' own smart card service. Cards that aren't e-AMUSEMENT PASSes (FeliCa, phones) "
                           "get an E00401 number, as spice2x's -scardfix."));
#else
    m_scard->setToolTip(tr("Needs the pcscd service (systemctl enable --now pcscd.socket). Cards that aren't e-AMUSEMENT "
                           "PASSes (FeliCa, phones) get an E00401 number, as spice2x's -scardfix."));
#endif
    fr->addRow("", m_scard);
    m_cardio = new QCheckBox(tr("cardio readers (USB HID, -cardio): the first is player 1's"));
    m_cardio->setChecked(s.value("readers/cardio", false).toBool());
    m_cardio->setToolTip(tr("Read through /dev/hidraw: a udev rule lets you open it (docs/PLAY.md, Card readers)."));
#ifdef Q_OS_WIN
    m_cardio->setToolTip(tr("Readers that show themselves as cardio's HID device (usage page FFCA): the first two found."));
#endif
    fr->addRow("", m_cardio);
    v->addWidget(rd);
}

// The Cabinet I/O page: the I/O boards, the lights
void SettingsDialog::cabinetPage(QVBoxLayout *v) {
    auto &s = cfg::settings();
    auto *lights = new QGroupBox(tr("Lights"));
    auto *fl = new QVBoxLayout(lights);
    m_socket = new QCheckBox(tr("Publish lamps on a local socket (live view in Lights, external LED tools)"));
    m_socket->setChecked(s.value("lights/socket", true).toBool());
    fl->addWidget(m_socket);
    m_lightsOverlay = new QCheckBox(tr("Show the game's lights over its picture (F9 in the game shows or hides them)"));
    m_lightsOverlay->setChecked(s.value("lights/overlay", false).toBool());
    m_lightsOverlay->setToolTip(tr("Every lamp the game drives, lit or not, grouped as the cabinet has them, and how often "
                                   "they change: for seeing what a game does with its lights without the cabinet."));
    fl->addWidget(m_lightsOverlay);
    // TR: %1 is the socket's path
    auto *path = new QLabel(tr("Socket: <tt>%1</tt> (protocol: docs/LIGHTS.md)").arg(cfg::lightsSocket().toHtmlEscaped()));
    path->setTextInteractionFlags(Qt::TextSelectableByMouse);
    fl->addWidget(path);

    auto *cab = new QGroupBox(tr("I/O boards"));
    auto *fc = new QFormLayout(cab);
    m_bio2 = new QComboBox;
    m_bio2->setEditable(true);
    m_bio2->addItem("");
    m_bio2->addItems(serialPorts());
    m_bio2->setCurrentText(s.value("cabinet/bio2").toString());
    m_bio2->lineEdit()->setPlaceholderText(tr("none"));
    m_bio2->setToolTip(tr("--bio2: beatmania IIDX's BIO2 I/O board (IIDX 25-26's BI2A firmware) on its USB serial port: "
                          "the cabinet's keys, turntables, buttons, effector and coin, and its lamps and ticker, for "
                          "beatmania IIDX, III and the DJ Main games."));
    fc->addRow(tr("IIDX BIO2 board:"), m_bio2);
    m_popnio = new QComboBox;
    m_popnio->setEditable(true);
    m_popnio->addItems({"", "auto"});
    m_popnio->setCurrentText(s.value("cabinet/popnio").toString());
    m_popnio->lineEdit()->setPlaceholderText(tr("none"));
#ifdef Q_OS_WIN
    m_popnio->setToolTip(tr("--popn-io: a pop'n music cabinet's USB I/O board (the one pop'n music 15 and later drive through "
                            "ezusb.dll, or a board that copies it, such as PopnIO): its buttons, TEST, SERVICE and coin, and its "
                            "button, top and side lamps, for every pop'n music game. On Windows choose auto: the ezusb.dll below "
                            "finds the board (Konami's needs Cypress's CyUSB driver installed for it). The game's log says "
                            "how it goes, in its lines starting popn-io."));
#else
    m_popnio->setToolTip(tr("--popn-io: a pop'n music cabinet's USB I/O board (the one pop'n music 15 and later drive through "
                            "ezusb.dll, or a board that copies it, such as PopnIO): its buttons, TEST, SERVICE and coin, and its "
                            "button, top and side lamps, for every pop'n music game. auto takes the first; a USB port "
                            "(as in /sys/bus/usb/devices, e.g. 3-2.1) chooses one."));
#endif
    fc->addRow(tr("pop'n music I/O board:"), m_popnio);
    m_popnioDll = new QLineEdit(s.value("cabinet/popnioDll").toString());
#ifdef Q_OS_WIN
    m_popnioDll->setPlaceholderText(tr("the ezusb.dll that drives it: the game's, or a replacement"));
    m_popnioDll->setToolTip(tr("--popn-io-dll: HIRAKU drives the board through it, as the games do: the game's ezusb.dll "
                               "(pop'n music 15 or later, with Cypress's driver installed), or a replacement such as "
                               "UltimatePopnController's."));
#else
    m_popnioDll->setPlaceholderText(tr("only for a board with no firmware: the game's ezusb.dll"));
    m_popnioDll->setToolTip(tr("--popn-io-dll: Konami's board keeps no firmware; the game loads it at every start "
                               "from its ezusb.dll (pop'n music 15 or later), and HIRAKU does the same from this file."));
#endif
    fc->addRow(tr("  its ezusb.dll:"), withBrowse(m_popnioDll, false, this, tr("Choose an ezusb.dll"), tr("ezusb.dll (ezusb*.dll);;All files (*)")));
    v->addWidget(cab);
    v->addWidget(lights);
}

// The General page: folders, the launcher, every game's arguments
void SettingsDialog::generalPage(QVBoxLayout *v) {
    auto &s = cfg::settings();
    // the interface's language (launcher/language.h): "Follow the system" (with what that gives on this
    // PC), then each language with a catalogue, by its own name; saved as ui/language at OK, used from
    // the next start (the main window offers to restart)
    auto *langBox = new QGroupBox(tr("Language"));
    auto *fl = new QFormLayout(langBox);
    const QStringList have = language::available(cfg::rootDir());
    const QStringList system = language::systemLanguages();
    std::vector<std::string> sysList, haveList;
    for (const QString &c : system) sysList.push_back(c.toStdString());
    for (const QString &c : have) haveList.push_back(c.toStdString());
    const QString systemCode = QString::fromStdString(hk::lang::choose("", sysList, haveList));
    const auto ownName = [](const QString &code) {
        for (const hk::lang::Language &l : hk::lang::languages())
            if (code == l.code) return QString::fromUtf8(l.name);
        return code;
    };
    m_language = new QComboBox;
    // TR: %1 is the language that following the system gives on this PC, in its own name ("Français")
    m_language->addItem(tr("Follow the system (%1)").arg(ownName(systemCode)), QString());
    m_language->addItem("English", "en");
    for (const hk::lang::Language &l : hk::lang::languages())
        if (have.contains(QString(l.code))) m_language->addItem(QString::fromUtf8(l.name), QString(l.code));
    const QString saved = s.value("ui/language").toString();
    m_language->setCurrentIndex(std::max(0, m_language->findData(saved == "system" ? QString() : saved)));
    m_language->setProperty("hkStart", m_language->currentIndex());   // (saved at OK only when changed)
    m_language->setToolTip(tr("The language of HIRAKU's windows, of its game menu and of the boxes over a game. "
                              "It changes when HIRAKU starts again."));
    fl->addRow(tr("Language:"), m_language);
    // a Brazilian Portuguese system: Portugal's catalogue (decided: there is no Brazilian one)
    bool brazil = false;
    for (const QString &c : system) brazil = brazil || c.startsWith("pt-BR", Qt::CaseInsensitive) || c.startsWith("pt_BR", Qt::CaseInsensitive);
    if (brazil && systemCode == "pt_PT") {
        auto *note = new QLabel(tr("This system asks for Brazilian Portuguese: HIRAKU has Portuguese (Portugal), and uses it."));
        note->setWordWrap(true);
        note->setProperty("hkMuted", true);
        fl->addRow(note);
    }
    // a language this PC has no font for (a Linux PC without Noto Sans CJK): kept, with a warning
    auto *noFont = new QLabel;
    noFont->setWordWrap(true);
    fl->addRow(noFont);
    const auto fontCheck = [this, noFont, systemCode, ownName] {
        const QString code = m_language->currentData().toString().isEmpty() ? systemCode : m_language->currentData().toString();
        const QFontDatabase::WritingSystem ws = code == "ja" ? QFontDatabase::Japanese
                                                : code == "ko" ? QFontDatabase::Korean
                                                : code == "zh_CN" ? QFontDatabase::SimplifiedChinese
                                                : code == "zh_TW" ? QFontDatabase::TraditionalChinese
                                                                  : QFontDatabase::Any;
        const bool missing = ws != QFontDatabase::Any && QFontDatabase::families(ws).isEmpty();
        // TR: %1 is a language's own name ("日本語")
        noFont->setText(missing ? tr("⚠ This PC has no font for %1: its texts would show as empty boxes. Install one "
                                     "(Noto Sans CJK, from your system's packages).").arg(ownName(code))
                                : QString());
        noFont->setVisible(missing);
    };
    connect(m_language, &QComboBox::currentIndexChanged, this, fontCheck);
    fontCheck();
    v->addWidget(langBox);
    auto *paths = new QGroupBox(tr("Folders"));
    auto *fp = new QFormLayout(paths);
    m_root = new QLineEdit(s.value("paths/root").toString());
    m_root->setPlaceholderText(cfg::rootDir());
    fp->addRow(tr("HIRAKU folder:"), withBrowse(m_root, true, this));
    if (cfg::portable()) {   // the folder is the install: settings and saves are in its user/
        m_root->parentWidget()->setEnabled(false);
        m_root->setToolTip(tr("A portable install (it has a folder named user): it always runs from its own folder, "
                              "and keeps its settings and saves in user/."));
    }
    m_bin = new QLineEdit(s.value("paths/bin").toString());
    m_bin->setPlaceholderText(cfg::binDir());
    fp->addRow(tr("Game executables:"), withBrowse(m_bin, true, this));
    m_nvram = new QLineEdit(s.value("paths/nvram").toString());
    m_nvram->setPlaceholderText(cfg::portable() ? tr("default: user/data in the HIRAKU folder") : tr("default: ~/.local/share/hiraku"));
    fp->addRow(tr("Saves (NVRAM):"), withBrowse(m_nvram, true, this));
    m_place = new QComboBox;   // tools/ingest.py --place
    m_place->addItem(tr("Ask each time"), "ask");
    m_place->addItem(tr("Move them into the HIRAKU folder"), "move");
    m_place->addItem(tr("Leave them where they are"), "stay");
    m_place->setCurrentIndex(std::max(0, m_place->findData(s.value("ingest/place", "ask").toString())));
    m_place->setToolTip(tr("A game's CHDs, disks and ROM set (a PC game's folder): moved into the HIRAKU folder's dumps/ "
                           "(gone from where they were), or read where they are (keep them there). Its ROMs are copied in "
                           "either way."));
    fp->addRow(tr("Games' files when added:"), m_place);
    auto *start = new QGroupBox(tr("Starting a game"));
    auto *fs = new QFormLayout(start);
    m_fastBoot = new QCheckBox(tr("Fast start-up: through the game's start-up checks at full speed, to its title"));
    m_fastBoot->setChecked(s.value("game/fastBoot", true).toBool());
    m_fastBoot->setToolTip(tr("The game still boots as the cabinet does (its checks, its loading), only as fast as this "
                              "PC can run it, without sound, with a box over its picture saying so; from its title or "
                              "attract on, at its own speed. A button of the game (a coin, Test) goes back to its own "
                              "speed at once. The arcade games (not the PC ones); Tab, held, fast-forwards any time."));
    fs->addRow("", m_fastBoot);
    v->addWidget(start);
    // the game menu (runtime/menu/menu.h) and the way out to it from the cabinet (runtime/host/menukey.h)
    auto *menu = new QGroupBox(tr("Game menu: change games from the cabinet"));
    auto *fm = new QFormLayout(menu);
    m_menuOn = new QCheckBox(tr("5, 7 and 3 held together on a card reader's 10-key leave the game for the game menu"));
    m_menuOn->setChecked(s.value("menu/enabled", true).toBool());
    m_menuOn->setToolTip(tr("Either player's 10-key, the three keys together; on a cabinet with no 10-key, the Test and "
                            "Service switches held together. A box on the picture says so while they are held. The game "
                            "ends as when its window is closed (its saves written), and a list of the games opens, driven "
                            "by the cabinet's own controls: a turntable, pop'n's buttons, the panels, the 10-key itself. "
                            "The game chosen there starts; Escape closes the menu."));
    fm->addRow("", m_menuOn);
    m_menuHold = new QDoubleSpinBox;
    m_menuHold->setRange(0.5, 10.0);
    m_menuHold->setSingleStep(0.5);
    m_menuHold->setDecimals(1);
    m_menuHold->setSuffix(" s");
    m_menuHold->setValue(s.value("menu/hold", 2.0).toDouble());
    fm->addRow(tr("Held for:"), m_menuHold);
    m_menuKey = new QLineEdit(s.value("menu/key").toString());
    m_menuKey->setPlaceholderText(tr("none (e.g. key:F12 or joy0:button11)"));
    m_menuKey->setToolTip(tr("A key or a controller's button that does the same when held, as a controls file spells it: "
                             "key:F12, joy0:button11, joy[DAO FPS]:button9."));
    fm->addRow(tr("A key or button of your own:"), m_menuKey);
    m_menuGames = new QComboBox;
    m_menuGames->addItem(tr("The games this cabinet's controls play"), "fit");
    m_menuGames->addItem(tr("Every game"), "all");
    m_menuGames->setCurrentIndex(std::max(0, m_menuGames->findData(s.value("menu/games", "fit").toString())));
    m_menuGames->setToolTip(tr("This cabinet's: the games with the same controls as the one just left (on an IIDX cabinet, "
                               "every beatmania, IIDX and beatmania III)."));
    fm->addRow(tr("It lists:"), m_menuGames);
    m_menuAtStart = new QCheckBox(tr("HIRAKU starts in the game menu (a cabinet that boots into it)"));
    m_menuAtStart->setChecked(s.value("menu/atStart", false).toBool());
    m_menuAtStart->setToolTip(tr("With full screen (Display) and \"Hide the launcher while a game runs\" below, the cabinet's "
                                 "screen shows only the menu and the games. Escape in the menu comes back to this window."));
    fm->addRow("", m_menuAtStart);
    m_menuPictures = new QCheckBox(tr("Keep a picture of each game's title screen for the menu"));
    m_menuPictures->setChecked(s.value("menu/pictures", true).toBool());
    // TR: %1 is a folder
    m_menuPictures->setToolTip(tr("The first time a game reaches its title screen with no control touched, that picture is kept "
                                  "(in %1) and the menu shows it behind the list. "
                                  "A picture of your own there, named after the game's id (iidx16.png), is shown instead. "
                                  "Not every game has a title time yet: those show none until you give one.")
                                   .arg(QDir::toNativeSeparators(cfg::menuPicturesDir())));
    fm->addRow("", m_menuPictures);
    const auto menuRows = [this] {
        for (QWidget *w : {(QWidget *)m_menuHold, (QWidget *)m_menuKey, (QWidget *)m_menuGames, (QWidget *)m_menuAtStart, (QWidget *)m_menuPictures})
            w->setEnabled(m_menuOn->isChecked());
    };
    connect(m_menuOn, &QCheckBox::toggled, this, menuRows);
    menuRows();
    v->addWidget(menu);
    auto *adv = new QGroupBox(tr("Launcher"));
    auto *fx = new QFormLayout(adv);
    m_hideLauncher = new QCheckBox(tr("Hide the launcher while a game runs"));
    m_hideLauncher->setChecked(s.value("ui/hideWhileRunning", false).toBool());
    fx->addRow("", m_hideLauncher);
    m_checkUpdates = new QCheckBox(tr("Look for a new version (asks GitHub): when the launcher starts, and in games started without it"));
    m_checkUpdates->setChecked(s.value("updates/check", true).toBool());
    if (!upd::available()) {
        m_checkUpdates->setEnabled(false);
        m_checkUpdates->setToolTip(tr("A release package updates itself; this copy isn't one"));
    }
    fx->addRow("", m_checkUpdates);
    m_extra = new QLineEdit(s.value("game/extraArgs").toString());
    m_extra->setPlaceholderText(tr("e.g. --bios-seconds 21.65"));
    m_extra->setToolTip(tr("Command-line options for every game (docs/PLAY.md lists them). For one game alone: Game > "
                           "Arguments… (or right-click it)."));
    fx->addRow(tr("Arguments (every game):"), m_extra);
    v->addWidget(adv);
    v->addWidget(paths);
}

void SettingsDialog::accept() {
    auto &s = cfg::settings();
    const QString pcbid = m_pcbid->text().trimmed(), c0 = m_card[0]->text().trimmed(), c1 = m_card[1]->text().trimmed();
    if ((!pcbid.isEmpty() && pcbid.size() != 20) || (!c0.isEmpty() && c0.size() != 16) || (!c1.isEmpty() && c1.size() != 16)) {
        QMessageBox::warning(this, "e-AMUSEMENT", tr("A PCBID is 20 hex digits, and a card number 16 (E004...)."));
        return;
    }
    s.setValue("video/scale", m_scale->value());
    s.setValue("video/fullscreen", m_fullscreen->isChecked());
    s.setValue("video/splitScreens", m_splitScreens->isChecked());
    s.setValue("video/displaySync", m_displaySync->isChecked());
    s.setValue("video/smoothMotion", m_motion->currentData().toString().startsWith("smooth"));
    s.setValue("video/smoothMotionWhere", m_motion->currentData().toString() == "smooth-everywhere" ? "everywhere" : "songs");
    s.setValue("video/crtBeam", m_motion->currentData().toString() == "beam");
    s.setValue("video/crtBeamGamma", m_beamGamma->value());
    s.setValue("video/crtBeamGain", m_beamGain->value());
    s.setValue("video/statsOverlay", m_statsOverlay->currentData().toString());
    s.setValue("record/format", m_recFormat->currentData().toString());
    s.setValue("record/encoder", m_recEncoder->currentData().toString());
    s.setValue("record/scale", m_recScale->currentData().toInt());
    s.setValue("video/picture", m_picture->currentData().toString());
    s.setValue("video/pythonRenderer", m_pyRenderer->currentData().toString());
    for (const cfg::ShaderKey &k : cfg::shaderKeys()) {   // only what differs from the defaults
        if (m_shaderKeys.value(k.action) == k.binding) s.remove(QString("shaderKeys/") + k.action);
        else s.setValue(QString("shaderKeys/") + k.action, m_shaderKeys.value(k.action));
    }
    s.setValue("audio/ms", m_audioMs->value());
    s.setValue("audio/fixed", m_audioFixed->isChecked());
    s.setValue("system/priority", m_priority->isChecked());
    if (m_powerPlan) s.setValue("system/highPerformance", m_powerPlan->isChecked());
    s.setValue("audio/volumeDb", m_volume->value());
    s.setValue("audio/soundEveryMs", m_fineSound->isChecked());
    s.setValue("audio/evenVolume", m_evenVolume->isChecked());
    s.setValue("audio/device", m_audioDevice->currentData().toString());
    if (m_audioExclusive) s.setValue("audio/exclusive", m_audioExclusive->isChecked());
    s.setValue("audio/buffer", m_audioBuffer->currentData().toInt());
    s.setValue("audio/soundfont", m_soundfont->text().trimmed());
    s.setValue("music/folder", m_musicFolder->text().trimmed());
    s.setValue("music/synth", m_musicSynth->currentData().toString());
    s.setValue("music/soundfont", m_musicFont->text().trimmed());
    s.setValue("music/shuffle", m_musicShuffle->isChecked());
    s.setValue("lights/socket", m_socket->isChecked());
    s.setValue("lights/overlay", m_lightsOverlay->isChecked());
    s.setValue("paths/root", m_root->text().trimmed());
    s.setValue("paths/bin", m_bin->text().trimmed());
    s.setValue("paths/nvram", m_nvram->text().trimmed());
    s.setValue("ui/hideWhileRunning", m_hideLauncher->isChecked());
    s.setValue("game/fastBoot", m_fastBoot->isChecked());
    s.setValue("menu/enabled", m_menuOn->isChecked());
    s.setValue("menu/hold", m_menuHold->value());
    s.setValue("menu/key", m_menuKey->text().trimmed());
    s.setValue("menu/games", m_menuGames->currentData().toString());
    s.setValue("menu/atStart", m_menuAtStart->isChecked());
    s.setValue("menu/pictures", m_menuPictures->isChecked());
    s.setValue("ingest/place", m_place->currentData().toString());
    s.setValue("updates/check", m_checkUpdates->isChecked());
    s.setValue("game/extraArgs", m_extra->text().trimmed());
    if (m_language->currentIndex() != m_language->property("hkStart").toInt()) {
        s.setValue("ui/language", m_language->currentData().toString());
        m_languageChanged = true;
    }
    s.remove("eamuse/server");   // now eamuse/url
    s.setValue("eamuse/url", m_eaUrl->text().trimmed());
    s.setValue("eamuse/smartea", m_smartea->isChecked());
    s.setValue("eamuse/pcbid", m_pcbid->text().trimmed().toUpper());
    s.setValue("eamuse/key", m_eaKey->text().trimmed());
    for (int n = 0; n < 2; n++) s.setValue(QString("eamuse/card%1").arg(n), m_card[n]->text().trimmed().toUpper());
    s.setValue("eamuse/autocard", m_autocard->currentData().toString());
    s.setValue("eamuse/autopin", m_autopin->isChecked());
    s.setValue("game/pressTime", m_pressTime->isChecked());
    s.setValue("game/judgeOffset", m_judgeOffset->value());
    for (int n = 0; n < 2; n++) s.setValue(QString("readers/serial%1").arg(n), m_reader[n]->currentText().trimmed());
    s.setValue("readers/scard", m_scard->isChecked());
    s.setValue("readers/cardio", m_cardio->isChecked());
    s.setValue("cabinet/bio2", m_bio2->currentText().trimmed());
    s.setValue("cabinet/popnio", m_popnio->currentText().trimmed());
    s.setValue("cabinet/popnioDll", m_popnioDll->text().trimmed());
    s.sync();
    QDialog::accept();
}
