// SPDX-License-Identifier: GPL-3.0-or-later
// Konami's ACIO bus on a serial port, from the PC's side, as spice2x speaks it (reader/, acioemu/):
// the e-AMUSEMENT card readers (readers.h) and beatmania IIDX's BIO2 I/O board (bio2.h).
//
//   aa NODE CMD_HI CMD_LO ID LEN [payload] SUM   SUM: the 8-bit sum of NODE..payload. After the aa,
//                                                an aa or ff byte goes as ff and its inverse.
//
// Nodes are numbered from 1 (command 0001 to node 0); a node answers with a packet of its own (its
// node | 0x80, the command, the same ID). A run of zeros and aa wakes the bus, and a node answers aa.
// HK_ACIO_LOG=1 prints the packets.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hk::host {

class AcioPort {
public:
    struct Message {
        uint8_t node = 0, id = 0;
        uint16_t cmd = 0;
        std::vector<uint8_t> payload;
    };

    explicit AcioPort(std::string path);
    ~AcioPort();
    AcioPort(const AcioPort &) = delete;
    AcioPort &operator=(const AcioPort &) = delete;

    const std::string &path() const { return path_; }
    bool open();           // false with errno set
    void close();
    bool is_open() const { return fd_ >= 0; }

    // The handshake at each rate (baud) in turn, ten tries each, while keep_going(): the rate a
    // node answered at, or 0
    int handshake(const std::vector<int> &rates, const std::function<bool()> &keep_going);
    bool send(uint8_t node, uint16_t cmd, const std::vector<uint8_t> &payload);
    // The last whole packet with a good sum among what arrives: waiting up to wait_ms for it to
    // start, then until it pauses 30 ms (spice2x's port timeouts)
    bool receive(Message &m, int wait_ms = 500);
    bool ask(uint8_t node, uint16_t cmd, const std::vector<uint8_t> &payload, Message &m, int wait_ms = 500) {
        return send(node, cmd, payload) && receive(m, wait_ms);
    }

    // The BIO2's way (beatmania IIDX 27, docs/BIO2-BI2A.md): at 115200, a 250 ms line break, then
    // aa x 10 until ten aa come back (up to 100 rounds, while keep_going())
    bool handshake_bio2(const std::function<bool()> &keep_going);
    // The first whole packet with a good sum, as soon as it is in (up to wait_ms)
    bool receive_one(Message &m, int wait_ms);
    // send and receive_one, sent again up to `tries` times when no answer comes
    bool ask_one(uint8_t node, uint16_t cmd, const std::vector<uint8_t> &payload, Message &m, int wait_ms, int tries) {
        for (int k = 0; k < tries; k++)
            if (send(node, cmd, payload) && receive_one(m, wait_ms)) return true;
        return false;
    }
    void tag_zero(bool on) { tag_zero_ = on; }   // every packet's ID 0, as IIDX 27 sends them to the BIO2

    // The card readers' way (readers.cpp): the answer to `cmd`, the first packet with a good sum for
    // that command, as soon as it is in (up to wait_ms), not after the line has been quiet 30 ms as
    // receive() waits. What is read past it, and packets for other commands, are kept for the next
    // calls: an answer that comes late is taken by the next ask for its command, never lost (a
    // reader's statuses are encrypted one after another, and one lost puts the two ends out of step)
    bool receive_answer(uint16_t cmd, Message &m, int wait_ms);
    bool ask_answer(uint8_t node, uint16_t cmd, const std::vector<uint8_t> &payload, Message &m, int wait_ms) {
        return send(node, cmd, payload) && receive_answer(cmd, m, wait_ms);
    }

private:
    bool set_rate(int baud);
    bool write_all(const uint8_t *d, size_t n);
    std::vector<uint8_t> read_some(int first_ms, size_t most);
    std::vector<uint8_t> read_chunk(int wait_ms);   // what arrives first within wait_ms (none: empty)
    void line_break(int ms);
    void dump(const char *dir, const std::vector<uint8_t> &bytes) const;

    std::string path_;
    intptr_t fd_ = -1;   // a file descriptor; on Windows, the port's HANDLE
    uint8_t id_ = 0;
    bool tag_zero_ = false;
    std::vector<uint8_t> last_sent_;   // receive_one passes over an echo of it
    bool log_ = false;
    std::vector<uint8_t> in_;          // receive_answer: bytes read and not yet a whole packet
    std::vector<Message> aside_;       // ... and packets that answered another command
};

}  // namespace hk::host
