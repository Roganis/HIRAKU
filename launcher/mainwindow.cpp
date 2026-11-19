// SPDX-License-Identifier: GPL-3.0-or-later
#include "mainwindow.h"
#include "theme.h"
#include "runlogs.h"
#include "gamemenu.h"
#include "powerplan.h"
#include "menufont.h"
#include "recording.h"
#include "../third_party/mdi/icons.h"
#include "nvidia.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QCollator>
#include <algorithm>
#include <csignal>
#include <cstdio>
#include <functional>

#include <QCloseEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QKeyEvent>
#include <QFileDialog>
#include <QMimeData>
#include <QRegularExpression>
#include <QPainter>
#include <QSaveFile>
#include <QScreen>
#include <QDesktopServices>
#include <QDateTime>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QStandardPaths>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QProgressDialog>
#include <QInputDialog>
#include <QCheckBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QScrollArea>
#include <QSet>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <utility>

#include "aboutdialog.h"
#include "config.h"
#include "controlsdialog.h"
#include "dipsdialog.h"
#include "gamecards.h"
#include "lightsdialog.h"
#include "modsdialog.h"
#include "music.h"
#include "picturedialog.h"
#include "unlocksdialog.h"
#include "settingsdialog.h"
#include "report.h"
#include "shell/sys.h"
#include "updater.h"

namespace {

enum Col { Title, Year, Status, NCols };
// a series' or a system's row: its key in ui/collapsed ("pop'n music", "pop'n music/Firebeat")
constexpr int GroupRole = Qt::UserRole + 1;

// why a game stopped, in words: a Windows exception or a Unix signal, else its exit code
QString exitReason(int code, QProcess::ExitStatus st) {
#ifdef Q_OS_WIN
    switch ((quint32)code) {
    case 0xC0000005: return "an access violation (0xC0000005): it used memory it doesn't have";
    case 0xC000001D: return "an illegal instruction (0xC000001D): it uses an instruction this CPU doesn't have";
    case 0xC0000135: return "a DLL it needs is missing (0xC0000135)";
    case 0xC0000139: return "a DLL it loaded is older than it needs (0xC0000139: a function is missing)";
    case 0xC0000142: return "a DLL it loaded failed to start (0xC0000142)";
    case 0xC0000409: return "it stopped itself on a fatal error (0xC0000409)";
    case 0xC00000FD: return "a stack overflow (0xC00000FD)";
    case 0xC0000374: return "a corrupted heap (0xC0000374)";
    case 0xC0000094: return "a division by zero (0xC0000094)";
    case 0x80000003: return "a failed assertion or a breakpoint (0x80000003)";
    case 3: return "it aborted (exit code 3: a failed assertion or a fatal error)";
    }
    if (st == QProcess::CrashExit) return QString("it crashed (exception 0x%1)").arg((quint32)code, 8, 16, QChar('0')).toUpper();
#else
    if (st == QProcess::CrashExit) switch (code) {
        case SIGSEGV: return "a segmentation fault (SIGSEGV): it used memory it doesn't have";
        case SIGILL: return "an illegal instruction (SIGILL): it uses an instruction this CPU doesn't have";
        case SIGABRT: return "it aborted (SIGABRT: a failed assertion or a fatal error)";
        case SIGBUS: return "a bus error (SIGBUS): a file it had mapped went away, or a bad memory access";
        case SIGFPE: return "an arithmetic error (SIGFPE)";
        case SIGKILL: return "it was killed (SIGKILL): by the system when memory runs out, or by another program";
        default: return QString("it crashed (signal %1)").arg(code);
        }
#endif
    return QString("it exited with code %1").arg(code);
}

// --card-media (runtime/shell/cardmedia.h), in the game's Floppy disk / Memory cards menu: each
// e-AMUSEMENT card its own disk or memory card; and a card's image put to a card (a player's card
// of the home version: a blank one is no use to DDR). A card's folder is made when it is first touched.
void addCardMedia(QWidget *parent, QMenu *menu, const QString &id, const QString &what, const QString &file, qint64 bytes,
                  const QString &filter, bool running, const std::function<void()> &changed) {
    menu->addSection("e-AMUSEMENT cards");
    auto *on = menu->addAction("Each e-AMUSEMENT card its own " + what);
    on->setCheckable(true);
    on->setChecked(cfg::settings().value("cardMedia/" + id).toBool());
    on->setToolTip("A card touched to the card reader puts that card's own " + what + " in (touched again: takes it out; "
                   "another card swaps it), made blank the first time. Without a reader, the keys present Settings' "
                   "e-AMUSEMENT card numbers");
    QObject::connect(on, &QAction::toggled, parent, [id, changed](bool v) {
        if (v) cfg::settings().setValue("cardMedia/" + id, true);
        else cfg::settings().remove("cardMedia/" + id);
        changed();
    });
    const QString dir = cfg::cardMediaDir(id);
    auto *cards = menu->addMenu("A card's " + what);
    cards->setToolTipsVisible(true);
    static const QRegularExpression number("^[0-9A-F]{16}$");
    int listed = 0;
    for (const QFileInfo &f : QDir(dir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time)) {
        const QString n = f.fileName();
        if (!number.match(n).hasMatch()) continue;
        const QString card = QString::fromUtf8("card \u2026") + n.right(4);   // (its last four digits only)
        const QString target = f.filePath() + "/" + file;
        auto *a = cards->addAction("Put an image to " + card + "…", parent, [=] {
            if (running) {
                QMessageBox::information(parent, "HIRAKU", "Close the game first: a card's " + what + " is the game's while it runs.");
                return;
            }
            const QString from = QFileDialog::getOpenFileName(parent, "An image for " + card + "'s " + what, QDir::homePath(), filter);
            if (from.isEmpty()) return;
            if (QFileInfo(from).size() != bytes) {
                QMessageBox::warning(parent, "HIRAKU", QFileInfo(from).fileName() + " is not a " + what + "'s image (" +
                                                           QLocale().toString(bytes) + " bytes).");
                return;
            }
            // (the card's one before it kept beside, by the time it was put aside)
            if (QFileInfo::exists(target))
                QFile::rename(target, target + ".before-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss"));
            if (!QFile::copy(from, target)) {
                QMessageBox::warning(parent, "HIRAKU", "Couldn't write " + target + ".");
                return;
            }
            QFile(target).setPermissions(QFile(target).permissions() | QFileDevice::WriteOwner);
            QMessageBox::information(parent, "HIRAKU", QFileInfo(from).fileName() + " is " + card + "'s " + what +
                                                           " now: the next touch of that card puts it in.");
        });
        a->setToolTip(QString("Last used %1").arg(QLocale().toString(f.lastModified(), QLocale::ShortFormat)));
        listed++;
    }
    if (!listed) cards->addAction("No card has been touched yet: touch yours in the game once")->setEnabled(false);
    cards->addSeparator();
    cards->addAction("Open the cards' folder", parent, [dir] {
        QDir().mkpath(dir);
        QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    });
}

bool lightsFileUsed(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    for (const QByteArray &line : f.readAll().split('\n'))
        if (!line.trimmed().startsWith('#') && line.contains("hid:")) return true;
    return false;
}

}  // namespace

static bool installCassette(const Game &g);
static bool installCd(const Game &g);
static bool hasNetworkUnit(const Game &g);
static bool unitInstalled(const Game &g);
static QString unitState(const Game &g);
static QString nvramSeed(const Game &g);
static bool firstBoot(const Game &g);
static QString menuPicture(const QString &id);
static QString statusOf(const Game &g, bool *notable);

// The window's name: "HIRAKU ~completeMIX~" while the newest games are listed (secretKeys). Qt adds
// " — " and the application's display name to a title that doesn't end with it: that changes too
static QString mainTitle() {
    const bool mix = cfg::settings().value("ui/pc64").toBool();
    QGuiApplication::setApplicationDisplayName(mix ? "HIRAKU ~completeMIX~" : "HIRAKU");
    if (mix) return "HIRAKU ~completeMIX~";
    return upd::currentVersion().isEmpty() ? QString("HIRAKU") : "HIRAKU " + upd::currentVersion();
}

MainWindow::MainWindow() {
    setWindowTitle(mainTitle());
    resize(900, 600);

    // ---- actions ----
    // (their icons: Material Design Icons in the look, theme::mark; made again when it changes)
    m_play = new QAction("Play", this);
    m_play->setShortcut(QKeySequence(Qt::Key_Return));
    m_stop = new QAction("Stop", this);
    m_controls = new QAction("Controls", this);
    theme::mark(m_controls, mdi::gamepad_variant, 0);
    m_lights = new QAction("Lights", this);
    theme::mark(m_lights, mdi::lightbulb_on, 2);
    auto *settings = new QAction("Settings", this);
    theme::mark(settings, mdi::cog, 5);
    auto *pictureAct = new QAction("Picture", this);
    theme::mark(pictureAct, mdi::television_classic, 3);
    pictureAct->setToolTip("Sharp pixels or a CRT (light, full, analog), and the CRT's settings: for all games, or the selected one");
    connect(pictureAct, &QAction::triggered, this, [this] {
        auto *g = selected();
        PictureDialog d(g ? g->id : QString(), g ? g->title : QString(), false, this);
        if (d.exec() == QDialog::Accepted) showDetails();
    });
    auto *addFiles = new QAction("Add game files…", this);
    theme::mark(addFiles, mdi::file_plus);
    addFiles->setToolTip("Add a game from its files: the MAME ROM set (.zip/.7z), CHDs or disc images,\n"
                         "or a Konami Python game's .py1 (PCSX2 Reliquary's descriptor).\n"
                         "You can also drop them on this window.");
    connect(addFiles, &QAction::triggered, this, [this] {
        QStringList f = QFileDialog::getOpenFileNames(this, "Add game files", QString(),
            "Game files (*.zip *.7z *.chd *.cue *.iso *.img *.bin *.py1);;All files (*)");
        if (!f.isEmpty()) addGameFiles(f);
    });
    auto *addFolder = new QAction("Add a game folder…", this);
    theme::mark(addFolder, mdi::folder_plus);
    connect(addFolder, &QAction::triggered, this, [this] {
        QString d = QFileDialog::getExistingDirectory(this, "Add a game folder (its ROM set, CHDs, images)");
        if (!d.isEmpty()) addGameFiles({d});
    });
    setAcceptDrops(true);
    auto *refreshAct = new QAction("Refresh", this);
    theme::mark(refreshAct, mdi::refresh);
    refreshAct->setShortcut(QKeySequence::Refresh);
    m_showAll = new QAction("Show games not added yet", this);
    m_showAll->setToolTip("Every game HIRAKU runs, with what each needs to be added");
    m_showAll->setCheckable(true);
    m_showAll->setChecked(cfg::settings().value("ui/showAll", false).toBool());

    connect(m_play, &QAction::triggered, this, [this] {
        if (auto *g = selected()) play(g->id);
    });
    connect(m_stop, &QAction::triggered, this, &MainWindow::stop);
    connect(m_controls, &QAction::triggered, this, [this] {
        auto *g = selected();
        controls(g ? g->family : "bm3");
    });
    connect(m_lights, &QAction::triggered, this, [this] {
        auto *g = selected();
        LightsDialog d(g ? g->family : "popn", this);
        d.exec();
    });
    connect(settings, &QAction::triggered, this, [this] {
        SettingsDialog d(this);
        if (d.exec() == QDialog::Accepted) {
            refresh();
            m_music->settingsChanged();
            if (d.look() != theme::current() && d.look() < m_looks.size()) m_looks[d.look()]->trigger();   // (its Look page)
        }
    });
    connect(refreshAct, &QAction::triggered, this, &MainWindow::refresh);
    connect(m_showAll, &QAction::toggled, this, [this](bool on) {
        cfg::settings().setValue("ui/showAll", on);
        applyFilter();
        showDetails();
    });

    // ---- toolbar ----
    auto *tb = addToolBar("Main");
    tb->setObjectName("main toolbar");
    tb->setMovable(false);
    tb->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    tb->addAction(m_play);
    tb->addAction(m_stop);
    tb->addSeparator();
    tb->addAction(m_controls);
    tb->addAction(m_lights);
    tb->addAction(pictureAct);
    tb->addAction(settings);
    tb->addSeparator();
    tb->addAction(refreshAct);
    tb->addAction(addFiles);
    // the HIRAKU logo, as tall as the buttons beside it; a click shows About HIRAKU
    tb->addSeparator();
    auto *logo = m_logo = new QToolButton;
    logo->setObjectName("hkLogo");
    m_logoH = tb->iconSize().height() + tb->fontMetrics().height();
    logo->setAutoRaise(true);
    logo->setCursor(Qt::PointingHandCursor);
    logo->setToolTip("About HIRAKU");
    connect(logo, &QToolButton::clicked, this, [this] { AboutDialog(this).exec(); });
    // in a plain widget: a tool button added straight to the toolbar gets the toolbar's icon size
    auto *logoBox = new QWidget;
    auto *logoLay = new QHBoxLayout(logoBox);
    logoLay->setContentsMargins(0, 0, 0, 0);
    logoLay->addWidget(logo);
    tb->addWidget(logoBox);
    auto *spacer = new QWidget;
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    tb->addWidget(spacer);
    // the games as cards or as the list (View > Cards, List)
    m_asCards = new QAction("Cards", this);
    theme::mark(m_asCards, mdi::view_grid);
    m_asList = new QAction("List", this);
    theme::mark(m_asList, mdi::view_list);
    m_asCards->setToolTip("The games as cards, with pictures of their title screens, a shelf for each series");
    m_asList->setToolTip("The games as a list, by series and system");
    auto *asGroup = new QActionGroup(this);
    for (QAction *a : {m_asCards, m_asList}) {
        a->setCheckable(true);
        asGroup->addAction(a);
    }
    const bool cardsOn = cfg::settings().value("ui/view").toString() == "cards";
    (cardsOn ? m_asCards : m_asList)->setChecked(true);
    connect(m_asCards, &QAction::triggered, this, [this] { showAsCards(true); });
    connect(m_asList, &QAction::triggered, this, [this] { showAsCards(false); });
    auto *asBox = new QWidget;
    auto *asLay = new QHBoxLayout(asBox);
    asLay->setContentsMargins(0, 0, 8, 0);
    asLay->setSpacing(2);
    for (QAction *a : {m_asCards, m_asList}) {
        auto *b = new QToolButton;
        b->setDefaultAction(a);
        b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        b->setIconSize(QSize(16, 16));
        asLay->addWidget(b);
    }
    tb->addWidget(asBox);
    m_filter = new QLineEdit;
    m_filter->setPlaceholderText("Search");
    m_filter->setClearButtonEnabled(true);
    m_filter->setMaximumWidth(240);
    tb->addWidget(m_filter);
    tb->widgetForAction(m_play)->setObjectName("hkPlay");   // (Pop's green and red buttons: theme.cpp)
    tb->widgetForAction(m_stop)->setObjectName("hkStop");
    connect(m_filter, &QLineEdit::textChanged, this, [this] {
        applyFilter();
        updateActions();
        showDetails();
    });

    // ---- the list (series, the systems they ran on, their games) and the details under it ----
    m_list = new QTreeWidget;
    m_list->setColumnCount(NCols);
    m_list->setHeaderLabels({"Title", "Year", "Status"});
    m_list->setUniformRowHeights(true);
    m_list->setIconSize(QSize(32, 32));
    m_list->setAlternatingRowColors(true);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    m_list->header()->setStretchLastSection(false);
    m_list->header()->setSectionResizeMode(Title, QHeaderView::Stretch);
    m_list->header()->setSectionResizeMode(Year, QHeaderView::ResizeToContents);
    m_list->header()->setSectionResizeMode(Status, QHeaderView::ResizeToContents);
    connect(m_list, &QTreeWidget::itemDoubleClicked, this, [this] {
        if (auto *g = selected()) play(g->id);
    });
    connect(m_list, &QTreeWidget::itemSelectionChanged, this, [this] {
        updateActions();
        showDetails();
        m_cards->select(selected() ? selected()->id : QString());
    });
    connect(m_list, &QTreeWidget::customContextMenuRequested, this, &MainWindow::contextMenu);
    // a series or system the player closes stays closed (not one closed or opened by a search)
    m_collapsed = cfg::settings().value("ui/collapsed").toStringList();
    auto remember = [this](QTreeWidgetItem *it, bool open) {
        const QString key = it->data(Title, GroupRole).toString();
        if (m_arranging || key.isEmpty() || !m_filter->text().trimmed().isEmpty()) return;
        m_collapsed.removeAll(key);
        if (!open) m_collapsed << key;
        cfg::settings().setValue("ui/collapsed", m_collapsed);
    };
    connect(m_list, &QTreeWidget::itemExpanded, this, [remember](QTreeWidgetItem *it) { remember(it, true); });
    connect(m_list, &QTreeWidget::itemCollapsed, this, [remember](QTreeWidgetItem *it) { remember(it, false); });

    m_details = new QLabel;
    m_details->setWordWrap(true);
    m_details->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
    m_details->setOpenExternalLinks(false);
    connect(m_details, &QLabel::linkActivated, this, [this](const QString &link) {
        if (const Game *g = selected(); g && link == "hash-big") checkFiles(g->id, true);   // filesHtml's link
    });
    m_details->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_details->setMargin(8);
    // in the cards view the chosen game large over them (gamecards.h)
    m_head = new GameHead;
    m_head->hide();
    connect(m_head, &GameHead::play, this, [this] {
        if (const Game *g = selected()) play(g->id);
    });
    connect(m_head, &GameHead::controls, this, [this] {
        if (const Game *g = selected()) controls(g->family);
    });
    connect(m_head, &GameHead::unlocks, this, [this] {
        if (const Game *g = selected()) {
            UnlocksDialog d(g->id, g->title, m_running == g->id, this);
            d.exec();
        }
    });
    connect(m_head, &GameHead::mods, this, [this] {
        if (const Game *g = selected()) {
            ModsDialog d(g->id, g->title, this);
            if (d.exec() == QDialog::Accepted) gameChanged();
        }
    });
    auto *detailsBox = new QWidget;
    auto *dv = new QVBoxLayout(detailsBox);
    dv->setContentsMargins(0, 0, 0, 0);
    dv->setSpacing(0);
    dv->addWidget(m_head);
    dv->addWidget(m_details, 1);
    auto *details = new QScrollArea;
    details->setWidgetResizable(true);
    details->setFrameShape(QFrame::NoFrame);
    details->setWidget(detailsBox);
    m_detailsPane = details;

    // with no game added, the list's place says how to add one
    auto *empty = new QWidget;
    {
        auto *ev = new QVBoxLayout(empty);
        ev->addStretch(1);
        auto *head = new QLabel("<b>No games added yet</b>");
        head->setAlignment(Qt::AlignCenter);
        QFont big = head->font();
        big.setPointSizeF(big.pointSizeF() * 1.4);
        head->setFont(big);
        ev->addWidget(head);
        auto *how = new QLabel("Add a game from your own dumps: its MAME ROM set (.zip, .7z), CHDs or disc images, or a "
                               "Konami Python game's folder. HIRAKU recognises the game by its contents. You can also "
                               "drop them on this window.");
        how->setWordWrap(true);
        how->setAlignment(Qt::AlignCenter);
        how->setMaximumWidth(520);
        auto *howRow = new QHBoxLayout;
        howRow->addStretch(1);
        howRow->addWidget(how);
        howRow->addStretch(1);
        ev->addLayout(howRow);
        // (two side by side and one under them: the page fits a 640x480 screen's window)
        auto *buttons = new QGridLayout;
        buttons->setColumnStretch(0, 1);
        buttons->setColumnStretch(3, 1);
        auto *bFiles = new QPushButton("Add game files…");
        theme::mark(bFiles, mdi::file_plus);
        connect(bFiles, &QPushButton::clicked, addFiles, &QAction::trigger);
        auto *bFolder = new QPushButton("Add a game folder…");
        theme::mark(bFolder, mdi::folder_plus);
        connect(bFolder, &QPushButton::clicked, addFolder, &QAction::trigger);
        auto *bAll = new QPushButton("Show every game HIRAKU runs");
        bAll->setToolTip("With what each one needs (View > Show games not added yet)");
        connect(bAll, &QPushButton::clicked, m_showAll, [this] { m_showAll->setChecked(true); });
        buttons->addWidget(bFiles, 0, 1);
        buttons->addWidget(bFolder, 0, 2);
        buttons->addWidget(bAll, 1, 1, 1, 2);
        ev->addLayout(buttons);
        ev->addStretch(2);
    }
    m_listStack = new QStackedWidget;
    m_listStack->addWidget(m_list);
    m_listStack->addWidget(empty);
    // the cards: a card picked picks its game in the list, which the rest goes by
    m_cards = new GameCards;
    m_listStack->addWidget(m_cards);
    // only the page shown counts for the window's width (on a 640x480 screen a page out of sight
    // would take the Details panel's room)
    const auto onlyShown = [this](int shown) {
        for (int k = 0; k < m_listStack->count(); k++)
            m_listStack->widget(k)->setSizePolicy(k == shown ? QSizePolicy::Preferred : QSizePolicy::Ignored,
                                                  k == shown ? QSizePolicy::Preferred : QSizePolicy::Ignored);
        m_listStack->updateGeometry();
    };
    connect(m_listStack, &QStackedWidget::currentChanged, this, onlyShown);
    onlyShown(m_listStack->currentIndex());
    connect(m_cards, &GameCards::picked, this, [this](const QString &id) {
        if (QTreeWidgetItem *it = itemFor(id)) m_list->setCurrentItem(it);
    });
    connect(m_cards, &GameCards::activated, this, [this](const QString &id) {
        if (const Game *g = find(id); g && g->status == Game::Ready) play(id);
    });
    connect(m_cards, &GameCards::menuAsked, this, [this](const QString &id, const QPoint &at) {
        const Game *g = find(id);
        if (!g) return;
        QMenu menu(this);
        fillGameMenu(&menu, *g);
        menu.exec(at);
    });
    // a new version is out (updater.h): a bar above the list, until it's taken or closed
    m_updateBar = new QFrame;
    static_cast<QFrame *>(m_updateBar)->setFrameShape(QFrame::StyledPanel);
    auto *ub = new QHBoxLayout(m_updateBar);
    ub->setContentsMargins(8, 4, 4, 4);
    m_updateText = new QLabel;
    ub->addWidget(m_updateText, 1);
    auto *ubGo = new QPushButton("What's new…");
    connect(ubGo, &QPushButton::clicked, this, &MainWindow::offerUpdate);
    ub->addWidget(ubGo);
    auto *ubClose = new QToolButton;
    ubClose->setText("✕");
    ubClose->setAutoRaise(true);
    ubClose->setToolTip("Not now (Help > Check for updates finds it again)");
    connect(ubClose, &QToolButton::clicked, m_updateBar, &QWidget::hide);
    ub->addWidget(ubClose);
    m_updateBar->hide();
    auto *central = new QWidget;
    auto *cv = new QVBoxLayout(central);
    cv->setContentsMargins(0, 0, 0, 0);
    cv->setSpacing(0);
    cv->addWidget(m_updateBar);
    cv->addWidget(m_listStack, 1);
    setCentralWidget(central);

    // the details and the log beside the list, one above the other (a wide screen's width), as
    // docks: either can be dragged under the list instead, or closed (View)
    m_detailsDock = new QDockWidget("Details", this);
    m_detailsDock->setObjectName("details");
    m_detailsDock->setWidget(details);
    addDockWidget(Qt::RightDockWidgetArea, m_detailsDock);

    // ---- log ----
    m_log = new QPlainTextEdit;
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(5000);
    m_log->setFont(QFont("monospace"));
    m_logDock = new QDockWidget("Log", this);
    m_logDock->setObjectName("log");
    m_logDock->setWidget(m_log);
    addDockWidget(Qt::RightDockWidgetArea, m_logDock);
    splitDockWidget(m_detailsDock, m_logDock, Qt::Vertical);
    m_logDock->hide();

    // ---- music ----
    // under the two, its own height (music.h); it waits while a game or the game menu runs, and a
    // closed panel leaves no music playing
    m_music = new MusicPlayer;
    m_music->watch(&m_proc);
    m_music->watch(&m_menu);
    m_musicDock = new QDockWidget("Music", this);
    m_musicDock->setObjectName("music");
    m_musicDock->setWidget(m_music);
    addDockWidget(Qt::RightDockWidgetArea, m_musicDock);
    splitDockWidget(m_logDock, m_musicDock, Qt::Vertical);
    connect(m_musicDock->toggleViewAction(), &QAction::toggled, this, [this](bool on) {
        if (!on) m_music->pause();
    });

    // ---- menus ----
    auto *file = menuBar()->addMenu("&File");
    file->addAction(m_play);
    file->addAction(m_stop);
    file->addSeparator();
    file->addAction(addFiles);
    file->addAction(addFolder);
    file->addSeparator();
    file->addAction("Convert recordings for sharing…", this, &MainWindow::convertRecordings);
    file->addAction("Open the recordings folder", this, [] {
        QDir().mkpath(cfg::recordingsDir());
        QDesktopServices::openUrl(QUrl::fromLocalFile(cfg::recordingsDir()));
    });
    file->addSeparator();
    file->addAction("Open the HIRAKU folder", this, [] { QDesktopServices::openUrl(QUrl::fromLocalFile(cfg::rootDir())); });
    file->addAction("Open the settings folder", this, [] {
        QDir().mkpath(cfg::configDir());
        QDesktopServices::openUrl(QUrl::fromLocalFile(cfg::configDir()));
    });
    file->addSeparator();
    file->addAction("Quit", QKeySequence::Quit, this, &QWidget::close);
    // the picked game's own menu (what right-clicking it shows)
    auto *gameMenu = menuBar()->addMenu("&Game");
    connect(gameMenu, &QMenu::aboutToShow, this, [this, gameMenu] {
        gameMenu->clear();
        if (const Game *g = selected()) fillGameMenu(gameMenu, *g);
        else gameMenu->addAction("Pick a game in the list first")->setEnabled(false);
    });
    auto *opts = menuBar()->addMenu("&Options");
    opts->addAction(m_controls);
    opts->addAction(m_lights);
    opts->addAction(pictureAct);
    opts->addAction(settings);
    auto *view = menuBar()->addMenu("&View");
    view->addAction(m_asCards);
    view->addAction(m_asList);
    view->addSeparator();
    view->addAction(m_showAll);
    view->addAction(m_detailsDock->toggleViewAction());
    view->addAction(m_logDock->toggleViewAction());
    view->addAction(m_musicDock->toggleViewAction());
    view->addAction(refreshAct);
    view->addSeparator();
    auto *looks = view->addMenu("Look");
    auto *lookGroup = new QActionGroup(this);
    for (auto [look, name] : {std::pair{theme::System, "The system's"}, {theme::Pop, "Pop"}, {theme::Night, "Pop night"}}) {
        auto *a = looks->addAction(name);
        a->setCheckable(true);
        a->setChecked(theme::current() == look);
        lookGroup->addAction(a);
        m_looks << a;
        connect(a, &QAction::triggered, this, [this, look] {
            cfg::settings().setValue("ui/theme", theme::key(look));
            theme::apply(look);
            themeChanged();
        });
    }
    auto *help = menuBar()->addMenu("&Help");
    help->addAction("How to play (docs/PLAY.md)", this, [] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(cfg::rootDir() + "/docs/PLAY.md"));
    });
    help->addAction("Lights (docs/LIGHTS.md)", this, [] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(cfg::rootDir() + "/docs/LIGHTS.md"));
    });
    help->addAction("Open the last runs' logs", this, [] {
        QDir().mkpath(cfg::logsDir());
        QDesktopServices::openUrl(QUrl::fromLocalFile(cfg::logsDir()));
    })->setToolTip(QString("A file for each of the last %1 games run: what the log pane showed").arg(cfg::kRunLogs));
    help->addAction("Report a problem…", this, [this] { ReportDialog(this).exec(); })
        ->setToolTip("A run's logs and what system this is, cleaned of your folders, names and card numbers, "
                     "to attach to an issue on GitHub or to a mail");
    help->addSeparator();
    auto *updAct = help->addAction("Check for updates…", this, [this] { checkForUpdate(true); });
    if (!upd::available()) {
        updAct->setEnabled(false);
        updAct->setToolTip("A release package updates itself; this copy isn't one");
    }
    if (upd::available()) {
        // the last update's download, once its script (which starts this launcher) is done; not
        // once an update of this session has begun: update/ is then the new download (nor while
        // one is going on or was cut short: cleanUp itself looks)
        QTimer::singleShot(5000, this, [this] { if (!m_updateBegun) upd::cleanUp(); });
        if (cfg::settings().value("updates/check", true).toBool())
            QTimer::singleShot(2000, this, [this] { checkForUpdate(false); });
    }

    // ---- the details' list of a game's files (tools/checklist.py) ----
    connect(&m_files, &QProcess::finished, this, [this](int code, QProcess::ExitStatus st) {
        if (m_filesGame.isEmpty()) return;   // stopped for another game's (checkFiles)
        const QJsonObject o = QJsonDocument::fromJson(m_files.readAllStandardOutput()).object();
        if (st == QProcess::NormalExit && code == 0 && !o.isEmpty()) m_fileLists[m_filesGame] = o;
        else m_fileLists[m_filesGame] = QJsonObject{{"error", QString::fromUtf8(m_files.readAllStandardError()).trimmed()}};
        const QString id = m_filesGame;
        m_filesGame.clear();
        if (const Game *g = selected(); g && g->id == id) showDetails();
    });

    // ---- the game process ----
    m_ingest.setProcessChannelMode(QProcess::MergedChannels);
    connect(&m_ingest, &QProcess::readyRead, this, [this] {
        QString t = QString::fromUtf8(m_ingest.readAll());
        m_ingestOutput += t;
        log(t);
    });
    connect(&m_ingest, &QProcess::finished, this, [this](int code, QProcess::ExitStatus st) {
        m_fileLists.clear();   // games' files added or moved: their lists again
        refresh();
        QStringList added, errors, kept;
        for (const QString &l : m_ingestOutput.split('\n')) {
            // dumps/<id>/machine.txt (Windows: dumps\<id>\machine.txt)
            if ((l.startsWith("wrote") || l.startsWith("OK ")) && l.contains("machine.txt")) added << QString(l).replace('\\', '/').section('/', -2, -2);
            if (l.startsWith("ERROR") || l.startsWith("NEXT")) errors << l.mid(9).trimmed();
            if (l.startsWith("KEPT")) kept << l.mid(9).trimmed();   // a Python BIOS or keys, kept for the games
        }
        // several games at once: those added are, even when another failed (its error below)
        bool ok = st == QProcess::NormalExit && (code == 0 || !added.isEmpty());
        QStringList titles;   // the games by name, and the first one added selected
        for (const QString &id : added) titles << (find(id) ? find(id)->title : id);
        if (ok && !added.isEmpty())
            if (auto *it = itemFor(added.first()); it && !it->isHidden()) selectGame(it);
        QString msg = ok ? (titles.isEmpty() ? "Kept: " + kept.join("\n")
                                             : "Added: " + titles.join(", ") + (titles.size() == 1 ? ". It is ready to play." : "."))
                         : "The files could not be added.";
        if (!ok && !kept.isEmpty()) msg = "Kept: " + kept.join("\n") + "\n\n" + msg;
        if (!errors.isEmpty()) msg += "\n\n" + errors.join("\n");
        log("\n--- " + (ok ? QString("added ") + added.join(", ") : QString("ingest failed")) + " ---\n");
        if (ok) QMessageBox::information(this, "HIRAKU", msg);
        else QMessageBox::warning(this, "HIRAKU", msg);
        startIngest();   // the next batch, if any
    });
    connect(&m_ingest, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) return;
        QMessageBox::warning(this, "HIRAKU", "Could not run tools/ingest.py (python3 is needed):\n" + m_ingest.errorString());
    });
    m_proc.setProcessChannelMode(QProcess::MergedChannels);
    connect(&m_proc, &QProcess::readyRead, this, [this] { log(QString::fromLocal8Bit(m_proc.readAll())); });
    connect(&m_proc, &QProcess::finished, this, [this](int code, QProcess::ExitStatus st) {
        QString title = find(m_running) ? find(m_running)->title : m_running;
        // left from the cabinet's controls for the game menu (runtime/host/menukey.h); and a game
        // chosen in the menu goes back to it however it ends (its own way out, Escape, an error):
        // a cabinet has no mouse for the launcher's window
        const bool menuOn = cfg::settings().value("menu/enabled", true).toBool();
        const bool asked = st == QProcess::NormalExit && code == gamemenu::kExitCode && menuOn;
        bool ok = m_stopping || (st == QProcess::NormalExit && (code == 0 || code == 130)) || asked;
        const bool toMenu = !m_stopping && menuOn && (asked || m_fromMenu);
        m_fromMenu = false;
        QString msg = st == QProcess::CrashExit ? title + " crashed"
                      : asked                   ? title + " left for the game menu"
                                                : QString("%1 exited (code %2)").arg(title).arg(code);
        // the run log says why (a crash's exception, or the launcher's own ending of it)
        const QString why = m_killed ? title + " ended by the launcher: it hadn't stopped 5 s after being asked"
                            : st == QProcess::CrashExit ? title + " crashed: " + exitReason(code, st)
                                                        : msg;
        log("\n--- " + why + " ---\n");
        closeRunLog();
        statusBar()->showMessage(ok ? title + " stopped" : msg);
        if (!ok) m_logDock->show();
        const QString left = std::exchange(m_running, QString());
        m_stopping = false;
        m_killed = false;
        if (toMenu) cover(true);   // (before anything else is shown: the desktop stays out of sight)
        else {
            cover(false);
            if (isHidden()) show();
            planBack();
        }
        updateActions();
        // a network unit's install: done (INSTALLATION COMPLETED on the screen), the unit is fitted from
        // now on; else the next start installs again
        if (const QString id = std::exchange(m_unitInstall, QString()); !id.isEmpty() && find(id)) {
            if (unitInstalled(*find(id)) &&
                QMessageBox::question(this, "HIRAKU", "Did " + title + " show INSTALLATION COMPLETED?\n\nYes: the e-AMUSEMENT "
                                      "network unit is fitted from the next start (turn e-AMUSEMENT on in the game's test mode). "
                                      "No: the next start installs its data again.") == QMessageBox::Yes)
                cfg::settings().setValue("npu/" + id, "on");
        }
        gameChanged();   // its saves now exist: no longer a first boot
        // (an error's report is a dialog: not over a cabinet's menu, which says so itself; the log
        // pane is open in the launcher's window when it is next seen)
        if (!ok && !toMenu) exitReport(title, code, st);
        if (toMenu) {
            openMenu(left, ok ? QString() : title + "|ended with an error: HIRAKU's window has its log");
        }
    });
    m_menu.setProcessChannelMode(QProcess::MergedChannels);
    connect(&m_menu, &QProcess::finished, this, &MainWindow::menuClosed);
    connect(&m_menu, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) return;
        cover(false);
        if (isHidden()) show();
        planBack();
        statusBar()->showMessage("The game menu couldn't be started: " + m_menu.errorString());
    });
    connect(&m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) return;
        log("\n--- could not start " + m_proc.program() + ": " + m_proc.errorString() + " ---\n");
        closeRunLog();
        QMessageBox::warning(this, "HIRAKU", "Could not start " + m_proc.program() + ":\n" + m_proc.errorString());
        m_running.clear();
        cover(false);
        if (isHidden()) show();
        planBack();
        updateActions();
    });

    restoreGeometry(cfg::settings().value("ui/geometry").toByteArray());
    // no layout of this version saved: the panel beside the list starts at about a third of the window
    if (!restoreState(cfg::settings().value("ui/state").toByteArray(), kLayout))
        QTimer::singleShot(0, this, [this] { resizeDocks({m_detailsDock}, {std::max(300, width() / 3)}, Qt::Horizontal); });
    placeMusic(m_asCards->isChecked());   // (where the view puts it, whatever the saved layout had)
    if (!m_musicDock->isHidden()) m_music->autoplay();   // (when its panel is there)
    m_music->firstSongs();
    qApp->installEventFilter(this);
    themeChanged();
    refresh();
    planBack();   // (a launcher that ended before it could: the plan it had changed)
    // a cabinet that starts in the game menu (Settings > General)
    if (cfg::settings().value("menu/atStart", false).toBool() && cfg::settings().value("menu/enabled", true).toBool())
        QTimer::singleShot(0, this, [this] { openMenu(lastGame()); });
}

// The programs' handlers are let go first: a program still running (the details' file check, a
// moment after a game was picked) is ended by its QProcess's own end, which says "finished" to a
// handler whose window's members are already gone (a crash on quitting)
MainWindow::~MainWindow() {
    for (QProcess *p : {&m_files, &m_ingest, &m_proc, &m_menu}) p->disconnect(this);
    if (!m_boxFont.isEmpty()) QFile::remove(m_boxFont), QFile::remove(m_boxLogo);
}

void MainWindow::boxLook(QProcessEnvironment &env) {
    if (!m_boxScale) {
        // as large as the screen this window is on, in 360ths of its height (a game's boxes are):
        // 1.333 for a 480-line one (a CRT's), 3 for 1080
        const QScreen *scr = screen();
        m_boxScale = std::clamp(scr ? std::round(scr->size().height() * scr->devicePixelRatio() / 360.0 * 1000) / 1000 : 2.0, 1.0, 8.0);
        const QString base = QDir::tempPath() + QString("/hiraku-boxes-%1").arg(QCoreApplication::applicationPid());
        // the icons, in runtime/host/uibox.h's order from U+F0000
        static const char *const icons[] = {mdi::information, mdi::alert_circle, mdi::download,  mdi::camera,       mdi::volume_off,
                                            mdi::card_account_details, mdi::floppy, mdi::dialpad, mdi::fast_forward, mdi::view_list};
        QList<char32_t> drawn;
        for (char32_t k = 0; k < std::size(icons); k++) drawn << 0xF0000 + k;
        const QByteArray bytes = menufont::build(font(), QString::fromUtf8("·…–—’“”éèêàçüöäßñ"), menufont::times(menufont::kBoxes, m_boxScale), drawn,
                                                 [](QPainter &p, const QRectF &box, char32_t c) { theme::drawIcon(&p, icons[c - 0xF0000], box, Qt::white); });
        if (QSaveFile f(base + ".font"); f.open(QIODevice::WriteOnly) && f.write(bytes) > 0 && f.commit()) m_boxFont = base + ".font";
        if (menufont::logo(QImage(":/logo.png"), (int)std::lround(18 * m_boxScale)).save(base + ".logo.bmp", "BMP")) m_boxLogo = base + ".logo.bmp";
    }
    if (m_boxFont.isEmpty()) return;   // (the boxes as they were: SDL's 8x8 font)
    env.insert("HK_UI_LOOK", theme::menuLook());
    env.insert("HK_UI_FONT", m_boxFont);
    env.insert("HK_UI_LOGO", m_boxLogo);
    env.insert("HK_UI_SCALE", QString::number(m_boxScale, 'f', 3));
}

// A window under 900 pixels wide (a 640x480 screen's, a CRT's): the Details panel beside the list
// or the cards gets a share it can show the chosen game in (a layout saved on a larger screen may
// leave it a sliver), and keeps it until the window is wide again; the player may move it meanwhile
void MainWindow::resizeEvent(QResizeEvent *e) {
    QMainWindow::resizeEvent(e);
    const bool narrow = width() < 900;
    if (narrow == m_narrow) return;
    m_narrow = narrow;
    m_list->setColumnHidden(Year, narrow);   // (the Details panel says it: the titles get the room)
    if (narrow && m_detailsDock->isVisible() && dockWidgetArea(m_detailsDock) == Qt::RightDockWidgetArea)
        QTimer::singleShot(0, this, [this] {
            const int share = std::clamp(width() - 150 - 240, 240, width() * 45 / 100);   // (the shelves', one card's)
            resizeDocks({m_detailsDock}, {share}, Qt::Horizontal);
        });
}

// The icons are drawn in the text's colour (theme::icon), not taken from the desktop's icon theme:
// in the system's look they are drawn again when the desktop's colours change (its night mode)
void MainWindow::changeEvent(QEvent *e) {
    QMainWindow::changeEvent(e);
    if (e->type() != QEvent::PaletteChange || theme::pop()) return;
    const QRgb ink = QApplication::palette().color(QPalette::WindowText).rgb();
    if (ink == m_iconInk) return;
    m_iconInk = ink;
    QTimer::singleShot(0, this, [this] { themeChanged(); });
}

bool MainWindow::eventFilter(QObject *o, QEvent *e) {
    if (e->type() == QEvent::KeyPress) {
        // where the key is first given (the focus), not again on its way up to the parents
        auto *w = qobject_cast<QWidget *>(o);
        auto *k = static_cast<QKeyEvent *>(e);
        QWidget *focus = QApplication::focusWidget();
        if (w && w->window() == this && (w == focus || (!focus && w == this)) && !k->isAutoRepeat()) secretKeys(k->key());
    }
    return QMainWindow::eventFilter(o, e);
}

// The newest games (the AVS2 PC ones) aren't listed until the window
// has been typed the Konami code; typed again, they are hidden again (ui/pc64)
void MainWindow::secretKeys(int key) {
    static const int code[] = {Qt::Key_Up,   Qt::Key_Up,    Qt::Key_Down, Qt::Key_Down, Qt::Key_Left,
                               Qt::Key_Right, Qt::Key_Left, Qt::Key_Right, Qt::Key_B,    Qt::Key_A};
    const int n = (int)(sizeof code / sizeof code[0]);
    if (key == code[m_secret]) m_secret++;
    else m_secret = key != Qt::Key_Up ? 0 : m_secret == 2 ? 2 : 1;   // a third ↑: still the first two
    if (m_secret < n) return;
    m_secret = 0;
    const bool on = !cfg::settings().value("ui/pc64").toBool();
    cfg::settings().setValue("ui/pc64", on);
    setWindowTitle(mainTitle());
    refresh();
}

void MainWindow::themeChanged() {
    const bool pop = theme::pop();
    // the logo: HIRAKU's picture, or in Pop its word in the six colours
    QPixmap lp = pop ? theme::wordmark(m_logoH, devicePixelRatio())
                     : QPixmap(":/logo.png").scaledToHeight(m_logoH * devicePixelRatio(), Qt::SmoothTransformation);
    lp.setDevicePixelRatio(devicePixelRatio());
    m_logo->setIcon(QIcon(lp));
    m_logo->setIconSize(lp.deviceIndependentSize().toSize());
    m_play->setIcon(pop ? theme::glyph("play", Qt::white) : theme::icon(mdi::play));
    m_stop->setIcon(pop ? theme::glyph("stop", Qt::white) : theme::icon(mdi::stop));
    theme::restyle(this);   // (the toolbar's, the menus' and the buttons' icons, in this look)
    m_music->themeChanged();
    m_cards->themeChanged();
    m_head->themeChanged();
    if (m_list->topLevelItemCount()) refresh();   // the badges and the statuses' colours
}

void MainWindow::closeEvent(QCloseEvent *e) {
    if (!m_running.isEmpty()) {
        auto r = QMessageBox::question(this, "HIRAKU", "A game is running. Stop it and quit?");
        if (r != QMessageBox::Yes) {
            e->ignore();
            return;
        }
        stop();
        m_proc.waitForFinished(5000);
    }
    planBack();
    if (m_menu.state() != QProcess::NotRunning) {   // the game menu goes with the launcher
        m_menu.terminate();
        if (!m_menu.waitForFinished(3000)) m_menu.kill();
    }
    auto &s = cfg::settings();
    s.setValue("ui/geometry", saveGeometry());
    s.setValue("ui/state", saveState(kLayout));
    if (auto *g = selected()) s.setValue("ui/lastGame", g->id);
    e->accept();
}

void MainWindow::refresh() {
    QString keep = selected() ? selected()->id : cfg::settings().value("ui/lastGame").toString();
    m_games = scanGames();
    if (!cfg::settings().value("ui/pc64").toBool())   // the AVS2 PC games: only once unlocked (secretKeys)
        m_games.erase(std::remove_if(m_games.begin(), m_games.end(), [](const Game &g) { return g.hidden; }), m_games.end());
    // series by series, each system's games by year and title
    QList<Shelf> shelves;
    QMap<QString, QSet<QString>> systemsOf;
    for (const Game &g : m_games) {
        shelves << shelf(g);
        systemsOf[shelves.last().series] << shelves.last().system;
    }
    QList<int> order;
    for (int k = 0; k < m_games.size(); k++) order << k;
    QCollator titles;   // pop'n music 9 before pop'n music 10
    titles.setNumericMode(true);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        const Shelf &x = shelves[a], &y = shelves[b];
        if (x.seriesOrder != y.seriesOrder) return x.seriesOrder < y.seriesOrder;
        if (x.series != y.series) return x.series < y.series;
        if (x.systemOrder != y.systemOrder) return x.systemOrder < y.systemOrder;
        if (m_games[a].year != m_games[b].year) return m_games[a].year < m_games[b].year;
        return titles.compare(m_games[a].title, m_games[b].title) < 0;
    });
    m_order = order;   // (the game menu lists them the same way)
    m_arranging = true;
    m_list->clear();
    QMap<QString, QTreeWidgetItem *> groups;
    QTreeWidgetItem *kept = nullptr;
    QFont bold = m_list->font();
    bold.setBold(true);
    const QColor grey = m_list->palette().color(QPalette::Disabled, QPalette::Text);
    for (int k : order) {
        const Game &g = m_games[k];
        const Shelf &sh = shelves[k];
        // a series on one system alone has no row for it: the series' row names it
        const bool oneSystem = systemsOf[sh.series].size() == 1;
        QTreeWidgetItem *&top = groups[sh.series];
        if (!top) {
            top = new QTreeWidgetItem(m_list);
            top->setText(Title, oneSystem ? sh.series + "  ·  " + sh.system : sh.series);
            top->setIcon(Title, familyIcon(g.family));
            top->setFont(Title, bold);
            top->setForeground(Status, grey);
            top->setData(Title, GroupRole, sh.series);
        }
        QTreeWidgetItem *parent = top;
        if (!oneSystem) {
            QTreeWidgetItem *&sys = groups[sh.series + "/" + sh.system];
            if (!sys) {
                sys = new QTreeWidgetItem(top);
                sys->setText(Title, sh.system);
                sys->setForeground(Status, grey);
                sys->setData(Title, GroupRole, sh.series + "/" + sh.system);
            }
            parent = sys;
        }
        auto *it = new QTreeWidgetItem(parent);
        it->setText(Title, g.title);
        it->setIcon(Title, familyIcon(g.family));
        it->setText(Year, g.year);
        it->setData(Title, Qt::UserRole, g.id);
        it->setToolTip(Title, g.id + (g.mame.isEmpty() ? QString() : "  (MAME: " + g.mame + ")"));
        if (g.status != Game::Ready)
            for (int c = 0; c < NCols; c++) it->setForeground(c, grey);
        if (g.id == keep) kept = it;
    }
    m_arranging = false;
    refreshStatuses();
    applyFilter();
    // the game picked last, else the first one shown; in an open series either way (a series the
    // player closed stays closed)
    auto shown = [](QTreeWidgetItem *it) {
        bool on = !it->isHidden();
        for (auto *p = it->parent(); on && p; p = p->parent()) on = !p->isHidden() && p->isExpanded();
        return on;
    };
    if (kept && (shown(kept) || (m_asCards->isChecked() && !kept->isHidden()))) {   // (cards: its series closed in the list or not)
        m_list->setCurrentItem(kept);
        m_list->scrollToItem(kept);
    } else
        for (QTreeWidgetItem *it : gameItems())
            if (shown(it)) {
                m_list->setCurrentItem(it);
                break;
            }
    int ready = 0;
    for (auto &g : m_games) ready += g.status == Game::Ready;
    statusBar()->showMessage(QString("%1 of %2 games ready to play").arg(ready).arg(m_games.size()));
    updateActions();
    showDetails();
}

void MainWindow::applyFilter() {
    const QString f = m_filter->text().trimmed();
    auto shows = [&](const Game *g) {
        if (!g || (!m_showAll->isChecked() && (g->status == Game::NotInstalled || g->status == Game::Unavailable)))
            return false;
        if (f.isEmpty()) return true;
        const Shelf sh = shelf(*g);
        return g->title.contains(f, Qt::CaseInsensitive) || g->id.contains(f, Qt::CaseInsensitive) ||
               sh.series.contains(f, Qt::CaseInsensitive) || sh.system.contains(f, Qt::CaseInsensitive) ||
               cfg::familyName(g->family).contains(f, Qt::CaseInsensitive) || g->year.contains(f);
    };
    m_arranging = true;
    // a series' or system's row: hidden with none of its games shown; its count of them; open
    // while searching, else as the player left it
    std::function<QPair<int, int>(QTreeWidgetItem *)> visit = [&](QTreeWidgetItem *it) -> QPair<int, int> {
        if (it->childCount() == 0) {
            const Game *g = find(it->data(Title, Qt::UserRole).toString());
            const bool show = shows(g);
            it->setHidden(!show);
            return {show ? 1 : 0, show && g->status == Game::Ready ? 1 : 0};
        }
        QPair<int, int> n{0, 0};
        for (int k = 0; k < it->childCount(); k++) {
            auto c = visit(it->child(k));
            n.first += c.first, n.second += c.second;
        }
        it->setHidden(n.first == 0);
        it->setText(Status, n.first == n.second ? QString("%1 game%2").arg(n.first).arg(n.first == 1 ? "" : "s")
                                                : QString("%1 of %2 ready").arg(n.second).arg(n.first));
        it->setExpanded(!f.isEmpty() || !m_collapsed.contains(it->data(Title, GroupRole).toString()));
        return n;
    };
    for (int k = 0; k < m_list->topLevelItemCount(); k++) visit(m_list->topLevelItem(k));
    m_arranging = false;
    showEmpty();
    fillCards();
}

void MainWindow::showEmpty() {
    bool any = false;
    for (QTreeWidgetItem *it : gameItems()) any = any || !it->isHidden();
    const bool empty = !any && !m_games.isEmpty() && m_filter->text().trimmed().isEmpty() && !m_showAll->isChecked();
    m_listStack->setCurrentIndex(empty ? 1 : m_asCards->isChecked() ? 2 : 0);
    if (empty) m_detailsDock->hide();
    else if (m_detailsPane->property("hiddenEmpty").toBool()) m_detailsDock->show();   // only what this hid
    m_detailsPane->setProperty("hiddenEmpty", empty);
}

// A ready game's status says what its next start does differently: a first boot, an install, mods
static QString statusOf(const Game &g, bool *notable) {
    *notable = false;
    if (g.status != Game::Ready) return g.statusText();
    QStringList notes;
    if (installCassette(g) || installCd(g)) notes << "Installs at the next start", *notable = true;
    else if (unitState(g) == "install") notes << "Installs the network unit's data at the next start", *notable = true;
    else if (firstBoot(g)) notes << "First boot", *notable = true;
    if (const int n = cfg::modsOn(g.id).size()) notes << (n == 1 ? QString("1 mod") : QString("%1 mods").arg(n));
    if (!cfg::settings().value("gameArgs/" + g.id).toString().isEmpty()) notes << "own arguments";
    if (cfg::settings().contains("dips/" + g.id)) notes << "DIP switches set";
    if (unitState(g) == "on") notes << "e-AMUSEMENT unit";
    if (g.loader == "pc64" && cfg::settings().value("singleScreen/" + g.id).toBool()) notes << "one 4:3 screen";
    return notes.isEmpty() ? QString("Ready") : notes.join(", ");
}

void MainWindow::refreshStatuses() {
    for (QTreeWidgetItem *it : gameItems()) {
        const Game *g = find(it->data(Title, Qt::UserRole).toString());
        if (!g) continue;
        bool notable;
        it->setText(Status, statusOf(*g, &notable));
        if (g->status == Game::Ready)   // green, amber where the next start is special
            it->setForeground(Status, notable ? theme::notable() : theme::ready());
    }
    fillCards();
}

Card MainWindow::cardOf(const Game &g) const {
    const Shelf sh = shelf(g);
    Card c;
    c.id = g.id;
    c.title = g.title;
    c.detail = QStringList{g.year, sh.system}.filter(QRegularExpression(".")).join(" · ");
    c.series = sh.series;
    c.family = g.family;
    c.ready = g.status == Game::Ready;
    const QString st = statusOf(g, &c.notable);
    if (!c.ready || c.notable) c.status = st.section(", ", 0, 0);   // (the first: what the next start does)
    c.picture = menuPicture(g.id);
    return c;
}

void MainWindow::fillCards() {
    QList<Card> cards;
    for (int k : m_order) {
        if (k >= m_games.size()) continue;
        const Game &g = m_games[k];
        const QTreeWidgetItem *it = itemFor(g.id);
        if (it && !it->isHidden()) cards << cardOf(g);
    }
    m_cards->setCards(cards);
    m_cards->select(selected() ? selected()->id : QString());
}

// The music with the cards: a bar along the window's bottom, under the cards and the details, with
// no title; with the list, its panel under the details and the log, as before. Shown or closed as
// the player left it (View > Music)
void MainWindow::placeMusic(bool bar) {
    const bool shown = !m_musicDock->isHidden();
    m_music->arrange(bar);
    if (bar) {
        if (dockWidgetArea(m_musicDock) != Qt::BottomDockWidgetArea || m_musicDock->isFloating()) {
            m_musicDock->setFloating(false);
            addDockWidget(Qt::BottomDockWidgetArea, m_musicDock);
        }
        if (!m_musicDock->titleBarWidget()) m_musicDock->setTitleBarWidget(new QWidget);
    } else {
        if (dockWidgetArea(m_musicDock) == Qt::BottomDockWidgetArea) {
            addDockWidget(Qt::RightDockWidgetArea, m_musicDock);
            if (dockWidgetArea(m_logDock) == Qt::RightDockWidgetArea && !m_logDock->isFloating())
                splitDockWidget(m_logDock, m_musicDock, Qt::Vertical);
        }
        if (QWidget *t = m_musicDock->titleBarWidget()) {
            m_musicDock->setTitleBarWidget(nullptr);
            delete t;
        }
    }
    m_musicDock->setVisible(shown);
}

void MainWindow::showAsCards(bool on) {
    cfg::settings().setValue("ui/view", on ? "cards" : "list");
    (on ? m_asCards : m_asList)->setChecked(true);
    placeMusic(on);
    showEmpty();
    if (const Game *g = selected()) {
        if (on) m_cards->select(g->id);
        else if (QTreeWidgetItem *it = itemFor(g->id)) selectGame(it);
    }
    showDetails();
    if (on) m_cards->focusCards();
    else m_list->setFocus();
}

void MainWindow::gameChanged() {
    refreshStatuses();
    showDetails();
}

QList<QTreeWidgetItem *> MainWindow::gameItems() const {
    QList<QTreeWidgetItem *> out;
    std::function<void(QTreeWidgetItem *)> walk = [&](QTreeWidgetItem *it) {
        for (int k = 0; k < it->childCount(); k++) {
            if (it->child(k)->childCount()) walk(it->child(k));
            else out << it->child(k);
        }
    };
    walk(m_list->invisibleRootItem());
    return out;
}

QTreeWidgetItem *MainWindow::itemFor(const QString &id) const {
    for (QTreeWidgetItem *it : gameItems())
        if (it->data(Title, Qt::UserRole).toString() == id) return it;
    return nullptr;
}

void MainWindow::selectGame(QTreeWidgetItem *it) {
    for (auto *p = it->parent(); p; p = p->parent()) p->setExpanded(true);
    m_list->setCurrentItem(it);
    m_list->scrollToItem(it);
}

const Game *MainWindow::find(const QString &id) const {
    for (const Game &g : m_games)
        if (g.id == id) return &g;
    return nullptr;
}

const Game *MainWindow::selected() const {
    auto *it = m_list->currentItem();
    return it && !it->isHidden() ? find(it->data(Title, Qt::UserRole).toString()) : nullptr;
}

void MainWindow::updateActions() {
    const Game *g = selected();
    bool running = !m_running.isEmpty();
    m_play->setEnabled(g && g->status == Game::Ready && !running);
    m_stop->setEnabled(running);
    m_head->setPlayable(m_play->isEnabled());
}

static bool installCassette(const Game &g);
static bool hasInstallCd(const Game &g);
static bool installCd(const Game &g);
static bool hasNetworkUnit(const Game &g);
static bool unitInstalled(const Game &g);
static QString unitState(const Game &g);
static QString nvramSeed(const Game &g);

// a folder and everything in it (a game's saves, for a mod set's first start). Not what its links
// lead to: a PC game's saves link its data (prog/data -> the game's gigabytes), which the core makes
// again each run; followed, the copy took minutes, and one cut short left a folder of part of the
// data where the link belongs (EMPRESS's movies missing: "Could not create source filter")
// false: a file couldn't be copied (a full disk), and `to` holds part of the saves
static bool copyTree(const QString &from, const QString &to) {
    if (!QDir().mkpath(to)) return false;
    for (const QFileInfo &f : QDir(from).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System)) {
        if (f.isSymLink() || f.isJunction()) continue;
        if (f.isDir() ? !copyTree(f.filePath(), to + "/" + f.fileName()) : !QFile::copy(f.filePath(), to + "/" + f.fileName())) return false;
    }
    return true;
}

void MainWindow::showDetails() {
    const Game *g = selected();
    if (!g) {
        m_head->hide();
        bool any = false;
        for (QTreeWidgetItem *it : gameItems()) any = any || !it->isHidden();
        const QString f = m_filter->text().trimmed();
        m_details->setText(m_games.isEmpty()
                               ? "No games found in " + cfg::rootDir().toHtmlEscaped() +
                                     "/games. Is the HIRAKU folder right (Settings)?"
                           : any ? QString()
                           : !f.isEmpty() ? "No game matches <b>" + f.toHtmlEscaped() + "</b>."
                                          : QString());   // no game added: the list's place says how (showEmpty)
        return;
    }
    // the cards view: the game large over its details (gamecards.h), which then start after its title
    const bool large = m_asCards->isChecked();
    m_head->setVisible(large);
    if (large) {
        Head head;
        head.card = cardOf(*g);
        head.line = QStringList{g->maker, g->year, shelf(*g).system, g->statusText()}.filter(QRegularExpression(".")).join(" · ");
        for (const QString &m : cfg::modsOn(g->id)) head.mods << ModsDialog::read(cfg::modPath(g->id, m)).name;
        head.canPlay = m_play->isEnabled();
        head.unlocks = g->status == Game::Ready && UnlocksDialog::available(g->id);
        m_head->set(head);
    }
    QString h = large ? QString() : "<b>" + g->title.toHtmlEscaped() + "</b>";
    if (!large && !g->maker.isEmpty()) h += " · " + g->maker.toHtmlEscaped() + " " + g->year;
    if (!large) h += " · " + g->statusText();
    if (g->status != Game::Ready) {
        QString how = g->howTo().toHtmlEscaped();
        int pre = how.indexOf("\n\n", how.indexOf("\n\n") + 2);   // the commands, after the second paragraph break
        h += "<p>" + how.left(pre).replace("\n\n", "</p><p>") + "</p>";
        if (pre >= 0) h += "<pre>" + how.mid(pre + 2) + "</pre>";   // (a not-installed text has none)
    } else {
        bool fresh = !QFileInfo::exists(cfg::nvramDir(g->id));
        // Keyboardmania, ParaParaParadise: not set up until test mode has written the sound flash
        // (without it most sounds are missing; the game itself doesn't say)
        if (!fresh && (g->family == "kbm" || g->family == "ppp"))
            fresh = !QFileInfo::exists(cfg::nvramDir(g->id) + "/flash1.bin");
        if (fresh && !nvramSeed(*g).isEmpty())
            h += "<p><b>First boot:</b> the game starts installed, from the saves kept with its data "
                 "(<tt>nvram-seed</tt>, from a MAME NVRAM folder).</p>";
        else if (fresh && !g->firstBoot.isEmpty())
            h += "<p><b>First boot:</b> " + g->firstBoot.toHtmlEscaped() + "</p>";
        if (installCassette(*g))
            h += "<p><b>The install cassette is in the slot</b> for the next start: the game installs itself from "
                 "CD to flash. When it says the install is done (INFO 506), stop it and play again: it then boots "
                 "with the game's cassette.</p>";
        if (installCd(*g))
            h += "<p><b>The install CD is in the drive</b> for the next start: the game installs itself from it. "
                 "When it says the install is done, stop it and play again: it then boots from the game's CD.</p>";
        if (hasNetworkUnit(*g)) {
            const QString u = unitState(*g);
            if (u == "install")
                h += "<p><b>e-AMUSEMENT:</b> the next start installs the network unit's data from its CD (about 15 to 20 "
                     "minutes of the game's time; hold Tab to fast-forward). When the game shows INSTALLATION COMPLETED, "
                     "stop it: the unit is then fitted from the next start.</p>";
            else if (u == "on")
                h += "<p><b>e-AMUSEMENT:</b> the network unit is fitted (Game menu). Turn e-AMUSEMENT on in the game's test "
                     "mode; each guitar or drum kit has a card unit (Card in Controls). The server is HIRAKU's own, or the "
                     "one in Settings &gt; e-AMUSEMENT.</p>";
            else
                h += "<p><b>e-AMUSEMENT:</b> needs the cabinet's network unit, whose hard disk installs once from its CD: "
                     "Game &gt; Install the e-AMUSEMENT network unit.</p>";
        }
        h += "<p>Saves: <tt>" + cfg::saveDir(g->id).toHtmlEscaped() + "</tt>" + (fresh ? " (none yet)" : "") + "</p>";
        if (const QString mods = ModsDialog::summary(g->id); !mods.isEmpty())
            h += "<p><b>Mods on:</b> " + mods.toHtmlEscaped() + " (Game &gt; Mods…)</p>";
        QString m = picture::mode(g->id);
        bool own = cfg::settings().contains("picture/" + g->id) || QFileInfo::exists(picture::configDir(g->id));
        if (UnlocksDialog::available(g->id)) h += "<p>Unlocks and codes: Game &gt; Unlocks… (or right-click the game).</p>";
        if (const QString dips = DipsDialog::summary(g->id); !dips.isEmpty())
            h += "<p><b>DIP switches:</b> " + dips.toHtmlEscaped() + " (Game &gt; DIP switches…)</p>";
        if (const QString own = cfg::settings().value("gameArgs/" + g->id).toString(); !own.isEmpty())
            h += "<p>Its own arguments: <tt>" + own.toHtmlEscaped() + "</tt> (Game &gt; Arguments…)</p>";
        h += QString("<p>Picture: %1%2</p>")
                 .arg(m == "full"    ? "CRT full (crt-royale)"
                      : m == "light" ? "CRT light (crt-guest-advanced-fast)"
                      : m == "lola"  ? "CRT analog (Lola Retro)"
                                     : "sharp pixels",
                      own ? ", with its own settings" : "");
    }
    h += filesHtml(*g);
    m_details->setText(h);
}

// Saves to start a game from, kept with its data by tools/ingest.py (--nvram-seed): a MAME NVRAM
// folder with the game installed, so a System 573 game's first start skips its install from CD.
// "" if none.
static QString nvramSeed(const Game &g) {
    QString d = cfg::rootDir() + "/dumps/" + g.id + "/nvram-seed";
    return QFileInfo(d).isDir() ? d : QString();
}

// No saves yet, and none to start from: the game's next start is a first boot, as on a new cabinet.
// Keyboardmania, ParaParaParadise: until test mode has written the sound flash.
static bool firstBoot(const Game &g) {
    bool fresh = !QFileInfo::exists(cfg::nvramDir(g.id));
    if (!fresh && (g.family == "kbm" || g.family == "ppp")) fresh = !QFileInfo::exists(cfg::nvramDir(g.id) + "/flash1.bin");
    return fresh && nvramSeed(g).isEmpty();
}

// A System 573 game that installs from CD with an install cassette (ddr3mj): its first boot (no
// saves yet, and none to start from) has that cassette in the slot, as does the next start after
// "Boot once with the install cassette" (an install stopped halfway). Otherwise the game's own cassette.
static bool installCassette(const Game &g) {
    if (!g.installCassette) return false;
    if (cfg::settings().value("cassette/" + g.id).toString() == "install") return true;
    return !QFileInfo::exists(cfg::nvramDir(g.id)) && nvramSeed(g).isEmpty();
}

// A System 573 game with an install CD (MAME's "install" disc: ddr2mla, ddrbocd, the club versions):
// its machine.txt names it (tools/ingest.py, install_cd)
static bool hasInstallCd(const Game &g) {
    QFile f(cfg::rootDir() + "/dumps/" + g.id + "/machine.txt");
    if (g.core != "sys573" || !f.open(QIODevice::ReadOnly)) return false;
    for (const QByteArray &line : f.readAll().split('\n'))
        if (line.startsWith("install_cd=") && line.trimmed().size() > 11) return true;
    return false;
}

// ... and booted from it on its first start (no saves, none to start from), or the next start after
// "Boot once with the install CD" (an install stopped halfway), as installCassette() does
static bool installCd(const Game &g) {
    if (!hasInstallCd(g)) return false;
    if (cfg::settings().value("cd/" + g.id).toString() == "install") return true;
    return !QFileInfo::exists(cfg::nvramDir(g.id)) && nvramSeed(g).isEmpty();
}

// A System 573 game with the network PCB unit (e-AMUSEMENT GuitarFreaks and DrumMania,
// runtime/sys573/npu.h) and the unit's installer CD ingested: machine.txt's npu=1 and hdd_cd
static bool hasNetworkUnit(const Game &g) {
    QFile f(cfg::rootDir() + "/dumps/" + g.id + "/machine.txt");
    if (g.core != "sys573" || !f.open(QIODevice::ReadOnly)) return false;
    bool npu = false, cd = false;
    for (const QByteArray &line : f.readAll().split('\n')) {
        npu = npu || line.trimmed() == "npu=1";
        cd = cd || (line.startsWith("hdd_cd=") && line.trimmed().size() > 7);
    }
    return npu && cd;
}
// ... its disk installed: the save folder's npu_hdd/, formatted by the installer, with its songs
static bool unitInstalled(const Game &g) {
    const QString d = cfg::nvramDir(g.id) + "/npu_hdd";
    return QFileInfo::exists(d + "/.format") && QFileInfo::exists(d + "/stage");
}
// ... what the next start does with it: "install" (its data from the installer CD), "on" (fitted),
// or "" (none: the game offline, as before)
static QString unitState(const Game &g) {
    if (!hasNetworkUnit(g)) return {};
    const QString v = cfg::settings().value("npu/" + g.id).toString();
    return v == "install" || (v == "on" && unitInstalled(g)) ? v : QString();
}

// The sound's way out (Settings > Audio), for a game or the game menu
static void audioArguments(QStringList &a) {
    auto &s = cfg::settings();
    a << "--audio-ms" << QString::number(s.value("audio/ms", 8).toInt());
    if (s.value("audio/fixed", false).toBool()) a << "--audio-fixed";
    if (int v = s.value("audio/volumeDb", 0).toInt(); v) a << "--volume" << QString::number(v);
    if (QString d = s.value("audio/device").toString(); d.startsWith("asio:")) a << "--audio-asio" << d.mid(5);   // Windows
    else {
        if (!d.isEmpty()) a << "--audio-device" << d;
#ifdef _WIN32
        if (s.value("audio/exclusive", false).toBool()) a << "--audio-exclusive";
#endif
    }
    if (int f = s.value("audio/buffer", 0).toInt(); f > 0) a << "--audio-buffer" << QString::number(f);
}

// A cabinet's own parts, for a game of that family or the game menu driven by its controls: the
// family's controls and lights files, the e-AMUSEMENT card readers, the I/O boards
static void cabinetArguments(QStringList &a, const QString &family) {
    auto &s = cfg::settings();
    QString controls = cfg::controlsFile(family), lights = cfg::lightsFile(family);
    if (QFileInfo::exists(controls)) a << "--bindings" << controls;
    if (lightsFileUsed(lights)) a << "--lights" << lights;
    if (s.value("lights/socket", true).toBool()) a << "--lights-socket" << cfg::lightsSocket();
    // the card readers (runtime/host/readers.h), for every game: one with no reader of its own (a
    // beatmania on an IIDX cabinet) is still left for the game menu by their 10-key's 5, 7 and 3
    // (runtime/host/menukey.h)
    const QString r0 = s.value("readers/serial0").toString(), r1 = s.value("readers/serial1").toString();
    if (!r0.isEmpty() || !r1.isEmpty()) a << "--reader" << r0;   // "": player 2's only
    if (!r1.isEmpty()) a << "--reader" << r1;
    if (s.value("readers/scard", false).toBool()) a << "--scard";
    if (s.value("readers/cardio", false).toBool()) a << "--cardio";
    if (QString b = s.value("cabinet/bio2").toString(); !b.isEmpty() && cfg::isBeatmania(family))
        a << "--bio2" << b;   // the IIDX cabinet's controls and lamps (runtime/host/bio2.h)
    if (QString b = s.value("cabinet/popnio").toString(); !b.isEmpty() && cfg::controlsSet(family) == "popn") {
        a << "--popn-io" << b;   // the pop'n cabinet's buttons and lamps (runtime/host/popnio.h)
        if (QString f = s.value("cabinet/popnioDll").toString(); !f.isEmpty()) a << "--popn-io-dll" << f;
    }
    // a System 573 cabinet's USB_2_573 board (runtime/host/usb573.h): by default, when one is plugged in
    if (QString b = s.value("cabinet/usb573", "present").toString(); b != "off" && cfg::isSystem573(family))
        a << "--usb573" << b;
}

QStringList MainWindow::arguments(const Game &g) const {
    auto &s = cfg::settings();
    QStringList a = {"--window", QString::number(s.value("video/scale", 2).toInt())};
    if (g.coreBinary) a = QStringList{"--game", g.id} + a;   // the core's own binary (games.h)
    if (s.value("video/fullscreen", false).toBool()) a << "--fullscreen";
    if (!s.value("video/displaySync", true).toBool()) a << "--display-sync" << "off";   // runtime/host/pacing.h
    else if (g.core == "djmain" || g.core.isEmpty() || g.core == "firebeat" || g.core == "twinkle" || g.core == "sys573" ||
             g.core == "pc") {
        if (s.value("video/crtBeam", false).toBool())   // docs/SMOOTH.md
            a << "--crt-beam" << "--crt-beam-gamma" << QString::number(s.value("video/crtBeamGamma", 2.2).toDouble())
              << "--crt-beam-gain" << QString::number(s.value("video/crtBeamGain", 0.7).toDouble());
        else if (s.value("video/smoothMotion", false).toBool())
            a << "--smooth-motion";
    }
    // the picture mode, and its settings: all games', then the game's (picturedialog.h)
    a << "--picture" << picture::mode(g.id) << "--picture-config" << picture::configDir() << "--picture-config"
      << picture::configDir(g.id);
    for (const cfg::ShaderKey &k : cfg::shaderKeys())   // the in-game shader settings' controls (Settings > Display)
        if (s.contains(QString("shaderKeys/") + k.action))
            a << "--shader-key" << QString("%1=%2").arg(k.action, s.value(QString("shaderKeys/") + k.action).toString());
    audioArguments(a);
    if (!s.value("system/priority", true).toBool()) a << "--normal-priority";
    if (s.value("video/splitScreens", false).toBool() && g.family == "kbm") a << "--split-screens";
    // every millisecond is the cores' default (2026-10-20); the key is new, so that the old one's
    // stored "off" (saved by any OK in Settings) doesn't keep it off
    if ((g.core == "sys573" || g.core == "twinkle") && !s.value("audio/soundEveryMs", true).toBool()) a << "--mame-sound";
    cabinetArguments(a, g.family);
    // the way out to the game menu from the cabinet's controls (Settings > General; runtime/host/menukey.h)
    if (!s.value("menu/enabled", true).toBool()) a << "--game-menu" << "off";
    else {
        if (const double h = s.value("menu/hold", 2.0).toDouble(); h != 2.0) a << "--game-menu-hold" << QString::number(h);
        if (const QString k = s.value("menu/key").toString().trimmed(); !k.isEmpty()) a << "--game-menu-key" << k;
        // its picture of this game: the title screen, kept by the first run that reaches it untouched
        // (runtime/host/menupic.h); asked for until there is one
        if (g.titleSeconds > 0 && s.value("menu/pictures", true).toBool())
            if (const QString kept = cfg::menuPicturesDir() + "/auto/" + g.id + ".bmp"; !QFileInfo::exists(kept)) a << "--menu-picture" << kept;
        // F12 in the game: the screen as its picture, the player's own (menuPicture: the newest of those)
        a << "--menu-picture-own" << cfg::menuPicturesDir() + "/" + g.id + ".bmp";
    }
    if (s.value("lights/overlay", false).toBool()) a << "--lights-overlay";   // runtime/host/lampview.h
    if (const QString o = s.value("video/statsOverlay", "off").toString(); o != "off") a << "--stats-overlay" << o;   // host/statsview.h
    // F6's recordings (Settings > Display > Recording; runtime/host/record.h), in the saves' folder's
    a << "--record-folder" << cfg::recordingsDir()
      << recording::arguments(s.value("record/format", "ffv1").toString(), s.value("record/encoder", "auto").toString(),
                              s.value("record/scale", 0).toInt());
    if (s.value("game/fastBoot", true).toBool() && g.core != "pc") {   // runtime/host/warp.h
        // a start that installs the game's data (minutes): fast to the install's end instead
        if (g.installSeconds > 0 && (firstBoot(g) || installCassette(g) || installCd(g)))
            a << "--fast-boot-seconds" << QString::number(g.installSeconds);
        else
            a << "--fast-boot";
    }
    QString sf = s.value("audio/soundfont").toString();
    if (g.family == "kbm" && !sf.isEmpty()) a << "--soundfont" << sf;
    // the Konami Python games' GS (Settings > Display): Vulkan by default, less CPU than the
    // software one (PLAN.md section 23)
    if (g.core == "python") a << "--renderer" << s.value("video/pythonRenderer", "vk").toString();
    if (g.core == "python" || g.core == "pc") {   // e-AMUSEMENT, as spice2x's options (runtime/shell/eaoptions.h, docs/EAMUSE3.md)
        const QString url = s.value("eamuse/url", s.value("eamuse/server")).toString();
        if (!url.isEmpty()) a << "-url" << url;
        if (!url.isEmpty() && s.value("eamuse/smartea", false).toBool()) a << "-smartea";
        if (QString p = s.value("eamuse/pcbid").toString(); !p.isEmpty()) a << "-p" << p;
        for (int n = 0; n < 2; n++)
            if (QString c = s.value(QString("eamuse/card%1").arg(n)).toString(); !c.isEmpty()) a << QString("-card%1").arg(n) << c;
        if (QString ac = s.value("eamuse/autocard", "off").toString(); ac != "off") a << "-autocard" << ac;
        if (s.value("eamuse/autopin", false).toBool()) a << "-autopin";
    }
    if (g.loader == "pc64" && s.value("singleScreen/" + g.id).toBool()) a << "--single-screen";   // (Game menu)
    if (g.loader == "pc64" && !s.value("market/" + g.id).toString().isEmpty()) a << "--market" << s.value("market/" + g.id).toString();
    if (g.core == "pc") {   // pop'n music 15-18's presses (runtime/pc/press.c)
        if (s.value("game/pressTime", false).toBool()) a << "--press-time";
        if (int ms = s.value("game/judgeOffset", 0).toInt(); ms) a << "--judge-offset" << QString::number(ms);
    }
    // mods (docs/MODS.md): each folder in order, and the saves of their own (cfg::saveDir)
    const QStringList mods = cfg::modsOn(g.id);
    for (const QString &m : mods) a << "--mod" << cfg::modPath(g.id, m);
    if (!mods.isEmpty()) a << "--nvram" << cfg::saveDir(g.id);
    else if (!cfg::nvramBase().isEmpty()) a << "--nvram" << cfg::nvramDir(g.id);
    // beatmania III's floppy disk (Game menu): the game's own in its save folder, or a chosen image
    if (QString f = s.value("floppy/" + g.id).toString(); g.family == "bm3" && !f.isEmpty()) a << "--floppy" << f;
    // the System 573's memory card reader (Game menu): a PlayStation memory card in each slot
    for (const char *slot : {"1", "2"})
        if (QString f = s.value(QString("memcard%1/").arg(slot) + g.id).toString(); g.memoryCards && !f.isEmpty())
            a << QString("--memcard%1").arg(slot) << f;
    // each e-AMUSEMENT card its own disk or memory cards (Game menu; runtime/shell/cardmedia.h): without
    // a card reader the Floppy / Memory Card keys present Settings' e-AMUSEMENT card numbers
    if (s.value("cardMedia/" + g.id).toBool() && (g.family == "bm3" || g.memoryCards)) {
        a << "--card-media";
        for (int n = 0; n < 2; n++)
            if (QString c = s.value(QString("eamuse/card%1").arg(n)).toString(); !c.isEmpty()) a << QString("-card%1").arg(n) << c;
    }
    if (installCassette(g)) a << "--cassette" << "install";
    if (installCd(g)) a << "--cd" << "install";
    // the network unit (Game menu): its data installed from its CD, or fitted, with the e-AMUSEMENT
    // server set in Settings (else the one in the core, offline)
    if (unitState(g) == "install") a << "--cd" << "hdd";
    else if (unitState(g) == "on") {
        a << "--npu";
        if (QString url = s.value("eamuse/url", s.value("eamuse/server")).toString().trimmed(); !url.isEmpty()) {
            a << "--eamuse" << (url.contains("://") ? url : "http://" + url + "/");
            // the cabinet's own PCBID for that server (Settings > e-AMUSEMENT), as the other games get it
            if (QString p = s.value("eamuse/pcbid").toString(); !p.isEmpty()) a << "-p" << p;
        }
    }
    a << DipsDialog::arguments(g.id);            // the board's DIP switches (Game > DIP switches)
    a << UnlocksDialog::macroArguments(g.id);   // codes typed at every start
    a << QProcess::splitCommand(s.value("game/extraArgs").toString());   // every game's (Settings > General)
    a << QProcess::splitCommand(s.value("gameArgs/" + g.id).toString());  // ... then this one's (Game menu)
    return a;
}

void MainWindow::controls(const QString &family) {
    ControlsDialog d(family, this);
    d.setGames(m_games);
    if (d.exec() != QDialog::Accepted || d.gameToTry().isEmpty()) return;
    statusBar()->showMessage("Trying the controls: once the game is running, " + d.testHint(), 60000);
    play(d.gameToTry());
}

// The PC games on Windows with an NVIDIA card: its "Vulkan/OpenGL present method" for the program
// that shows them, in the run's log. It mattered while HIRAKU's window used OpenGL's own swap (its
// full screen and the game's Direct3D couldn't both be had without "Prefer layered on DXGI
// Swapchain"); HIRAKU now presents through a DXGI swap chain of its own, which works with the
// driver's default (an IIDX cabinet's GTX 1650), so the steps (nv::notice) are no longer shown. A
// run that falls back to OpenGL's swap says so, with the setting, in its log (host/window.cpp)
void MainWindow::nvidiaNotice(const Game &g) {
    const QString exe = QFileInfo(g.exe).fileName();
    const nv::PresentMethod m = nv::presentMethod(exe);
    if (!m.nvidia) return;
    log("NVIDIA: \"Vulkan/OpenGL present method\" for " + exe + ": " + nv::presentMethodName(m.value) + " (" + m.from + ")\n");
}

// The PC games on Linux run under Wine, which HIRAKU doesn't bring: not found (and no --wine among the
// game's arguments), the player is told how to get it, and the game isn't started (it closed at once,
// "exited (code 0)", its log saying wineboot said 127)
bool MainWindow::wineMissing(const Game &g) {
#ifdef Q_OS_WIN
    (void)g;
    return false;
#else
    if (g.core != "pc" || arguments(g).contains("--wine") || !QStandardPaths::findExecutable("wine").isEmpty()) return false;
    cover(false);
    QMessageBox box(this);
    box.setWindowTitle(g.title);
    box.setIcon(QMessageBox::Warning);
    box.setText(g.title + " needs Wine, which isn't installed.");
    box.setInformativeText("On Linux the PC games (pop'n music 15 on, beatmania IIDX 9th style on) are Konami's Windows "
                           "programs, run under Wine. Install it with your distribution's packages, then start the game "
                           "again:\n\n"
                           "Arch, CachyOS, Manjaro: sudo pacman -S wine (the multilib repository on)\n"
                           "Debian, Ubuntu, Mint: sudo apt install wine\n"
                           "Fedora: sudo dnf install wine\n\n"
                           "The first start then prepares Wine's folder for the PC games, which takes a minute.");
    box.setTextInteractionFlags(Qt::TextSelectableByMouse);
    box.exec();
    return true;
#endif
}

void MainWindow::play(const QString &id) {
    const Game *g = find(id);
    if (!g) {
        statusBar()->showMessage("No game " + id);
        return;
    }
    if (!m_running.isEmpty()) return;
    if (m_menu.state() != QProcess::NotRunning) {   // started from the launcher's window with the game menu up: the menu gives way
        m_menu.terminate();
        if (!m_menu.waitForFinished(3000)) m_menu.kill(), m_menu.waitForFinished(1000);
    }
    if (g->status != Game::Ready) {
        cover(false);   // (from the game menu: what is said mustn't be under the black)
        QMessageBox box(this);
        box.setWindowTitle(g->title);
        box.setIcon(QMessageBox::Information);
        box.setText(g->title + " is not ready to play.");
        box.setInformativeText(g->howTo());
        box.exec();
        return;
    }
    if (wineMissing(*g)) return;
    m_log->clear();
    m_runOutput.clear();
    openRunLog(g->id);
    // the first start of a game with saves to start from: they become its saves
    // (a copy that fails, a full disk: the part made is taken away, so the next start makes it again,
    // not kept as the game's saves for good; and the game isn't started on half its saves)
    const auto copyFailed = [&](const QString &from, const QString &to) {
        QDir(to).removeRecursively();
        closeRunLog();
        cover(false);
        QMessageBox::warning(this, "HIRAKU", QString("%1's saves couldn't be copied from\n%2\nto\n%3\n\nIs the disk full?").arg(g->title, from, to));
    };
    if (QString seed = nvramSeed(*g); !seed.isEmpty() && !QFileInfo::exists(cfg::nvramDir(g->id))) {
        bool copied = QDir().mkpath(cfg::nvramDir(g->id));
        for (const QFileInfo &f : QDir(seed).entryInfoList(QDir::Files))
            copied = copied && QFile::copy(f.filePath(), cfg::nvramDir(g->id) + "/" + f.fileName());
        if (!copied) return copyFailed(seed, cfg::nvramDir(g->id));
        log("saves: started from " + seed + "\n");
    }
    // the first start with these mods: their saves start as a copy of the clean game's
    if (const QString mod_saves = cfg::saveDir(g->id); mod_saves != cfg::nvramDir(g->id) && !QFileInfo::exists(mod_saves)) {
        if (QFileInfo::exists(cfg::nvramDir(g->id))) {
            if (!copyTree(cfg::nvramDir(g->id), mod_saves)) return copyFailed(cfg::nvramDir(g->id), mod_saves);
            log("saves: with mods, started from a copy of " + cfg::nvramDir(g->id) + "\n");
        } else {
            QDir().mkpath(mod_saves);
        }
    }
    QStringList args = arguments(*g);
    log("$ " + g->exe + " " + args.join(' ') + "\n");
    // Windows' High performance power plan while games run, if asked for (Settings > Audio; powerplan.h):
    // until the launcher's window is back (planBack), so not put back and changed again between a
    // game and the game menu
    if (powerplan::available() && cfg::settings().value("system/highPerformance", false).toBool())
        if (const QString said = powerplan::raise(cfg::settings(), powerplan::windowsApi()); !said.isEmpty()) log(said + "\n");
    if (g->core == "pc") nvidiaNotice(*g);
    m_running = g->id;
    m_stopping = false;
    m_fromMenu = false;   // (the menu says so itself, once the game has started)
    m_run++;
    m_proc.setWorkingDirectory(cfg::rootDir());
    {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("HIRAKU_ROOT", cfg::rootDir());   // the games use the launcher's root
        env.insert("HK_LAUNCHER", "1");   // the launcher says when a new release is out, not the game (host/notice.h)
        // a server's key (Settings > e-AMUSEMENT): this way and not as -eakey, so that it is in no
        // command line and no run's log (runtime/shell/eaoptions.h)
        auto &st = cfg::settings();
        if (const QString key = st.value("eamuse/key").toString();
            !key.isEmpty() && !st.value("eamuse/url", st.value("eamuse/server")).toString().trimmed().isEmpty())
            env.insert("HK_EA_KEY", key);
        boxLook(env);
        m_proc.setProcessEnvironment(env);
    }
    m_proc.start(g->exe, args);
    m_unitInstall = unitState(*g) == "install" ? g->id : QString();
    cfg::settings().remove("cassette/" + g->id);   // "Boot once with the install cassette" is used up
    cfg::settings().remove("cd/" + g->id);         // ... and "Boot once with the install CD"
    statusBar()->showMessage("Running " + g->title);
    cfg::settings().setValue("ui/lastGame", g->id);
    updateActions();
    if (cfg::settings().value("ui/hideWhileRunning", false).toBool()) hide();
}

void MainWindow::stop() {
    if (m_running.isEmpty()) return;
    m_stopping = true;
    m_proc.terminate();   // SIGTERM: the game saves its NVRAM and exits at the next frame
    // (this run only: a game started within those 5 s, after this one went, isn't the one asked to stop)
    QTimer::singleShot(5000, this, [this, run = m_run] {
        if (run == m_run && m_proc.state() != QProcess::NotRunning) m_killed = true, m_proc.kill();
    });
}

QString MainWindow::lastGame() const { return cfg::settings().value("ui/lastGame").toString(); }

// The game menu (runtime/menu/menu.h): the games ready to play, in the list's order and on its
// series' shelves, those the cabinet's controls can play (the controls set of the game just left;
// Settings > General: or all of them), driven by that game's family's controls
// A game's picture for the menu, "" if it has none: the player's own (menu-pictures/<id>.png, .jpg,
// .bmp: whatever Qt reads), made once into the BMP the menu reads, at the size it shows; else the
// title screen a run of the game kept (runtime/host/menupic.h)
static QString menuPicture(const QString &id) {
    const QString dir = cfg::menuPicturesDir();
    for (const QFileInfo &own : QDir(dir).entryInfoList({id + ".*"}, QDir::Files, QDir::Time)) {   // (the newest: F12's, or one chosen)
        const QString made = dir + "/auto/" + id + "-own.bmp";
        if (QFileInfo(made).exists() && QFileInfo(made).lastModified() >= own.lastModified()) return made;
        QImage img(own.filePath());
        if (img.isNull()) continue;
        if (img.height() > 264) img = img.scaledToHeight(264, Qt::SmoothTransformation);
        QDir().mkpath(dir + "/auto");
        if (img.convertToFormat(QImage::Format_RGB888).save(made, "BMP")) return made;
    }
    const QString kept = dir + "/auto/" + id + ".bmp";
    return QFileInfo::exists(kept) ? kept : QString();
}

void MainWindow::openMenu(const QString &left, const QString &notice) {
    if (!m_running.isEmpty() || m_menu.state() != QProcess::NotRunning) return;
    auto &s = cfg::settings();
    const auto giveUp = [this](const QString &why) {
        cover(false);
        if (isHidden()) show();
        statusBar()->showMessage(why);
    };
    // the cabinet: the game just left's, else the first ready game's
    const Game *from = find(left);
    if (!from || from->status != Game::Ready) {
        from = nullptr;
        for (int k : m_order)
            if (m_games[k].status == Game::Ready) {
                from = &m_games[k];
                break;
            }
    }
    if (!from) return giveUp("The game menu: no game is ready to play yet");
    const bool all = s.value("menu/games", "fit").toString() == "all";
    QString exe = cfg::binDir() + "/" + cfg::exeName("hiraku-menu");
    if (!QFileInfo(exe).isExecutable()) return giveUp("The game menu (hiraku-menu) isn't in " + cfg::binDir());
    const QString base = QDir::tempPath() + QString("/hiraku-menu-%1").arg(QCoreApplication::applicationPid());
    m_menuList = base + ".list", m_menuChoice = base + ".choice", m_menuFont = base + ".font", m_menuLogo = base + ".logo.bmp";
    QFile::remove(m_menuChoice);
    // The games, with their names as they are written: the menu sets them in this window's fonts,
    // handed over as a file (menufont.h). If that can't be made, in the menu's own 8x8 font, which
    // has ASCII only: the names without their other characters
    struct Listed {
        const Game *g;
        Shelf sh;
    };
    QList<Listed> listed;
    QString text = notice;
    for (int k : m_order) {
        const Game &g = m_games[k];
        if (g.status != Game::Ready || (!all && !cfg::sameCabinet(g.family, from->family))) continue;
        listed << Listed{&g, shelf(g)};
        text += listed.last().sh.series + g.title + g.year + listed.last().sh.system;
    }
    bool fonts = false;
    if (QSaveFile f(m_menuFont); f.open(QIODevice::WriteOnly)) fonts = f.write(menufont::build(font(), text)) > 0 && f.commit();
    const bool logo = menufont::logo(QImage(":/logo.png")).save(m_menuLogo, "BMP");
    QList<gamemenu::Row> rows;
    QMap<QString, QMap<QString, int>> families;   // each shelf's games by family: its colour is its most common's
    for (const Listed &l : listed) {
        const QString detail = l.g->year + "  " + l.sh.system;
        rows << (fonts ? gamemenu::Row{l.g->id, l.sh.series, l.g->title, detail, menuPicture(l.g->id)}
                       : gamemenu::Row{l.g->id, gamemenu::plainTitle(l.sh.series), gamemenu::plainTitle(l.g->title, l.g->latin),
                                       gamemenu::plainTitle(detail), menuPicture(l.g->id)});
        families[rows.last().shelf][l.g->family]++;
    }
    QMap<QString, QString> shelfColours;   // as the cards' shelves', seen on the menu's ground
    for (auto it = families.cbegin(); it != families.cend(); ++it) {
        QString most;
        for (auto f = it->cbegin(); f != it->cend(); ++f)
            if (most.isEmpty() || f.value() > it->value(most)) most = f.key();
        shelfColours[it.key()] = theme::against(familyColour(most), QApplication::palette().color(QPalette::Window)).name();
    }
    if (QSaveFile f(m_menuList); !f.open(QIODevice::WriteOnly) || f.write(gamemenu::listText(rows, from->id, shelfColours)) < 0 || !f.commit())
        return giveUp("The game menu's list couldn't be written to " + m_menuList);
    // the display, sound and cabinet as a game of that family gets them; no saves of its own
    QStringList a = {"--window", QString::number(s.value("video/scale", 2).toInt()), "--nvram", "",
                     "--family", from->family, "--list", m_menuList, "--choice", m_menuChoice};
    if (s.value("video/fullscreen", false).toBool()) a << "--fullscreen";
    if (!s.value("video/displaySync", true).toBool()) a << "--display-sync" << "off";
    if (const QScreen *scr = screen(); scr && scr->size().width() * 10 >= scr->size().height() * 15) a << "--wide";   // a wide screen: 16:9
    if (!notice.isEmpty()) a << "--notice" << (fonts ? notice : gamemenu::plainTitle(notice));
    if (fonts) a << "--font" << m_menuFont;
    if (logo) a << "--logo" << m_menuLogo;
    a << "--look" << theme::menuLook();   // the launcher's colours (the menu's own amber without)
    audioArguments(a);
    cabinetArguments(a, from->family);
    a << QProcess::splitCommand(QString::fromLocal8Bit(qgetenv("HK_MENU_ARGS")));   // (tests: a script to drive it)
    log("\n$ " + exe + " " + a.join(' ') + "\n");
    m_menu.setWorkingDirectory(cfg::rootDir());
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("HIRAKU_ROOT", cfg::rootDir());
    env.insert("HK_LAUNCHER", "1");
    m_menu.setProcessEnvironment(env);
    cover(true);
    m_menu.start(exe, a);
    uncoverLater();
    statusBar()->showMessage("The game menu is open (the cabinet's controls choose; Escape closes it)");
    if (s.value("ui/hideWhileRunning", false).toBool()) hide();
}

void MainWindow::menuClosed(int code, QProcess::ExitStatus st) {
    const QString id = gamemenu::readChoice(m_menuChoice);
    const QString said = QString::fromLocal8Bit(m_menu.readAll());
    QFile::remove(m_menuChoice);
    QFile::remove(m_menuList);
    QFile::remove(m_menuFont);
    QFile::remove(m_menuLogo);
    const bool fine = st == QProcess::NormalExit && (code == 0 || code == 130);
    if (!fine || !said.contains("menu: chosen")) log(said);   // (what went wrong, or nothing chosen: its own lines)
    if (fine && !id.isEmpty() && find(id)) {
        cover(true);
        play(id);   // (clears the log for the game's own)
        if (!m_running.isEmpty()) {
            m_fromMenu = true;
            return uncoverLater();
        }
    }
    cover(false);
    if (isHidden()) show();
    planBack();
    statusBar()->showMessage(fine ? "The game menu was closed" : QString("The game menu stopped (code %1)").arg(code));
    if (!fine) m_logDock->show();
}

// Black over every screen while one full-screen program gives way to the next (a game to the game
// menu, the menu to a game), so that the desktop doesn't show on a cabinet's screen in between.
// Only where the games are in full screen.
void MainWindow::planBack() {
    if (!powerplan::available()) return;
    if (const QString said = powerplan::restore(cfg::settings(), powerplan::windowsApi()); !said.isEmpty()) log(said + "\n");
}

void MainWindow::cover(bool on) {
    if (!on) {
        for (QWidget *w : m_covers) w->hide();
        return;
    }
    if (!cfg::settings().value("video/fullscreen", false).toBool()) return;
    if (m_covers.isEmpty())
        for (QScreen *scr : QGuiApplication::screens()) {
            auto *w = new QWidget(nullptr, Qt::Window | Qt::FramelessWindowHint);
            w->setWindowTitle("HIRAKU");
            w->setStyleSheet("background: black");
            w->setCursor(Qt::BlankCursor);
            w->setScreen(scr);
            w->setGeometry(scr->geometry());
            m_covers << w;
        }
    for (QWidget *w : m_covers) w->showFullScreen();
    m_coverShown++;
}

// ... and away again a few seconds after the next program was started: its window is up by then
// (a game's opens before the game boots), and should it have opened behind the cover it shows now
void MainWindow::uncoverLater() {
    QTimer::singleShot(4000, this, [this, shown = m_coverShown] {
        if (shown == m_coverShown) cover(false);   // (not a cover put up since, for the next change)
    });
}

void MainWindow::contextMenu(const QPoint &pos) {
    auto *it = m_list->itemAt(pos);
    if (!it) return;
    m_list->setCurrentItem(it);
    const Game *g = selected();
    if (!g) return;
    QMenu menu;
    fillGameMenu(&menu, *g);
    menu.exec(m_list->viewport()->mapToGlobal(pos));
}

void MainWindow::fillGameMenu(QMenu *menu, const Game &g) {
    QString id = g.id, title = g.title, family = g.family;
    menu->addAction(m_play);
    menu->addAction("Mods…", this, [this, id, title] {
        ModsDialog d(id, title, this);
        if (d.exec() == QDialog::Accepted) gameChanged();
    });
    menu->addSeparator();
    menu->addAction(theme::icon(mdi::gamepad_variant), "Controls for " + cfg::controlsSetName(cfg::controlsSet(family)) + "…", this, [this, family] {
        controls(family);
    });
    menu->addAction("Lights for " + cfg::familyName(family) + "…", this, [this, family] {
        LightsDialog d(family, this);
        d.exec();
    });
    menu->addAction(theme::icon(mdi::television_classic), "Picture for " + title + "…", this, [this, id, title] {
        PictureDialog d(id, title, true, this);
        if (d.exec() == QDialog::Accepted) showDetails();
    });
    // its picture in the game menu and on its card (menuPicture): one of the player's own, or the
    // title screen a run kept (again, if the one kept isn't good)
    {
        const QString dir = cfg::menuPicturesDir();
        const QFileInfoList own = QDir(dir).entryInfoList({id + ".*"}, QDir::Files);
        const QString kept = dir + "/auto/" + id + ".bmp";
        auto *pic = menu->addMenu(theme::icon(mdi::image), "Picture in the game menu");
        pic->setToolTipsVisible(true);
        pic->addAction("Choose a picture…", this, [this, id, title, dir] {
            const QString f = QFileDialog::getOpenFileName(this, "A picture for " + title + " in the game menu", QString(),
                                                           "Pictures (*.png *.jpg *.jpeg *.bmp *.webp);;All files (*)");
            if (f.isEmpty()) return;
            if (QImage(f).isNull()) {
                QMessageBox::warning(this, "HIRAKU", "That file isn't a picture HIRAKU can read.");
                return;
            }
            QDir().mkpath(dir);
            for (const QFileInfo &o : QDir(dir).entryInfoList({id + ".*"}, QDir::Files)) QFile::remove(o.filePath());
            QFile::remove(dir + "/auto/" + id + "-own.bmp");   // (made from the one before)
            const QString to = dir + "/" + id + "." + QFileInfo(f).suffix().toLower();
            if (!QFile::copy(f, to)) QMessageBox::warning(this, "HIRAKU", "The picture couldn't be copied to " + QDir::toNativeSeparators(dir) + ".");
            fillCards();
        })->setToolTip("Shown in the game menu behind the list, and on the game's card");
        if (!own.isEmpty())
            pic->addAction(QFileInfo::exists(kept) ? "Back to its title screen" : "Remove the picture chosen", this, [this, id, dir] {
                for (const QFileInfo &o : QDir(dir).entryInfoList({id + ".*"}, QDir::Files)) QFile::remove(o.filePath());
                QFile::remove(dir + "/auto/" + id + "-own.bmp");
                fillCards();
            });
        if (QFileInfo::exists(kept))
            pic->addAction("Keep its title screen again at the next start", this, [this, kept] {
                QFile::remove(kept);
                fillCards();
            })->setToolTip("The next start keeps a new one, if no control is touched before the title screen");
    }
    if (DipsDialog::available(id))
        menu->addAction("DIP switches…", this, [this, id, title] {
            DipsDialog d(id, title, this);
            if (d.exec() == QDialog::Accepted) gameChanged();
        })->setToolTip("The board's switches: coinage (Free Play), difficulty, demo sounds, hidden modes");
    if (UnlocksDialog::available(id))
        menu->addAction(theme::icon(mdi::lock_open_variant), "Unlocks…", this, [this, id, title] {
            UnlocksDialog d(id, title, m_running == id, this);
            d.exec();
        });
    if (g.family == "bm3") {   // beatmania III's floppy drive (runtime/dev/fdc.h): where a player's disk goes
        auto *floppy = menu->addMenu(theme::icon(mdi::floppy), "Floppy disk");
        floppy->setToolTipsVisible(true);
        auto *group = new QActionGroup(floppy);
        const QString now = cfg::settings().value("floppy/" + id).toString();
        auto choice = [&](const QString &label, bool checked, const QString &tip, std::function<void()> chosen) {
            auto *a = floppy->addAction(label, this, std::move(chosen));
            a->setCheckable(true);
            a->setChecked(checked);
            a->setToolTip(tip);
            group->addAction(a);
        };
        choice("No disk", now.isEmpty(), "The drive stays empty, as on a cabinet nobody brought a disk to", [this, id] {
            cfg::settings().remove("floppy/" + id);
            gameChanged();
        });
        choice("The game's own disk", now == "default",
               "A disk kept in the game's save folder (floppy.img), made blank and formatted the first time: the game "
               "keeps the player's name, options and scores on it at the end of a credit, and reads them at the next",
               [this, id] {
                   cfg::settings().setValue("floppy/" + id, "default");
                   gameChanged();
               });
        const bool file = !now.isEmpty() && now != "default";
        choice(file ? "A disk image: " + QFileInfo(now).fileName() + "…" : "A disk image…", file,
               "A 1.44 MB disk image of your own (made blank and formatted if the file isn't there). One file can be "
               "given to all five games: a disk went from one beatmania III to the next",
               [this, id, now, file] {
                   const QString f = QFileDialog::getSaveFileName(this, "A 1.44 MB floppy disk image (made if it isn't there)",
                                                                  file ? now : QDir::homePath() + "/beatmania III.img",
                                                                  "Disk images (*.img *.ima *.flp *.dsk);;All files (*)", nullptr,
                                                                  QFileDialog::DontConfirmOverwrite);
                   if (!f.isEmpty()) cfg::settings().setValue("floppy/" + id, f);
                   gameChanged();
               });
        floppy->addSeparator();
        floppy->addAction("In the game, Insert takes the disk out and puts it in (Controls)")->setEnabled(false);
        addCardMedia(this, floppy, id, "floppy disk", "floppy.img", 1474560, "Disk images (*.img *.ima *.flp *.dsk);;All files (*)",
                     !m_running.isEmpty(), [this] { gameChanged(); });
    }
    if (g.memoryCards) {   // the System 573's memory card reader (runtime/sys573/memcard.h): a player's PlayStation card
        auto *cards = menu->addMenu(theme::icon(mdi::floppy), "Memory cards");
        cards->setToolTipsVisible(true);
        for (int slot = 1; slot <= 2; slot++) {
            const QString key = QString("memcard%1/").arg(slot) + id, name = QString("memcard%1.mcr").arg(slot);
            cards->addSection(QString("Slot %1").arg(slot));
            auto *group = new QActionGroup(cards);
            const QString now = cfg::settings().value(key).toString();
            auto choice = [&](const QString &label, bool checked, const QString &tip, std::function<void()> chosen) {
                auto *a = cards->addAction(label, this, std::move(chosen));
                a->setCheckable(true);
                a->setChecked(checked);
                a->setToolTip(tip);
                group->addAction(a);
            };
            choice("No card", now.isEmpty(), "The slot stays empty, as on a cabinet nobody brought a card to", [this, key] {
                cfg::settings().remove(key);
                gameChanged();
            });
            choice("The game's own card", now == "default",
                   "A card kept in the game's save folder (" + name + "), made formatted and empty the first time",
                   [this, key] {
                       cfg::settings().setValue(key, "default");
                       gameChanged();
                   });
            const bool file = !now.isEmpty() && now != "default";
            choice(file ? "A card image: " + QFileInfo(now).fileName() + "…" : "A card image…", file,
                   "A raw PlayStation memory card image of your own (128 KiB, .mcr), which the game reads and writes "
                   "in place: the one a PlayStation emulator plays the home version with (made formatted if the file "
                   "isn't there)",
                   [this, key, now, file] {
                       const QString f = QFileDialog::getSaveFileName(this, "A PlayStation memory card image (made if it isn't there)",
                                                                      file ? now : QDir::homePath() + "/memory card.mcr",
                                                                      "Memory card images (*.mcr *.mcd *.mc *.srm);;All files (*)",
                                                                      nullptr, QFileDialog::DontConfirmOverwrite);
                       if (!f.isEmpty()) cfg::settings().setValue(key, f);
                       gameChanged();
                   });
        }
        cards->addSeparator();
        cards->addAction(g.family == "gtrf" ? "In the game, Home and End take card 1 or 2 out and put it in (Controls)"
                                            : "In the game, Insert and Delete take card 1 or 2 out and put it in (Controls)")
            ->setEnabled(false);
        addCardMedia(this, cards, id, "memory card", "memcard.mcr", 131072,
                     "Memory card images (*.mcr *.mcd *.mc *.srm);;All files (*)", !m_running.isEmpty(), [this] { gameChanged(); });
    }
    if (g.installCassette) {
        auto *inst = menu->addAction("Boot once with the install cassette", this, [this, id](bool on) {
            if (on) cfg::settings().setValue("cassette/" + id, "install");
            else cfg::settings().remove("cassette/" + id);
            gameChanged();
        });
        inst->setCheckable(true);
        inst->setChecked(cfg::settings().value("cassette/" + id).toString() == "install");
        inst->setToolTip("For an install that stopped before it was done (the game then shows ERROR 147)");
    }
    if (hasInstallCd(g)) {
        auto *cd = menu->addAction("Boot once with the install CD", this, [this, id](bool on) {
            if (on) cfg::settings().setValue("cd/" + id, "install");
            else cfg::settings().remove("cd/" + id);
            gameChanged();
        });
        cd->setCheckable(true);
        cd->setChecked(cfg::settings().value("cd/" + id).toString() == "install");
        cd->setToolTip("To install again, or finish an install that stopped before it was done");
    }
    if (hasNetworkUnit(g)) {   // e-AMUSEMENT GuitarFreaks and DrumMania: the network unit (runtime/sys573/npu.h)
        menu->addAction(unitInstalled(g) ? "Install the e-AMUSEMENT network unit again" : "Install the e-AMUSEMENT network unit",
                        this, [this, id] {
                            cfg::settings().setValue("npu/" + id, "install");
                            gameChanged();
                        })
            ->setToolTip("At the next start the game installs the unit's hard disk from its CD (about 15 to 20 minutes of "
                         "its time). When it shows INSTALLATION COMPLETED, stop it: the unit is then fitted");
        auto *fit = menu->addAction("e-AMUSEMENT network unit fitted", this, [this, id](bool on) {
            cfg::settings().setValue("npu/" + id, on ? "on" : "off");
            gameChanged();
        });
        fit->setCheckable(true);
        fit->setEnabled(unitInstalled(g));
        fit->setChecked(unitState(g) == "on");
        fit->setToolTip("e-AMUSEMENT through the cabinet's network unit and its card units (installed first)");
    }
    if (g.loader == "pc64") {   // the 64-bit PC games: on one 4:3 screen (runtime/pc64/crt.c)
        auto *one = menu->addAction("One 4:3 screen, nine buttons", this, [this, id](bool on) {
            if (on) cfg::settings().setValue("singleScreen/" + id, true);
            else cfg::settings().remove("singleScreen/" + id);
            gameChanged();
        });
        one->setCheckable(true);
        one->setChecked(cfg::settings().value("singleScreen/" + id).toBool());
        one->setToolTip("For an older cabinet: the main screen at 640x480, no touch screen or big buttons; "
                        "the save folder's single-screen/ layout when there is one");
        // the cabinet's market (runtime/pc64/main.c): its language and the songs it lists
        auto *market = menu->addMenu("Market");
        market->setToolTip("The country the cabinet was sold for: its texts' language and the songs it lists "
                           "(licensed songs are some countries' only)");
        auto *group = new QActionGroup(market);
        const QString now = cfg::settings().value("market/" + id).toString();
        for (const auto &[code, label] : std::vector<std::pair<QString, QString>>{
                 {"", "As the game's data says"}, {"J", "Japan"}, {"A", "Asia (English)"}, {"K", "Korea"}, {"U", "USA"}}) {
            auto *m = market->addAction(label, this, [this, id, code] {
                if (code.isEmpty()) cfg::settings().remove("market/" + id);
                else cfg::settings().setValue("market/" + id, code);
                gameChanged();
            });
            m->setCheckable(true);
            m->setChecked(code == now);
            group->addAction(m);
        }
    }
    menu->addSeparator();
    QString nv = cfg::nvramDir(id);
    auto *open = menu->addAction(theme::icon(mdi::folder_open), "Open the save folder", this,
                                [nv] { QDesktopServices::openUrl(QUrl::fromLocalFile(nv)); });
    open->setEnabled(QFileInfo::exists(nv));
    auto *reset = menu->addAction(theme::icon(mdi::delete_), "Reset save data…", this, [this, nv, title, id] {
        if (m_running == id) {
            QMessageBox::information(this, title, "Stop the game first.");
            return;
        }
        auto r = QMessageBox::question(this, title,
                                       "Move " + title + "'s save data (operator settings, rankings, sound flash) to the trash?\n\n" + nv +
                                           "\n\nThe next start is a first boot, as on a new cabinet." +
                                           (QFileInfo::exists(nv + "/floppy.img")
                                                ? "\n\nThe game's own floppy disk (floppy.img, a player's name and scores) is in that folder and goes with it."
                                                : "") +
                                           (QFileInfo::exists(nv + "/memcard1.mcr") || QFileInfo::exists(nv + "/memcard2.mcr")
                                                ? "\n\nThe game's own memory cards (memcard1.mcr, memcard2.mcr) are in that folder and go with it."
                                                : ""));
        if (r != QMessageBox::Yes) return;
        if (!QFile::moveToTrash(nv)) QMessageBox::warning(this, title, "Could not move " + nv + " to the trash.");
        gameChanged();
    });
    reset->setEnabled(QFileInfo::exists(nv));
    menu->addSeparator();
    menu->addAction("Arguments for " + title + "…", this, [this, id, title] {
        bool ok = false;
        const QString now = cfg::settings().value("gameArgs/" + id).toString();
        const QString a = QInputDialog::getText(this, title,
                                                "Arguments for " + title + " alone, added after those for every game "
                                                "(Settings > General):",
                                                QLineEdit::Normal, now, &ok);
        if (!ok) return;
        if (a.trimmed().isEmpty()) cfg::settings().remove("gameArgs/" + id);
        else cfg::settings().setValue("gameArgs/" + id, a.trimmed());
        gameChanged();
    })->setToolTip("Command-line options for this game only (docs/PLAY.md lists them)");
    if (g.status == Game::Ready) {
        QStringList args = arguments(g);
        for (QString &a : args)
            if (a.contains(' ')) a = "'" + a + "'";
        QString cmd = g.exe + " " + args.join(' ');
        menu->addAction("Copy the command line", this, [cmd] { QApplication::clipboard()->setText(cmd); });
    }
}

void MainWindow::dragEnterEvent(QDragEnterEvent *e) {
    if (e->mimeData()->hasUrls()) e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *e) {
    QStringList paths, music;
    for (const QUrl &u : e->mimeData()->urls())
        if (u.isLocalFile()) (MusicPlayer::isMidi(u.toLocalFile()) ? music : paths) << u.toLocalFile();
    if (!paths.isEmpty() || !music.isEmpty()) e->acceptProposedAction();
    if (!music.isEmpty()) m_music->playFiles(music);   // MIDI files: played, not added as a game
    if (!paths.isEmpty()) addGameFiles(paths);
}

void MainWindow::addGameFiles(const QStringList &paths) {
    const QString place = askPlace(paths);
    if (place == "cancel") return;
    m_ingestQueue << (place.isEmpty() ? paths : QStringList{"--place", place} + paths);
    if (m_ingest.state() == QProcess::NotRunning) startIngest();
}

// Where a game's files go when it's added (tools/ingest.py --place): moved into the HIRAKU folder's
// dumps/, or read where they are; Settings > General can answer for good. A Konami Python BIOS or
// key files alone (copied: they're small): nothing to ask
QString MainWindow::askPlace(const QStringList &paths) {
    static const QRegularExpression support(
        R"(^(coh-h31000\.\w+|ps2-0190j.*\.bin|(civ|cks|eks|kek|arcade_kelf_override_kbit|arcade_kelf_override_kc)\.bin|bios|keys)$)",
        QRegularExpression::CaseInsensitiveOption);
    bool onlySupport = true;
    for (const QString &p : paths) onlySupport = onlySupport && support.match(QFileInfo(p).fileName()).hasMatch();
    if (onlySupport) return QString();
    const QString kept = cfg::settings().value("ingest/place", "ask").toString();
    if (kept == "move" || kept == "stay") return kept;
    QMessageBox box(QMessageBox::Question, "Add games", "Move the game's files into the HIRAKU folder, or leave them where they are?",
                    QMessageBox::Cancel, this);
    box.setInformativeText(
        "Move: its CHDs, disks and ROM set (a PC game: its folder) go into the HIRAKU folder (dumps/) and are gone "
        "from where they are now. From another drive they are copied, then removed.\n\n"
        "Leave: HIRAKU reads them where they are: keep them there (a USB drive plugged in to play).\n\n"
        "Its ROMs are copied in either way (they are small); a Firebeat game's discs are unpacked into the HIRAKU "
        "folder either way.");
    QPushButton *move = box.addButton("Move into the HIRAKU folder", QMessageBox::AcceptRole);
    QPushButton *stay = box.addButton("Leave where they are", QMessageBox::AcceptRole);
    auto *remember = new QCheckBox("Don't ask again (Settings > General)");
    box.setCheckBox(remember);
    box.setDefaultButton(stay);
    box.exec();
    const QString r = box.clickedButton() == move ? "move" : box.clickedButton() == stay ? "stay" : "cancel";
    if (r != "cancel" && remember->isChecked()) cfg::settings().setValue("ingest/place", r);
    return r;
}

// The game's files listed (tools/checklist.py), in the background; big: the big ones hashed too
void MainWindow::checkFiles(const QString &id, bool big) {
    const QStringList py = cfg::python();
    const QString script = cfg::rootDir() + "/tools/checklist.py";
    if (py.isEmpty() || !QFileInfo::exists(script)) return;
    if (m_files.state() != QProcess::NotRunning) {
        if (m_filesGame == id && (m_filesBig || !big)) return;   // on it already
        m_filesGame.clear();                                     // (its end then does nothing)
        m_files.kill();
        m_files.waitForFinished(2000);
    }
    m_filesGame = id, m_filesBig = big;
    m_files.setWorkingDirectory(cfg::rootDir());
    m_files.start(py[0], py.mid(1) + QStringList{script, id} + (big ? QStringList{"--big"} : QStringList{}));
    if (big) showDetails();   // it says the big files are being read
}

// The details' list of the game's files: each with its mark (MAME's SHA-1 matched, there, another
// dump, missing), its size and MD5
QString MainWindow::filesHtml(const Game &g) {
    const auto it = m_fileLists.constFind(g.id);
    if (it == m_fileLists.constEnd()) {
        checkFiles(g.id, false);
        return "<p><b>Files:</b> checking…</p>";
    }
    if (it->contains("error"))
        return "<p><b>Files:</b> could not be listed (" + (*it)["error"].toString().toHtmlEscaped() + ")</p>";
    const QJsonArray items = (*it)["items"].toArray();
    if (items.isEmpty()) return QString();
    const QLocale loc;
    int ok = 0, bad = 0, missing = 0;
    qint64 unhashed = 0;
    QString rows;
    for (const QJsonValue &v : items) {
        const QJsonObject f = v.toObject();
        const QString st = f["status"].toString(), md5 = f["md5"].toString(), note = f["note"].toString();
        const qint64 size = (qint64)f["size"].toDouble();
        ok += st == "ok", bad += st == "bad", missing += st == "missing";
        const QString mark = st == "ok"        ? "<span style='color:#2e9d4a'>✓</span>"
                             : st == "bad"     ? "<span style='color:#d33'>✗</span>"
                             : st == "missing" ? "<span style='color:#d33'>–</span>"
                                               : "•";
        rows += "<tr><td valign='top'>" + mark + "&nbsp;</td><td>" + f["name"].toString().toHtmlEscaped() + " <small>(" +
                f["kind"].toString().toHtmlEscaped() + (size > 0 ? ", " + loc.formattedDataSize(size) : QString()) + ")</small>";
        if (!md5.isEmpty()) rows += "<br><small><tt>MD5 " + md5 + "</tt></small>";
        if (st == "missing") rows += "<br><small>missing</small>";
        if (st == "bad") rows += "<br><small>not the dump MAME knows (its SHA-1: <tt>" + f["expected"].toString() + "</tt>)</small>";
        if (md5.isEmpty() && size > 0 && st != "missing") unhashed += size;
        if (!note.isEmpty() && note != "big") rows += "<br><small>" + note.toHtmlEscaped() + "</small>";
        rows += "</td></tr>";
    }
    QString sum = QString("%1 file%2").arg(items.size()).arg(items.size() == 1 ? "" : "s");
    if (ok) sum += QString(", %1 the dumps MAME knows").arg(ok);
    if (bad) sum += QString(", <b>%1 another dump</b>").arg(bad);
    if (missing) sum += QString(", <b>%1 missing</b>").arg(missing);
    QString h = "<p><b>Files:</b> " + sum + "<br><small>✓ the dump MAME knows (its SHA-1) · • there, nothing to check it "
                "against · ✗ another dump · – missing</small></p><table cellspacing='0' cellpadding='1'>" + rows + "</table>";
    if (unhashed > 0)
        h += m_files.state() != QProcess::NotRunning && m_filesGame == g.id && m_filesBig
                 ? "<p><small>Reading the big files (" + loc.formattedDataSize(unhashed) + ")…</small></p>"
                 : "<p><a href='hash-big'>Hash the big files too</a> <small>(" + loc.formattedDataSize(unhashed) +
                       ": their MD5 and SHA-1, kept once read)</small></p>";
    return h;
}

void MainWindow::startIngest() {
    if (m_ingestQueue.isEmpty()) return;
    QStringList paths = m_ingestQueue.takeFirst();
    QString script = cfg::rootDir() + "/tools/ingest.py";
    if (!QFileInfo::exists(script)) {
        QMessageBox::warning(this, "HIRAKU", script + " is missing: is the HIRAKU folder set right (Settings)?");
        return;
    }
    m_ingestOutput.clear();
    m_logDock->show();
    log("\n$ tools/ingest.py " + paths.join(' ') + "\n");
    m_ingest.setWorkingDirectory(cfg::rootDir());
    const QStringList py = cfg::python();
    if (py.isEmpty()) {
        QMessageBox::warning(this, "HIRAKU", "Adding games runs tools/ingest.py, which needs Python 3 (python.org).");
        return;
    }
    // --copy-chd: the CHDs of the games that read them in place go into dumps/<game>/ as they are
    // (a hard link where it can be), not extracted: no chdman needed, nothing doubled in size
    m_ingest.start(py[0], py.mid(1) + QStringList{script, "--copy-chd"} + paths);
}

void MainWindow::checkForUpdate(bool asked) {
    m_checkAsked = asked;
    if (!m_check) {
        m_check = new UpdateCheck(this);
        connect(m_check, &UpdateCheck::done, this, [this](bool ok, const QJsonObject &rel, const QString &error) {
            if (!ok) {
                if (m_checkAsked) QMessageBox::warning(this, "HIRAKU", "Could not look for a new version:\n" + error);
                return;
            }
            if (!rel["newer"].toBool()) {
                if (m_checkAsked)
                    QMessageBox::information(this, "HIRAKU", "This is the latest version (" + upd::currentVersion() + ").");
                return;
            }
            m_release = rel;
            m_updateText->setText(QString("<b>%1</b> is out (this is %2).")
                                      .arg(rel["name"].toString().toHtmlEscaped(), upd::currentVersion().toHtmlEscaped()));
            m_updateBar->show();
            if (m_checkAsked) offerUpdate();
        });
    }
    m_check->start();
}

void MainWindow::offerUpdate() {
    if (m_release.isEmpty()) return;
    if (!m_running.isEmpty() || m_ingest.state() != QProcess::NotRunning) {
        QMessageBox::information(this, "HIRAKU", "Close the game, or wait for the game being added, before updating.");
        return;
    }
    m_updateBegun = true;
    UpdateDialog d(m_release, this);
    if (d.exec() != QDialog::Accepted) return;
    QString err;
    if (!upd::startApply(&err)) {
        QMessageBox::warning(this, "HIRAKU", err);
        return;
    }
    close();
}

// A run's log: a file of its own in cfg::logsDir(), the oldest beyond the newest cfg::kRunLogs
// removed, so the last runs' reports can be looked at (and sent) after the launcher closes
void MainWindow::openRunLog(const QString &game) {
    closeRunLog();
    const QString dir = cfg::logsDir();
    if (!QDir().mkpath(dir)) return;
    const QString name = QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss") + "-" + game + ".log";
    QStringList old = runlogs::oldestFirst(dir);   // (as written, whatever the PC's clock has done since: runlogs.h)
    while (old.size() >= cfg::kRunLogs) QFile::remove(dir + "/" + old.takeFirst());
    m_lastRunLog = dir + "/" + name;
    m_runLog = new QFile(m_lastRunLog);
    if (!m_runLog->open(QIODevice::WriteOnly | QIODevice::Text)) {
        delete m_runLog;
        m_runLog = nullptr;
        return;
    }
    // which HIRAKU: the package's version, and this launcher's build (each program prints its own)
    QString version;
    if (QFile v(cfg::rootDir() + "/VERSION"); v.open(QIODevice::ReadOnly)) version = " " + QString::fromUtf8(v.readAll()).trimmed();
    m_runLog->write(("# HIRAKU" + version + " (launcher " + QString::fromUtf8(hk::sys::build_id()) + "), " + game + ", " +
                     QDateTime::currentDateTime().toString(Qt::ISODate) + "\n").toUtf8());
}

void MainWindow::closeRunLog() {
    if (!m_runLog) return;
    m_runLog->close();
    delete m_runLog;
    m_runLog = nullptr;
}

void MainWindow::log(const QString &text) {
    if (!m_running.isEmpty()) {   // the game's output, its last 64 KB, for exitReport
        m_runOutput += text;
        if (m_runOutput.size() > 65536) m_runOutput.remove(0, m_runOutput.size() - 49152);
    }
    if (m_echo) { fputs(text.toLocal8Bit().constData(), stderr); fflush(stderr); }
    if (m_runLog) {
        m_runLog->write(text.toUtf8());
        m_runLog->flush();   // a crash of the launcher keeps what came
    }
    m_log->moveCursor(QTextCursor::End);
    m_log->insertPlainText(text);
    m_log->moveCursor(QTextCursor::End);
}

// File > Convert recordings for sharing: lossless recordings (F6's .mkv) made into H.264 MP4 beside
// them, one after another, by a game's program (--convert-recording: hiraku-menu's, which every
// package has), with Settings' encoder and size; a progress box, Stop ends it (the file half-made
// goes); each result in the log
void MainWindow::convertRecordings() {
    QString exe;
    for (const QString &name : {"hiraku-menu", "hiraku-djmain", "hiraku-sys573", "hiraku-twinkle", "hiraku-firebeat"})
        for (const QString &d : {cfg::binDir(), QCoreApplication::applicationDirPath(), cfg::rootDir() + "/bin", cfg::rootDir() + "/build/cmake"})
            if (const QFileInfo f(d + "/" + cfg::exeName(name)); exe.isEmpty() && f.isFile() && f.isExecutable()) exe = f.absoluteFilePath();
    if (exe.isEmpty()) {
        QMessageBox::warning(this, "Convert recordings", "No program to convert with: hiraku-menu isn't in " + cfg::binDir() + ".");
        return;
    }
    QDir().mkpath(cfg::recordingsDir());
    const QStringList files = recording::firstParts(QFileDialog::getOpenFileNames(
        this, "Convert recordings for sharing (H.264, .mp4)", cfg::recordingsDir(), "Exact recordings (*.mkv)"));
    if (files.isEmpty()) return;
    auto &s = cfg::settings();
    const QStringList opts = recording::arguments("h264", s.value("record/encoder", "auto").toString(), s.value("record/scale", 0).toInt(), true);
    auto *box = new QProgressDialog("Converting…", "Stop", 0, 100 * (int)files.size(), this);
    box->setWindowTitle("Convert recordings");
    box->setWindowModality(Qt::WindowModal);
    box->setMinimumDuration(0);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setAutoClose(false), box->setAutoReset(false);
    auto *proc = new QProcess(box);
    proc->setProcessChannelMode(QProcess::MergedChannels);
    struct State {
        int at = -1;
        bool stopped = false;
        QStringList made, partial, failed;   // (partial: up to where a recording cut short ends)
        QByteArray pending;
    };
    auto st = std::make_shared<State>();
    auto *next = new std::function<void()>;   // (the next file, or the end: the box's, gone with it)
    connect(box, &QObject::destroyed, [next] { delete next; });
    *next = [this, box, proc, st, files, exe, opts] {
        if (++st->at >= files.size() || st->stopped) {
            box->close();
            QString text = st->made.isEmpty() && st->partial.isEmpty() ? QString("No recording was converted.")
                           : st->made.isEmpty() ? QString() : "Made:\n" + st->made.join("\n");
            if (!st->partial.isEmpty())
                text += QString(text.isEmpty() ? "" : "\n\n") +
                        "Converted up to the damage (the recording was cut short or is damaged; the log says how much "
                        "was kept):\n" + st->partial.join("\n");
            if (!st->failed.isEmpty()) text += "\n\nNot converted (the log says why):\n" + st->failed.join("\n");
            QMessageBox::information(this, "Convert recordings", text);
            return;
        }
        const QString in = files[st->at];
        box->setLabelText(QString("Converting %1 (%2 of %3)…").arg(QFileInfo(in).fileName()).arg(st->at + 1).arg(files.size()));
        box->setValue(100 * st->at);
        log("convert: " + in + "\n");
        proc->start(exe, QStringList{"--convert-recording", in} + opts);
    };
    connect(proc, &QProcess::readyRead, box, [this, box, proc, st] {
        st->pending += proc->readAll();
        for (int nl; (nl = (int)st->pending.indexOf('\n')) >= 0;) {
            const QByteArray line = st->pending.left(nl + 1);
            st->pending.remove(0, nl + 1);
            if (const int pc = recording::progressOf(line); pc >= 0) box->setValue(100 * st->at + pc);
            else log(QString::fromUtf8(line));
        }
    });
    connect(proc, &QProcess::finished, box, [files, st, next](int code, QProcess::ExitStatus how) {
        const QString in = files[st->at], out = recording::convertedName(in);
        const recording::Result r = recording::resultOf(how == QProcess::NormalExit, code);
        if (r == recording::Result::Done) st->made << out;
        else if (r == recording::Result::UpToDamage) st->partial << out;
        else {
            if (st->stopped) QFile::remove(out);   // (half made)
            st->failed << in;
        }
        (*next)();
    });
    connect(proc, &QProcess::errorOccurred, box, [files, st, next](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) return;
        st->failed << files[st->at];
        (*next)();
    });
    connect(box, &QProgressDialog::canceled, proc, [proc, st] {
        st->stopped = true;
        proc->kill();
    });
    (*next)();
}

// A game that stopped by itself with an error: why, in words, its last messages, and its log
void MainWindow::exitReport(const QString &title, int code, QProcess::ExitStatus st) {
    // the messages that say what went wrong (an error, an assertion...), else the last ones
    static const QRegularExpression bad("error|assert|fail|fatal|abort|exception|missing|not found|cannot|can't|could not|unable",
                                        QRegularExpression::CaseInsensitiveOption);
    QStringList lines, last;
    for (const QString &l : m_runOutput.split('\n')) {
        const QString t = l.trimmed();
        if (t.isEmpty() || t.startsWith("---")) continue;
        last << t;
        if (bad.match(t).hasMatch()) lines << t;
    }
    if (lines.isEmpty()) lines = last;
    lines = lines.mid(std::max<qsizetype>(0, lines.size() - 6));
    for (QString &l : lines)
        if (l.size() > 200) l = l.left(200) + "…";
    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(title);
    box.setText(QString("%1 stopped: %2.").arg(title, exitReason(code, st)));
    box.setInformativeText((lines.isEmpty() ? QString("It wrote nothing.") : "Its last messages:\n\n" + lines.join('\n')) +
                           "\n\nThe whole log is in the log pane, and in the run's log file (Help > Open the last "
                           "runs' logs): report it with the button below (or Help > Report a problem).");
    QPushButton *open = nullptr, *report = nullptr;
    if (!m_lastRunLog.isEmpty() && QFileInfo::exists(m_lastRunLog)) {
        report = box.addButton("Report this problem…", QMessageBox::ActionRole);
        open = box.addButton("Open the log file", QMessageBox::ActionRole);
    }
    box.addButton(QMessageBox::Close);
    box.exec();
    if (open && box.clickedButton() == open) QDesktopServices::openUrl(QUrl::fromLocalFile(m_lastRunLog));
    if (report && box.clickedButton() == report) ReportDialog(this, m_lastRunLog).exec();
}
