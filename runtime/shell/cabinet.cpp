// SPDX-License-Identifier: GPL-3.0-or-later
#include "cabinet.h"

#include <cctype>
#include <map>

namespace hk::cab {

namespace {

std::vector<Control> make_bm3() {
    std::vector<Control> c;
    const char *p1keys[5] = {"key:Z, joy0:button0", "key:S, joy0:button1", "key:X, joy0:button2", "key:D, joy0:button3",
                             "key:C, joy0:button4"};
    const char *p2keys[5] = {"key:M", "key:K", "key:Comma", "key:L", "key:Period"};
    for (int p = 1; p <= 2; p++) {
        std::string P = "P" + std::to_string(p), g = "Player " + std::to_string(p);
        for (int k = 1; k <= 5; k++)
            c.push_back({P + " Button " + std::to_string(k), "Key " + std::to_string(k), g, false, p == 1 ? p1keys[k - 1] : p2keys[k - 1]});
        c.push_back({P + " Turntable", "Turntable (analog axis)", g, true, p == 1 ? "joy0:axis0" : ""});
        c.push_back({P + " Turntable -", "Turntable − (as a button)", g, false, p == 1 ? "key:Left, key:Left Shift" : "key:Down"});
        c.push_back({P + " Turntable +", "Turntable + (as a button)", g, false, p == 1 ? "key:Right, key:A" : "key:Up"});
        c.push_back({P + " Foot", "Foot pedal", g, false, p == 1 ? "key:Space, joy0:button7" : ""});
        c.push_back({p == 1 ? "1 Player Start" : "2 Players Start", "Start", g, false, p == 1 ? "key:1, joy0:button8" : "key:2"});
    }
    c.push_back({"Coin 1", "Coin", "Cabinet", false, "key:5"});   // as DJ Main's and IIDX's (their Effect is on button 9)
    c.push_back({"Test", "Test", "Cabinet", false, "key:F2"});
    c.push_back({"Service", "Service", "Cabinet", false, "key:9"});
    // the service panel's A and B (MAME's IN0 bits 2 and 3): the test mode's buttons
    c.push_back({"A", "Test mode A", "Cabinet", false, "key:F3"});
    c.push_back({"B", "Test mode B", "Cabinet", false, "key:F4"});
    return c;
}

// pop'n music: the nine buttons zig-zag like the cabinet's two rows (odd = lower)
std::vector<Control> make_popn() {
    std::vector<Control> c;
    const char *keys = "ZSXDCFVGB";
    const char *colour[9] = {"white", "yellow", "green", "blue", "red", "blue", "green", "yellow", "white"};
    for (int k = 1; k <= 9; k++)
        c.push_back({"P1 Button " + std::to_string(k), "Button " + std::to_string(k) + " (" + colour[k - 1] + ")", "Player", false,
                     std::string("key:") + keys[k - 1] + ", joy0:button" + std::to_string(k - 1)});
    c.push_back({"Coin 1", "Coin", "Cabinet", false, "key:5, joy0:button9"});
    c.push_back({"Test", "Test", "Cabinet", false, "key:F2"});
    c.push_back({"Service", "Service", "Cabinet", false, "key:9"});
    return c;
}

// pop'n on Konami Python: the same, plus the e-AMUSEMENT card slot and 10-key
std::vector<Control> make_pypopn() {
    std::vector<Control> c = make_popn();
    c.push_back({"Card", "e-AMUSEMENT card (insert / take back)", "e-AMUSEMENT", false, "key:Insert"});
    for (int k = 0; k <= 9; k++)
        c.push_back({"Keypad " + std::to_string(k), "10-key " + std::to_string(k), "e-AMUSEMENT", false, "key:Keypad " + std::to_string(k)});
    c.push_back({"Keypad Enter", "10-key Enter", "e-AMUSEMENT", false, "key:Keypad Enter"});
    c.push_back({"Keypad Clear", "10-key Clear", "e-AMUSEMENT", false, "key:Backspace"});
    c.push_back({"Keypad .", "10-key .", "e-AMUSEMENT", false, "key:Keypad ."});
    return c;
}

// the 64-bit PC pop'n's cabinet (runtime/pc64/): pop'n's, with its two big buttons either side
std::vector<Control> make_pcpopnhc() {
    std::vector<Control> c = make_pypopn();
    c.push_back({"P1 Big Red", "Big red button (left)", "Player", false, "key:Left Shift"});
    c.push_back({"P1 Big Blue", "Big blue button (right)", "Player", false, "key:Right Shift"});
    return c;
}

// ParaParaParadise: the five sensor bars left to right, plus the two buttons
std::vector<Control> make_ppp() {
    std::vector<Control> c;
    const char *keys = "ZXCVB";
    for (int k = 1; k <= 5; k++)
        c.push_back({"P1 Button " + std::to_string(k + 2), "Sensor bar " + std::to_string(k) + (k == 1 ? " (left)" : k == 5 ? " (right)" : ""),
                     "Player", false, std::string("key:") + keys[k - 1] + ", joy0:button" + std::to_string(k - 1)});
    c.push_back({"P1 Button 1", "Left button (menu up)", "Player", false, "key:Left, joy0:button5"});
    c.push_back({"P1 Button 2", "Right button (menu down)", "Player", false, "key:Right, joy0:button6"});
    c.push_back({"1 Player Start", "Start / OK", "Player", false, "key:1, joy0:button8"});
    c.push_back({"Coin 1", "Coin", "Cabinet", false, "key:5, joy0:button9"});
    c.push_back({"Test", "Test", "Cabinet", false, "key:F2"});
    c.push_back({"Service", "Service", "Cabinet", false, "key:9"});
    return c;
}

// Keyboardmania: player 1's 24 keys (C1..B2) on two letter rows, as MAME maps them;
// player 2's octave 1 on the keypad (7 8 9 4 5 6 1 2 3 0 . Enter), octave 2 on
// Insert Home PageUp Delete End PageDown, the arrows (Left Down Right Up), / and *;
// the wheels up and down on P and M, the keypad's + and -
std::vector<Control> make_kbm() {
    std::vector<Control> c;
    static const char *notes[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    static const char *p1 = "QWERTYUIOASDFGHJKLZXCVBN";
    static const char *p2[24] = {"Keypad 7", "Keypad 8", "Keypad 9", "Keypad 4", "Keypad 5", "Keypad 6", "Keypad 1", "Keypad 2",
                                 "Keypad 3", "Keypad 0", "Keypad .", "Keypad Enter", "Insert", "Home", "PageUp", "Delete",
                                 "End", "PageDown", "Left", "Down", "Right", "Up", "Keypad /", "Keypad *"};
    for (int p = 1; p <= 2; p++) {
        std::string P = "P" + std::to_string(p), g = "Player " + std::to_string(p);
        for (int k = 0; k < 24; k++)
            c.push_back({P + " Key " + std::to_string(k + 1),
                         "Key " + std::to_string(k + 1) + " (" + notes[k % 12] + std::to_string(k / 12 + 1) + ")", g, false,
                         p == 1 ? std::string("key:") + p1[k] : std::string("key:") + p2[k]});
        c.push_back({p == 1 ? "1 Player Start" : "2 Players Start", "Start", g, false, p == 1 ? "key:1" : "key:2"});
        // the modulation wheel beside the keys (MAME's WHEEL_P1/P2, 0-255): on an analog axis, or
        // moved up and down by keys or buttons (runtime/host/input.cpp)
        c.push_back({P + " Wheel", "Wheel (analog axis)", g, true, ""});
        c.push_back({P + " Wheel Up", "Wheel up", g, false, p == 1 ? "key:P" : "key:Keypad +"});
        c.push_back({P + " Wheel Down", "Wheel down", g, false, p == 1 ? "key:M" : "key:Keypad -"});
    }
    c.push_back({"Coin 1", "Coin", "Cabinet", false, "key:5"});
    c.push_back({"Test", "Test", "Cabinet", false, "key:F2"});
    c.push_back({"Service", "Service", "Cabinet", false, "key:9"});
    return c;
}

// Dance Dance Revolution (System 573, MAME ksys573.cpp "ddr"): four panels per player on the
// GN845-PWB(B) stage, and the base panel's Start and buttons. A dance pad usually sends its
// arrows as buttons 0-3.
std::vector<Control> make_ddr() {
    std::vector<Control> c;
    const char *dirs[4] = {"Left", "Down", "Up", "Right"};
    const char *p1[4] = {"key:Left, joy0:button0", "key:Down, joy0:button1", "key:Up, joy0:button2", "key:Right, joy0:button3"};
    const char *p2[4] = {"key:Keypad 4, joy1:button0", "key:Keypad 2, joy1:button1", "key:Keypad 8, joy1:button2",
                         "key:Keypad 6, joy1:button3"};
    for (int p = 1; p <= 2; p++) {
        std::string P = "P" + std::to_string(p), g = "Player " + std::to_string(p);
        for (int k = 0; k < 4; k++) c.push_back({P + " " + dirs[k], std::string(dirs[k]) + " panel", g, false, p == 1 ? p1[k] : p2[k]});
        c.push_back({p == 1 ? "1 Player Start" : "2 Players Start", "Start", g, false, p == 1 ? "key:1, joy0:button9" : "key:2, joy1:button9"});
        for (int k = 1; k <= 3; k++) c.push_back({P + " Button " + std::to_string(k), "Panel button " + std::to_string(k), g, false, ""});
    }
    c.push_back({"Coin 1", "Coin", "Cabinet", false, "key:5, joy0:button8"});
    c.push_back({"Test", "Test", "Cabinet", false, "key:F2"});
    c.push_back({"Service", "Service", "Cabinet", false, "key:9"});
    return c;
}

// Dance Dance Revolution Solo (System 573, MAME ksys573.cpp "ddrsolo"): one player, six panels
// (up-left and up-right too), each read by two sensors ("P1 Left 1", "P1 Left 2"); a panel here
// presses both. Select L/R beside Start.
std::vector<Control> make_ddrsolo() {
    std::vector<Control> c;
    const char *dirs[6] = {"Left", "Down", "Up", "Right", "Up-Left", "Up-Right"};
    const char *keys[6] = {"key:Left, joy0:button0", "key:Down, joy0:button1", "key:Up, joy0:button2",
                           "key:Right, joy0:button3", "key:Q, joy0:button4", "key:E, joy0:button5"};
    for (int k = 0; k < 6; k++) c.push_back({std::string("P1 ") + dirs[k], std::string(dirs[k]) + " panel", "Player 1", false, keys[k]});
    c.push_back({"1 Player Start", "Start", "Player 1", false, "key:1, joy0:button9"});
    c.push_back({"P1 Select L", "Select left", "Player 1", false, "key:Z, joy0:button6"});
    c.push_back({"P1 Select R", "Select right", "Player 1", false, "key:C, joy0:button7"});
    c.push_back({"Coin 1", "Coin", "Cabinet", false, "key:5, joy0:button8"});
    c.push_back({"Test", "Test", "Cabinet", false, "key:F2"});
    c.push_back({"Service", "Service", "Cabinet", false, "key:9"});
    return c;
}

// Dance Maniax (System 573, MAME ksys573.cpp "dmx"): per player four hand sensors, the upper
// pair above the head and the lower pair at the waist, left and right. Each lower sensor is two
// beams (MAME's D0 and D1); a lower sensor here breaks both, and each beam has its own name too
// ("P1 D-Sensor D0 L"). Select L/R and Start on the panel. On the keyboard player 1's left hand
// takes W (upper) and S (lower), the right hand O and L; player 2 is on the keypad (7 4, 9 6).
std::vector<Control> make_dmx() {
    std::vector<Control> c;
    for (int p = 1; p <= 2; p++) {
        const std::string P = "P" + std::to_string(p), g = "Player " + std::to_string(p), j = p == 1 ? "joy0" : "joy1";
        const bool one = p == 1;
        c.push_back({P + " Upper Left", "Upper left sensor", g, false, (one ? "key:W, " : "key:Keypad 7, ") + j + ":button0"});
        c.push_back({P + " Lower Left", "Lower left sensor", g, false, (one ? "key:S, " : "key:Keypad 4, ") + j + ":button1"});
        c.push_back({P + " Lower Right", "Lower right sensor", g, false, (one ? "key:L, " : "key:Keypad 6, ") + j + ":button2"});
        c.push_back({P + " Upper Right", "Upper right sensor", g, false, (one ? "key:O, " : "key:Keypad 9, ") + j + ":button3"});
        c.push_back({P + " Select L", "Select left", g, false, (one ? "key:Left, " : "key:Keypad 1, ") + j + ":button4"});
        c.push_back({P + " Select R", "Select right", g, false, (one ? "key:Right, " : "key:Keypad 3, ") + j + ":button5"});
        c.push_back({one ? "1 Player Start" : "2 Players Start", "Start", g, false, (one ? "key:1, key:Return, " : "key:2, key:Keypad Enter, ") + j + ":button9"});
        for (const char *beam : {"D0 L", "D1 L", "D0 R", "D1 R"})
            c.push_back({P + " D-Sensor " + beam, std::string("Lower ") + (beam[3] == 'L' ? "left" : "right") + " sensor, beam " + beam[1] + " alone",
                         g + " (single beams)", false, ""});
    }
    c.push_back({"Coin 1", "Coin", "Cabinet", false, "key:5, joy0:button8"});
    c.push_back({"Test", "Test", "Cabinet", false, "key:F2"});
    c.push_back({"Service", "Service", "Cabinet", false, "key:9"});
    return c;
}

// Mambo a GoGo (System 573, MAME ksys573.cpp "mamboagg"): three congas, left, centre and right,
// each with three pads: 1 the far half of the head, 2 and 3 the near left and near right quarters.
// Each pad is two sensors, which MAME gives one button; a pad here presses both, and each sensor
// has its own name too ("Left Pad 1 (Top Left)"). The panel has Select left/right (konami573's P1
// Left/Right, which MAME's mamboagg ports keep) and Start: the music select. On the keyboard each conga is a triangle, the far pad above the near two:
// W over A S, Y over G H, O over K L. A Taiko-style drum sends its zones as buttons 0-8.
std::vector<Control> make_mambo() {
    std::vector<Control> c;
    const char *conga[3] = {"Left", "Centre", "Right"};
    const char *keys[3][3] = {{"key:W", "key:A", "key:S"}, {"key:Y", "key:G", "key:H"}, {"key:O", "key:K", "key:L"}};
    const char *pad[3] = {"pad 1 (far half)", "pad 2 (near left)", "pad 3 (near right)"};
    for (int k = 0; k < 3; k++)
        for (int z = 0; z < 3; z++)
            c.push_back({std::string(conga[k]) + " Pad " + std::to_string(z + 1), std::string(conga[k]) + " conga, " + pad[z],
                         std::string(conga[k]) + " conga", false, std::string(keys[k][z]) + ", joy0:button" + std::to_string(k * 3 + z)});
    c.push_back({"Select Left", "Select left", "Panel", false, "key:Left, joy0:button11"});
    c.push_back({"Select Right", "Select right", "Panel", false, "key:Right, joy0:button12"});
    c.push_back({"1 Player Start", "Start", "Panel", false, "key:1, key:Return, joy0:button9"});
    c.push_back({"Coin 1", "Coin", "Cabinet", false, "key:5, joy0:button10"});
    c.push_back({"Test", "Test", "Cabinet", false, "key:F2"});
    c.push_back({"Service", "Service", "Cabinet", false, "key:9"});
    static const char *sensors[9][2] = {
        {"Top Left", "Top Right"}, {"Middle Left", "Bottom Left"}, {"Middle Right", "Bottom Right"},
        {"Top Left", "Top Right"}, {"Middle Left", "Bottom Left"}, {"Middle Right", "Bottom Right"},
        {"Top Left", "Top Right"}, {"Middle Left", "Bottom Left"}, {"Middle Right", "Bottom Right"}};
    for (int k = 0; k < 9; k++)
        for (const char *s : sensors[k])
            c.push_back({std::string(conga[k / 3]) + " Pad " + std::to_string(k % 3 + 1) + " (" + s + ")",
                         std::string(conga[k / 3]) + " conga, pad " + std::to_string(k % 3 + 1) + ", sensor " + s + " alone",
                         "Single sensors", false, ""});
    return c;
}

// The e-AMUSEMENT card unit's slot and 10-key (System 573 GuitarFreaks and DrumMania with the
// network unit, runtime/sys573/cardunit.h; the names as pop'n's on Konami Python). `prefix`: ""
// for the first reader, "P2 " for GuitarFreaks' second, whose 10-key has no keys by default.
void add_card_unit(std::vector<Control> &c, const std::string &prefix, const std::string &card_key) {
    const std::string g = prefix.empty() ? "e-AMUSEMENT" : "e-AMUSEMENT (player 2)";
    const bool keys = prefix.empty();
    c.push_back({prefix + "Card", "e-AMUSEMENT card (insert / take back)", g, false, "key:" + card_key});
    for (int k = 0; k <= 9; k++)
        c.push_back({prefix + "Keypad " + std::to_string(k), "10-key " + std::to_string(k), g, false,
                     keys ? "key:Keypad " + std::to_string(k) : ""});
    c.push_back({prefix + "Keypad Enter", "10-key Enter", g, false, keys ? "key:Keypad Enter" : ""});
    c.push_back({prefix + "Keypad Clear", "10-key Clear", g, false, keys ? "key:Backspace" : ""});
    c.push_back({prefix + "Keypad .", "10-key .", g, false, keys ? "key:Keypad ." : ""});
}

// DrumMania (System 573, MAME ksys573.cpp "drmn"): five pads, the bass pedal, the select buttons
// and Start. On the keyboard the pads sit left to right as on the kit. With the network unit, the
// card unit.
std::vector<Control> make_drmn() {
    std::vector<Control> c = {
        {"Hi-Hat", "Hi-hat", "Drums", false, "key:S, joy0:button0"},
        {"Snare", "Snare", "Drums", false, "key:D, joy0:button1"},
        {"High Tom", "High tom", "Drums", false, "key:J, joy0:button2"},
        {"Low Tom", "Low tom", "Drums", false, "key:K, joy0:button3"},
        {"Cymbal", "Cymbal", "Drums", false, "key:L, joy0:button4"},
        {"Bass Drum", "Bass pedal", "Drums", false, "key:Space, joy0:button5"},
        {"Select L", "Select left", "Panel", false, "key:Left"},
        {"Select R", "Select right", "Panel", false, "key:Right"},
        {"1 Player Start", "Start", "Panel", false, "key:1, key:Return, joy0:button9"},
        {"Coin 1", "Coin", "Cabinet", false, "key:5, joy0:button8"},
        {"Test", "Test", "Cabinet", false, "key:F2"},
        {"Service", "Service", "Cabinet", false, "key:9"},
    };
    add_card_unit(c, "", "Insert");
    return c;
}

// GuitarFreaks (System 573, MAME ksys573.cpp "gtrfrks"): per guitar the neck's R, G and B, the
// pick (strum), wailing (tilt) and the effect knob; Start per player. On the keyboard player 1
// fingers A S D and picks with Space, player 2 J K L and Right Shift.
std::vector<Control> make_gtrf() {
    std::vector<Control> c;
    for (int p = 1; p <= 2; p++) {
        const std::string P = "P" + std::to_string(p), g = "Guitar " + std::to_string(p);
        const bool one = p == 1;
        c.push_back({P + " Button R", "Neck R (red)", g, false, one ? "key:A, joy0:button0" : "key:J, joy1:button0"});
        c.push_back({P + " Button G", "Neck G (green)", g, false, one ? "key:S, joy0:button1" : "key:K, joy1:button1"});
        c.push_back({P + " Button B", "Neck B (blue)", g, false, one ? "key:D, joy0:button2" : "key:L, joy1:button2"});
        c.push_back({P + " Pick", "Pick", g, false, one ? "key:Space, joy0:button3" : "key:Right Shift, joy1:button3"});
        c.push_back({P + " Wailing", "Wailing (tilt)", g, false, one ? "key:W, joy0:button4" : "key:I, joy1:button4"});
        c.push_back({P + " Effect Knob", "Effect knob (analog axis)", g, true, one ? "joy0:axis0" : "joy1:axis0"});
        c.push_back({one ? "1 Player Start" : "2 Players Start", "Start", g, false, one ? "key:1, joy0:button9" : "key:2, joy1:button9"});
    }
    c.push_back({"Coin 1", "Coin", "Cabinet", false, "key:5, joy0:button8"});
    c.push_back({"Test", "Test", "Cabinet", false, "key:F2"});
    c.push_back({"Service", "Service", "Cabinet", false, "key:9"});
    add_card_unit(c, "", "Insert");   // one card unit per guitar
    add_card_unit(c, "P2 ", "Delete");
    return c;
}

// The board's status LEDs (lamp_output_w, active low): every family has them.
void add_status(std::vector<Lamp> &l) {
    for (int k = 0; k < 8; k++) l.push_back({"Status LED " + std::to_string(k + 1), "Board (diagnostic)", 0, k, true});
}

void add_run(std::vector<Lamp> &l, const std::string &base, const std::string &group, int reg, std::vector<int> bits, int first = 1) {
    for (size_t k = 0; k < bits.size(); k++) l.push_back({base + " " + std::to_string(first + (int)k), group, reg, bits[k], false});
}

// DJ Main beatmania (MAME djmain.cpp beatmania_btn): five keys and a turntable
// per player, the effect button. The turntable is an 8-bit dial ("TT1/TT2").
std::vector<Control> make_djbm() {
    std::vector<Control> c;
    const char *p1keys[5] = {"key:Z, joy0:button0", "key:S, joy0:button1", "key:X, joy0:button2", "key:D, joy0:button3",
                             "key:C, joy0:button4"};
    const char *p2keys[5] = {"key:M", "key:K", "key:Comma", "key:L", "key:Period"};
    for (int p = 1; p <= 2; p++) {
        std::string P = "P" + std::to_string(p), g = "Player " + std::to_string(p);
        for (int k = 1; k <= 5; k++)
            c.push_back({P + " Button " + std::to_string(k), "Key " + std::to_string(k), g, false, p == 1 ? p1keys[k - 1] : p2keys[k - 1]});
        c.push_back({P + " Turntable", "Turntable (analog axis)", g, true, p == 1 ? "joy0:axis0" : ""});
        c.push_back({P + " Turntable -", "Turntable − (as a button)", g, false, p == 1 ? "key:Left, key:Left Shift" : "key:Down"});
        c.push_back({P + " Turntable +", "Turntable + (as a button)", g, false, p == 1 ? "key:Right, key:A" : "key:Up"});
        c.push_back({p == 1 ? "1 Player Start" : "2 Players Start", "Start", g, false, p == 1 ? "key:1, joy0:button8" : "key:2"});
    }
    c.push_back({"Effect", "Effect", "Cabinet", false, "key:E, joy0:button9"});   // as IIDX's (E2 on its controllers)
    c.push_back({"Coin 1", "Coin", "Cabinet", false, "key:5"});
    c.push_back({"Test", "Test", "Cabinet", false, "key:F2"});
    c.push_back({"Service", "Service", "Cabinet", false, "key:9"});
    c.push_back({"Reset", "Reset switch", "Cabinet", false, ""});
    return c;
}

// DJ Main pop'n music (MAME djmain.cpp popnmusic_btn): nine buttons, no start button
std::vector<Control> make_djpopn() {
    std::vector<Control> c = make_popn();
    c.push_back({"Reset", "Reset switch", "Cabinet", false, ""});
    return c;
}

// DJ Main pop'n stage (MAME djmain.cpp popnstage): ten buttons, and three
// selection buttons that MAME reads as the start buttons
std::vector<Control> make_djpstage() {
    std::vector<Control> c;
    const char *keys = "ZSXDCFVGBH";
    for (int k = 1; k <= 10; k++)
        c.push_back({"P1 Button " + std::to_string(k), "Button " + std::to_string(k), "Player", false,
                     std::string("key:") + keys[k - 1] + ", joy0:button" + std::to_string(k - 1)});
    c.push_back({"1 Player Start", "Select left", "Selection", false, "key:Left"});
    c.push_back({"2 Players Start", "Select OK (middle)", "Selection", false, "key:Return, key:1"});
    c.push_back({"3 Players Start", "Select right", "Selection", false, "key:Right"});
    c.push_back({"Coin 1", "Coin", "Cabinet", false, "key:5, joy0:button12"});
    c.push_back({"Test", "Test", "Cabinet", false, "key:F2"});
    c.push_back({"Service", "Service", "Cabinet", false, "key:9"});
    return c;
}

// beatmania IIDX on Twinkle (MAME twinkle.cpp INPUT_PORTS twinkle): seven keys and a turntable per
// player (the turntable is an 8-bit position, "1P Table"/"2P Table" in MAME), Start, VEFX, Effect,
// and the effector's five sliders (4 bits each). Keys as in the usual keyboard layout: Z S X D C F V
// and M K , L . ; / ; a USB IIDX controller's keys on buttons 0-6. The keys go by the other beatmania
// cabinets' names ("P1 Button 1", which the Twinkle core takes as MAME's "P1-1 (SW1)"), and the
// inputs they share have the same defaults: the launcher gives all three one bindings file.
std::vector<Control> make_iidx() {
    std::vector<Control> c;
    const char *p1keys[7] = {"key:Z, joy0:button0", "key:S, joy0:button1", "key:X, joy0:button2", "key:D, joy0:button3",
                             "key:C, joy0:button4", "key:F, joy0:button5", "key:V, joy0:button6"};
    const char *p2keys[7] = {"key:M", "key:K", "key:Comma", "key:L", "key:Period", "key:Semicolon", "key:Slash"};
    for (int p = 1; p <= 2; p++) {
        std::string P = "P" + std::to_string(p), g = "Player " + std::to_string(p);
        for (int k = 1; k <= 7; k++)
            c.push_back({P + " Button " + std::to_string(k), "Key " + std::to_string(k), g, false, p == 1 ? p1keys[k - 1] : p2keys[k - 1]});
        c.push_back({P + " Turntable", "Turntable (analog axis)", g, true, p == 1 ? "joy0:axis0" : ""});
        c.push_back({P + " Turntable -", "Turntable − (as a button)", g, false, p == 1 ? "key:Left, key:Left Shift" : "key:Down"});
        c.push_back({P + " Turntable +", "Turntable + (as a button)", g, false, p == 1 ? "key:Right, key:A" : "key:Up"});
        c.push_back({p == 1 ? "1 Player Start" : "2 Players Start", "Start", g, false, p == 1 ? "key:1, joy0:button8" : "key:2"});
    }
    c.push_back({"Effect", "Effect", "Cabinet", false, "key:E, joy0:button9"});
    c.push_back({"VEFX", "VEFX", "Cabinet", false, "key:W, joy0:button10"});
    for (int k = 1; k <= 5; k++)
        c.push_back({"Volume " + std::to_string(k), "Effector slider " + std::to_string(k) + " (analog axis)", "Effector", true, ""});
    c.push_back({"Coin 1", "Coin", "Cabinet", false, "key:5"});
    c.push_back({"Test", "Test", "Cabinet", false, "key:F2"});
    c.push_back({"Service", "Service", "Cabinet", false, "key:9"});
    // 9th style's (the C02 cabinet, runtime/pc/ezusb_c02.c): a card slot and 10-key each side;
    // player 2's 10-key has no keys by default
    add_card_unit(c, "", "Insert");
    add_card_unit(c, "P2 ", "Delete");
    return c;
}

// Twinkle's outputs (MAME twinkle.cpp), as the Twinkle core numbers them:
// reg 0 = I/O output 0x37 (active low), reg 1 = 0x87 (the spotlights, active low; MAME's spotlight0
// is bit 3, 1 bit 2, 2 bit 1, 3 bit 0, 4-7 bits 4-7), reg 2 = 0x8f (the neon, active low),
// reg 3 = the key lamps (key_led_w), reg 4 = the main board's LEDs (led_w, active low).
std::vector<Lamp> make_iidx_lamps() {
    std::vector<Lamp> l;
    add_run(l, "P1 Key Lamp", "Player 1", 3, {0, 1, 2, 3, 4, 5, 6});
    add_run(l, "P2 Key Lamp", "Player 2", 3, {7, 8, 9, 10, 11, 12, 13});
    l.push_back({"P1 Start Lamp", "Player 1", 0, 0, true});
    l.push_back({"P2 Start Lamp", "Player 2", 0, 1, true});
    l.push_back({"VEFX Lamp", "Panel", 0, 2, true});
    l.push_back({"Effect Lamp", "Panel", 0, 3, true});
    l.push_back({"Credit Lamp", "Panel", 0, 4, true});
    const int spot_bits[8] = {3, 2, 1, 0, 4, 5, 6, 7};
    for (int k = 0; k < 8; k++) l.push_back({"Spotlight " + std::to_string(k + 1), "Cabinet", 1, spot_bits[k], true});
    l.push_back({"Neon", "Cabinet", 2, 0, true});
    for (int k = 0; k < 9; k++) l.push_back({"Board LED " + std::to_string(k + 1), "Board (diagnostic)", 4, k, true});
    return l;
}

// DJ Main: reg 0 = 0x5d0000 (light_ctrl_1), reg 1 = 0x5d2000 (light_ctrl_2), as
// MAME decodes them for beatmania; reg 2 = the board's row of 8 LEDs (active low).
std::vector<Lamp> make_djbm_lamps() {
    std::vector<Lamp> l;
    add_run(l, "P1 Button Lamp", "Player 1", 0, {0, 1, 2, 3, 4});
    add_run(l, "P2 Button Lamp", "Player 2", 0, {6, 7, 8});
    add_run(l, "P2 Button Lamp", "Player 2", 1, {12, 13}, 4);
    l.push_back({"P1 Start Lamp", "Player 1", 1, 0});
    l.push_back({"P2 Start Lamp", "Player 2", 1, 1});
    l.push_back({"Effect Lamp", "Cabinet", 1, 2});
    l.push_back({"Left Red Highlight", "Highlights", 0, 10, true});
    l.push_back({"Left Blue Highlight", "Highlights", 0, 9, true});
    l.push_back({"Right Red Highlight", "Highlights", 0, 11, true});
    l.push_back({"Right Blue Highlight", "Highlights", 0, 5, true});
    l.push_back({"Neon (SSR)", "Cabinet", 1, 11});
    for (int k = 0; k < 8; k++) l.push_back({"Status LED " + std::to_string(k + 1), "Board (diagnostic)", 2, k, true});
    return l;
}

// pop'n music on DJ Main drives the same two registers. MAME doesn't name the
// bits for pop'n; this is what pop'n music 1 does with them (oracle/scripts/
// popn1j-play.his, in lockstep with MAME): each button's bit lights while it is
// pressed (reg 0: buttons 1-5 on bits 0-4, 6-9 on bits 6-9, as beatmania's two
// rows), bits 6-10 of reg 1 run a 5-step chase (the top lamps, as on Firebeat
// pop'n; which is topmost is not known) and bits 0-3 light patterns (4 side
// lamps, likewise). Reg 1 bit 15 pulses once per coin (the counter) and bit 14
// is the coin blocker; the other bits never change.
std::vector<Lamp> make_djpopn_lamps() {
    std::vector<Lamp> l;
    add_run(l, "Button Lamp", "Buttons", 0, {0, 1, 2, 3, 4, 6, 7, 8, 9});
    add_run(l, "Top Lamp", "Top", 1, {6, 7, 8, 9, 10});
    add_run(l, "Side Lamp", "Side", 1, {0, 1, 2, 3});
    for (int k = 0; k < 8; k++) l.push_back({"Status LED " + std::to_string(k + 1), "Board (diagnostic)", 2, k, true});
    return l;
}

// MAME firebeat_popn_state::lamp_output_popn_w
std::vector<Lamp> make_popn_lamps() {
    std::vector<Lamp> l;
    add_run(l, "Button Lamp", "Buttons", 0, {8, 9, 10, 11, 12, 13, 14, 15, 31});
    add_run(l, "Top Lamp", "Top (1 = topmost)", 0, {19, 20, 21, 22, 23});
    add_run(l, "Side Lamp", "Side pillars", 0, {27, 28, 29, 30});
    add_status(l);
    return l;
}

// pop'n music on Konami Python 1 (pop'n 9-14): word 8 of the JAMMA output command the
// game sends the interface board once a frame (runtime/python/core.cpp), read off the
// game's own LAMP CHECK (I/O CHECK in test mode) lamp by lamp, active high. The names
// are Firebeat pop'n's, so lights files carry over: SW-LAMP 1-9 are the buttons,
// HI-LAMP 1-5 the top lamps (which is topmost is not known), LEFT-LAMP 1-2 and
// RIGHT-LAMP 1-2 the side lamps. Bit 9 is never used.
std::vector<Lamp> make_pypopn_lamps() {
    std::vector<Lamp> l;
    add_run(l, "Button Lamp", "Buttons", 0, {0, 1, 2, 3, 4, 5, 6, 7, 8});
    add_run(l, "Top Lamp", "Top (test mode: HI-LAMP 1-5)", 0, {14, 13, 12, 11, 10});
    add_run(l, "Side Lamp", "Side (test mode: LEFT-LAMP 1-2, RIGHT-LAMP 1-2)", 0, {15, 16, 17, 18});
    return l;
}

// MAME firebeat_ppp_state::lamp_output_ppp_w, lamp_output2_ppp_w, lamp_output3_ppp_w
std::vector<Lamp> make_ppp_lamps() {
    std::vector<Lamp> l = {{"Left Lamp", "Cabinet", 0, 8}, {"Right Lamp", "Cabinet", 0, 9}, {"Door Lamp", "Cabinet", 0, 10},
                           {"OK Lamp", "Cabinet", 0, 11}, {"Slim Lamp", "Cabinet", 0, 15}};
    add_run(l, "Stage LED", "Stage", 0, {24, 25, 26, 27, 16, 17, 18, 19});
    add_run(l, "Top LED", "Top", 1, {16, 17, 18, 19, 0, 1, 2, 3});
    add_run(l, "Lamp", "Lamps", 2, {16, 18, 20, 22});
    add_status(l);
    return l;
}

// MAME firebeat_kbm_state::lamp_output_kbm_w
std::vector<Lamp> make_kbm_lamps() {
    std::vector<Lamp> l = {{"Start 1P Lamp", "Cabinet", 0, 24}, {"Start 2P Lamp", "Cabinet", 0, 25},
                           {"Door Lamp", "Cabinet", 0, 28}, {"Neon", "Cabinet", 0, 11}};
    add_run(l, "Lamp", "Lamps", 0, {8, 9, 10});
    add_status(l);
    return l;
}

// beatmania III: MAME decodes no cabinet lamps (lamp_output2_w/3_w only log)
std::vector<Lamp> make_bm3_lamps() {
    std::vector<Lamp> l;
    add_status(l);
    return l;
}

// DDR: the I/O board's outputs as MAME's ddr_output_callback names them. reg 0, bit = output
// number (analog: gx700pwbf latch * 8 + bit; digital: k573dio's); the speaker neon is 30 on the
// analog board and 28 on the digital one, both shown as 30.
std::vector<Lamp> make_ddr_lamps() {
    std::vector<Lamp> l;
    const char *dirs[4] = {"Up", "Left", "Right", "Down"};
    for (int p = 0; p < 2; p++)
        for (int k = 0; k < 4; k++)
            l.push_back({"P" + std::to_string(p + 1) + " Foot " + dirs[k], "Pad " + std::to_string(p + 1), 0, p * 8 + k});
    l.push_back({"P1 Start Lamp", "Cabinet", 0, 17});
    l.push_back({"P2 Start Lamp", "Cabinet", 0, 18});
    l.push_back({"Body Right Low", "Cabinet", 0, 20});
    l.push_back({"Body Left Low", "Cabinet", 0, 21});
    l.push_back({"Body Left High", "Cabinet", 0, 22});
    l.push_back({"Body Right High", "Cabinet", 0, 23});
    l.push_back({"Speaker Neon", "Cabinet", 0, 30});
    return l;
}

// DDR Solo: the outputs MAME's ddrsolo_output_callback names (reg 0, bit = output number)
std::vector<Lamp> make_ddrsolo_lamps() {
    return {
        {"Extra Ball 1", "Cabinet", 0, 10}, {"Extra Ball 2", "Cabinet", 0, 9}, {"Extra Ball 3", "Cabinet", 0, 11},
        {"Extra Ball 4", "Cabinet", 0, 8}, {"Speaker", "Cabinet", 0, 16}, {"Start Lamp", "Panel", 0, 20},
        {"Body Center", "Cabinet", 0, 21}, {"Body Right", "Cabinet", 0, 22}, {"Body Left", "Cabinet", 0, 23},
    };
}

// DrumMania: the digital board's outputs as MAME's drmn_output_callback names them (DrumMania
// 2nd and later). reg 0, bit = output number.
std::vector<Lamp> make_drmn_lamps() {
    return {
        {"Hi-Hat", "Drums", 0, 0}, {"High Tom", "Drums", 0, 1}, {"Low Tom", "Drums", 0, 2}, {"Snare", "Drums", 0, 3},
        {"Cymbal", "Drums", 0, 12}, {"Start Lamp", "Panel", 0, 13}, {"Select Lamp", "Panel", 0, 14},
        {"Spot Left/Right", "Cabinet", 0, 8}, {"Neon Top", "Cabinet", 0, 9}, {"Neon Woofer", "Cabinet", 0, 11},
    };
}

// GuitarFreaks: MAME's LAMPS port bits 7 to 4 (gtrfrks_lamps_b7..b4), as the board numbers them
std::vector<Lamp> make_gtrf_lamps() {
    return {
        {"Spot Left", "Cabinet", 0, 0}, {"Spot Right", "Cabinet", 0, 1},
        {"Start Lamp P1", "Panel", 0, 2}, {"Start Lamp P2", "Panel", 0, 3},
    };
}

// Dance Maniax: the outputs MAME's dmx_output_callback names (reg 0, bit = output number: each
// sensor's blue and red lamps, the spotlights) and its LAMPS port (dmx_lamps_b0..b5, bit 32 + n)
std::vector<Lamp> make_dmx_lamps() {
    std::vector<Lamp> l;
    static const int blue[12] = {16, 17, 20, 21, 8, 9, 4, 5, 0, 1, 12, 13}, red[12] = {19, 18, 23, 22, 11, 10, 7, 6, 3, 2, 15, 14};
    for (int k = 0; k < 12; k++) l.push_back({"Blue IO Lamp " + std::to_string(k), "Sensors (blue)", 0, blue[k]});
    for (int k = 0; k < 12; k++) l.push_back({"Red IO Lamp " + std::to_string(k), "Sensors (red)", 0, red[k]});
    l.push_back({"Spot Light Yellow", "Cabinet", 0, 28});
    l.push_back({"Spot Light Blue", "Cabinet", 0, 29});
    l.push_back({"Spot Light Pink", "Cabinet", 0, 31});
    l.push_back({"P1 Start Lamp", "Panel", 0, 33});
    l.push_back({"P2 Start Lamp", "Panel", 0, 36});
    l.push_back({"Left 1", "Cabinet", 0, 32});
    l.push_back({"Right 1", "Cabinet", 0, 34});
    l.push_back({"Left 0", "Cabinet", 0, 35});
    l.push_back({"Right 0", "Cabinet", 0, 37});
    return l;
}

// Mambo a GoGo: MAME's mamboagg_output_callback (reg 0, bit = output number) and its LAMPS port
// (mamboagg_lamps_b3..b5, bit 32 + n)
std::vector<Lamp> make_mambo_lamps() {
    return {
        {"Conga Left", "Congas", 0, 28}, {"Conga Centre", "Congas", 0, 31}, {"Conga Right", "Congas", 0, 29},
        {"Fire Lamp Left", "Cabinet", 0, 4}, {"Fire Fan Left", "Cabinet", 0, 5},
        {"Fire Fan Right", "Cabinet", 0, 6}, {"Fire Lamp Right", "Cabinet", 0, 7},
        {"Start Lamp", "Panel", 0, 35}, {"Select Left Lamp", "Panel", 0, 37}, {"Select Right Lamp", "Panel", 0, 36},
    };
}

// pop'n stage: not decoded yet (no dump to watch); listed by register and bit
std::vector<Lamp> make_djpstage_lamps() {
    std::vector<Lamp> l;
    for (int r = 0; r < 2; r++)
        for (int b = 0; b < 16; b++) l.push_back({"Light " + std::string(1, (char)('A' + r)) + std::to_string(b), "Lights (undecoded)", r, b});
    for (int k = 0; k < 8; k++) l.push_back({"Status LED " + std::to_string(k + 1), "Board (diagnostic)", 2, k, true});
    return l;
}

std::string trim(std::string s) {
    while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && isspace((unsigned char)s[i])) i++;
    return s.substr(i);
}

}  // namespace

const std::vector<Control> &controls(const std::string &family) {
    static const std::map<std::string, std::vector<Control>> all = {
        {"bm3", make_bm3()}, {"popn", make_popn()}, {"ppp", make_ppp()}, {"kbm", make_kbm()},
        {"djbm", make_djbm()}, {"djpopn", make_djpopn()}, {"djpstage", make_djpstage()}, {"ddr", make_ddr()}, {"ddrsolo", make_ddrsolo()},
        {"dmx", make_dmx()}, {"mambo", make_mambo()}, {"drmn", make_drmn()}, {"gtrf", make_gtrf()}, {"iidx", make_iidx()}, {"pypopn", make_pypopn()},
        {"pcpopn", make_pypopn()},   // the PC pop'n music games (runtime/pc/): the same controls
        {"pcpopnhc", make_pcpopnhc()}};
    auto f = all.find(family);
    return f != all.end() ? f->second : all.at("bm3");
}

const std::vector<Lamp> &lamps(const std::string &family) {
    static const std::map<std::string, std::vector<Lamp>> all = {
        {"bm3", make_bm3_lamps()}, {"popn", make_popn_lamps()}, {"ppp", make_ppp_lamps()}, {"kbm", make_kbm_lamps()},
        {"djbm", make_djbm_lamps()}, {"djpopn", make_djpopn_lamps()}, {"djpstage", make_djpstage_lamps()},
        {"ddr", make_ddr_lamps()}, {"ddrsolo", make_ddrsolo_lamps()}, {"dmx", make_dmx_lamps()}, {"mambo", make_mambo_lamps()}, {"drmn", make_drmn_lamps()}, {"gtrf", make_gtrf_lamps()}, {"iidx", make_iidx_lamps()},
        {"pypopn", make_pypopn_lamps()}, {"pcpopn", make_pypopn_lamps()}, {"pcpopnhc", make_pypopn_lamps()}};
    auto f = all.find(family);
    return f != all.end() ? f->second : all.at("bm3");
}

std::string default_bindings(const std::string &family) {
    std::string s;
    for (auto &c : controls(family))
        if (!c.defaults.empty()) s += c.name + " = " + c.defaults + "\n";
    return s;
}

std::vector<Entry> parse_list_file(const std::string &text) {
    std::vector<Entry> out;
    size_t pos = 0;
    int line_no = 0;
    while (pos < text.size()) {
        size_t e = text.find('\n', pos);
        std::string line = text.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        pos = e == std::string::npos ? text.size() : e + 1;
        line_no++;
        if (auto h = line.find('#'); h != std::string::npos) line = line.substr(0, h);
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        Entry en{trim(line.substr(0, eq)), {}, line_no};
        std::string rhs = line.substr(eq + 1);
        size_t p = 0;
        while (p <= rhs.size()) {
            size_t c = rhs.find(',', p);
            std::string b = trim(rhs.substr(p, c == std::string::npos ? std::string::npos : c - p));
            if (!b.empty()) en.items.push_back(b);
            if (c == std::string::npos) break;
            p = c + 1;
        }
        out.push_back(std::move(en));
    }
    return out;
}

std::string pad_name(const std::string &sdl_name) {
    std::string s;
    for (char c : sdl_name) s += (unsigned char)c < 0x20 || std::string("[]@,#=").find(c) != std::string::npos ? '_' : c;
    s = trim(s);
    return s.empty() ? "Controller" : s;
}

std::string format_entry(const std::string &name, const std::vector<std::string> &items) {
    std::string s = name + " =";
    for (size_t k = 0; k < items.size(); k++) s += (k ? ", " : " ") + items[k];
    return s;
}

}  // namespace hk::cab
