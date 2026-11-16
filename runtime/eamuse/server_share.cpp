// SPDX-License-Identifier: GPL-3.0-or-later
// One card for all, pop'n music 9-18 (docs/EAMUSE3.md): clear marks and best scores shared (server.h).
#include "eamuse/server_parts.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <set>

namespace hk::ea {

// pop'n 12's player data (docs/EAMUSE3.md): get and new answer a binary record of 0x708 bytes as
// base64 in <b> (0x1917f8; decoded at 0x18f860 into 0x1867e68); set sends this credit's changes as
// attributes and children (0x1919e8), merged into the record here. The record, as the game fills
// and reads it: 0x00 the name (Shift-JIS, 13 bytes), 0x1b the puzzle position, 0x1c-0x33 six puzzle
// pieces (32-bit), 0x34 the password (four digits, a byte each), 0x38 the option, 0x3a the
// character (16-bit), from 0x3c a 16-bit word per song (2-bit clear marks per chart in bits 0-7,
// flags in 0x100-0x800: 0x1b3958), from 0x5f0 the network battle's (set's <netvs>). Scores are kept apart
// (playerdata score asks one chart's).
// One card for all: shared clear marks (docs/EAMUSE3.md, "One card for all"). All ten games number
// their songs alike (checked by their song tables), and every chart's clear mark is two bits, 1 a
// clear and 2-3 the better medals, in pop'n 12's order: song word bits 0-1, 2-3, 4-5, 6-7 for
// the four charts (12-18: their words' low byte; pop'n 13 shows a 12 record so, 0x1812f98).
// pop'n 10 and 11 keep the last two charts the other way round (11 -> 12 swaps them, 0x1a0a38).
// pop'n 9 keeps only a cleared bit per chart, byte 0x416 + song, bits 0-3 in pop'n 12's order
// (pop'n 10 makes them its flags 0x100, 0x200, 0x800, 0x400 and pop'n 11 those marks of 1). The
// marks travel; each game's own bits (flags, scores) stay its own.
namespace {

// the songs each game has (its song table's entries but the "-" ones left in place of songs taken
// out; pop'n 14 renamed some genres and comments, the songs the same)
struct SongRanges { const char *game; std::vector<std::pair<int, int>> ranges; };
const SongRanges SONGS[] = {
    {"C00", {{0, 251}, {253, 256}, {258, 324}}},
    {"C20", {{0, 389}}},
    {"D11", {{0, 427}, {429, 449}}},
    {"E12", {{0, 243}, {247, 251}, {253, 253}, {255, 256}, {258, 259}, {261, 261}, {264, 327}, {330, 336}, {339, 391}, {393, 427}, {429, 594}}},
    {"E13", {{0, 243}, {247, 251}, {253, 253}, {255, 256}, {258, 259}, {261, 261}, {264, 327}, {330, 336}, {339, 391}, {393, 427}, {429, 648}}},
    {"F14", {{0, 243}, {247, 251}, {253, 253}, {255, 256}, {258, 259}, {261, 261}, {264, 327}, {330, 336}, {339, 391}, {393, 427}, {429, 699}}},
    // pop'n 15-18 number songs as 14 (15's summary of a pop'n 14 player, 0x482d70, reads both
    // records' words by the same index; 16-18's song tables are 15's with songs added and others
    // taken out: 15 0x5084f0, 16 0x523ab8, 17 0x52d028, 18 0x55adc0 in their last programs, a song
    // taken out flagged 0x80000000). Their records have room for 763, 839, 907 and 976 songs
    {"G15", {{0, 38}, {40, 57}, {59, 66}, {68, 90}, {92, 140}, {142, 169}, {171, 174}, {176, 231}, {233, 243}, {247, 253}, {256, 256}, {258, 259}, {261, 261}, {264, 323}, {325, 327}, {330, 336}, {339, 365}, {367, 391}, {393, 427}, {429, 436}, {446, 472}, {474, 762}}},   // 728 songs
    {"H16", {{0, 38}, {40, 57}, {59, 66}, {68, 90}, {92, 140}, {142, 169}, {171, 174}, {176, 231}, {235, 235}, {239, 240}, {242, 242}, {247, 253}, {256, 256}, {258, 258}, {261, 261}, {264, 279}, {281, 300}, {307, 323}, {325, 327}, {330, 333}, {335, 335}, {339, 365}, {367, 391}, {393, 397}, {400, 427}, {429, 436}, {446, 472}, {474, 537}, {539, 540}, {542, 595}, {597, 599}, {601, 649}, {651, 838}}},   // 780 songs
    {"I17", {{0, 0}, {2, 5}, {8, 10}, {12, 18}, {20, 21}, {23, 35}, {38, 38}, {40, 40}, {42, 45}, {47, 53}, {55, 57}, {59, 59}, {61, 66}, {68, 74}, {76, 86}, {88, 90}, {92, 98}, {100, 101}, {103, 108}, {110, 111}, {113, 116}, {118, 120}, {122, 140}, {142, 149}, {151, 169}, {171, 174}, {176, 186}, {188, 189}, {192, 196}, {199, 231}, {235, 235}, {240, 240}, {242, 242}, {248, 252}, {256, 256}, {258, 258}, {261, 261}, {264, 267}, {269, 269}, {271, 279}, {283, 295}, {298, 300}, {307, 323}, {326, 327}, {330, 333}, {340, 358}, {360, 362}, {364, 365}, {367, 391}, {393, 394}, {397, 397}, {401, 403}, {405, 406}, {408, 413}, {415, 427}, {429, 436}, {446, 449}, {451, 456}, {458, 460}, {462, 464}, {466, 471}, {475, 479}, {483, 488}, {490, 492}, {495, 507}, {509, 509}, {511, 515}, {517, 522}, {524, 530}, {532, 537}, {539, 540}, {542, 542}, {544, 563}, {565, 583}, {585, 595}, {597, 599}, {601, 649}, {651, 752}, {754, 754}, {756, 815}, {818, 823}, {825, 906}}},   // 777 songs
    {"J39", {{0, 0}, {2, 5}, {8, 10}, {12, 18}, {20, 21}, {23, 35}, {38, 38}, {40, 40}, {43, 45}, {47, 53}, {55, 55}, {57, 57}, {61, 66}, {68, 74}, {76, 76}, {78, 79}, {81, 81}, {83, 86}, {88, 90}, {92, 98}, {100, 101}, {103, 108}, {110, 111}, {113, 113}, {115, 116}, {118, 120}, {122, 140}, {142, 149}, {151, 169}, {171, 174}, {176, 186}, {188, 189}, {192, 196}, {199, 231}, {240, 240}, {242, 242}, {248, 249}, {252, 252}, {256, 256}, {258, 258}, {264, 267}, {269, 269}, {271, 279}, {283, 295}, {298, 300}, {307, 323}, {327, 327}, {333, 333}, {340, 357}, {360, 362}, {364, 365}, {367, 389}, {391, 391}, {393, 394}, {401, 403}, {405, 406}, {408, 413}, {415, 427}, {429, 429}, {431, 436}, {446, 449}, {451, 456}, {458, 460}, {462, 464}, {466, 471}, {475, 479}, {484, 488}, {490, 492}, {495, 507}, {509, 509}, {511, 515}, {517, 522}, {524, 530}, {532, 537}, {539, 539}, {544, 563}, {565, 583}, {585, 595}, {597, 599}, {601, 649}, {651, 672}, {674, 724}, {726, 752}, {754, 754}, {756, 762}, {764, 815}, {818, 823}, {825, 905}, {907, 975}}},   // 820 songs
};

bool has_song(const std::string &game, size_t n) {
    for (const SongRanges &g : SONGS)
        if (game == g.game) {
            for (const auto &[a, b] : g.ranges)
                if ((int)n >= a && (int)n <= b) return true;
            return false;
        }
    return false;
}

constexpr size_t SONG_MAX = 1024;

uint8_t swap34(uint8_t m) { return (uint8_t)((m & 0x0f) | (m & 0x30) << 2 | (m & 0xc0) >> 2); }

uint8_t best(uint8_t a, uint8_t b) {   // per chart, the better mark
    uint8_t o = 0;
    for (int c = 0; c < 8; c += 2) o |= (uint8_t)(std::max(a >> c & 3, b >> c & 3) << c);
    return o;
}

// where a game keeps its song words in its player data: offset, bytes a word, and whether the
// data is a userdata blob (pop'n 9-11) or a record (12-14)
struct Words { size_t at, bytes; };
Words words_of(const std::string &game) {
    if (game == "C20") return {0x3fc, 2};
    if (game == "D11") return {0x410, 2};
    if (game == "E12") return {0x3c, 2};
    if (game == "E13") return {0x28, 4};
    if (game == "F14") return {0x24, 4};
    if (game == "G15") return {0x58, 2};
    if (game == "H16") return {0x50, 2};   // 16-18 set a chart's mark as 15 does (16 0x448cf5, 18 0x4613fa)
    if (game == "I17") return {0x54, 2};
    if (game == "J39") return {0x68, 2};
    return {0x416, 1};   // C00: a byte a song
}

// a song's marks as the game keeps them, in pop'n 12's order
uint8_t marks_in(const std::string &game, const std::string &d, size_t n) {
    const Words w = words_of(game);
    const size_t at = w.at + w.bytes * n;
    if (at + w.bytes > d.size()) return 0;
    const unsigned lo = (uint8_t)d[at], hi = w.bytes > 1 ? (uint8_t)d[at + 1] : 0;
    if (game == "C00") {   // cleared bits
        uint8_t m = 0;
        for (int c = 0; c < 4; c++) if (lo >> c & 1) m |= (uint8_t)(1 << 2 * c);
        return m;
    }
    if (game == "C20") {   // marks (11's order) and the cleared flags of a pop'n 9 player
        uint8_t m = swap34((uint8_t)lo);
        static const int flag_chart[4] = {0, 1, 3, 2};   // 0x100, 0x200, 0x400, 0x800
        for (int b = 0; b < 4; b++)
            if (hi >> b & 1) m = best(m, (uint8_t)(1 << 2 * flag_chart[b]));
        return m;
    }
    if (game == "D11") return swap34((uint8_t)lo);
    return (uint8_t)lo;
}

// a song's marks put into the game's data: each chart the better of the two
void marks_out(const std::string &game, std::string &d, size_t n, uint8_t m) {
    const Words w = words_of(game);
    const size_t at = w.at + w.bytes * n;
    if (at + w.bytes > d.size() || !m) return;
    if (game == "C00") {
        uint8_t b = (uint8_t)d[at];
        if (b == 0xff) b = 0;   // never played
        for (int c = 0; c < 4; c++) if (m >> 2 * c & 3) b |= (uint8_t)(1 << c);
        d[at] = (char)b;
        return;
    }
    const uint8_t mine = game == "C20" || game == "D11" ? swap34(m) : m;
    d[at] = (char)best((uint8_t)d[at], mine);
}

}  // namespace

std::vector<uint8_t> Server::shared_marks(const std::string &card) const {
    std::vector<uint8_t> marks(SONG_MAX, 0);
    if (!safe_name(card)) return marks;
    std::string ref;   // pop'n 13 and 14 keep their players by ref id
    for (const Record &c : from_lines(read_file("cards")))
        if (attr(c, "cardid") == card) ref = attr(c, "refid");
    struct Source { const char *game; std::string data; };
    const Source sources[] = {
        {"C00", unbase64_b(read_file("userdata-" + card))},
        {"C20", unbase64_b(read_file("userdata-C20-" + card))},
        {"D11", unbase64_b(read_file("userdata-D11-" + card))},
        {"E12", read_file("pdata-E12-" + card)},
        {"E13", safe_name(ref) ? read_file("pdata-E13-" + ref) : ""},
        {"F14", safe_name(ref) ? read_file("pdata-F14-" + ref) : ""},
        {"G15", safe_name(ref) ? read_file("pdata-G15-" + ref) : ""},
        {"H16", safe_name(ref) ? read_file("pdata-H16-" + ref) : ""},
        {"I17", safe_name(ref) ? read_file("pdata-I17-" + ref) : ""},
        {"J39", safe_name(ref) ? read_file("pdata-J39-" + ref) : ""},
    };
    for (const Source &s : sources)
        for (size_t n = 0; n < SONG_MAX && !s.data.empty(); n++)
            if (has_song(s.game, n)) marks[n] = best(marks[n], marks_in(s.game, s.data, n));
    return marks;
}

// data: the game's own player data (a userdata answer with <b> pieces for pop'n 9-11, the raw
// record for 12-14), returned with the card's shared marks put in
std::string Server::with_shared_marks(const std::string &game, const std::string &card, std::string data) const {
    const std::vector<uint8_t> marks = shared_marks(card);
    const bool blob = game == "C00" || game == "C20" || game == "D11";
    std::string d = blob ? unbase64_b(data) : data;
    for (size_t n = 0; n < SONG_MAX; n++)
        if (has_song(game, n)) marks_out(game, d, n, marks[n]);
    if (!blob) return d;
    // back into the same <b> pieces
    std::string out;
    size_t at = 0, from = 0;
    for (size_t b = data.find("<b>"); b != std::string::npos; b = data.find("<b>", b + 3)) {
        const size_t e = data.find("</b>", b);
        if (e == std::string::npos) break;
        const size_t n = unbase64_b(data.substr(b, e + 4 - b)).size();
        out += data.substr(from, b + 3 - from) + base64(d.substr(at, n));
        at += n;
        from = e;
    }
    return out + data.substr(from);
}

// Scores (docs/EAMUSE3.md, "Shared scores"): pop'n 12, 13 and 14 number their charts alike, the
// sheet sent being the chart (0x1b3958, 0x1810e60, 0x17fdfa0: the same code in all three): 0-3
// Normal, Hyper, 5 buttons, EX (the song word's two-bit places), 5 one mode's; 13 and 14 play
// their own 5-button charts as 6 (GRADIUS: 167 notes, 12's sheet 2 313), and have a 7. pop'n
// 9-11 keep no scores (their players, 1.6-2.6 KB, have room for the marks only). 15-18 number
// them as 14 (seven a song: each song's level by sheet at the same place in their song tables,
// 15 0x4759ef, 18 0x470f08). So a game asking a chart's best gets its own or another's of the
// card, same song and sheet, for songs both games have. (Konami re-rated many charts' levels from
// one game to the next, the charts the same; the songs pop'n 14 gave new ids are its own.)
namespace {

const char *const SCORED[] = {"E12", "E13", "F14", "G15", "H16", "I17", "J39"};

// the songs pop'n 14 gave new ids (and some new levels): its charts of them, and 15-18's, their own
const std::set<long> F14_ANEW = {450, 451, 452, 455, 457, 460, 461, 462, 465, 466, 469, 477, 478, 481, 484, 485, 487, 492};

bool same_chart(const std::string &a, const std::string &b, long n) {
    if (a == b) return true;
    const bool renumbered_a = a != "E12" && a != "E13", renumbered_b = b != "E12" && b != "E13";
    return has_song(a, (size_t)n) && has_song(b, (size_t)n) && !(renumbered_a != renumbered_b && F14_ANEW.count(n));
}

// pop'n 18's <hiscore>: 17 bits a chart, seven a song, least significant bit first (0x456570)
constexpr size_t HISCORES = 0x1ab7, HISCORE_BITS = 17;
long hiscore_at(const std::string &h, size_t i) {
    long v = 0;
    for (size_t b = 0; b < HISCORE_BITS; b++) {
        const size_t bit = i * HISCORE_BITS + b;
        if (bit / 8 < h.size() && ((uint8_t)h[bit / 8] >> bit % 8 & 1)) v |= 1L << b;
    }
    return v;
}

}  // namespace

// a player's best scores in one game, by song and sheet: 12-15's kept here a line a chart
// (pscore-<game>-<who>), 16's and 17's in their records (32 bits a chart, chart-major: 16 0x8dc,
// 846 a chart; 17 0xcd4, 950), 18's as its <hiscore> (pscore-J39-<who>)
std::map<std::pair<long, long>, long> Server::scores_of(const std::string &game, const std::string &who) const {
    std::map<std::pair<long, long>, long> out;
    if (!safe_name(who)) return out;
    if (game == "H16" || game == "I17") {
        const std::string rec = read_file("pdata-" + game + "-" + who);
        const size_t at = game == "H16" ? 0x8dc : 0xcd4, per = game == "H16" ? 846 : 950;
        for (size_t sheet = 0; sheet < 7; sheet++)
            for (size_t n = 0; n < per; n++) {
                const size_t p = at + 4 * (sheet * per + n);
                if (p + 4 > rec.size()) return out;
                const long v = (long)((uint32_t)(uint8_t)rec[p] | (uint32_t)(uint8_t)rec[p + 1] << 8 |
                                      (uint32_t)(uint8_t)rec[p + 2] << 16 | (uint32_t)(uint8_t)rec[p + 3] << 24);
                if (v > 0) out[{(long)n, (long)sheet}] = v;
            }
        return out;
    }
    if (game == "J39") {
        const std::string h = read_file("pscore-J39-" + who);
        for (size_t i = 0; i < HISCORES && (i + 1) * HISCORE_BITS <= h.size() * 8; i++)
            if (const long v = hiscore_at(h, i); v > 0) out[{(long)(i / 7), (long)(i % 7)}] = v;
        return out;
    }
    for (const Record &e : from_lines(read_file("pscore-" + game + "-" + who))) {
        long &v = out[{num(e, "num"), num(e, "sheet")}];
        v = std::max(v, num(e, "score"));
    }
    return out;
}

// the player's key in each game: pop'n 12's the card, 13 on the ref id
static void card_and_ref(const std::vector<Record> &cards, const std::string &game,
                         const std::string &key, std::string &card, std::string &ref) {
    card = game == "E12" ? key : "", ref = game == "E12" ? "" : key;
    for (const Record &c : cards) {
        if (game == "E12" && attr(c, "cardid") == key) ref = attr(c, "refid");
        if (game != "E12" && attr(c, "refid") == key) card = attr(c, "cardid");
    }
}

long Server::shared_score(const std::string &game, const std::string &key, const std::string &song, const std::string &sheet) const {
    std::string card, ref;
    card_and_ref(from_lines(read_file("cards")), game, key, card, ref);
    const long n = std::strtol(song.c_str(), nullptr, 10), s = std::strtol(sheet.c_str(), nullptr, 10);
    long best = 0;
    for (const char *g : SCORED) {
        if (!same_chart(game, g, n)) continue;
        const auto scores = scores_of(g, std::string(g) == "E12" ? card : ref);
        if (const auto it = scores.find({n, s}); it != scores.end()) best = std::max(best, it->second);
    }
    return best;
}

std::map<std::pair<long, long>, long> Server::shared_scores(const std::string &game, const std::string &key) const {
    std::string card, ref;
    card_and_ref(from_lines(read_file("cards")), game, key, card, ref);
    std::map<std::pair<long, long>, long> best;
    for (const char *g : SCORED) {
        if (game == g) continue;
        for (const auto &[chart, v] : scores_of(g, std::string(g) == "E12" ? card : ref))
            if (same_chart(game, g, chart.first)) best[chart] = std::max(best[chart], v);
    }
    return best;
}

}  // namespace hk::ea
