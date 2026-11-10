// SPDX-License-Identifier: GPL-3.0-or-later
// A local e-AMUSE3 server (protocol.h): what Konami's servers answered in 2003, from each game's
// own requests and response parsers. The modules several games share are here (numbering,
// userid, posevent, pcbtracker, the owner's message); each game adds its own through `Game`
// (the GuitarFreaks and DrumMania ones: runtime/sys573/eagfdm.h). From the Python core's
// server (python-groundwork branch, runtime/python/easerver).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "eamuse/protocol.h"

namespace hk::ea {

class Server {
public:
    // A game's own answer to one request: the module element, or "" to leave it to the
    // common modules (and, failing those, <MODULE status="0"/>).
    using Game = std::function<std::string(Server &, const Call &, const Request &, const std::string &host)>;

    // dir: where the server keeps what it must remember (cards, player data), or "" for nothing
    Server(std::string dir, Game game) : store_(std::move(dir)), game_(std::move(game)) {}

    // One HTTP request (head and body, as sent) to the whole HTTP response.
    std::string handle(const std::string &request);

    // Whether the cabinet a call says it is from (its srcid, the PCBID) is answered: a server on a
    // network takes the ones it was told (eamuse/access.h). Unset: every one, as a core's own server
    std::function<bool(const std::string &srcid)> admit;
    // Each call in clear (request and answer), for a core's network log.
    std::function<void(const std::string &)> log;
    // The cabinet's wall clock, as seconds since 1970 read as local time: in a core, the emulated
    // clock (reproducible runs); in a standalone server, the host's clock in Japan time.
    std::function<int64_t()> now;

    const Store &store() const { return store_; }
    // services.get: each name with this server's URL
    static std::string services(const std::string &host, const std::vector<const char *> &names, const char *module = "services");
    // The owner's message for the cabinets' title screens (the file "message"), at most 255
    // bytes, one line; "" if none.
    std::string message() const;

private:
    std::string common(const Call &c, const Request &r);

    Store store_;
    Game game_;
    uint32_t serial_ = 0;   // for the answers' X-Eamuse-Info: deterministic
};

}  // namespace hk::ea
