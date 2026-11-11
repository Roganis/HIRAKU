// SPDX-License-Identifier: GPL-3.0-or-later
// Updates a release package to GitHub's latest release, through tools/update.py: the check, the
// dialog that shows what's new and downloads it, and the hand-over to the script, which puts the
// new files in once the launcher has closed and starts it again.
//
// The script keeps a journal while it puts them in (update-journal in the package's folder) and
// holds a lock (update-lock). A launcher started meanwhile steps aside; one that finds the journal
// with nobody at work (a kill, a power cut) starts the script again, which finishes the update or
// undoes it; and nothing of an update is removed while either is there.
#pragma once

#include <QDialog>
#include <QJsonObject>
#include <QProcess>

class QLabel;
class QProgressBar;
class QPushButton;

namespace upd {
QString packageRoot();      // the folder above the launcher's bin/
QString currentVersion();   // its VERSION file: "" in a checkout or a build by hand
bool available();           // a release package, which can update itself
void cleanUp();             // what the last update left: update/ (the download), update-old/;
                            // nothing while an update is going on or was cut short
bool applying();            // the script is putting an update in right now
bool cutShort();            // an update was cut short: its journal is in the folder
// The script, started on its own to put the downloaded package in once this launcher has ended:
// false if it couldn't start
bool startApply(QString *error);
// ... or to put right an update that was cut short. once: only if it wasn't already tried on the
// folder as it is (a journal that the script can't deal with must not end every launcher started)
bool resume(bool once, QString *error);
// Tells that an update was cut short and couldn't be put right (why: the script's own words, if
// any), and offers to try again: true if the script was started, and the launcher must end
bool offerResume(QWidget *parent, const QString &why);
}

// One look at the latest release (tools/update.py check)
class UpdateCheck : public QObject {
    Q_OBJECT
public:
    explicit UpdateCheck(QObject *parent = nullptr);
    void start();
signals:
    // ok: rel is the release (current, latest, newer, name, notes, page, size); else error says why
    void done(bool ok, const QJsonObject &rel, const QString &error);
private:
    QProcess m_proc;
};

// What's new, then the download; accepted = ready: the launcher closes and upd::startApply() runs
class UpdateDialog : public QDialog {
    Q_OBJECT
public:
    UpdateDialog(const QJsonObject &rel, QWidget *parent = nullptr);
    void reject() override;
private:
    void download();
    QJsonObject m_rel;
    QProcess m_proc;
    QLabel *m_status;
    QProgressBar *m_bar;
    QPushButton *m_go;
    QString m_err;
};
