// SPDX-License-Identifier: GPL-3.0-or-later
// The interface's catalogues (runtime/lang/lang.h): a .po read, a text found by its context, the
// English kept for what isn't translated, the plural rules of the languages HIRAKU is translated
// into, arguments in another order, a broken file refused with its line, UTF-8; which catalogue
// "follow the system" takes; and the repository's own catalogues (DIR: its lang/) each read clean.
//
//   test_lang [LANG_DIR [SCRATCH_DIR]]   (SCRATCH_DIR: made and removed, for the catalogues' folders)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "lang/fit.h"
#include "lang/lang.h"
#include "shell/userdir.h"

using namespace hk;

static int g_failed = 0;
#define CHECK(x) \
    do { \
        if (!(x)) g_failed++, fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
    } while (0)

static const char *kFrench = R"(# A test catalogue
msgid ""
msgstr ""
"Language: fr\n"
"Content-Type: text/plain; charset=UTF-8\n"
"Plural-Forms: nplurals=2; plural=(n > 1);\n"

#: runtime/menu/nav.cpp
msgctxt "menu"
msgid "play"
msgstr "jouer"

# the same English elsewhere, said otherwise
msgctxt "CaptureDialog"
msgid "play"
msgstr "lancer"

msgctxt "menu"
msgid "Starting %1"
msgstr "Lancement de %1"

msgctxt "test"
msgid "%1 of %2"
msgstr "%2 : n° %1"

msgctxt "test"
msgid "%n game"
msgid_plural "%n games"
msgstr[0] "%n jeu"
msgstr[1] "%n jeux"

msgctxt "test"
msgid "one line, "
"two pieces"
msgstr "une ligne, "
"en deux morceaux\n"

#, fuzzy
msgctxt "test"
msgid "change shelf"
msgstr "changer d'étagère"

msgctxt "test"
msgid "not yet"
msgstr ""

# a translator's slip: its %1 is gone
msgctxt "test"
msgid "Recording kept: %1"
msgstr "Enregistrement gardé"

#~ msgctxt "test"
#~ msgid "an old text"
#~ msgstr "un vieux texte"
)";

static void test_lookup() {
    lang::Catalogue c;
    std::string err;
    CHECK(c.parse(kFrench, err, "fr.po"));
    if (!err.empty()) fprintf(stderr, "%s\n", err.c_str());
    CHECK(c.language() == "fr");
    CHECK(c.plurals() == 2);
    const std::string *t = c.find("menu", "play");
    CHECK(t && *t == "jouer");
    t = c.find("CaptureDialog", "play");   // (by its context)
    CHECK(t && *t == "lancer");
    CHECK(!c.find("nowhere", "play"));
    CHECK(!c.find("test", "change shelf"));   // fuzzy: not used
    CHECK(!c.find("test", "not yet"));        // empty: not translated
    CHECK(!c.find("test", "an old text"));    // obsolete
    t = c.find("test", "one line, two pieces");
    CHECK(t && *t == "une ligne, en deux morceaux\n");
    // the slip kept out, with its line, and the rest used
    CHECK(!c.find("test", "Recording kept: %1"));
    for (const std::string &w : c.warnings()) printf("  (as it should: %s)\n", w.c_str());
    CHECK(c.warnings().size() == 1 && c.warnings()[0].rfind("fr.po:48: ", 0) == 0);
    CHECK(c.size() == 6);

    // the program's: English before a catalogue is used, and for what it hasn't
    CHECK(!strcmp(lang::tr("menu", "play"), "play"));
    CHECK(lang::code() == "en");
    lang::use(std::move(c), "fr");
    CHECK(lang::code() == "fr");
    CHECK(!strcmp(lang::tr("menu", "play"), "jouer"));
    const char *english = "No game is ready to play";
    CHECK(lang::tr("menu", english) == english);   // (the English itself, never empty, never a key)
    CHECK(lang::arg(lang::tr("menu", "Starting %1"), {"beatmania IIDX 16 EMPRESS"}) == "Lancement de beatmania IIDX 16 EMPRESS");
    // another order
    CHECK(lang::arg(lang::tr("test", "%1 of %2"), {"3", "12"}) == "12 : n\xc2\xb0 3");
    CHECK(lang::arg("%1 of %2", {"3", "12"}) == "3 of 12");
    // plurals: French's 0 and 1 are singular
    CHECK(lang::trn("test", "%n game", "%n games", 0) == "0 jeu");
    CHECK(lang::trn("test", "%n game", "%n games", 1) == "1 jeu");
    CHECK(lang::trn("test", "%n game", "%n games", 2) == "2 jeux");
    // an untranslated plural: English's own rule
    CHECK(lang::trn("test", "%n file", "%n files", 1) == "1 file");
    CHECK(lang::trn("test", "%n file", "%n files", 0) == "0 files");
    lang::use(lang::Catalogue(), "en");
    // a % that isn't an argument stays; an argument not given stays as it is
    CHECK(lang::arg("100% of %1, %3", {"it"}) == "100% of it, %3");
}

// The plural rules of the languages HIRAKU is to be translated into (their .po headers, as gettext
// and CLDR give them), and a harder one to check the reader follows any
static void test_plurals() {
    struct Case {
        const char *rule;
        long n;
        int form;
    } cases[] = {
        // French: 0 and 1 singular
        {"nplurals=2; plural=(n > 1);", 0, 0},
        {"nplurals=2; plural=(n > 1);", 1, 0},
        {"nplurals=2; plural=(n > 1);", 2, 1},
        // German, Spanish, Italian, Portuguese (Portugal): 1 singular, 0 plural
        {"nplurals=2; plural=(n != 1);", 0, 1},
        {"nplurals=2; plural=(n != 1);", 1, 0},
        {"nplurals=2; plural=(n != 1);", 21, 1},
        // Japanese, Korean, Chinese: one form
        {"nplurals=1; plural=0;", 0, 0},
        {"nplurals=1; plural=0;", 1, 0},
        {"nplurals=1; plural=0;", 1000, 0},
        // Polish (not planned; a rule with ?:, %, && and ||)
        {"nplurals=3; plural=(n==1 ? 0 : n%10>=2 && n%10<=4 && (n%100<10 || n%100>=20) ? 1 : 2);", 1, 0},
        {"nplurals=3; plural=(n==1 ? 0 : n%10>=2 && n%10<=4 && (n%100<10 || n%100>=20) ? 1 : 2);", 22, 1},
        {"nplurals=3; plural=(n==1 ? 0 : n%10>=2 && n%10<=4 && (n%100<10 || n%100>=20) ? 1 : 2);", 12, 2},
        {"nplurals=3; plural=(n==1 ? 0 : n%10>=2 && n%10<=4 && (n%100<10 || n%100>=20) ? 1 : 2);", 25, 2},
        {"nplurals=2; plural=!(n == 1);", 1, 0},
        {"nplurals=2; plural=n>1;", 5, 1},   // (no brackets, no last ';')
    };
    for (const Case &k : cases) {
        const int got = lang::plural_form(k.rule, k.n);
        if (got != k.form) fprintf(stderr, "  %s n=%ld: %d, not %d\n", k.rule, k.n, got, k.form);
        CHECK(got == k.form);
    }
    CHECK(lang::plural_form("nplurals=2; plural=(n >;", 1) == -1);
    CHECK(lang::plural_form("plural=n;", 1) == -1);
    CHECK(lang::plural_form("nplurals=2; plural=(n > 1) ? 1 : ;", 1) == -1);
}

// A broken file: refused whole, with its line; nothing of it used
static void test_broken() {
    const std::string head = "msgid \"\"\nmsgstr \"\"\n\"Content-Type: text/plain; charset=UTF-8\\n\"\n\n";
    struct Case {
        std::string text;
        int line;
    } cases[] = {
        {head + "msgid \"play\"\nmsgstr \"jouer\n", 6},             // a string not closed
        {head + "msgid \"play\"\n", 5},                             // no msgstr (at the end: the entry's line)
        {head + "msgid \"play\"\nmsgstr \"a\" b\n", 6},             // something after
        {head + "msgid \"play\"\nmsgtext \"jouer\"\n", 6},          // a word it doesn't know
        {head + "msgid \"play\"\nmsgstr \"jou\\qer\"\n", 6},        // an escape it doesn't know
        {head + "msgid \"play\"\nmsgstr \"jou\xc3\"\n", 6},          // not UTF-8
        {head + "\"loose\"\n", 5},                                  // a string with no keyword (after a blank line)
        {head + "msgid \"a\"\nmsgstr \"b\"\n\nmsgid \"a\"\nmsgstr \"c\"\n", 8},   // twice
        {"msgid \"\"\nmsgstr \"\"\n\"Content-Type: text/plain; charset=ISO-8859-1\\n\"\n", 1},
        {"msgid \"play\"\nmsgstr \"jouer\"\n", 1},                  // no header
        {"msgid \"\"\nmsgstr \"\"\n\"Plural-Forms: nplurals=2; plural=(n >;\\n\"\n", 1},
        {head + "msgid \"%n game\"\nmsgid_plural \"%n games\"\nmsgstr \"jeux\"\n", 7},   // a plural with one msgstr
    };
    for (const Case &k : cases) {
        lang::Catalogue c;
        std::string err;
        const bool ok = c.parse(k.text, err, "xx.po");
        const std::string want = "xx.po:" + std::to_string(k.line) + ":";
        if (ok || err.rfind(want, 0) != 0) fprintf(stderr, "  expected %s, got %s\n", want.c_str(), ok ? "(read)" : err.c_str());
        CHECK(!ok && err.rfind(want, 0) == 0);
        CHECK(c.size() == 0);
    }
    // a plural with the wrong number of forms: that entry kept out, the file read
    lang::Catalogue c;
    std::string err;
    CHECK(c.parse("msgid \"\"\nmsgstr \"\"\n\"Plural-Forms: nplurals=1; plural=0;\\n\"\n\nmsgid \"%n game\"\nmsgid_plural \"%n games\"\n"
                  "msgstr[0] \"%n\"\nmsgstr[1] \"%n\"\n\nmsgid \"x\"\nmsgstr \"y\"\n",
                  err, "ja.po"));
    CHECK(c.size() == 1 && c.warnings().size() == 1);
    // a file that isn't there: English, said
    CHECK(!c.load("/nonexistent/hiraku/xx.po", err) && c.size() == 0 && !err.empty());
    // a BOM and Windows line ends are read
    CHECK(c.parse("\xef\xbb\xbfmsgid \"\"\r\nmsgstr \"\"\r\n\r\nmsgctxt \"m\"\r\nmsgid \"a\"\r\nmsgstr \"\xe3\x81\x82\"\r\n", err));
    CHECK(c.find("m", "a") && *c.find("m", "a") == "\xe3\x81\x82");
}

// Japanese, one plural form; UTF-8 kept whole; the glyphs a font needs
static void test_japanese() {
    lang::Catalogue c;
    std::string err;
    CHECK(c.parse("msgid \"\"\nmsgstr \"\"\n\"Language: ja\\n\"\n\"Content-Type: text/plain; charset=UTF-8\\n\"\n"
                  "\"Plural-Forms: nplurals=1; plural=0;\\n\"\n\n"
                  "msgctxt \"menu\"\nmsgid \"play\"\nmsgstr \"\xe3\x83\x97\xe3\x83\xac\xe3\x82\xa4\"\n\n"
                  "msgctxt \"test\"\nmsgid \"%n game\"\nmsgid_plural \"%n games\"\nmsgstr[0] \"%n\xe4\xbd\x9c\xe5\x93\x81\"\n\n"
                  "msgctxt \"test\"\nmsgid \"%1 of %2\"\nmsgstr \"%2\xe4\xb8\xad%1\"\n",
                  err, "ja.po"));
    CHECK(c.language() == "ja" && c.plurals() == 1 && c.size() == 3);
    lang::use(std::move(c), "ja");
    CHECK(!strcmp(lang::tr("menu", "play"), "\xe3\x83\x97\xe3\x83\xac\xe3\x82\xa4"));
    CHECK(lang::trn("test", "%n game", "%n games", 1) == "1\xe4\xbd\x9c\xe5\x93\x81");
    CHECK(lang::trn("test", "%n game", "%n games", 7) == "7\xe4\xbd\x9c\xe5\x93\x81");
    CHECK(lang::arg(lang::tr("test", "%1 of %2"), {"3", "12"}) == "12\xe4\xb8\xad" "3");
    CHECK(lang::current().all_texts("menu").find("\xe3\x83\x97") != std::string::npos);
    CHECK(lang::current().all_texts("menu").find("\xe4\xbd\x9c") == std::string::npos);   // (another context's)
    lang::use(lang::Catalogue(), "en");
}

// "Follow the system": a system's languages -> a catalogue
static void test_choose() {
    const std::vector<std::string> all = {"fr", "de", "es", "it", "pt_PT", "ja", "ko", "zh_CN", "zh_TW"};
    struct Case {
        const char *saved;
        std::vector<std::string> system;
        const char *want;
    } cases[] = {
        {"", {"fr_FR.UTF-8"}, "fr"},
        {"", {"fr_CA.UTF-8"}, "fr"},
        {"", {"fr-CA", "fr", "en-US"}, "fr"},      // (Qt's uiLanguages, Windows' list)
        {"", {"de_CH.UTF-8@euro"}, "de"},
        {"", {"ja_JP.UTF-8"}, "ja"},
        {"", {"ja-JP"}, "ja"},
        {"", {"ko_KR.UTF-8"}, "ko"},
        {"", {"zh_TW.UTF-8"}, "zh_TW"},
        {"", {"zh_HK.UTF-8"}, "zh_TW"},
        {"", {"zh-Hant-HK"}, "zh_TW"},
        {"", {"zh_MO"}, "zh_TW"},
        {"", {"zh_CN.UTF-8"}, "zh_CN"},
        {"", {"zh_SG.UTF-8"}, "zh_CN"},
        {"", {"zh-Hans-SG"}, "zh_CN"},
        {"", {"zh"}, "zh_CN"},
        {"", {"pt_PT.UTF-8"}, "pt_PT"},
        {"", {"pt_BR.UTF-8"}, "pt_PT"},           // (no Brazilian catalogue: Portugal's)
        {"", {"pt-BR"}, "pt_PT"},
        {"", {"en_US.UTF-8"}, "en"},
        {"", {"en-GB", "fr-FR"}, "en"},           // (English first: the source)
        {"", {"nl_NL.UTF-8", "de_DE.UTF-8"}, "de"},   // (the first that has one)
        {"", {"nl_NL.UTF-8"}, "en"},
        {"", {"C"}, "en"},
        {"", {"POSIX"}, "en"},
        {"", {}, "en"},
        {"system", {"it_IT.UTF-8"}, "it"},
        {"ja", {"fr_FR.UTF-8"}, "ja"},            // the player's choice over the system's
        {"en", {"fr_FR.UTF-8"}, "en"},
        {"ko", {"fr_FR.UTF-8"}, "ko"},
    };
    for (const Case &k : cases) {
        const std::string got = lang::choose(k.saved, k.system, all);
        if (got != k.want) fprintf(stderr, "  %s %s: %s, not %s\n", k.saved, k.system.empty() ? "-" : k.system[0].c_str(), got.c_str(), k.want);
        CHECK(got == k.want);
    }
    // a catalogue that isn't there: the next of the system's, else English
    CHECK(lang::choose("", {"ja_JP.UTF-8", "fr_FR.UTF-8"}, {"fr"}) == "fr");
    CHECK(lang::choose("ja", {"fr_FR.UTF-8"}, {"fr"}) == "en");
    CHECK(lang::choose("", {"zh_TW.UTF-8"}, {"zh_CN"}) == "en");   // (never the other Chinese)
    // the list's names, each in its own language
    CHECK(lang::languages().size() == 10 && !strcmp(lang::languages()[1].name, "Fran\xc3\xa7" "ais"));
}

// Lines to a width and a text cut short (lang/fit.h): ASCII one unit wide, Japanese two
static int cw(uint32_t c) { return c < 0x80 ? 1 : lang::breaks_around(c) ? 2 : 1; }
static std::string join(const std::vector<std::string> &v) {
    std::string out;
    for (const std::string &l : v) out += (out.empty() ? "" : "|") + l;
    return out;
}
static void test_fit() {
    // at spaces, the spaces gone; a word wider than the line between its characters
    CHECK(join(lang::wrap("the sound device stopped", 10, cw)) == "the sound|device|stopped");
    CHECK(join(lang::wrap("abcdefghijkl xy", 5, cw)) == "abcde|fghij|kl xy");
    CHECK(join(lang::wrap("short", 10, cw)) == "short");
    CHECK(lang::wrap("", 10, cw).empty());
    CHECK(join(lang::wrap("one\ntwo", 10, cw)) == "one|two");
    // Japanese: between any two characters, but not before 。、」 nor after 「
    CHECK(join(lang::wrap("\xe3\x81\x82\xe3\x81\x84\xe3\x81\x86\xe3\x81\x88", 4, cw)) == "\xe3\x81\x82\xe3\x81\x84|\xe3\x81\x86\xe3\x81\x88");   // あい|うえ
    // あいう。 at 6: the 。 doesn't start a line, う goes down with it
    CHECK(join(lang::wrap("\xe3\x81\x82\xe3\x81\x84\xe3\x81\x86\xe3\x80\x82", 6, cw)) == "\xe3\x81\x82\xe3\x81\x84|\xe3\x81\x86\xe3\x80\x82");
    // あ「いう」 at 4: 「 stays with い
    CHECK(join(lang::wrap("\xe3\x81\x82\xe3\x80\x8c\xe3\x81\x84\xe3\x81\x86\xe3\x80\x8d", 4, cw)) == "\xe3\x81\x82|\xe3\x80\x8c\xe3\x81\x84|\xe3\x81\x86\xe3\x80\x8d");
    // Latin after Japanese: a break between them, none inside the word
    CHECK(join(lang::wrap("\xe3\x81\x82\xe3\x81\x84" "abc", 5, cw)) == "\xe3\x81\x82\xe3\x81\x84|abc");
    // a value kept whole: its spaces aren't breaks, a break before it is taken instead
    CHECK(join(lang::wrap("Saved in " + lang::keep("my saves folder") + " now", 20, cw)) == "Saved in|my saves folder now");
    CHECK(join(lang::wrap("ab " + lang::keep("cd ef gh") + " ij", 8, cw)) == "ab|cd ef gh|ij");
    // ... unless it alone is wider than the line
    CHECK(join(lang::wrap(lang::keep("abcdefgh"), 5, cw)) == "abcde|fgh");
    // Japanese inside a value kept whole: no break there
    // (あい + うえ kept at 6: あいう|え without it)
    CHECK(join(lang::wrap("\xe3\x81\x82\xe3\x81\x84" + lang::keep("\xe3\x81\x86\xe3\x81\x88"), 6, cw)) == "\xe3\x81\x82\xe3\x81\x84|\xe3\x81\x86\xe3\x81\x88");
    CHECK(join(lang::wrap("\xe3\x81\x82\xe3\x81\x84\xe3\x81\x86\xe3\x81\x88", 6, cw)) == "\xe3\x81\x82\xe3\x81\x84\xe3\x81\x86|\xe3\x81\x88");
    // no line wider than asked, whatever the text
    for (const char *t : {"Floppy disk taken out while the game was writing it", "\xe3\x83\x95\xe3\x83\xad\xe3\x83\x83\xe3\x83\x94\xe3\x83\xbc\xe3\x82\x92\xe5\x8f\x96\xe3\x82\x8a\xe5\x87\xba\xe3\x81\x97\xe3\x81\xbe\xe3\x81\x97\xe3\x81\x9f"})
        for (int wd = 2; wd < 30; wd++)
            for (const std::string &l : lang::wrap(t, wd, cw)) CHECK(lang::width(l, cw) <= wd);
    // cut short by whole characters, never inside a UTF-8 sequence
    CHECK(lang::clip("abcdefgh", 8, cw) == "abcdefgh");
    CHECK(lang::clip("abcdefgh", 6, cw) == "abcd..");
    CHECK(lang::clip("ab cdefgh", 5, cw) == "ab..");   // (the space before the dots goes)
    const std::string ja = "\xe3\x83\x95\xe3\x83\xac\xe3\x83\xbc\xe3\x83\xa0";   // フレーム: 8 wide
    CHECK(lang::clip(ja, 8, cw) == ja);
    CHECK(lang::clip(ja, 7, cw) == "\xe3\x83\x95\xe3\x83\xac..");   // (2 + 2 + 2 dots: a third wouldn't fit)
    CHECK(lang::clip("caf\xc3\xa9 au lait", 6, cw) == "caf\xc3\xa9..");
    for (int wd = 0; wd < 10; wd++) {
        const std::string c = lang::clip(ja + "abc", wd, cw);
        for (const lang::Char &ch : lang::chars(c)) CHECK(ch.c != 0xfffd);   // (whole characters only)
    }
    CHECK(lang::unmarked(lang::keep("x")) == "x");
}

// A text said in the log in English and shown translated (a core's notice), and the catalogue's
// folders: the player's own before the package's, a broken one of the player's passed over
static void put_env(const char *k, const std::string &v) {
#ifdef _WIN32
    _putenv_s(k, v.c_str());
#else
    if (v.empty()) unsetenv(k);
    else setenv(k, v.c_str(), 1);
#endif
}
static void test_later(const std::string &scratch) {
    lang::Catalogue c;
    std::string err;
    CHECK(c.parse(kFrench, err));
    lang::use(std::move(c), "fr");
    const std::string said = lang::later(HK_N("menu", "Starting %1"), {"a game"});
    CHECK(lang::english(said) == "Starting a game");
    CHECK(lang::shown("menu", said) == "Lancement de a game");
    CHECK(lang::shown("menu", said, true) == "Lancement de " + lang::keep("a game"));
    CHECK(lang::shown("menu", "play") == "jouer");                 // (a plain text: as a whole)
    CHECK(lang::english("play") == "play");
    CHECK(lang::shown("menu", "not in the catalogue") == "not in the catalogue");
    lang::use(lang::Catalogue(), "en");
    CHECK(lang::shown("menu", said) == "Starting a game");
    if (scratch.empty()) return;
    // a made-up HIRAKU folder and user folders
    namespace fs = std::filesystem;
    const std::string root = scratch + "/root", data = scratch + "/data";
    fs::remove_all(scratch);
    put_env("XDG_DATA_HOME", data), put_env("XDG_CONFIG_HOME", scratch + "/config"), put_env("LOCALAPPDATA", data);
    const std::string ini = hk::user_config_dir(root) + "/launcher.ini";
    fs::create_directories(root + "/lang"), fs::create_directories(data + "/hiraku/lang"), fs::create_directories(hk::user_config_dir(root));
    auto write = [](const std::string &path, const std::string &text) { std::ofstream(path, std::ios::binary) << text; };
    std::string mine = kFrench;
    mine.replace(mine.find("msgstr \"jouer\""), 14, "msgstr \"JOUER\"");
    write(root + "/lang/fr.po", kFrench);
    write(data + "/hiraku/lang/fr.po", mine);
    put_env("HK_LANG", "fr");
    CHECK(lang::catalogue_dirs(root).size() == 2 && lang::catalogue_dirs(root)[1] == root + "/lang");
    CHECK(lang::start(root) == "fr" && std::string(lang::tr("menu", "play")) == "JOUER");   // the player's own first
    write(data + "/hiraku/lang/fr.po", "msgid \"broken");
    CHECK(lang::start(root) == "fr" && std::string(lang::tr("menu", "play")) == "jouer");   // ... refused: the package's
    // a game started on its own: the launcher's saved language
    put_env("HK_LANG", "");
    write(ini, "[paths]\nroot=/x\n\n[ui]\nlanguage=fr\n");
    CHECK(lang::saved_language(root) == "fr");
    put_env("LANGUAGE", ""), put_env("LC_ALL", "en_US.UTF-8");
    CHECK(lang::start(root) == "fr");
    write(ini, "[ui]\nlanguage=en\n");
    CHECK(lang::start(root) == "en");
    write(ini, "[ui]\nlanguage=\n");   // (follows the system: English here)
    CHECK(lang::saved_language(root).empty() && lang::start(root) == "en");
    lang::use(lang::Catalogue(), "en");
    fs::remove_all(scratch);
}

// The repository's catalogues: each reads clean, with nothing kept out
static void test_files(const std::string &dir) {
    for (const lang::Language &l : lang::languages()) {
        if (!strcmp(l.code, "en")) continue;
        const std::string path = dir + "/" + l.code + ".po";
        if (FILE *f = fopen(path.c_str(), "rb")) fclose(f);
        else continue;   // (not translated yet)
        lang::Catalogue c;
        std::string err;
        CHECK(c.load(path, err));
        if (!err.empty()) fprintf(stderr, "  %s\n", err.c_str());
        for (const std::string &w : c.warnings()) fprintf(stderr, "  %s\n", w.c_str());
        CHECK(c.warnings().empty());
        CHECK(c.language() == l.code);
        printf("%s: %zu texts\n", path.c_str(), c.size());
    }
}

int main(int argc, char **argv) {
    test_lookup();
    test_plurals();
    test_broken();
    test_japanese();
    test_choose();
    test_fit();
    test_later(argc > 2 ? argv[2] : "");
    if (argc > 1) test_files(argv[1]);
    if (g_failed) {
        fprintf(stderr, "%d checks failed\n", g_failed);
        return 1;
    }
    printf("ok\n");
    return 0;
}
