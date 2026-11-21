// SPDX-License-Identifier: GPL-3.0-or-later
// HIRAKU's e-AMUSE3 server (docs/EAMUSE3.md): what Konami's servers answered, from each game's own
// requests and response parsers. One server for every game: inside a core (the game's own, offline)
// and as hiraku-eamuse on a network (tools/eamuse/server.cpp), the same class.
//
// The protocol (eamuse/protocol.h; bemaniutils documents the later games'): an HTTP/1.0 POST whose
// body is XML, RC4-encrypted with key = MD5(the 6 bytes of "X-Eamuse-Info: 1-xxxxxxxx-xxxx" + a shared
// secret). A request is <call srcid=.. model=..><MODULE method=.. .../></call>; the answer
// <response><MODULE status="0" ...>...</MODULE></response>. A call goes to its game's answers by its
// model's first three letters, its row in the table of games (games.h), whose answers it is tried on
// in order (server.cpp's answer); the games' answers, a file a family:
//   server_popn.cpp    pop'n music 9 to 14 (Konami Python 1), and what the later games share with them
//                      (services, cards and PINs, the shop); pop'n 9's own modules are server.cpp's
//   server_pc.cpp      pop'n music 15 to 18 (PC games)
//   server_hc.cpp, server_p24.cpp   the two later PC pop'n games
//   server_iidx.cpp    beatmania IIDX 9th style to EMPRESS (PC games)
//   server_gfdmv.cpp   GuitarFreaks and DrumMania V (Konami Python 2)
//   server_gfdm.cpp    the System 573 GuitarFreaks and DrumMania
//   server_rank.cpp, server_share.cpp   the rankings; one card for all
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "eamuse/games.h"
#include "eamuse/protocol.h"
#include "eamuse/store.h"

namespace hk::ea {

// The arcade a cabinet belongs to (hiraku-server's machines, server/accounts.h): its id (whose own
// settings come first: games.h's setting()), the shop's name and prefecture the answers give
struct Cabinet {
    std::string arcade;   // "": none (the server's settings)
    std::string shop;     // "": the server's own name (HIRAKU)
    int pref = -1;        // -1: the server's own (13)
};

class Server {
public:
    // dir: where the server keeps what it must remember (cards, player data), or "" for nothing
    explicit Server(std::string dir) : store_(std::make_unique<FolderStore>(std::move(dir))) {}
    // ... or what it is given keeps it (store.h)
    explicit Server(std::unique_ptr<Store> store) : store_(std::move(store)) {}

    // One HTTP request (head and body, as sent) to the whole HTTP response.
    std::string handle(const std::string &request);

    // Whether the cabinet a call says it is from (its srcid, the PCBID) is answered: a server on a
    // network takes the ones it was told (eamuse/access.h). Unset: every one, as a core's own server
    std::function<bool(const std::string &srcid)> admit;
    // The arcade of the cabinet a call is from (its srcid), asked once a call after admit. Unset, or
    // an empty answer: none, today's answers byte for byte
    std::function<Cabinet(const std::string &srcid)> cabinet;
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
    struct Board;   // a ranking (server_rank.cpp)

private:
    friend const std::vector<Game> &games();   // (the table names the answers below)
    static bool parse(const std::string &xml, Call &c);
    // A call's request to its answer: its game's answers (games.h) tried in order
    std::string answer(const Call &c, const Request &r, const std::string &host);

    // The games' answers, each a link of a game's chain (games.h's Answer): "" leaves the request to
    // the next.
    // The modules pop'n music 9 and the System 573 games answer alike (a card's number, KONAMI ID,
    // posevent's sales)
    std::string answer_shared(const Game &, const Call &, const Request &r, const std::string &);
    // pop'n music 9's own modules (userdata, rank, eemall, services, ntp, package), which every game
    // but the System 573 ones falls back to
    std::string answer_popn9(const Game &, const Call &, const Request &r, const std::string &host);
    // The System 573 GuitarFreaks and DrumMania's own (server_gfdm.cpp)
    std::string answer_gfdm(const Game &, const Call &call, const Request &r, const std::string &host);
    // services.get's answer: each name with this server's URL
    static std::string services(const std::string &host, const std::vector<const char *> &names);
    std::string message() const;   // the owner's title-screen message (package "message"), or ""
    // A game's setting for the asking cabinet: its arcade's value, else the server's, else the default
    // (games.h's setting())
    std::string setting_of(const Game &g, const char *name, const std::string &model = "") const {
        return setting(*store_, g, name, model, cab_.arcade);
    }
    // The shop's name and prefecture the answers give: the asking cabinet's arcade's, else HIRAKU's
    std::string shop_name() const { return cab_.shop.empty() ? "HIRAKU" : cab_.shop; }
    int shop_pref() const { return cab_.pref < 0 ? 13 : cab_.pref; }

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
    // pop'n 10 on: their own calls and answers, and what the later games share with them
    std::string answer_later(const Game &g, const Call &call, const Request &r, const std::string &host);
    // GuitarFreaks and DrumMania V (Konami Python 2: E02, F02, E03, F03)
    std::string answer_gfdmv(const Game &, const Call &, const Request &r, const std::string &);
    std::string answer_game(const std::string &game, const Request &r);
    std::string answer_playerdata(const std::string &game, const Request &r);
    // pop'n 15 (G15, a PC game: runtime/pc/): <G15 method="type_method" ...>
    std::string answer_g15(const Game &, const Call &, const Request &r, const std::string &);
    // pop'n 16, 17 and 18 (H16, I17, J39, PC games: runtime/pc/): modules game, playerdata and
    // ranking, binary XML
    std::string answer_j39(const Game &, const Call &call, const Request &r, const std::string &);
    // the 64-bit PC pop'n (M39: runtime/pc64/): its modules "player" and "info"
    std::string answer_hc(const Game &, const Call &, const Request &r, const std::string &);
    // the last 32-bit PC pop'n (also M39): info24, player24, lobby24 (server_p24.cpp)
    std::string answer_p24(const Game &, const Call &, const Request &r, const std::string &);
    std::string answer_lobby24(const Request &r);   // its NET TAISEN lobby
    // beatmania IIDX 9th style (C02: runtime/pc/): its module "C02" (music, weekly, ranking, grade)
    std::string answer_c02(const Game &, const Call &, const Request &r, const std::string &);
    // Konami's ee'MALL 1st Avenue station (C33: runtime/pc/ezusb_c33.c): its start-up's calls (server_eemall.cpp)
    std::string answer_eemall(const Game &, const Call &, const Request &r, const std::string &host);
    // ... a visit's calls: the card's player, its KONAMI ID, tickets, points and items ("": not one)
    std::string eemall_visit(const Game &, const Call &, const Request &r);
    // ... and the shop server it then downloads from by plain GETs: the file at path, or false
    static bool eemall_shop_file(const std::string &path, std::string &body);
    // beatmania IIDX 10th style, RED, HAPPY SKY and DistorteD (D01, E11, ECO, FDD): their module named
    // after the model (pc, music, ranking, weekly, grade, shop), 9th style's way with the player's
    // record added
    std::string answer_iidx_red(const Game &, const Call &call, const Request &r, const std::string &);
    // ... and DistorteD (FDD) and GOLD on: the same calls as modules of their own with attributes (pc,
    // music, ranking, shop, grade), the player found by the card's refid (cardmng)
    std::string answer_iidx_fdd(const Game &, const Call &call, const Request &r, const std::string &);
    // beatmania IIDX 9th style to DistorteD's player: the card's record as the game wrote it, a file a game
    std::string answer_iidx_userdata(const Game &, const Call &call, const Request &r, const std::string &);
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
    // A game asks for a new player (new, or one carried over from the game before) only after it
    // was told the card has none. Asked for a card that has its player, the player is kept and
    // answered as it is, and this says so: nothing a call says makes a new player of an old one.
    // (A player's record that isn't one, cut short or damaged, reads as "no player"; the new one made
    // in its place first keeps it aside: Store::aside.)
    void say_kept(const Table &t, const Key &k) const;

    // What the server keeps (tables.h), and where (store.h)
    std::unique_ptr<Store> store_;
    uint32_t serial_ = 0;   // for the answers' X-Eamuse-Info: deterministic
    std::string src_;       // the asking cabinet (the call's srcid)
    std::string eemall_card_;   // the ee'MALL station's card of the call's personaldata.write, for the i_set after it
    Cabinet cab_;           // ... and its arcade (cabinet)
    std::string self_;      // this server, among those sharing its data folder (a random number)
    // What a server others can reach keeps is bounded (a call registers a card, writes a card's file):
    // the store's things (FolderStore: limit-files) and cards (the store's limit "cards"; both
    // defaults in games.cpp); a card's userdata, 128 KiB
    static constexpr size_t USERDATA_MAX = 0x20000;
    mutable bool cards_full_said_ = false;
};

}  // namespace hk::ea
