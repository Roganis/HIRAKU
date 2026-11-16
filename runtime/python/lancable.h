// SPDX-License-Identifier: GPL-3.0-or-later
// The shop's LAN between Python cabinets in separate HIRAKU processes, for NET TAISEN (pop'n
// 12-14; docs/EAMUSE3.md). Each process is one cabinet with its own address on 192.168.50.0/24
// (the host .2, the guests .3 on); the cable carries the UDP datagrams their games send to each
// other, the lobby's (the host keeps the lobby) and broadcasts.
//
// The cabinets run in lockstep, a frame at a time: at each frame's end every side hands over the
// datagrams its game sent during the frame and takes everyone else's, which its game finds at
// the start of the next. So a battle replays the same on any host, however its processes are
// scheduled, and either side may run faster than real time; a datagram takes one frame (16.7 ms),
// about what a shop's LAN plus the cabinets' polling took.
//
// It is a star: the host listens at a Unix socket (a path) or TCP (host:port) and waits for all
// its guests; each guest connects (trying for up to ten minutes). Each frame the guests send
// their datagrams to the host, which sends everyone all of them. If a side quits, the cable goes
// idle for the others.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "eamuse/packet.h"

namespace hk::py {

class LanCable {
public:
    // A datagram on the LAN: addresses as 32-bit numbers (192.168.50.2 = 0xc0a83202)
    using Packet = ea::Packet;

    ~LanCable();
    // host: listen at `where` and wait for cabinets - 1 guests; else connect to it. false: a
    // message was printed.
    bool open(const std::string &where, bool host, int cabinets);
    bool is_open() const { return !fds_.empty(); }
    // this cabinet's number: 0 the host, 1 on the guests (its address: 192.168.50.(2 + index))
    int index() const { return index_; }

    // A frame's end: ours out, the others' in (in the order of the cabinets, each in the order
    // sent; never our own)
    void exchange(std::vector<Packet> &out, std::vector<Packet> &in);

private:
    bool send_batch(int fd, const std::vector<Packet> &p);
    bool recv_batch(int fd, std::vector<Packet> &p);
    void lost(const char *why);

    std::vector<int> fds_;   // the host: one per guest; a guest: the host
    bool host_ = false;
    int index_ = 0;
    uint64_t step_ = 0;
};

}  // namespace hk::py
