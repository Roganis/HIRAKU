# Translating HIRAKU

HIRAKU's interface (the launcher, the game menu, the boxes and the performance overlay over a game)
is translated with one catalogue per language: a gettext `.po` file in `lang/`. The launcher and the
game programs read the same file as it is, at start; nothing is compiled. Players choose the language
in Settings > General > Language (docs/PLAY.md, "Language").

    lang/hiraku.pot      every text to translate (made by tools/lang.py: never edit it by hand)
    lang/<code>.po       a language's translations: fr, ja (and de, es, it, pt_PT, ko, zh_CN, zh_TW to come)
    lang/GLOSSARY.md     the words each language keeps to, and its style

## Translating with Poedit

1. Install [Poedit](https://poedit.net) (free, Windows, macOS, Linux), or use any .po editor
   (Lokalize, GNOME Translation Editor, OmegaT) or a text editor.
2. Open `lang/<code>.po`. For a new language, ask in an issue first, or start one with
   `tools/lang.py --new CODE` (or open `lang/hiraku.pot` in Poedit and save it as `CODE.po`).
3. Translate what is empty, and check what is marked **fuzzy** (Poedit: "needs work"): its English
   changed since it was translated, and it isn't used until a translator clears the mark.
4. Try it: copy the file to your own catalogues' folder (`~/.local/share/hiraku/lang/`,
   `%LOCALAPPDATA%\hiraku\lang\`, or `user/data/lang/` in a portable install) and start HIRAKU with
   that language (the setting, or `HK_LANG=fr`). Your file is read before the package's. The
   launcher's and each game's log say `lang: fr, N texts translated`, and name with its line every
   entry left out and why.
5. Send it as a pull request on GitHub (the file under `lang/`), or attached to an issue.

## What an entry tells you

- **Context** (msgctxt): who shows the text. Launcher windows have their class's name
  (`MainWindow`, `SettingsDialog`, `CaptureDialog`...); the game side has three: `menu` (the game
  menu, from the cabinet), `boxes` (the boxes over a game: a notice, the fast start-up, a floppy
  disk or memory card), `overlay` (the performance overlay, F7). The same English in two contexts
  is two entries, and may be translated differently.
- **Notes** (`#.`, "Notes for translators" in Poedit): what the arguments are and where the text
  goes. Developers write them as `// TR:` comments beside the text in the code.
- **References** (`#:`): the source file that shows the text.

## Arguments, plurals, markup

- `%1`..`%9` are put in by the program (a game's title, a number, a path). Keep each one; put them
  in any order the language needs ("%2 中 %1"). A translation whose arguments aren't the English's
  is left out (the English is shown) and said in the log.
- `%n` is a plural's number. A plural entry has the forms the file's header asks for (its
  `Plural-Forms`: French two, 0 and 1 singular; Japanese, Korean, Chinese one). A form may leave the
  number out where the language says it in words ("un jeu").
- `&` marks the letter for Alt in a menu or button: keep one (Japanese and Chinese: `ゲーム(&G)`).
- HTML tags (`<b>`, `<p>`, `<tt>`, `&gt;`) stay as they are, the text between them translated.
- `…` ends a menu item that opens a window: keep it.

## The game side's limits

The game programs draw their own text in a font the launcher makes for them from the catalogue's
characters. A box over a game breaks its lines itself (between words, or between any two Japanese
or Chinese characters), up to about 60 % of the screen's width; the overlay cuts a line that is
too long with "..". Keep these texts short. The overlay writes numbers with "." in every language.
Without the launcher (a game started by hand, with no font from it) the boxes and the overlay stay
in English.

## What stays English

Logs, the exit report, `--help` and option names, `game.toml` and mod keys, anything written to a
file; the licence notice in About (legal text); control names such as "P1 Button 3" (they are the
bindings' keys); a mod's own texts (its author's); the games themselves.

## For developers: marking a text

    tr("text")                          a launcher class's text (Q_OBJECT, or Q_DECLARE_TR_FUNCTIONS(Class))
    language::tr("Ctx", "text")         a free function's (launcher/language.h)
    language::trn("Ctx", "%n game", "%n games", n)
    QT_TRANSLATE_NOOP("Ctx", "text")    a table's text, translated where it is shown
    hk::lang::tr("menu", "text")        the game side (runtime/lang/lang.h), and lang::trn, lang::arg
    HK_N("boxes", "text")               a table's text on the game side; HK_NP("ctx", "one", "many") a plural's
    lang::later(HK_N("boxes", "Memory card %1 in its slot"), {n})
                                        a text passed along before it is shown: lang::english() for the
                                        log, lang::shown() for the screen

Then run `tools/lang.py`: `lang/hiraku.pot` and every `lang/*.po` are brought up to date (a new
text comes in empty, a changed one keeps its old translation marked fuzzy, a removed one goes to
the end as obsolete). `tools/lang.py --stats` says how far each language is; `tools/lang.py --check`
(the unit test `lang_extracted`) fails when the catalogues aren't what the sources make, and the
unit test `lang_catalogues` reads every catalogue. `msgfmt -c lang/fr.po` (gettext) checks a file
the way translation tools do.

The words to use, language by language: [lang/GLOSSARY.md](../lang/GLOSSARY.md).
