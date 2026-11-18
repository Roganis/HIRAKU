// SPDX-License-Identifier: GPL-3.0-or-later
// The controls sets (launcher/config.h): the beatmania cabinets (DJ Main, IIDX, III) share one
// bindings file until the player sets them apart; then each has its own, started as a copy of the
// shared one (and its controllers' layouts), and the shared one is left as it was for when they
// share again. The old per-family files are joined into the shared one only in a folder from before
// the shared set: not while the shared one is there, nor once the three have been set apart. And
// which games fit one cabinet (the game menu's list): the beatmania three always do.
// The Controls dialog's tick box for it (cfg::ControlsEdits): ticked or unticked it writes nothing,
// and the sets changed before it aren't saved by it; changes before ticking carry into the three's
// copies; the dialog's Save writes the sets changed and then the setting, its Cancel nothing.
//   test_controls_sets DIR   (DIR is made a portable install's folder, on Linux and Windows alike:
//                             the launcher's settings go to DIR/user/config, never the player's)
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>

#include <cstdio>

#include "config.h"

static int g_failed = 0;
#define CHECK(x) \
    do { \
        if (!(x)) g_failed++, fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
    } while (0)

static QByteArray read(const QString &path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}
static void write(const QString &path, const QByteArray &text) {
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) f.write(text);
}

// every file under `dir` (the settings too), by name: what a cancelled dialog must leave as it was
static QMap<QString, QByteArray> files(const QString &dir) {
    cfg::settings().sync();
    QMap<QString, QByteArray> all;
    for (QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories); it.hasNext();) {
        const QString f = it.next();
        all[f] = read(f);
    }
    return all;
}

// a file the dialog wrote, its lines (written as text: CR LF on Windows)
static QByteArray plain(const QString &path) { return read(path).replace("\r\n", "\n"); }

// a set as the dialog reads its file and writes it (the dialog's own: launcher/controlsdialog.cpp)
using Bindings = cfg::ControlsEdits::Bindings;
static Bindings readSet(const QString &set) {
    Bindings b;
    for (const QByteArray &line : read(cfg::controlsSetFile(set)).split('\n'))
        if (const int eq = line.indexOf('='); eq > 0)
            b[QString(line.left(eq)).trimmed()] = QString(line.mid(eq + 1)).trimmed().split(", ", Qt::SkipEmptyParts);
    return b;
}
static QByteArray setText(const QString &set, const Bindings &b) {
    QByteArray t = "# " + set.toUtf8() + "\n";
    for (auto it = b.cbegin(); it != b.cend(); ++it) t += it.key().toUtf8() + " = " + it->join(", ").toUtf8() + "\n";
    return t;
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    const QString root = QDir(QString::fromLocal8Bit(argv[1])).absolutePath();
    QDir(root).removeRecursively();
    // a portable install's folder (launcher/config.cpp, portableRoot): games/ makes it HIRAKU's
    // root, user/ makes it portable, and its settings are then in user/config
    QDir().mkpath(root + "/games");
    QDir().mkpath(root + "/user");
    qputenv("HIRAKU_ROOT", root.toLocal8Bit());
    QCoreApplication app(argc, argv);
    // before anything is written: the settings folder must be this test's own, never a player's
    // (a portable install around the test's program would take precedence)
    if (!cfg::portable() || !QDir::cleanPath(cfg::configDir()).startsWith(root + "/")) {
        fprintf(stderr, "FAIL: the settings folder %s isn't under %s: not run, so as not to touch it\n",
                qPrintable(cfg::configDir()), qPrintable(root));
        return 1;
    }
    const QString dir = cfg::configDir() + "/controls";

    // shared: the three are one set, in one file
    CHECK(!cfg::beatmaniaApart());
    for (const char *f : {"djbm", "iidx", "bm3"}) {
        CHECK(cfg::isBeatmania(f));
        CHECK(cfg::controlsSet(f) == "beatmania");
        CHECK(cfg::controlsFile(f) == dir + "/beatmania.txt");
    }
    CHECK(!cfg::isBeatmania("popn") && cfg::controlsSet("pypopn") == "popn");
    CHECK(cfg::sameCabinet("iidx", "bm3") && cfg::sameCabinet("djpopn", "pypopn") && !cfg::sameCabinet("iidx", "popn"));
    CHECK(cfg::sameCabinet("ddr", "ddr") && !cfg::sameCabinet("ddr", "drmn"));
    CHECK(cfg::controlsSeries()[0].second.contains("beatmania") && !cfg::controlsSeries()[0].second.contains("iidx"));

    // apart: each its own file, started from the shared one, its layouts too
    write(dir + "/beatmania.txt", "P1 Button 1 = key:Z\n");
    write(dir + "/layouts/beatmania/pad.txt", "P1 Button 1 = button0\n");
    write(dir + "/bm3.txt", "P1 Button 1 = key:Q\n");   // (one from an earlier time apart: kept)
    cfg::setBeatmaniaApart(true);
    CHECK(cfg::beatmaniaApart());
    CHECK(cfg::controlsSet("iidx") == "iidx" && cfg::controlsSet("djbm") == "djbm" && cfg::controlsSet("bm3") == "bm3");
    CHECK(cfg::controlsFile("iidx") == dir + "/iidx.txt");
    CHECK(read(dir + "/iidx.txt") == "P1 Button 1 = key:Z\n" && read(dir + "/djbm.txt") == "P1 Button 1 = key:Z\n");
    CHECK(read(dir + "/bm3.txt") == "P1 Button 1 = key:Q\n");
    CHECK(read(dir + "/layouts/iidx/pad.txt") == "P1 Button 1 = button0\n");
    CHECK(cfg::controlsSetMembers("iidx") == QStringList{"iidx"});
    const QStringList sets = cfg::controlsSeries()[0].second;
    CHECK(!sets.contains("beatmania") && sets.contains("djbm") && sets.contains("iidx") && sets.contains("bm3") && sets.contains("popn"));
    CHECK(cfg::controlsSetName("bm3") == "beatmania III");
    // the game menu's games that fit: still the three together, their sets apart or not
    CHECK(cfg::sameCabinet("iidx", "bm3") && cfg::sameCabinet("djbm", "iidx") && !cfg::sameCabinet("bm3", "popn"));

    // the start-up's joining leaves them as they are, apart ...
    write(dir + "/iidx.txt", "P1 Button 1 = key:X\n");
    cfg::mergeControls();
    CHECK(read(dir + "/iidx.txt") == "P1 Button 1 = key:X\n" && QFile::exists(dir + "/djbm.txt"));
    CHECK(read(dir + "/beatmania.txt") == "P1 Button 1 = key:Z\n");

    // ... and together again: the shared file as it was, the others kept for the next time
    cfg::setBeatmaniaApart(false);
    CHECK(cfg::controlsFile("iidx") == dir + "/beatmania.txt");
    cfg::mergeControls();
    CHECK(read(dir + "/beatmania.txt") == "P1 Button 1 = key:Z\n");
    CHECK(read(dir + "/iidx.txt") == "P1 Button 1 = key:X\n");
    cfg::setBeatmaniaApart(true);
    CHECK(read(dir + "/iidx.txt") == "P1 Button 1 = key:X\n");   // (not copied over)
    cfg::setBeatmaniaApart(false);

    // together again with no shared file yet (never saved): the three are still that choice's,
    // not joined into one and put away as .old
    QFile::remove(dir + "/beatmania.txt");
    cfg::mergeControls();
    CHECK(!QFile::exists(dir + "/beatmania.txt"));
    CHECK(read(dir + "/iidx.txt") == "P1 Button 1 = key:X\n" && !QFile::exists(dir + "/iidx.txt.old"));
    CHECK(QFile::exists(dir + "/djbm.txt") && QFile::exists(dir + "/bm3.txt"));

    // an older launcher's folder (from before the shared set: the setting never written), its files
    // one a family and no shared one: joined into it, as before
    cfg::settings().remove("controls/beatmaniaApart");
    QFile::remove(dir + "/djbm.txt");
    QFile::remove(dir + "/bm3.txt");
    cfg::mergeControls();
    CHECK(read(dir + "/beatmania.txt").contains("P1 Button 1 = key:X"));
    CHECK(!QFile::exists(dir + "/iidx.txt") && QFile::exists(dir + "/iidx.txt.old"));

    // The Controls dialog's tick box. A folder as a player's: the shared file, pop'n music's, a layout
    QDir(dir).removeRecursively();
    cfg::settings().remove("controls/beatmaniaApart");
    write(dir + "/beatmania.txt", "P1 Button 1 = key:Z\n");
    write(dir + "/popn.txt", "P1 Button 1 = key:A\n");
    write(dir + "/layouts/beatmania/pad.txt", "P1 Button 1 = button0\n");
    QMap<QString, QByteArray> before = files(cfg::configDir());
    QString err;

    // ticked, with a pop'n music set changed before, then Cancel: nothing written, the setting not set
    {
        cfg::ControlsEdits e;
        CHECK(!e.apart && e.set("iidx") == "beatmania");
        e.edits["popn"] = readSet("popn");
        e.edits["popn"]["P1 Button 1"] = QStringList{"key:B"};
        e.dirty << "popn";
        e.setApart(true, readSet);
        CHECK(e.apart && e.set("iidx") == "iidx" && e.series()[0].second.contains("bm3"));
        CHECK(e.edits.value("iidx").value("P1 Button 1") == QStringList{"key:Z"});   // (started from the shared set)
        CHECK(e.unsaved());
        CHECK(files(cfg::configDir()) == before);   // (ticking wrote nothing, the pop'n set not saved by it)
    }
    CHECK(files(cfg::configDir()) == before);
    CHECK(!cfg::settings().contains("controls/beatmaniaApart") && !cfg::beatmaniaApart());

    // ticked and unticked again: nothing left to save
    {
        cfg::ControlsEdits e;
        e.setApart(true, readSet);
        e.setApart(false, readSet);
        CHECK(!e.unsaved() && e.edits.size() == 1);   // (the shared set, read for the copies)
    }

    // the shared set changed, then ticked: the change carries into the three (not written by the tick);
    // one of them changed, Save: every set changed written, the setting set, the layouts copied
    {
        cfg::ControlsEdits e;
        e.edits["beatmania"] = readSet("beatmania");
        e.edits["beatmania"]["P1 Button 1"] = QStringList{"key:M"};
        e.dirty << "beatmania";
        e.edits["popn"] = readSet("popn");
        e.edits["popn"]["P1 Button 1"] = QStringList{"key:B"};
        e.dirty << "popn";
        e.setApart(true, readSet);
        for (const char *m : {"djbm", "iidx", "bm3"}) CHECK(e.edits.value(m).value("P1 Button 1") == QStringList{"key:M"});
        CHECK(files(cfg::configDir()) == before);
        e.edits["iidx"]["P1 Button 2"] = QStringList{"key:N"};
        e.dirty << "iidx";
        CHECK(e.save(setText, &err) && err.isEmpty());
        CHECK(!e.unsaved() && cfg::beatmaniaApart());
        CHECK(plain(dir + "/beatmania.txt") == "# beatmania\nP1 Button 1 = key:M\n");   // (its own change, saved)
        CHECK(plain(dir + "/iidx.txt") == "# iidx\nP1 Button 1 = key:M\nP1 Button 2 = key:N\n");
        CHECK(plain(dir + "/djbm.txt") == "# djbm\nP1 Button 1 = key:M\n" && plain(dir + "/bm3.txt") == "# bm3\nP1 Button 1 = key:M\n");
        CHECK(plain(dir + "/popn.txt") == "# popn\nP1 Button 1 = key:B\n");
        CHECK(read(dir + "/layouts/iidx/pad.txt") == "P1 Button 1 = button0\n" && QFile::exists(dir + "/layouts/bm3/pad.txt"));
    }

    // apart, unticked, then Cancel: still apart, nothing written
    before = files(cfg::configDir());
    {
        cfg::ControlsEdits e;
        CHECK(e.apart && e.set("bm3") == "bm3");
        e.setApart(false, readSet);
        CHECK(e.set("bm3") == "beatmania" && e.unsaved());
    }
    CHECK(files(cfg::configDir()) == before && cfg::beatmaniaApart());

    // unticked, Save: together again, the three's files kept for next time
    {
        cfg::ControlsEdits e;
        e.setApart(false, readSet);
        CHECK(e.save(setText, &err));
        CHECK(!cfg::beatmaniaApart() && cfg::controlsFile("iidx") == dir + "/beatmania.txt");
        before.remove(cfg::configDir() + "/launcher.ini");
        QMap<QString, QByteArray> now = files(cfg::configDir());
        now.remove(cfg::configDir() + "/launcher.ini");
        CHECK(now == before);
    }

    // never set apart before (the setting not written), ticked, one changed and unticked again, Save:
    // that one's file written, and the setting with it, so the start-up's joining leaves it alone
    cfg::settings().remove("controls/beatmaniaApart");
    for (const char *m : {"djbm", "iidx", "bm3"}) QFile::remove(dir + "/" + QString(m) + ".txt");
    {
        cfg::ControlsEdits e;
        e.setApart(true, readSet);
        e.edits["bm3"]["P1 Button 1"] = QStringList{"key:Q"};
        e.dirty << "bm3";
        e.setApart(false, readSet);
        CHECK(e.edits.contains("bm3") && !e.edits.contains("iidx"));   // (the unchanged copies dropped)
        CHECK(e.save(setText, &err));
        CHECK(plain(dir + "/bm3.txt") == "# bm3\nP1 Button 1 = key:Q\n" && !QFile::exists(dir + "/iidx.txt"));
        CHECK(cfg::settings().contains("controls/beatmaniaApart") && !cfg::beatmaniaApart());
        QFile::remove(dir + "/beatmania.txt");
        cfg::mergeControls();
        CHECK(QFile::exists(dir + "/bm3.txt") && !QFile::exists(dir + "/beatmania.txt"));
    }

    if (g_failed) fprintf(stderr, "%d failed\n", g_failed);
    return g_failed ? 1 : 0;
}
