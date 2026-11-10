// SPDX-License-Identifier: GPL-3.0-or-later
// GuitarFreaks and DrumMania V's card units on Konami Python 2 (gfdmcard.h).
#include "python/gfdmcard.h"
#include "shell/sys.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>

namespace hk::py {

namespace {

constexpr uint8_t HEADER = 0xaa;
constexpr uint8_t BUS = 0xaa;    // a command for the bus itself
constexpr uint8_t NODE = 0x00;   // a command for a reader; its answer says 0x01

// a packet's length byte (windyfairy's reading): 0, or 2^(n-1), +1 with bit 4
size_t length_of(uint8_t l) { return l ? (size_t(1) << ((l & 0x0f) - 1)) + ((l & 0xf0) ? 1 : 0) : 0; }

uint8_t length_byte(size_t size) {
    int bit = 0;
    while ((size_t(1) << bit) < (size & ~size_t(0x0f))) bit++;
    return uint8_t((bit + 1) | ((size - (size_t(1) << bit)) ? 0x10 : 0));
}

// the 10-key's keys as PS/2 set 2 scan codes, in the order of keys_' bits (the 573 unit's)
constexpr uint8_t KEYS[] = {0x69, 0x72, 0x7a, 0x6b, 0x73, 0x74, 0x6c, 0x75, 0x7d, 0x70, 0x71, 0x66, 0x5a};

}  // namespace

GfdmCardUnit::GfdmCardUnit(std::string file) : file_(std::move(file)) {
    static const uint8_t blank[] = {0x08, 0x1f, 0x7d, 0xf0, 0x56};   // windyfairy's new card
    memcpy(data_, blank, sizeof blank);
    if (FILE *f = file_.empty() ? nullptr : fopen(file_.c_str(), "rb")) {
        if (fread(data_, 1, sizeof data_, f) != sizeof data_) {
            memset(data_, 0, sizeof data_);
            memcpy(data_, blank, sizeof blank);
        }
        fclose(f);
    }
}

void GfdmCardUnit::save() const {
    if (file_.empty()) return;
    if (!sys::write_file(file_, data_, sizeof data_)) fprintf(stderr, "card: cannot write %s: the one from before is kept\n", file_.c_str());
}

bool GfdmCardUnit::write(const uint8_t *data, uint32_t size, std::vector<uint8_t> &reply) {
    in_.insert(in_.end(), data, data + size);
    packet(reply);
    return true;
}

void GfdmCardUnit::packet(std::vector<uint8_t> &out) {
    for (;;) {
        while (!in_.empty() && in_.front() != HEADER) in_.pop_front();
        if (in_.size() < 4) return;
        if (in_[1] == 0xaa && in_[2] == 0xaa && in_[3] == 0x55) {   // sync: echoed
            in_.erase(in_.begin(), in_.begin() + 4);
            out.insert(out.end(), {0xaa, 0xaa, 0xaa, 0x55});
            sync_ = 1;
            continue;
        }
        if (sync_ == 1 && in_[1] == 0xaa && in_[2] == 0x00 && in_[3] == 0x00) {   // the second sync
            in_.erase(in_.begin(), in_.begin() + 4);
            out.insert(out.end(), {0xaa, 0xaa, 0x00, 0x00});
            sync_ = 2;
            continue;
        }
        if (sync_ != 2) { in_.pop_front(); continue; }
        if (in_.size() < 6) return;
        const uint8_t cmd = in_[1], node = in_[2], sub = in_[3];
        const size_t len = length_of(in_[4]);
        if (in_.size() < len + 6) return;
        uint8_t sum = 0;
        for (size_t i = 1; i < len + 5; i++) sum += in_[i];
        const std::vector<uint8_t> raw(in_.begin(), in_.begin() + ptrdiff_t(len + 6));   // the whole packet
        in_.erase(in_.begin(), in_.begin() + ptrdiff_t(len + 6));
        if (sum != raw[len + 5]) continue;
        const std::vector<uint8_t> data(raw.begin() + 5, raw.begin() + 5 + ptrdiff_t(len));

        // an answer: aa, the command's answer code, the node, the sub command, the length, the
        // payload and the sum of every byte after the aa
        auto answer = [&](uint8_t code, uint8_t sub_, const std::vector<uint8_t> &payload) {
            const size_t from = out.size() + 1;
            out.insert(out.end(), {HEADER, code, node, sub_, length_byte(payload.size())});
            out.insert(out.end(), payload.begin(), payload.end());
            uint8_t s = 0;
            for (size_t i = from; i < out.size(); i++) s += out[i];
            out.push_back(s);
        };
        if (cmd == BUS) {
            switch (sub) {
            case 0x01:   // the node count: the packet goes round the chain, each node adding one
                answer(BUS, 0x01, {uint8_t(data.empty() ? 1 : data[0] + 1)});
                break;
            case 0x02:   // a node's version (type 3, 1.6, "ICCA"), after the packet heard back
                out.insert(out.end(), raw.begin(), raw.end());
                if (node == 1) answer(BUS, 0x02, {0, 0, 0, 3, 0, 1, 6, 0, 'I', 'C', 'C', 'A', 0, 0, 0, 0});
                else answer(BUS, 0x02, std::vector<uint8_t>(16, 0));
                break;
            case 0x03:   // start: status 0
                out.insert(out.end(), raw.begin(), raw.end());
                answer(BUS, 0x03, {0});
                break;
            default: break;
            }
        } else if (cmd == NODE && node == 1) {
            std::vector<uint8_t> payload;
            switch (sub) {
            case 0x12:   // the card's status: in (2), front and back sensors
                payload = {uint8_t(inserted_ ? 2 | 64 | 128 : 0)};
                break;
            case 0x14: case 0x15:   // the slot: close, open, eject, format, read, write
                if (!data.empty() && data[0] == 2) inserted_ = false;   // given out
                payload = {0};
                break;
            case 0x16:   // write
                memcpy(data_, data.data(), std::min(data.size(), sizeof data_));
                save();
                payload = {0};
                break;
            case 0x18:   // read
                if (!inserted_) payload = {0xff};
                else { payload = {0}; payload.insert(payload.end(), data_, data_ + sizeof data_); }
                break;
            case 0x26: {   // the 10-key's new presses, as scan codes (0: none)
                for (size_t i = 0; i < std::size(KEYS); i++)
                    if ((keys_ & (1u << i)) && !(last_keys_ & (1u << i))) payload.push_back(KEYS[i]);
                if (payload.empty()) payload = {0};
                last_keys_ = keys_;
                break;
            }
            default: payload = {0}; break;   // init (00, 10, 11, 20), 10-key status (24), format (1e, 1f), ...
            }
            out.insert(out.end(), raw.begin(), raw.end());   // the packet echoed, then the answer
            answer(0x01, sub, payload);
        }
    }
}

void GfdmCardUnit::card_button() { inserted_ = !inserted_; }

void GfdmCardUnit::key(uint8_t scan, bool down) {
    for (size_t i = 0; i < std::size(KEYS); i++)
        if (KEYS[i] == scan) {
            if (down) keys_ |= 1u << i;
            else keys_ &= ~(1u << i);
        }
}

}  // namespace hk::py
