// SPDX-License-Identifier: GPL-3.0-or-later
// hiraku-eamuse: HIRAKU's e-AMUSE3 server on the network. The same servers the cores run inside
// themselves offline, here behind a plain HTTP/1.0 listener, so several cabinets (HIRAKU with
// -url host:port) can share cards, player data and rankings: Konami Python 1 pop'n music 9-14
// (runtime/python/easerver.cpp) and the System 573 GuitarFreaks and DrumMania
// (runtime/sys573/eagfdm.h), such as the two cabinets of a session. A call goes to one or the
// other by its model; both keep their files in the same folder (one card numbering, one set of
// KONAMI IDs). docs/EAMUSE3.md describes the protocol.
//
// It also runs NET TAISEN's lobby and relay for pop'n music 12-14 cabinets (runtime/python/netrelay.h),
// on TCP port 5740 of the same address.
//
//   hiraku-eamuse [--port 8080] [--bind 0.0.0.0] [--data DIR] [--allow ADDRESSES] [--cabinets FILE]
//                 [--key-file FILE] [--clock "2003-06-01 12:00:00"] [--tz 9] [--quiet] [--no-relay]
//   hiraku-eamuse [--data DIR] [--cabinets FILE] --new-cabinet [NAME]
//   hiraku-eamuse [--data DIR] [--key-file FILE] --new-key
//     --data   where it keeps what it must remember (default ./eamuse-data)
//     --allow  the addresses it takes connections from, calls and relay: 192.168.1.0/24,10.0.0.5
//              (and this computer's own); without it, any
//     --cabinets  the cabinets it answers, a PCBID a line (default: cabinets.txt in the data folder,
//              when there is one); without the file, any. Read again whenever it changes
//     --new-cabinet  a PCBID nobody else has, added to that file with NAME after it and printed: the
//              cabinet is started with -p PCBID (the launcher: Settings, e-AMUSEMENT, PCBID)
//     --key-file  the file with the server's key (default: key.txt in the data folder, when there is
//              one). With a key, calls and relay are taken only behind it (shell/keyed.h): nothing
//              goes over the network in clear, and who hasn't the key is answered nothing. Each
//              cabinet is given the same key (-eakey KEY; the launcher: Settings, e-AMUSEMENT, Server key)
//     --new-key  a key nobody can guess, written to that file and printed
//     --clock  the cabinets' time (ntp.get), running from this start (default: the host's clock)
//     --tz     with the host's clock: the cabinets' time zone, hours from UTC (default 9, Japan: the
//              games set their clocks to what ntp.get says and stamp their own times "+9")
//     --quiet  don't print each call
//     --no-relay  no NET TAISEN relay
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "eamuse/access.h"
#include "eamuse/server.h"
#include "python/easerver.h"
#include "python/lobby.h"
#include "python/netrelay.h"
#include "shell/keyed.h"
#include "shell/net.h"
#include "shell/sys.h"
#include "sys573/eagfdm.h"

#ifndef _WIN32
#include <fcntl.h>
#endif

namespace {

// One HTTP/1.0 request from a connection (recv: its bytes as they come, in clear), added to what
// req already holds of it: the head and Content-Length bytes of body, all of it before `until` (a
// connection that sends a byte now and then is let go, not waited on for ever)
bool read_request(const std::function<ssize_t(char *, size_t)> &recv, std::string &req, std::chrono::steady_clock::time_point until) {
    char buf[4096];
    for (;;) {
        if (std::chrono::steady_clock::now() > until) return false;
        const size_t he = req.find("\r\n\r\n");
        if (he != std::string::npos) {
            std::string head = req.substr(0, he);
            for (char &c : head) c = (char)std::tolower((unsigned char)c);
            size_t body = 0;
            if (const size_t cl = head.find("\r\ncontent-length:"); cl != std::string::npos)
                body = std::strtoul(head.c_str() + cl + 17, nullptr, 10);
            if (body > (1u << 20)) return false;
            if (req.size() >= he + 4 + body) return true;
        }
        if (req.size() > (1u << 21)) return false;
        const ssize_t n = recv(buf, sizeof buf);
        if (n <= 0) return false;
        req.append(buf, (size_t)n);
    }
}

// A new file that only its owner reads (a key's). False if it is there already or can't be written
bool write_private(const std::string &file, const std::string &text) {
#ifdef _WIN32
    return hk::sys::write_file(file, text);   // (a folder's own access rules are the file's)
#else
    const int fd = ::open(file.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return false;
    const bool ok = ::write(fd, text.data(), text.size()) == (ssize_t)text.size();
    return ::close(fd) == 0 && ok;
#endif
}

// The key in a file: its first line that isn't empty or a comment (#). "" if there is none
std::string key_in(const std::string &file) {
    std::ifstream f(file);
    std::string line;
    while (std::getline(f, line)) {
        const size_t from = line.find_first_not_of(" \t\r"), to = line.find_last_not_of(" \t\r");
        if (from != std::string::npos && line[from] != '#') return line.substr(from, to - from + 1);
    }
    return "";
}

int64_t parse_clock(const char *s) {
    struct tm tm {};
    if (sscanf(s, "%d-%d-%d %d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) < 3)
        return -1;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    return (int64_t)hk::sys::from_utc(tm);
}

}  // namespace

int main(int argc, char **argv) {
    int port = 8080;
    std::string bind_addr = "0.0.0.0", data = "eamuse-data";
    int64_t clock_start = -1;
    int tz_hours = 9;
    bool quiet = false, relay_on = true, new_cabinet = false, new_key = false;
    std::string cabinets_file, new_name, key_file;
    hk::ea::AddressList allowed;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto val = [&]() -> const char * {
            if (i + 1 >= argc) { fprintf(stderr, "%s needs a value\n", a.c_str()); exit(2); }
            return argv[++i];
        };
        if (a == "--port") port = atoi(val());
        else if (a == "--bind") bind_addr = val();
        else if (a == "--data") data = val();
        else if (a == "--clock") {
            clock_start = parse_clock(val());
            if (clock_start < 0) { fprintf(stderr, "--clock: YYYY-MM-DD[ HH:MM:SS]\n"); return 2; }
        } else if (a == "--tz") tz_hours = atoi(val());
        else if (a == "--allow") {
            std::string error;
            if (!allowed.add(val(), &error)) { fprintf(stderr, "--allow: %s\n", error.c_str()); return 2; }
        } else if (a == "--cabinets") cabinets_file = val();
        else if (a == "--new-cabinet") {   // (its name is optional: the next word unless that is an option)
            new_cabinet = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') new_name = argv[++i];
        } else if (a == "--key-file") key_file = val();
        else if (a == "--new-key") new_key = true;
        else if (a == "--quiet") quiet = true;
        else if (a == "--no-relay") relay_on = false;
        else {
            fprintf(stderr, "usage: hiraku-eamuse [--port N] [--bind ADDR] [--data DIR] [--allow ADDRESSES] [--cabinets FILE] "
                            "[--key-file FILE] [--clock \"YYYY-MM-DD HH:MM:SS\"] [--tz H] [--quiet] [--no-relay]\n"
                            "       hiraku-eamuse [--data DIR] [--cabinets FILE] --new-cabinet [NAME]\n"
                            "       hiraku-eamuse [--data DIR] [--key-file FILE] --new-key\n"
                            "  --allow ADDRESSES   take connections only from these (192.168.1.0/24,10.0.0.5) and this computer\n"
                            "  --cabinets FILE     answer only the cabinets whose PCBIDs it lists, one a line\n"
                            "                      (default: cabinets.txt in the data folder, when there is one)\n"
                            "  --new-cabinet NAME  add a new PCBID to that file and print it, for the cabinet's -p\n"
                            "  --key-file FILE     the server's key (default: key.txt in the data folder, when there is one):\n"
                            "                      calls and relay only behind it, nothing in clear on the network\n"
                            "  --new-key           make a key, write it to that file and print it, for the cabinets' -eakey\n");
            return a == "--help" || a == "-h" ? 0 : 2;
        }
    }
    hk::sys::make_dir(data);
    // The cabinets it answers (eamuse/access.h)
    hk::ea::CabinetList cabinets;
    cabinets.open(cabinets_file.empty() ? data + "/cabinets.txt" : cabinets_file);
    if (new_cabinet) {
        const std::string id = hk::ea::new_pcbid();
        if (!cabinets.add(id, new_name)) { fprintf(stderr, "hiraku-eamuse: can't write %s\n", cabinets.file().c_str()); return 1; }
        printf("%s\n", id.c_str());
        fprintf(stderr, "added to %s%s%s: start that cabinet with -p %s (the launcher: Settings, e-AMUSEMENT, PCBID). "
                        "From now on only the cabinets in that file are answered\n",
                cabinets.file().c_str(), new_name.empty() ? "" : " as ", new_name.c_str(), id.c_str());
        return 0;
    }
    // The server's key (shell/keyed.h)
    const bool key_named = !key_file.empty();
    if (!key_named) key_file = data + "/key.txt";
    if (new_key) {
        if (std::ifstream(key_file).good()) {
            fprintf(stderr, "hiraku-eamuse: %s is there already (its key is the server's; to make another, remove the file, "
                            "and give every cabinet the new one)\n", key_file.c_str());
            return 1;
        }
        const std::string text = hk::net::Key::make();
        if (text.empty() || !write_private(key_file, "# hiraku-eamuse's key: the cabinets' -eakey. Who has it is a cabinet of this server's\n" + text + "\n")) {
            fprintf(stderr, "hiraku-eamuse: can't %s\n", text.empty() ? "make a key (the system gives no random numbers)" : ("write " + key_file).c_str());
            return 1;
        }
        printf("%s\n", text.c_str());
        fprintf(stderr, "written to %s: start each cabinet with -eakey %s (the launcher: Settings, e-AMUSEMENT, Server key). "
                        "From now on this server takes calls and the relay's connections only behind that key\n",
                key_file.c_str(), text.c_str());
        return 0;
    }
    hk::net::Key key;
    if (const bool there = std::ifstream(key_file).good(); there || key_named) {
        const std::string text = key_in(key_file), wrong = there ? hk::net::Key::wrong_with(text) : "no such file (--new-key makes it)";
        if (!wrong.empty()) {   // (never a server that was meant to have a key and runs without)
            fprintf(stderr, "hiraku-eamuse: %s: %s\n", key_file.c_str(), wrong.c_str());
            return 1;
        }
        key = hk::net::Key(text);
    }
    if (!cabinets_file.empty() && !cabinets.in_force()) {
        fprintf(stderr, "hiraku-eamuse: --cabinets: no %s (--new-cabinet makes it)\n", cabinets_file.c_str());
        return 1;
    }
    // GuitarFreaks xx39, DrumMania xx40 ("D40:J:A:A"), by a model's first three letters; J39 and M39
    // are pop'n music's (18, and the later PC games)
    auto gfdm_model = [](const std::string &m) {
        return m.size() >= 3 && (m.compare(1, 2, "39") == 0 || m.compare(1, 2, "40") == 0) && m.compare(0, 3, "J39") != 0 &&
               m.compare(0, 3, "M39") != 0;
    };
    // the game's own modules, by its model
    hk::ea::Server server(data, [gfdm_model](hk::ea::Server &s, const hk::ea::Call &c, const hk::ea::Request &r, const std::string &host) {
        if (gfdm_model(c.model)) return hk::k573::Eamuse::answer(s, c, r, host);
        return std::string();
    });
    hk::py::EamuseServer popn(data);   // pop'n music 9-14 (Konami Python 1), and every other model
    const int64_t started = (int64_t)time(nullptr);
    // the cabinets' wall clock
    auto now = [&] {
        return clock_start < 0 ? (int64_t)time(nullptr) + tz_hours * 3600 : clock_start + ((int64_t)time(nullptr) - started);
    };
    server.now = now;
    popn.now = now;
    if (!quiet) {
        server.log = [](const std::string &s) { printf("%s\n", s.c_str()); fflush(stdout); };
        popn.log = server.log;
    }
    // What was refused, said whatever --quiet says (a cabinet that gets no answer is looked for here), each
    // once a minute at the most
    std::mutex noting;
    std::map<std::string, std::chrono::steady_clock::time_point> noted;
    auto note = [&](const std::string &what, const std::string &line) {
        std::lock_guard<std::mutex> one(noting);
        const auto t = std::chrono::steady_clock::now();
        if (noted.size() > 256) noted.clear();
        const auto [at, fresh] = noted.try_emplace(what, t);
        if (!fresh && t - at->second < std::chrono::minutes(1)) return;
        at->second = t;
        printf("hiraku-eamuse: %s\n", line.c_str());
        fflush(stdout);
    };
    auto dotted = [](uint32_t ip) {
        char s[20];
        snprintf(s, sizeof s, "%u.%u.%u.%u", ip >> 24, ip >> 16 & 255, ip >> 8 & 255, ip & 255);
        return std::string(s);
    };
    auto from_allowed = [&](uint32_t ip, const char *port_name) {
        if (allowed.has(ip)) return true;
        note("address " + dotted(ip), "refused: a connection from " + dotted(ip) + " (" + port_name + "), which --allow doesn't name");
        return false;
    };
    std::string asking;   // the address the call being answered came from (the calls are answered one at a time)
    auto admit = [&](const std::string &srcid) {
        if (cabinets.has(srcid)) return true;
        const std::string id = hk::ea::pcbid_of(srcid);
        note("cabinet " + srcid, "refused: a call from " + asking + " as cabinet " + (id.empty() ? "\"" + srcid.substr(0, 40) + "\" (not a PCBID)" : id) +
                                     ", which " + cabinets.file() + " doesn't list" + (id.empty() ? "" : " (a line with that PCBID lets it in)"));
        return false;
    };
    server.admit = admit;
    popn.admit = admit;
    // GuitarFreaks and DrumMania by their model ("D40:J:A:A"), read from the call in clear
    auto gfdm = [gfdm_model](const std::string &req) {
        std::string copy = req, model;
        hk::py::EamuseServer::edit(copy, [&](std::string &xml) {
            const size_t at = xml.find("model=\"");
            if (at != std::string::npos) model = xml.substr(at + 7, 3);
        });
        return gfdm_model(model);
    };

#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif
    const int ls = hk::net::socket(AF_INET, SOCK_STREAM, 0);
    hk::net::set_int(ls, SOL_SOCKET, SO_REUSEADDR, 1);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, bind_addr.c_str(), &sa.sin_addr) != 1) { fprintf(stderr, "--bind: an IPv4 address\n"); return 2; }
    if (bind(ls, (sockaddr *)&sa, sizeof sa) != 0 || listen(ls, 16) != 0) {
        fprintf(stderr, "hiraku-eamuse: %s:%d: %s\n", bind_addr.c_str(), port, hk::net::error().c_str());
        return 1;
    }
    printf("hiraku-eamuse: e-AMUSE3 on %s:%d, data in %s\n", bind_addr.c_str(), port, data.c_str());
    // e-AMUSE3 has no log-in of its own: a cabinet is whoever sends its calls, unless told which to take
    if (!allowed.empty()) printf("hiraku-eamuse: connections taken from %s and this computer only\n", allowed.text().c_str());
    if (cabinets.in_force()) {
        printf("hiraku-eamuse: calls answered for the %zu cabinet%s in %s only\n", cabinets.size(), cabinets.size() == 1 ? "" : "s",
               cabinets.file().c_str());
        for (const std::string &line : cabinets.not_read()) printf("hiraku-eamuse: %s, line %s: not a PCBID (20 hex digits)\n", cabinets.file().c_str(), line.c_str());
    }
    if (key.set())
        printf("hiraku-eamuse: calls and relay taken only behind the key in %s; nothing goes over the network in clear\n", key_file.c_str());
    else if (bind_addr != "127.0.0.1" && allowed.empty() && !cabinets.in_force())
        printf("hiraku-eamuse: whoever can reach this port can read and change the players kept here: for a network you trust. "
               "--new-key gives the server a key that every cabinet must have, --allow ADDRESSES takes connections from those "
               "only, --new-cabinet starts a list of the cabinets answered (--bind 127.0.0.1: this computer only)\n");
    else if (bind_addr != "127.0.0.1" && allowed.empty())
        printf("hiraku-eamuse: NET TAISEN's relay takes any address (it carries no PCBID): --allow ADDRESSES or a key (--new-key) for that\n");
    else if (bind_addr != "127.0.0.1")
        printf("hiraku-eamuse: what is said goes over the network in clear (PCBIDs, cards, PINs): a key (--new-key) hides it\n");
    auto turned_away = [&](uint32_t ip, const char *port_name, const char *why) {
        note("key " + dotted(ip) + why, "refused: a connection from " + dotted(ip) + " (" + port_name + ") " + why);
    };
    hk::py::RelayServer relay;
    if (relay_on) {
        relay.log = server.log;
        relay.key = key;
        relay.turned_away = [&](uint32_t ip, const char *why) { turned_away(ip, "the relay", why); };
        relay.admit = [&](uint32_t ip) { return from_allowed(ip, "the relay"); };
        if (relay.start(bind_addr, hk::py::Lobby::PORT))
            printf("hiraku-eamuse: NET TAISEN lobby and relay on %s:%u\n", bind_addr.c_str(), hk::py::Lobby::PORT);
    }
    fflush(stdout);
    // The game makes one request per connection (HTTP/1.0). Each connection is read and answered on a
    // thread of its own, so one that is slow (or says nothing) holds up no other cabinet; the calls
    // themselves are answered one at a time (the servers' files and counters are one cabinet's at a
    // time), which takes no longer than it did. At most kConnections at once, each kSeconds in all.
    constexpr int kConnections = 64, kSeconds = 15;
    std::mutex answering;
    std::atomic<int> connections{0};
    for (;;) {
        sockaddr_in peer{};
        const int fd = hk::net::accept(ls, &peer);
        if (fd < 0) {   // (no descriptors left, say: not a loop that spins)
            hk::sys::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        const uint32_t ip = ntohl(peer.sin_addr.s_addr);
        if (connections >= kConnections || !from_allowed(ip, "e-AMUSE3")) {
            hk::net::close(fd);
            continue;
        }
        connections++;
        auto serve = [&, fd, ip] {
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(kSeconds);
            hk::net::set_timeouts(fd, 3000);
            // The connection's first four bytes say which it is: a cabinet with a key ("HKK1",
            // shell/keyed.h) or a call in clear. A server with a key reads nothing of the second
            // kind, and nothing of the first until a record of it has held.
            std::string req;
            char first[4];
            bool come = true;
            for (size_t got = 0; come && got < sizeof first;) {
                const ssize_t n = hk::net::recv(fd, first + got, sizeof first - got);
                if (n <= 0 || std::chrono::steady_clock::now() > until) come = false;
                else got += (size_t)n;
            }
            hk::net::Keyed keyed;
            const bool with_key = come && hk::net::keyed::is_hello(first);
            if (!come) {
            } else if (with_key && !key.set()) {
                hk::net::send(fd, hk::net::keyed::kNoKey, 4);
                turned_away(ip, "e-AMUSE3", "with a key, and this server has none");
                come = false;
            } else if (!with_key && key.set()) {
                turned_away(ip, "e-AMUSE3", "without the key");
                come = false;
            } else if (with_key) {
                come = keyed.accept(fd, key, (const uint8_t *)first);
            } else {
                req.assign(first, sizeof first);
            }
            auto recv = [&](char *p, size_t n) { return with_key ? keyed.recv(p, n) : hk::net::recv(fd, p, n); };
            if (come && read_request(recv, req, until)) {
                std::string answer;
                {
                    std::lock_guard<std::mutex> one(answering);
                    asking = dotted(ip);
                    answer = gfdm(req) ? server.handle(req) : popn.handle(req);
                }
                if (with_key) {
                    keyed.send(answer.data(), answer.size());
                } else {
                    for (size_t sent = 0; sent < answer.size() && std::chrono::steady_clock::now() < until;) {
                        const ssize_t n = hk::net::send(fd, answer.data() + sent, answer.size() - sent);
                        if (n <= 0) break;
                        sent += (size_t)n;
                    }
                }
            } else if (come && with_key && keyed.wrong()) {
                turned_away(ip, "e-AMUSE3", "with another key");
            }
            hk::net::close(fd);
            connections--;
        };
        try {
            std::thread(serve).detach();
        } catch (const std::exception &) {   // no thread to be had: this connection on the main one
            serve();
        }
    }
}
