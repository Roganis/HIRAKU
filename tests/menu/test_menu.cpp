// SPDX-License-Identifier: GPL-3.0-or-later
// The game menu (runtime/menu/) and the way out to it (runtime/host/menukey.h):
//   - the hold: 5, 7 and 3 of one 10-key (not across the two), Test and Service, the player's own
//     key; how long, once, and what the box is told
//   - the list the launcher writes, and where the player is in it
//   - every cabinet: each family that a games/*/game.toml names can choose, change shelf and play
//     with controls it has, and the lamps named for them are its own
//   - the picture is drawn at both sizes
// test_menu GAMES_DIR                      the checks
// test_menu GAMES_DIR --ppm FILE FAMILY [wide]   a picture of the menu for that cabinet, to look at
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

#include "host/menukey.h"
#include "host/menupic.h"
#include "menu/menu.h"
#include "shell/cabinet.h"

using namespace hk;
namespace mk = hk::host::menukey;

static int g_failed = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) g_failed++, fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); \
    } while (0)

static void test_hold() {
    mk::set(false);
    mk::input("Keypad 5", 1), mk::input("Keypad 7", 1), mk::input("Keypad 3", 1);
    CHECK(!mk::frame(0) && !mk::frame(10));   // off: nothing is watched

    mk::set(true, 2.0);
    mk::input("Keypad 5", 1), mk::input("Keypad 7", 1);
    CHECK(!mk::frame(0.0) && !mk::frame(5.0));   // two of the three
    mk::input("Keypad 3", 1);
    CHECK(!mk::frame(10.0));                     // whole from now
    CHECK(mk::progress(10.2) == 0);              // a touch shows nothing
    CHECK(mk::progress(11.0) > 0.49 && mk::progress(11.0) < 0.51);
    CHECK(!mk::frame(11.9));
    mk::input("Keypad 7", 0);                    // let go before the time: it starts again
    CHECK(!mk::frame(12.5) && mk::progress(12.5) == 0);
    mk::input("Keypad 7", 1);
    CHECK(!mk::frame(13.0) && !mk::frame(14.9));
    CHECK(mk::frame(15.0));
    CHECK(!mk::frame(15.1));                     // once

    mk::set(true, 2.0);   // the three across the two 10-keys aren't it; player 2's own are
    mk::input("Keypad 5", 1), mk::input("P2 Keypad 7", 1), mk::input("P2 Keypad 3", 1);
    CHECK(!mk::frame(0) && !mk::frame(9));
    mk::input("P2 Keypad 5", 1);
    CHECK(!mk::frame(10) && mk::frame(12));

    mk::set(true, 1.0);   // a cabinet with no 10-key: Test and Service together
    mk::input("Test", 1);
    CHECK(!mk::frame(0) && !mk::frame(5));
    mk::input("Service", 1);
    CHECK(!mk::frame(6) && mk::frame(7));

    mk::set(true, 1.0);   // the player's own key, and other inputs change nothing
    mk::input("P1 Button 1", 1), mk::input("Coin 1", 1);
    CHECK(!mk::frame(0) && !mk::frame(5));
    mk::input(mk::OWN_KEY, 1);
    CHECK(!mk::frame(6) && mk::frame(7.5));
    mk::set(false);
}

static const char *kList = "hiraku-menu-list 1\n"
                           "current\tbm7thmix\n"
                           "game\tbm1stmix\tbeatmania\tbeatmania\t1997  DJ Main\n"
                           "game\tbm7thmix\tbeatmania\tbeatmania 7th MIX\t2002  DJ Main\n"
                           "game\tbmfinal\tbeatmania\tbeatmania THE FINAL\t2002  DJ Main\n"
                           "game\tbmiidx\tbeatmania IIDX\tbeatmania IIDX\t1999  Twinkle\n"
                           "game\tiidx16\tbeatmania IIDX\tbeatmania IIDX 16 EMPRESS\t2008  PC\n"
                           "game\tpopn18\tpop'n music\tpop'n music 18 \xe3\x81\x9b\xe3\x82\x93\t2010  PC\n";

static void test_list() {
    menu::List l;
    std::string err;
    CHECK(l.parse(kList, err));
    CHECK(l.shelves.size() == 3 && l.games() == 6 && l.current == "bm7thmix");
    CHECK(l.shelves[1].name == "beatmania IIDX" && l.shelves[1].games[1].id == "iidx16");
    CHECK(l.shelves[1].games[1].detail == "2008  PC");
    CHECK(l.shelves[2].games[0].title == "pop'n music 18 ??");   // what the font hasn't: a mark each
    CHECK(!l.parse("", err));
    CHECK(!l.parse("something else\n", err));
    CHECK(!l.parse("hiraku-menu-list 1\ngame\tonly-an-id\n", err) && err.find("line 2") != std::string::npos);
    CHECK(l.parse("hiraku-menu-list 1\r\n# none\r\n", err) && l.games() == 0);   // no game ready: a list all the same
}

static void test_state() {
    menu::List l;
    std::string err;
    CHECK(l.parse(kList, err));
    menu::State s;
    s.set(l);
    CHECK(s.shelf() == 0 && s.row() == 1 && s.selected()->id == "bm7thmix");   // opens on the game just left
    CHECK(s.act(menu::Act::next, 1) && s.selected()->id == "bmfinal");
    CHECK(s.act(menu::Act::next, 2) && s.selected()->id == "bm1stmix");        // round the end
    CHECK(s.act(menu::Act::prev, 3) && s.selected()->id == "bmfinal");
    CHECK(s.act(menu::Act::shelf_next, 4) && s.selected()->id == "bmiidx");
    CHECK(s.act(menu::Act::next, 5) && s.selected()->id == "iidx16");
    CHECK(s.act(menu::Act::shelf_prev, 6) && s.selected()->id == "bmfinal");   // a shelf keeps its place
    CHECK(s.act(menu::Act::shelf_prev, 7) && s.selected()->id == "popn18");    // round the shelves
    CHECK(!s.act(menu::Act::next, 8));                                         // a shelf of one game
    CHECK(!s.chosen());
    CHECK(s.act(menu::Act::start, 9) && s.chosen() && s.chosen()->id == "popn18" && s.chosen_at() == 9);
    CHECK(!s.act(menu::Act::prev, 10) && !s.act(menu::Act::shelf_next, 10));   // chosen: nothing moves
    for (int k = 0; k < 40; k++) s.animate();
    CHECK(s.shown_row() == s.row());

    menu::List none;
    CHECK(none.parse("hiraku-menu-list 1\n", err));
    s.set(none);
    CHECK(!s.selected() && !s.act(menu::Act::start, 0) && !s.act(menu::Act::next, 0));
    l.current = "not-there";
    s.set(l);
    CHECK(s.selected()->id == "bm1stmix");
}

// the families the games name (their game.toml's `family = "..."`)
static std::set<std::string> families(const std::string &games_dir) {
    std::set<std::string> out;
    std::error_code ec;
    for (const auto &d : std::filesystem::directory_iterator(games_dir, ec)) {
        std::ifstream in(d.path() / "game.toml");
        for (std::string line; std::getline(in, line);) {
            char name[64];
            if (sscanf(line.c_str(), " family = \"%63[^\"]\"", name) == 1) out.insert(name);
        }
    }
    return out;
}

static void test_cabinets(const std::string &games_dir) {
    const std::set<std::string> all = families(games_dir);
    CHECK(all.size() >= 15);   // (the games folder was read)
    for (const std::string &f : all) {
        std::set<std::string> has, lamps;
        for (const cab::Control &c : cab::controls(f)) has.insert(c.name);
        for (const cab::Lamp &l : cab::lamps(f)) lamps.insert(l.name);
        bool prev = false, next = false, shelf = false, start = false;
        const auto &rules = menu::rules(f);
        if (rules.empty()) g_failed++, fprintf(stderr, "family %s: no control drives the menu\n", f.c_str());
        for (const menu::Rule &r : rules) {
            if (!has.count(r.control)) g_failed++, fprintf(stderr, "family %s: no control \"%s\"\n", f.c_str(), r.control.c_str());
            CHECK(menu::action(f, r.control) == r.act);
            prev |= r.act == menu::Act::prev, next |= r.act == menu::Act::next, start |= r.act == menu::Act::start;
            shelf |= r.act == menu::Act::shelf_prev || r.act == menu::Act::shelf_next;
            const std::string lamp = menu::lamp_for(f, r.control);
            if (!lamp.empty() && !lamps.count(lamp))
                g_failed++, fprintf(stderr, "family %s: no lamp \"%s\" (for %s)\n", f.c_str(), lamp.c_str(), r.control.c_str());
        }
        if (!(prev && next && shelf && start)) g_failed++, fprintf(stderr, "family %s: can't choose, change shelf and play\n", f.c_str());
        const auto hints = menu::legend(f);
        CHECK(hints.size() == 3);
        for (const menu::Hint &h : hints) CHECK(h.control.size() + 2 + h.does.size() <= 38);   // a line of the 640-wide picture
        // either 10-key drives it in every family
        CHECK(menu::action(f, "Keypad 8") == menu::Act::prev && menu::action(f, "P2 Keypad 2") == menu::Act::next);
        CHECK(menu::action(f, "Keypad 4") == menu::Act::shelf_prev && menu::action(f, "Keypad 6") == menu::Act::shelf_next);
        CHECK(menu::action(f, "P2 Keypad Enter") == menu::Act::start && menu::action(f, "Keypad 00") == menu::Act::start);
        CHECK(menu::action(f, "Keypad 5") == menu::Act::none && menu::action(f, "Coin 1") == menu::Act::none);
    }
    CHECK(menu::turntable("iidx") && menu::turntable("djbm") && !menu::turntable("popn"));
    CHECK(menu::lamp_for("iidx", "P2 Button 4") == "P2 Key Lamp 4" && menu::lamp_for("djbm", "P1 Button 2") == "P1 Button Lamp 2");
    CHECK(menu::legend("a family not yet made").size() == 3);   // the 10-key's, at least
}

static void write_ppm(const std::string &path, const std::vector<uint32_t> &px, int w, int h) {
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (uint32_t p : px) {
        const uint8_t rgb[3] = {(uint8_t)(p >> 16), (uint8_t)(p >> 8), (uint8_t)p};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static void test_draw() {
    menu::List l;
    std::string err;
    CHECK(l.parse(kList, err));
    menu::State s;
    s.set(l);
    for (int w : {640, 848}) {
        std::vector<uint32_t> px((size_t)w * 480, 0);
        menu::draw(s, "iidx", 0, px.data(), w, 480);
        std::set<uint32_t> colours(px.begin(), px.end());
        CHECK(colours.size() > 8);                 // a ground, text, the bar
        CHECK(colours.count(0xffffb830u) == 1);    // the bar's amber
        bool opaque = true;
        for (uint32_t p : px) opaque &= p >> 24 == 0xff;
        CHECK(opaque);
        const std::vector<uint32_t> first = px;
        s.act(menu::Act::next, 1);
        s.animate();
        menu::draw(s, "iidx", 1, px.data(), w, 480);
        CHECK(px != first);                        // a move shows
        s.act(menu::Act::prev, 2);
    }
    menu::State empty;
    menu::List none;
    CHECK(none.parse("hiraku-menu-list 1\n", err));
    empty.set(none);
    std::vector<uint32_t> px(640 * 480, 0);
    menu::draw(empty, "popn", 0, px.data(), 640, 480);   // says there is none, and doesn't fall over
}

// A game's picture: a frame kept by the shell (host/menupic.h) is what the menu reads and shows
// behind the list, at its right; a blank frame isn't kept, and a file that isn't there is no picture
static void test_picture(const std::string &dir) {
    // a frame of many colours, light at the right (a title screen, for what it matters here)
    std::vector<uint32_t> frame(512 * 384);
    for (int y = 0; y < 384; y++)
        for (int x = 0; x < 512; x++)
            frame[(size_t)y * 512 + x] = x >= 128 ? 0xc0c0c0u + (uint32_t)(y / 8 % 4) * 0x101010u + (uint32_t)(x / 64 % 4) * 0x000810u : (uint32_t)(y / 24) << 20 | 0x203040;
    const std::string file = dir + "/menu-picture-test.bmp", blank_file = dir + "/menu-picture-blank.bmp";
    std::filesystem::remove(file);
    std::filesystem::remove(blank_file);
    CHECK(host::menupic::save(file, frame.data(), 512, 384, 4.0 / 3.0));
    CHECK(std::filesystem::exists(file) && std::filesystem::file_size(file) == 54 + 320 * 3 * 240);   // 240 lines of the monitor's shape
    const std::vector<uint32_t> blank(64 * 64, 0x101010);
    CHECK(!host::menupic::save(blank_file, blank.data(), 64, 64, 0) && !std::filesystem::exists(blank_file));
    // a start-up error's screen: white and red text on black, a blue box
    std::vector<uint32_t> text(640 * 480, 0);
    for (int y = 0; y < 480; y++)
        for (int x = 0; x < 640; x++)
            text[(size_t)y * 640 + x] = y >= 200 && y < 360 && x >= 40 && x < 600 ? (x % 8 < 2 && y % 16 < 8 ? 0xffffffu : 0x2020a0u) : y < 120 && x % 16 < 3 && y % 12 < 7 ? (x < 320 ? 0xffffffu : 0xff2020u) : 0;
    CHECK(!host::menupic::save(blank_file, text.data(), 640, 480, 4.0 / 3.0) && !std::filesystem::exists(blank_file));

    const auto drawn = [](const std::string &picture, uint64_t at) {
        menu::List l;
        std::string err;
        CHECK(l.parse("hiraku-menu-list 1\ngame\ta\tShelf\tA game\t2001\ngame\tb\tShelf\tAnother\t2002\t" + picture + "\ncurrent\tb\n", err));
        CHECK(l.shelves.size() == 1 && l.shelves[0].games[1].picture == picture && l.shelves[0].games[0].picture.empty());
        menu::State s;
        s.set(l);
        std::vector<uint32_t> px(640 * 480, 0);
        menu::draw(s, "popn", at, px.data(), 640, 480);
        return px;
    };
    const std::vector<uint32_t> without = drawn("", 60), with = drawn(file, 60), missing = drawn(dir + "/no-such-picture.bmp", 60);
    const size_t at = (size_t)118 * 640 + 618;   // in the list's place, right of every text
    CHECK((without[at] >> 16 & 0xff) < 60 && (with[at] >> 16 & 0xff) > 120);
    CHECK(missing == without);
    CHECK((drawn(file, 1)[at] >> 16 & 0xff) < (with[at] >> 16 & 0xff));   // it comes up, it isn't there at once
    for (uint32_t p : with) CHECK(p >> 24 == 0xff);
    std::filesystem::remove(file);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "test_menu GAMES_DIR [SCRATCH_DIR | --ppm FILE FAMILY [wide]]\n");
        return 2;
    }
    if (argc >= 5 && !strcmp(argv[2], "--ppm")) {
        menu::List l;
        std::string err;
        // the list given (HK_MENU_LIST), or the test's
        if (const char *file = getenv("HK_MENU_LIST"); file ? !l.load(file, err) : !l.parse(kList, err)) {
            fprintf(stderr, "%s\n", err.c_str());
            return 2;
        }
        menu::State s;
        s.set(l);
        const int w = argc > 5 ? 848 : 640;
        std::vector<uint32_t> px((size_t)w * 480, 0);
        const uint64_t frame = getenv("HK_MENU_FRAME") ? strtoull(getenv("HK_MENU_FRAME"), nullptr, 0) : 100;
        if (getenv("HK_MENU_START")) s.act(menu::Act::start, frame - 4);
        menu::draw(s, argv[4], frame, px.data(), w, 480);
        write_ppm(argv[3], px, w, 480);
        return 0;
    }
    // (first: nothing has been pressed yet)
    CHECK(!host::menukey::touched());
    host::menukey::input("P1 Button 1", 0);
    CHECK(!host::menukey::touched());
    host::menukey::input("P1 Button 1", 1);
    CHECK(host::menukey::touched());   // with the way out off too: a picture isn't taken from a game someone plays
    test_hold();
    test_list();
    test_state();
    test_cabinets(argv[1]);
    test_draw();
    test_picture(argc > 2 ? argv[2] : ".");
    if (g_failed) {
        fprintf(stderr, "%d checks failed\n", g_failed);
        return 1;
    }
    printf("ok\n");
    return 0;
}
