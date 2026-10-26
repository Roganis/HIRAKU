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
//   hiraku-eamuse [--port 8080] [--bind 0.0.0.0] [--data DIR] [--clock "2003-06-01 12:00:00"] [--tz 9] [--quiet] [--no-relay]
//     --data   where it keeps what it must remember (default ./eamuse-data)
//     --clock  the cabinets' time (ntp.get), running from this start (default: the host's clock)
//     --tz     with the host's clock: the cabinets' time zone, hours from UTC (default 9, Japan: the
//              games set their clocks to what ntp.get says and stamp their own times "+9")
//     --quiet  don't print each call
//     --no-relay  no NET TAISEN relay
#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

#include "eamuse/server.h"
#include "python/easerver.h"
#include "python/lobby.h"
#include "python/netrelay.h"
#include "shell/net.h"
#include "shell/sys.h"
#include "sys573/eagfdm.h"

namespace {

// One HTTP/1.0 request from a connection: the head and Content-Length bytes of body
bool read_request(int fd, std::string &req) {
    char buf[4096];
    for (;;) {
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
        const ssize_t n = hk::net::recv(fd, buf, sizeof buf);
        if (n <= 0) return false;
        req.append(buf, (size_t)n);
    }
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
    bool quiet = false, relay_on = true;
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
        else if (a == "--quiet") quiet = true;
        else if (a == "--no-relay") relay_on = false;
        else {
            fprintf(stderr, "usage: hiraku-eamuse [--port N] [--bind ADDR] [--data DIR] [--clock \"YYYY-MM-DD HH:MM:SS\"] [--tz H] [--quiet] [--no-relay]\n");
            return a == "--help" || a == "-h" ? 0 : 2;
        }
    }
    hk::sys::make_dir(data);
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
    hk::py::RelayServer relay;
    if (relay_on) {
        relay.log = server.log;
        if (relay.start(bind_addr, hk::py::Lobby::PORT))
            printf("hiraku-eamuse: NET TAISEN lobby and relay on %s:%u\n", bind_addr.c_str(), hk::py::Lobby::PORT);
    }
    fflush(stdout);
    for (;;) {   // one connection at a time: the game makes one request per connection (HTTP/1.0)
        const int fd = hk::net::accept(ls);
        if (fd < 0) continue;
        hk::net::set_timeouts(fd, 10000);
        std::string req;
        if (read_request(fd, req)) {
            const std::string answer = gfdm(req) ? server.handle(req) : popn.handle(req);
            for (size_t sent = 0; sent < answer.size();) {
                const ssize_t n = hk::net::send(fd, answer.data() + sent, answer.size() - sent);
                if (n <= 0) break;
                sent += (size_t)n;
            }
        }
        hk::net::close(fd);
    }
}
