// SPDX-License-Identifier: GPL-3.0-or-later
// The controls sets (launcher/config.h): the beatmania cabinets (DJ Main, IIDX, III) share one
// bindings file until the player sets them apart; then each has its own, started as a copy of the
// shared one (and its controllers' layouts), and the shared one is left as it was for when they
// share again. The old per-family files are joined into the shared one only when it isn't there.
//   test_controls_sets DIR
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStandardPaths>

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

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    QDir(argv[1]).removeRecursively();
    qputenv("XDG_CONFIG_HOME", argv[1]);
    QCoreApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(false);
    const QString dir = cfg::configDir() + "/controls";
    CHECK(cfg::configDir().startsWith(QString(argv[1])));

    // shared: the three are one set, in one file
    CHECK(!cfg::beatmaniaApart());
    for (const char *f : {"djbm", "iidx", "bm3"}) {
        CHECK(cfg::isBeatmania(f));
        CHECK(cfg::controlsSet(f) == "beatmania");
        CHECK(cfg::controlsFile(f) == dir + "/beatmania.txt");
    }
    CHECK(!cfg::isBeatmania("popn") && cfg::controlsSet("pypopn") == "popn");
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

    // an older launcher's files, one a family and no shared one: joined into it, as before
    QFile::remove(dir + "/beatmania.txt");
    QFile::remove(dir + "/djbm.txt");
    QFile::remove(dir + "/bm3.txt");
    cfg::mergeControls();
    CHECK(read(dir + "/beatmania.txt").contains("P1 Button 1 = key:X"));
    CHECK(!QFile::exists(dir + "/iidx.txt") && QFile::exists(dir + "/iidx.txt.old"));

    if (g_failed) fprintf(stderr, "%d failed\n", g_failed);
    return g_failed ? 1 : 0;
}
