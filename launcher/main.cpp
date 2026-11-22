// SPDX-License-Identifier: GPL-3.0-or-later
// hiraku: the launcher. Lists the supported games, maps controls and lights,
// and starts hiraku-<game>.
//
//   hiraku [GAME]            GAME (e.g. popn7) starts that game right away
//   hiraku --add FILE...     add a game from its files (ROM set, CHDs, images, folders),
//                            as dropping them on the window does; the log goes to stderr too
//   hiraku --updated VERSION | --update-failed WHY   (tools/update.py, restarting it after an update)
//   hiraku --show WHAT [--shot FILE]   open a window (settings, controls, lights, picture, unlocks,
//                            about, capture, or main) over the launcher; --shot saves a picture of it and quits
//                            (HK_LANG=fr: in that language)
//                            (checking how the windows fit a small screen: launcher/fit.h)
#include <QApplication>
#include <QIcon>
#include <QMessageBox>
#include <QTimer>

#include "aboutdialog.h"
#include "capturedialog.h"
#include "controlsdialog.h"
#include "language.h"
#include "lightsdialog.h"
#include "picturedialog.h"
#include "settingsdialog.h"
#include "unlocksdialog.h"

#include <cstdlib>
#include <string>

#include "config.h"
#include "fit.h"
#include "mainwindow.h"
#include "nvidia.h"
#include "dipsdialog.h"
#include "modsdialog.h"
#include "modsettingsdialog.h"
#include "theme.h"
#include "updater.h"

#ifdef Q_OS_WIN
#include <cstdio>
#define NOMINMAX
#include <windows.h>   // (last: its macros mustn't reach the launcher's headers)
#endif

// A package with its own Qt (tools/package.sh) starts the launcher through bin/hiraku, a script
// that points LD_LIBRARY_PATH at bin/qt/lib and clears QT_PLUGIN_PATH, keeping the values it found
// in HK_SAVED_<name> (absent: it was unset). The loader read LD_LIBRARY_PATH when this process
// started, and Qt its plugin path when the application was made, so both can now go back as they
// were: the games, the tools and whatever opens a folder start with the user's own environment,
// not the bundled Qt.
// (Linux only: Windows finds a program's DLLs beside it.)
static void restore_environment() {
#ifndef Q_OS_WIN
    if (!getenv("HK_SAVED_ENV")) return;
    for (const char *name : {"LD_LIBRARY_PATH", "QT_PLUGIN_PATH"}) {
        const std::string saved = std::string("HK_SAVED_") + name;
        if (const char *v = getenv(saved.c_str())) setenv(name, v, 1);
        else unsetenv(name);
        unsetenv(saved.c_str());
    }
    unsetenv("HK_SAVED_ENV");
#endif
}

// The tools (Python) write UTF-8 to the launcher, which reads them so. On Windows Python writes a
// pipe in the PC's code page otherwise (cp1252 on a western PC), and a tool printing Japanese (an
// unlock's title) stopped at its first character with nothing printed.
static void python_utf8() {
#ifdef Q_OS_WIN
    qputenv("PYTHONUTF8", "1");
    qputenv("PYTHONIOENCODING", "utf-8");
#endif
}

// On Windows the launcher is a window program: no console window opens with it (CMakeLists.txt,
// WIN32_EXECUTABLE). Started from a console (hiraku --add FILE, --shot), what it prints still goes
// to that console, unless its output was sent to a file or a pipe, which it then keeps.
static void use_parent_console() {
#ifdef Q_OS_WIN
    auto sent_elsewhere = [](DWORD which) {
        const HANDLE h = GetStdHandle(which);
        const DWORD type = h && h != INVALID_HANDLE_VALUE ? GetFileType(h) : FILE_TYPE_UNKNOWN;
        return type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE;
    };
    const bool out = sent_elsewhere(STD_OUTPUT_HANDLE), err = sent_elsewhere(STD_ERROR_HANDLE);
    if ((out && err) || !AttachConsole(ATTACH_PARENT_PROCESS)) return;
    if (!out) freopen("CONOUT$", "w", stdout);
    if (!err) freopen("CONOUT$", "w", stderr);
#endif
}

int main(int argc, char **argv) {
    use_parent_console();
    QApplication app(argc, argv);
    restore_environment();
    python_utf8();
    QApplication::setApplicationName("hiraku");
    QApplication::setApplicationDisplayName("HIRAKU");
    QApplication::setDesktopFileName("hiraku");
    QApplication::setWindowIcon(QIcon(":/icon.png"));
    cfg::mergeControls();
    // the interface's language: HK_LANG, else the saved choice (ui/language: "" follows the system)
    language::install(app, language::choose(cfg::settings().value("ui/language").toString(), cfg::rootDir()), cfg::rootDir());
    language::useFamily(language::current());   // (Japanese, Korean, Chinese: the language's own font first)
    theme::applySaved();   // the system's look, or Pop (theme.h)
    app.installEventFilter(new ScreenFit(&app));   // every window inside a small screen too (fit.h)
    QStringList args = app.arguments().mid(1);
    // An update's script starts the launcher itself when it's done (--updated, --update-failed).
    // Started any other way while it works, the launcher would be in its way; and one that finds an
    // update cut short hands over to the script, which starts it again (once: see upd::resume)
    const bool fromUpdate = !args.isEmpty() && (args.first() == "--updated" || args.first() == "--update-failed");
    if (!fromUpdate && upd::applying()) {
        QMessageBox::information(nullptr, "HIRAKU", language::tr("Updater", "An update is being put in. HIRAKU starts by itself when it's done."));
        return 0;
    }
    if (!fromUpdate && upd::cutShort() && upd::resume(true, nullptr)) return 0;
    MainWindow w;
    w.show();
    if (upd::cutShort() && (args.isEmpty() || fromUpdate))   // still there: the script couldn't put it right
        QTimer::singleShot(0, &w, [&w, why = args.value(1)] {
            if (upd::offerResume(&w, why)) w.close();
        });
    if (args.size() >= 2 && args.first() == "--show") {
        QWidget *win = &w;
        const QString what = args[1];
        if (what == "settings") win = new SettingsDialog(&w);
        else if (what == "controls") win = new ControlsDialog("iidx", &w);
        else if (what == "lights") win = new LightsDialog("iidx", &w);
        else if (what == "picture") win = new PictureDialog("bmiidx", "beatmania IIDX", true, &w);
        else if (what == "unlocks") win = new UnlocksDialog("popn12", "pop'n music 12 Iroha", false, &w);
        else if (what == "mods") win = new ModsDialog("popn14", "pop'n music 14 FEVER!", &w);
        else if (what == "mod-settings") {   // the first of popn14's mods (the player's) that has settings
            for (const QString &f : QDir(cfg::modsDir("popn14")).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
                ModSettings m;
                if (m.load(cfg::modPath("popn14", f), cfg::modSettingsFile("popn14", f)) && !m.empty()) {
                    win = new ModSettingsDialog("popn14", f, &w);
                    break;
                }
            }
        }
        else if (what == "dips") win = new DipsDialog("popn1j", "pop'n music", &w);
        else if (what == "about") win = new AboutDialog(&w);
        else if (what == "capture") win = new CaptureDialog("P1 Turntable", false, &w);   // (one binding, its 6 s)
        else if (what == "capture-all") win = new CaptureDialog("P1 Button 3", false, &w, "3 of 16", true);   // (Bind all's walk)
        else if (what == "nvidia") {   // the PC games' NVIDIA notice, with a sample value
            nv::PresentMethod sample;
            sample.nvidia = true, sample.value = 2, sample.from = "the global profile";
            win = nv::noticeBox(&w, "D:\\HIRAKU\\bin\\hiraku-pc.exe", sample);
        }
        if (win != &w) win->show();
        if (args.size() >= 4 && args[2] == "--shot")
            QTimer::singleShot(2000, &w, [win, file = args[3]] {
                const bool ok = win->grab().save(file);
                fprintf(stderr, "--shot: %s %dx%d -> %s%s\n", win->metaObject()->className(), win->width(), win->height(),
                        file.toLocal8Bit().constData(), ok ? "" : " (not saved)");
                QApplication::quit();
            });
    } else if (!args.isEmpty() && args.first() == "--add") {
        w.setEcho(true);
        QTimer::singleShot(0, &w, [&w, files = args.mid(1)] { w.addGameFiles(files); });
    } else if (args.size() >= 2 && args.first() == "--updated") {   // tools/update.py, once the new files are in
        QTimer::singleShot(0, &w, [&w, v = args[1]] { QMessageBox::information(&w, "HIRAKU", language::tr("Updater", "HIRAKU is now %1.").arg(v)); });   // TR: %1 is the version, "0.15.0"
    } else if (args.size() >= 2 && args.first() == "--update-failed") {
        if (!upd::cutShort())   // (cut short: said above)
            QTimer::singleShot(0, &w, [&w, why = args[1]] {
                // TR: %1 is why, in English (the update script's words)
                QMessageBox::warning(&w, "HIRAKU", language::tr("Updater", "The update could not be put in; this is still the version you had.\n\n%1").arg(why));
            });
    } else if (!args.isEmpty() && args.first() == "--menu") {   // a cabinet's shortcut: start in the game menu
        QTimer::singleShot(0, &w, [&w] { w.openMenu(w.lastGame()); });
    } else if (!args.isEmpty()) {
        QTimer::singleShot(0, &w, [&w, id = args.first()] { w.play(id); });
    }
    return app.exec();
}
