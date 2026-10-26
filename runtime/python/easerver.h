// SPDX-License-Identifier: GPL-3.0-or-later
// A local e-AMUSE3 server for Python 1 pop'n (PLAN.md section 23, "e-AMUSEMENT"): what
// Konami's servers answered in 2003, from the game's own requests and response parsers.
//
// The protocol is the one later e-AMUSEMENT games use (bemaniutils documents it): an HTTP/1.0
// POST whose body is XML, RC4-encrypted with key = MD5(the 6 bytes of "X-Eamuse-Info:
// 1-xxxxxxxx-xxxx" + a shared secret). A request is <call srcid=.. model=..><MODULE
// method=.. .../></call>; the answer <response><MODULE status="0" ...>...</MODULE></response>.
// Modules: services (the others' URLs), numbering, package, userdata, userid, rank,
// p_friend, eemall, posevent, pcbtracker, ntp.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "eamuse/protocol.h"

namespace hk::py {

class EamuseServer {
public:
    // dir: where the server keeps what it must remember (cards, player data), or "" for nothing
    explicit EamuseServer(std::string dir) : dir_(std::move(dir)) {}

    // One HTTP request (head and body, as sent) to the whole HTTP response.
    std::string handle(const std::string &request);

    // Each call in clear (request and answer), for HK_PY_NET_LOG.
    std::function<void(const std::string &)> log;
    // Each call's module and method as it comes ("playerdata.get"; the core's scripts wait on them)
    std::function<void(const std::string &)> on_call;
    // A card the server knows and holds a PIN for was read (cardmng.inquire): its PIN, for HIRAKU to
    // type when the game asks (-autopin)
    std::function<void(const std::string &pin)> on_card_pin;
    // The cabinet's wall clock, as seconds since 1970 read as UTC (ntp.get sends it as is, and the
    // game sets its clock to it): in the core, the emulated clock (reproducible runs); in
    // hiraku-eamuse, the host's clock in Japan time unless told otherwise.
    std::function<int64_t()> now;

    // The protocol's own pieces, public for tools and tests.
    static std::vector<uint8_t> key(const std::string &info);   // "1-xxxxxxxx-xxxx"
    static void rc4(const std::vector<uint8_t> &key, std::string &data);
    // An HTTP message (request or answer) as text: its first line and its body in clear.
    static std::string clear(const std::string &message);

    // For what HIRAKU changes in a cabinet's calls (spice2x's -p and -card0, shell/eaoptions.h):
    // an HTTP message's body handed to `fn` in clear, then put back as it came (encrypted again
    // under its own X-Eamuse-Info) with its Content-Length.
    static void edit(std::string &message, const std::function<void(std::string &body)> &fn);
    // Every name="..." in xml set to value.
    static void set_attr(std::string &xml, const std::string &name, const std::string &value);
    // An answer to `request` with this XML (<response>...</response>), in kind as handle()'s
    // (binary: the call was binary XML, and so is the answer)
    std::string reply(const std::string &request, std::string xml, bool binary = false);

public:
    struct Board;   // a ranking (easerver_rank.cpp)

private:
    // A call and its module elements (eamuse/protocol.h)
    using Request = ea::Request;
    using Call = ea::Call;
    static bool parse(const std::string &xml, Call &c);
    std::string answer(const Call &c, const Request &r, const std::string &host);
    std::string message() const;   // the owner's title-screen message (package "message"), or ""

    struct Entered {   // a result entered on a board (enter)
        std::map<std::string, std::string> kept;   // the entrant's best, as kept
        size_t place, entries;
        bool update;   // the result is the new best
    };
    Entered enter(const Board &b, const Request &r);
    std::string rank_register(const Board &b, const Request &r);
    std::string next_number();   // a new card's number (numbering.assign, cardmng)
    // pop'n 10-14's rank.get tables (15 courses, then the shops' board); as <b> pieces
    std::string rank_table(const std::string &game);
    std::string rank_b(const std::string &game);
    void rank_shop(const std::string &game, const Request &r, const char *mask, const char *mask_hi);
    // pop'n 10 on: their own calls and answers ("" leaves the call to pop'n 9's)
    std::string answer_later(const std::string &game, const Request &r, const std::string &host);
    // GuitarFreaks and DrumMania V (Konami Python 2: E02, F02, E03, F03), "" for the rest
    std::string answer_gfdmv(const std::string &game, const Request &r);
    std::string answer_game(const std::string &game, const Request &r);
    std::string answer_playerdata(const std::string &game, const Request &r);
    // pop'n 15 (G15, a PC game: runtime/pc/): <G15 method="type_method" ...>
    std::string answer_g15(const Request &r);
    // pop'n 16, 17 and 18 (H16, I17, J39, PC games: runtime/pc/): modules game and playerdata, binary XML
    std::string answer_j39(const std::string &game, const Request &r);
    // the 64-bit PC pop'n (M39: runtime/pc64/): its module "player"
    std::string answer_hc(const Request &r);
    std::string answer_p24(const Request &r);   // the last 32-bit PC pop'n: info24, player24 (easerver_p24.cpp)
    std::string answer_lobby24(const Request &r);   // and its NET TAISEN lobby
    // beatmania IIDX 9th style (C02: runtime/pc/): its module "C02" (music, weekly, ranking, grade)
    std::string answer_c02(const Request &r);
    // One card for all (docs/EAMUSE3.md): every chart's best clear mark over all the pop'n games
    // a card has played, by song (pop'n 12's word low byte: two bits a chart), and a game's own
    // player data with them put in
    std::vector<uint8_t> shared_marks(const std::string &card) const;
    std::string with_shared_marks(const std::string &game, const std::string &card, std::string data) const;
    // and a chart's best score over pop'n 12-18 (key: the asking game's player, its card for 12,
    // its ref id for 13 on); every chart's best of the player's other games, by song and sheet,
    // for the charts `game` has too; one game's own (who: its card or ref id)
    long shared_score(const std::string &game, const std::string &key, const std::string &song, const std::string &sheet) const;
    std::map<std::pair<long, long>, long> shared_scores(const std::string &game, const std::string &key) const;
    std::map<std::pair<long, long>, long> scores_of(const std::string &game, const std::string &who) const;
    std::string rank_get(const Board &b, const Request &r);
    static std::string item(const Board &b, const std::map<std::string, std::string> &entry, size_t place);
    std::string read_file(const std::string &name) const;
    void write_file(const std::string &name, const std::string &data) const;

    std::string dir_;
    uint32_t serial_ = 0;   // for the answers' X-Eamuse-Info: deterministic
    std::string src_;       // the asking cabinet (the call's srcid)
    std::string self_;      // this server, among those sharing its data folder (a random number)
};

}  // namespace hk::py
