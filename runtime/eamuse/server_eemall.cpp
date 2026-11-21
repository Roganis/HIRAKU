// SPDX-License-Identifier: GPL-3.0-or-later
// Konami's ee'MALL station on the server (C33: 1st Avenue, runtime/pc/ezusb_c33.c) (server.h).
#include "eamuse/server_parts.h"
#include "shell/sys.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace hk::ea {

// ee'MALL 1st Avenue (C33:J:A:E): pop'n music 9's e-AMUSE3 library (SSReAmuse.dll) with the
// station's own calls (SSRNet.dll). Its start-up so far:
//   - services.get: the station's library looks up pcbtracker, posevent, numbering, package and
//     userdata, and SSRNet its own modules' "local", "shopinf" and "userid";
//   - pcbtracker.alive (expire), package.list (pkgtype "auto": nothing to download);
//   - shopinf.getsservip <model srcip/>: the shop server's address (<sservip ipaddr/>, dotted), where
//     the station downloads the shop's catalogue by plain HTTP. This server is the shop server too:
//     its address as the station reached it (a dotted host), else 127.0.0.1 (hiraku-pc's DLL sends
//     the station's port-80 connections to the server wherever they go: runtime/pc/net.c). The
//     station pings it, then GETs /sum/file.xml (a <datasync> of <list url path size sum/>, sum the
//     MD5 of the file in lower-case hex), /file.xml, and in it looks for /C33/file.xml, its own
//     catalogue's list: there is none here, which the station takes as "no shop data for this
//     station" and starts (RECORDS, GOODS, POLICE BOX and the DJ game stay closed; SSRNet.dll 0x10007556).
// A visit (eemall_visit below): the card's player, the KONAMI ID, tickets, points, the inventory.
std::string Server::answer_eemall(const Game &g, const Call &c, const Request &r, const std::string &host) {
    const std::string &m = r.module, &f = r.method;
    if (std::string a = eemall_visit(g, c, r); !a.empty()) return a;
    if (m == "services" && f == "get")
        return services(host, {"pcbtracker", "posevent", "numbering", "package", "userdata", "local", "shopinf", "userid"});
    if (m == "pcbtracker" && f == "alive") return "<pcbtracker status=\"0\" expire=\"600\"/>";
    if (m == "package" && f == "list") return "<package status=\"0\"/>";
    if (m == "shopinf" && f == "getsservip") {
        std::string ip = host.substr(0, host.find(':'));
        bool dotted = !ip.empty() && ip.size() <= 15;
        for (char c : ip) dotted = dotted && ((c >= '0' && c <= '9') || c == '.');
        return "<shopinf status=\"0\"><sservip ipaddr=\"" + (dotted ? ip : std::string("127.0.0.1")) + "\"/></shopinf>";
    }
    return "";
}

// The shop server's lists (plain XML, the station's GET has no Host header and LF line ends): one
// list naming the other, and that one empty (no /C33/file.xml: no catalogue for the station)
bool Server::eemall_shop_file(const std::string &path, std::string &body) {
    static const char empty[] = "<datasync></datasync>";   // 21 bytes, MD5 cadefd108db93db56ef7dd2cdb5b16fa
    if (path == "/file.xml") return body = empty, true;
    if (path == "/sum/file.xml")
        return body = "<datasync><list url=\"/file.xml\" path=\"file.xml\" size=\"21\" sum=\"cadefd108db93db56ef7dd2cdb5b16fa\"/></datasync>", true;
    return false;
}

// ---- a visit (SSRNet.dll is the station's network library)
//
// The card: the station reads the magnetic card itself (pop'n music 9's layout, runtime/pc/ezusb_c33.c)
// and refuses one never played on a game (its header's first byte without 0x40 or 0x80: SSRIo.dll
// 0x10003920), so every card that reaches personaldata.read was registered by a game. It sends the
// card's first five bytes (card_header) and the next 58 (card_data) as hex; the card's number is
// bytes 6-13 backwards, as pop'n 9 numbers it: the station's player is that card's player in every
// HIRAKU game (one card for all: pop'n 9's userdata-NUMBER, the KONAMI IDs' users).
// The station's own record is kept by game and card (tables::player, "player-C33-NUMBER", as pop'n
// 10-14's mileage): points, tickets, plays, its character, its items, the day of its last visit.
namespace {

// The card's number from card_data (bytes 5.. of the card): bytes 6-13 backwards, two hex digits a
// byte, as pop'n 9 and the station's later calls write it (card_id, 16 hex digits). "": not a number
std::string eemall_card_number(const std::string &card_data) {
    if (card_data.size() < 18) return "";
    std::string n;
    for (int i = 8; i >= 1; i--) {
        const std::string b = card_data.substr(2 * i, 2);
        for (char ch : b)
            if (!std::isxdigit((unsigned char)ch)) return "";
        n += (char)std::toupper((unsigned char)b[0]);
        n += (char)std::toupper((unsigned char)b[1]);
    }
    return n.find_first_not_of('0') == std::string::npos ? "" : n;
}

// The game that registered the card, from its header's bits 10-27 (SSRIo.dll 0x100037c0: three
// letters of six bits, + 0x20): "C00" for pop'n music 9's, whose model the station knows (C00:J:A:A)
std::string eemall_card_model(const std::string &card_header) {
    if (card_header.size() < 8) return "";
    uint32_t h = (uint32_t)std::strtoul(card_header.substr(0, 8).c_str(), nullptr, 16);
    std::string code;
    for (int bit = 10; bit < 28; bit += 6) code += (char)(((h >> (32 - bit - 6)) & 0x3f) + 0x20);
    return code == "C00" ? "C00:J:A:A" : code;
}

std::string hex_md5(const std::string &s) {
    static const char *hx = "0123456789abcdef";
    std::string o;
    for (uint8_t b : md5(std::vector<uint8_t>(s.begin(), s.end()))) o += hx[b >> 4], o += hx[b & 15];
    return o;
}

// Days since 1970 of a "YYYY-MM-DD" (-1: not one)
long eemall_day(const std::string &date) {
    int y, mo, d;
    if (std::sscanf(date.c_str(), "%d-%d-%d", &y, &mo, &d) != 3) return -1;
    std::tm t{};
    t.tm_year = y - 1900, t.tm_mon = mo - 1, t.tm_mday = d, t.tm_hour = 12;
    return (long)(sys::from_utc(t) / 86400);
}

// The items kept, "model/number,model/number...", as pairs
std::vector<std::pair<std::string, std::string>> eemall_items(const std::string &list) {
    std::vector<std::pair<std::string, std::string>> v;
    for (size_t at = 0; at < list.size();) {
        size_t e = list.find(',', at);
        if (e == std::string::npos) e = list.size();
        const std::string one = list.substr(at, e - at);
        if (const size_t sl = one.find('/'); sl != std::string::npos) v.emplace_back(one.substr(0, sl), one.substr(sl + 1));
        at = e + 1;
    }
    return v;
}

}  // namespace

// Tickets and points last 60 days after a visit (the program's own rule, shown on its screens).
// The station doesn't check it: it only gives back the date and time this server sends
// (personaldata.ticket/write's date, time and ofs, SSRNet.dll 0x10002790), so the server does
constexpr long EEMALL_KEEP_DAYS = 60;

std::string Server::eemall_visit(const Game &, const Call &, const Request &r) {
    const std::string &m = r.module, &f = r.method;
    // the server's clock (as ntp.get gives it the cabinets: hiraku-pc's is Japan's): the day, and the
    // time as the station's library writes them (pop'n 9's userdata time, "+9" its zone)
    const time_t now_t = now ? (time_t)now() : time(nullptr);
    const std::tm jst = sys::utc(now_t);
    char today[16], stamp[40];
    std::snprintf(today, sizeof today, "%04d-%02d-%02d", jst.tm_year + 1900, jst.tm_mon + 1, jst.tm_mday);
    std::snprintf(stamp, sizeof stamp, "%s %02d:%02d:%02d+9", today, jst.tm_hour, jst.tm_min, jst.tm_sec);
    auto record = [&](const std::string &card) {
        std::vector<Record> v = from_lines(store_->get(tables::player, {"C33", card}));
        return v.empty() ? Record{} : v[0];
    };
    auto keep_record = [&](const std::string &card, const Record &p) { store_->put(tables::player, {"C33", card}, to_line(p)); };
    auto owner = [&](const std::string &card) -> Record {   // the card's KONAMI ID (server.cpp's userid)
        for (const Record &u : from_lines(store_->get(tables::users, {})))
            if (("," + attr(u, "cards") + ",").find("," + card + ",") != std::string::npos) return u;
        return {};
    };
    if (m == "personaldata" && f == "read") {
        // (parser SSRNet.dll 0x10002360; read by 0x1000d2e0): status 0 with a konami_id = the player,
        // whose password the station checks itself (pass: the MD5 of the 4 characters typed, lower-case
        // hex, 0x1000bee0); konami_id "" = a card with no KONAMI ID (the station offers to make one or
        // to add the card to one: userid.adduser / addcard); any other status = an error screen
        const std::string card = eemall_card_number(attr(r.attrs, "card_data"));
        if (card.empty() || !safe_name(card)) return "<personaldata status=\"300\"/>";
        Record p = record(card);
        // a visit more than 60 days after the last: its tickets and points are gone
        const long last = eemall_day(attr(p, "date")), day = eemall_day(today);
        if (last >= 0 && day - last > EEMALL_KEEP_DAYS && (attr(p, "point") != "0" || attr(p, "ticket") != "0")) {
            if (log) log("ee'MALL: card " + card + ": last visit " + attr(p, "date") + ", over " + std::to_string(EEMALL_KEEP_DAYS) + " days: its points and tickets are gone");
            p["point"] = "0", p["ticket"] = "0";
        }
        if (attr(p, "date").empty()) p["date"] = today;
        p["seq"] = std::to_string(std::strtol(attr(p, "seq").c_str(), nullptr, 10) + 1);
        p["model"] = eemall_card_model(attr(r.attrs, "card_header"));
        keep_record(card, p);
        const Record u = owner(card);
        auto n = [&](const char *k) { const std::string v = attr(p, k); return v.empty() ? std::string("0") : v; };
        std::string items;
        size_t count = 0;
        for (const auto &it : eemall_items(attr(p, "items")))
            if (count < 99) items += "<item num=\"" + esc(it.second) + "\" model=\"" + esc(it.first) + "\"/>", count++;
        return "<personaldata status=\"0\" konami_id=\"" + esc(attr(u, "user")) + "\" pass=\"" + (u.empty() ? "" : hex_md5(attr(u, "pass"))) +
               "\" date=\"" + esc(attr(p, "date")) + "\" model=\"" + esc(attr(p, "model")) + "\" play_num=\"" + n("plays") + "\" point=\"" + n("point") +
               "\" chara_num=\"" + n("chara_num") + "\" chara_model=\"" + esc(attr(p, "chara_model")) + "\" post_num=\"0\" item_num=\"" +
               std::to_string(count) + "\" ticket_num=\"" + n("ticket") + "\" card_id=\"" + card + "\" seq=\"" + esc(attr(p, "seq")) +
               "\" id_s=\"1\" record_shop=\"1\" userdata_flag=\"0\" id_limit=\"0\" time=\"" + stamp + "\">" + items + "</personaldata>";
    }
    if (m == "personaldata" && f == "clist") {   // the KONAMI ID's cards (0x10001ef0): <item card_id model title/>, card_num
        std::string items;
        int count = 0;
        for (const Record &u : from_lines(store_->get(tables::users, {})))
            if (attr(u, "user") == attr(r.attrs, "user_id") && !attr(u, "user").empty()) {
                const std::string cards = attr(u, "cards");
                for (size_t at = 0; at < cards.size();) {
                    size_t e = cards.find(',', at);
                    if (e == std::string::npos) e = cards.size();
                    const std::string cd = cards.substr(at, e - at);
                    // a card the station has read gives its game's model: the station then asks
                    // personaldata.playnum (points from that card's plays elsewhere) for it
                    if (!cd.empty() && safe_name(cd)) items += "<item card_id=\"" + esc(cd) + "\" model=\"" + esc(attr(record(cd), "model")) + "\"/>", count++;
                    at = e + 1;
                }
            }
        return "<personaldata status=\"0\" card_num=\"" + std::to_string(count) + "\">" + items + "</personaldata>";
    }
    if (m == "personaldata" && f == "playnum")   // points earned on other games (0x10002170): none kept yet
        return "<personaldata status=\"0\" point=\"0\"/>";
    if (m == "personaldata" && (f == "ticket" || f == "write" || f == "i_set")) {
        // ticket (coins became tickets, 0x10002790), write (the visit's end, 0x10002930) and its second
        // element i_set (the player's whole inventory: model<i>, item_no<i>). All answer status only.
        const std::string card = attr(r.attrs, "card_id");
        if (f == "i_set") {
            // i_set names no card: it follows the write in the same call, of the card that write named
            std::string items, popn;
            const long count = num(r.attrs, "item_num");
            for (long i = 0; i < count && i < 999; i++) {
                const std::string model = attr(r.attrs, ("model" + std::to_string(i)).c_str()), no = attr(r.attrs, ("item_no" + std::to_string(i)).c_str());
                if (no.empty() || model.find_first_of(",/\t\n") != std::string::npos || no.find_first_not_of("0123456789") != std::string::npos) continue;
                items += (items.empty() ? "" : ",") + model + "/" + no;
                if (model.compare(0, 3, "C00") == 0) popn += no + "\n";
            }
            if (!eemall_card_.empty()) {
                Record p = record(eemall_card_);
                p["items"] = items;
                keep_record(eemall_card_, p);
                // pop'n music 9's items also go on the list the pop'n games read (tables::eemall, one
                // number a line); numbers already there (the owner's) stay
                if (!popn.empty()) {
                    std::string list = store_->get(tables::eemall, {eemall_card_});
                    for (size_t at = 0; at < popn.size();) {
                        const size_t e = popn.find('\n', at);
                        const std::string no = popn.substr(at, e - at);
                        if (("\n" + list).find("\n" + no + "\n") == std::string::npos) list += (list.empty() || list.back() == '\n' ? "" : "\n") + no + "\n";
                        at = e + 1;
                    }
                    store_->put(tables::eemall, {eemall_card_}, list);
                }
                eemall_card_.clear();
            }
            return "<personaldata status=\"0\"/>";
        }
        if (card.empty() || !safe_name(card)) return "<personaldata status=\"1\"/>";
        eemall_card_ = card;
        Record p = record(card);
        p["ticket"] = std::to_string(num(r.attrs, "ticket_num"));
        if (f == "write") {
            p["point"] = std::to_string(num(r.attrs, "point"));
            p["plays"] = std::to_string(num(r.attrs, "play_num"));
            p["chara_num"] = std::to_string(num(r.attrs, "chara_num"));
            p["chara_model"] = attr(r.attrs, "chara_model");
            if (p["chara_model"].find_first_of("\t\n") != std::string::npos) p["chara_model"].clear();
        }
        p["date"] = today;   // the visit's day: tickets and points last 60 days from it
        keep_record(card, p);
        return "<personaldata status=\"0\"/>";
    }
    // the places' calls, with nothing in them: what the program reads when there is nothing
    if (m == "userinfo" && f == "get") return "<userinfo status=\"0\" length=\"0\" data=\"\"/>";   // the INFORMATION text
    if (m == "postdata2" && f == "get") return "<postdata2 status=\"0\" post_num=\"0\"/>";             // POST OFFICE: no mail
    if (m == "recieved" && f == "get") return "<recieved status=\"0\"/>";                              // nothing received
    if (m == "address" && f == "get") return "<address status=\"0\"/>";                                // no address book
    if (m == "lostdata" && f == "read") return "<lostdata status=\"0\" lost_num=\"0\"/>";              // POLICE BOX: nothing lost
    if (m == "restrant" && f == "get") return "<restrant status=\"0\" sub_code=\"0\"/>";               // RESTAURANT: no event
    return "";
}

}  // namespace hk::ea
