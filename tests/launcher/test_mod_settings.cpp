// SPDX-License-Identifier: GPL-3.0-or-later
// A mod's settings panel, the launcher's side (launcher/modsettings.h): the rows read from a mod.toml
// by the loader's own reader (a tick box, a list of named values, a number), their values from the
// settings file the game keeps (a number or a name, clamped as the game does), Defaults, the
// summary, the actions' keys, and the file written back with its other lines kept (the actions'
// bindings, a setting the mod no longer has, comments); a mod.toml the game would refuse. Made-up
// mods, no display.
//
//   test_mod_settings <scratch folder>      exit 0 when all hold
#include <QDir>
#include <QFile>

#include <cstdio>

#include "modsettings.h"

namespace {

int failed = 0;
void check(bool ok, const QString &what) {
    printf("%s%s\n", ok ? "ok    " : "FAIL  ", what.toLocal8Bit().constData());
    failed += !ok;
}
void put(const QString &path, const QByteArray &text) {
    QDir().mkpath(path.section('/', 0, -2));
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(text);
}
QByteArray get(const QString &path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

}  // namespace

int main(int argc, char **argv) {
    const QString root = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QDir::tempPath() + "/hk-mod-settings-test";
    QDir(root).removeRecursively();
    const QString mod = root + "/mods/timing", file = root + "/mod-settings/test/timing.txt";
    put(mod + "/mod.toml",
        "name = \"Timing\"\ngame = \"test\"\napi = 1\nrequires = [\"settings\", \"choose\", \"actions\"]\n"
        "[[setting]]\nname = \"timing\"\nlabel = \"Backing track timing\"\nchoices = [\"Arcade\", \"PS2\"]\n"
        "[[setting]]\nname = \"cover\"\nlabel = \"Lane cover\"\nmin = 0\nmax = 1\ndefault = 1\n"
        "[[setting]]\nname = \"shift\"\nlabel = \"Judging (ms)\"\nmin = -100\nmax = 100\ndefault = 0\nstep = 5\n"
        "[[action]]\nname = \"later\"\nlabel = \"Judging later\"\nbinding = \"key:Keypad 8\"\nsetting = \"shift\"\ndo = \"up\"\n"
        "[[action]]\nname = \"flip\"\nbinding = \"joy0:button3\"\nsetting = \"timing\"\ndo = \"toggle\"\n"
        "[[files]]\nfolder = \"alt/ps2\"\nwhen = \"timing=PS2\"\n");

    // nothing kept yet: the mod's defaults
    ModSettings s;
    QString err;
    check(s.load(mod, file, &err) && s.rows().size() == 3, "three settings read (" + err + ")");
    if (s.rows().size() != 3) return 1;
    check(s.rows()[0].kind == ModSettings::Kind::list && s.rows()[1].kind == ModSettings::Kind::tick &&
              s.rows()[2].kind == ModSettings::Kind::number,
          "a list (named values), a tick box (0/1), a number box");
    check(s.rows()[0].value == 0 && s.rows()[1].value == 1 && s.rows()[2].value == 0, "values at the mod's defaults");
    check(s.summary() == "Backing track timing: Arcade, Lane cover: on, Judging (ms): 0", "the summary: " + s.summary());
    const QStringList keys = s.bindings();
    check(keys.size() == 2 && keys[0] == "Keypad 8: Judging later (Judging (ms) up by 5)" &&
              keys[1] == "joy0:button3: flip (Backing track timing, Arcade / PS2)",
          "the actions' keys: " + keys.join(" | "));

    // kept: a name, a number out of range, a binding changed, lines the panel doesn't know
    put(file, "# the player's note\ntiming=ps2\nshift=250\naction.later=key:F5\nretired=4\n");
    check(s.load(mod, file, &err), "read again with a settings file");
    check(s.rows()[0].value == 1 && s.rows()[2].value == 100, "a name (\"ps2\", any case) and a number clamped (250 -> 100), as the game takes them");
    check(s.bindings().value(0).startsWith("F5: "), "the binding as the file has it: " + s.bindings().value(0));
    s.set(1, 0);
    s.set(2, -500);
    check(s.rows()[2].value == -100, "set: clamped to the setting's ends");
    check(s.save(&err), "saved (" + err + ")");
    const QByteArray text = get(file);
    check(text == "# the player's note\ntiming=1\nshift=-100\naction.later=key:F5\nretired=4\ncover=0\n",
          "written: each setting's line, the comment, the action's binding and an old setting kept:\n" + QString::fromUtf8(text));
    // read back as the game would
    ModSettings again;
    again.load(mod, file);
    check(again.rows()[0].value == 1 && again.rows()[1].value == 0 && again.rows()[2].value == -100, "read back the same");
    again.defaults();
    check(again.rows()[0].value == 0 && again.rows()[1].value == 1 && again.rows()[2].value == 0, "Defaults: every one at the mod's default");
    check(QString::fromStdString(again.written()).contains("action.later=key:F5\n"), "... the binding still kept");

    // a new file: the head line first
    QFile::remove(file);
    ModSettings fresh;
    fresh.load(mod, file);
    fresh.save();
    check(get(file).startsWith("# HIRAKU: a mod's settings") && get(file).contains("timing=0\n"), "a new settings file: the head line, then the values");

    // no settings: none; a mod.toml the game would refuse: why
    put(root + "/mods/plain/mod.toml", "name = \"Plain\"\ngame = \"test\"\n");
    ModSettings plain;
    check(plain.load(root + "/mods/plain", root + "/mod-settings/test/plain.txt") && plain.empty(), "a mod without settings: no rows");
    ModSettings files_only;
    check(files_only.load(root + "/mods/none", root + "/x.txt") && files_only.empty(), "a folder of files (no mod.toml): no rows");
    put(root + "/mods/bad/mod.toml", "name = \"Bad\"\n[[setting]]\nname = \"t\"\nchoices = [\"A\", \"B\"]\ndefault = \"C\"\n");
    ModSettings bad;
    err.clear();
    check(!bad.load(root + "/mods/bad", root + "/x.txt", &err) && err.contains("bad/mod.toml:2") && err.contains("one of 'choices'"),
          "a setting the game would refuse: refused with its line (" + err + ")");

    printf("%s\n", failed ? "FAILED" : "all ok");
    return failed ? 1 : 0;
}
