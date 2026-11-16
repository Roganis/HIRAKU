// SPDX-License-Identifier: GPL-3.0-or-later
// The e-AMUSEMENT server on a network (listener.h): hiraku-eamuse's and hiraku-server's listener.
#include "eamuse/listener.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "eamuse/lobby.h"
#include "eamuse/netrelay.h"
#include "eamuse/server.h"
#include "shell/net.h"
#include "shell/sys.h"

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace hk::ea {

namespace {

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

int Listener::option(int argc, char **argv, int &i) {
    const std::string a = argv[i];
    auto val = [&]() -> const char * {
        if (i + 1 >= argc) { fprintf(stderr, "%s needs a value\n", a.c_str()); exit(2); }
        return argv[++i];
    };
    if (a == "--port") port = atoi(val());
    else if (a == "--bind") bind = val();
    else if (a == "--clock") {
        clock_start = parse_clock(val());
        if (clock_start < 0) { fprintf(stderr, "--clock: YYYY-MM-DD[ HH:MM:SS]\n"); return 2; }
    } else if (a == "--tz") tz_hours = atoi(val());
    else if (a == "--allow") {
        std::string error;
        if (!allowed.add(val(), &error)) { fprintf(stderr, "--allow: %s\n", error.c_str()); return 2; }
    } else if (a == "--cabinets") cabinets_file = val();
    else if (a == "--key-file") key_file = val();
    else if (a == "--quiet") quiet = true;
    else if (a == "--no-relay") relay = false;
    else return 0;
    return 1;
}

int Listener::start(const std::string &folder, bool new_cabinet, const std::string &new_name, bool new_key) {
    // The cabinets it answers (eamuse/access.h)
    cabinets.open(cabinets_file.empty() ? folder + "/cabinets.txt" : cabinets_file);
    if (new_cabinet) {
        const std::string id = new_pcbid();
        if (!cabinets.add(id, new_name)) { fprintf(stderr, "%s: can't write %s\n", program, cabinets.file().c_str()); return 1; }
        printf("%s\n", id.c_str());
        fprintf(stderr, "added to %s%s%s: start that cabinet with -p %s (the launcher: Settings, e-AMUSEMENT, PCBID). "
                        "From now on only the cabinets in that file are answered\n",
                cabinets.file().c_str(), new_name.empty() ? "" : " as ", new_name.c_str(), id.c_str());
        return 0;
    }
    // The server's key (shell/keyed.h)
    const bool key_named = !key_file.empty();
    if (!key_named) key_file = folder + "/key.txt";
    if (new_key) {
        if (std::ifstream(key_file).good()) {
            fprintf(stderr, "%s: %s is there already (its key is the server's; to make another, remove the file, "
                            "and give every cabinet the new one)\n", program, key_file.c_str());
            return 1;
        }
        const std::string text = net::Key::make();
        if (text.empty() || !write_private(key_file, std::string("# ") + program + "'s key: the cabinets' -eakey. Who has it is a cabinet of this server's\n" + text + "\n")) {
            fprintf(stderr, "%s: can't %s\n", program, text.empty() ? "make a key (the system gives no random numbers)" : ("write " + key_file).c_str());
            return 1;
        }
        printf("%s\n", text.c_str());
        fprintf(stderr, "written to %s: start each cabinet with -eakey %s (the launcher: Settings, e-AMUSEMENT, Server key). "
                        "From now on this server takes calls and the relay's connections only behind that key\n",
                key_file.c_str(), text.c_str());
        return 0;
    }
    if (const bool there = std::ifstream(key_file).good(); there || key_named) {
        const std::string text = key_in(key_file), wrong = there ? net::Key::wrong_with(text) : "no such file (--new-key makes it)";
        if (!wrong.empty()) {   // (never a server that was meant to have a key and runs without)
            fprintf(stderr, "%s: %s: %s\n", program, key_file.c_str(), wrong.c_str());
            return 1;
        }
        key = net::Key(text);
    }
    if (!cabinets_file.empty() && !cabinets.in_force()) {
        fprintf(stderr, "%s: --cabinets: no %s (--new-cabinet makes it)\n", program, cabinets_file.c_str());
        return 1;
    }
    return -1;
}

int Listener::serve(Server &server, const std::string &kept) {
    const int64_t started = (int64_t)time(nullptr);
    const int64_t clock0 = clock_start;
    const int tz = tz_hours;
    // the cabinets' wall clock
    server.now = [clock0, tz, started] {
        return clock0 < 0 ? (int64_t)time(nullptr) + tz * 3600 : clock0 + ((int64_t)time(nullptr) - started);
    };
    if (!quiet) server.log = [](const std::string &s) { printf("%s\n", s.c_str()); fflush(stdout); };
    const char *const me = program;
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
        printf("%s: %s\n", me, line.c_str());
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
    server.admit = [&](const std::string &srcid) {
        if (cabinets.has(srcid)) return true;
        const std::string id = pcbid_of(srcid);
        note("cabinet " + srcid, "refused: a call from " + asking + " as cabinet " + (id.empty() ? "\"" + srcid.substr(0, 40) + "\" (not a PCBID)" : id) +
                                     ", which " + cabinets.file() + " doesn't list" + (id.empty() ? "" : " (a line with that PCBID lets it in)"));
        return false;
    };

#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif
    const int ls = net::socket(AF_INET, SOCK_STREAM, 0);
    net::set_int(ls, SOL_SOCKET, SO_REUSEADDR, 1);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, bind.c_str(), &sa.sin_addr) != 1) { fprintf(stderr, "--bind: an IPv4 address\n"); return 2; }
    if (::bind(ls, (sockaddr *)&sa, sizeof sa) != 0 || listen(ls, 16) != 0) {
        fprintf(stderr, "%s: %s:%d: %s\n", me, bind.c_str(), port, net::error().c_str());
        return 1;
    }
    printf("%s: e-AMUSE3 on %s:%d, data in %s\n", me, bind.c_str(), port, kept.c_str());
    // e-AMUSE3 has no log-in of its own: a cabinet is whoever sends its calls, unless told which to take
    if (!allowed.empty()) printf("%s: connections taken from %s and this computer only\n", me, allowed.text().c_str());
    if (cabinets.in_force()) {
        printf("%s: calls answered for the %zu cabinet%s in %s only\n", me, cabinets.size(), cabinets.size() == 1 ? "" : "s",
               cabinets.file().c_str());
        for (const std::string &line : cabinets.not_read()) printf("%s: %s, line %s: not a PCBID (20 hex digits)\n", me, cabinets.file().c_str(), line.c_str());
    }
    if (key.set())
        printf("%s: calls and relay taken only behind the key in %s; nothing goes over the network in clear\n", me, key_file.c_str());
    else if (bind != "127.0.0.1" && allowed.empty() && !cabinets.in_force())
        printf("%s: whoever can reach this port can read and change the players kept here: for a network you trust. "
               "--new-key gives the server a key that every cabinet must have, --allow ADDRESSES takes connections from those "
               "only, --new-cabinet starts a list of the cabinets answered (--bind 127.0.0.1: this computer only)\n", me);
    else if (bind != "127.0.0.1" && allowed.empty())
        printf("%s: NET TAISEN's relay takes any address (it carries no PCBID): --allow ADDRESSES or a key (--new-key) for that\n", me);
    else if (bind != "127.0.0.1")
        printf("%s: what is said goes over the network in clear (PCBIDs, cards, PINs): a key (--new-key) hides it\n", me);
    auto turned_away = [&](uint32_t ip, const char *port_name, const char *why) {
        note("key " + dotted(ip) + why, "refused: a connection from " + dotted(ip) + " (" + port_name + ") " + why);
    };
    RelayServer relay_server;
    if (relay) {
        relay_server.log = server.log;
        relay_server.key = key;
        relay_server.turned_away = [&](uint32_t ip, const char *why) { turned_away(ip, "the relay", why); };
        relay_server.admit = [&](uint32_t ip) { return from_allowed(ip, "the relay"); };
        if (relay_server.start(bind, Lobby::PORT))
            printf("%s: NET TAISEN lobby and relay on %s:%u\n", me, bind.c_str(), Lobby::PORT);
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
        const int fd = net::accept(ls, &peer);
        if (fd < 0) {   // (no descriptors left, say: not a loop that spins)
            sys::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        const uint32_t ip = ntohl(peer.sin_addr.s_addr);
        if (connections >= kConnections || !from_allowed(ip, "e-AMUSE3")) {
            net::close(fd);
            continue;
        }
        connections++;
        auto serve_one = [&, fd, ip] {
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(kSeconds);
            net::set_timeouts(fd, 3000);
            // The connection's first four bytes say which it is: a cabinet with a key ("HKK1",
            // shell/keyed.h) or a call in clear. A server with a key reads nothing of the second
            // kind, and nothing of the first until a record of it has held.
            std::string req;
            char first[4];
            bool come = true;
            for (size_t got = 0; come && got < sizeof first;) {
                const ssize_t n = net::recv(fd, first + got, sizeof first - got);
                if (n <= 0 || std::chrono::steady_clock::now() > until) come = false;
                else got += (size_t)n;
            }
            net::Keyed keyed;
            const bool with_key = come && net::keyed::is_hello(first);
            if (!come) {
            } else if (with_key && !key.set()) {
                net::send(fd, net::keyed::kNoKey, 4);
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
            auto recv = [&](char *p, size_t n) { return with_key ? keyed.recv(p, n) : net::recv(fd, p, n); };
            if (come && read_request(recv, req, until)) {
                std::string answer;
                {
                    std::lock_guard<std::mutex> one(answering);
                    asking = dotted(ip);
                    answer = server.handle(req);
                }
                if (with_key) {
                    keyed.send(answer.data(), answer.size());
                } else {
                    for (size_t sent = 0; sent < answer.size() && std::chrono::steady_clock::now() < until;) {
                        const ssize_t n = net::send(fd, answer.data() + sent, answer.size() - sent);
                        if (n <= 0) break;
                        sent += (size_t)n;
                    }
                }
            } else if (come && with_key && keyed.wrong()) {
                turned_away(ip, "e-AMUSE3", "with another key");
            }
            net::close(fd);
            connections--;
        };
        try {
            std::thread(serve_one).detach();
        } catch (const std::exception &) {   // no thread to be had: this connection on the main one
            serve_one();
        }
    }
}

}  // namespace hk::ea
