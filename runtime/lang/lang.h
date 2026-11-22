// SPDX-License-Identifier: GPL-3.0-or-later
// The interface's languages: one catalogue per language, gettext's .po, read as it is (no compiled
// .mo, no libintl), by the launcher (launcher/language.h, through Qt's own translation hook) and by
// the game programs (here: the game menu, a game's boxes and overlay). English is the source and
// the fallback: a text with no translation, or with a broken one, shows in English, never empty.
//
//   <root>/lang/hiraku.pot     every text to translate, made by tools/lang.py from the sources
//   <root>/lang/<code>.po      a language's: fr, ja, de, es, it, pt_PT, ko, zh_CN, zh_TW
//
// A text is found by its context (msgctxt: who shows it, "menu", "CaptureDialog") and its English
// (msgid). Arguments are %1..%9 in any order (Qt's way, the same on both sides), %n a plural's
// number; plurals as the catalogue's Plural-Forms header says. Entries marked fuzzy, obsolete
// (#~) or empty are not used. Logs, command-line options, game.toml and mod keys stay English.
#pragma once

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace hk::lang {

// --- a catalogue -------------------------------------------------------------------
class Catalogue {
public:
    Catalogue();
    ~Catalogue();
    Catalogue(Catalogue &&) noexcept;
    Catalogue &operator=(Catalogue &&) noexcept;
    // A .po file's text. false: it is refused whole (`error` says why, "fr.po:12: ..."), and the
    // catalogue stays empty (everything in English). `name` is the file's, for the messages.
    bool parse(std::string_view text, std::string &error, const std::string &name = "catalogue");
    bool load(const std::string &path, std::string &error);
    // Entries kept out (a translation whose %1..%9 aren't the English's, a plural with the wrong
    // number of forms): each "fr.po:40: why". The rest of the catalogue is used.
    const std::vector<std::string> &warnings() const { return warnings_; }

    std::string language() const { return language_; }   // its header's Language:, "fr"
    int plurals() const;                                 // its header's nplurals (1 without)
    size_t size() const { return entries_.size(); }      // translated entries
    // The translation, or nullptr (the caller shows the English)
    const std::string *find(std::string_view context, std::string_view text) const;
    // A plural's form for n (the header's rule), or nullptr
    const std::string *find_plural(std::string_view context, std::string_view singular, long n) const;
    // Every translated text with this context prefix ("" all): the glyphs a font file needs
    std::string all_texts(std::string_view context_prefix = "") const;
    // The plural rule's form for n (0 for a catalogue without one)
    int plural_index(long n) const;

private:
    struct Entry {
        std::vector<std::string> forms;   // msgstr, or msgstr[0..]
        bool plural = false;
        int line = 0;   // where it is in the file
    };
public:
    struct Rule;   // (lang.cpp's: the plural rule)

private:
    std::unordered_map<std::string, Entry> entries_;   // msgctxt \x04 msgid
    std::vector<std::string> warnings_;
    std::string language_;
    std::unique_ptr<Rule> rule_;
};

// A Plural-Forms expression ("nplurals=2; plural=(n > 1);"): its form for n, or -1 if it isn't one
int plural_form(const std::string &header_value, long n);

// "%1 of %2" with its arguments put in, in the translation's order; %n is n (plurals). A % that
// isn't followed by a digit or n stays as it is.
std::string arg(std::string_view pattern, std::initializer_list<std::string_view> args, long n = 0);

// --- which language ------------------------------------------------------------------
// The catalogues a release has, and their names in their own language (the setting's list)
struct Language {
    const char *code, *name;
};
const std::vector<Language> &languages();
// "Follow the system": the saved choice ("" or "system": the system's; "en"; a code), the system's
// languages in its order of preference (POSIX "fr_CA.UTF-8", BCP 47 "zh-Hant-HK", Windows "pt-BR"),
// and the catalogues there are -> the catalogue's code, or "en". fr_CA -> fr; zh_TW, zh_HK, zh-Hant
// -> zh_TW; zh_CN, zh_SG, zh-Hans, zh -> zh_CN; pt_BR -> pt_PT (there is no Brazilian one); a system
// whose first language is English, or that has none of ours, -> en.
std::string choose(const std::string &saved, const std::vector<std::string> &system, const std::vector<std::string> &available);
// The system's languages (LANGUAGE, LC_ALL, LC_MESSAGES, LANG; Windows: the user's UI languages)
std::vector<std::string> system_languages();

// --- the program's catalogue ---------------------------------------------------------
// Once, at start: the language from HK_LANG (the launcher's choice, which it passes its games),
// else the launcher's saved one (ui/language in its launcher.ini: a game started on its own), else
// the system's; its catalogue: the player's own <user data folder>/lang/<code>.po first (a tester's
// corrected file, kept over updates), else the package's <root>/lang/<code>.po. A player's file
// that is refused is said with its line and the package's is used; none to be had: English.
// Returns the code in use.
std::string start(const std::string &root);
// The launcher's saved choice ("" follows the system): its settings file's [ui] language=
std::string saved_language(const std::string &root);
// Where a language's catalogues are looked for, in order: the player's folder, then the package's
std::vector<std::string> catalogue_dirs(const std::string &root);
// The catalogue for `code` from those folders, the first that reads; `said` gets each refusal
// ("…/fr.po:12: …") and each entry kept out. false: none could be read (English).
bool load_for(const std::string &root, const std::string &code, Catalogue &out, std::vector<std::string> &said);
// ... or a catalogue given (tests; the launcher's own: launcher/language.h)
void use(Catalogue c, const std::string &code);
const Catalogue &current();
const std::string &code();   // "en" before start

// The text in the program's language, else the English given (which outlives the call)
const char *tr(const char *context, const char *text);
// ... one with a number: the plural's form for n, %n replaced
std::string trn(const char *context, const char *singular, const char *plural, long n);

// --- a text said in a log and shown on a screen ----------------------------------------
// Code that passes a text along before it is shown (a core's notices: a floppy disk, a memory
// card) carries it as one string: its English pattern, marked where it is written with HK_N, and
// its arguments. The log gets english(), the screen shown(). A string that isn't one of these is
// taken as it is: english() gives it back, shown() translates it as a whole (tr), so a text marked
// with HK_N and passed on plain is shown translated too; so is an argument that is one (a medium's
// name, "Floppy disk", in "%1 of card %2 in the drive").
std::string later(const char *pattern, std::initializer_list<std::string_view> args);
std::string english(const std::string &s);
// keep: each argument's value marked to be kept on one line where it can (lang/fit.h's wrap)
std::string shown(const char *context, const std::string &s, bool keep = false);

}  // namespace hk::lang

// A text marked for translation where it is written (a table), translated where it is shown with
// hk::lang::tr(context, text): the extraction (tools/lang.py) finds it with its context
#define HK_N(context, text) (text)
// ... a plural's two English forms, for a call that takes them as two arguments
#define HK_NP(context, singular, plural) (singular), (plural)
