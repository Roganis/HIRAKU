// SPDX-License-Identifier: GPL-3.0-or-later
// The game menu (runtime/menu/) and the way out to it (runtime/host/menukey.h):
//   - the hold: 5, 7 and 3 of one 10-key (not across the two), Test and Service, the player's own
//     key; how long, once, and what the box is told
//   - the list the launcher writes, and where the player is in it
//   - every cabinet: each family that a games/*/game.toml names can choose, change shelf and play
//     with controls it has, and the lamps named for them are its own
//   - the picture is drawn at both sizes, and in the launcher's look (its colours, the shelves')
//   - HIRAKU's logo: its pictures read, and where it is when (opening, up to the top, closing)
// test_menu GAMES_DIR                      the checks
// test_menu GAMES_DIR --ppm FILE FAMILY [wide]   a picture of the menu for that cabinet, to look at
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "host/menukey.h"
#include "host/menupic.h"
#include "menu/font.h"
#include "menu/logo.h"
#include "menu/menu.h"
#include "menu/pads.h"

#include <SDL3/SDL.h>
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
    // a name is kept as it is written; the 8x8 font shows a mark for each character it hasn't
    CHECK(l.shelves[2].games[0].title.rfind("pop'n music 18 ", 0) == 0 && l.shelves[2].games[0].title.size() > 17);
    CHECK(menu::ascii_only(l.shelves[2].games[0].title) == "pop'n music 18 ??");
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
    // the ee'MALL station: a dial and two buttons
    CHECK(menu::turntable("eemall") && menu::action("eemall", "A") == menu::Act::start && menu::action("eemall", "B") == menu::Act::shelf_next);
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

// a frame by which the logo has gone up into its corner and the black has gone (test_logo)
constexpr uint64_t kUp = 200;

static void test_draw() {
    menu::List l;
    std::string err;
    CHECK(l.parse(kList, err));
    menu::State s;
    s.set(l);
    for (int w : {640, 848}) {
        std::vector<uint32_t> px((size_t)w * 480, 0);
        menu::draw(s, "iidx", kUp, px.data(), w, 480);
        std::set<uint32_t> colours(px.begin(), px.end());
        CHECK(colours.size() > 8);                 // a ground, text, the bar
        CHECK(colours.count(0xffffb830u) == 1);    // the bar's amber
        bool opaque = true;
        for (uint32_t p : px) opaque &= p >> 24 == 0xff;
        CHECK(opaque);
        const std::vector<uint32_t> first = px;
        s.act(menu::Act::next, kUp + 1);
        s.animate();
        menu::draw(s, "iidx", kUp + 1, px.data(), w, 480);
        CHECK(px != first);                        // a move shows
        s.act(menu::Act::prev, kUp + 2);
    }
    // a notice (the game that came back ended with an error) is said until the first move
    {
        std::vector<uint32_t> px(640 * 480, 0), said(640 * 480, 0);
        menu::draw(s, "iidx", kUp + 50, px.data(), 640, 480);
        s.set_notice("beatmania 7th MIX|ended with an error");
        menu::draw(s, "iidx", kUp + 50, said.data(), 640, 480);
        CHECK(said != px && s.notice() == "beatmania 7th MIX|ended with an error");
        CHECK(s.act(menu::Act::next, kUp + 51) && s.notice().empty());
        s.act(menu::Act::prev, kUp + 52);
        s.animate();
    }
    menu::State empty;
    menu::List none;
    CHECK(none.parse("hiraku-menu-list 1\n", err));
    empty.set(none);
    std::vector<uint32_t> px(640 * 480, 0);
    menu::draw(empty, "popn", 0, px.data(), 640, 480);   // says there is none, and doesn't fall over
}

// The launcher's look (--look): its colours read, a light ground's defaults, the shelves' colours
// from the list; drawn with them (no amber left), and the menu's own colours again after
static void test_look() {
    menu::Look l;
    std::string err;
    CHECK(l.parse("ground=#120d26,foot=#1d1440,ink=#f3eeff,dim=#948bc0,line=#2e2556,bar=#8b52ff,point=#ffd23a,"
                  "stripe=#4a7bff:#2bd268:#ffd23a:#ff8f33:#ff4757:#9a5cff,round=17", err));
    CHECK(l.ground == 0x120d26 && l.foot == 0x1d1440 && l.bar == 0x8b52ff && l.round == 17);
    CHECK(l.stripe.size() == 6 && l.stripe[0] == 0x4a7bff && l.stripe[5] == 0x9a5cff);
    menu::Look day;
    CHECK(day.parse("ground=#fff8ee,bar=#6b2be0", err));   // the rest: a light look's, the foot the ground's
    CHECK(day.foot == 0xfff8ee && day.ink == 0x232629 && day.bar == 0x6b2be0 && menu::light(day.ground) && !menu::light(day.ink));
    menu::Look none;
    CHECK(none.parse("", err) && none.ground == menu::Look().ground && none.bar == 0xffb830);
    for (const char *bad : {"ground=#12", "ground=red", "colour=#000000", "round=x", "round=99", "stripe=#fff:#000000"}) {
        menu::Look b;
        err.clear();
        CHECK(!b.parse(bad, err) && !err.empty());
    }

    menu::List list;
    CHECK(list.parse(std::string(kList) + "shelf\tbeatmania IIDX\t#3a2e7a\n", err));   // (anywhere in the list)
    CHECK(list.shelves.size() == 3 && !list.shelves[0].has_colour && list.shelves[1].has_colour && list.shelves[1].colour == 0x3a2e7a);
    menu::List odd;
    CHECK(!odd.parse("hiraku-menu-list 1\nshelf\tx\tblue\n", err) && err.find("line 2") != std::string::npos);

    // drawn: the ground, the shelf's colour on its pill, the six under the top, no amber
    list.current = "bmiidx";
    menu::State s;
    s.set(list);
    menu::set_look(l);
    std::vector<uint32_t> px(640 * 480, 0);
    menu::draw(s, "popn", kUp, px.data(), 640, 480);
    const std::set<uint32_t> colours(px.begin(), px.end());
    CHECK(px[(size_t)2 * 640 + 320] == (0xff000000u | 0x120d26));   // the top's ground (the foot further down)
    CHECK(colours.count(0xff000000u | 0x3a2e7a));                   // the pill
    CHECK(px[(size_t)48 * 640 + 40] == (0xff000000u | 0x4a7bff) && px[(size_t)48 * 640 + 600] == (0xff000000u | 0x9a5cff));
    CHECK(!colours.count(0xffffb830u));
    CHECK(colours.count(0xff000000u | 0x2bd268));                   // pop'n's "Green buttons": its dot
    // a light ground: the logo's black box stands on it
    menu::set_look(day);
    menu::draw(s, "popn", kUp, px.data(), 640, 480);
    CHECK(px[(size_t)470 * 640 + 320] == (0xff000000u | 0xfff8ee));
    // the menu's own again
    menu::own_look();
    menu::draw(s, "iidx", kUp, px.data(), 640, 480);
    CHECK(std::set<uint32_t>(px.begin(), px.end()).count(0xffffb830u) == 1);
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
    // F12's (the player's own choice): kept whatever it shows
    CHECK(host::menupic::save(blank_file, text.data(), 640, 480, 4.0 / 3.0, true) && std::filesystem::exists(blank_file));
    std::filesystem::remove(blank_file);

    const auto drawn = [](const std::string &picture, uint64_t at) {
        menu::List l;
        std::string err;
        CHECK(l.parse("hiraku-menu-list 1\ngame\ta\tShelf\tA game\t2001\ngame\tb\tShelf\tAnother\t2002\t" + picture + "\ncurrent\tb\n", err));
        CHECK(l.shelves.size() == 1 && l.shelves[0].games[1].picture == picture && l.shelves[0].games[0].picture.empty());
        menu::State s;
        s.set(l);
        s.act(menu::Act::next, kUp), s.act(menu::Act::prev, kUp);   // (a move at kUp: the picture comes up from then)
        std::vector<uint32_t> px(640 * 480, 0);
        menu::draw(s, "popn", kUp + at, px.data(), 640, 480);
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

// The launcher's fonts for the menu (menu/font.h), from a file made up here: read, measured and
// drawn; a character it hasn't shows as its question mark; a file cut short anywhere, or with a
// glyph outside its pixels, is refused whole; the menu drawn with it differs from the 8x8 one's
static void test_font(const std::string &dir) {
    const auto u32 = [](std::string &o, uint32_t v) { for (int k = 0; k < 4; k++) o += (char)(v >> (8 * k) & 0xff); };
    const auto u16 = [](std::string &o, int v) { o += (char)(v & 0xff), o += (char)(v >> 8 & 0xff); };
    std::string file("HKMF1\0\0\0", 8), ink;
    u32(file, 3);
    const uint32_t chars[] = {'A', '?', ' ', 0x65e5};   // (the last: a Japanese one)
    for (int size = 0; size < 3; size++) {
        const int box = 6 + size * 4;   // each glyph a box of ink, a space none
        u32(file, (uint32_t)(box + 4));
        u32(file, 4);
        for (const uint32_t c : chars) {
            const int w = c == ' ' ? 0 : box, shade = c == '?' ? 0x40 : 0xff;
            u32(file, c), u16(file, box + 2), u16(file, 1), u16(file, 2), u16(file, w), u16(file, w);
            u32(file, (uint32_t)ink.size());
            ink.append((size_t)w * w, (char)shade);
        }
    }
    u32(file, (uint32_t)ink.size());
    file += ink;
    menu::Font f;
    std::string err;
    CHECK(f.parse((const uint8_t *)file.data(), file.size(), err) && f.loaded() && f.line(0) == 10 && f.line(2) == 18);
    CHECK(f.width(1, "A A") == 3 * 12 && f.width(1, "A\xe6\x97\xa5") == 2 * 12);
    CHECK(f.width(1, "\xc3\xa9") == 12 && f.width(1, "\xff") == 12);   // one it hasn't, a byte that is no character: its '?'
    std::vector<uint32_t> px(64 * 32, 0xff000000u);
    f.draw(px.data(), 64, 32, 0, 0, 64, 32, 4, 3, 1, "A\xc3\xa9", 0xffffff, 255);
    CHECK((px[(size_t)5 * 64 + 5] & 0xffffff) > 0xf0f0f0);        // 'A': ink, where left and top put it
    CHECK(px[(size_t)4 * 64 + 5] == 0xff000000u && px[(size_t)5 * 64 + 4] == 0xff000000u);
    const uint32_t mark = px[(size_t)5 * 64 + 17] & 0xff;           // the '?' in the other's place: faint
    CHECK(mark > 0x20 && mark < 0x60);
    std::vector<uint32_t> clipped(64 * 32, 0xff000000u);
    f.draw(clipped.data(), 64, 32, 8, 0, 64, 32, 4, 3, 1, "A", 0xffffff, 255);
    CHECK(clipped[(size_t)5 * 64 + 5] == 0xff000000u && (clipped[(size_t)5 * 64 + 9] & 0xffffff) != 0);   // kept to its box
    f.draw(clipped.data(), 64, 32, 0, 0, 64, 32, 60, 28, 2, "AAAA", 0xffffff, 255);                          // off the edges: no harm
    CHECK(menu::utf8_points("a\xe6\x97\xa5\xe6") == (std::vector<uint32_t>{'a', 0x65e5, 0xfffd}));
    CHECK(menu::utf8_first("a\xe6\x97\xa5z", 2) == "a\xe6\x97\xa5" && menu::utf8_first("abc", 9) == "abc");
    // cut short anywhere: refused, and nothing of it kept
    for (size_t n = 0; n < file.size(); n++) {
        menu::Font cut;
        if (cut.parse((const uint8_t *)file.data(), n, err) || cut.loaded()) {
            CHECK(!"a font file cut short was taken");
            break;
        }
    }
    std::string outside = file;
    outside[8 + 4 + 8 + 14] = (char)0xff, outside[8 + 4 + 8 + 15] = (char)0xff, outside[8 + 4 + 8 + 16] = (char)0xff;   // the first glyph's place: far away
    CHECK(!f.parse((const uint8_t *)outside.data(), outside.size(), err) && !f.loaded());

    // the menu with it
    menu::List l;
    CHECK(l.parse(kList, err));
    menu::State s;
    s.set(l);
    std::vector<uint32_t> plain(640 * 480, 0), real(640 * 480, 0), again(640 * 480, 0);
    menu::draw(s, "iidx", kUp + 40, plain.data(), 640, 480);
    const std::string path = dir + "/menu-font-test.hkmf";
    std::ofstream(path, std::ios::binary) << file;
    CHECK(menu::set_font(path, err));
    menu::draw(s, "iidx", kUp + 40, real.data(), 640, 480);
    CHECK(real != plain);
    for (uint32_t p : real) CHECK(p >> 24 == 0xff);
    CHECK(!menu::set_font(dir + "/no-such-font", err));   // a file that isn't there: the font before stays
    menu::draw(s, "iidx", kUp + 40, again.data(), 640, 480);
    CHECK(again == real);
    CHECK(menu::set_font("", err));
    menu::draw(s, "iidx", kUp + 40, again.data(), 640, 480);
    CHECK(again == plain);
    std::filesystem::remove(path);
}

// A plain gamepad (menu/pads.h), one SDL makes up: its d-pad and stick choose and change shelf, A
// and Start play; what it holds when first seen does nothing until let go; a controller that is
// the cabinet's is left alone
static void test_pads() {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    if (!SDL_Init(SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD)) {
        printf("no SDL gamepads here (%s): the gamepad's part not run\n", SDL_GetError());
        return;
    }
    const auto plug = [](const char *name) {
        SDL_VirtualJoystickDesc d;
        SDL_INIT_INTERFACE(&d);
        d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        d.naxes = SDL_GAMEPAD_AXIS_COUNT, d.nbuttons = SDL_GAMEPAD_BUTTON_DPAD_RIGHT + 1;
        d.name = name;
        return SDL_AttachVirtualJoystick(&d);
    };
    const SDL_JoystickID id = plug("HIRAKU test gamepad"), cabinet = plug("HIRAKU test cabinet controls");
    SDL_Joystick *joy = id ? SDL_OpenJoystick(id) : nullptr, *cab_joy = cabinet ? SDL_OpenJoystick(cabinet) : nullptr;
    if (!joy || !cab_joy || !SDL_IsGamepad(id)) {
        printf("SDL can't make a gamepad up here (%s): the gamepad's part not run\n", SDL_GetError());
        SDL_Quit();
        return;
    }
    // held when the menu opens: down on the d-pad (as a pop'n controller's adapter holds it)
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_DPAD_DOWN, true);
    SDL_UpdateJoysticks();
    menu::Pads pads;
    std::vector<uint32_t> taken;
    // (only the two made up here: a real controller plugged into this PC is nobody's business)
    pads.set([&](uint32_t pad) { return pad != id; }, [&](uint32_t pad) { taken.push_back(pad); });
    std::map<std::string, bool> down;
    std::map<std::string, menu::Act> acts;
    const auto poll = [&] {
        SDL_UpdateJoysticks();
        SDL_UpdateGamepads();
        return pads.poll([&](const std::string &name, menu::Act a, bool d) { down[name] = d, acts[name] = a; });
    };
    const auto held = [&](menu::Act a) {
        int n = 0;
        for (const auto &[name, d] : down) n += d && acts[name] == a;
        return n;
    };
    CHECK(!poll() && pads.count() == 1 && taken == std::vector<uint32_t>{id});   // the cabinet's isn't taken
    CHECK(held(menu::Act::next) == 0);                                             // what it held at first does nothing
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_DPAD_DOWN, false);
    CHECK(!poll());
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_DPAD_DOWN, true);
    CHECK(poll() && held(menu::Act::next) == 1);   // let go and pressed: a move
    CHECK(!poll() && held(menu::Act::next) == 1);  // still held: no new press
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_DPAD_DOWN, false);
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_DPAD_UP, true);
    CHECK(poll() && held(menu::Act::next) == 0 && held(menu::Act::prev) == 1);
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_DPAD_UP, false);
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTX, 30000);   // the stick to the right: the next shelf
    CHECK(poll() && held(menu::Act::shelf_next) == 1 && held(menu::Act::prev) == 0);
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTX, 4000);    // not half way: nothing
    CHECK(!poll() && held(menu::Act::shelf_next) == 0);
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTX, -30000);
    CHECK(poll() && held(menu::Act::shelf_prev) == 1);
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTX, 0);
    for (const SDL_GamepadButton b : {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_START}) {
        SDL_SetJoystickVirtualButton(joy, b, true);
        CHECK(poll() && held(menu::Act::start) == 1);
        SDL_SetJoystickVirtualButton(joy, b, false);
        CHECK(!poll() && held(menu::Act::start) == 0);
    }
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_EAST, true);   // the others do nothing
    CHECK(!poll());
    // the cabinet's own controller's buttons aren't the menu's
    SDL_SetJoystickVirtualButton(cab_joy, SDL_GAMEPAD_BUTTON_SOUTH, true);
    CHECK(!poll() && held(menu::Act::start) == 0);
    CHECK(menu::legend(menu::kPadFamily).size() == 3 && menu::legend(menu::kPadFamily)[2].control == "A or Start");
    SDL_CloseJoystick(joy);
    SDL_CloseJoystick(cab_joy);
    SDL_DetachVirtualJoystick(id);
    SDL_DetachVirtualJoystick(cabinet);
}

// HIRAKU's logo (menu/logo.h): read from what the menu carries; it opens in the middle, goes up to
// the top at the end of its opening or at the first press, and closes in the middle once a game is
// chosen, the menu's end waiting for it
static void test_logo() {
    namespace lg = menu::logo;
    CHECK(lg::ready() && lg::width() == 501 && lg::height() == 145);
    CHECK(lg::frames(lg::opening) == 105 && lg::frames(lg::closing) == 54);
    const size_t n = (size_t)lg::width() * lg::height() * 2;
    const auto opaque = [&](const uint8_t *p) {
        size_t k = 0;
        for (size_t i = 1; i < n; i += 2) k += p[i] != 0;
        return k;
    };
    const uint8_t *still = lg::frame(lg::opening, 104);
    CHECK(opaque(still) == n / 2);                                        // standing: the whole box
    CHECK(still[((72 * 501) + 10) * 2] == 0 && still[((72 * 501) + 10) * 2 + 1] == 255);   // black inside
    CHECK(lg::still_from(lg::opening) > 30 && lg::still_from(lg::opening) < 104);
    CHECK(!memcmp(lg::frame(lg::closing, 0), still, n));                  // closing starts where opening stood
    CHECK(opaque(lg::frame(lg::closing, 53)) == 0);                       // ... and leaves nothing
    CHECK(opaque(lg::frame(lg::opening, 0)) < n / 20);                    // opening: a line, first
    CHECK(lg::frame(lg::opening, 1000) == lg::frame(lg::opening, 104));   // past the end: the last

    menu::List l;
    std::string err;
    CHECK(l.parse(kList, err));
    menu::State s;
    s.set(l);
    const menu::LogoAt mid = menu::logo_at(s, 60, 640, 480);
    CHECK(mid.anim == lg::opening && mid.frame == 60 && mid.w == 501 && mid.x == 70 && mid.y == 168);   // (69.5, 167.5: rounded)
    const menu::LogoAt top = menu::logo_at(s, 105 + menu::kLogoGlide, 848, 480);
    CHECK(top.anim == lg::opening && top.frame == 104 && top.h == 40 && top.x == 24 && top.y == 4);
    CHECK(menu::logo_at(s, 3000, 640, 480).h == 40);                      // and stays
    CHECK(menu::logo_at(s, 0, 640, 480).dark == 255 && menu::logo_at(s, 104, 640, 480).dark == 255);   // opening on black
    CHECK(menu::logo_at(s, 105 + 4, 640, 480).dark > 0 && menu::logo_at(s, 105 + menu::kLogoFade, 640, 480).dark == 0);   // gone quickly
    menu::State pressed;
    pressed.set(l);
    CHECK(pressed.act(menu::Act::next, 30));
    CHECK(menu::logo_at(pressed, 30 + menu::kLogoGlide, 640, 480).h == 40);   // a press: up at once
    CHECK(menu::logo_at(pressed, 30 + menu::kLogoGlide, 640, 480).frame == 30 + (int)menu::kLogoGlide);   // (still opening)
    CHECK(pressed.act(menu::Act::start, 200));
    CHECK(menu::logo_at(pressed, 200, 640, 480).h == 40 && menu::logo_at(pressed, 200, 640, 480).dark == 0);
    CHECK(menu::logo_at(pressed, 30, 640, 480).dark == 255 && menu::logo_at(pressed, 30 + menu::kLogoFade, 640, 480).dark == 0);
    CHECK(menu::logo_at(pressed, 200 + menu::kLogoGlide, 640, 480).dark == 255);   // black again, down in the middle
    const menu::LogoAt down = menu::logo_at(pressed, 200 + menu::kLogoGlide, 640, 480);
    CHECK(down.anim == lg::closing && down.frame == 0 && down.w == 501);
    CHECK(menu::logo_closed(pressed) == 200 + menu::kLogoGlide + 54);
    CHECK(menu::logo_at(pressed, menu::logo_closed(pressed), 640, 480).anim == -1);
    CHECK(menu::logo_at(pressed, menu::logo_closed(pressed), 640, 480).dark == 255);   // ends on black
    // chosen before the opening stood still: it closes from the standing box all the same
    menu::State quick;
    quick.set(l);
    CHECK(quick.act(menu::Act::start, 21));
    CHECK(menu::logo_closed(quick) == (uint64_t)lg::still_from(lg::opening) + 54);
    // drawn: the box's black in the middle while it opens, the list there once it has gone up
    std::vector<uint32_t> px(640 * 480, 0);
    menu::draw(s, "iidx", 60, px.data(), 640, 480);
    CHECK(px[240 * 640 + 100] == 0xff000000u && px[20 * 640 + 20] == 0xff000000u);   // (and all round it)
    menu::draw(s, "iidx", 300, px.data(), 640, 480);
    CHECK(px[240 * 640 + 100] != 0xff000000u && px[24 * 640 + 26] != px[24 * 640 + 300]);   // (the corner: the box)
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
        const uint64_t frame = getenv("HK_MENU_FRAME") ? strtoull(getenv("HK_MENU_FRAME"), nullptr, 0) : 300;
        if (getenv("HK_MENU_START")) s.act(menu::Act::start, frame - 4);
        if (getenv("HK_MENU_NOTICE")) s.set_notice(getenv("HK_MENU_NOTICE"));
        if (std::string why; getenv("HK_MENU_FONT") && !menu::set_font(getenv("HK_MENU_FONT"), why)) fprintf(stderr, "%s\n", why.c_str());
        if (getenv("HK_MENU_LOGO")) menu::set_logo(getenv("HK_MENU_LOGO"));
        if (const char *look = getenv("HK_MENU_LOOK")) {   // (the launcher's --look)
            menu::Look lk;
            if (std::string why; lk.parse(look, why)) menu::set_look(lk);
            else fprintf(stderr, "%s\n", why.c_str());
        }
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
    test_look();
    test_logo();
    test_picture(argc > 2 ? argv[2] : ".");
    test_font(argc > 2 ? argv[2] : ".");
    test_pads();
    if (g_failed) {
        fprintf(stderr, "%d checks failed\n", g_failed);
        return 1;
    }
    printf("ok\n");
    return 0;
}
