// SPDX-License-Identifier: GPL-3.0-or-later
// The launcher's language (launcher/language.h): its catalogue answers Qt's own hook
// (QCoreApplication::translate, a class's tr()), Qt's standard buttons included; plurals; English for
// what isn't translated, and for a catalogue missing or broken; the choice (HK_LANG, the saved one,
// the system's). With --shot: the slice marked so far (the binding dialog) drawn in each catalogue
// the checkout has, to look at.
//
//   test_language SCRATCH_DIR                       the checks
//   test_language SCRATCH_DIR --shot ROOT OUT_DIR   ROOT/lang's catalogues: OUT_DIR/capture-<code>.png
#include <QApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFontDatabase>
#include <QFile>
#include <QPushButton>
#include <QTimer>

#include <cstdio>

#include "capturedialog.h"
#include "language.h"

static int g_failed = 0;
#define CHECK(x) \
    do { \
        if (!(x)) g_failed++, fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
    } while (0)

static void write(const QString &path, const QByteArray &text) {
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) f.write(text);
}

static const char kFrench[] = R"po(msgid ""
msgstr ""
"Language: fr\n"
"Content-Type: text/plain; charset=UTF-8\n"
"Plural-Forms: nplurals=2; plural=(n > 1);\n"

msgctxt "CaptureDialog"
msgid "Skip"
msgstr "Passer"

msgctxt "Test|a verb"
msgid "Play"
msgstr "Jouer"

msgctxt "Test"
msgid "Play"
msgstr "Lecture"

msgctxt "Test"
msgid "%n game(s)"
msgid_plural "%n game(s)"
msgstr[0] "%n jeu"
msgstr[1] "%n jeux"

msgctxt "Test"
msgid "%n file"
msgid_plural "%n files"
msgstr[0] "%n fichier"
msgstr[1] "%n fichiers"

msgctxt "QPlatformTheme"
msgid "Cancel"
msgstr "Annuler"

msgctxt "menu"
msgid "play"
msgstr "jouer"
)po";

static void checks(QApplication &app, const QString &scratch) {
    const QString root = scratch + "/root";
    QDir().mkpath(root + "/lang");
    // the player's own catalogues' folder (<user data folder>/lang): a made-up one, empty for now
    qputenv("XDG_DATA_HOME", (scratch + "/data").toUtf8());
    qputenv("LOCALAPPDATA", (scratch + "/data").toUtf8());
    QDir(scratch + "/data").removeRecursively();
    write(root + "/lang/fr.po", kFrench);
    write(root + "/lang/de.po", "msgid \"\"\nmsgstr \"\n");   // (broken: a string not closed)

    CHECK(language::available(root) == QStringList({"fr", "de"}));
    qunsetenv("HK_LANG");
    CHECK(language::choose("fr", root) == "fr");
    CHECK(language::choose("ja", root) == "en");   // (no catalogue: English)
    qputenv("HK_LANG", "fr");
    CHECK(language::choose("en", root) == "fr");   // HK_LANG over the saved choice
    qunsetenv("HK_LANG");

    CHECK(language::install(app, "fr", root) == "fr");
    CHECK(language::current() == "fr");
    CHECK(QCoreApplication::translate("CaptureDialog", "Skip") == "Passer");
    CHECK(QCoreApplication::translate("CaptureDialog", "Stop") == "Stop");   // untranslated: the English
    CHECK(QCoreApplication::translate("Test", "Play") == "Lecture");
    CHECK(QCoreApplication::translate("Test", "Play", "a verb") == "Jouer");   // Qt's disambiguation
    // Qt's own plurals (one source text; Qt puts %n in) and the catalogue's (singular and plural)
    CHECK(QCoreApplication::translate("Test", "%n game(s)", nullptr, 1) == "1 jeu");
    CHECK(QCoreApplication::translate("Test", "%n game(s)", nullptr, 3) == "3 jeux");
    CHECK(language::trn("Test", "%n file", "%n files", 0) == "0 fichier");
    CHECK(language::trn("Test", "%n file", "%n files", 2) == "2 fichiers");
    CHECK(language::trn("Test", "%n folder", "%n folders", 2) == "2 folders");
    // a class's own tr(): its context (Q_DECLARE_TR_FUNCTIONS)
    CHECK(CaptureDialog::tr("Skip") == "Passer");
    // Qt's own standard buttons: from the catalogue (context QPlatformTheme)
    {
        QDialogButtonBox box(QDialogButtonBox::Cancel);
        CHECK(box.button(QDialogButtonBox::Cancel)->text().remove('&') == "Annuler");
    }
    // the game programs' texts: their glyphs for the font files
    CHECK(language::gameTexts().contains("jouer") && !language::gameTexts().contains("Passer"));

    // a broken catalogue: English, everything as written
    CHECK(language::install(app, "de", root) == "en");
    CHECK(QCoreApplication::translate("CaptureDialog", "Skip") == "Skip");
    CHECK(language::trn("Test", "%n file", "%n files", 1) == "1 file");
    // ... and one that isn't there
    CHECK(language::install(app, "ja", root) == "en");
    CHECK(language::install(app, "en", root) == "en");

    // a player's own catalogue (decision 6): read before the package's; a broken one passed over
    {
        QDir().mkpath(scratch + "/data/hiraku/lang");
        QByteArray mine(kFrench);
        mine.replace("msgstr \"Passer\"", "msgstr \"PASSER\"");
        write(scratch + "/data/hiraku/lang/fr.po", mine);
        write(scratch + "/data/hiraku/lang/it.po", mine.replace("Language: fr", "Language: it"));
        CHECK(language::available(root) == QStringList({"fr", "de", "it"}));   // (a language the package hasn't too)
        CHECK(language::install(app, "fr", root) == "fr" && QCoreApplication::translate("CaptureDialog", "Skip") == "PASSER");
        write(scratch + "/data/hiraku/lang/fr.po", "msgid \"broken");
        CHECK(language::install(app, "fr", root) == "fr" && QCoreApplication::translate("CaptureDialog", "Skip") == "Passer");
        CHECK(language::install(app, "en", root) == "en");
        QDir(scratch + "/data").removeRecursively();
    }

    // Japanese, Korean, Chinese: the language's own family, one this PC has for its writing system
    CHECK(language::cjkFamily("fr").isEmpty() && language::cjkFamily("en").isEmpty());
    for (const auto &[code, ws] : {std::pair{"ja", QFontDatabase::Japanese}, std::pair{"ko", QFontDatabase::Korean},
                                   std::pair{"zh_CN", QFontDatabase::SimplifiedChinese}, std::pair{"zh_TW", QFontDatabase::TraditionalChinese}}) {
        const QString f = language::cjkFamily(code);
        printf("%s: %s\n", code, f.isEmpty() ? "(no font for it here)" : qPrintable(f));
        CHECK(f.isEmpty() == QFontDatabase::families(ws).isEmpty());
        CHECK(f.isEmpty() || QFontDatabase::families(ws).contains(f));
    }
    if (const QString ja = language::cjkFamily("ja"); !ja.isEmpty()) {   // ... first in the application's font
        const QFont before = QApplication::font();
        language::useFamily("ja");
        CHECK(QApplication::font().families().value(0) == ja);
        QApplication::setFont(before);
    }
}

// The binding dialog, as one binding and as Bind all's walk, in each language there is
static int shots(QApplication &app, const QString &root, const QString &out) {
    QDir().mkpath(out);
    QStringList codes = {"en"};
    codes << language::available(root);
    int saved = 0;
    for (const QString &code : codes) {
        language::install(app, code, root);
        for (const bool walk : {false, true}) {
            CaptureDialog d(walk ? "P1 Button 3  (Player 1)" : "P1 Turntable  (Player 1)", false, nullptr, walk ? "3 of 16" : "", walk);
            d.show();
            for (int k = 0; k < 10; k++) app.processEvents();
            const QString file = out + "/capture" + (walk ? "-all-" : "-") + code + ".png";
            const bool ok = d.grab().save(file);
            printf("%s: %dx%d -> %s%s\n", qPrintable(code), d.width(), d.height(), qPrintable(file), ok ? "" : " (not saved)");
            saved += ok;
            d.hide();
        }
    }
    return saved == 2 * codes.size() ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
#ifndef _WIN32   // (no display needed; a Windows build has only its own platform, which needs none to draw into a picture)
    qputenv("QT_QPA_PLATFORM", "offscreen");
#endif
    QApplication app(argc, argv);
    if (argc >= 5 && QString(argv[2]) == "--shot") return shots(app, QDir(QString::fromLocal8Bit(argv[3])).absolutePath(), QDir(QString::fromLocal8Bit(argv[4])).absolutePath());
    checks(app, QDir(QString::fromLocal8Bit(argv[1])).absolutePath());   // (a drive letter on Windows: Wine's runs)
    if (g_failed) {
        fprintf(stderr, "%d checks failed\n", g_failed);
        return 1;
    }
    printf("ok\n");
    return 0;
}
