// SPDX-License-Identifier: GPL-3.0-or-later
#include "report.h"
#include "runlogs.h"

#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QSysInfo>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

#include "config.h"
#include "shell/sys.h"

// Where a report can be mailed, by who has no GitHub account ("": nowhere, and the dialog has no mail button)
static const char kMail[] = "mrroganis@gmail.com";

namespace report {

Cleaner Cleaner::forThisComputer() {
    Cleaner c;
    c.folders << cfg::rootDir();
    c.homes << QDir::homePath();
    if (const QString p = qEnvironmentVariable("USERPROFILE"); !p.isEmpty()) c.homes << p;
    for (const char *v : {"USERNAME", "USER"})
        if (const QString n = qEnvironmentVariable(v); !n.isEmpty() && !c.names.contains(n)) c.names << n;
    if (const QString h = QSysInfo::machineHostName(); !h.isEmpty()) c.names << h;
    return c;
}

QString make(const QString &runLog, const QString &game, const QString &title, Cleaner &c) {
    QString version = "a build from source (no VERSION file)";
    if (QFile v(cfg::rootDir() + "/VERSION"); v.open(QIODevice::ReadOnly)) version = QString::fromUtf8(v.readAll()).trimmed();
    QString out;
    out += "HIRAKU problem report, made " + QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm") + "\n";
    out += "HIRAKU: " + version + ", the launcher's build " + QString::fromUtf8(hk::sys::build_id()) + "\n";
    out += "System: " + QSysInfo::prettyProductName() + " (" + QSysInfo::kernelType() + " " + QSysInfo::kernelVersion() + ", " +
           QSysInfo::currentCpuArchitecture() + ")" + "\n";
    out += "Game: " + title + " (" + game + ")\n";
    QFile f(runLog);
    QString log = f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString("(the run's log can't be read)");
    log.replace("\r\n", "\n");
    out += "\n=== The run's log (" + QFileInfo(runLog).fileName() + ") ===\n" + log;
    // the game's own log (a PC game's), when it is this run's: written from the run's start to its end
    const QFileInfo run(runLog);
    const QDateTime started = QDateTime::fromString(run.fileName().left(15), "yyyyMMdd-HHmmss");
    const QFileInfo pc(cfg::saveDir(game) + "/pc.log");
    if (pc.exists() && started.isValid() && pc.lastModified() >= started.addSecs(-5) && pc.lastModified() <= run.lastModified().addSecs(120)) {
        QFile p(pc.filePath());
        if (p.open(QIODevice::ReadOnly)) {
            QByteArray b = p.readAll();
            if (b.size() > 512 * 1024) b = b.left(256 * 1024) + "\n...\n" + b.right(256 * 1024);   // (a long run's)
            out += "\n=== The game's own log (pc.log) ===\n" + QString::fromUtf8(b).replace("\r\n", "\n");
        }
    }
    return c.clean(out);
}

}  // namespace report

ReportDialog::ReportDialog(QWidget *parent, const QString &runLog) : QDialog(parent) {
    setWindowTitle("Report a problem");
    resize(900, 640);
    auto *lay = new QVBoxLayout(this);
    auto *intro = new QLabel(
        "A report of one run: its log, the game's own log and what system this is, cleaned of your "
        "folders' and names, card numbers and network addresses. Read it below before you share it: "
        "what it shows is all that goes in. Save it, then attach the file to an issue on GitHub" +
        (*kMail ? QString(", or to a mail to %1 (no account needed).").arg(kMail) : QString(".")));
    intro->setWordWrap(true);
    intro->setTextInteractionFlags(Qt::TextSelectableByMouse);   // (the address, to copy into a webmail)
    lay->addWidget(intro);
    auto *row = new QHBoxLayout;
    row->addWidget(new QLabel("The run:"));
    m_runs = new QComboBox;
    row->addWidget(m_runs, 1);
    lay->addLayout(row);
    m_text = new QPlainTextEdit;
    m_text->setReadOnly(true);
    m_text->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    lay->addWidget(m_text, 1);
    auto *buttons = new QDialogButtonBox;
    auto *saveBtn = buttons->addButton("Save the report…", QDialogButtonBox::AcceptRole);
    QPushButton *mailBtn = *kMail ? buttons->addButton("Send it by mail", QDialogButtonBox::ActionRole) : nullptr;
    auto *issueBtn = buttons->addButton("Open a GitHub issue", QDialogButtonBox::ActionRole);
    buttons->addButton(QDialogButtonBox::Close);
    issueBtn->setToolTip("Opens GitHub's new issue page in your browser, with the system and the game filled in: "
                         "describe what happened and attach the saved report");
    lay->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    disconnect(saveBtn, nullptr, this, nullptr);
    connect(saveBtn, &QPushButton::clicked, this, [this] { save(); });
    connect(issueBtn, &QPushButton::clicked, this, [this] { openIssue(); });
    if (mailBtn) {
        mailBtn->setToolTip(QString("Saves the report, then opens your mail program with a message to %1, the system and "
                                    "the game filled in: describe what happened, and attach the saved report if it "
                                    "isn't attached already. With no mail program set up, send the file to that "
                                    "address from your webmail").arg(kMail));
        connect(mailBtn, &QPushButton::clicked, this, [this] { mail(); });
    }

    // the runs, newest first ("20261108-183240-iidx16.log")
    const QDir dir(cfg::logsDir());
    QStringList logs = runlogs::oldestFirst(dir.path());   // (as written: runlogs.h)
    std::reverse(logs.begin(), logs.end());
    for (const QString &l : logs) {
        const QDateTime t = QDateTime::fromString(l.left(15), "yyyyMMdd-HHmmss");
        const QString game = l.mid(16).chopped(4);
        m_runs->addItem((t.isValid() ? t.toString("yyyy-MM-dd HH:mm") : l) + "   " + game, dir.filePath(l));
    }
    if (!runLog.isEmpty())
        if (const int i = m_runs->findData(runLog); i >= 0) m_runs->setCurrentIndex(i);
    connect(m_runs, &QComboBox::currentIndexChanged, this, [this] { fill(); });
    if (logs.isEmpty()) {
        m_text->setPlainText("No game has been run yet: run the game that has the problem, then come back here.");
        saveBtn->setEnabled(false);
        if (mailBtn) mailBtn->setEnabled(false);
    } else {
        fill();
    }
}

void ReportDialog::fill() {
    const QString log = m_runs->currentData().toString();
    const QString game = QFileInfo(log).fileName().mid(16).chopped(4);
    // the game's name: the run log's last line says it ("--- beatmania IIDX 14 GOLD exited (code 0) ---")
    QString title = game;
    if (QFile f(log); f.open(QIODevice::ReadOnly)) {
        const QStringList lines = QString::fromUtf8(f.readAll()).split('\n', Qt::SkipEmptyParts);
        static const QRegularExpression end(R"(^--- (.+?) (exited|crashed|ended by)\b)");
        for (qsizetype i = lines.size() - 1; i >= 0 && i >= lines.size() - 3; i--)
            if (const auto m = end.match(lines[i]); m.hasMatch()) title = m.captured(1);
    }
    report::Cleaner c = report::Cleaner::forThisComputer();
    m_text->setPlainText(report::make(log, game, title, c));
    m_saved.clear();   // (a file saved before is another run's report)
}

void ReportDialog::save() {
    const QString log = QFileInfo(m_runs->currentData().toString()).completeBaseName();
    QString dir = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    if (dir.isEmpty()) dir = QDir::homePath();
    const QString path = QFileDialog::getSaveFileName(this, "Save the report", dir + "/hiraku-report-" + log + ".txt", "Text (*.txt)");
    if (path.isEmpty()) return;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(m_text->toPlainText().toUtf8()) < 0) {
        QMessageBox::warning(this, "Report a problem", "Could not write " + path + ": " + f.errorString());
        return;
    }
    m_saved = path;
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));   // where it is, to drag into the issue
}

void ReportDialog::openIssue() {
    const QStringList head = m_text->toPlainText().section("\n=== ", 0, 0).split('\n', Qt::SkipEmptyParts);
    QString body = "**What happened** (and what you did just before):\n\n\n"
                   "**The report:** drag the file saved by Help > Report a problem here" +
                   (m_saved.isEmpty() ? QString() : " (" + QFileInfo(m_saved).fileName() + ")") + "\n\n";
    for (const QString &l : head.mid(1)) body += "- " + l + "\n";   // HIRAKU, the system, the game
    QString game = head.size() > 3 ? head[3].mid(6) : QString();
    QUrlQuery q;
    q.addQueryItem("title", game.isEmpty() ? QString("A problem") : game + ": ");
    q.addQueryItem("body", body);
    QUrl url("https://github.com/Roganis/HIRAKU/issues/new");
    url.setQuery(q);
    QDesktopServices::openUrl(url);
}

void ReportDialog::mail() {
    if (m_saved.isEmpty()) save();   // the file to attach
    if (m_saved.isEmpty()) return;
    const QStringList head = m_text->toPlainText().section("\n=== ", 0, 0).split('\n', Qt::SkipEmptyParts);
    const QString game = head.size() > 3 ? head[3].mid(6) : QString();
    const QString subject = "HIRAKU problem report" + (game.isEmpty() ? QString() : ": " + game);
    QString body = "What happened, and what you did just before:\n\n\n\n"
                   "(The report is " + QFileInfo(m_saved).fileName() + ": please attach it if it isn't attached already.)\n\n";
    for (const QString &l : head.mid(1)) body += l + "\n";   // HIRAKU, the system, the game
#ifdef Q_OS_LINUX
    // xdg-email hands the file itself to the mail programs that take one (Thunderbird, KMail, Evolution...)
    if (QProcess::startDetached("xdg-email", {"--utf8", "--subject", subject, "--body", body, "--attach", m_saved, kMail})) return;
#endif
    // a mailto: address can't carry a file: the folder save() opened has it, to drag into the message
    QDesktopServices::openUrl(QUrl::fromEncoded("mailto:" + QByteArray(kMail) + "?subject=" + QUrl::toPercentEncoding(subject) +
                                                "&body=" + QUrl::toPercentEncoding(body.replace("\n", "\r\n"))));
}
