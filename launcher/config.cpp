// SPDX-License-Identifier: GPL-3.0-or-later
#include "config.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <QRegularExpression>
#include <QStandardPaths>

#include <algorithm>

#ifndef Q_OS_WIN
#include <unistd.h>
#endif

#ifndef HK_ROOT
#define HK_ROOT "."
#endif

namespace cfg {

namespace {
// The root found without the settings (which may live in it): $HIRAKU_ROOT, else the checkout the
// launcher was built from (HK_ROOT; "" in a release's launcher, which knows no checkout), else the
// launcher's folder or its parent (a copied build, a package).
QString foundRoot() {
    QString env = qEnvironmentVariable("HIRAKU_ROOT");
    if (!env.isEmpty() && QDir(env + "/games").exists()) return env;
    const QString builtIn = QString(HK_ROOT);
    if (!builtIn.isEmpty() && QDir(builtIn + "/dumps").exists()) return builtIn;
    QDir app(QCoreApplication::applicationDirPath());
    if (app.exists("dumps")) return app.absolutePath();
    if (app.cdUp() && app.exists("dumps")) return app.absolutePath();
    if (!builtIn.isEmpty()) return builtIn;
    return app.absolutePath();   // (a package whose dumps/ is gone: still the folder above bin/)
}

// A portable install's folder, or "": the launcher's own folder or its parent when it holds a
// folder `user` (first: a package built on this PC must not take the checkout it came from),
// else the root above when it does
QString portableRoot() {
    static const QString r = [] {
        QDir app(QCoreApplication::applicationDirPath());
        if (app.exists("user") && app.exists("games")) return app.absolutePath();
        if (app.cdUp() && app.exists("user") && app.exists("games")) return app.absolutePath();
        QDir f(foundRoot());
        return f.exists("user") ? f.absolutePath() : QString();
    }();
    return r;
}
}  // namespace

bool portable() { return !portableRoot().isEmpty(); }

QString configDir() {
    if (portable()) return portableRoot() + "/user/config";
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/hiraku";
}

QSettings &settings() {
    static QSettings s(configDir() + "/launcher.ini", QSettings::IniFormat);
    return s;
}

QString rootDir() {
    // a portable install is its own root, wherever it was moved. Otherwise the setting, else as
    // the game binaries find theirs (foundRoot). A setting without games/ is skipped: one left
    // from another copy of HIRAKU (an older package moved or deleted) would show an empty list.
    if (portable()) return portableRoot();
    QString r = settings().value("paths/root").toString();
    if (!r.isEmpty() && QDir(r + "/games").exists()) return r;
    return foundRoot();
}

QString binDir() {
    QString b = settings().value("paths/bin").toString();
    return b.isEmpty() ? QCoreApplication::applicationDirPath() : b;
}

QString nvramBase() { return settings().value("paths/nvram").toString(); }

QString nvramDir(const QString &game) {
    QString base = nvramBase();
    if (base.isEmpty())
        base = portable() ? portableRoot() + "/user/data"
                          : QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/hiraku";
    return base + "/" + game;
}

QString modsDir(const QString &game) {
    QString base = nvramBase();
    if (base.isEmpty())
        base = portable() ? portableRoot() + "/user/data"
                          : QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/hiraku";
    return base + "/mods/" + game;
}

QString bundledModsDir(const QString &game) { return rootDir() + "/games/" + game + "/mods"; }

QString logsDir() {
    QString base = nvramBase();
    if (base.isEmpty())
        base = portable() ? portableRoot() + "/user/data"
                          : QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/hiraku";
    return base + "/logs";
}

QString modPath(const QString &game, const QString &mod) {
    const QString own = modsDir(game) + "/" + mod;
    return QFileInfo(own).isDir() ? own : bundledModsDir(game) + "/" + mod;
}

QStringList modsOn(const QString &game) {
    QStringList on;   // the ones still there
    for (const QString &m : settings().value("mods/" + game).toStringList())
        if (QFileInfo(modPath(game, m)).isDir()) on << m;
    return on;
}

void setModsOn(const QString &game, const QStringList &mods) {
    if (mods.isEmpty()) settings().remove("mods/" + game);
    else settings().setValue("mods/" + game, mods);
}

QString saveDir(const QString &game) {
    const QStringList on = modsOn(game);
    if (on.isEmpty()) return nvramDir(game);
    QString name = game;
    for (QString m : on) name += "+" + m.replace(QRegularExpression("[^A-Za-z0-9_.-]"), "_");
    return QFileInfo(nvramDir(game)).path() + "/" + name;
}

QString controlsFile(const QString &family) { return configDir() + "/controls/" + controlsSet(family) + ".txt"; }

// The members of a set (beatmania, pop'n music) had a bindings file each before they shared the set's:
// the files there are joined into it, oldest first (a later line for an input replaces an earlier
// one), and the others kept as <family>.txt.old
void mergeControls() {
    for (auto &series : controlsSeries())
        for (const QString &set : series.second) {
            QFileInfoList old;
            for (const QString &m : controlsSetMembers(set))
                if (QFileInfo f(configDir() + "/controls/" + m + ".txt"); f.exists()) old << f;
            if (old.isEmpty() || (old.size() == 1 && old[0].baseName() == set)) continue;
            std::sort(old.begin(), old.end(), [](auto &a, auto &b) { return a.lastModified() < b.lastModified(); });
            QByteArray text = "# HIRAKU controls for " + controlsSetName(set).toUtf8() + ", joined from ";
            for (int k = 0; k < old.size(); k++) text += (k ? ", " : "") + old[k].fileName().toUtf8();
            text += " (a later line replaces an earlier one).\n";
            for (auto &f : old) {
                QFile in(f.filePath());
                if (!in.open(QIODevice::ReadOnly)) return;
                text += "\n# from " + f.fileName().toUtf8() + "\n" + in.readAll() + "\n";
            }
            QSaveFile out(controlsFile(set));
            if (!out.open(QIODevice::WriteOnly) || out.write(text) != text.size() || !out.commit()) return;
            for (auto &f : old)
                if (f.baseName() != set) QFile::rename(f.filePath(), f.filePath() + ".old");
        }
}
QString lightsFile(const QString &family) { return configDir() + "/lights/" + family + ".txt"; }

QString lightsSocket() {
#ifdef Q_OS_WIN
    return "127.0.0.1:47817";   // Windows: TCP on this computer only (runtime/host/lights.h)
#else
    QString run = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (run.isEmpty()) return QDir::tempPath() + QString("/hiraku-lights-%1.sock").arg(getuid());
    return run + "/hiraku-lights.sock";
#endif
}

QString exeName(const QString &base) {
#ifdef Q_OS_WIN
    return base + ".exe";
#else
    return base;
#endif
}

QStringList python() {
#ifdef Q_OS_WIN
    // a package's own (tools/package-win.sh: python.org's embeddable Python in hiraku\python)
    for (const QString &d : {QCoreApplication::applicationDirPath() + "/../python", QCoreApplication::applicationDirPath() + "/python"})
        if (QFileInfo::exists(d + "/python.exe")) return {QDir::cleanPath(d + "/python.exe")};
    if (QString py = QStandardPaths::findExecutable("py"); !py.isEmpty()) return {py, "-3"};
    if (QString p = QStandardPaths::findExecutable("python"); !p.isEmpty()) return {p};
    return {};
#else
    // a release's own (tools/package.sh, HK_PYTHON_DIR: hiraku/python), else the system's
    for (const QString &d : {QCoreApplication::applicationDirPath() + "/../python", QCoreApplication::applicationDirPath() + "/python"})
        if (QFileInfo::exists(d + "/bin/python3")) return {QDir::cleanPath(d + "/bin/python3")};
    return {"python3"};
#endif
}

const QList<ShaderKey> &shaderKeys() {
    static const QList<ShaderKey> k = {
        {"show", "Show it, move it, hide it", "key:F8"},
        {"up", "Previous setting", "key:Up"},
        {"down", "Next setting", "key:Down"},
        {"less", "Less", "key:Left"},
        {"more", "More", "key:Right"},
        {"saved", "Back to the saved value", "key:Backspace"},
        {"default", "Back to the default", "key:Delete"},
        {"save", "Save for all games", "key:Return"},
        {"save_game", "Save for this game", ""},
    };
    return k;
}

QString familyName(const QString &family) {
    if (family == "bm3") return "beatmania III";
    if (family == "popn") return "pop'n music (Firebeat)";
    if (family == "ppp") return "ParaParaParadise";
    if (family == "kbm") return "Keyboardmania";
    if (family == "djbm") return "beatmania (DJ Main)";
    if (family == "djpopn") return "pop'n music (DJ Main)";
    if (family == "djpstage") return "pop'n stage (DJ Main)";
    if (family == "ddr") return "Dance Dance Revolution (System 573)";
    if (family == "ddrsolo") return "Dance Dance Revolution Solo (System 573)";
    if (family == "dmx") return "Dance Maniax (System 573)";
    if (family == "mambo") return "Mambo a GoGo (System 573)";
    if (family == "drmn") return "DrumMania (System 573)";
    if (family == "gtrf") return "GuitarFreaks (System 573)";
    if (family == "pypopn") return "pop'n music (Konami Python)";
    if (family == "pcpopn") return "pop'n music (PC)";
    if (family == "pcpopnhc") return "pop'n music (PC, two screens)";
    if (family == "iidx") return "beatmania IIDX";
    return family;
}

QList<QPair<QString, QStringList>> familySeries() {
    return {{"beatmania", {"djbm", "iidx", "bm3"}},
            {"pop'n music", {"djpopn", "popn", "pypopn", "pcpopn", "pcpopnhc", "djpstage"}},
            {"Other games", {"ddr", "ddrsolo", "dmx", "drmn", "gtrf", "kbm", "mambo", "ppp"}}};
}

QString controlsSet(const QString &family) {
    if (family == "djbm" || family == "iidx" || family == "bm3") return "beatmania";
    if (family == "djpopn" || family == "pypopn" || family == "pcpopn" || family == "pcpopnhc") return "popn";
    return family;
}

// The first member's table gives the order (IIDX: the seven keys together)
QStringList controlsSetMembers(const QString &set) {
    if (set == "beatmania") return {"iidx", "djbm", "bm3"};
    if (set == "popn") return {"djpopn", "popn", "pypopn", "pcpopn", "pcpopnhc"};
    return {set};
}

QString controlsSetName(const QString &set) {
    if (set == "beatmania") return "beatmania";
    if (set == "popn") return "pop'n music";
    return familyName(set);
}

QString controlsMemberName(const QString &family) {
    if (family == "djbm" || family == "djpopn") return "DJ Main";
    if (family == "iidx") return "IIDX";
    if (family == "bm3") return "beatmania III";
    if (family == "popn") return "Firebeat";
    if (family == "pypopn") return "Konami Python";
    if (family == "pcpopn" || family == "pcpopnhc") return "PC";
    return familyName(family);
}

QList<QPair<QString, QStringList>> controlsSeries() {
    return {{"", {"beatmania", "popn", "djpstage", "ddr", "ddrsolo", "dmx", "drmn", "gtrf", "kbm", "mambo", "ppp"}}};
}

QStringList families() {
    QStringList l;
    for (auto &s : familySeries()) l += s.second;
    return l;
}

}  // namespace cfg
