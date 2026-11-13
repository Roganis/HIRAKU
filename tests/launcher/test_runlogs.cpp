// SPDX-License-Identifier: GPL-3.0-or-later
// The run logs' order (launcher/runlogs.h): by when they were written, and what a PC's clock set
// back does to it.
//
//   test_runlogs DIR      DIR: a folder to make files in. Exit 0 when all hold
#include <QDir>
#include <QFile>

#include <cstdio>

#include "runlogs.h"

namespace {

int failed = 0;
void check(bool ok, const char *what) {
    printf("%s%s\n", ok ? "ok    " : "FAIL  ", what);
    failed += !ok;
}

void make(const QString &dir, const QString &name, const QDateTime &written) {
    QFile f(dir + "/" + name);
    if (!f.open(QIODevice::WriteOnly)) return;
    f.write("log\n");
    f.flush();   // (what is still to be written would date the file again)
    f.setFileTime(written, QFileDevice::FileModificationTime);
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    const QString dir = QString::fromLocal8Bit(argv[1]) + "/runlogs";
    QDir(dir).removeRecursively();
    QDir().mkpath(dir);
    const QDateTime now(QDate(2026, 11, 7), QTime(15, 0));
    // the week before, by the clock as it was then; and today, by a clock that has lost six days
    make(dir, "20261108-182743-iidx16.log", QDateTime(QDate(2026, 11, 8), QTime(18, 30)));
    make(dir, "20261111-193257-iidx14.log", QDateTime(QDate(2026, 11, 11), QTime(19, 59)));
    make(dir, "20261107-134300-iidx9.log", QDateTime(QDate(2026, 11, 7), QTime(13, 52)));
    make(dir, "20261107-144741-iidx14.log", QDateTime(QDate(2026, 11, 7), QTime(14, 59)));
    make(dir, "20261101-120000-bm1stmix.log", QDateTime(QDate(2026, 11, 1), QTime(12, 5)));
    make(dir, "notes.txt", now);
    const QStringList got = runlogs::oldestFirst(dir, now);
    check(got == QStringList{"20261108-182743-iidx16.log", "20261111-193257-iidx14.log", "20261101-120000-bm1stmix.log",
                             "20261107-134300-iidx9.log", "20261107-144741-iidx14.log"},
          "logs dated after now are the oldest, then the rest as written; only logs");
    // a clock that is right: as written, whatever the names say
    const QStringList right = runlogs::oldestFirst(dir, QDateTime(QDate(2026, 11, 13), QTime(17, 0)));
    check(right == QStringList{"20261101-120000-bm1stmix.log", "20261107-134300-iidx9.log", "20261107-144741-iidx14.log",
                               "20261108-182743-iidx16.log", "20261111-193257-iidx14.log"},
          "with the clock right: as written");
    // a log being written (a minute "ahead" on another disk's clock) is not one from before
    make(dir, "20261107-150000-iidx12.log", now.addSecs(60));
    check(runlogs::oldestFirst(dir, now).last() == "20261107-150000-iidx12.log", "the log just written is the newest");
    check(runlogs::oldestFirst(dir + "/none", now).isEmpty(), "no folder: no logs");
    printf(failed ? "%d FAILED\n" : "all hold\n", failed);
    return failed ? 1 : 0;
}
