// SPDX-License-Identifier: GPL-3.0-or-later
#include "updater.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QTextBrowser>
#include <QUrl>
#include <QVBoxLayout>

#include "config.h"

namespace upd {

QString packageRoot() { return QDir::cleanPath(QCoreApplication::applicationDirPath() + "/.."); }

QString currentVersion() {
    QFile f(packageRoot() + "/VERSION");
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()).trimmed() : QString();
}

bool available() { return !currentVersion().isEmpty() && QFileInfo::exists(packageRoot() + "/tools/update.py"); }

void cleanUp() {
    for (const char *d : {"/update", "/update-old"}) {
        QDir dir(packageRoot() + d);
        if (dir.exists()) dir.removeRecursively();   // what is still in use stays, for next time
    }
}

}  // namespace upd

namespace {

QString script() { return upd::packageRoot() + "/tools/update.py"; }

// the last lines the script wrote, as its error
QString lastLines(const QByteArray &out) {
    const QStringList l = QString::fromUtf8(out).trimmed().split('\n');
    return l.mid(std::max<qsizetype>(0, l.size() - 3)).join('\n');
}

}  // namespace

UpdateCheck::UpdateCheck(QObject *parent) : QObject(parent) {
    connect(&m_proc, &QProcess::finished, this, [this](int code, QProcess::ExitStatus st) {
        const QByteArray out = m_proc.readAllStandardOutput();
        const QJsonObject rel = QJsonDocument::fromJson(out.trimmed().split('\n').last()).object();
        if (st == QProcess::NormalExit && code == 0 && !rel.isEmpty()) emit done(true, rel, {});
        else emit done(false, {}, lastLines(m_proc.readAllStandardError() + out));
    });
    connect(&m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) emit done(false, {}, "Python could not be started: " + m_proc.errorString());
    });
}

void UpdateCheck::start() {
    if (m_proc.state() != QProcess::NotRunning) return;
    const QStringList py = cfg::python();
    m_proc.setWorkingDirectory(upd::packageRoot());
    // --save: the answer where the games look (runtime/host/notice.h), so one started on its own
    // doesn't ask GitHub again soon after
    const QString data = cfg::portable() ? upd::packageRoot() + "/user/data"
                                         : QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/hiraku";
    m_proc.start(py[0], py.mid(1) + QStringList{script(), "check", "--save", data + "/update-check.json"});
}

UpdateDialog::UpdateDialog(const QJsonObject &rel, QWidget *parent) : QDialog(parent), m_rel(rel) {
    setWindowTitle("Update HIRAKU");
    auto *v = new QVBoxLayout(this);
    const QString size = QLocale().formattedDataSize(rel["size"].toInteger());
    auto *head = new QLabel(QString("<b>%1</b> is out: this is %2. The download is %3.<br>Your games, saves and "
                                    "settings stay as they are; the launcher restarts once it's in.")
                                .arg(rel["name"].toString().toHtmlEscaped(), upd::currentVersion().toHtmlEscaped(), size));
    head->setWordWrap(true);
    v->addWidget(head);
    auto *notes = new QTextBrowser;
    notes->setOpenExternalLinks(true);
    notes->setMarkdown(rel["notes"].toString());
    v->addWidget(notes, 1);
    m_status = new QLabel;
    m_status->setWordWrap(true);
    m_status->hide();
    v->addWidget(m_status);
    m_bar = new QProgressBar;
    m_bar->hide();
    v->addWidget(m_bar);
    auto *bb = new QDialogButtonBox;
    m_go = bb->addButton("Update and restart", QDialogButtonBox::AcceptRole);
    bb->addButton("Later", QDialogButtonBox::RejectRole);
    if (!rel["page"].toString().isEmpty()) {
        auto *page = bb->addButton("Release page", QDialogButtonBox::HelpRole);
        connect(page, &QPushButton::clicked, this, [this] { QDesktopServices::openUrl(QUrl(m_rel["page"].toString())); });
    }
    connect(m_go, &QPushButton::clicked, this, &UpdateDialog::download);
    connect(bb, &QDialogButtonBox::rejected, this, &UpdateDialog::reject);
    v->addWidget(bb);
    resize(560, 460);

    connect(&m_proc, &QProcess::readyReadStandardOutput, this, [this] {
        while (m_proc.canReadLine()) {
            const QString line = QString::fromUtf8(m_proc.readLine()).trimmed();
            if (line.startsWith("progress ")) {
                const QStringList p = line.split(' ');
                const qint64 done = p.value(1).toLongLong(), total = p.value(2).toLongLong();
                m_bar->setRange(0, total > 0 ? 1000 : 0);
                if (total > 0) m_bar->setValue((int)(done * 1000 / total));
                m_status->setText("Downloading… " + QLocale().formattedDataSize(done));
            } else if (line == "unpacking") {
                m_bar->setRange(0, 0);
                m_status->setText("Unpacking…");
            } else if (line.startsWith("ready ")) {
                m_status->setText("Ready: the launcher closes and starts again.");
            }
        }
    });
    connect(&m_proc, &QProcess::finished, this, [this](int code, QProcess::ExitStatus st) {
        if (st == QProcess::NormalExit && code == 0) {
            accept();
            return;
        }
        m_bar->hide();
        m_status->setText("The update failed: " + lastLines(m_proc.readAllStandardError()));
        m_go->setEnabled(true);
        m_go->setText("Try again");
    });
}

void UpdateDialog::download() {
    // the package's folder must take new files (a copy in a read-only place can't update itself)
    QFileInfo root(upd::packageRoot());
    if (!root.isWritable()) {
        QMessageBox::warning(this, "Update HIRAKU", root.filePath() + " can't be written to: download the new "
                             "version from the release page instead.");
        return;
    }
    m_go->setEnabled(false);
    m_status->show();
    m_status->setText("Downloading…");
    m_bar->setRange(0, 0);
    m_bar->show();
    const QStringList py = cfg::python();
    m_proc.setWorkingDirectory(upd::packageRoot());
    m_proc.start(py[0], py.mid(1) + QStringList{script(), "download", upd::packageRoot() + "/update"});
}

void UpdateDialog::reject() {
    if (m_proc.state() != QProcess::NotRunning) {
        m_proc.kill();
        m_proc.waitForFinished(3000);
        QDir(upd::packageRoot() + "/update").removeRecursively();
    }
    QDialog::reject();
}

bool UpdateDialog::startInstall(QString *error) const {
    // the new package's own Python and update.py do it, so nothing they run from is replaced
    const QString root = upd::packageRoot(), neu = root + "/update/new/hiraku";
#ifdef Q_OS_WIN
    const QString py = neu + "/python/python.exe";
#else
    const QString py = neu + "/python/bin/python3";
#endif
    const QStringList args{neu + "/tools/update.py", "apply", root, neu, "--wait",
                           QString::number(QCoreApplication::applicationPid()), "--restart"};
    if (QFileInfo::exists(py) && QProcess::startDetached(py, args, root)) return true;
    const QStringList sys = cfg::python();   // a package without its own Python
    if (QProcess::startDetached(sys[0], sys.mid(1) + args, root)) return true;
    if (error) *error = "Python could not be started to put the new files in.";
    return false;
}
