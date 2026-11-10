// SPDX-License-Identifier: GPL-3.0-or-later
// The e-AMUSEMENT GuitarFreaks and DrumMania's card units (cardunit.h).
#include "sys573/cardunit.h"
#include "shell/sys.h"

#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace hk::k573 {

namespace {

constexpr uint8_t HEADER = 0xaa;
constexpr uint8_t BUS = 0xaa;    // a command for the bus itself
constexpr uint8_t NODE = 0x00;   // a command for a node (a reader); its answer says 0x01
constexpr int BAUD = 38400;

// a packet's length byte (windyfairy's reading): 0, or 2^(n-1), +1 with bit 4
size_t length_of(uint8_t l) { return l ? (size_t(1) << ((l & 0x0f) - 1)) + ((l & 0xf0) ? 1 : 0) : 0; }

uint8_t length_byte(size_t size) {
    int bit = 0;
    while ((size_t(1) << bit) < (size & ~size_t(0x0f))) bit++;
    return uint8_t((bit + 1) | ((size - (size_t(1) << bit)) ? 0x10 : 0));
}

// the 10-key: its keys' PS/2 set 2 scan codes (as windyfairy's unit and the Python core's)
struct Key {
    const char *name;
    uint8_t code;
};
constexpr Key KEYS[] = {{"1", 0x69}, {"2", 0x72}, {"3", 0x7a}, {"4", 0x6b}, {"5", 0x73}, {"6", 0x74}, {"7", 0x6c},
                        {"8", 0x75}, {"9", 0x7d}, {"0", 0x70}, {".", 0x71}, {"Clear", 0x66}, {"Enter", 0x5a}};

}  // namespace

void CardUnit::configure(int readers, std::string dir) {
    readers_.assign(readers, Reader{});
    dir_ = std::move(dir);
    for (int i = 0; i < readers; i++) {
        Reader &r = readers_[i];
        r.file = dir_.empty() ? "" : dir_ + "/card" + std::to_string(i + 1) + ".bin";
        static const uint8_t blank[] = {0x08, 0x1f, 0x7d, 0xf0, 0x56};   // windyfairy's new card
        memcpy(r.data, blank, sizeof blank);
        if (FILE *f = r.file.empty() ? nullptr : fopen(r.file.c_str(), "rb")) {
            if (fread(r.data, 1, sizeof r.data, f) != sizeof r.data) memcpy(r.data, blank, sizeof blank);
            fclose(f);
        }
    }
}

void CardUnit::attach(running_machine &m, std::function<void(int)> rxd) {
    machine_ = &m;
    rxd_ = std::move(rxd);
    log_ = getenv("HK_573_CARD_LOG") != nullptr;
    send_timer_ = m.alloc_timer([this](s32 p) { send_tick(p); });
}

void CardUnit::reset() {
    rx_bits_ = -1;
    in_.clear();
    out_.clear();
    sync_ = 0;
    sending_ = false;
    selected_ = false;
    for (Reader &r : readers_) r.last_keys = 0;
    if (send_timer_) send_timer_->adjust(attotime::never);
}

void CardUnit::select(bool extio) {
    if (readers_.empty() || extio == selected_) return;   // no card units: SIO1 stays as it is wired
    if (log_ && machine_) fprintf(stderr, "card: %.6f switched %s\n", machine_->time().as_double(), extio ? "in" : "out");
    selected_ = extio;
    if (on_select) on_select(extio);
}

bool CardUnit::card_read(int player, const std::string &number) {
    if (player < 0 || player >= (int)readers_.size()) return false;
    Reader &r = readers_[player];
    if (r.inserted) {
        if (r.number != number) printf("card: reader %d holds a card: take it back first\n", player + 1);
        return true;
    }
    if (r.number != number) {
        save();
        r.number = number;
        r.file = dir_.empty() ? "" : dir_ + "/cards/" + number + ".bin";
        if (!dir_.empty()) sys::make_dir(dir_ + "/cards");
        static const uint8_t blank[] = {0x08, 0x1f, 0x7d, 0xf0, 0x56};   // as configure()'s
        memset(r.data, 0, sizeof r.data);
        memcpy(r.data, blank, sizeof blank);
        if (FILE *f = r.file.empty() ? nullptr : fopen(r.file.c_str(), "rb")) {
            if (fread(r.data, 1, sizeof r.data, f) != sizeof r.data) memcpy(r.data, blank, sizeof blank);
            fclose(f);
        }
    }
    r.inserted = true;
    return true;
}

void CardUnit::save() {
    for (const Reader &r : readers_)
        if (!r.file.empty() && !sys::write_file(r.file, r.data, sizeof r.data))
            fprintf(stderr, "card: cannot write %s: the one from before is kept\n", r.file.c_str());
}

bool CardUnit::set_input(const std::string &name, int value) {
    if (readers_.empty()) return false;
    size_t n = 0;
    std::string key = name;
    if (key.compare(0, 3, "P2 ") == 0) {
        if (readers_.size() < 2) return false;
        n = 1;
        key = key.substr(3);
    }
    Reader &r = readers_[n];
    if (key == "Card") {
        if (value) r.inserted = !r.inserted;   // a press puts it in, or takes it back
        return true;
    }
    if (key.compare(0, 7, "Keypad ") != 0) return false;
    for (size_t i = 0; i < std::size(KEYS); i++)
        if (key.compare(7, std::string::npos, KEYS[i].name) == 0) {
            if (value) r.keys |= 1u << i;
            else r.keys &= ~(1u << i);
            return true;
        }
    return false;
}

void CardUnit::log(const char *what, const std::deque<uint8_t> &b, size_t n) {
    if (!log_) return;
    fprintf(stderr, "card: %.6f %s", machine_->time().as_double(), what);
    for (size_t i = 0; i < n && i < b.size(); i++) fprintf(stderr, " %02x", b[i]);
    fprintf(stderr, "\n");
}

// SIO1's line, a bit a call: a start bit (0), eight data bits from the lowest, a stop bit (1).
// Its bit clock calls only while it sends, so a byte's bits come together.
void CardUnit::txd(int bit) {
    if (readers_.empty()) return;
    static const bool bits = getenv("HK_573_CARD_LOG") && atoi(getenv("HK_573_CARD_LOG")) >= 3;
    if (bits) fprintf(stderr, "card: %.6f bit %d%s\n", machine_->time().as_double(), bit, selected_ ? "" : " (switched out)");
    if (!selected_) return;
    if (rx_bits_ < 0) {
        if (bit == 0) { rx_bits_ = 0; rx_byte_ = 0; }
        return;
    }
    if (rx_bits_ < 8) {
        rx_byte_ |= uint8_t(bit ? 1 : 0) << rx_bits_;
        rx_bits_++;
        return;
    }
    rx_bits_ = -1;   // the stop bit
    static const bool raw = getenv("HK_573_CARD_LOG") && atoi(getenv("HK_573_CARD_LOG")) >= 2;
    if (raw) fprintf(stderr, "card: %.6f byte %02x%s\n", machine_->time().as_double(), rx_byte_, bit ? "" : " (no stop bit)");
    in_.push_back(rx_byte_);
    packet();
}

void CardUnit::packet() {
    while (!in_.empty() && in_.front() != HEADER) in_.pop_front();
    if (in_.size() < 4) return;
    const size_t before = out_.size();
    if (in_[1] == 0xaa && in_[2] == 0xaa && in_[3] == 0x55) {   // sync: echoed
        log("in sync", in_, 4);
        for (int i = 0; i < 4; i++) in_.pop_front();
        for (uint8_t b : {0xaa, 0xaa, 0xaa, 0x55}) reply_byte(b);
        sync_ = 1;
    } else if (sync_ == 1 && in_[1] == 0xaa && in_[2] == 0x00 && in_[3] == 0x00) {   // the second sync
        log("in sync 2", in_, 4);
        for (int i = 0; i < 4; i++) in_.pop_front();
        for (uint8_t b : {0xaa, 0xaa, 0x00, 0x00}) reply_byte(b);
        sync_ = 2;
    } else if (sync_ == 2 && in_.size() >= 6) {
        const uint8_t cmd = in_[1], node = in_[2], sub = in_[3];
        const size_t len = length_of(in_[4]);
        if (in_.size() < len + 6) return;
        uint8_t sum = 0;
        for (size_t i = 1; i < len + 5; i++) sum += in_[i];
        log("in", in_, len + 6);
        if (sum != in_[len + 5]) {
            if (log_) fprintf(stderr, "card: bad sum %02x, not %02x\n", in_[len + 5], sum);
            for (size_t i = 0; i < len + 6; i++) in_.pop_front();
            return;
        }
        const std::vector<uint8_t> raw(in_.begin(), in_.begin() + ptrdiff_t(len + 6));   // the whole packet
        const std::vector<uint8_t> data(raw.begin() + 5, raw.begin() + 5 + ptrdiff_t(len));
        for (size_t i = 0; i < len + 6; i++) in_.pop_front();

        bool answer = true;
        if (cmd == BUS) {
            // the bus's own commands (drmn10m's checks, 0x80090f6c, 0x80091160, 0x80091380)
            auto packet = [&](uint8_t sub_, const std::vector<uint8_t> &payload_) {
                const size_t sum_from = out_.size() + 1;   // the sum: every byte after the header (0x8009150c)
                for (uint8_t b : {HEADER, BUS, node, sub_, length_byte(payload_.size())}) reply_byte(b);
                for (uint8_t b : payload_) reply_byte(b);
                uint8_t s = 0;
                for (size_t i = sum_from; i < out_.size(); i++) s += out_[i];
                reply_byte(s);
            };
            switch (sub) {
            case 0x01:   // the node count: the packet goes round the chain, each node adding one
                packet(0x01, {uint8_t(data.empty() ? readers_.size() : data[0] + readers_.size())});
                break;
            case 0x02:   // a node's version, after the 573's packet heard back: type (big-endian),
                // flag, major, minor, revision, product (windyfairy's: type 3, 1.6, "ICCA")
                for (uint8_t b : raw) reply_byte(b);
                if (node >= 1 && node <= readers_.size()) packet(0x02, {0, 0, 0, 3, 0, 1, 6, 0, 'I', 'C', 'C', 'A', 0, 0, 0, 0});
                else packet(0x02, std::vector<uint8_t>(16, 0));
                break;
            case 0x03:   // start: status 0
                for (uint8_t b : raw) reply_byte(b);
                packet(0x03, {0});
                break;
            default: answer = false; break;
            }
        } else if (cmd == NODE && node >= 1 && node <= readers_.size()) {
            Reader &r = readers_[node - 1];
            std::vector<uint8_t> payload;
            switch (sub) {
            case 0x12: {   // the card's status: in (2), front and back sensors
                payload = {uint8_t(r.inserted ? 2 | 64 | 128 : 0)};
                break;
            }
            case 0x14: case 0x15:   // the slot: close, open, eject, format, read, write
                if (log_) fprintf(stderr, "card: reader %d slot %d\n", node, data.empty() ? -1 : data[0]);
                if (!data.empty() && data[0] == 2) r.inserted = false;   // given out
                payload = {0};
                break;
            case 0x16:   // write
                memcpy(r.data, data.data(), std::min(data.size(), sizeof r.data));
                save();
                payload = {0};
                break;
            case 0x18:   // read
                if (!r.inserted) payload = {0xff};
                else { payload = {0}; payload.insert(payload.end(), r.data, r.data + sizeof r.data); }
                break;
            case 0x26: {   // the 10-key's new presses, as scan codes (0: none)
                uint32_t found = 0;
                for (size_t i = 0; i < std::size(KEYS); i++)
                    if (r.keys & (1u << i)) {
                        if (!(r.last_keys & (1u << i))) payload.push_back(KEYS[i].code);
                        found |= 1u << i;
                    }
                if (payload.empty()) payload = {0};
                r.last_keys = found;
                break;
            }
            default: payload = {0}; break;   // init (00, 10, 11, 20), 10-key status (24), format (1e, 1f), ...
            }
            // the packet echoed, then the answer (the line is half duplex: the 573 reads its own)
            for (uint8_t b : raw) reply_byte(b);
            const size_t sum_from = out_.size() + 1;
            for (uint8_t b : {HEADER, uint8_t(0x01), node, sub, length_byte(payload.size())}) reply_byte(b);
            for (uint8_t b : payload) reply_byte(b);
            uint8_t s = 0;
            for (size_t i = sum_from; i < out_.size(); i++) s += out_[i];
            reply_byte(s);
        } else {
            answer = false;
        }
        if (!answer && log_) fprintf(stderr, "card: no answer to %02x %02x %02x\n", cmd, node, sub);
    } else {
        return;
    }
    if (out_.size() > before) {
        if (log_) {
            std::deque<uint8_t> d(out_.begin() + ptrdiff_t(before), out_.end());
            log("out", d, d.size());
        }
        if (send_timer_ && !sending_) {
            sending_ = true;
            send_timer_->adjust(attotime::from_hz(BAUD) * 10);
        }
    }
}

// a byte to the 573 every ten bit times, while the card units are switched in
void CardUnit::send_tick(s32) {
    if (out_.empty()) {
        sending_ = false;
        return;
    }
    if (selected_) {
        const uint8_t b = out_.front();
        out_.pop_front();
        rxd_(0);
        for (int i = 0; i < 8; i++) rxd_((b >> i) & 1);
        rxd_(1);
    }
    send_timer_->adjust(attotime::from_hz(BAUD) * 10);
}

}  // namespace hk::k573
