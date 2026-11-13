// SPDX-License-Identifier: GPL-3.0-or-later
// The run logs of a folder in the order they were written (Qt's core only: tested on its own,
// tests/launcher/test_runlogs.cpp). By when each file was last written, not by its name's date: and
// a log written "after now" was written before the PC's clock was set back, so it counts as older
// than any written since. (A cabinet whose clock had lost six days kept the week before's ten logs
// as "the newest", and threw each new run's log away when the next run started.)
#pragma once

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QStringList>

#include <algorithm>

namespace runlogs {

// The folder's *.log files' names, oldest first
inline QStringList oldestFirst(const QString &dir, const QDateTime &now = QDateTime::currentDateTime()) {
    QFileInfoList logs = QDir(dir).entryInfoList({"*.log"}, QDir::Files);
    const QDateTime limit = now.addSecs(120);   // (a file just written, on a disk whose clock isn't this PC's to the second)
    std::stable_sort(logs.begin(), logs.end(), [&limit](const QFileInfo &a, const QFileInfo &b) {
        const QDateTime ta = a.lastModified(), tb = b.lastModified();
        if ((ta > limit) != (tb > limit)) return ta > limit;
        return ta != tb ? ta < tb : a.fileName() < b.fileName();
    });
    QStringList names;
    for (const QFileInfo &l : logs) names << l.fileName();
    return names;
}

}  // namespace runlogs
