// SPDX-License-Identifier: GPL-3.0-or-later
// NET TAISEN online (pop'n 12-14; docs/EAMUSE3.md): cabinets on different PCs meet through
// hiraku-eamuse, which keeps the lobby and passes the datagrams the games send each other.
//
// Each cabinet (the Python core with -url, the server being hiraku-eamuse) holds a TCP
// connection to the server's relay, at Lobby::PORT on the server's host: the port the services
// list's globby URL names, so the game's lobby and the relay are one address. The relay gives
// each cabinet an address of its own, 10.57.x.y, as the world would see a shop's router; the
// cabinet's portfw get answers it (the core puts it in), so the games tell each other to send
// there. A datagram from a game goes to the relay with the address and port the game sent it
// to: to the lobby's port at an address that isn't a cabinet's, the relay's lobby answers it;
// to a cabinet's address, it goes to that cabinet, its sender the sending cabinet's address.
// Broadcasts (a shop's LAN) go nowhere.
//
// Nothing here is in lockstep: datagrams arrive when the network brings them, and each cabinet
// takes them at its next frame, as real cabinets took theirs. (The LAN cable, lancable.h, is
// the reproducible way, for cabinets on one PC or a LAN.)
//
// Over the connection: a hello each way, then datagrams, each a header (sender, receiver, their
// ports, the size) and its bytes.
#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "python/lancable.h"

namespace hk::py {

// The cabinet's side
class RelayClient {
public:
    ~RelayClient();
    // Connect to the relay at host:port within `seconds`; false: *error says why
    bool open(const std::string &host, uint16_t port, int seconds, std::string *error);
    bool is_open() const { return fd_ >= 0; }
    // the address the relay gave this cabinet (192.168.50.2 = 0xc0a83202 style)
    uint32_t address() const { return address_; }
    // A frame's end: ours out, whatever came since in (never blocks on the network for long)
    void exchange(std::vector<LanCable::Packet> &out, std::vector<LanCable::Packet> &in);

private:
    void reader();
    int fd_ = -1;
    uint32_t address_ = 0;
    std::thread thread_;
    std::mutex mutex_;
    std::vector<LanCable::Packet> came_;
    bool lost_ = false;
};

// The server's side (hiraku-eamuse): accepts cabinets and passes datagrams, on a thread of its own
class RelayServer {
public:
    // listen at bind_addr:port (0: any free one); false: a message was printed
    bool start(const std::string &bind_addr, uint16_t port);
    uint16_t port() const { return port_; }   // where it listens
    std::function<void(const std::string &)> log;
    // Whether a connection from this address (host byte order) is taken (eamuse/access.h); unset: any.
    // Set before start()
    std::function<bool(uint32_t ip)> admit;

private:
    void run();
    int listen_fd_ = -1;
    uint16_t port_ = 0;
    std::thread thread_;
};

}  // namespace hk::py
