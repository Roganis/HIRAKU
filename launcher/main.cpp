// SPDX-License-Identifier: GPL-3.0-or-later
// hiraku: the launcher. Lists the supported games, maps controls and lights,
// and starts hiraku-<game>.
//
//   hiraku [GAME]            GAME (e.g. popn7) starts that game right away
//   hiraku --add FILE...     add a game from its files (ROM set, CHDs, images, folders),
//                            as dropping them on the window does; the log goes to stderr too
//   hiraku --updated VERSION | --update-failed WHY   (tools/update.py, restarting it after an update)
//   hiraku --show WHAT [--shot FILE]   open a window (settings, controls, lights, picture, unlocks,
//                            about, or main) over the launcher; --shot saves a picture of it and quits
//                            (checking how the windows fit a small screen: launcher/fit.h)
#include <QApplication>
#include <QIcon>
#include <QMessageBox>
#include <QTimer>

#include "aboutdialog.h"
#include "controlsdialog.h"
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

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    restore_environment();
    python_utf8();
    QApplication::setApplicationName("hiraku");
    QApplication::setApplicationDisplayName("HIRAKU");
    QApplication::setDesktopFileName("hiraku");
    QApplication::setWindowIcon(QIcon(":/icon.png"));
    cfg::mergeControls();
    app.installEventFilter(new ScreenFit(&app));   // every window inside a small screen too (fit.h)
    MainWindow w;
    w.show();
    QStringList args = app.arguments().mid(1);
    if (args.size() >= 2 && args.first() == "--show") {
        QWidget *win = &w;
        const QString what = args[1];
        if (what == "settings") win = new SettingsDialog(&w);
        else if (what == "controls") win = new ControlsDialog("iidx", &w);
        else if (what == "lights") win = new LightsDialog("iidx", &w);
        else if (what == "picture") win = new PictureDialog("bmiidx", "beatmania IIDX", true, &w);
        else if (what == "unlocks") win = new UnlocksDialog("popn12", "pop'n music 12 Iroha", false, &w);
        else if (what == "mods") win = new ModsDialog("popn14", "pop'n music 14 FEVER!", &w);
        else if (what == "dips") win = new DipsDialog("popn1j", "pop'n music", &w);
        else if (what == "about") win = new AboutDialog(&w);
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
        QTimer::singleShot(0, &w, [&w, v = args[1]] { QMessageBox::information(&w, "HIRAKU", "HIRAKU is now " + v + "."); });
    } else if (args.size() >= 2 && args.first() == "--update-failed") {
        QTimer::singleShot(0, &w, [&w, why = args[1]] {
            QMessageBox::warning(&w, "HIRAKU", "The update could not be put in; this is still the version you had.\n\n" + why);
        });
    } else if (!args.isEmpty()) {
        QTimer::singleShot(0, &w, [&w, id = args.first()] { w.play(id); });
    }
    return app.exec();
}
