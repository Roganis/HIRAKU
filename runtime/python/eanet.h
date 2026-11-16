// SPDX-License-Identifier: GPL-3.0-or-later
// The network a Python 1 cabinet's e-AMUSEMENT sees, inside HIRAKU: no host sockets of its own.
// The interface board has the network port, and the game (through the IOP) sends it BSD-style
// socket commands on channels 0-7 (PLAN.md section 23, "e-AMUSEMENT"). This answers them as
// a small LAN would:
//   - the board's addresses: 192.168.50.2/24, gateway and DNS 192.168.50.1;
//   - DNS (UDP 53): "services..." is 192.168.50.1; any other name gets an address of its own
//     on the LAN (192.168.50.10 on), remembered, so a connection to it names its host again;
//   - UDP to 192.168.50.1 at Lobby::PORT: NET TAISEN's lobby (pop'n 12-14, lobby.h), answered at
//     once; datagrams to other cabinets and broadcasts go on the LAN cable to cabinets in other
//     processes (lancable.h: then this cabinet is 192.168.50.2 + its number, and only the
//     first cabinet keeps the lobby), else nowhere, as on a LAN with no other cabinet. A recvfrom
//     waits for its socket's timeout (in whole frames), as the board's;
//   - TCP: each HTTP request the game sends goes to a handler, with the host and port it
//     connected to (docs/EAMUSE3.md). The handler answers at once (the server in the core:
//     reproducible) or later (a real server on the network); a recv waits for the answer, as
//     the board's does.
// Board commands (the game's RPC number + 1): 0b socket, 0c bind, 0d listen, 0e accept,
// 0f connect, 13 setsockopt, 14 send, 15 sendto, 16 recv, 17 recvfrom, 1c shutdown,
// 1d close, 1e a board property. The rest (start-up, DHCP, the machine id) stay the
// emulator's.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "eamuse/lan.h"
#include "python/lancable.h"
#include "eamuse/lobby.h"

namespace hk::py {

using ea::Lobby;   // NET TAISEN's lobby (eamuse/lobby.h)

class VirtualNet {
public:
    // How it reaches the emulator: data into IOP memory, the channel's status block, and
    // reading what the game sends.
    struct Io {
        std::function<void(uint32_t addr, const uint8_t *data, uint32_t size)> write;
        std::function<void(uint32_t channel, const uint32_t words[8])> respond;
        std::function<bool(uint32_t addr, uint8_t *data, uint32_t size)> read;
    };
    // Where a connection goes (eamuse/lan.h)
    using Target = ea::Lan::Target;
    // One HTTP request in (bytes as sent); call `done` with the whole response, now or later
    // and from any thread ("" = the connection failed).
    using Http = std::function<void(const Target &, const std::string &request, std::function<void(std::string)> done)>;

    VirtualNet(Io io, Http http) : io_(std::move(io)), http_(std::move(http)) {}

    // A board socket command; false = not ours.
    bool command(uint32_t channel, const uint32_t *p, uint32_t quads);
    // Call once a frame (the emulator's thread): answers that arrived complete waiting recvs.
    void poll();

    // On a LAN with cabinets in other processes: this one's number (0: 192.168.50.2, the lobby's
    // keeper), and the cable, called at each frame's end with the datagrams for the others, to
    // fill with theirs (LanCable::exchange)
    void set_cabinet(int index);
    // Online (netrelay.h): the address the relay gave this cabinet, which other cabinets send to;
    // the lobby is the server's then, and every datagram for elsewhere goes on `cable`
    void set_online(uint32_t address);
    std::function<void(std::vector<LanCable::Packet> &out, std::vector<LanCable::Packet> &in)> cable;

    // Each command and its answer, for HK_PY_NET_LOG.
    std::function<void(const std::string &)> log;

private:
    struct Datagram {
        std::vector<uint8_t> data;
        uint32_t from = 0;      // the sender's address, as the board's status block carries it
        uint16_t port = 0;      // and port
    };
    struct Socket {
        uint64_t id = 0;           // unique, unlike the descriptor
        int type = 0;              // 1 stream, 2 datagram, 3 raw (ICMP)
        Target target;
        bool connected = false;
        std::string request;       // stream: bytes the game sent, not complete yet
        std::string pending;       // stream: answer not read yet
        bool in_flight = false;    // a request is with the handler
        bool answered = false;     // the handler has answered (and will close: HTTP/1.0)
        bool waiting = false;      // a recv is parked: channel, buffer, length
        uint32_t wait_ch = 0, wait_buf = 0, wait_len = 0;
        uint32_t wait_from = 0;            // datagram: where recvfrom wants the sender's address
        uint64_t wait_until = 0;           // and the frame its timeout runs out
        uint64_t timeout_frames = 0;       // its receive timeout (setsockopt 0x200), in whole frames
        std::vector<Datagram> datagrams;   // datagram: answers not read yet
        bool dns = false;                  // it asked DNS: an empty recvfrom gives 0, not a timeout
        uint16_t port = 0;                 // datagram: the port it is bound to (or was given)
    };
    void respond(uint32_t channel, int32_t ret, uint32_t w2 = 0, uint32_t w3 = 0);
    void http_progress(Socket &s);
    void take_answers();
    void from_lan(LanCable::Packet &p);
    void take_datagram(Socket &s, uint32_t ch, uint32_t buf, uint32_t len, uint32_t from_addr);
    void deliver(Socket &s, uint32_t ch, uint32_t buf, uint32_t len);
    std::vector<uint8_t> dns_answer(const uint8_t *q, size_t n, std::string *name);
    Target target_of(uint32_t ip_be, uint16_t port) const;

    Io io_;
    Http http_;
    std::map<int, Socket> sockets_;
    uint8_t address_[4] = {192, 168, 50, 2};   // this cabinet's
    bool lobby_here_ = true;
    uint32_t online_ = 0;                      // the relay's address for this cabinet
    std::vector<LanCable::Packet> to_lan_;   // datagrams for the cable, at the frame's end
    uint16_t next_port_ = 49152;             // for a socket that sends unbound
    Lobby lobby_;
    uint64_t polls_ = 0;   // frames: the lobby's clock
    uint64_t next_id_ = 1;
    std::map<std::string, uint32_t> name_ip_;   // DNS names given out, and back
    std::map<uint32_t, std::string> ip_name_;
    std::mutex done_mutex_;                       // answers from the handler, any thread
    std::vector<std::pair<uint64_t, std::string>> done_;
};

}  // namespace hk::py
