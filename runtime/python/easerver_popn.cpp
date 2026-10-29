// SPDX-License-Identifier: GPL-3.0-or-later
// pop'n music 10-14 on the server: their own calls, and the player data (easerver.h).
#include "python/easerver_parts.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <set>

#include "shell/sys.h"
#include <ctime>
#include "python/kbin.h"
#include "python/lobby.h"

namespace hk::py {

// pop'n music 10 on (docs/EAMUSE3.md, "Later games"): game is the model's first three
// characters. "" leaves the call to pop'n 9's answers (services, numbering, userid, posevent, ...).
std::string EamuseServer::answer_later(const std::string &game, const Request &r, const std::string &host) {
    const std::string &m = r.module, &f = r.method;
    if (game == "E02" || game == "F02" || game == "E03" || game == "F03")
        if (std::string a = answer_gfdmv(game, r); !a.empty()) return a;
    if (m == "services" && f == "get") {   // as pop'n 9, and eemall/shopinf look up their own names
        // message: pop'n 13 fails its start-up without it (the library looks a call's URL up by its
        // module, 0x17d94c0: -10 when missing); cardmng, cardreis: pop'n 13's cards; facility:
        // pop'n 14's start-up (5-2009 without it)
        static const char *modules[] = {"posevent", "pcbtracker", "numbering", "package", "userdata", "userid", "local",
                                        "eemall", "shopinf", "message", "cardmng", "cardreis", "facility"};
        std::string items;
        for (const char *mod : modules) items += std::string("<item name=\"") + mod + "\" url=\"http://" + esc(host) + "/\"/>";
        // NET TAISEN (pop'n 12-14): portfw, where the world reaches the cabinet (a call's module must
        // be listed, or the library fails it at once: -10, 0x184ed0); globby, the lobby (0x199400),
        // UDP on this server's host (lobby.h)
        if (game == "E12" || game == "E13" || game == "F14")
            items += "<item name=\"portfw\" url=\"http://" + esc(host) + "/\"/>" +
                     "<item name=\"globby\" url=\"globby://" + esc(host.substr(0, host.find(':'))) + ":" + std::to_string(Lobby::PORT) + "/\"/>";
        // GuitarFreaks and DrumMania V (Python 2: E02 DrumMania V, F02 V2, E03/F03 GuitarFreaks): the
        // library's keepalive (ea3-keepalive.c) pings the router, center and server whose addresses
        // this item gives (ga/ma, pa/ia, the host), and ntp sets its clock; HIRAKU's network for
        // Python 2 (ethnet.h) answers both at 10.50.0.1
        if (game == "E02" || game == "F02" || game == "E03" || game == "F03")
            items += "<item name=\"ntp\" url=\"ntp://10.50.0.1/\"/><item name=\"keepalive\" url=\"http://10.50.0.1/keepalive?pa=10.50.0.1&amp;ia=10.50.0.1&amp;"
                     "ga=10.50.0.1&amp;ma=10.50.0.1&amp;t1=2&amp;t2=10\"/>";
        // The PC pop'n games (G15: pop'n 15, runtime/pc/): the same keepalive, its router where the
        // cabinet's card says it is (192.168.0.1, runtime/pc/net.c), its other addresses this machine;
        // HIRAKU's DLL answers their pings
        // and NET TAISEN's lobby (the PC games' own: Lobby::PC_PORT, at the relay, which the DLL
        // reaches whatever the address: runtime/pc/vsock.c)
        if (pc_popn(game))   // each its own (pop'n 16-18: the same lobby and libraries, built into the program)
            items += "<item name=\"globby\" url=\"globby://" + esc(host.substr(0, host.find(':'))) + ":" + std::to_string(Lobby::pc_port(game)) + "/\"/>";
        if (pc_iidx(game))   // the PC IIDX games' own modules (GOLD's table at 0x548c64)
            for (const char *mod : {"pc", "music", "grade", "ranking", "shop"})
                items += std::string("<item name=\"") + mod + "\" url=\"http://" + esc(host) + "/\"/>";
        if (pc_popn(game) || pc_iidx(game))
            items += "<item name=\"pcbevent\" url=\"http://" + esc(host) + "/\"/>"
                     "<item name=\"keepalive\" url=\"http://127.0.0.1/keepalive?pa=127.0.0.1&amp;ia=127.0.0.1&amp;"
                     "ga=192.168.0.1&amp;ma=127.0.0.1&amp;t1=2&amp;t2=10\"/>";
        // the 64-bit PC pop'n (M39, 64-bit AVS2, PLAN.md §32): its start-up puts a pcbevent before
        // package.list, and stops at a network error (boot status 1,2008) when the module isn't listed
        // and its own modules (registered with the library at start-up): listed, or the library fails
        // their calls before sending (the boot check's NETWORK ERROR)
        // (the last 32-bit PC pop'n, also M39, PLAN.md §35: info24 and player24 of its own, and
        // lobby24 found under the name lobby2)
        if (game == "M39")
            for (const char *mod : {"pcbevent", "pcb", "info", "player", "info24", "player24", "lobby24", "loctest24", "sidmgr",
                                    "sppass", "eacoin", "dlstatus", "ins", "apsmanager", "local2", "local3", "lobby2"})
                items += std::string("<item name=\"") + mod + "\" url=\"http://" + esc(host) + "/\"/>";
        if (game == "M39")   // the keepalive as pop'n 15-18's (its pings answered by HIRAKU's loader)
            items += "<item name=\"keepalive\" url=\"http://127.0.0.1/keepalive?pa=127.0.0.1&amp;ia=127.0.0.1&amp;"
                     "ga=192.168.0.1&amp;ma=127.0.0.1&amp;t1=2&amp;t2=10\"/>";
        return "<services expire=\"600\" status=\"0\">" + items + "</services>";
    }
    if (m == "facility" && f == "get" && game == "M39") {   // the 64-bit PC pop'n: pcb.boot reads the location
        // as text (avs2-ea3's facility query 4: id, type, name, country, region, customercode,
        // companycode; a missing or other-typed one fails its start-up), then portfw's address and port;
        // in Japan the region as ISO 3166-2 (0x18008a500 takes the number after "JP-", or throws)
        return "<facility status=\"0\" expire=\"600\"><location><id>HIRAKU</id><type>0</type><name>HIRAKU</name>"
               "<country>JP</country><region>JP-13</region><customercode>HIRAKU</customercode>"
               "<companycode>HIRAKU</companycode></location><line><id>0</id><class __type=\"u8\">0</class></line>"
               "<portfw><globalip __type=\"ip4\">192.168.0.2</globalip><globalport __type=\"u16\">5700</globalport>"
               "<privateport __type=\"u16\">5700</privateport></portfw><public><flag __type=\"u8\">0</flag>"
               "<name>HIRAKU</name><latitude>0</latitude><longitude>0</longitude></public><share><eacoin>"
               "<notchamount __type=\"s32\">0</notchamount><notchcount __type=\"s32\">0</notchcount>"
               "<supplylimit __type=\"s32\">100000</supplylimit></eacoin><url><eapass>www.ea-pass.konami.net</eapass>"
               "<arcadefan>www.konami.jp/am</arcadefan><konaminetdx>http://am.573.jp</konaminetdx>"
               "<konamiid>http://id.konami.jp</konamiid><eagate>http://eagate.573.jp</eagate></url></share></facility>";
    }
    if ((m == "player" || m == "info") && game == "M39") return answer_hc(r);   // the 64-bit PC pop'n's (easerver_hc.cpp)
    if ((m == "player24" || m == "info24" || m == "lobby24") && game == "M39") return answer_p24(r);   // the last 32-bit PC pop'n's (easerver_p24.cpp)
    if (m == "facility" && f == "get" && (pc_popn(game) || pc_iidx(game))) {   // pop'n 15's library (libavs ea3_facility): every
        // field it imports (location, line, portfw, public, share), or it fails the start-up (5-2009);
        // pop'n 18's (J39) also the location's name and type, the line's class and share/eacoin
        // (its map in libavs-win32-ea3.dll, from 0x35fe0)
        return "<facility status=\"0\" expire=\"600\"><location><id>HIRAKU</id><country>JP</country>"
               "<region>13</region><name>HIRAKU</name><type __type=\"u8\">0</type></location>"
               "<line><id>0</id><class __type=\"u8\">0</class></line><portfw><globalip __type=\"ip4\">192.168.0.2"
               "</globalip><globalport __type=\"u16\">5700</globalport><privateport __type=\"u16\">5700</privateport>"
               "</portfw><public><flag __type=\"u8\">0</flag><name>HIRAKU</name><latitude>0</latitude>"
               "<longitude>0</longitude></public><share><url><eapass>www.ea-pass.konami.net</eapass>"
               "<arcadefan>www.konami.jp/am</arcadefan><konaminetdx>http://am.573.jp</konaminetdx>"
               "<konamiid>http://id.konami.jp</konamiid></url><eapass><valid __type=\"u16\">1</valid></eapass>"
               "<eacoin><notchamount __type=\"s32\">0</notchamount><notchcount __type=\"s32\">0</notchcount>"
               "<supplylimit __type=\"s32\">100000</supplylimit></eacoin></share>"
               "</facility>";
    }
    if (m == "facility" && f == "get") {   // pop'n 14 (0x17d6860): the shop, as text elements in three groups
        return "<facility status=\"0\" expire=\"600\"><location><id>HIRAKU</id><country>JP</country>"
               "<region>13</region><line>0</line></location><portfw><globalip>192.168.50.2</globalip>"
               "<globalport>5700</globalport><privateport>5700</privateport></portfw><public><name>HIRAKU</name>"
               "<flag>0</flag></public></facility>";
    }
    if (m == "cardmng") {   // pop'n 13: a card has an id (written on it) and its player a ref id, in "cards"
        const std::string cardtype = attr(r.attrs, "cardtype");
        std::vector<Record> cards = from_lines(read_file("cards"));
        auto add = [&](const std::string &id) {   // a new card and its player
            uint64_t n = 1;
            if (std::string c = read_file("refid"); !c.empty()) n = std::strtoull(c.c_str(), nullptr, 10);
            write_file("refid", std::to_string(n + 1) + "\n");
            char ref[17];
            snprintf(ref, sizeof ref, "48524B%010llX", (unsigned long long)n);   // "HRK" and a count
            cards.push_back({{"cardid", id}, {"refid", ref}, {"cardtype", cardtype}});
            std::string out;
            for (const Record &c : cards) out += to_line(c);
            write_file("cards", out);
            return std::string(ref);
        };
        auto refid_of = [&](const std::string &id) {
            for (const Record &c : cards)
                if (attr(c, "cardid") == id) return attr(c, "refid");
            return std::string();
        };
        if (f == "getnewid") {   // cardtype newflag: a new card's id and ref id
            const std::string id = next_number();
            return "<cardmng status=\"0\" cardid=\"" + id + "\" refid=\"" + add(id) + "\"/>";
        }
        if (f == "inquire") {   // cardtype cardid update: the card's ref id; newflag 1 when the server didn't
            // know it. pop'n 14 also wants binded (the card has its player, 0x17d51b0) and expired.
            const std::string id = attr(r.attrs, "cardid");
            if (!safe_name(id)) return "<cardmng status=\"1\"/>";
            std::string ref = refid_of(id);
            const bool fresh = ref.empty();
            // beatmania IIDX 13: a pass the server doesn't know is 112 (not registered): the game asks the
            // new PIN twice and registers it (getrefid), then its DJ name (pc reg); binded, a DistorteD
            // player for the pass (easerver_iidx.cpp)
            if (game == "FDD" || pc_iidx(game)) {
                if (fresh) return "<cardmng status=\"112\"/>";
                bool binded = false;
                for (const Record &pl : from_lines(read_file("iidx-" + game + "-players"))) binded = binded || attr(pl, "rid") == ref;
                return "<cardmng status=\"0\" refid=\"" + ref + "\" binded=\"" + (binded ? "1" : "0") + "\" expired=\"0\" newflag=\"0\"/>";
            }
            if (fresh) ref = add(id);
            // Only at the card's first reading: not when the game asks for an older game's player of the
            // card (model="I17"), nor at the credit's end (pop'n 15-18's update="1"; pop'n 14 reads a
            // card with update="1" from the start): no PIN asked then
            const bool pc_popn = game == "G15" || game == "H16" || game == "I17" || game == "J39";
            if (!fresh && on_card_pin && attr(r.attrs, "model").empty() && !(pc_popn && attr(r.attrs, "update") == "1"))
                for (const Record &c : cards)
                    if (attr(c, "cardid") == id && !attr(c, "pass").empty()) on_card_pin(attr(c, "pass"));
            // pop'n 15 asks for its own player (binded 0: the card has none yet, the game makes one
            // or carries pop'n 14's over) and, with model="F14:...", for pop'n 14's (binded 1 and
            // expired 0: the game asks for it, playerdata_get model="F14")
            if (game == "J39" || game == "I17" || game == "H16") {   // pop'n 16-18: its own player, as pop'n 15's (answer_j39)
                // and with model="<the game before>", that game's (the player carried over)
                const std::string model = attr(r.attrs, "model").substr(0, 3), of = model.empty() ? game : model;
                const size_t size = of == "J39" ? 0x1700u : of == "I17" ? 0x74bcu : of == "H16" ? 0x6784u : of == "G15" ? 0xb38u : 0;
                const bool binded = size && read_file("pdata-" + of + "-" + ref).size() == size;
                return "<cardmng status=\"0\" refid=\"" + ref + "\" binded=\"" + (binded ? "1" : "0") +
                       "\" expired=\"0\" newflag=\"" + (fresh ? "1" : "0") + "\"/>";
            }
            if (game == "G15") {
                const std::string model = attr(r.attrs, "model").substr(0, 3);
                const bool binded = model.empty() ? read_file("pdata-G15-" + ref).size() == 0xb38
                                    : model == "F14" && read_file("pdata-F14-" + ref).size() == 0xdb8;
                return "<cardmng status=\"0\" refid=\"" + ref + "\" binded=\"" + (binded ? "1" : "0") +
                       "\" expired=\"0\" newflag=\"" + (fresh ? "1" : "0") + "\"/>";
            }
            return "<cardmng status=\"0\" refid=\"" + ref + "\" binded=\"" + (fresh ? "0" : "1") +
                   "\" expired=\"0\" newflag=\"" + (fresh ? "1" : "0") + "\"/>";
        }
        if (f == "gettmpid")   // cardtype oldid: a new id for an older card's player
            return "<cardmng status=\"0\" newid=\"" + next_number() + "\"/>";
        // pop'n 14's e-AMUSEMENT PASS: the PIN is the server's (the card holds only its id)
        auto save = [&] {
            std::string out;
            for (const Record &c : cards) out += to_line(c);
            write_file("cards", out);
        };
        auto card_of_ref = [&](const std::string &ref) -> Record * {
            for (Record &c : cards)
                if (attr(c, "refid") == ref) return &c;
            return nullptr;
        };
        if (f == "getrefid") {   // cardtype cardid newflag passwd: a card made a player, with its PIN
            const std::string id = attr(r.attrs, "cardid");
            if (!safe_name(id)) return "<cardmng status=\"1\"/>";
            std::string ref = refid_of(id);
            if (ref.empty()) ref = add(id);
            if (Record *c = card_of_ref(ref); c && attr(*c, "pass").empty()) {
                (*c)["pass"] = attr(r.attrs, "passwd");
                save();
            }
            return "<cardmng status=\"0\" refid=\"" + ref + "\"/>";
        }
        if (f == "authpass") {   // refid pass: 0, or 116 for a wrong PIN (0x17ac2c8). A card's first PIN is kept.
            Record *c = card_of_ref(attr(r.attrs, "refid"));
            if (!c) return "<cardmng status=\"112\"/>";
            if (attr(*c, "pass").empty()) {
                (*c)["pass"] = attr(r.attrs, "pass");
                save();
            }
            return attr(*c, "pass") == attr(r.attrs, "pass") ? "<cardmng status=\"0\"/>" : "<cardmng status=\"116\"/>";
        }
        // rebind (cardtype newid), bindmodel (refid newflag: the player starts on this game),
        // bindcard (cardtype newid refid): nothing kept
        if (f == "rebind" || f == "bindmodel" || f == "bindcard") return "<cardmng status=\"0\"/>";
    }
    if (m == "userdata") {   // as pop'n 9, kept per game: userdata-<game>-<card>
        std::string card = attr(r.attrs, "card");
        // an older game's card being carried over is read as CARD:MODEL, that game's data
        // (seen from pop'n 10: "E004...:C00"); pop'n 9's (C00) is userdata-<card>
        if (const size_t colon = card.find(':'); f == "read" && colon != std::string::npos) {
            const std::string from = card.substr(colon + 1, 3);
            card.resize(colon);
            if (!safe_name(card) || !safe_name(from)) return "<userdata status=\"1\"/>";
            const std::string data = read_file(from == "C00" ? "userdata-" + card : "userdata-" + from + "-" + card);
            return data.empty() ? "<userdata status=\"109\"/>" : data;
        }
        if (!safe_name(card)) return "<userdata status=\"1\"/>";
        if (f == "read") {
            std::string data = read_file("userdata-" + game + "-" + card);
            if (!data.empty()) return with_shared_marks(game, card, data);   // the card's marks from the other games put in
            // a card carried over from pop'n 9: what pop'n 9 saved (the game converts it)
            if (data.empty()) data = read_file("userdata-" + card);
            return data.empty() ? "<userdata status=\"109\"/>" : data;
        }
        if (f == "write") {
            write_file("userdata-" + game + "-" + card, "<userdata status=\"0\" time=\"" + esc(attr(r.attrs, "time")) + "\">" + r.inner + "</userdata>");
            return "<userdata status=\"0\"/>";
        }
    }
    if (m == "ntp" && f == "get") {   // one attribute: "YYYY-MM-DD hh:mm:ss" and the zone (pop'n 10 0x19da48)
        const time_t t = now ? (time_t)now() : 0;
        const struct tm tm = sys::utc(t);
        char b[96];
        snprintf(b, sizeof b, "<ntp status=\"0\" time=\"%04d-%02d-%02d %02d:%02d:%02d+9\"/>", tm.tm_year + 1900, tm.tm_mon + 1,
                 tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
        return b;
    }
    if (m == "rank") {
        if (f == "exp") {   // a course played: <rank update entry rank hi_score hi_cool/> (0x19dda8)
            const Board b{nullptr, "card_id", EXP_KEEP, "score", "cool", 10};
            const std::string file = game + "-rank-exp";
            Board named = b;
            named.file = file.c_str();
            const Entered e = enter(named, r);
            return "<rank status=\"0\" update=\"" + std::string(e.update ? "1" : "0") + "\" entry=\"" + std::to_string(e.entries) +
                   "\" rank=\"" + std::to_string(e.place) + "\" hi_score=\"" + esc(attr(e.kept, "score")) + "\" hi_cool=\"" +
                   esc(attr(e.kept, "cool")) + "\"/>";
        }
        // the rankings as a <b> block the game decodes (0x19e300): pop'n 10's table (rank_table)
        if (f == "get") return game == "C20" ? "<rank status=\"0\">" + rank_b(game) + "</rank>" : "<rank status=\"0\"/>";
        // the server's ranking course (0x19e420); 117: none (the game clears it, 0x187f2c)
        if (f == "course") return "<rank status=\"117\"/>";
        if (f == "shop") {   // praystar_lo/hi: the shop's HOSHI NI NEGAIO wishes (pop'n 10)
            rank_shop(game, r, "praystar_lo", "praystar_hi");
            return "<rank status=\"0\"/>";
        }
    }
    if (m == "message" && f == "get") {   // (0x19f4e0) phase: the event phase, set online when >= 0
        // (0x18dee0); -1 leaves the cabinet's own. text: the owner's message.
        const std::string ph = read_file("phase-" + game);
        const long phase = ph.empty() ? -1 : std::strtol(ph.c_str(), nullptr, 10);
        return "<message status=\"0\" phase=\"" + std::to_string(phase) + "\" text=\"" + esc(message()) + "\"/>";
    }
    if (m == "eemall" && f == "get") {   // (0x19eda8) points, then <item num/> and <kisekae path pri/>
        const std::string card = attr(r.attrs, "card_id");
        std::string items;
        const std::string list = safe_name(card) ? read_file("eemall-" + card) : "";
        size_t at = 0, count = 0;
        while (at < list.size() && count < 99) {
            size_t e = list.find('\n', at);
            if (e == std::string::npos) e = list.size();
            const long n = std::strtol(list.c_str() + at, nullptr, 10);
            if (e > at && n > 0) { items += "<item num=\"" + std::to_string(n) + "\"/>"; count++; }
            at = e + 1;
        }
        return "<eemall status=\"0\" now_point=\"0\" add_point=\"0\">" + items + "</eemall>";
    }
    if (m == "shopinf" && f == "getsservip") return "<shopinf status=\"0\"/>";   // no shop servers (0x19f2a8)
    if (m == "eemall" && f == "check") return "<eemall status=\"0\"/>";   // pop'n 11 (0x18c098)
    if (m == "portfw" && f == "get") {   // pop'n 11: no port forward (0x18c4c8)
        // pop'n 12-14: where the world reaches the cabinet's NET TAISEN (0x190520): on HIRAKU's LAN,
        // no router between, its own address, and the port its battle socket listens on (5700)
        if (game == "E12" || game == "E13" || game == "F14") {
            const std::string ip = attr(r.attrs, "privateip");
            return "<portfw status=\"0\" globalip=\"" + esc(ip.empty() ? "192.168.50.2" : ip) + "\" port=\"5700\"/>";
        }
        return "<portfw status=\"0\"/>";
    }
    if (m == game && game == "G15") return answer_g15(r);
    if ((game == "J39" || game == "I17" || game == "H16") && (m == "game" || m == "playerdata" || m == "ranking")) return answer_j39(game, r);
    if (m == game) return answer_game(game, r);   // pop'n 11 on: <D11 type=.. method=..>
    return "";
}

// pop'n 11 on: the module named after the game, a type and a method (pop'n 11's parsers
// 0x18c858-0x18ddf8). The answer is that element again.
std::string EamuseServer::answer_game(const std::string &game, const Request &r) {
    const std::string type = attr(r.attrs, "type"), &f = r.method;
    const std::string open = "<" + game + " status=\"0\"";
    const bool ranked = game == "D11" || game == "E12" || game == "E13" || game == "F14";   // the games whose table is worked out
    if (type == "rank" && ranked && f == "get") return open + ">" + rank_b(game) + "</" + game + ">";
    // item: the shop's POPN TOURIST finds (pop'n 11, with extra) or NINPOUCHO word (pop'n 12);
    // item_lo/item_hi: POP'N LAND's or WORLD's (pop'n 13)
    if (type == "rank" && ranked && f == "shop") {
        if (game == "E13") rank_shop(game, r, "item_lo", "item_hi");
        else if (game == "F14") rank_shop(game, r, "item", "robo");   // kept; pop'n 14 shows no shop board
        else rank_shop(game, r, "item", "extra");
        return open + "/>";
    }
    // pop'n 13: a shop's phase (game active, at a credit's start and end), so its rank shop goes
    // on that area's board (phase 1 POP'N LAND, 2 POP'N WORLD)
    if (game == "E13" && type == "game" && f == "active" && r.attrs.count("phase")) {
        std::vector<Record> shops = from_lines(read_file(game + "-shop-phase"));
        bool found = false;
        for (Record &e : shops)
            if (attr(e, "shop_name") == attr(r.attrs, "shop_name")) { e["phase"] = attr(r.attrs, "phase"); found = true; }
        if (!found) shops.push_back({{"shop_name", attr(r.attrs, "shop_name")}, {"phase", attr(r.attrs, "phase")}});
        std::string out;
        for (const Record &e : shops) out += to_line(e);
        write_file(game + "-shop-phase", out);
        return open + "/>";
    }
    if (type == "rank" && f == "exp") {   // as pop'n 10's rank.exp, on the game's element
        const std::string file = game + "-rank-exp";
        Board b{file.c_str(), "card_id", EXP_KEEP, "score", "cool", 10};
        const Entered e = enter(b, r);
        return open + " update=\"" + std::string(e.update ? "1" : "0") + "\" entry=\"" + std::to_string(e.entries) + "\" rank=\"" +
               std::to_string(e.place) + "\" hi_score=\"" + esc(attr(e.kept, "score")) + "\" hi_cool=\"" + esc(attr(e.kept, "cool")) + "\"/>";
    }
    if (type == "game" && f == "get") {   // phase: as pop'n 10's message.get (-1: the cabinet's own)
        const std::string ph = read_file("phase-" + game);
        const long phase = ph.empty() ? -1 : std::strtol(ph.c_str(), nullptr, 10);
        // pop'n 13 and 14 also read net_event (14's 0x17e9308), the network event running: 1 the
        // INTERNET RANKING (14's 0x17fdc78: the attract mode's course boards, the courses' results
        // and high scores online; every use asks for exactly 1), 2 NET TAISEN, one or the other.
        // 2 unless the data folder's net_event-GAME says otherwise
        std::string flags;
        // NET TAISEN on (docs/EAMUSE3.md): pop'n 12 by play_enable (0x191160); pop'n 13 and 14 by
        // net_event 2 (13's 0x17edd48, 14's 0x17ddef8). Cabinets of
        // one shop may meet (local_matching_enable), seconds to look for opponents, and every class
        // in room 0 so anyone meets anyone (room_num, 12 and 13: ten rooms; the mask 1 << room goes
        // to the lobby). ojama_msec, ojama_gauge(_rate), cpu_level and 14's others are left out:
        // the game's own (pop'n 12: an ojama 1000 ms at least, 0x126f38)
        if (game == "E12") flags += " play_enable=\"1\"";
        if (game == "E13" || game == "F14") {
            const std::string ne = read_file("net_event-" + game);
            flags += " net_event=\"" + std::to_string(ne.empty() ? 2 : std::strtol(ne.c_str(), nullptr, 10)) + "\"";
        }
        if (game == "E12" || game == "E13" || game == "F14") flags += " local_matching_enable=\"1\" matching_sec=\"30\"";
        if (game == "E12" || game == "E13") flags += " room_num=\"0,0,0,0,0,0,0,0,0,0\"";
        return open + " phase=\"" + std::to_string(phase) + "\"" + flags + " message=\"" + esc(message()) + "\"/>";
    }
    if (type == "player") {   // mileage and 9 tickets per card (player_set sends them); course: 4 numbers
        const std::string card = attr(r.attrs, "card_id");
        if (!safe_name(card)) return "<" + game + " status=\"1\"/>";
        const std::string file = "player-" + game + "-" + card;
        if (f == "set") {
            write_file(file, to_line({{"mileage", attr(r.attrs, "mileage")}, {"ticket", attr(r.attrs, "ticket")}}));
            return open + "/>";
        }
        if (f == "get") {
            const std::vector<Record> p = from_lines(read_file(file));
            std::string ticket = p.empty() ? "" : attr(p[0], "ticket");
            while (!ticket.empty() && ticket.back() == ',') ticket.pop_back();
            if (ticket.empty()) ticket = "0,0,0,0,0,0,0,0,0";
            return open + " mileage=\"" + esc(p.empty() || attr(p[0], "mileage").empty() ? "0" : attr(p[0], "mileage")) +
                   "\" ticket=\"" + esc(ticket) + "\" course=\"0,0,0,0\"/>";
        }
    }
    if (type == "playerdata") return answer_playerdata(game, r);
    // rank get (an empty table), rank shop, pf get/ask, game select, renewc reg/ask, game active:
    // nothing kept
    return open + "/>";
}

std::string EamuseServer::answer_playerdata(const std::string &game, const Request &r) {
    // each game's record (docs/EAMUSE3.md): its size, the password's, option's and character's
    // places, and the songs' clear words (16 bits each in pop'n 12, 32 in pop'n 13)
    struct Layout { size_t record, password, option, chara, songs, song_bytes, songs_end; };
    // pop'n 14 has no password in it (cardmng.authpass checks it on the server): NONE
    constexpr size_t NONE = ~(size_t)0;
    const bool p13 = game == "E13", p14 = game == "F14";
    const Layout L = p14 ? Layout{0xdb8, NONE, 0x1c, 0x1e, 0x24, 4, 0xdb8}     // 0x16506b8, 0x17fdfa0
                   : p13 ? Layout{0xd6c, 0x1c, 0x20, 0x22, 0x28, 4, 0xd6c}     // 0x166daa8, 0x1810e60
                         : Layout{0x708, 0x34, 0x38, 0x3a, 0x3c, 2, 0x5f0};
    const size_t RECORD = L.record;
    const std::string &f = r.method;
    const std::string open = "<" + game + " status=\"0\"";
    // the player's key: pop'n 13's ref id (cardmng), else pop'n 12's card
    std::string card = attr(r.attrs, "ref_id");
    if (card.empty()) card = attr(r.attrs, "card_id");
    if (card.empty()) card = attr(r.attrs, "new_card_id");
    if (!safe_name(card)) return "<" + game + " status=\"1\"/>";
    const std::string file = "pdata-" + game + "-" + card, scores_file = "pscore-" + game + "-" + card;
    std::string rec = read_file(file);
    auto put16 = [&](size_t at, long v) { if (at + 2 <= rec.size()) { rec[at] = (char)(v & 255); rec[at + 1] = (char)(v >> 8 & 255); } };
    auto put32 = [&](size_t at, long v) { put16(at, v & 0xffff); put16(at + 2, v >> 16 & 0xffff); };
    auto set_name = [&](const std::string &name) {
        for (size_t i = 0; i < 13; i++) rec[i] = i < name.size() && i < 12 ? name[i] : 0;
    };
    auto set_password = [&](const std::string &pw) {
        if (L.password == NONE) return;
        for (size_t i = 0; i < 4; i++) rec[L.password + i] = i < pw.size() && std::isdigit((unsigned char)pw[i]) ? (char)(pw[i] - '0') : 0;
    };
    // pop'n 13 and 14: f_compress (0x17f83c0), the record compressed (lzss)
    auto with_record = [&] {
        return p13 || p14 ? open + " f_compress=\"1\"><b>" + base64(lzss(rec)) + "</b></" + game + ">"
                   : open + "><b>" + base64(rec) + "</b></" + game + ">";
    };

    if (f == "new") {   // a new card: its record (name, password; the rest zero)
        rec.assign(RECORD, 0);
        set_name(attr(r.attrs, "name"));
        set_password(attr(r.attrs, "password"));
        write_file(file, rec);
        return with_record();
    }
    if (f == "get") {   // 109: no player on this card (0x164be8)
        // an older game's card being carried over (docs/EAMUSE3.md, "One card for all"): get with
        // that game's model answers its player as that game saved it, in its <b> pieces (pop'n 12
        // appends them, 0x191910, and converts the 0xa28 bytes of a pop'n 11 player, 0x1a0a38)
        if (const std::string from = attr(r.attrs, "model"); !from.empty() && from != game) {
            if (!safe_name(from)) return "<" + game + " status=\"1\"/>";
            // pop'n 12's player is its record (pdata-E12-CARD), keyed by the card: pop'n 13 takes it
            // uncompressed (f_compress 0) and converts it (0x17e9dd8)
            // pop'n 13's player for pop'n 14 (get ref_id=<the typed POP'N ID> model="E13", 0xd6c
            // compressed as get's, which the game converts): that ref id's, or (one card for all)
            // the player of the card the core put in card_id
            if (from == "E13") {
                std::string old = safe_name(card) ? read_file("pdata-E13-" + card) : "";
                if (old.size() != 0xd6c)
                    for (const Record &c : from_lines(read_file("cards")))
                        if (attr(c, "cardid") == attr(r.attrs, "card_id") && safe_name(attr(c, "refid"))) old = read_file("pdata-E13-" + attr(c, "refid"));
                if (old.size() != 0xd6c) return "<" + game + " status=\"109\"/>";
                return open + " f_compress=\"1\"><b>" + base64(lzss(old)) + "</b></" + game + ">";
            }
            if (from == "E12") {
                const std::string id = attr(r.attrs, "card_id");
                const std::string old = safe_name(id) ? read_file("pdata-E12-" + id) : "";
                if (old.size() != 0x708) return "<" + game + " status=\"109\"/>";
                return open + " f_compress=\"0\"><b>" + base64(old) + "</b></" + game + ">";
            }
            const std::string data = read_file(from == "C00" ? "userdata-" + card : "userdata-" + from + "-" + card);
            const size_t b = data.find("<b>"), e = data.rfind("</b>");
            if (b == std::string::npos || e == std::string::npos) return "<" + game + " status=\"109\"/>";
            return open + ">" + data.substr(b, e + 4 - b) + "</" + game + ">";
        }
        if (rec.size() != RECORD) return "<" + game + " status=\"109\"/>";
        // the card's clear marks from the other pop'n games put in (pop'n 13 and 14 key the player by
        // ref id; the card is card_id, or the ref id's in the cards)
        std::string number = attr(r.attrs, "card_id");
        if (!safe_name(number) || number == "-")
            for (const Record &c : from_lines(read_file("cards")))
                if (attr(c, "refid") == card) number = attr(c, "cardid");
        if (safe_name(number)) rec = with_shared_marks(game, number, rec);
        return with_record();
    }
    if (f == "set") {
        if (rec.size() != RECORD) rec.assign(RECORD, 0);
        if (r.attrs.count("name")) set_name(attr(r.attrs, "name"));
        if (r.attrs.count("password")) set_password(attr(r.attrs, "password"));
        if (r.attrs.count("option")) put16(L.option, num(r.attrs, "option"));
        if (r.attrs.count("chara_num")) put16(L.chara, num(r.attrs, "chara_num"));
        if (!p13 && !p14 && r.attrs.count("puzzle_now")) rec[0x1b] = (char)num(r.attrs, "puzzle_now");
        if (!p13 && !p14 && r.attrs.count("puzzle_piece")) {
            const std::string pp = attr(r.attrs, "puzzle_piece");
            size_t at = 0;
            for (int k = 0; k < 6 && at <= pp.size(); k++) {
                put32(0x1c + 4 * (size_t)k, std::strtol(pp.c_str() + at, nullptr, 10));
                const size_t comma = pp.find(',', at);
                if (comma == std::string::npos) break;
                at = comma + 1;
            }
        }
        // the songs played: their clear word into the record, the score kept if better
        std::vector<Record> scores = from_lines(read_file(scores_file));
        for (size_t at = r.inner.find("<music"); at != std::string::npos; at = r.inner.find("<music", at + 1)) {
            std::map<std::string, std::string> m;
            bool sc = false;
            read_attrs(r.inner, at + 6, m, &sc);
            const long n = num(m, "num");
            if (n < 0) continue;
            const size_t word = L.songs + L.song_bytes * (size_t)n;
            if (word + L.song_bytes <= L.songs_end) {
                if (L.song_bytes == 4) put32(word, num(m, "data"));
                else put16(word, num(m, "data"));
            }
            const std::string sheet = attr(m, "sheet");
            bool found = false;
            for (Record &e : scores)
                if (attr(e, "num") == attr(m, "num") && attr(e, "sheet") == sheet) {
                    found = true;
                    if (num(m, "score") > num(e, "score")) e["score"] = attr(m, "score");
                }
            if (!found) scores.push_back({{"num", attr(m, "num")}, {"sheet", sheet}, {"score", attr(m, "score")}});
        }
        std::string t;
        for (const Record &e : scores) t += to_line(e);
        if (!scores.empty()) write_file(scores_file, t);
        // NET TAISEN: the credit's <netvs .../> goes back into the record, each attribute where the
        // game's loader reads it from (it copies the record and takes each field from its place: pop'n
        // 12 0x19ef40, 13 0x17f28d0, 14 0x17e29e8; the set builders,
        // 0x191d28, 0x17f7b04, 0x17e84b4, send them in the same order). A list ("a,b,c") is that many
        // values in a row; names are the game's own text, 13 bytes each. pop'n 13 and 14's reset flag
        // isn't in the record.
        struct Field { const char *name; size_t at; int count, width; };   // width: bytes, or 13 for a name
        static const Field netvs12[] = {
            {"map_hi", 0x5f0, 1, 4}, {"map_lo", 0x5f4, 1, 4}, {"stage_record", 0x5fe, 3, 2}, {"rank", 0x604, 1, 1},
            {"member_name", 0x60e, 3, 13}, {"member_pref", 0x683, 3, 1}, {"class", 0x68c, 1, 1},
            {"class_skill", 0x68d, 1, 1}, {"music_num", 0x68e, 1, 2}, {"sheet_num", 0x690, 1, 1},
            {"set_ojama", 0x691, 3, 1}, {"get_ojama", 0x694, 1, 4}, {"badge", 0x698, 1, 1}, {"caption", 0x699, 1, 1},
            {"fruit", 0x69a, 1, 1}};
        static const Field netvs13[] = {
            {"music_num", 0xb4c, 1, 2}, {"sheet_num", 0xb4e, 1, 1}, {"category_num", 0xb4f, 1, 1}, {"badge", 0xb50, 1, 1},
            {"caption", 0xb51, 1, 1}, {"type", 0xb52, 1, 1}, {"class", 0xb53, 1, 1}, {"class_skill", 0xb54, 1, 1},
            {"class_start", 0xb55, 1, 1}, {"record", 0xb56, 1, 1}, {"member_name", 0xb6a, 3, 13},
            {"member_pref", 0xbdf, 3, 1}, {"set_ojama", 0xc01, 3, 1}, {"get_ojama_hi", 0xc04, 1, 4},
            {"get_ojama_lo", 0xc08, 1, 4}, {"trump_list", 0xc4e, 52, 2}, {"ojama_condition", 0xcb6, 48, 1},
            {"ojama_cnt", 0xce6, 10, 1}};
        static const Field netvs14[] = {
            {"music_num", 0xbdc, 1, 2}, {"sheet_num", 0xbde, 1, 1}, {"category_num", 0xbdf, 1, 1}, {"badge", 0xbe0, 1, 1},
            {"caption", 0xbe1, 1, 1}, {"job_num", 0xbe2, 1, 1}, {"class", 0xbe3, 1, 1}, {"class_skill", 0xbe4, 1, 1},
            {"class_start", 0xbe5, 1, 1}, {"class_upgrade", 0xbe6, 1, 1}, {"record", 0xbe7, 1, 1},
            {"member_name", 0xbfb, 3, 13}, {"member_pref", 0xc70, 3, 1}, {"ojama_condition", 0xc79, 51, 1},
            {"job_var", 0xd66, 1, 2}, {"job_exp", 0xd68, 9, 4}, {"set_ojama", 0xda5, 3, 1},
            {"get_ojama_hi", 0xda8, 1, 4}, {"get_ojama_lo", 0xdac, 1, 4}, {"item_hi", 0xdb0, 1, 4}, {"item_lo", 0xdb4, 1, 4}};
        if (const size_t nv = r.inner.find("<netvs"); nv != std::string::npos) {
            std::map<std::string, std::string> m;
            bool sc = false;
            read_attrs(r.inner, nv + 6, m, &sc);
            const Field *fields = p14 ? netvs14 : p13 ? netvs13 : netvs12;
            const size_t count = p14 ? std::size(netvs14) : p13 ? std::size(netvs13) : std::size(netvs12);
            for (size_t k = 0; k < count; k++) {
                const Field &fd = fields[k];
                if (!m.count(fd.name)) continue;
                const std::string list = attr(m, fd.name);
                size_t at = 0;
                for (int i = 0; i < fd.count; i++) {
                    const size_t comma = list.find(',', at);
                    const std::string v = list.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
                    const size_t place = fd.at + (size_t)i * (size_t)fd.width;
                    if (place + (size_t)fd.width > rec.size()) break;
                    if (fd.width == 13)
                        for (size_t b = 0; b < 13; b++) rec[place + b] = b < v.size() && b < 12 ? v[b] : 0;
                    else {
                        const long n = std::strtol(v.c_str(), nullptr, 10);
                        for (int b = 0; b < fd.width; b++) rec[place + (size_t)b] = (char)(n >> (8 * b) & 255);
                    }
                    if (comma == std::string::npos) break;
                    at = comma + 1;
                }
            }
        }
        write_file(file, rec);
        return open + "/>";
    }
    if (f == "score")   // one chart's best: <E12 score/> (0x192a00), the card's in pop'n 12-14
        return open + " score=\"" + std::to_string(shared_score(game, card, attr(r.attrs, "music_num"), attr(r.attrs, "sheet_num"))) + "\"/>";
    // conv (old_card_id new_card_id name ...): a player carried over from the game before
    // (docs/EAMUSE3.md, "One card for all"). The game converts the older record it was given (get
    // with that game's model) for itself and keeps it only in memory; its credit's set sends only
    // the songs played. So the record is built here as the game builds it: pop'n 12 from pop'n 11's
    // 0xa28 bytes, whose 600 song words at 0x410 number the songs as pop'n 12 does, each word's bits
    // 4-5 and 6-7 swapped and its high byte dropped (0x1a0a38, checked: the game's own result in
    // RAM is the same); the name as sent.
    if (f == "conv" && game == "E12") {
        const std::string old = attr(r.attrs, "old_card_id");
        if (!safe_name(old)) return "<" + game + " status=\"1\"/>";
        const std::string from = unbase64_b(read_file("userdata-D11-" + old));
        if (rec.size() != RECORD) rec.assign(RECORD, 0);
        set_name(attr(r.attrs, "name"));
        // the PIN, a digit a byte at 0x9f3 (the name follows at 0x9f7): the credit's set sends it
        // too, but a later game asks for it even if this credit never ends properly
        if (from.size() >= 0x9f7)
            for (size_t i = 0; i < 4; i++) rec[L.password + i] = (char)((uint8_t)from[0x9f3 + i] % 10);
        for (size_t n = 0; n < 600 && 0x410 + 2 * n + 2 <= from.size(); n++) {
            const unsigned w = (uint8_t)from[0x410 + 2 * n] | (uint8_t)from[0x410 + 2 * n + 1] << 8;
            put16(L.songs + 2 * n, (w & 0xf) | (w & 0x30) << 2 | (w & 0xc0) >> 2);   // the high byte dropped, as there
        }
        write_file(file, rec);
        return open + "/>";
    }
    // pop'n 13 from pop'n 12's record (pdata-E12-<old card>): pop'n 13 asks only conv with the
    // player's new ref id and keeps nothing itself, so the record is the server's to make. The songs
    // are numbered alike (its display of the older record, 0x1812f98, looks songs up by the same
    // index), and both keep a chart's clear mark in two bits, the four charts at bits 0-1, 2-3, 4-5,
    // 6-7 (pop'n 13's own display, 0x1812c4c): the low byte carries over, 32 bits a song from 0x28.
    // pop'n 12's flags above it don't (their meaning in 13 isn't known). The PIN and the name too.
    if (f == "conv" && game == "E13") {
        const std::string old = attr(r.attrs, "old_card_id");
        if (!safe_name(old)) return "<" + game + " status=\"1\"/>";
        const std::string from = read_file("pdata-E12-" + old);
        if (rec.size() != RECORD) rec.assign(RECORD, 0);
        set_name(attr(r.attrs, "name"));
        if (from.size() == 0x708) {
            for (size_t i = 0; i < 4; i++) rec[L.password + i] = (char)((uint8_t)from[0x34 + i] % 10);
            for (size_t n = 0; 0x3c + 2 * n + 2 <= 0x5f0 && L.songs + 4 * n + 4 <= L.songs_end; n++)
                put32(L.songs + 4 * n, (uint8_t)from[0x3c + 2 * n]);
        }
        write_file(file, rec);
        return open + "/>";
    }
    // pop'n 14: conv (name ref_id card_id popn_id ea_passwd) after the player chose "カーニバルから
    // データを引きつぐ" and typed pop'n 13's transfer number (POP'N ID). The game takes the answer's
    // record as its own, byte for byte (checked in RAM). pop'n 13's record as it is would put song n
    // at pop'n 14's n+1 (14's song words start at 0x24, 13's at 0x28: 0x17fdfa0, 0x17a0170; both
    // number songs alike, by their song tables and the songs played) and 13's PIN where 14 keeps its
    // option and character; so the answer is the record converted to pop'n 14's layout: the name,
    // the option and character (13's 0x20, 0x22 to 0x1c, 0x1e), and each song's word. The server
    // keeps it as pop'n 14's record (the credit's set sends only the songs played). Transfer numbers
    // aren't issued here; with one card for all, the card's ref id is pop'n 13's too.
    if (f == "conv" && p14) {
        const std::string old = read_file("pdata-E13-" + card);
        if (old.size() != 0xd6c) return "<" + game + " status=\"109\"/>";
        rec.assign(RECORD, 0);
        set_name(attr(r.attrs, "name"));
        put16(L.option, (uint8_t)old[0x20] | (uint8_t)old[0x21] << 8);
        put16(L.chara, (uint8_t)old[0x22] | (uint8_t)old[0x23] << 8);
        for (size_t n = 0; 0x28 + 4 * n + 4 <= old.size() && L.songs + 4 * n + 4 <= L.songs_end; n++)
            for (size_t b = 0; b < 4; b++) rec[L.songs + 4 * n + b] = old[0x28 + 4 * n + b];
        write_file(file, rec);
        return open + " f_compress=\"1\"><b>" + base64(lzss(rec)) + "</b></" + game + ">";
    }
    // tmp, renew: not yet
    return open + "/>";
}

}  // namespace hk::py
