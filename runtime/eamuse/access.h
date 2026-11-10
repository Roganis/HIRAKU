// SPDX-License-Identifier: GPL-3.0-or-later
// Who a server on a network answers (hiraku-eamuse, tools/eamuse/server.cpp). e-AMUSE3 has no log-in:
// a cabinet is whoever sends its calls, so a server that others can reach is told which to take.
//
//   AddressList   the addresses its ports take connections from (--allow): 192.168.1.0/24, 10.0.0.5.
//                 The whole port: every game's calls and NET TAISEN's relay. This computer's own
//                 (127.0.0.0/8) is always in.
//   CabinetList   the cabinets whose calls are answered, by PCBID (the calls' srcid): a file, a
//                 cabinet a line, 20 hex digits and whatever names it after. Read again when it
//                 changes, so a cabinet is added without stopping the server.
//
// A PCBID is as good a secret as it is known to few: a game's own (the one its dump carries) is
// every copy's, so each cabinet is given one of its own (-p, shell/eaoptions.h; new_pcbid()).
#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace hk::ea {

class AddressList {
public:
    // One or more of A.B.C.D or A.B.C.D/BITS, with commas or spaces between. False, *error saying
    // which, for one that isn't (nothing of the text is taken then)
    bool add(const std::string &text, std::string *error) {
        std::vector<Range> more;
        for (size_t at = 0; at < text.size();) {
            const size_t end = text.find_first_of(", \t", at);
            const std::string item = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
            at = end == std::string::npos ? text.size() : end + 1;
            if (item.empty()) continue;
            unsigned a, b, c, d, bits = 32;
            char tail = 0;
            const int n = std::sscanf(item.c_str(), "%u.%u.%u.%u/%u%c", &a, &b, &c, &d, &bits, &tail);
            // (sscanf alone would take "1.2.3.4x", "1.2.3.4.5" and "1.2.3.4/": digits, three dots, and
            // after a slash, if there is one, the bits)
            const size_t slash = item.find('/');
            const bool plain = item.find_first_not_of("0123456789./") == std::string::npos &&
                               std::count(item.begin(), item.end(), '.') == 3 &&
                               (slash == std::string::npos || (slash + 1 < item.size() && item.find('/', slash + 1) == std::string::npos));
            if (!plain || (n != 4 && n != 5) || a > 255 || b > 255 || c > 255 || d > 255 || bits > 32) {
                if (error) *error = item + " isn't an address (192.168.1.20) or a range (192.168.1.0/24)";
                return false;
            }
            const uint32_t mask = bits ? ~uint32_t(0) << (32 - bits) : 0;
            more.push_back({(a << 24 | b << 16 | c << 8 | d) & mask, mask, item});
        }
        ranges_.insert(ranges_.end(), more.begin(), more.end());
        return true;
    }
    bool empty() const { return ranges_.empty(); }
    // ip: host byte order. With none listed every address is in
    bool has(uint32_t ip) const {
        if (ranges_.empty() || ip >> 24 == 127) return true;
        for (const Range &r : ranges_)
            if ((ip & r.mask) == r.net) return true;
        return false;
    }
    std::string text() const {
        std::string s;
        for (const Range &r : ranges_) s += (s.empty() ? "" : ", ") + r.as_given;
        return s;
    }

private:
    struct Range {
        uint32_t net, mask;
        std::string as_given;
    };
    std::vector<Range> ranges_;
};

// 20 hex digits, in capitals; "" if the text isn't
inline std::string pcbid_of(const std::string &text) {
    if (text.size() != 20) return "";
    std::string id;
    for (char c : text) {
        if (!std::isxdigit((unsigned char)c)) return "";
        id += (char)std::toupper((unsigned char)c);
    }
    return id;
}

// The check byte of a PCB id, its last: a CRC-8 (0x8c) of its bytes 2-8 in the security plug's
// order (bemanitools' security_id; runtime/pc/ezusb_dev.c's id_check, which puts it right in an id
// that has another: an id with its check byte right is sent as it is by every core)
inline uint8_t pcbid_check(const uint8_t id[10]) {
    const uint8_t b[7] = {id[2], id[8], id[7], id[6], id[5], id[4], id[3]};
    uint8_t c = 0;
    for (uint8_t v : b)
        for (int k = 0; k < 8; k++) {
            const int bit = (c ^ (v >> k)) & 1;
            c >>= 1;
            if (bit) c ^= 0x8c;
        }
    return c;
}

// A PCBID nobody else has: 01 (as the boards' start), eight bytes by chance, the check byte
inline std::string new_pcbid() {
    std::random_device rd;
    uint8_t id[10] = {1};
    for (int i = 1; i < 9; i++) id[i] = (uint8_t)rd();
    id[9] = pcbid_check(id);
    char s[21];
    for (int i = 0; i < 10; i++) std::snprintf(s + 2 * i, 3, "%02X", id[i]);
    return s;
}

class CabinetList {
public:
    // The file (it need not be there yet: in_force() says)
    void open(const std::string &file) {
        file_ = file;
        load();
    }
    const std::string &file() const { return file_; }
    // A list is in force once its file is there, with nothing in it too (no cabinet is answered then)
    bool in_force() {
        load();
        return there_;
    }
    size_t size() const { return ids_.size(); }
    // Lines that were neither a PCBID nor a comment, as the file was last read ("LINE: text")
    const std::vector<std::string> &not_read() const { return not_read_; }
    bool has(const std::string &srcid) {
        load();
        return !there_ || ids_.count(pcbid_of(srcid)) != 0;
    }
    // A new line at the file's end (the file made if it isn't there): false if it can't be written
    bool add(const std::string &id, const std::string &name) {
        std::ofstream f(file_, std::ios::app);
        f << id << (name.empty() ? "" : "  " + name) << "\n";
        f.close();
        stamp_ = {};
        return f.good();
    }

private:
    // (again when the file's time or size has changed: asked at each call, which is nothing beside the call)
    void load() {
        if (file_.empty()) return;
        std::error_code ec;
        const auto time = std::filesystem::last_write_time(file_, ec);
        const bool there = !ec;
        const auto size = there ? std::filesystem::file_size(file_, ec) : 0;
        const Stamp now{there, there ? time.time_since_epoch().count() : 0, (uint64_t)size};
        if (loaded_ && now.there == stamp_.there && now.time == stamp_.time && now.size == stamp_.size) return;
        loaded_ = true, stamp_ = now, there_ = there;
        ids_.clear(), not_read_.clear();
        std::ifstream f(file_);
        std::string line;
        for (int n = 1; std::getline(f, line); n++) {
            const size_t from = line.find_first_not_of(" \t\r");
            if (from == std::string::npos || line[from] == '#') continue;
            const size_t to = line.find_first_of(" \t\r#", from);
            const std::string id = pcbid_of(line.substr(from, to == std::string::npos ? std::string::npos : to - from));
            if (id.empty()) not_read_.push_back(std::to_string(n) + ": " + line.substr(from));
            else ids_.insert(id);
        }
    }
    struct Stamp {
        bool there = false;
        long long time = 0;
        uint64_t size = 0;
    };
    std::string file_;
    std::set<std::string> ids_;
    std::vector<std::string> not_read_;
    Stamp stamp_;
    bool loaded_ = false, there_ = false;
};

}  // namespace hk::ea
