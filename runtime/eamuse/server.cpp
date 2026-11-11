// SPDX-License-Identifier: GPL-3.0-or-later
// A local e-AMUSE3 server (server.h).
#include "eamuse/server.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>

namespace hk::ea {

std::string Server::services(const std::string &host, const std::vector<const char *> &names, const char *module) {
    std::string items;
    for (const char *n : names) items += std::string("<item name=\"") + n + "\" url=\"http://" + esc(host) + "/\"/>";
    return std::string("<") + module + " expire=\"600\" status=\"0\">" + items + "</" + module + ">";
}

std::string Server::message() const {
    std::string t = store_.read("message");
    if (const size_t nl = t.find_first_of("\r\n"); nl != std::string::npos) t.erase(nl);
    if (t.size() > 255) t.resize(255);
    return t;
}

// The modules pop'n music 9 to 14 and GuitarFreaks/DrumMania answer alike (the Python core's
// docs/EAMUSE3.md, "The modules")
std::string Server::common(const Call &, const Request &r) {
    const std::string &m = r.module, &f = r.method;
    if (m == "numbering" && f == "assign") {   // a new card's number: 16 digits
        uint64_t next = 1;
        if (std::string n = store_.read("numbering"); !n.empty()) next = std::strtoull(n.c_str(), nullptr, 10);
        store_.write("numbering", std::to_string(next + 1) + "\n");
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
        return std::string("<numbering status=\"0\" number=\"") + digits + "\"/>";
    }
    if (m == "userid") {   // KONAMI ID: a user and password, with cards
        std::vector<Record> users = from_lines(store_.read("users"));
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
            store_.write("users", t);
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
    if (m == "posevent" && (f == "sale" || f == "sales"))   // approve is required
        return "<posevent status=\"0\" approve=\"1\" margin=\"0\"/>";
    // services this server keeps nothing for: pcbtracker alive, posevent income, ...
    return "<" + m + " status=\"0\"/>";
}

std::string Server::handle(const std::string &req) try {
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
    Call c;
    std::string xml;
    if (parse(body, c)) {
        // a cabinet the server wasn't told to take: no answer to read anything from
        if (admit && !admit(c.srcid)) return "HTTP/1.0 403 Forbidden\r\nContent-Length: 0\r\n\r\n";
        xml = "<response>";
        // (the data folder is this call's to its answer: the servers sharing it, python/easerver.cpp)
        const sys::FileLock folder = store_.lock();
        for (const Request &r : c.requests) {
            std::string a = game_ ? game_(*this, c, r, header(head, "Host")) : "";
            if (a.empty()) a = common(c, r);
            // module elements written out as <x ...></x> (pop'n's parser also takes <x .../>)
            if (a.size() > 2 && a.compare(a.size() - 2, 2, "/>") == 0 && a.find('>') == a.size() - 1)
                a = a.substr(0, a.size() - 2) + "></" + r.module + ">";
            xml += a;
        }
        xml += "</response>";
    }
    if (log) log(std::string("e-AMUSE3 call") + (info.empty() ? " (plain): " : ": ") + body + "\ne-AMUSE3 answer: " + (xml.empty() ? "(not understood)" : xml));
    if (xml.empty()) return "HTTP/1.0 400 Bad Request\r\nContent-Length: 0\r\n\r\n";

    // Answer in kind: an encrypted call gets an encrypted answer; a plain one a plain answer with
    // no X-Eamuse-Info, which pop'n insists on.
    std::string info_line;
    if (!info.empty()) {
        serial_++;
        char out_info[32];
        snprintf(out_info, sizeof out_info, "1-%08x-%04x", 0x484b0000u + serial_, serial_ & 0xffff);
        rc4(key(out_info), xml);
        info_line = std::string("X-Eamuse-Info: ") + out_info + "\r\n";
    }
    return "HTTP/1.0 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: " + std::to_string(xml.size()) +
           "\r\n" + info_line + "\r\n" + xml;
} catch (const std::exception &e) {
    // a call this server's code trips over (a field shorter or longer than it reckoned with) is one
    // bad call, answered as such: never the end of the server and of every other cabinet's session
    if (log) log(std::string("e-AMUSE3 call not answered: ") + e.what());
    return "HTTP/1.0 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n";
}

}  // namespace hk::ea
