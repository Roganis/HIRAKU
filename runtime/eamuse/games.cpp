// SPDX-License-Identifier: GPL-3.0-or-later
// The games HIRAKU's e-AMUSE3 server answers (games.h).
#include "eamuse/games.h"
#include "eamuse/server.h"

namespace hk::ea {

// Adding a game: its answers as members of Server in a server_<family>.cpp (or an existing family's
// file, server.h names them), one row here, its section in docs/EAMUSE3.md, its calls in a
// transcript (tests/eamuse/transcripts/). Each chain member answers what is its own and leaves the
// rest ("") to the next; most chains end in the later pop'n games' shared answers (answer_later:
// services, cards, the shop) and pop'n 9's (answer_shared, answer_popn9), which every game falls
// back to. A model with a '?' matches after every exact row: a later pop'n with a ?39 model (K39,
// L39) needs its own exact row, or it is taken for a System 573 GuitarFreaks.
const std::vector<Game> &games() {
    using S = Server;
    static const Answer later = &S::answer_later, shared = &S::answer_shared, popn9 = &S::answer_popn9;
    static const Answer c02 = &S::answer_c02, red = &S::answer_iidx_red, fdd = &S::answer_iidx_fdd, iidx_userdata = &S::answer_iidx_userdata;
    static const std::vector<Game> table = {
        {"popn9", "pop'n music 9", "C00", Family::popn9, {shared, popn9}},
        {"popn10", "pop'n music 10", "C20", Family::popn, {later, shared, popn9}},
        {"popn11", "pop'n music 11", "D11", Family::popn, {later, shared, popn9}},
        {"popn12", "pop'n music 12", "E12", Family::popn, {later, shared, popn9}},
        {"popn13", "pop'n music 13", "E13", Family::popn, {later, shared, popn9}},
        {"popn14", "pop'n music 14", "F14", Family::popn, {later, shared, popn9}},
        {"popn15", "pop'n music 15", "G15", Family::popn_pc, {&S::answer_g15, later, shared, popn9}},
        {"popn16", "pop'n music 16", "H16", Family::popn_pc, {&S::answer_j39, later, shared, popn9}},
        {"popn17", "pop'n music 17", "I17", Family::popn_pc, {&S::answer_j39, later, shared, popn9}},
        {"popn18", "pop'n music 18", "J39", Family::popn_pc, {&S::answer_j39, later, shared, popn9}},
        {"popn-pc-later", "the later PC pop'n games", "M39", Family::popn_pc_later, {&S::answer_hc, &S::answer_p24, later, shared, popn9}},
        {"iidx9", "beatmania IIDX 9th style", "C02", Family::iidx9, {c02, iidx_userdata, shared, popn9}},
        {"iidx10", "beatmania IIDX 10th style", "D01", Family::iidx10_13, {c02, red, iidx_userdata, shared, popn9}},
        {"iidx11", "beatmania IIDX RED", "E11", Family::iidx10_13, {c02, red, iidx_userdata, shared, popn9}},
        {"iidx12", "beatmania IIDX HAPPY SKY", "ECO", Family::iidx10_13, {c02, red, iidx_userdata, shared, popn9}},
        {"iidx13", "beatmania IIDX DistorteD", "FDD", Family::iidx10_13, {c02, red, fdd, later, iidx_userdata, shared, popn9}},
        {"iidx14", "beatmania IIDX GOLD", "GLD", Family::iidx14_16, {fdd, later, shared, popn9}},
        {"iidx15", "beatmania IIDX DJ TROOPERS", "HDD", Family::iidx14_16, {fdd, later, shared, popn9}},
        {"iidx16", "beatmania IIDX EMPRESS", "I00", Family::iidx14_16, {fdd, later, shared, popn9}},
        {"drmnvj", "DrumMania V", "E02", Family::gfdm_v, {&S::answer_gfdmv, later, shared, popn9}},
        {"drmnv2j", "DrumMania V2", "F02", Family::gfdm_v, {&S::answer_gfdmv, later, shared, popn9}},
        {"", "GuitarFreaks V", "E03", Family::gfdm_v, {&S::answer_gfdmv, later, shared, popn9}},
        {"", "GuitarFreaks V2", "F03", Family::gfdm_v, {&S::answer_gfdmv, later, shared, popn9}},
        {"gfdm573-gf", "the System 573 GuitarFreaks", "?39", Family::gfdm_573, {&S::answer_gfdm, shared}},
        {"gfdm573-dm", "the System 573 DrumMania", "?40", Family::gfdm_573, {&S::answer_gfdm, shared}},
        // last: a model no row above names, answered as a later pop'n
        {"", "a game the table doesn't name", "", Family::unknown, {later, shared, popn9}},
    };
    return table;
}

namespace {
bool matches(const char *pattern, const std::string &model, bool wild) {
    if (model.size() < 3) return false;
    bool has_wild = false;
    for (int i = 0; i < 3; i++) {
        if (!pattern[i]) return false;
        if (pattern[i] == '?') has_wild = true;
        else if (pattern[i] != model[i]) return false;
    }
    return has_wild == wild;
}
}  // namespace

const Game &game_of(const std::string &model) {
    const std::vector<Game> &t = games();
    // a call with no model: pop'n 9's (the first row), as the server always took it
    if (model.empty()) return t.front();
    for (bool wild : {false, true})
        for (const Game &g : t)
            if (matches(g.model, model, wild)) return g;
    return t.back();
}

}  // namespace hk::ea
