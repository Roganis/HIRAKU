// SPDX-License-Identifier: GPL-3.0-or-later
// The boxes over a game's picture in the launcher's look (runtime/host/uibox.h): none without the
// launcher's look and fonts (the 8x8 boxes stay); with them, drawn at their scale, rounded (see-
// through corners), on the look's ground, a notice edged in its icon's colour, its icon drawn from
// the font, a bar filled in the six colours as far as it's done and the line's colour after.
//   test_uibox DIR
#include <cstdio>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "host/uibox.h"
#include "lang/fit.h"
#include "lang/lang.h"

using namespace hk::host;

static int g_failed = 0;
#define CHECK(x) \
    do { \
        if (!(x)) g_failed++, fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
    } while (0)

// a font file (menu/font.h): ASCII as boxes of ink, and the icons as full squares
static std::string font_file() {
    const auto u32 = [](std::string &o, uint32_t v) { for (int k = 0; k < 4; k++) o += (char)(v >> (8 * k) & 0xff); };
    const auto u16 = [](std::string &o, int v) { o += (char)(v & 0xff), o += (char)(v >> 8 & 0xff); };
    std::string file("HKMF1\0\0\0", 8), ink;
    u32(file, 3);
    std::vector<uint32_t> chars;
    for (uint32_t c = 32; c < 127; c++) chars.push_back(c);
    for (int k = 0; k < ui::kIcons; k++) chars.push_back((uint32_t)ui::Icon::info + (uint32_t)k);
    for (int size = 0; size < 3; size++) {
        const int line = 12 + size * 6;
        u32(file, (uint32_t)line);
        u32(file, (uint32_t)chars.size());
        for (const uint32_t c : chars) {
            const bool icon = c >= 0xF0000;
            const int w = c == ' ' ? 0 : icon ? line : line / 2, h = icon ? line : line / 2;
            u32(file, c), u16(file, icon ? line : line / 2 + 2), u16(file, 0), u16(file, icon ? 0 : line / 4), u16(file, w), u16(file, h);
            u32(file, (uint32_t)ink.size());
            ink.append((size_t)w * h, (char)0xff);
        }
    }
    u32(file, (uint32_t)ink.size());
    return file + ink;
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    const std::string dir = argv[1], font = dir + "/uibox.font";
    {
        std::ofstream f(font, std::ios::binary);
        f << font_file();
    }
    const char *six = "stripe=#000011:#000022:#000033:#000044:#000055:#000066";
    const std::string look = std::string("ground=#101020,ink=#f0f0f0,dim=#808080,line=#303050,bar=#8040ff,point=#ffd000,") + six;

    // without the launcher's: not styled (and a look it can't read, or no font, is none)
    CHECK(!ui::set_for_test("", "", "", 2) && !ui::styled() && ui::scale() == 1);
    CHECK(!ui::set_for_test("ground=red", font, "", 2) && !ui::styled());
    CHECK(!ui::set_for_test(look, dir + "/no-such.font", "", 2) && !ui::styled());
    CHECK(ui::set_for_test(look, font, "", 2) && ui::styled() && ui::scale() == 2);
    CHECK(ui::look().ground == 0x101020 && ui::look().stripe.size() == 6);

    // a notice: rounded, its ground, its edge and icon in the icon's colour (card: the first of the six)
    ui::Image n;
    ui::notice_box(n, ui::Icon::card, {"Card read", "Player 1"});
    CHECK(n.scale == 2 && n.w > 100 && n.h > 40 && n.px.size() == (size_t)n.w * n.h);
    CHECK(n.px[0] >> 24 == 0 && n.px[(size_t)n.w - 1] >> 24 == 0);            // see-through corners
    CHECK(n.px[(size_t)(n.h / 2) * n.w + n.w - 6] == (0xff000000u | 0x101020));   // the ground
    CHECK(n.px[(size_t)(n.h / 2) * n.w + 1] == (0xff000000u | 0x000011));          // the edge
    const std::set<uint32_t> colours(n.px.begin(), n.px.end());
    CHECK(colours.count(0xff000011u));   // the icon
    bool title = false;   // (the font's ink: within a step of the ink's colour)
    for (const uint32_t c : colours) title |= (c & 0xffffff) >= 0xefefef && (c & 0xffffff) <= 0xf0f0f0 && (c >> 8 & 255) == (c & 255);
    CHECK(title);   // the title
    CHECK(ui::icon_colour(ui::Icon::alert) == 0x000055 && ui::icon_colour(ui::Icon::dialpad) == 0xffd000);

    // a long paragraph: broken into lines no wider than the boxes' most (lang/fit.h), the box with it
    {
        const std::string para = "The card waits at the slot: the game hasn't opened it. Is e-AMUSEMENT on in its test mode?";
        const std::vector<std::string> lines = ui::wrap(para, 0);
        CHECK(lines.size() >= 2 && ui::max_line_width() == 600);
        for (const std::string &l : lines) CHECK(hk::lang::width(l, [](uint32_t) { return 8; }) <= 600 / 8 * 8);
        ui::Image one, two;
        ui::notice_box(one, ui::Icon::card, {"Card", "short"});
        ui::notice_box(two, ui::Icon::card, {"Card", para});
        CHECK(two.w <= one.w - 6 * 8 + 600 + 8 && two.h > one.h);   // (wider only up to the most; taller)
        // a value kept whole (an argument): not broken where it can go on the next line whole
        const std::vector<std::string> kept = ui::wrap(std::string(64, 'a') + " " + hk::lang::keep("bb cc dd ee ff"), 0);
        CHECK(kept.size() == 2 && kept[1] == "bb cc dd ee ff");
        // the text as a box shows it: English without the launcher's language
        CHECK(ui::said("boxes", hk::lang::later("Memory card %1 in its slot", {"1"})) == "Memory card 1 in its slot");
    }

    // a bar box half done: the six's first three, then the line's colour
    ui::Image b;
    ui::bar_box(b, true, ui::Icon::menu, "GAME MENU", ui::Icon::dialpad, "Keep holding", 0.5, "");
    CHECK(b.w >= 600 && b.scale == 2);   // (300 a 360-line screen's pixels wide, at least)
    int bar_y = -1;
    for (int y = 0; y < b.h && bar_y < 0; y++)
        if (b.px[(size_t)y * b.w + b.w / 2 + 40] == (0xff000000u | 0x303050) && b.px[(size_t)y * b.w + b.w / 4] == (0xff000000u | 0x000022)) bar_y = y;
    CHECK(bar_y > 0);
    if (bar_y > 0) {
        const uint32_t *row = &b.px[(size_t)bar_y * b.w];
        CHECK(row[b.w / 2 - 20] == (0xff000000u | 0x000033) && row[b.w - b.w / 6] == (0xff000000u | 0x303050));
    }
    // no logo given: none drawn, nor its box (the heading's icon instead)
    CHECK(!std::set<uint32_t>(b.px.begin(), b.px.end()).count(0xff000000u));

    // back to none
    CHECK(!ui::set_for_test("", "", "", 1) && !ui::styled());
    if (g_failed) fprintf(stderr, "%d failed\n", g_failed);
    return g_failed ? 1 : 0;
}
