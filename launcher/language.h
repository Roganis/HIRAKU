// SPDX-License-Identifier: GPL-3.0-or-later
// The launcher's language (runtime/lang/lang.h: one .po catalogue per language, shared with the
// game programs). It answers through Qt's own translation hook, a QTranslator installed on the
// application, so QCoreApplication::translate and QObject::tr find the launcher's texts, and Qt's
// own (its standard buttons, a line edit's menu: contexts QPlatformTheme, QLineEdit, ...) come
// from the same catalogue first, then from Qt's qtbase_<code>.qm where the Qt in use has them.
//
// The setting (Settings > General > Language, saved as ui/language: "" follows the system) is read
// at start; a change takes effect at the next start. HK_LANG overrides it (a test, a translator
// trying a catalogue), and is what the launcher hands its games.
#pragma once

#include <QCoreApplication>
#include <QString>
#include <QTranslator>

#include "lang/lang.h"

namespace language {

// The texts of a context, in the launcher's language: Qt's own hook (the catalogue's translator)
inline QString tr(const char *context, const char *text) { return QCoreApplication::translate(context, text); }
// ... with a number: the plural's form for n, %n replaced (English's own rule where untranslated)
QString trn(const char *context, const char *singular, const char *plural, long n);

// The catalogues' folder (<root>/lang) and the codes it has
QString dir(const QString &root);
QStringList available(const QString &root);
// The system's languages, in its order (Qt's view: QLocale::system().uiLanguages())
QStringList systemLanguages();
// The code to use: HK_LANG, else the saved choice, else the system's (lang::choose)
QString choose(const QString &saved, const QString &root);

// Read the catalogue for `code` (the player's <user data folder>/lang/<code>.po first, else the
// package's <root>/lang) and install it, and Qt's own where there is one.
// The code in use ("en" if the catalogue is missing or refused: said on stderr).
QString install(QCoreApplication &app, const QString &code, const QString &root);
// The code in use: HK_LANG for the games
QString current();

// Japanese, Korean, Simplified and Traditional Chinese: the family this PC has made for the
// language (Qt's writing-system lookup, the language's own UI fonts first), "" for the others or
// none. The same Han characters are drawn differently in each: the language's own goes first.
QString cjkFamily(const QString &code);
// ... put first in the application's font (the launcher's windows; the game menu's and the boxes'
// font files are drawn from it): a QGuiApplication's, after install()
void useFamily(const QString &code);

// Every translated text of the game programs' contexts (the game menu, a game's boxes, its
// overlay): the characters the font files handed to them need (launcher/menufont.h)
QString gameTexts();

// Qt's hook: the launcher's catalogue answering QCoreApplication::translate. Qt's (context,
// disambiguation) is the catalogue's msgctxt "context" or "context|disambiguation"; a call with
// n >= 0 takes the plural's form for n (Qt's own %n: Qt puts the number in)
class Translator : public QTranslator {
public:
    QString translate(const char *context, const char *sourceText, const char *disambiguation = nullptr, int n = -1) const override;
    bool isEmpty() const override;
};

}  // namespace language
