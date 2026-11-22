// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// The cores' base volume evened out (--even-volume, on by default; docs/PLAY.md, Sound levels): a trim in dB
// for each family of games, so that a song plays about as loud on one machine as on another. The
// machines make their sound at very different levels: the System 573's and Twinkle's hot CD and MP3
// masters (already cut by their boards' mix_gain), the PC games' DirectSound volumes summed with no
// headroom, DJ Main and Firebeat at MAME's own level, the Python core at the PS2's.
//
// The trim is the window's (host/window.h window_set_level): it multiplies the sound on its way to
// the device, with the player's own volume (--volume), after the DC blocker and before the clamp to
// full scale. --wav, --digest, the regress runs and the recorder (host/record.h) take the core's
// sound before it: none of them changes.
//
// A game that stands apart from its family has its own in its game.toml (replacing the family's):
//
//   [sound]
//   level_db = -2.5   # dB, from a measurement of the game's songs
//
// Older programs read no [sound] table (they look for their own keys and tables only), so a newer
// games folder plays as before in them.
#include <cmath>
#include <cstdlib>
#include <string>

namespace hk::level {

struct Trim {
    double db = 0;                 // the trim
    std::string what = "unknown";  // the family's name for the log ("System 573"), or the game's own
    bool known = false;            // a family of the table's, or the game's own level_db
    bool own = false;              // the game's own (game.toml [sound] level_db)
};

// The table. Measured 2026-11-22: BS.1770 integrated loudness (LUFS) of each family's songs, the
// loudest 60 s of a run, from the core's own samples (--wav); the trims bring them to about -18 LUFS.
//   core      family(ies)                  songs measured (LUFS)                         mean    trim
//   sys573    (all)                        ddr2m -10.5, ddr3mj -13.3, dstage -15.4       -13.1   -5
//   twinkle   (all)                        bmiidx3 -13.8, bmiidx8 -13.9                  -13.9   -4
//   pc        iidx (9th style to 16)       iidx16 -13.4, iidx12 -14.9 (demo)            -14.2   -4
//   pc        pcpopn (15 to 18, 28)        popn15 -15.9, popn18 -18.8, popn28 -16.1      -16.9   -1
//                                          (attract demo play)
//   firebeat  bm3, popn, ppp, kbm          bm3final -17.4, bm3 -19.1, popn7 -17.5        -18.3    0
//   python    (all)                        popn14 -17.6, popn12 -19.0, popn9 -21.2       -19.3   +1
//   djmain    (all)                        popn1j -20.2, bm1stmix -22.9                  -21.6   +3
//   pc        pcpopnhc (the 64-bit pop'n)  -22.7 from 25 s of demo only: not a song           0
// Peaks after the boosts (song stretches): DJ Main -7.0 dBFS + 3 = -4.0, Python -5.1 + 1 = -4.1;
// attract: DJ Main -7.4 + 3, Python -5.0 + 1. None reaches full scale.
// Anything else (the game menu, the ee'MALL station, a core not in the table): 0.
// `core` is game.toml's `core` (a Firebeat game's has none), `family` its `family`
inline Trim family_trim(const std::string &core, const std::string &family) {
    auto t = [](double db, const char *what) {
        Trim r;
        r.db = db, r.what = what, r.known = true;
        return r;
    };
    if (core == "sys573") return t(-5, "System 573");
    if (core == "twinkle") return t(-4, "Twinkle");
    if (core == "djmain") return t(+3, "DJ Main");
    if (core == "python") return t(+1, "Konami Python");
    if (core == "pc" && family == "iidx") return t(-4, "PC beatmania IIDX");
    if (core == "pc" && family == "pcpopn") return t(-1, "PC pop'n music");
    if (core == "pc" && family == "pcpopnhc") return t(0, "64-bit PC pop'n music");   // (no song measured yet)
    if ((core.empty() || core == "firebeat") && (family == "bm3" || family == "popn" || family == "ppp" || family == "kbm"))
        return t(0, "Firebeat");
    return Trim{};
}

// A game.toml's text: its top's `key = "value"` (before any [table]); "" if none
inline std::string top_value(const std::string &text, const std::string &key) {
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        at = end + 1;
        const size_t a = line.find_first_not_of(" \t\r");
        if (a == std::string::npos || line[a] == '#') continue;
        if (line[a] == '[') break;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(a, eq - a);
        k.erase(k.find_last_not_of(" \t") + 1);
        if (k != key) continue;
        const size_t q = line.find('"', eq), q2 = q == std::string::npos ? q : line.find('"', q + 1);
        return q2 == std::string::npos ? std::string() : line.substr(q + 1, q2 - q - 1);
    }
    return {};
}

// A game.toml's [sound] level_db into `db` (`has`: it's there). false with `err` (a line for the log)
// when the table has what can't be used: a key it doesn't know, a value not a number or out of
// -20..+20 dB; the family's trim then stays
inline bool parse_override(const std::string &text, double &db, bool &has, std::string &err) {
    has = false;
    bool in = false;
    size_t at = 0;
    for (int n = 1; at < text.size(); n++) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        at = end + 1;
        if (const size_t h = line.find('#'); h != std::string::npos) line.erase(h);
        const size_t a = line.find_first_not_of(" \t\r"), b = line.find_last_not_of(" \t\r");
        if (a == std::string::npos) continue;
        line = line.substr(a, b - a + 1);
        if (line[0] == '[') {
            in = line == "[sound]";
            continue;
        }
        if (!in) continue;
        auto fail = [&](const std::string &why) {
            err = "game.toml:" + std::to_string(n) + ": [sound]: " + why;
            has = false;
            return false;
        };
        const size_t eq = line.find('=');
        if (eq == std::string::npos) return fail("expected key = number");
        std::string key = line.substr(0, eq), val = line.substr(eq + 1);
        key.erase(key.find_last_not_of(" \t") + 1);
        val.erase(0, val.find_first_not_of(" \t"));
        if (key != "level_db") return fail("unknown key '" + key + "' (level_db)");
        char *e = nullptr;
        const double v = std::strtod(val.c_str(), &e);
        if (val.empty() || *e || !std::isfinite(v) || v < -20 || v > 20)
            return fail("level_db is a number of dB, -20 to 20");
        db = v, has = true;
    }
    return true;
}

// The trim for a game: its game.toml's own [sound] level_db, else its family's (`err` a line for
// the log when its [sound] can't be used). `on` false (--even-volume off): none
inline Trim for_game(const std::string &toml_text, bool on, std::string &err) {
    if (!on) return Trim{};
    Trim t = family_trim(top_value(toml_text, "core"), top_value(toml_text, "family"));
    double db = 0;
    bool has = false;
    if (parse_override(toml_text, db, has, err) && has) {
        t.db = db, t.known = true, t.own = true;
        if (t.what == "unknown") t.what = "this game";
    }
    return t;
}

}  // namespace hk::level
