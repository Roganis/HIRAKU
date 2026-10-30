// SPDX-License-Identifier: GPL-3.0-or-later
#include "settingsdialog.h"

#include <algorithm>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSettings>
#include <QSpinBox>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyle>
#include <QTabWidget>
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
#include "inputs.h"
#include "updater.h"

#include <QFile>
#include <QMap>
#include <QTextStream>
#include "picturedialog.h"

namespace {

// A line edit with a "Browse…" button
QWidget *withBrowse(QLineEdit *e, bool directory, QWidget *parent, const QString &title = "Choose a SoundFont",
                    const QString &filter = "SoundFonts (*.sf2 *.sf3);;All files (*)") {
    auto *w = new QWidget(parent);
    auto *h = new QHBoxLayout(w);
    h->setContentsMargins(0, 0, 0, 0);
    h->addWidget(e, 1);
    auto *b = new QPushButton("Browse…");
    b->setAutoDefault(false);
    h->addWidget(b);
    QObject::connect(b, &QPushButton::clicked, w, [e, directory, parent, title, filter] {
        QString p = directory ? QFileDialog::getExistingDirectory(parent, "Choose a folder", e->text())
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
    auto *b = new QPushButton("Pick");
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
        menu.addAction("New card number", [e] {
            QString n = "E004";   // as an e-AMUSEMENT PASS: E004 and 12 hex digits
            for (int i = 0; i < 12; i++) n += QString::number(QRandomGenerator::global()->bounded(16), 16).toUpper();
            e->setText(n);
        });
        menu.addAction("None", [e] { e->clear(); });
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

SettingsDialog::SettingsDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle("Settings");
    auto &s = cfg::settings();
    // a tab for each kind of setting; a tab's groups scroll (a small screen: 640x480 too,
    // launcher/fit.h), OK and Cancel stay below
    auto *tabs = new QTabWidget;
    QList<QWidget *> bodies;
    auto page = [&](const QString &name, const QString &about = QString()) {
        auto *body = new QWidget;
        auto *l = new QVBoxLayout(body);
        if (!about.isEmpty()) {
            auto *note = new QLabel(about);
            note->setWordWrap(true);
            l->addWidget(note);
        }
        auto *scroll = new QScrollArea;
        scroll->setWidget(body);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        tabs->addTab(scroll, name);
        bodies << body;
        return l;
    };

    displayPage(page("Display"));
    audioPage(page("Audio"));
    // as spicecfg's Network page, with its option names, so a spice2x setup carries over
    eamusePage(page("e-AMUSEMENT", "For the Konami Python games (pop'n music 9-14, DrumMania V and V2) and the PC games "
                                "(pop'n music 15-18), with spice2x's option names. The game's e-AMUSEMENT must be ON "
                                "in its test mode."));
    cabinetPage(page("Cabinet I/O"));
    generalPage(page("General"));

    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, this, &SettingsDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    // number boxes as wide as their numbers, not the dialog
    for (QSpinBox *b : {m_scale, m_volume, m_audioMs}) b->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    QSize most;
    for (QWidget *body : bodies) {
        static_cast<QVBoxLayout *>(body->layout())->addStretch(1);   // the groups at the top of their tab
        // long lists (sound devices, readers) shrink to the dialog instead of widening it
        for (QComboBox *c : body->findChildren<QComboBox *>()) {
            c->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
            c->setMinimumContentsLength(16);
        }
        most = most.expandedTo(body->sizeHint());
    }
    tabs->setCurrentIndex(std::clamp(s.value("ui/settingsTab", 0).toInt(), 0, tabs->count() - 1));
    connect(tabs, &QTabWidget::currentChanged, this, [](int k) { cfg::settings().setValue("ui/settingsTab", k); });
    auto *outer = new QVBoxLayout(this);
    outer->addWidget(tabs);
    outer->addWidget(bb);
    // as the largest tab, as the screen allows (fit.h shrinks it)
    resize(most.width() + tabs->style()->pixelMetric(QStyle::PM_ScrollBarExtent) + 40, most.height() + 110);
}

// The Display tab: the window, frames and picture, the shader controls
void SettingsDialog::displayPage(QVBoxLayout *v) {
    auto &s = cfg::settings();
    auto *video = new QGroupBox("Window");
    auto *fv = new QFormLayout(video);
    m_scale = new QSpinBox;
    m_scale->setRange(1, 6);
    m_scale->setSuffix("×");
    m_scale->setValue(s.value("video/scale", 2).toInt());
    fv->addRow("Window size:", m_scale);
    m_fullscreen = new QCheckBox("Start in fullscreen (F11 or Alt+Enter toggles)");
    m_fullscreen->setChecked(s.value("video/fullscreen", false).toBool());
    fv->addRow("", m_fullscreen);
    m_splitScreens = new QCheckBox("Keyboardmania: each player's monitor in its own window (two displays)");
    m_splitScreens->setChecked(s.value("video/splitScreens", false).toBool());
    m_splitScreens->setToolTip("The cabinet has a monitor for each player. Player 2's window opens on the next display; "
                               "with fullscreen, each fills its own. F11 toggles the window with the keyboard.");
    fv->addRow("", m_splitScreens);
    v->addWidget(video);
    auto *frames = new QGroupBox("Frames and picture");
    fv = new QFormLayout(frames);
    m_displaySync = new QCheckBox("Smooth frames on a 60, 120 or 240 Hz display");
    m_displaySync->setChecked(s.value("video/displaySync", true).toBool());
    m_displaySync->setToolTip("The games run at their own rates (pop'n 58.49 Hz, beatmania III 59.94 Hz), which a "
                              "fixed-refresh display can't show evenly. On: each frame stays up the same number of "
                              "refreshes, and the game runs at the display's rate (pop'n 2.6 % faster, its sound "
                              "with it). Off: the exact speed, for a variable-refresh (FreeSync, G-Sync) display.");
    fv->addRow("", m_displaySync);
    // DJ Main, Firebeat, Twinkle and System 573 games, with smooth frames on (Windows, or Wayland
    // with the fifo protocol: KWin 6.7); docs/SMOOTH.md
    m_motion = new QComboBox;
    m_motion->addItem("Each frame held for its refreshes", "held");
    m_motion->addItem("Smooth motion: every refresh a picture of its own moment", "smooth");
    m_motion->addItem("CRT beam: each refresh the part of the tube the beam sweeps (120 Hz and up)", "beam");
    m_motion->setCurrentIndex(s.value("video/crtBeam", false).toBool()          ? 2
                              : s.value("video/smoothMotion", false).toBool() ? 1
                                                                                : 0);
    m_motion->setToolTip("DJ Main, Firebeat, Twinkle and System 573 games, and the PC beatmania IIDX games, with smooth "
                         "frames on, on a display faster than the game. Smooth motion: the game at its exact speed on any "
                         "display (60, 144, 240 Hz), the notes and other moving objects carried on along their move at "
                         "each refresh (the PC IIDX games: the notes only, the rest as the game drew it). CRT beam "
                         "(Blur Busters' CRT Beam Simulator): each refresh lights the part of the tube the electron "
                         "beam sweeps then and lets the rest fade, for a CRT's motion clarity; best at 240 Hz, SDR, "
                         "with the monitor's ABL/APL and its own strobing off. Windows, or Wayland with the fifo "
                         "protocol (KWin 6.7).");
    fv->addRow("Motion:", m_motion);
    m_beamGamma = new QDoubleSpinBox;
    m_beamGamma->setRange(1.8, 2.8);
    m_beamGamma->setSingleStep(0.1);
    m_beamGamma->setDecimals(2);
    m_beamGamma->setValue(s.value("video/crtBeamGamma", 2.2).toDouble());
    m_beamGamma->setToolTip("Your display's gamma (2.2 for most; 2.4 if set so). A mismatch shows as horizontal bands "
                            "and washed-out colours.");
    m_beamGain = new QDoubleSpinBox;
    m_beamGain->setRange(0.3, 1.0);
    m_beamGain->setSingleStep(0.05);
    m_beamGain->setDecimals(2);
    m_beamGain->setValue(s.value("video/crtBeamGain", 0.7).toDouble());
    m_beamGain->setToolTip("Brightness against motion blur: lower is sharper and darker. 0.7 at 240 Hz, about 0.5 at 120 Hz.");
    auto *beamRow = new QWidget;
    auto *bh = new QHBoxLayout(beamRow);
    bh->setContentsMargins(0, 0, 0, 0);
    bh->addWidget(new QLabel("Display gamma"));
    bh->addWidget(m_beamGamma);
    bh->addSpacing(12);
    bh->addWidget(new QLabel("Brightness"));
    bh->addWidget(m_beamGain);
    bh->addStretch(1);
    fv->addRow("CRT beam:", beamRow);
    // motion and the beam need smooth frames (each frame held for whole refreshes) on
    auto beamShown = [this, beamRow] {
        m_motion->setEnabled(m_displaySync->isChecked());
        beamRow->setEnabled(m_displaySync->isChecked() && m_motion->currentData().toString() == "beam");
    };
    connect(m_motion, &QComboBox::currentIndexChanged, this, beamShown);
    connect(m_displaySync, &QCheckBox::toggled, this, beamShown);
    beamShown();
    m_picture = new QComboBox;
    m_picture->addItem("Off: sharp pixels", "off");
    m_picture->addItem("CRT light: crt-guest-advanced-fast", "light");
    m_picture->addItem("CRT full: crt-royale", "full");
    m_picture->addItem("CRT analog: Lola Retro (the heaviest)", "lola");
    m_picture->setCurrentIndex(std::max(0, m_picture->findData(picture::globalMode())));
    m_picture->setToolTip("For all games; a game can have its own (Picture, in the game's menu). In the game, F10 cycles them.");
    auto *pic = new QWidget;
    auto *ph = new QHBoxLayout(pic);
    ph->setContentsMargins(0, 0, 0, 0);
    ph->addWidget(m_picture, 1);
    auto *adjust = new QPushButton("Adjust…");
    adjust->setAutoDefault(false);
    ph->addWidget(adjust);
    connect(adjust, &QPushButton::clicked, this, [this] {
        cfg::settings().setValue("video/picture", m_picture->currentData().toString());
        PictureDialog d(QString(), QString(), false, this);
        if (d.exec() == QDialog::Accepted) m_picture->setCurrentIndex(std::max(0, m_picture->findData(picture::globalMode())));
    });
    fv->addRow("Picture:", pic);
    v->addWidget(frames);
    auto *py = new QGroupBox("Konami Python games (pop'n music 9-14, DrumMania V and V2)");
    auto *fy = new QFormLayout(py);
    m_pyRenderer = new QComboBox;
    // (not OpenGL: PCSX2's needs a window of its own, and the core draws without one)
    m_pyRenderer->addItem("Vulkan (the default)", "vk");
#ifdef Q_OS_WIN
    m_pyRenderer->addItem("Direct3D 11", "dx11");
    m_pyRenderer->addItem("Direct3D 12", "dx12");
#endif
    m_pyRenderer->addItem("Software: the CPU draws (the most CPU)", "sw");
    m_pyRenderer->setCurrentIndex(std::max(0, m_pyRenderer->findData(s.value("video/pythonRenderer", "vk").toString())));
#ifdef Q_OS_WIN
    m_pyRenderer->setToolTip("How PCSX2's GS draws the PS2's pictures (--renderer). Vulkan takes the least CPU. If these "
                             "games crash or show nothing on this PC, its graphics driver may not get on with Vulkan: "
                             "try Direct3D 11, then Software (which still shows its picture through Direct3D).");
#else
    m_pyRenderer->setToolTip("How PCSX2's GS draws the PS2's pictures (--renderer). Vulkan takes the least CPU. If these "
                             "games crash or show nothing on this PC, try Software (it still needs a Vulkan driver to "
                             "show its picture).");
#endif
    fy->addRow("Graphics:", m_pyRenderer);
    v->addWidget(py);
    // the shader's settings in the game (runtime/host/shaderview.h): a key or a controller's
    // button for each of its controls, as in Controls
    auto *sk = new QGroupBox("The shader's settings while playing (F8)");
    auto *gk = new QGridLayout(sk);
    auto *skNote = new QLabel("A panel beside the picture: pick a setting, change it, see it at once, save it. A key or a "
                              "controller's button for each (with the keyboard, Shift does ten steps, or saves for this game).");
    skNote->setWordWrap(true);
    gk->addWidget(skNote, 0, 0, 1, 3);
    QList<QPair<QString, QPushButton *>> skButtons;
    auto shown = [](const QString &b) { return b.isEmpty() ? QString("none") : describeBinding(b); };
    int row = 1;
    for (const cfg::ShaderKey &k : cfg::shaderKeys()) {
        const QString action = k.action, label = k.label;
        m_shaderKeys[action] = s.value("shaderKeys/" + action, QString(k.binding)).toString();
        auto *b = new QPushButton(shown(m_shaderKeys[action]));
        b->setAutoDefault(false);
        b->setToolTip("Click, then press the key or the controller's button");
        connect(b, &QPushButton::clicked, this, [this, b, action, label, shown] {
            CaptureDialog d(label + "  (the shader's settings, F8)", false, this);
            if (d.exec() != CaptureDialog::Captured || d.result().isEmpty()) return;
            m_shaderKeys[action] = d.result();
            b->setText(shown(d.result()));
        });
        auto *none = new QToolButton;
        none->setText("✕");
        none->setAutoRaise(true);
        none->setToolTip("No control for it");
        connect(none, &QToolButton::clicked, this, [this, b, action, shown] {
            m_shaderKeys[action].clear();
            b->setText(shown(QString()));
        });
        gk->addWidget(new QLabel(label + ":"), row, 0);
        gk->addWidget(b, row, 1);
        gk->addWidget(none, row, 2);
        skButtons.append({action, b});
        row++;
    }
    auto *skDefaults = new QPushButton("Defaults");
    skDefaults->setAutoDefault(false);
    connect(skDefaults, &QPushButton::clicked, this, [this, skButtons, shown] {
        for (const cfg::ShaderKey &k : cfg::shaderKeys()) m_shaderKeys[k.action] = k.binding;
        for (const auto &[action, b] : skButtons) b->setText(shown(m_shaderKeys[action]));
    });
    gk->addWidget(skDefaults, row, 1, Qt::AlignLeft);
    gk->setColumnStretch(1, 1);
    v->addWidget(sk);
}

// The Audio tab: the output, its timing, Keyboardmania's SoundFont
void SettingsDialog::audioPage(QVBoxLayout *v) {
    auto &s = cfg::settings();
    auto *audio = new QGroupBox("Output");
    auto *fa = new QFormLayout(audio);
    auto *latency = new QGroupBox("Latency");
    auto *fq = new QFormLayout(latency);
    m_audioMs = new QSpinBox;
    m_audioMs->setRange(2, 30);
    m_audioMs->setSuffix(" ms");
    m_audioMs->setValue(s.value("audio/ms", 8).toInt());
    m_audioMs->setToolTip("The sound's lead over the device, and its least: it grows by itself while frames keep "
                          "coming late on this PC, and comes back down when they stop (docs/PLAY.md, Latency).");
    m_audioFixed = new QCheckBox("Fixed: never more (a rare break in the sound instead)");
    m_audioFixed->setChecked(s.value("audio/fixed", false).toBool());
    m_audioFixed->setToolTip("The queue stays at this setting all game long: the least delay on every sound, and "
                             "a click whenever a frame comes later than it covers.");
    m_volume = new QSpinBox;
    m_volume->setRange(-20, 20);
    m_volume->setSuffix(" dB");
    m_volume->setValue(s.value("audio/volumeDb", 0).toInt());
    m_volume->setToolTip("Louder or quieter than the machine makes it, for every game (6 dB: twice as loud). "
                         "Some games, Keyboardmania among them, are quiet next to others; as in MAME.");
    fq->addRow("Audio queue:", m_audioMs);
    fq->addRow("", m_audioFixed);
    m_priority = new QCheckBox("Ahead of other programs (higher priority, and GameMode if installed)");
    m_priority->setChecked(s.value("system/priority", true).toBool());
    m_priority->setToolTip("The game's threads run ahead of other programs (a browser, a build), so a busy PC "
                           "doesn't delay its frames and sound; the sound server and the desktop keep theirs. "
                           "A sound card of its own (Output) is written by a real-time thread either way.");
    fq->addRow("", m_priority);
    m_audioDevice = new QComboBox;
    const QString device = s.value("audio/device").toString();
#ifdef _WIN32
    m_audioDevice->addItem("Windows' default output", QString());
    for (const QString &name : windowsPlaybackDevices()) m_audioDevice->addItem(name, name);
    for (const QString &name : asioDrivers()) m_audioDevice->addItem("ASIO: " + name, "asio:" + name);
    if (!device.isEmpty() && m_audioDevice->findData(device) < 0) m_audioDevice->addItem(device + " (not found now)", device);
    m_audioDevice->setToolTip("The sound device the game plays on. ASIO: the card through its own ASIO driver, with "
                              "no Windows audio in between; its buffer is the one set in the driver's control panel "
                              "unless Device buffer below says otherwise. If the driver won't start, the game uses "
                              "Windows' default output.");
    fa->addRow("Output:", m_audioDevice);
    m_audioExclusive = new QCheckBox("Exclusive (WASAPI): the device alone, lowest latency");
    m_audioExclusive->setChecked(s.value("audio/exclusive", false).toBool());
    m_audioExclusive->setToolTip("The game has the device to itself in WASAPI exclusive mode, with no Windows mixer in "
                                 "between and its buffer as small as the device allows (Device buffer below); other "
                                 "programs are silent on it meanwhile. If the device refuses (another program has it "
                                 "alone, or Windows' sound settings don't allow exclusive mode), the game plays shared.");
    fa->addRow("", m_audioExclusive);
    auto exclusiveFor = [this] { m_audioExclusive->setEnabled(!m_audioDevice->currentData().toString().startsWith("asio:")); };
    connect(m_audioDevice, &QComboBox::currentIndexChanged, this, exclusiveFor);
#else
    m_audioDevice->addItem("The system's default (the sound server)", QString());
    for (const auto &[name, label] : alsaPlaybackDevices()) m_audioDevice->addItem("Alone: " + label, name);
    if (!device.isEmpty() && m_audioDevice->findData(device) < 0) m_audioDevice->addItem("Alone: " + device, device);
    m_audioDevice->setToolTip("A sound card alone: the game opens it through ALSA with no sound server in between, for "
                              "the lowest latency; nothing else plays on it meanwhile. If it's busy (the sound server lets "
                              "go of a card a few seconds after the last sound on it), the game uses the default instead.");
    fa->addRow("Output:", m_audioDevice);
#endif
    m_audioDevice->setCurrentIndex(std::max(0, m_audioDevice->findData(device)));
#ifdef _WIN32
    exclusiveFor();
#endif
    m_audioBuffer = new QComboBox;
    for (int f : {0, 32, 48, 64, 96, 128, 192, 256, 512})
        m_audioBuffer->addItem(f ? QString("%1 frames (%2 ms)").arg(f).arg(f * 1000.0 / 48000, 0, 'f', 1)
#ifdef _WIN32
                                 : QString("Default (128 frames, 2.7 ms; ASIO: the driver's own)"), f);
#else
                                 : QString("Default (128 frames, 2.7 ms)"), f);
#endif
    m_audioBuffer->setCurrentIndex(std::max(0, m_audioBuffer->findData(s.value("audio/buffer", 0).toInt())));
    m_audioBuffer->setToolTip("The sound device's buffer (the sound server's quantum; on Windows in exclusive mode the device's own; with a Linux card alone, its "
                              "period: HIRAKU keeps two of them queued on the card, four on a USB card, which needs them). Smaller is "
                              "sooner, until it crackles; the audio queue above tunes itself to it. Times at 48 kHz.");
    fa->addRow("Device buffer:", m_audioBuffer);
    fa->addRow("Volume:", m_volume);
    m_fineSound = new QCheckBox("Sound every millisecond on System 573 and Twinkle (not MAME's)");
    m_fineSound->setChecked(s.value("audio/soundEveryMs", true).toBool());
    m_fineSound->setToolTip("MAME hands these machines' sound on 50 times a second, which adds up to 20 ms of "
                            "latency, uneven from moment to moment. Every millisecond is lower and steadier. On "
                            "Twinkle (beatmania IIDX) the keysounds then start when the game triggers them, so the "
                            "sound no longer matches MAME's. On by default; off is MAME's timing (--mame-sound; "
                            "docs/PLAY.md, Latency).");
    fq->addRow("", m_fineSound);
    m_pressTime = new QCheckBox("Presses judged when they happened (pop'n music 15-18, IIDX EMPRESS)");
    m_pressTime->setChecked(s.value("game/pressTime", false).toBool());
    m_pressTime->setToolTip("The game judges a press at the frame it reads the buttons in, up to a frame (17 ms) after "
                            "it happened, as on its cabinet. On: each press is judged at the moment the controller "
                            "gave it. IIDX judges in whole frames: there, each press counts in the frame it happened "
                            "in, however late the game read it (--press-time; docs/PLAY.md, PC).");
    fq->addRow("", m_pressTime);
    m_judgeOffset = new QSpinBox;
    m_judgeOffset->setRange(-200, 200);
    m_judgeOffset->setSuffix(" ms");
    m_judgeOffset->setValue(s.value("game/judgeOffset", 0).toInt());
    m_judgeOffset->setToolTip("pop'n music 15-18, IIDX EMPRESS: each press judged this much earlier than it came "
                              "(--judge-offset), for a TV or a sound system that shows or plays late, so presses made in "
                              "time with it count as in time. Positive when the notes look late; pop'n judges in 4 ms "
                              "steps, IIDX in frames (each press moved by whole frames from its own time: part of a "
                              "frame on average, with presses judged when they happened).");
    fq->addRow("Judgement offset:", m_judgeOffset);
    v->addWidget(audio);
    v->addWidget(latency);
    auto *kbm = new QGroupBox("Keyboardmania");
    auto *fk = new QFormLayout(kbm);
    m_soundfont = new QLineEdit(s.value("audio/soundfont").toString());
    m_soundfont->setPlaceholderText("default: the system's General MIDI SoundFont");
    m_soundfont->setToolTip("The instruments of Keyboardmania's MIDI out (its keyboards' sound when a song plays them).");
    fk->addRow("SoundFont:", withBrowse(m_soundfont, false, this));
    v->addWidget(kbm);
}

// The e-AMUSEMENT tab: the server, the PCBID and cards, the card readers
void SettingsDialog::eamusePage(QVBoxLayout *v) {
    auto &s = cfg::settings();
    auto *ea = new QGroupBox("Server");
    auto *fe = new QFormLayout(ea);
    m_eaUrl = new QLineEdit(s.value("eamuse/url", s.value("eamuse/server")).toString());
    m_eaUrl->setPlaceholderText("empty: the server inside HIRAKU (-ea)");
    m_eaUrl->setToolTip("-url. Empty: the server inside HIRAKU, which keeps its data in the game's save folder. "
                        "host:port or http://host:port/path: a server on the network, such as hiraku-eamuse on another "
                        "PC (docs/EAMUSE3.md). The game's e-AMUSEMENT must be ON in test mode.");
    fe->addRow("EA Service URL:", m_eaUrl);
    m_smartea = new QCheckBox("The server inside HIRAKU when that one doesn't answer (-smartea)");
    m_smartea->setChecked(s.value("eamuse/smartea", false).toBool());
    fe->addRow("", m_smartea);
    const QRegularExpression hex20("[0-9A-Fa-f]{0,20}"), hex16("[0-9A-Fa-f]{0,16}");
    m_pcbid = new QLineEdit(s.value("eamuse/pcbid").toString());
    m_pcbid->setValidator(new QRegularExpressionValidator(hex20, m_pcbid));
    m_pcbid->setPlaceholderText("the game's own (pop'n 9: 01401401000100045FEF)");
    m_pcbid->setToolTip("-p: the cabinet's PCBID, 20 hex digits, for a server that only takes the machines it knows.");
    fe->addRow("PCBID:", m_pcbid);
    v->addWidget(ea);
    auto *cards = new QGroupBox("Cards");
    fe = new QFormLayout(cards);
    for (int n = 0; n < 2; n++) {
        m_card[n] = new QLineEdit(s.value(QString("eamuse/card%1").arg(n)).toString());
        m_card[n]->setValidator(new QRegularExpressionValidator(hex16, m_card[n]));
        m_card[n]->setPlaceholderText(n ? "card1.txt (pop'n has one reader: not used)" : "card0.txt, or the save folder's card.bin");
        m_card[n]->setToolTip(QString("-card%1: the card's number, 16 hex digits (E004...), the same as in spice2x, so a "
                                      "server knows the player. Empty: card%1.txt in the save folder, if there is one; "
                                      "else the save folder's card.bin, numbered by the server.%2")
                                  .arg(n)
                                  .arg(n ? " pop'n has one card reader, player 1's." : ""));
        fe->addRow(QString("Player %1 Card:").arg(n + 1), withPick(m_card[n], this));
    }
    m_autocard = new QComboBox;
    for (const char *v : {"off", "p1", "p2", "both"}) m_autocard->addItem(v, v);
    m_autocard->setCurrentIndex(std::max(0, m_autocard->findData(s.value("eamuse/autocard", "off").toString())));
    m_autocard->setToolTip("-autocard: the card is put in whenever the reader waits for one, so Insert needn't be pressed.");
    fe->addRow("Auto Card Insert:", m_autocard);
    m_autopin = new QCheckBox("Type the card's PIN (pop'n music 14-18)");
    m_autopin->setChecked(s.value("eamuse/autopin", false).toBool());
    m_autopin->setToolTip("-autopin: when the game asks for a known card's PIN, HIRAKU types it on the 10-key, as the "
                          "server inside HIRAKU keeps it (not with a server on the network).");
    fe->addRow("", m_autopin);
    v->addWidget(cards);

    // real readers, as spice2x's -reader, -scard and -cardio (runtime/host/readers.h)
    auto *rd = new QGroupBox("Card readers (pop'n music 9-18, GuitarFreaks, DrumMania)");
    auto *fr = new QFormLayout(rd);
    const QStringList ports = serialPorts();
    for (int n = 0; n < 2; n++) {
        m_reader[n] = new QComboBox;
        m_reader[n]->setEditable(true);
        m_reader[n]->addItem("");
        m_reader[n]->addItems(ports);
        m_reader[n]->setCurrentText(s.value(QString("readers/serial%1").arg(n)).toString());
#ifdef Q_OS_WIN
        m_reader[n]->lineEdit()->setPlaceholderText("none (or type its port: COM3)");
#else
        m_reader[n]->lineEdit()->setPlaceholderText("none");
#endif
        m_reader[n]->setToolTip("-reader: a Konami e-AMUSEMENT reader with its 10-key on a serial port (a USB-serial "
                                "adapter). A card held to it is the player's card in the game; its 10-key is the "
                                "game's (00 is Enter where the game has no 00).");
        fr->addRow(QString("Player %1 serial reader:").arg(n + 1), m_reader[n]);
    }
    m_scard = new QCheckBox("NFC readers (ACR122U and the like) through PC/SC (-scard): the first is player 1's");
    m_scard->setChecked(s.value("readers/scard", false).toBool());
#ifdef Q_OS_WIN
    m_scard->setToolTip("Through Windows' own smart card service. Cards that aren't e-AMUSEMENT PASSes (FeliCa, phones) "
                        "get an E00401 number, as spice2x's -scardfix.");
#else
    m_scard->setToolTip("Needs the pcscd service (systemctl enable --now pcscd.socket). Cards that aren't e-AMUSEMENT "
                        "PASSes (FeliCa, phones) get an E00401 number, as spice2x's -scardfix.");
#endif
    fr->addRow("", m_scard);
    m_cardio = new QCheckBox("cardio readers (USB HID, -cardio): the first is player 1's");
    m_cardio->setChecked(s.value("readers/cardio", false).toBool());
    m_cardio->setToolTip("Read through /dev/hidraw: a udev rule lets you open it (docs/PLAY.md, Card readers).");
#ifdef Q_OS_WIN
    m_cardio->setToolTip("Readers that show themselves as cardio's HID device (usage page FFCA): the first two found.");
#endif
    fr->addRow("", m_cardio);
    v->addWidget(rd);
}

// The Cabinet I/O tab: the I/O boards, the lights
void SettingsDialog::cabinetPage(QVBoxLayout *v) {
    auto &s = cfg::settings();
    auto *lights = new QGroupBox("Lights");
    auto *fl = new QVBoxLayout(lights);
    m_socket = new QCheckBox("Publish lamps on a local socket (live view in Lights, external LED tools)");
    m_socket->setChecked(s.value("lights/socket", true).toBool());
    fl->addWidget(m_socket);
    m_lightsOverlay = new QCheckBox("Show the game's lights over its picture (F9 in the game shows or hides them)");
    m_lightsOverlay->setChecked(s.value("lights/overlay", false).toBool());
    m_lightsOverlay->setToolTip("Every lamp the game drives, lit or not, grouped as the cabinet has them, and how often "
                                "they change: for seeing what a game does with its lights without the cabinet.");
    fl->addWidget(m_lightsOverlay);
    auto *path = new QLabel("Socket: <tt>" + cfg::lightsSocket().toHtmlEscaped() + "</tt> (protocol: docs/LIGHTS.md)");
    path->setTextInteractionFlags(Qt::TextSelectableByMouse);
    fl->addWidget(path);

    auto *cab = new QGroupBox("I/O boards");
    auto *fc = new QFormLayout(cab);
    m_bio2 = new QComboBox;
    m_bio2->setEditable(true);
    m_bio2->addItem("");
    m_bio2->addItems(serialPorts());
    m_bio2->setCurrentText(s.value("cabinet/bio2").toString());
    m_bio2->lineEdit()->setPlaceholderText("none");
    m_bio2->setToolTip("--bio2: beatmania IIDX's BIO2 I/O board (IIDX 25-26's BI2A firmware) on its USB serial port: "
                       "the cabinet's keys, turntables, buttons, effector and coin, and its lamps and ticker, for "
                       "beatmania IIDX, III and the DJ Main games.");
    fc->addRow("IIDX BIO2 board:", m_bio2);
    m_popnio = new QComboBox;
    m_popnio->setEditable(true);
    m_popnio->addItems({"", "auto"});
    m_popnio->setCurrentText(s.value("cabinet/popnio").toString());
    m_popnio->lineEdit()->setPlaceholderText("none");
#ifdef Q_OS_WIN
    m_popnio->setToolTip("--popn-io: a pop'n music cabinet's USB I/O board (the one pop'n music 15 and later drive through "
                         "ezusb.dll, or a board that copies it, such as PopnIO): its buttons, TEST, SERVICE and coin, and its "
                         "button, top and side lamps, for every pop'n music game. On Windows choose auto: the ezusb.dll below "
                         "finds the board (Konami's needs Cypress's CyUSB driver installed for it). The game's log says "
                         "how it goes, in its lines starting popn-io.");
#else
    m_popnio->setToolTip("--popn-io: a pop'n music cabinet's USB I/O board (the one pop'n music 15 and later drive through "
                         "ezusb.dll, or a board that copies it, such as PopnIO): its buttons, TEST, SERVICE and coin, and its "
                         "button, top and side lamps, for every pop'n music game. auto takes the first; a USB port "
                         "(as in /sys/bus/usb/devices, e.g. 3-2.1) chooses one.");
#endif
    fc->addRow("pop'n music I/O board:", m_popnio);
    m_popnioDll = new QLineEdit(s.value("cabinet/popnioDll").toString());
#ifdef Q_OS_WIN
    m_popnioDll->setPlaceholderText("the ezusb.dll that drives it: the game's, or a replacement");
    m_popnioDll->setToolTip("--popn-io-dll: HIRAKU drives the board through it, as the games do: the game's ezusb.dll "
                            "(pop'n music 15 or later, with Cypress's driver installed), or a replacement such as "
                            "UltimatePopnController's.");
#else
    m_popnioDll->setPlaceholderText("only for a board with no firmware: the game's ezusb.dll");
    m_popnioDll->setToolTip("--popn-io-dll: Konami's board keeps no firmware; the game loads it at every start "
                            "from its ezusb.dll (pop'n music 15 or later), and HIRAKU does the same from this file.");
#endif
    fc->addRow("  its ezusb.dll:", withBrowse(m_popnioDll, false, this, "Choose an ezusb.dll", "ezusb.dll (ezusb*.dll);;All files (*)"));
    v->addWidget(cab);
    v->addWidget(lights);
}

// The General tab: folders, the launcher, every game's arguments
void SettingsDialog::generalPage(QVBoxLayout *v) {
    auto &s = cfg::settings();
    auto *paths = new QGroupBox("Folders");
    auto *fp = new QFormLayout(paths);
    m_root = new QLineEdit(s.value("paths/root").toString());
    m_root->setPlaceholderText(cfg::rootDir());
    fp->addRow("HIRAKU folder:", withBrowse(m_root, true, this));
    if (cfg::portable()) {   // the folder is the install: settings and saves are in its user/
        m_root->parentWidget()->setEnabled(false);
        m_root->setToolTip("A portable install (it has a folder named user): it always runs from its own folder, "
                           "and keeps its settings and saves in user/.");
    }
    m_bin = new QLineEdit(s.value("paths/bin").toString());
    m_bin->setPlaceholderText(cfg::binDir());
    fp->addRow("Game executables:", withBrowse(m_bin, true, this));
    m_nvram = new QLineEdit(s.value("paths/nvram").toString());
    m_nvram->setPlaceholderText(cfg::portable() ? "default: user/data in the HIRAKU folder" : "default: ~/.local/share/hiraku");
    fp->addRow("Saves (NVRAM):", withBrowse(m_nvram, true, this));
    m_place = new QComboBox;   // tools/ingest.py --place
    m_place->addItem("Ask each time", "ask");
    m_place->addItem("Move them into the HIRAKU folder", "move");
    m_place->addItem("Leave them where they are", "stay");
    m_place->setCurrentIndex(std::max(0, m_place->findData(s.value("ingest/place", "ask").toString())));
    m_place->setToolTip("A game's CHDs, disks and ROM set (a PC game's folder): moved into the HIRAKU folder's dumps/ "
                        "(gone from where they were), or read where they are (keep them there). Its ROMs are copied in "
                        "either way.");
    fp->addRow("Games' files when added:", m_place);
    auto *start = new QGroupBox("Starting a game");
    auto *fs = new QFormLayout(start);
    m_fastBoot = new QCheckBox("Fast start-up: through the game's start-up checks at full speed, to its title");
    m_fastBoot->setChecked(s.value("game/fastBoot", true).toBool());
    m_fastBoot->setToolTip("The game still boots as the cabinet does (its checks, its loading), only as fast as this "
                           "PC can run it, without sound, with a box over its picture saying so; from its title or "
                           "attract on, at its own speed. A button of the game (a coin, Test) goes back to its own "
                           "speed at once. The arcade games (not the PC ones); Tab, held, fast-forwards any time.");
    fs->addRow("", m_fastBoot);
    v->addWidget(start);
    auto *adv = new QGroupBox("Launcher");
    auto *fx = new QFormLayout(adv);
    m_hideLauncher = new QCheckBox("Hide the launcher while a game runs");
    m_hideLauncher->setChecked(s.value("ui/hideWhileRunning", false).toBool());
    fx->addRow("", m_hideLauncher);
    m_checkUpdates = new QCheckBox("Look for a new version (asks GitHub): when the launcher starts, and in games started without it");
    m_checkUpdates->setChecked(s.value("updates/check", true).toBool());
    if (!upd::available()) {
        m_checkUpdates->setEnabled(false);
        m_checkUpdates->setToolTip("A release package updates itself; this copy isn't one");
    }
    fx->addRow("", m_checkUpdates);
    m_extra = new QLineEdit(s.value("game/extraArgs").toString());
    m_extra->setPlaceholderText("e.g. --bios-seconds 21.65");
    m_extra->setToolTip("Command-line options for every game (docs/PLAY.md lists them). For one game alone: Game > "
                        "Arguments… (or right-click it).");
    fx->addRow("Arguments (every game):", m_extra);
    v->addWidget(adv);
    v->addWidget(paths);
}

void SettingsDialog::accept() {
    auto &s = cfg::settings();
    const QString pcbid = m_pcbid->text().trimmed(), c0 = m_card[0]->text().trimmed(), c1 = m_card[1]->text().trimmed();
    if ((!pcbid.isEmpty() && pcbid.size() != 20) || (!c0.isEmpty() && c0.size() != 16) || (!c1.isEmpty() && c1.size() != 16)) {
        QMessageBox::warning(this, "e-AMUSEMENT", "A PCBID is 20 hex digits, and a card number 16 (E004...).");
        return;
    }
    s.setValue("video/scale", m_scale->value());
    s.setValue("video/fullscreen", m_fullscreen->isChecked());
    s.setValue("video/splitScreens", m_splitScreens->isChecked());
    s.setValue("video/displaySync", m_displaySync->isChecked());
    s.setValue("video/smoothMotion", m_motion->currentData().toString() == "smooth");
    s.setValue("video/crtBeam", m_motion->currentData().toString() == "beam");
    s.setValue("video/crtBeamGamma", m_beamGamma->value());
    s.setValue("video/crtBeamGain", m_beamGain->value());
    s.setValue("video/picture", m_picture->currentData().toString());
    s.setValue("video/pythonRenderer", m_pyRenderer->currentData().toString());
    for (const cfg::ShaderKey &k : cfg::shaderKeys()) {   // only what differs from the defaults
        if (m_shaderKeys.value(k.action) == k.binding) s.remove(QString("shaderKeys/") + k.action);
        else s.setValue(QString("shaderKeys/") + k.action, m_shaderKeys.value(k.action));
    }
    s.setValue("audio/ms", m_audioMs->value());
    s.setValue("audio/fixed", m_audioFixed->isChecked());
    s.setValue("system/priority", m_priority->isChecked());
    s.setValue("audio/volumeDb", m_volume->value());
    s.setValue("audio/soundEveryMs", m_fineSound->isChecked());
    s.setValue("audio/device", m_audioDevice->currentData().toString());
    if (m_audioExclusive) s.setValue("audio/exclusive", m_audioExclusive->isChecked());
    s.setValue("audio/buffer", m_audioBuffer->currentData().toInt());
    s.setValue("audio/soundfont", m_soundfont->text().trimmed());
    s.setValue("lights/socket", m_socket->isChecked());
    s.setValue("lights/overlay", m_lightsOverlay->isChecked());
    s.setValue("paths/root", m_root->text().trimmed());
    s.setValue("paths/bin", m_bin->text().trimmed());
    s.setValue("paths/nvram", m_nvram->text().trimmed());
    s.setValue("ui/hideWhileRunning", m_hideLauncher->isChecked());
    s.setValue("game/fastBoot", m_fastBoot->isChecked());
    s.setValue("ingest/place", m_place->currentData().toString());
    s.setValue("updates/check", m_checkUpdates->isChecked());
    s.setValue("game/extraArgs", m_extra->text().trimmed());
    s.remove("eamuse/server");   // now eamuse/url
    s.setValue("eamuse/url", m_eaUrl->text().trimmed());
    s.setValue("eamuse/smartea", m_smartea->isChecked());
    s.setValue("eamuse/pcbid", m_pcbid->text().trimmed().toUpper());
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
