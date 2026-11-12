// SPDX-License-Identifier: GPL-3.0-or-later
// The shop's LAN between Python cabinets in separate processes (lancable.h).
#include "python/lancable.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include "shell/net.h"
#include "shell/sys.h"

namespace hk::py {

namespace {

constexpr uint32_t MAGIC = 0x48'4b'50'4c;   // "HKPL"
constexpr uint32_t VERSION = 1;

struct Hello {
    uint32_t magic, version;
    int32_t index, cabinets;   // from the host: the guest's number, and how many there are
};

// "host:port" (TCP) unless it looks like a path (as sys573/link.cpp: C:\ is a path on Windows)
bool is_tcp(const std::string &w) { return w.find_first_of("/\\") == std::string::npos && w.rfind(':') != std::string::npos; }

int tcp_socket(const std::string &w, bool host) {
    const size_t c = w.rfind(':');
    std::string name = w.substr(0, c), port = w.substr(c + 1);
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = host ? AI_PASSIVE : 0;
    net::start();
    if (getaddrinfo(name.empty() ? nullptr : name.c_str(), port.c_str(), &hints, &res) != 0) return -1;
    int fd = -1;
    for (addrinfo *a = res; a && fd < 0; a = a->ai_next) {
        fd = net::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        if (host) {
            net::set_int(fd, SOL_SOCKET, SO_REUSEADDR, 1);
            if (bind(fd, a->ai_addr, (int)a->ai_addrlen) == 0 && listen(fd, 4) == 0) break;
        } else if (connect(fd, a->ai_addr, (int)a->ai_addrlen) == 0) {
            break;
        }
        net::close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

int unix_socket(const std::string &path, bool host) {
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    if (path.size() >= sizeof a.sun_path) return -1;
    memcpy(a.sun_path, path.c_str(), path.size() + 1);
    int fd = net::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (host) {
        net::remove_path(path);
        if (bind(fd, (sockaddr *)&a, sizeof a) == 0 && listen(fd, 4) == 0) return fd;
    } else if (connect(fd, (sockaddr *)&a, sizeof a) == 0) {
        return fd;
    }
    net::close(fd);
    return -1;
}

bool send_all(int fd, const void *p, size_t n) {
    const char *c = (const char *)p;
    while (n) {
        ssize_t k = net::send(fd, c, n);
        if (k < 0 && net::interrupted()) continue;
        if (k <= 0) return false;
        c += k;
        n -= (size_t)k;
    }
    return true;
}

bool recv_all(int fd, void *p, size_t n) {
    char *c = (char *)p;
    while (n) {
        ssize_t k = net::recv(fd, c, n);
        if (k < 0 && net::interrupted()) continue;
        if (k <= 0) return false;
        c += k;
        n -= (size_t)k;
    }
    return true;
}

void no_delay(int fd) {
    net::set_int(fd, IPPROTO_TCP, TCP_NODELAY, 1);   // fails harmlessly on a Unix socket
}

struct BatchHeader {
    uint64_t step;
    uint32_t count;
};

struct PacketHeader {
    uint32_t src, dst;
    uint16_t sport, dport;
    uint32_t size;
};

}  // namespace

LanCable::~LanCable() {
    for (int fd : fds_) net::close(fd);
}

bool LanCable::open(const std::string &where, bool host, int cabinets) {
    host_ = host;
    const bool tcp = is_tcp(where);
    if (host) {
        if (cabinets < 2 || cabinets > 4) {
            fprintf(stderr, "lan: 2 to 4 cabinets, not %d\n", cabinets);
            return false;
        }
        int ls = tcp ? tcp_socket(where, true) : unix_socket(where, true);
        if (ls < 0) {
            fprintf(stderr, "lan: can't listen at %s: %s\n", where.c_str(), net::error().c_str());
            return false;
        }
        for (int i = 1; i < cabinets; i++) {
            fprintf(stderr, "lan: waiting for cabinet %d of %d at %s\n", i + 1, cabinets, where.c_str());
            const int fd = net::accept(ls);
            if (fd < 0) break;
            no_delay(fd);
            Hello mine{MAGIC, VERSION, i, cabinets}, theirs{};
            if (!send_all(fd, &mine, sizeof mine) || !recv_all(fd, &theirs, sizeof theirs) || theirs.magic != MAGIC ||
                theirs.version != VERSION) {
                fprintf(stderr, "lan: cabinet %d isn't a matching HIRAKU Python core\n", i + 1);
                net::close(fd);
                i--;
                continue;
            }
            fds_.push_back(fd);
        }
        net::close(ls);
        if (!tcp) net::remove_path(where);
        if ((int)fds_.size() != cabinets - 1) {
            fprintf(stderr, "lan: not all cabinets came\n");
            return false;
        }
        index_ = 0;
    } else {
        int fd = -1;
        for (int tries = 0; tries < 6000 && fd < 0; tries++) {   // ten minutes (the host may still be starting)
            fd = tcp ? tcp_socket(where, false) : unix_socket(where, false);
            if (fd < 0) hk::sys::sleep_for(std::chrono::milliseconds(100));
        }
        if (fd < 0) {
            fprintf(stderr, "lan: no host cabinet at %s\n", where.c_str());
            return false;
        }
        no_delay(fd);
        Hello mine{MAGIC, VERSION, 0, 0}, theirs{};
        if (!recv_all(fd, &theirs, sizeof theirs) || theirs.magic != MAGIC || theirs.version != VERSION ||
            !send_all(fd, &mine, sizeof mine)) {
            fprintf(stderr, "lan: the host isn't a matching HIRAKU Python core\n");
            net::close(fd);
            return false;
        }
        fds_.push_back(fd);
        index_ = theirs.index;
        cabinets = theirs.cabinets;
    }
    fprintf(stderr, "lan: connected, cabinet %d of %d (192.168.50.%d)\n", index_ + 1, cabinets, 2 + index_);
    return true;
}

bool LanCable::send_batch(int fd, const std::vector<Packet> &p) {
    BatchHeader h{step_, (uint32_t)p.size()};
    if (!send_all(fd, &h, sizeof h)) return false;
    for (const Packet &k : p) {
        PacketHeader ph{k.src, k.dst, k.sport, k.dport, (uint32_t)k.data.size()};
        if (!send_all(fd, &ph, sizeof ph) || (!k.data.empty() && !send_all(fd, k.data.data(), k.data.size()))) return false;
    }
    return true;
}

bool LanCable::recv_batch(int fd, std::vector<Packet> &p) {
    BatchHeader h{};
    if (!recv_all(fd, &h, sizeof h) || h.step != step_ || h.count > 100000) return false;
    for (uint32_t i = 0; i < h.count; i++) {
        PacketHeader ph{};
        if (!recv_all(fd, &ph, sizeof ph) || ph.size > 65536) return false;
        Packet k{ph.src, ph.dst, ph.sport, ph.dport, std::vector<uint8_t>(ph.size)};
        if (ph.size && !recv_all(fd, k.data.data(), ph.size)) return false;
        p.push_back(std::move(k));
    }
    return true;
}

void LanCable::exchange(std::vector<Packet> &out, std::vector<Packet> &in) {
    if (fds_.empty()) return;
    step_++;
    if (host_) {
        // everyone's, in the cabinets' order: ours, then each guest's
        std::vector<Packet> all = out;
        for (int fd : fds_)
            if (!recv_batch(fd, all)) return lost("a guest is gone");
        for (int fd : fds_)
            if (!send_batch(fd, all)) return lost("a guest is gone");
        in.insert(in.end(), std::make_move_iterator(all.begin() + (ptrdiff_t)out.size()), std::make_move_iterator(all.end()));
    } else {
        std::vector<Packet> all;
        if (!send_batch(fds_[0], out) || !recv_batch(fds_[0], all)) return lost("the host is gone");
        // the host sends everyone's: ours are the ones from our address
        const uint32_t me = 0xc0a83202u + (uint32_t)index_;
        for (Packet &k : all)
            if (k.src != me) in.push_back(std::move(k));
    }
    out.clear();
}

void LanCable::lost(const char *why) {
    fprintf(stderr, "lan: %s at step %llu; the cable is idle now\n", why, (unsigned long long)step_);
    for (int fd : fds_) net::close(fd);
    fds_.clear();
}

}  // namespace hk::py
