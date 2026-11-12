// SPDX-License-Identifier: GPL-3.0-or-later
// e-AMUSEMENT online: the game's HTTP requests forwarded to a real server (protocol.h).
// The cabinet's own server ("services", which the game looks up first) is the URL given with
// --eamuse; the URLs that server hands out (services.get) are reached as they say. Requests
// and answers go through unchanged (encrypted as the game and the server encrypt them),
// except the request line's path (the URL's path in front) and the Host header. A server that has a
// key (hiraku-eamuse's, shell/keyed.h) is given the same one here: each connection then goes behind it.
#pragma once

#include <cstdint>
#include <string>

#include "eamuse/lan.h"
#include "shell/keyed.h"

namespace hk::ea {

class Remote {
public:
    // url: http://host[:port][/path]. ok() says whether it could be read. key: the server's, if it has one
    explicit Remote(const std::string &url, const net::Key &key = {});
    const net::Key &key() const { return key_; }
    bool ok() const { return !host_.empty(); }
    const std::string &host() const { return host_; }
    uint16_t port() const { return port_; }

    // One request to the server it belongs to, and its whole answer (HTTP/1.0: up to the
    // close), or "" with *error set. Blocking: run it off the emulator's thread.
    std::string exchange(const Lan::Target &t, const std::string &request, std::string *error) const;
    // Whether the cabinet's own server takes a connection within 3 s, and has this side's key if
    // there is one (-smartea, and what is said at the start); *error says why not.
    bool reachable(std::string *error) const;

private:
    static int connect_to(const std::string &host, uint16_t port, int seconds, std::string *error);
    std::string host_, path_;   // path_: "" or "/x" (no trailing slash)
    uint16_t port_ = 80;
    net::Key key_;
};

}  // namespace hk::ea
