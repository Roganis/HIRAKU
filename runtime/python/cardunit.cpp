// SPDX-License-Identifier: GPL-3.0-or-later
// The e-AMUSEMENT card reader/writer and 10-key of a Python 1 pop'n cabinet (cardunit.h).
#include "python/cardunit.h"
#include "shell/sys.h"

#include <cstdio>
#include <fstream>
#include <iterator>

namespace hk::py {

namespace {

constexpr size_t CARD_BYTES = 128;

// The card header's CRC-8 (0x1a3050): reflected 0x31 (0x8c), initial 0xff, inverted.
uint8_t crc8(const uint8_t *p, size_t n) {
    uint8_t c = 0xff;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int b = 0; b < 8; b++) c = (c & 1) ? (c >> 1) ^ 0x8c : c >> 1;
    }
    return ~c;
}

// The card data's CRC-16 (0x1a30c0): reflected 0x1021 (0x8408), initial 0xffff, inverted
uint16_t crc16(const uint8_t *p, size_t n) {
    uint16_t c = 0xffff;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int b = 0; b < 8; b++) c = (c & 1) ? (c >> 1) ^ 0x8408 : c >> 1;
    }
    return (uint16_t)~c;
}

uint8_t sum8(const uint8_t *p, size_t n) {
    uint8_t s = 0;
    for (size_t i = 0; i < n; i++) s += p[i];
    return s;
}

}  // namespace

CardUnit::CardUnit(std::string card_file, Header fresh) : file_(std::move(card_file)), fresh_(fresh) { load(); }

CardUnit::Header CardUnit::fresh_header(const std::string &game) {
    if (game == "popn10") return {0x28, 0x23, 0x49, 0x01};
    if (game == "popn11") return {0x28, 0x24, 0x45, 0x11};
    if (game == "popn12") return {0x28, 0x25, 0x45, 0x21};
    if (game == "popn13") return {0x28, 0x25, 0x45, 0x31};
    return {0x28, 0x23, 0x41, 0x01};   // pop'n 9
}

std::vector<uint8_t> CardUnit::new_card(const Header &h) {
    std::vector<uint8_t> c(CARD_BYTES, 0);
    for (int i = 0; i < 4; i++) c[i] = h[i];
    c[4] = crc8(c.data(), 4);
    // the data (5-0x3e, blank) and its CRC at 0x3f: pop'n 12 reads a card only with both right
    // (0x1acbb0); pop'n 9-11 never look at a new card's
    const uint16_t d = crc16(c.data() + 5, 0x3a);
    c[0x3f] = (uint8_t)d;
    c[0x40] = (uint8_t)(d >> 8);
    return c;
}

void CardUnit::load() {
    std::ifstream f(file_, std::ios::binary);
    card_.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    if (card_.size() != CARD_BYTES) {
        card_ = new_card(fresh_);
        save();
    }
}

void CardUnit::present_older(std::vector<uint8_t> older) {
    if (older.size() != CARD_BYTES) return;
    older_ = older;
    card_ = std::move(older);
    // no card file of this game's until it writes one (or gives the older card back): a run that
    // ends before the card is used shows the older one again next time
    if (!file_.empty()) std::remove(file_.c_str());
}

// The card is back with the player: an older game's, given back unchanged, is followed by a fresh
// card of this game (the same number: the server numbers it as before)
void CardUnit::given_back() {
    if (older_.empty() || card_ != older_) return;
    const bool agreed = older_asked_ && older_keyed_;
    older_asked_ = older_keyed_ = false;
    if (!agreed) return;
    older_.clear();
    card_ = new_card(fresh_);
    save();
    if (note) note("the older game's card given back: a fresh card of this game in its place");
}

void CardUnit::save() {
    older_.clear();   // written: this game's own now
    if (file_.empty()) return;
    const std::string tmp = file_ + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char *>(card_.data()), (std::streamsize)card_.size());
        if (!f) { fprintf(stderr, "card: cannot write %s\n", tmp.c_str()); return; }
    }
    sys::replace_file(tmp, file_);
}

uint8_t CardUnit::status() const {
    return (inside_ ? 0x80 : 0) | (read_ready_ ? 0x20 : 0) | (buffer_ready_ ? 0x10 : 0);
}

void CardUnit::answer(uint8_t type, uint8_t cmd, const std::vector<uint8_t> &payload, uint8_t len,
                      std::vector<uint8_t> &reply) const {
    const size_t at = reply.size();
    reply.insert(reply.end(), {0xaa, type, addr_, cmd, len});
    reply.insert(reply.end(), payload.begin(), payload.end());
    reply.push_back(sum8(reply.data() + at + 1, reply.size() - at - 1));
}

bool CardUnit::write(const uint8_t *d, uint32_t n, std::vector<uint8_t> &reply) {
    if (n == 4 && d[0] == 0xaa && d[1] == 0xaa && d[2] == 0xaa && d[3] == 0x55) {   // sync
        reply.insert(reply.end(), d, d + n);
        return true;
    }
    if (n < 6 || d[0] != 0xaa || (d[1] != 0xaa && d[1] != 0x00)) return false;
    reply.insert(reply.end(), d, d + n);   // the echo
    if (sum8(d + 1, n - 2) != d[n - 1]) return true;   // a damaged packet: no answer, the game retries
    const uint8_t cmd = d[3];
    const uint8_t *arg = d + 5;
    const size_t args = n - 6;

    if (d[1] == 0xaa) {   // the bus
        switch (cmd) {
        case 0x01:   // assign addresses: one node, ours
            reply.insert(reply.end(), {0xaa, 0xaa, 0x00, 0x01, 0x01, addr_});
            reply.push_back(sum8(reply.data() + reply.size() - 5, 5));
            break;
        case 0x02:   // revision: device id, flag, version, name (as Konami's ACIO nodes give it)
            answer(0xa5, cmd, {0x03, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 'H', 'K', 'C', 'A', 'R', 'D', ' ', ' '}, 0x05, reply);
            break;
        case 0x03:   // start its program
            answer(0xa5, cmd, {0x00}, 0x01, reply);
            break;
        default:
            break;
        }
        return true;
    }

    switch (cmd) {   // the card unit
    case 0x00: case 0x01:
    case 0x10: case 0x11:   // reset: a card inside comes back out (pop'n 10 returns a refused card so)
        read_ready_ = buffer_ready_ = false;
        if (inside_) { inside_ = false; offered_ = false; accepting_ = false; given_back(); }
        answer(0x01, cmd, {0x00}, 0x01, reply);
        break;
    case 0x12:   // status
        answer(0x01, cmd, {status()}, 0x01, reply);
        break;
    case 0x14: case 0x15:   // control
        if (cmd == 0x14 && args >= 1) {
            read_ready_ = false;
            switch (arg[0]) {
            case 1:   // open the slot: takes the card the player holds there
                accepting_ = opened_ = true;
                if (offered_) { inside_ = true; accepting_ = false; }
                break;
            case 2:   // close / eject: the card comes back out
                accepting_ = false;
                if (inside_) { inside_ = false; offered_ = false; given_back(); }
                break;
            case 4:   // read the card into the buffer
                read_ready_ = inside_;
                break;
            case 5:   // write the buffer to the card
                if (inside_ && buffer_ready_) {
                    card_ = buffer_;
                    save();
                }
                buffer_ready_ = false;
                break;
            default:
                break;
            }
        }
        answer(0x01, cmd, {0x00}, 0x01, reply);
        break;
    case 0x16:   // write buffer: 128 bytes
        if (args >= CARD_BYTES) {
            buffer_.assign(arg, arg + CARD_BYTES);
            buffer_ready_ = true;
        }
        answer(0x01, cmd, {0x00}, 0x01, reply);
        break;
    case 0x18: {   // read buffer: a byte, then the card's 128
        std::vector<uint8_t> p(1, 0x00);
        p.insert(p.end(), card_.begin(), card_.end());
        answer(0x01, cmd, p, 0x18, reply);
        break;
    }
    case 0x1e: case 0x1f:   // format: the card blank
        if (inside_) {
            card_.assign(CARD_BYTES, 0);
            save();
        }
        answer(0x01, cmd, {0x00}, 0x01, reply);
        break;
    case 0x20:   // 10-key: init (1 = done)
        answer(0x01, cmd, {0x01}, 0x01, reply);
        break;
    case 0x24:   // 10-key: status (0 = fine)
        answer(0x01, cmd, {0x00}, 0x01, reply);
        break;
    case 0x26: {   // 10-key: events. LEN n > 0 = 2^(n-1) bytes; one key event per poll
        std::vector<uint8_t> ev;
        if (!keys_.empty()) {
            const size_t k = keys_.front() == 0xf0 && keys_.size() >= 2 ? 2 : 1;
            ev.assign(keys_.begin(), keys_.begin() + (long)k);
            keys_.erase(keys_.begin(), keys_.begin() + (long)k);
        }
        answer(0x01, cmd, ev, ev.empty() ? 0 : ev.size() == 1 ? 1 : 2, reply);
        break;
    }
    default:
        answer(0x01, cmd, {0x00}, 0x01, reply);
        break;
    }
    return true;
}

void CardUnit::card_button() {
    if (inside_) return;   // the reader holds it until the game gives it back
    offered_ = !offered_;
    if (offered_ && accepting_) { inside_ = true; accepting_ = false; }
}

void CardUnit::key(uint8_t scan, bool down) {
    if (inside_ && older_asked_) older_keyed_ = true;
    if (!down) keys_.push_back(0xf0);
    keys_.push_back(scan);
}

std::string CardUnit::state() const {
    if (inside_) return "card in the reader";
    if (offered_) return accepting_ ? "card in the reader" : "card at the slot";
    return "";
}

}  // namespace hk::py
