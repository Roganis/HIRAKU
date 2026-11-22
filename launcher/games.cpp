// SPDX-License-Identifier: GPL-3.0-or-later
#include <QCoreApplication>
#include "games.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPainter>
#include <QRegularExpression>
#include <QTextStream>

#include "config.h"
#include "theme.h"

namespace {

// The top-level keys of a game.toml (title, family, ...): all the launcher needs.
void readManifest(const QString &path, Game &g) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return;
    QTextStream in(&f);
    bool first = true;
    static const QRegularExpression kv(R"(^\s*([A-Za-z_]+)\s*=\s*"([^"]*)\")");
    // "# Title (CODE), Konami 2002", maybe with more after a colon ("Konami 1998: generated from ...")
    static const QRegularExpression head(R"(^#\s*.*\),\s*([^:]+?)\s+(\d{4})(?:/\d{4})?\s*(?::.*)?$)");
    QString section;   // "": the top; the rest only for [pc] loader
    while (!in.atEnd()) {
        QString line = in.readLine();
        if (first) {
            first = false;
            auto m = head.match(line);
            if (m.hasMatch()) {
                g.maker = m.captured(1);
                g.year = m.captured(2);
            }
        }
        if (line.trimmed().startsWith('[')) {
            section = line.trimmed().mid(1).section(']', 0, 0).trimmed();
            continue;
        }
        // (a number, not a text in quotes: when its title screen shows, for the game menu's picture)
        static const QRegularExpression seconds(R"(^\s*title_seconds\s*=\s*([0-9.]+))");
        if (auto n = seconds.match(line); n.hasMatch() && section.isEmpty()) g.titleSeconds = n.captured(1).toDouble();
        static const QRegularExpression install(R"(^\s*install_seconds\s*=\s*([0-9.]+))");   // (fast start-up)
        if (auto n = install.match(line); n.hasMatch() && section.isEmpty()) g.installSeconds = n.captured(1).toDouble();
        // (a yes, not a text in quotes: the System 573's memory card reader, for its Game menu)
        static const QRegularExpression cards(R"(^\s*memory_card_reader\s*=\s*true\b)");
        if (section == "board" && cards.match(line).hasMatch()) g.memoryCards = true;
        auto m = kv.match(line);
        if (!m.hasMatch()) continue;
        QString k = m.captured(1), v = m.captured(2);
        if (!section.isEmpty()) {
            if (section == "pc" && k == "loader") g.loader = v, g.hidden = v == "pc64" || v == "pc32";
            // (the C33 station: its board is there, adding its files and its package's DLLs are not yet)
            if (section == "pc" && k == "board" && v == "c33") g.hidden = true;
            continue;
        }
        if (k == "id") g.id = v;
        else if (k == "title") g.title = v;
        else if (k == "title_latin") g.latin = v;
        else if (k == "family") g.family = v;
        else if (k == "mame") g.mame = v;
        else if (k == "core") g.core = v;
        else if (k == "first_boot") g.firstBoot = v;
        else if (k == "install_cassette") g.installCassette = v == "yes";
    }
}

}  // namespace

QString Game::statusText() const {
    // TR: a game's status in the main window's list and its details (short)
    switch (status) {
    case Ready: return tr("Ready");
    case NotBuilt: return tr("Not built");
    case NotInstalled: return tr("Not installed");
    case Unavailable: return tr("Not in this package");
    }
    return {};
}

QString Game::howTo() const {
    QString root = cfg::rootDir();
    // a release (a package: no source beside it) can't build anything, so it says what it can do
    const bool source = QFileInfo::exists(root + "/CMakeLists.txt");
    if (status == Unavailable) {
        if (core == "python")
            // TR: %1 is the game's title
            return tr("%1 is a Konami Python game, and this HIRAKU package has no hiraku-python, which runs "
                      "them. Is the binaries folder right (Settings)?")
                .arg(title);
        if (core.isEmpty() || core == "firebeat")
            return tr("This HIRAKU package has no hiraku-firebeat, which runs the Firebeat games. Is the "
                      "binaries folder right (Settings)?");
        // TR: %1 is the game's title; hiraku-%2 and hiraku-%3 are program names (keep them as they are)
        return tr("This HIRAKU package has no binary for %1 (hiraku-%2 or hiraku-%3). Is the binaries "
                  "folder right (Settings)?")
            .arg(title, id, core);
    }
    // TR: the end of the help for a game not installed yet ("Drop them": the files just listed)
    const QString drop = tr("Drop them on this window (or File > Add game files), all at once. HIRAKU recognises "
                            "the set by its contents and copies the files into its own folder, so they can go "
                            "afterwards; CHDs are read as they are, nothing is unpacked.");
    if (core == "sys573" && status == NotInstalled)   // System 573: the set and the CD
        // TR: %1 is the game's title, %2 the MAME set's name, %3 the paragraph on dropping the files
        return tr("HIRAKU has no data for %1 yet. You need your own dumps: the MAME ROM set \"%2\" "
                  "(.zip, .7z or a folder) and its CDs (CHDs: the game's, and an install or multi-session "
                  "unit's CD where MAME's set has one).\n\n%3")
            .arg(title, mame, drop);
    if (core == "twinkle" && status == NotInstalled)   // Twinkle: the set, two CDs and the hard disk
        // TR: %1 is the game's title, %2 the MAME set's name, %3 the paragraph on dropping the files
        return tr("HIRAKU has no data for %1 yet. You need your own dumps: the MAME ROM set \"%2\" "
                  "(.zip, .7z or a folder), its program CD, the player's Video CD (or, from 6th style "
                  "on, its DVD) and its hard disk (CHDs).\n\n%3")
            .arg(title, mame, drop);
    if (core == "pc") {   // a PC game: Konami's own program in HIRAKU's cabinet (hiraku-pc)
        if (status == NotInstalled) {
            // TR: what the game's folder holds, in "...as the cabinet's drive holds it: %2" (C02, JAA, JAB, prog, data: folder names)
            const QString holds = family == "iidx" ? tr("C02 with data and its programs, a folder each by revision (JAA, JAB...: "
                                                        "the last is run)")
                                                   : tr("prog (its programs, by date) and data");
#ifdef _WIN32
            // TR: %1 is the game's title, %2 what the folder holds
            return tr("HIRAKU has no data for %1 yet. You need the game's own folder, as the cabinet's "
                      "drive holds it: %2.\n\n"
                      "Drop that folder on this window, or File > Add game files. HIRAKU reads it where it "
                      "is (it is large), so keep it there.")
                .arg(title, holds);
#else
            // TR: %1 is the game's title, %2 what the folder holds
            return tr("HIRAKU has no data for %1 yet. You need the game's own folder, as the cabinet's "
                      "drive holds it: %2.\n\n"
                      "Drop that folder on this window, or File > Add game files. HIRAKU reads it where it "
                      "is (it is large), so keep it there. On Linux the game runs under Wine, which needs to "
                      "be installed.")
                .arg(title, holds);
#endif
        }
        if (status == NotBuilt || status == Unavailable)
            return tr("This HIRAKU has no hiraku-pc, which runs the PC games. Is the binaries folder right (Settings)?");
        return {};
    }
    if (core == "python") {   // Konami Python: PCSX2 Reliquary hosted, built in Reliquary's tree
        if (status == NotInstalled)
            // TR: %1 is the game's title; bios and keys are folder names (keep them)
            return tr("HIRAKU has no data for %1 yet. You need your own dumps: the game's folder (its disk "
                      "CHD, interface board ROMs, dongles and memory card files, with or without the .py1 "
                      "beside it), and the arcade BIOS (COH-H31000) and the MagicGate key files in folders "
                      "named bios and keys.\n\n"
                      "Drop the game's folder with the bios and keys folders on this window, or File > Add "
                      "game files (the BIOS and keys only once: later games find them). The disk is copied "
                      "into HIRAKU's folder.")
                .arg(title);
        if (status == NotBuilt)
            // TR: %1 is the game's title, %2 its program's path, %3 HIRAKU's folder, %4 the command to run there
            return source ? tr("The data for %1 is installed, but %2 has not been built yet. It is built with "
                               "PCSX2 Reliquary's source (docs/PLAY.md, Konami Python).\n\nIn %3:\n\n%4")
                                .arg(title, exe, root, "oracle/pcsx2/build-reliquary.sh all")
                          // TR: %1 is the game's title
                          : tr("The data for %1 is installed, but this HIRAKU package has no Konami Python "
                               "core: it comes with HIRAKU built from source for now (docs/PLAY.md).")
                                .arg(title);
        return {};
    }
    // TR: %1 is the game's title, %2 its program's path, %3 HIRAKU's folder, %4 the commands to run there
    const char *notBuilt = QT_TRANSLATE_NOOP("Game", "The data for %1 is installed, but %2 has not been built yet.\n\nIn %3:\n\n%4");
    if (family.startsWith("dj") || core == "sys573" || core == "twinkle") {   // DJ Main, 573, Twinkle: no recompilation
        if (status == NotInstalled)
            // TR: %1 is the game's title, %2 the MAME set's name, %3 the paragraph on dropping the files
            return tr("HIRAKU has no data for %1 yet. You need your own dumps: the MAME ROM set \"%2\" "
                      "(.zip, .7z or a folder) and its hard disk (CHD).\n\n%3")
                .arg(title, mame, drop);
        if (status == NotBuilt)
            return source ? tr(notBuilt).arg(title, exe, root,
                                             QString("cmake -S . -B build/cmake -G Ninja && ninja -C build/cmake hiraku-%1").arg(id))
                          // TR: %1 is the game's title; hiraku-%2 and hiraku-%3 are program names (keep them as they are)
                          : tr("The data for %1 is installed, but this HIRAKU package has no binary for "
                               "it (hiraku-%2 or hiraku-%3). Is the binaries folder right (Settings)?")
                                .arg(title, id, core);
        return {};
    }
    QString build = QString("analyze/ghidra.sh %1 && analyze/prep.py %1\n"
                            "cmake -S . -B build/cmake -G Ninja && ninja -C build/cmake hiraku-%1")
                        .arg(id);
    if (status == NotInstalled && exe.endsWith(cfg::exeName("hiraku-firebeat")))   // a package's: the executor
        // TR: %1 is the game's title, %2 the MAME set's name
        return tr("HIRAKU has no data for %1 yet. You need your own dumps: the MAME ROM set \"%2\" "
                  "(.zip, .7z or a folder) and its discs (CHDs: the program CD, and the second disc, DVD "
                  "or hard disk where the game has one), or your own disc images (.cue/.iso).\n\n"
                  "Drop them on this window (or File > Add game files), all at once. HIRAKU finds the "
                  "game by its program, and finds its way around the program itself when it starts.")
            .arg(title, mame);
    if (status == NotInstalled)
        // TR: %1 is the game's title, %2 the MAME set's name
        return tr("HIRAKU has no data for %1 yet. You need your own dumps: the program disc, "
                  "its second disc or HDD, and the MAME ROM set \"%2\" (docs/PLAY.md lists them per game).\n\n"
                  "Drop them on this window, or File > Add game files: CHDs, disc images (.cue/.iso/.img) "
                  "and the set (.zip/.7z), all at once.")
            .arg(title, mame);
    if (status == NotBuilt) {
        bool prepped = QFileInfo::exists(root + "/build/" + id + "/recomp.txt");
        return tr(notBuilt).arg(title, exe, root,
                                prepped ? QString("cmake -S . -B build/cmake -G Ninja && ninja -C build/cmake hiraku-%1").arg(id) : build);
    }
    return {};
}

QList<Game> scanGames() {
    QList<Game> out;
    QString root = cfg::rootDir(), bin = cfg::binDir();
    QDir dir(root + "/games");
    for (const QString &sub : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        QString manifest = dir.filePath(sub + "/game.toml");
        if (!QFileInfo::exists(manifest)) continue;
        Game g;
        g.id = sub;
        readManifest(manifest, g);
        if (g.title.isEmpty()) g.title = g.id;
        // the binaries folder (Settings), next to the launcher, a copied build's bin/,
        // else where the README's build puts it
        // A game's own binary first, then its core's (a release has only those: DJ Main, System 573
        // and Twinkle games run from one binary per core, told the game with --game)
        bool built = false;
        QStringList names = {"hiraku-" + g.id};
        if (g.core == "djmain" || g.core == "sys573" || g.core == "twinkle" || g.core == "python" || g.core == "pc")
            names << "hiraku-" + g.core;
        if (g.core.isEmpty() || g.core == "firebeat") names << "hiraku-firebeat";   // the executor (PLAN.md section 27)
        for (const QString &name : names) {
            for (const QString &d : {bin, QCoreApplication::applicationDirPath(), root + "/bin", root + "/build/cmake"}) {
                QFileInfo exe(d + "/" + cfg::exeName(name));
                if (exe.isFile() && exe.isExecutable()) {
                    g.exe = exe.absoluteFilePath();
                    g.coreBinary = name != "hiraku-" + g.id;
                    built = true;
                    break;
                }
            }
            if (built) break;
        }
        if (!built) g.exe = bin + "/" + cfg::exeName("hiraku-" + g.id);
        bool installed = QFileInfo::exists(root + "/dumps/" + g.id + "/machine.txt");
        g.status = !installed ? Game::NotInstalled : built ? Game::Ready : Game::NotBuilt;
        if (!built && !QFileInfo::exists(root + "/CMakeLists.txt")) g.status = Game::Unavailable;
        out.push_back(g);
    }
    return out;
}

Shelf shelf(const Game &g) {
    // the series, in the order Konami started them
    static const QList<QPair<QString, QStringList>> series = {
        {"beatmania", {"djbm", "bm3"}},
        {"beatmania IIDX", {"iidx"}},
        {"pop'n music", {"djpopn", "djpstage", "popn", "pypopn", "pcpopn", "pcpopnhc"}},
        {"Dance Dance Revolution", {"ddr", "ddrsolo"}},
        {"GuitarFreaks", {"gtrf"}},
        {"DrumMania", {"drmn"}},
        {"Keyboardmania", {"kbm"}},
        {"ParaParaParadise", {"ppp"}},
        {"Dance Maniax", {"dmx"}},
        {"Mambo a GoGo", {"mambo"}},
    };
    // the systems, by their first game's year
    static const QList<QPair<QString, QString>> systems = {
        {"djmain", "DJ Main"}, {"sys573", "System 573"}, {"twinkle", "Twinkle"},
        {"firebeat", "Firebeat"}, {"python", "Konami Python"}, {"pc", "PC"},
    };
    Shelf s{g.family, g.core, (int)series.size(), (int)systems.size()};
    for (int k = 0; k < series.size(); k++)
        if (series[k].second.contains(g.family)) s.series = series[k].first, s.seriesOrder = k;
    const QString core = g.core.isEmpty() ? "firebeat" : g.core;
    for (int k = 0; k < systems.size(); k++)
        if (systems[k].first == core) s.system = systems[k].second, s.systemOrder = k;
    return s;
}

namespace {
struct Look { QColor bg; const char *text; };
Look familyLook(const QString &family) {
    return family == "popn" ? Look{QColor(0xe8, 0x4a, 0x8a), "pop'n"}
           : family == "djbm" ? Look{QColor(0x22, 0x2a, 0x5a), "bm"}
           : family == "djpopn" ? Look{QColor(0xf0, 0xb4, 0x1e), "pop'n"}
           : family == "pypopn" ? Look{QColor(0x3c, 0xb4, 0xdc), "pop'n"}
           : family == "pcpopn" || family == "pcpopnhc" ? Look{QColor(0x7c, 0xc8, 0x3c), "pop'n"}
           : family == "djpstage" ? Look{QColor(0xe0, 0x50, 0x90), "stage"}
           : family == "ppp"  ? Look{QColor(0xf0, 0x8a, 0x24), "PPP"}
           : family == "kbm"  ? Look{QColor(0x2e, 0x9e, 0x6a), "KBM"}
           : family == "ddr"  ? Look{QColor(0x8a, 0x2b, 0xc8), "DDR"}
           : family == "ddrsolo" ? Look{QColor(0x8a, 0x2b, 0xc8), "Solo"}
           : family == "dmx"  ? Look{QColor(0x1e, 0xa0, 0xb4), "DMX"}
           : family == "mambo" ? Look{QColor(0xe0, 0x8c, 0x1e), "Mambo"}
           : family == "drmn" ? Look{QColor(0xc8, 0x3a, 0x2b), "DM"}
           : family == "gtrf" ? Look{QColor(0x2b, 0x8a, 0xc8), "GF"}
           : family == "iidx" ? Look{QColor(0x18, 0x18, 0x20), "IIDX"}
                              : Look{QColor(0x3a, 0x4c, 0xb8), "bm III"};
}
}  // namespace

QColor familyColour(const QString &family) { return theme::badge(family, familyLook(family).bg); }

QIcon familyIcon(const QString &family) {
    Look l = familyLook(family);
    const bool pop = theme::pop();
    l.bg = theme::badge(family, l.bg);
    QIcon icon;
    for (int size : {32, 64}) {
        QPixmap pm(size, size);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        if (pop) {   // Pop: a sweet, a shade under it, a shine on top
            const double r = size / 4.0;
            p.setBrush(l.bg.darker(135));
            p.drawRoundedRect(QRectF(1, 2.5, size - 2, size - 3.5), r, r);
            QLinearGradient g(0, 0, 0, size);
            g.setColorAt(0, l.bg.lighter(118));
            g.setColorAt(1, l.bg);
            p.setBrush(g);
            p.drawRoundedRect(QRectF(1, 1, size - 2, size - 4.5), r, r);
            p.setBrush(QColor(255, 255, 255, 60));
            p.drawRoundedRect(QRectF(size * 0.18, size * 0.1, size * 0.64, size * 0.16), size * 0.08, size * 0.08);
        } else {
            p.setBrush(l.bg);
            p.drawRoundedRect(QRectF(1, 1, size - 2, size - 2), size / 6.0, size / 6.0);
        }
        QFont f = p.font();
        f.setWeight(pop ? QFont::Black : QFont::Bold);
        f.setPixelSize(size * (strlen(l.text) > 4 ? 0.26 : 0.32));
        p.setFont(f);
        const bool light = 0.299 * l.bg.redF() + 0.587 * l.bg.greenF() + 0.114 * l.bg.blueF() > 0.7;   // (yellow: dark letters)
        const QRectF box(0, pop ? -1 : 0, size, size);
        if (pop && !light) {
            p.setPen(QColor(0, 0, 0, 70));
            p.drawText(box.translated(0, size / 32.0), Qt::AlignCenter, l.text);
        }
        p.setPen(light ? QColor("#3a2a00") : Qt::white);
        p.drawText(box, Qt::AlignCenter, l.text);
        icon.addPixmap(pm);
        if (pop) icon.addPixmap(pm, QIcon::Selected);   // (not faded on the selected row)
    }
    return icon;
}
