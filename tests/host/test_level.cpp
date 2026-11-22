// SPDX-License-Identifier: GPL-3.0-or-later
// The cores' base volume evened out (runtime/shell/level.h): the families' trims, a game.toml's own
// [sound] level_db and what of it can't be used, --even-volume off, and a family the table hasn't
// (0 dB); then every game.toml in games/ (argv[1]) has a trim of the table's or is one of the known
// ones without.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

#include "shell/level.h"

namespace {

int failures = 0;
void check(bool ok, const std::string &what) {
    if (!ok) failures++, std::printf("FAIL: %s\n", what.c_str());
}
bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

std::string toml(const char *core, const char *family, const char *rest = "") {
    std::string t = "# a game\nid = \"x\"\n";
    if (*core) t += std::string("core = \"") + core + "\"   # its core\n";
    t += std::string("family = \"") + family + "\"\n";
    return t + "\n[roms]\nbios = \"a.bin\"\n" + rest;
}

double trim(const std::string &text, bool on = true) {
    std::string err;
    return hk::level::for_game(text, on, err).db;
}

}  // namespace

int main(int argc, char **argv) {
    using hk::level::family_trim;
    // the table
    check(near(family_trim("sys573", "ddr").db, -5) && near(family_trim("sys573", "gtrf").db, -5), "System 573 -5");
    check(near(family_trim("twinkle", "iidx").db, -4), "Twinkle -4");
    check(near(family_trim("pc", "iidx").db, -4), "PC IIDX -4");
    check(near(family_trim("pc", "pcpopn").db, -1), "PC pop'n -1");
    check(near(family_trim("pc", "pcpopnhc").db, 0) && family_trim("pc", "pcpopnhc").known, "64-bit pop'n 0, known");
    check(near(family_trim("", "bm3").db, 0) && family_trim("", "kbm").known && family_trim("", "ppp").known, "Firebeat 0");
    check(near(family_trim("python", "pypopn").db, 1) && near(family_trim("python", "drmn").db, 1), "Python +1");
    check(near(family_trim("djmain", "djbm").db, 3) && near(family_trim("djmain", "djpopn").db, 3), "DJ Main +3");
    // a family the table hasn't: 0 dB, unknown
    check(near(family_trim("pc", "eemall").db, 0) && !family_trim("pc", "eemall").known, "ee'MALL: none");
    check(!family_trim("", "iidx").known && !family_trim("", "").known, "no core and not Firebeat's: none");
    check(!family_trim("newcore", "ddr").known && near(family_trim("newcore", "ddr").db, 0), "an unknown core: 0");
    check(trim("") == 0, "no game.toml (the game menu): 0");

    // game.toml's top: core and family read, comments and later tables' keys not
    check(hk::level::top_value(toml("sys573", "ddr"), "core") == "sys573", "top_value core");
    check(hk::level::top_value(toml("", "bm3"), "core").empty(), "top_value: none");
    check(hk::level::top_value("[x]\ncore = \"pc\"\n", "core").empty(), "top_value: a table's key isn't the top's");
    check(near(trim(toml("sys573", "ddr")), -5), "a 573 game.toml");
    {
        std::string err;
        check(near(trim(toml("", "popn")), 0) && hk::level::for_game(toml("", "popn"), true, err).what == "Firebeat",
              "a Firebeat game.toml (no core key)");
    }
    check(near(trim(toml("djmain", "djpopn")), 3), "a DJ Main game.toml");

    // --even-volume off: none, whatever the family or the game's own
    check(near(trim(toml("sys573", "ddr"), false), 0), "off: 573 0");
    check(near(trim(toml("djmain", "djbm", "[sound]\nlevel_db = 5\n"), false), 0), "off: own level not used");

    // the game's own [sound] level_db replaces its family's
    {
        std::string err;
        hk::level::Trim t = hk::level::for_game(toml("", "ppp", "\n[sound]\nlevel_db = -2.5   # measured\n"), true, err);
        check(near(t.db, -2.5) && t.own && t.known && err.empty(), "own level_db -2.5");
        t = hk::level::for_game(toml("pc", "eemall", "[sound]\nlevel_db=+4\n"), true, err);
        check(near(t.db, 4) && t.own && t.what == "this game", "own level_db in an unknown family");
    }
    // what can't be used: said, and the family's trim stays
    auto bad = [&](const char *rest, const char *what) {
        std::string err;
        const hk::level::Trim t = hk::level::for_game(toml("sys573", "ddr", rest), true, err);
        check(near(t.db, -5) && !t.own && !err.empty(), std::string("bad [sound]: ") + what);
        if (!err.empty()) std::printf("  (%s: %s)\n", what, err.c_str());
    };
    bad("[sound]\nlevel = 3\n", "unknown key");
    bad("[sound]\nlevel_db = loud\n", "not a number");
    bad("[sound]\nlevel_db = \"3\"\n", "a text");
    bad("[sound]\nlevel_db = 30\n", "too far");
    bad("[sound]\nlevel_db\n", "no value");
    bad("[sound]\nlevel_db = nan\n", "nan");
    {   // a [sound] elsewhere in the file: other tables' keys aren't its own
        std::string err;
        double db = 9;
        bool has = true;
        check(hk::level::parse_override("[sound]\n[smooth]\nwhile_changing = 0x10\n", db, has, err) && !has && err.empty(),
              "an empty [sound], then another table");
        check(hk::level::parse_override("level_db = 3\n[roms]\nlevel_db = 3\n", db, has, err) && !has, "level_db outside [sound]");
    }

    // every game in games/: a family of the table's, else one of those known to have none
    if (argc > 1) {
        const std::set<std::string> without = {"eemall"};
        int n = 0;
        for (const auto &e : std::filesystem::directory_iterator(argv[1])) {
            const std::filesystem::path p = e.path() / "game.toml";
            if (!std::filesystem::exists(p)) continue;
            std::ifstream in(p);
            std::stringstream ss;
            ss << in.rdbuf();
            std::string err;
            const hk::level::Trim t = hk::level::for_game(ss.str(), true, err);
            const std::string id = e.path().filename().string();
            check(err.empty(), id + ": its [sound] " + err);
            check(t.known || without.count(id), id + ": no family's trim");
            n++;
        }
        check(n > 100, "games/ read");
        std::printf("%d games' game.toml read\n", n);
    }
    std::printf(failures ? "%d failed\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
