// SPDX-License-Identifier: GPL-3.0-or-later
// Help > Report a problem: a run's log, the game's own log (a PC game's pc.log) and what the system
// is, in one text file to attach to a GitHub issue or to a mail (for who has no GitHub account),
// cleaned of what says who the player is or where their files are:
//   HIRAKU's folder            <hiraku>/... (its own layout kept: bin/, user/data/<game>/...)
//   any other folder           <folder>/<its last part> (the game's revision folder, say, stays readable)
//   the home folder, the user's and the computer's names     <home>, <user>, <computer>
//   e-AMUSEMENT card numbers   #1, #2... (the same card the same number through the report)
//   a PCB id, addresses on the network (not this computer's own), MAC addresses
// The person sees all of it before saving it, and HIRAKU sends nothing itself: it opens the browser
// or the person's mail program with a message started, for them to finish and send.
#pragma once

#include <QDialog>
#include <QString>

#include "cleaner.h"

class QComboBox;
class QPlainTextEdit;

namespace report {

// The report's text for a run log (a file in cfg::logsDir()); game: the run's game id, title its name
QString make(const QString &runLog, const QString &game, const QString &title, Cleaner &c);

}  // namespace report

class ReportDialog : public QDialog {
public:
    // runLog: the run to start with ("" the newest)
    explicit ReportDialog(QWidget *parent, const QString &runLog = QString());

private:
    void fill();
    void save();
    void openIssue();
    void mail();
    QComboBox *m_runs;
    QPlainTextEdit *m_text;
    QString m_saved;   // the shown report's file, once saved
};
