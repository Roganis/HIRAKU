// SPDX-License-Identifier: GPL-3.0-or-later
#include "mainwindow.h"

#include <QAction>
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
#include <QDesktopServices>
#include <QDateTime>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
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

#include "aboutdialog.h"
#include "config.h"
#include "controlsdialog.h"
#include "dipsdialog.h"
#include "lightsdialog.h"
#include "modsdialog.h"
#include "picturedialog.h"
#include "unlocksdialog.h"
#include "settingsdialog.h"
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
static QString nvramSeed(const Game &g);
static bool firstBoot(const Game &g);

MainWindow::MainWindow() {
    setWindowTitle(upd::currentVersion().isEmpty() ? QString("HIRAKU") : "HIRAKU " + upd::currentVersion());
    resize(900, 600);

    // ---- actions ----
    m_play = new QAction(QIcon::fromTheme("media-playback-start"), "Play", this);
    m_play->setShortcut(QKeySequence(Qt::Key_Return));
    m_stop = new QAction(QIcon::fromTheme("media-playback-stop"), "Stop", this);
    m_controls = new QAction(QIcon::fromTheme("input-gaming"), "Controls", this);
    m_lights = new QAction(QIcon::fromTheme("dialog-information", QIcon::fromTheme("weather-clear")), "Lights", this);
    auto *settings = new QAction(QIcon::fromTheme("preferences-system"), "Settings", this);
    auto *pictureAct = new QAction(QIcon::fromTheme("video-display", QIcon::fromTheme("preferences-desktop-display")), "Picture", this);
    pictureAct->setToolTip("Sharp pixels or a CRT (light, full, analog), and the CRT's settings: for all games, or the selected one");
    connect(pictureAct, &QAction::triggered, this, [this] {
        auto *g = selected();
        PictureDialog d(g ? g->id : QString(), g ? g->title : QString(), false, this);
        if (d.exec() == QDialog::Accepted) showDetails();
    });
    auto *addFiles = new QAction(QIcon::fromTheme("list-add"), "Add game files…", this);
    addFiles->setToolTip("Add a game from its files: the MAME ROM set (.zip/.7z), CHDs or disc images,\n"
                         "or a Konami Python game's .py1 (PCSX2 Reliquary's descriptor).\n"
                         "You can also drop them on this window.");
    connect(addFiles, &QAction::triggered, this, [this] {
        QStringList f = QFileDialog::getOpenFileNames(this, "Add game files", QString(),
            "Game files (*.zip *.7z *.chd *.cue *.iso *.img *.bin *.py1);;All files (*)");
        if (!f.isEmpty()) addGameFiles(f);
    });
    auto *addFolder = new QAction(QIcon::fromTheme("folder-new"), "Add a game folder…", this);
    connect(addFolder, &QAction::triggered, this, [this] {
        QString d = QFileDialog::getExistingDirectory(this, "Add a game folder (its ROM set, CHDs, images)");
        if (!d.isEmpty()) addGameFiles({d});
    });
    setAcceptDrops(true);
    auto *refreshAct = new QAction(QIcon::fromTheme("view-refresh"), "Refresh", this);
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
        if (d.exec() == QDialog::Accepted) refresh();
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
    auto *logo = new QToolButton;
    const int logoH = tb->iconSize().height() + tb->fontMetrics().height();
    QPixmap lp = QPixmap(":/logo.png").scaledToHeight(logoH * devicePixelRatio(), Qt::SmoothTransformation);
    lp.setDevicePixelRatio(devicePixelRatio());
    logo->setIcon(QIcon(lp));
    logo->setIconSize(lp.deviceIndependentSize().toSize());
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
    m_filter = new QLineEdit;
    m_filter->setPlaceholderText("Search");
    m_filter->setClearButtonEnabled(true);
    m_filter->setMaximumWidth(240);
    tb->addWidget(m_filter);
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
    auto *details = new QScrollArea;
    details->setWidgetResizable(true);
    details->setFrameShape(QFrame::NoFrame);
    details->setWidget(m_details);
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
        auto *buttons = new QHBoxLayout;
        buttons->addStretch(1);
        auto *bFiles = new QPushButton(QIcon::fromTheme("list-add"), "Add game files…");
        connect(bFiles, &QPushButton::clicked, addFiles, &QAction::trigger);
        auto *bFolder = new QPushButton(QIcon::fromTheme("folder-new"), "Add a game folder…");
        connect(bFolder, &QPushButton::clicked, addFolder, &QAction::trigger);
        auto *bAll = new QPushButton("Show every game HIRAKU runs");
        bAll->setToolTip("With what each one needs (View > Show games not added yet)");
        connect(bAll, &QPushButton::clicked, m_showAll, [this] { m_showAll->setChecked(true); });
        for (auto *b : {bFiles, bFolder, bAll}) buttons->addWidget(b);
        buttons->addStretch(1);
        ev->addLayout(buttons);
        ev->addStretch(2);
    }
    m_listStack = new QStackedWidget;
    m_listStack->addWidget(m_list);
    m_listStack->addWidget(empty);
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

    // ---- menus ----
    auto *file = menuBar()->addMenu("&File");
    file->addAction(m_play);
    file->addAction(m_stop);
    file->addSeparator();
    file->addAction(addFiles);
    file->addAction(addFolder);
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
    view->addAction(m_showAll);
    view->addAction(m_detailsDock->toggleViewAction());
    view->addAction(m_logDock->toggleViewAction());
    view->addAction(refreshAct);
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
    help->addSeparator();
    auto *updAct = help->addAction("Check for updates…", this, [this] { checkForUpdate(true); });
    if (!upd::available()) {
        updAct->setEnabled(false);
        updAct->setToolTip("A release package updates itself; this copy isn't one");
    }
    if (upd::available()) {
        // the last update's download, once its script (which starts this launcher) is done; not
        // once an update of this session has begun: update/ is then the new download
        QTimer::singleShot(5000, this, [this] { if (!m_updateBegun) upd::cleanUp(); });
        if (cfg::settings().value("updates/check", true).toBool())
            QTimer::singleShot(2000, this, [this] { checkForUpdate(false); });
    }

    // ---- the details' list of a game's files (tools/checklist.py) ----
    connect(&m_files, &QProcess::finished, this, [this](int code, QProcess::ExitStatus st) {
        if (m_filesGame.isEmpty()) return;   // stopped for another game's (checkFiles)
        const QJsonObject o = QJsonDocument::fromJson(m_files.readAllStandardOutput()).object();
        if (st == QProcess::NormalExit && code == 0 && !o.isEmpty()) m_fileLists[m_filesGame] = o;
        else m_fileLists[m_filesGame] = QJsonObject{{"error", QString::fromLocal8Bit(m_files.readAllStandardError()).trimmed()}};
        const QString id = m_filesGame;
        m_filesGame.clear();
        if (const Game *g = selected(); g && g->id == id) showDetails();
    });

    // ---- the game process ----
    m_ingest.setProcessChannelMode(QProcess::MergedChannels);
    connect(&m_ingest, &QProcess::readyRead, this, [this] {
        QString t = QString::fromLocal8Bit(m_ingest.readAll());
        m_ingestOutput += t;
        log(t);
    });
    connect(&m_ingest, &QProcess::finished, this, [this](int code, QProcess::ExitStatus st) {
        m_fileLists.clear();   // games' files added or moved: their lists again
        refresh();
        QStringList added, errors, kept;
        for (const QString &l : m_ingestOutput.split('\n')) {
            // dumps/<id>/machine.txt (Windows: dumps\<id>\machine.txt)
            if (l.startsWith("wrote") && l.contains("machine.txt")) added << QString(l).replace('\\', '/').section('/', -2, -2);
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
        bool ok = m_stopping || (st == QProcess::NormalExit && (code == 0 || code == 130));
        QString msg = st == QProcess::CrashExit ? title + " crashed" : QString("%1 exited (code %2)").arg(title).arg(code);
        log("\n--- " + msg + " ---\n");
        closeRunLog();
        statusBar()->showMessage(ok ? title + " stopped" : msg);
        if (!ok) m_logDock->show();
        m_running.clear();
        m_stopping = false;
        if (isHidden()) show();
        updateActions();
        gameChanged();   // its saves now exist: no longer a first boot
        if (!ok) exitReport(title, code, st);
    });
    connect(&m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) return;
        log("\n--- could not start " + m_proc.program() + ": " + m_proc.errorString() + " ---\n");
        closeRunLog();
        QMessageBox::warning(this, "HIRAKU", "Could not start " + m_proc.program() + ":\n" + m_proc.errorString());
        m_running.clear();
        if (isHidden()) show();
        updateActions();
    });

    restoreGeometry(cfg::settings().value("ui/geometry").toByteArray());
    // no layout of this version saved: the panel beside the list starts at about a third of the window
    if (!restoreState(cfg::settings().value("ui/state").toByteArray(), kLayout))
        QTimer::singleShot(0, this, [this] { resizeDocks({m_detailsDock}, {std::max(300, width() / 3)}, Qt::Horizontal); });
    qApp->installEventFilter(this);
    refresh();
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
    refresh();
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
    if (kept && shown(kept)) {
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
}

void MainWindow::showEmpty() {
    bool any = false;
    for (QTreeWidgetItem *it : gameItems()) any = any || !it->isHidden();
    const bool empty = !any && !m_games.isEmpty() && m_filter->text().trimmed().isEmpty() && !m_showAll->isChecked();
    m_listStack->setCurrentIndex(empty ? 1 : 0);
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
    else if (firstBoot(g)) notes << "First boot", *notable = true;
    if (const int n = cfg::modsOn(g.id).size()) notes << (n == 1 ? QString("1 mod") : QString("%1 mods").arg(n));
    if (!cfg::settings().value("gameArgs/" + g.id).toString().isEmpty()) notes << "own arguments";
    if (cfg::settings().contains("dips/" + g.id)) notes << "DIP switches set";
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
            it->setForeground(Status, notable ? QColor(0xc0, 0x80, 0x10) : QColor(0x2e, 0x9e, 0x4a));
    }
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
}

static bool installCassette(const Game &g);
static bool hasInstallCd(const Game &g);
static bool installCd(const Game &g);
static QString nvramSeed(const Game &g);

// a folder and everything in it (a game's saves, for a mod set's first start)
static void copyTree(const QString &from, const QString &to) {
    QDir().mkpath(to);
    for (const QFileInfo &f : QDir(from).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden)) {
        if (f.isDir()) copyTree(f.filePath(), to + "/" + f.fileName());
        else QFile::copy(f.filePath(), to + "/" + f.fileName());
    }
}

void MainWindow::showDetails() {
    const Game *g = selected();
    if (!g) {
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
    QString h = "<b>" + g->title.toHtmlEscaped() + "</b>";
    if (!g->maker.isEmpty()) h += " · " + g->maker.toHtmlEscaped() + " " + g->year;
    h += " · " + g->statusText();
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
    a << "--audio-ms" << QString::number(s.value("audio/ms", 8).toInt());
    if (s.value("audio/fixed", false).toBool()) a << "--audio-fixed";
    if (!s.value("system/priority", true).toBool()) a << "--normal-priority";
    if (int v = s.value("audio/volumeDb", 0).toInt(); v) a << "--volume" << QString::number(v);
    if (s.value("video/splitScreens", false).toBool() && g.family == "kbm") a << "--split-screens";
    if (QString d = s.value("audio/device").toString(); d.startsWith("asio:")) a << "--audio-asio" << d.mid(5);   // Windows
    else {
        if (!d.isEmpty()) a << "--audio-device" << d;
#ifdef _WIN32
        if (s.value("audio/exclusive", false).toBool()) a << "--audio-exclusive";
#endif
    }
    if (int f = s.value("audio/buffer", 0).toInt(); f > 0) a << "--audio-buffer" << QString::number(f);
    // every millisecond is the cores' default (2026-10-20); the key is new, so that the old one's
    // stored "off" (saved by any OK in Settings) doesn't keep it off
    if ((g.core == "sys573" || g.core == "twinkle") && !s.value("audio/soundEveryMs", true).toBool()) a << "--mame-sound";
    QString controls = cfg::controlsFile(g.family), lights = cfg::lightsFile(g.family);
    if (QFileInfo::exists(controls)) a << "--bindings" << controls;
    if (lightsFileUsed(lights)) a << "--lights" << lights;
    if (s.value("lights/socket", true).toBool()) a << "--lights-socket" << cfg::lightsSocket();
    if (s.value("lights/overlay", false).toBool()) a << "--lights-overlay";   // runtime/host/lampview.h
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
    if (g.core == "pc") {   // pop'n music 15-18's presses (runtime/pc/press.c)
        if (s.value("game/pressTime", false).toBool()) a << "--press-time";
        if (int ms = s.value("game/judgeOffset", 0).toInt(); ms) a << "--judge-offset" << QString::number(ms);
    }
    if (g.core == "python" || g.core == "pc" || g.family == "drmn" || g.family == "gtrf") {   // the e-AMUSEMENT card readers (runtime/host/readers.h)
        const QString r0 = s.value("readers/serial0").toString(), r1 = s.value("readers/serial1").toString();
        if (!r0.isEmpty() || !r1.isEmpty()) a << "--reader" << r0;   // "": player 2's only
        if (!r1.isEmpty()) a << "--reader" << r1;
        if (s.value("readers/scard", false).toBool()) a << "--scard";
        if (s.value("readers/cardio", false).toBool()) a << "--cardio";
    }
    if (QString b = s.value("cabinet/bio2").toString(); !b.isEmpty() && cfg::controlsSet(g.family) == "beatmania")
        a << "--bio2" << b;   // the IIDX cabinet's controls and lamps (runtime/host/bio2.h)
    if (QString b = s.value("cabinet/popnio").toString(); !b.isEmpty() && cfg::controlsSet(g.family) == "popn") {
        a << "--popn-io" << b;   // the pop'n cabinet's buttons and lamps (runtime/host/popnio.h)
        if (QString f = s.value("cabinet/popnioDll").toString(); !f.isEmpty()) a << "--popn-io-dll" << f;
    }
    // mods (docs/MODS.md): each folder in order, and the saves of their own (cfg::saveDir)
    const QStringList mods = cfg::modsOn(g.id);
    for (const QString &m : mods) a << "--mod" << cfg::modPath(g.id, m);
    if (!mods.isEmpty()) a << "--nvram" << cfg::saveDir(g.id);
    else if (!cfg::nvramBase().isEmpty()) a << "--nvram" << cfg::nvramDir(g.id);
    if (installCassette(g)) a << "--cassette" << "install";
    if (installCd(g)) a << "--cd" << "install";
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

void MainWindow::play(const QString &id) {
    const Game *g = find(id);
    if (!g) {
        statusBar()->showMessage("No game " + id);
        return;
    }
    if (!m_running.isEmpty()) return;
    if (g->status != Game::Ready) {
        QMessageBox box(this);
        box.setWindowTitle(g->title);
        box.setIcon(QMessageBox::Information);
        box.setText(g->title + " is not ready to play.");
        box.setInformativeText(g->howTo());
        box.exec();
        return;
    }
    m_log->clear();
    m_runOutput.clear();
    openRunLog(g->id);
    // the first start of a game with saves to start from: they become its saves
    if (QString seed = nvramSeed(*g); !seed.isEmpty() && !QFileInfo::exists(cfg::nvramDir(g->id))) {
        QDir().mkpath(cfg::nvramDir(g->id));
        for (const QFileInfo &f : QDir(seed).entryInfoList(QDir::Files))
            QFile::copy(f.filePath(), cfg::nvramDir(g->id) + "/" + f.fileName());
        log("saves: started from " + seed + "\n");
    }
    // the first start with these mods: their saves start as a copy of the clean game's
    if (const QString mod_saves = cfg::saveDir(g->id); mod_saves != cfg::nvramDir(g->id) && !QFileInfo::exists(mod_saves)) {
        QDir().mkpath(mod_saves);
        if (QFileInfo::exists(cfg::nvramDir(g->id))) {
            copyTree(cfg::nvramDir(g->id), mod_saves);
            log("saves: with mods, started from a copy of " + cfg::nvramDir(g->id) + "\n");
        }
    }
    QStringList args = arguments(*g);
    log("$ " + g->exe + " " + args.join(' ') + "\n");
    m_running = g->id;
    m_stopping = false;
    m_proc.setWorkingDirectory(cfg::rootDir());
    {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("HIRAKU_ROOT", cfg::rootDir());   // the games use the launcher's root
        env.insert("HK_LAUNCHER", "1");   // the launcher says when a new release is out, not the game (host/notice.h)
        m_proc.setProcessEnvironment(env);
    }
    m_proc.start(g->exe, args);
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
    QTimer::singleShot(5000, this, [this] {
        if (m_proc.state() != QProcess::NotRunning) m_proc.kill();
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
    menu->addAction(QIcon::fromTheme("input-gaming"), "Controls for " + cfg::controlsSetName(cfg::controlsSet(family)) + "…", this, [this, family] {
        controls(family);
    });
    menu->addAction("Lights for " + cfg::familyName(family) + "…", this, [this, family] {
        LightsDialog d(family, this);
        d.exec();
    });
    menu->addAction(QIcon::fromTheme("video-display"), "Picture for " + title + "…", this, [this, id, title] {
        PictureDialog d(id, title, true, this);
        if (d.exec() == QDialog::Accepted) showDetails();
    });
    if (DipsDialog::available(id))
        menu->addAction("DIP switches…", this, [this, id, title] {
            DipsDialog d(id, title, this);
            if (d.exec() == QDialog::Accepted) gameChanged();
        })->setToolTip("The board's switches: coinage (Free Play), difficulty, demo sounds, hidden modes");
    if (UnlocksDialog::available(id))
        menu->addAction(QIcon::fromTheme("unlock", QIcon::fromTheme("object-unlocked")), "Unlocks…", this, [this, id, title] {
            UnlocksDialog d(id, title, m_running == id, this);
            d.exec();
        });
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
    }
    menu->addSeparator();
    QString nv = cfg::nvramDir(id);
    auto *open = menu->addAction(QIcon::fromTheme("folder-open"), "Open the save folder", this,
                                [nv] { QDesktopServices::openUrl(QUrl::fromLocalFile(nv)); });
    open->setEnabled(QFileInfo::exists(nv));
    auto *reset = menu->addAction(QIcon::fromTheme("edit-delete"), "Reset save data…", this, [this, nv, title, id] {
        if (m_running == id) {
            QMessageBox::information(this, title, "Stop the game first.");
            return;
        }
        auto r = QMessageBox::question(this, title,
                                       "Move " + title + "'s save data (operator settings, rankings, sound flash) to the trash?\n\n" + nv +
                                           "\n\nThe next start is a first boot, as on a new cabinet.");
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
    QStringList paths;
    for (const QUrl &u : e->mimeData()->urls())
        if (u.isLocalFile()) paths << u.toLocalFile();
    if (!paths.isEmpty()) {
        e->acceptProposedAction();
        addGameFiles(paths);
    }
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
    if (!d.startInstall(&err)) {
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
    QStringList old = QDir(dir).entryList({"*.log"}, QDir::Files, QDir::Name);   // oldest first (by name: the time)
    while (old.size() >= cfg::kRunLogs) QFile::remove(dir + "/" + old.takeFirst());
    m_lastRunLog = dir + "/" + name;
    m_runLog = new QFile(m_lastRunLog);
    if (!m_runLog->open(QIODevice::WriteOnly | QIODevice::Text)) {
        delete m_runLog;
        m_runLog = nullptr;
        return;
    }
    m_runLog->write(("# HIRAKU, " + game + ", " + QDateTime::currentDateTime().toString(Qt::ISODate) + "\n").toUtf8());
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
                           "runs' logs): send that file when you report the problem.");
    QPushButton *open = nullptr;
    if (!m_lastRunLog.isEmpty() && QFileInfo::exists(m_lastRunLog))
        open = box.addButton("Open the log file", QMessageBox::ActionRole);
    box.addButton(QMessageBox::Close);
    box.exec();
    if (open && box.clickedButton() == open) QDesktopServices::openUrl(QUrl::fromLocalFile(m_lastRunLog));
}
