// SPDX-License-Identifier: GPL-3.0-or-later
// The game menu from each cabinet's own controls: for every family of shell/cabinet.cpp, which of
// its controls choose a game (up and down the list), change shelf, and play. The list goes down
// the screen and the shelves across it, so a control with a direction does what it points to;
// the others follow the cabinet's own menus where they have a habit (pop'n's blue and red buttons,
// a turntable, the Select buttons beside Start). tests/menu/test_menu.cpp holds every family of
// games/ to having all three.
#include "menu.h"

#include <map>

namespace hk::menu {

namespace {

struct Family {
    std::vector<Rule> rules;
    std::vector<Hint> hints;
    bool turntable = false;
};

using A = Act;

// beatmania's (DJ Main, Twinkle, the PC IIDX, beatmania III): the turntables choose, as in their
// song lists; the black keys change shelf; a white key or Start plays
Family beatmania(int keys) {
    Family f;
    f.turntable = true;
    for (const char *p : {"P1", "P2"}) {
        const std::string P = p;
        f.rules.push_back({P + " Turntable -", A::prev});
        f.rules.push_back({P + " Turntable +", A::next});
        for (int k = 1; k <= keys; k++)
            f.rules.push_back({P + " Button " + std::to_string(k), k % 2 ? A::start : k == 2 ? A::shelf_prev : A::shelf_next});
    }
    f.rules.push_back({"1 Player Start", A::start});
    f.rules.push_back({"2 Players Start", A::start});
    f.hints = {{"Turntable", "choose a game"}, {"Black keys", "change shelf"}, {"White key or Start", "play"}};
    return f;
}

// pop'n music's nine buttons: blue chooses, green changes shelf, red plays
Family popn(bool big_buttons) {
    Family f;
    f.rules = {{"P1 Button 4", A::prev},       {"P1 Button 6", A::next}, {"P1 Button 3", A::shelf_prev},
               {"P1 Button 7", A::shelf_next}, {"P1 Button 5", A::start}};
    if (big_buttons) f.rules.push_back({"P1 Big Red", A::shelf_prev}), f.rules.push_back({"P1 Big Blue", A::shelf_next});
    f.hints = {{"Blue buttons", "choose a game"}, {"Green buttons", "change shelf"}, {"Red button", "play"}};
    return f;
}

const std::map<std::string, Family> &families() {
    static const std::map<std::string, Family> all = [] {
        std::map<std::string, Family> m;
        m["iidx"] = beatmania(7);
        m["djbm"] = beatmania(5);
        m["bm3"] = beatmania(5);
        m["popn"] = m["djpopn"] = m["pypopn"] = m["pcpopn"] = popn(false);
        m["pcpopnhc"] = popn(true);
        // pop'n stage: its three selection buttons; the outer foot buttons change shelf
        m["djpstage"] = {{{"1 Player Start", A::prev}, {"3 Players Start", A::next}, {"2 Players Start", A::start},
                          {"P1 Button 1", A::shelf_prev}, {"P1 Button 10", A::shelf_next}},
                         {{"Select left / right", "choose a game"}, {"Outer buttons", "change shelf"}, {"Select OK", "play"}}};
        // DDR: the panels, either player's
        {
            Family f;
            for (const char *p : {"P1", "P2"}) {
                const std::string P = p;
                f.rules.push_back({P + " Up", A::prev});
                f.rules.push_back({P + " Down", A::next});
                f.rules.push_back({P + " Left", A::shelf_prev});
                f.rules.push_back({P + " Right", A::shelf_next});
            }
            f.rules.push_back({"1 Player Start", A::start});
            f.rules.push_back({"2 Players Start", A::start});
            f.hints = {{"Up / Down panels", "choose a game"}, {"Left / Right panels", "change shelf"}, {"Start", "play"}};
            m["ddr"] = f;
        }
        m["ddrsolo"] = {{{"P1 Up", A::prev}, {"P1 Down", A::next}, {"P1 Select L", A::prev}, {"P1 Select R", A::next},
                         {"P1 Left", A::shelf_prev}, {"P1 Right", A::shelf_next}, {"P1 Up-Left", A::shelf_prev},
                         {"P1 Up-Right", A::shelf_next}, {"1 Player Start", A::start}},
                        {{"Select L / R", "choose a game"}, {"Left / Right panels", "change shelf"}, {"Start", "play"}}};
        // Dance Maniax: Select beside Start; the upper sensors change shelf
        {
            Family f;
            for (const char *p : {"P1", "P2"}) {
                const std::string P = p;
                f.rules.push_back({P + " Select L", A::prev});
                f.rules.push_back({P + " Select R", A::next});
                f.rules.push_back({P + " Upper Left", A::shelf_prev});
                f.rules.push_back({P + " Upper Right", A::shelf_next});
            }
            f.rules.push_back({"1 Player Start", A::start});
            f.rules.push_back({"2 Players Start", A::start});
            f.hints = {{"Select L / R", "choose a game"}, {"Upper sensors", "change shelf"}, {"Start", "play"}};
            m["dmx"] = f;
        }
        // Mambo a GoGo: Select beside Start; the left and right congas change shelf, the centre one plays
        {
            Family f;
            f.rules = {{"Select Left", A::prev}, {"Select Right", A::next}, {"1 Player Start", A::start}};
            for (int z = 1; z <= 3; z++) {
                f.rules.push_back({"Left Pad " + std::to_string(z), A::shelf_prev});
                f.rules.push_back({"Right Pad " + std::to_string(z), A::shelf_next});
                f.rules.push_back({"Centre Pad " + std::to_string(z), A::start});
            }
            f.hints = {{"Select left / right", "choose a game"}, {"Left / right conga", "change shelf"}, {"Start or centre conga", "play"}};
            m["mambo"] = f;
        }
        m["drmn"] = {{{"Select L", A::prev}, {"Select R", A::next}, {"Hi-Hat", A::prev}, {"Cymbal", A::next},
                      {"High Tom", A::shelf_prev}, {"Low Tom", A::shelf_next}, {"1 Player Start", A::start}, {"Snare", A::start}},
                     {{"Select, hi-hat / cymbal", "choose a game"}, {"Toms", "change shelf"}, {"Start or snare", "play"}}};
        // GuitarFreaks: the neck's red and blue choose, green changes shelf, Start or the pick plays
        {
            Family f;
            for (const char *p : {"P1", "P2"}) {
                const std::string P = p;
                f.rules.push_back({P + " Button R", A::prev});
                f.rules.push_back({P + " Button B", A::next});
                f.rules.push_back({P + " Button G", A::shelf_next});
                f.rules.push_back({P + " Pick", A::start});
            }
            f.rules.push_back({"1 Player Start", A::start});
            f.rules.push_back({"2 Players Start", A::start});
            f.hints = {{"Neck R / B", "choose a game"}, {"Neck G", "change shelf"}, {"Start or pick", "play"}};
            m["gtrf"] = f;
        }
        // Keyboardmania: the two lowest white keys choose (C and D), the black keys beside them
        // change shelf; the wheel chooses too (menu/core.cpp)
        {
            Family f;
            for (const char *p : {"P1", "P2"}) {
                const std::string P = p;
                f.rules.push_back({P + " Key 1", A::prev});
                f.rules.push_back({P + " Key 3", A::next});
                f.rules.push_back({P + " Key 2", A::shelf_prev});
                f.rules.push_back({P + " Key 4", A::shelf_next});
            }
            f.rules.push_back({"1 Player Start", A::start});
            f.rules.push_back({"2 Players Start", A::start});
            f.hints = {{"Wheel, or lowest C / D", "choose a game"}, {"Lowest black keys", "change shelf"}, {"Start", "play"}};
            m["kbm"] = f;
        }
        // ParaParaParadise: its two buttons are its menus' up and down; the outer sensor bars change shelf
        m["ppp"] = {{{"P1 Button 1", A::prev}, {"P1 Button 2", A::next}, {"P1 Button 3", A::shelf_prev},
                     {"P1 Button 7", A::shelf_next}, {"1 Player Start", A::start}},
                    {{"Left / Right buttons", "choose a game"}, {"Outer sensor bars", "change shelf"}, {"Start", "play"}}};
        return m;
    }();
    return all;
}

const Family &family_of(const std::string &family) {
    static const Family none{{}, {{"10-key 8 / 2", "choose a game"}, {"10-key 4 / 6", "change shelf"}, {"10-key Enter", "play"}}};
    const auto &all = families();
    const auto f = all.find(family);
    return f != all.end() ? f->second : none;
}

}  // namespace

Act action(const std::string &family, const std::string &control) {
    for (const Rule &r : family_of(family).rules)
        if (r.control == control) return r.act;
    // a reader's 10-key, either player's: 8 and 2 are its up and down, 4 and 6 its left and right
    std::string k = control;
    if (k.rfind("P2 ", 0) == 0) k = k.substr(3);
    if (k == "Keypad 8") return A::prev;
    if (k == "Keypad 2") return A::next;
    if (k == "Keypad 4") return A::shelf_prev;
    if (k == "Keypad 6") return A::shelf_next;
    if (k == "Keypad Enter" || k == "Keypad 00") return A::start;
    return A::none;
}

const std::vector<Rule> &rules(const std::string &family) { return family_of(family).rules; }
bool turntable(const std::string &family) { return family_of(family).turntable; }
std::vector<Hint> legend(const std::string &family) { return family_of(family).hints; }

std::string lamp_for(const std::string &family, const std::string &control) {
    const bool p1 = control.rfind("P1 ", 0) == 0, p2 = control.rfind("P2 ", 0) == 0;
    const std::string rest = p1 || p2 ? control.substr(3) : control;
    const std::string P = p2 ? "P2" : "P1";
    const bool start1 = control == "1 Player Start", start2 = control == "2 Players Start";
    if (family == "iidx" || family == "djbm") {
        if (start1) return "P1 Start Lamp";
        if (start2) return "P2 Start Lamp";
        if (rest.rfind("Button ", 0) == 0) return P + (family == "iidx" ? " Key Lamp " : " Button Lamp ") + rest.substr(7);
    } else if (family == "popn" || family == "djpopn" || family == "pypopn" || family == "pcpopn" || family == "pcpopnhc") {
        if (p1 && rest.rfind("Button ", 0) == 0) return "Button Lamp " + rest.substr(7);
    } else if (family == "ddr") {
        if (start1) return "P1 Start Lamp";
        if (start2) return "P2 Start Lamp";
        if (rest == "Up" || rest == "Down" || rest == "Left" || rest == "Right") return P + " Foot " + rest;
    } else if (family == "ddrsolo" || family == "mambo" || family == "drmn") {
        if (start1) return "Start Lamp";
        if (family == "mambo" && (control == "Select Left" || control == "Select Right")) return control + " Lamp";
        if (family == "mambo" && control.find(" Pad ") != std::string::npos) return "Conga " + control.substr(0, control.find(' '));
        if (family == "drmn" && (control == "Select L" || control == "Select R")) return "Select Lamp";
        if (family == "drmn" && (control == "Hi-Hat" || control == "Cymbal" || control == "High Tom" || control == "Low Tom" || control == "Snare"))
            return control;
    } else if (family == "dmx") {
        if (start1) return "P1 Start Lamp";
        if (start2) return "P2 Start Lamp";
    } else if (family == "gtrf") {
        if (start1) return "Start Lamp P1";
        if (start2) return "Start Lamp P2";
    } else if (family == "kbm") {
        if (start1) return "Start 1P Lamp";
        if (start2) return "Start 2P Lamp";
    } else if (family == "ppp") {
        if (control == "P1 Button 1") return "Left Lamp";
        if (control == "P1 Button 2") return "Right Lamp";
        if (start1) return "OK Lamp";
    }
    return {};
}

}  // namespace hk::menu
