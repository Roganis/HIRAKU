// SPDX-License-Identifier: GPL-3.0-or-later
// A local e-AMUSE3 server for Python 1 pop'n (easerver.h): the calls taken apart and answered, the
// files kept. Its games' answers are in easerver_*.cpp: rankings (rank), GuitarFreaks and
// DrumMania V (gfdm), pop'n 9-14 (popn), one card for all (share), pop'n 15-18 (pc), beatmania
// IIDX 9th style (iidx).
#include "python/easerver_parts.h"
#include "python/kbin.h"
#include "python/lobby.h"
#include "shell/sys.h"

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <set>
#include <ctime>
#include <fstream>

#ifdef _MSC_VER
#define strncasecmp _strnicmp   // MSVC's name for it
#endif

namespace hk::py {

// pop'n 13's compression for player records (f_compress, the game's 0x1810140): LZSS with a 4 KiB
// window that starts at 0xfee, a flag byte per 8 items (1 = a literal byte, 0 = a match: 12-bit
// window position and 4-bit length - 3, so 3 to 18 bytes), after the length in 4 bytes. The game
// answers in kind only with the data small enough for its HTTP reader: 0xd6c bytes in clear are
// refused, compressed they are a few hundred.
std::string lzss(const std::string &in) {
    const size_t n = in.size();
    std::string out;
    for (int k = 0; k < 4; k++) out += (char)(n >> (8 * k) & 255);
    size_t i = 0;
    while (i < n) {
        const size_t flags_at = out.size();
        out += '\0';
        uint8_t flags = 0;
        for (int bit = 0; bit < 8 && i < n; bit++) {
            size_t best = 0, from = 0;
            for (size_t q = i > 4078 ? i - 4078 : 0; q < i; q++) {   // earlier output, overlap allowed
                size_t len = 0;
                while (len < 18 && i + len < n && in[q + len] == in[i + len]) len++;
                if (len > best) { best = len; from = q; }
            }
            if (best >= 3) {
                const unsigned at = (unsigned)(0xfee + from) & 0xfff;
                out += (char)(at & 255);
                out += (char)(((at >> 4) & 0xf0) | (best - 3));
                i += best;
            } else {
                flags |= (uint8_t)(1 << bit);
                out += in[i++];
            }
        }
        out[flags_at] = (char)flags;
    }
    return out;
}

std::string base64(const std::string &in) {
    static const char *T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    for (size_t i = 0; i < in.size(); i += 3) {
        const uint32_t v = (uint8_t)in[i] << 16 | (i + 1 < in.size() ? (uint8_t)in[i + 1] << 8 : 0) | (i + 2 < in.size() ? (uint8_t)in[i + 2] : 0);
        o += T[v >> 18 & 63];
        o += T[v >> 12 & 63];
        o += i + 1 < in.size() ? T[v >> 6 & 63] : '=';
        o += i + 2 < in.size() ? T[v & 63] : '=';
    }
    return o;
}

// The bytes of every <b>...</b> in x, base64-decoded and joined (a game's player data, as saved)
std::string unbase64_b(const std::string &x) {
    std::string out;
    for (size_t at = x.find("<b>"); at != std::string::npos; at = x.find("<b>", at + 3)) {
        const size_t e = x.find("</b>", at);
        if (e == std::string::npos) break;
        uint32_t acc = 0;
        int bits = 0;
        for (size_t i = at + 3; i < e; i++) {
            const char c = x[i];
            int v = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' ? 62 : c == '/' ? 63 : -1;
            if (v < 0) continue;
            acc = acc << 6 | (uint32_t)v;
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                out += (char)(acc >> bits & 0xff);
            }
        }
    }
    return out;
}

std::string fmt(const char *f, unsigned a, unsigned b) {
    char s[64];
    snprintf(s, sizeof s, f, a, b);
    return s;
}

std::vector<uint8_t> EamuseServer::key(const std::string &info) { return ea::key(info); }
void EamuseServer::rc4(const std::vector<uint8_t> &k, std::string &d) { ea::rc4(k, d); }
std::string EamuseServer::clear(const std::string &m) { return ea::clear(m); }
bool EamuseServer::parse(const std::string &x, Call &c) { return ea::parse(x, c); }

// The PC pop'n games, libavs-win32's e-AMUSEMENT (runtime/pc/): pop'n 15, 16, 17, 18
bool pc_popn(const std::string &game) { return game == "G15" || game == "H16" || game == "I17" || game == "J39"; }
// The PC beatmania IIDX games on libavs's e-AMUSE3 (runtime/pc/, PLAN.md section 36): GOLD, DJ TROOPERS, EMPRESS
bool pc_iidx(const std::string &game) { return game == "GLD" || game == "HDD" || game == "I00"; }
std::string EamuseServer::read_file(const std::string &name) const {
    if (dir_.empty()) return "";
    std::ifstream f(dir_ + "/" + name, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

void EamuseServer::write_file(const std::string &name, const std::string &data) const {
    if (dir_.empty()) return;
    sys::make_dir(dir_);
    const std::string path = dir_ + "/" + name, tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << data;
        if (!f) return;
    }
    sys::replace_file(tmp, path);
}

std::string EamuseServer::answer(const Call &c, const Request &r, const std::string &host) {
    const std::string &m = r.module, &f = r.method;
    src_ = c.srcid;
    // pop'n 10 on (model C20, D11, E12, ...): their own calls and answer formats; pop'n 9 (C00)
    // and anything not handled there go on below
    const std::string game = c.model.substr(0, 3);
    // beatmania IIDX 9th style (model C02, the PC's e-AMUSE3, PLAN.md section 33): pop'n 9's calls
    // (numbering, userid, ntp, posevent, package: below) and two of its own
    // RED, HAPPY SKY and DistorteD (E11, ECO, FDD) the same way, their module named after the model
    const bool iidx_red = game == "E11" || game == "ECO" || game == "FDD";
    // GOLD on: DistorteD's modules and cards over libavs (pop'n 15's transport)
    if (pc_iidx(game) && (m == "pc" || m == "music" || m == "ranking" || m == "shop" || m == "grade"))
        return answer_iidx_fdd(game, r);
    if (game == "C02" || iidx_red) {
        // its module: method music / weekly / ranking / grade with a command line (easerver_iidx.cpp)
        if (m == "C02") return answer_c02(r);
        if (iidx_red && m == game) return answer_iidx_red(game, r);
        // DistorteD: its own modules, and its IC cards through the later games' services, cardmng and
        // facility (as pop'n 13's)
        if (game == "FDD" && (m == "pc" || m == "music" || m == "ranking" || m == "shop" || m == "grade"))
            return answer_iidx_fdd(game, r);
        if (game == "FDD")
            if (std::string a = answer_later(game, r, host); !a.empty()) return a;
        // the player: the card's record as the game wrote it (its DJ name, PIN, ...), its own file
        const std::string card = attr(r.attrs, "card");
        if (m == "userdata" && f == "read") {
            const std::string data = safe_name(card) ? read_file("userdata-" + game + "-" + card) : "";
            return data.empty() ? "<userdata status=\"109\"/>" : data;   // 109: none
        }
        if (m == "userdata" && f == "write") {
            if (!safe_name(card)) return "<userdata status=\"1\"/>";
            write_file("userdata-" + game + "-" + card, "<userdata status=\"0\" time=\"" + esc(attr(r.attrs, "time")) + "\">" + r.inner + "</userdata>");
            return "<userdata status=\"0\"/>";
        }
    } else if (!game.empty() && game != "C00") {
        if (std::string a = answer_later(game, r, host); !a.empty()) return a;
    }
    if (m == "numbering" && f == "assign")   // a new card's number (0x197530): 16 digits
        return "<numbering status=\"0\" number=\"" + next_number() + "\"/>";
    if (m == "userdata" && f == "read") {   // 109: not registered (0x184ca0)
        const std::string card = attr(r.attrs, "card");
        const std::string data = safe_name(card) ? read_file("userdata-" + card) : "";
        if (data.empty()) return "<userdata status=\"109\"/>";
        return with_shared_marks("C00", card, data);   // kept as the answer element itself, the card's marks put in
    }
    if (m == "userdata" && f == "write") {
        const std::string card = attr(r.attrs, "card");
        if (!safe_name(card)) return "<userdata status=\"1\"/>";
        write_file("userdata-" + card, "<userdata status=\"0\" time=\"" + esc(attr(r.attrs, "time")) + "\">" + r.inner + "</userdata>");
        return "<userdata status=\"0\"/>";
    }
    if (m == "userid") {   // KONAMI ID: a user and password, with cards (0x1863e8, 0x186620, 0x186818)
        std::vector<Record> users = from_lines(read_file("users"));
        const std::string card = attr(r.attrs, "card"), user = attr(r.attrs, "user"), pass = attr(r.attrs, "pass");
        auto owner = [&](const std::string &cd) -> Record * {
            for (Record &u : users) {
                const std::string cards = "," + attr(u, "cards") + ",";
                if (!cd.empty() && cards.find("," + cd + ",") != std::string::npos) return &u;
            }
            return nullptr;
        };
        auto named = [&](const std::string &n) -> Record * {
            for (Record &u : users)
                if (attr(u, "user") == n) return &u;
            return nullptr;
        };
        auto save = [&] {
            std::string t;
            for (const Record &u : users) t += to_line(u);
            write_file("users", t);
        };
        if (f == "ctou") {   // card to user: 112 = none
            const Record *u = owner(card);
            if (!u) return "<userid status=\"112\"/>";
            return "<userid status=\"0\" user=\"" + esc(attr(*u, "user")) + "\" active=\"" + esc(attr(*u, "active")) + "\"/>";
        }
        if (f == "adduser") {   // 113: the user exists, 114: the card is someone's
            if (user.empty() || named(user)) return "<userid status=\"113\"/>";
            if (owner(card)) return "<userid status=\"114\"/>";
            users.push_back({{"user", user}, {"pass", pass}, {"cards", card}, {"active", "1"}});
            save();
            return "<userid status=\"0\" alter=\"\"/>";
        }
        if (f == "addcard") {   // 115: no such user, 116: wrong password, 114: the card is someone's
            Record *u = named(user);
            if (!u) return "<userid status=\"115\"/>";
            if (attr(*u, "pass") != pass) return "<userid status=\"116\"/>";
            if (Record *o = owner(card); o && o != u) return "<userid status=\"114\"/>";
            if (!owner(card)) (*u)["cards"] = attr(*u, "cards").empty() ? card : attr(*u, "cards") + "," + card;
            save();
            return "<userid status=\"0\"/>";
        }
    }
    if (m == "rank") {
        if (f == "register") return "<rank status=\"0\">" + rank_register(BOARD_SCORE, r) + "</rank>";
        if (f == "register_h") return "<rank status=\"0\">" + rank_register(BOARD_H, r) + "</rank>";
        if (f == "register_gz") return "<rank status=\"0\">" + rank_register(BOARD_GZ, r) + "</rank>";
        if (f == "get") return "<rank status=\"0\">" + rank_get(BOARD_SCORE, r) + "</rank>";
        if (f == "get_h") return "<rank status=\"0\">" + rank_get(BOARD_H, r) + "</rank>";
        if (f == "get_gz") return "<rank status=\"0\">" + rank_get(BOARD_GZ, r) + "</rank>";
    }
    if (m == "eemall" && f == "i_get") {   // the card's ee'MALL items: eemall-<card>, one number a line
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
        return "<eemall status=\"0\">" + items + "</eemall>";
    }
    if (m == "services" && f == "get") {   // the other modules' URLs (0x18a808)
        // the names the methods look up (their entries' second word): rank, p_friend, eemall and
        // ntp all use "local"
        static const char *modules[] = {"posevent", "pcbtracker", "numbering", "package", "userdata", "userid", "local"};
        std::string items;
        for (const char *mod : modules) items += std::string("<item name=\"") + mod + "\" url=\"http://" + esc(host) + "/\"/>";
        return "<services expire=\"600\" status=\"0\">" + items + "</services>";
    }
    if (m == "ntp" && f == "get") {   // <time year.. sec../> (0x19b740)
        const time_t t = now ? (time_t)now() : 0;
        const struct tm tm = sys::utc(t);
        char b[160];
        snprintf(b, sizeof b, "<ntp status=\"0\"><time year=\"%d\" month=\"%d\" day=\"%d\" hour=\"%d\" min=\"%d\" sec=\"%d\"></time></ntp>",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
        return b;
    }
    if (m == "posevent" && (f == "sale" || f == "sales"))   // approve is required (0x18af68)
        return "<posevent status=\"0\" approve=\"1\" margin=\"0\"/>";
    if (m == "package" && f == "list") {   // (0x197960) the one package pop'n 9 takes: "message"
        const std::string text = message();
        if (text.empty()) return "<package status=\"0\"/>";
        // the game downloads url, at most size - 1 bytes, and scrolls it on its title (0x1237c0)
        return "<package status=\"0\"><item url=\"http://" + esc(host) + "/message\" name=\"message\" desc=\"\" size=\"" +
               std::to_string(text.size() + 1) + "\" pkgtype=\"all\" sumtype=\"\" sum=\"\" from=\"\" till=\"\"/></package>";
    }
    // services this server keeps nothing for: pcbtracker alive, posevent income, package intend,
    // p_friend get/state/ask
    return "<" + m + " status=\"0\"/>";
}

std::string EamuseServer::next_number() {
    uint64_t next = 1;
    if (std::string n = read_file("numbering"); !n.empty()) next = std::strtoull(n.c_str(), nullptr, 10);
    write_file("numbering", std::to_string(next + 1) + "\n");
    char digits[17];
    snprintf(digits, sizeof digits, "200301%09llu", (unsigned long long)next);   // 15 digits
    int sum = 0;   // a Luhn check digit ("card16m10")
    for (int i = 14, k = 0; i >= 0; i--, k++) {
        int d = digits[i] - '0';
        if (k % 2 == 0) { d *= 2; if (d > 9) d -= 9; }
        sum += d;
    }
    digits[15] = (char)('0' + (10 - sum % 10) % 10);
    digits[16] = 0;
    return digits;
}

// The owner's message for the cabinets' title screens (the file "message"), at most 255 bytes,
// one line; Shift-JIS or ASCII as the owner writes it.
std::string EamuseServer::message() const {
    std::string t = read_file("message");
    if (const size_t nl = t.find_first_of("\r\n"); nl != std::string::npos) t.erase(nl);
    if (t.size() > 255) t.resize(255);
    return t;
}

std::string EamuseServer::handle(const std::string &req) {
    if (req.compare(0, 4, "GET ") == 0) {   // a package's download: the message
        const size_t sp = req.find(' ', 4);
        const std::string path = req.substr(4, sp == std::string::npos ? std::string::npos : sp - 4);
        const std::string text = message();
        const bool ok = !text.empty() && path.size() >= 8 && path.compare(path.size() - 8, 8, "/message") == 0;
        if (log) log("GET " + path + (ok ? " -> the message, " + std::to_string(text.size()) + " bytes" : " -> 404"));
        if (!ok) return "HTTP/1.0 404 Not Found\r\nContent-Length: 0\r\n\r\n";
        return "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nContent-Length: " + std::to_string(text.size()) + "\r\n\r\n" + text;
    }
    const size_t head_end = req.find("\r\n\r\n");
    const std::string head = req.substr(0, head_end), info = header(head, "X-Eamuse-Info");
    std::string body = head_end == std::string::npos ? "" : req.substr(head_end + 4);
    if (!info.empty()) rc4(key(info), body);
    // packed (X-Compress: lz77), and binary XML (pop'n 18's libavs: kbin.h), read as text; the
    // answer goes back in kind
    std::string packing = header(head, "X-Compress");
    for (char &ch : packing) ch = (char)std::tolower((unsigned char)ch);
    if (packing == "lz77") body = kbin::lz77_unpack(body);
    const bool binary = kbin::is_binary(body);
    if (binary) {
        std::string text;
        if (kbin::to_text(body, text)) body = text;
    }
    Call c;
    std::string xml;
    if (parse(body, c)) {
        xml = "<response>";
        for (const Request &r : c.requests) {
            if (on_call) on_call(r.module + "." + (r.method.empty() ? attr(r.attrs, "type") : r.method));
            // module elements written out as <x ...></x> (the game's parser also takes <x .../>)
            std::string a = answer(c, r, header(head, "Host"));
            if (a.size() > 2 && a.compare(a.size() - 2, 2, "/>") == 0 && a.find('>') == a.size() - 1)
                a = a.substr(0, a.size() - 2) + "></" + r.module + ">";
            xml += a;
        }
        xml += "</response>";
    }
    if (log) {   // a binary body in hex
        std::string shown = body;
        if (!body.empty() && (unsigned char)body[0] != '<') {
            static const char *hx = "0123456789abcdef";
            shown.clear();
            for (unsigned char ch : body) shown += hx[ch >> 4], shown += hx[ch & 15];
        }
        log(std::string("e-AMUSE3 call") + (info.empty() ? " (plain): " : ": ") + shown + "\ne-AMUSE3 answer: " + (xml.empty() ? "(not understood)" : xml));
    }
    if (xml.empty()) return "HTTP/1.0 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
    return reply(req, xml, binary);
}

std::string EamuseServer::reply(const std::string &req, std::string xml, bool binary) {
    const std::string info = header(req.substr(0, req.find("\r\n\r\n")), "X-Eamuse-Info");
    std::string packing;
    if (binary) {   // a binary call: a binary answer, not packed
        std::string bin;
        if (kbin::from_text(xml, bin)) xml = bin, packing = "X-Compress: none\r\n";
    }
    // Answer in kind: an encrypted call gets an encrypted answer; a plain one (ntp, rank, ...: their
    // packets have no cipher) a plain answer with no X-Eamuse-Info, which the game insists on (0x190290).
    std::string info_line;
    if (!info.empty()) {
        serial_++;
        const std::string out_info = "1-" + fmt("%08x-%04x", 0x484b0000u + serial_, serial_ & 0xffff);
        rc4(key(out_info), xml);
        info_line = "X-Eamuse-Info: " + out_info + "\r\n";
    }
    return "HTTP/1.0 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: " + std::to_string(xml.size()) +
           "\r\n" + info_line + packing + "\r\n" + xml;
}

void EamuseServer::edit(std::string &m, const std::function<void(std::string &)> &fn) {
    const size_t he = m.find("\r\n\r\n");
    if (he == std::string::npos) return;
    std::string head = m.substr(0, he);
    const std::string info = header(head, "X-Eamuse-Info");
    std::string body = m.substr(he + 4);
    if (!info.empty()) rc4(key(info), body);
    fn(body);
    if (!info.empty()) rc4(key(info), body);
    for (size_t at = head.find("\r\n"); at != std::string::npos; at = head.find("\r\n", at + 2)) {
        if (strncasecmp(head.c_str() + at + 2, "Content-Length:", 15) != 0) continue;
        const size_t e = head.find("\r\n", at + 2);
        head.replace(at + 2, (e == std::string::npos ? head.size() : e) - at - 2, "Content-Length: " + std::to_string(body.size()));
        break;
    }
    m = head + "\r\n\r\n" + body;
}

void EamuseServer::set_attr(std::string &xml, const std::string &name, const std::string &value) {
    const std::string key = name + "=\"";
    for (size_t at = xml.find(key); at != std::string::npos; at = xml.find(key, at + 1)) {
        if (at == 0 || !std::isspace((unsigned char)xml[at - 1])) continue;   // srcid, not xsrcid
        const size_t b = at + key.size(), e = xml.find('"', b);
        if (e == std::string::npos) return;
        xml.replace(b, e - b, value);
    }
}


}  // namespace hk::py
