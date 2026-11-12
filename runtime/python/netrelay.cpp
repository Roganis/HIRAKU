// SPDX-License-Identifier: GPL-3.0-or-later
// NET TAISEN online through hiraku-eamuse's relay (netrelay.h).
#include "python/netrelay.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>

#include "shell/net.h"

#include "python/lobby.h"

namespace hk::py {

namespace {

constexpr uint32_t MAGIC = 0x48'4b'50'52;   // "HKPR"
constexpr uint32_t VERSION = 1;
constexpr uint32_t FIRST = 0x0a390001;      // 10.57.0.1, the first cabinet's address

struct Hello {
    uint32_t magic, version, address;   // address: from the relay, the cabinet's
};

struct PacketHeader {
    uint32_t src, dst;
    uint16_t sport, dport;
    uint32_t size;
};

std::string dotted(uint32_t a) {
    char b[20];
    snprintf(b, sizeof b, "%u.%u.%u.%u", a >> 24, (a >> 16) & 255, (a >> 8) & 255, a & 255);
    return b;
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

// A datagram as it goes over the connection
std::string packet_bytes(const LanCable::Packet &k) {
    const PacketHeader h{k.src, k.dst, k.sport, k.dport, (uint32_t)k.data.size()};
    std::string s((const char *)&h, sizeof h);
    s.append((const char *)k.data.data(), k.data.size());
    return s;
}

}  // namespace

// --- the cabinet's side ---

RelayClient::~RelayClient() {
    if (fd_ >= 0) net::shutdown(fd_);
    if (thread_.joinable()) thread_.join();
    if (fd_ >= 0) net::close(fd_);
}

bool RelayClient::put(const void *p, size_t n) { return keyed_ ? keyed_->send(p, n) : send_all(fd_, p, n); }

bool RelayClient::take(void *p, size_t n) {
    if (!keyed_) return recv_all(fd_, p, n);
    char *c = (char *)p;
    while (n) {
        const ssize_t k = keyed_->recv(c, n);
        if (k <= 0) return false;
        c += k, n -= (size_t)k;
    }
    return true;
}

bool RelayClient::open(const std::string &host, uint16_t port, int seconds, std::string *error, const net::Key &key) {
    net::start();
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (int e = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res); e != 0) {
        *error = host + ": " + net::resolve_error(e);
        return false;
    }
    int fd = -1;
    for (addrinfo *a = res; a && fd < 0; a = a->ai_next) {
        fd = net::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        if (!net::connect_within(fd, a->ai_addr, a->ai_addrlen, seconds * 1000)) {
            *error = host + ":" + std::to_string(port) + ": " + net::error();
            net::close(fd);
            fd = -1;
        }
    }
    freeaddrinfo(res);
    if (fd < 0) return false;
    net::set_int(fd, IPPROTO_TCP, TCP_NODELAY, 1);
    net::set_recv_timeout(fd, seconds * 1000);
    fd_ = fd;
    if (key.set()) {
        keyed_ = std::make_unique<net::Keyed>();
        std::string why;
        if (!keyed_->connect(fd, key, &why)) {
            *error = host + ":" + std::to_string(port) + ": " + why;
            keyed_.reset();
            net::close(fd);
            fd_ = -1;
            return false;
        }
    }
    Hello mine{MAGIC, VERSION, 0}, theirs{};
    if (!put(&mine, sizeof mine) || !take(&theirs, sizeof theirs) || theirs.magic != MAGIC || theirs.version != VERSION) {
        // (a relay with a key says nothing to a cabinet without one)
        *error = host + ":" + std::to_string(port) + (key.set() ? ": not a HIRAKU relay (or another version)"
                                                                 : ": not a HIRAKU relay, another version, or one that wants a key (-eakey)");
        keyed_.reset();
        net::close(fd);
        fd_ = -1;
        return false;
    }
    net::set_recv_timeout(fd, 0);
    address_ = theirs.address;
    thread_ = std::thread([this] { reader(); });
    return true;
}

void RelayClient::reader() {
    for (;;) {
        PacketHeader h{};
        if (!take(&h, sizeof h) || h.size > 65536) break;
        LanCable::Packet k{h.src, h.dst, h.sport, h.dport, std::vector<uint8_t>(h.size)};
        if (h.size && !take(k.data.data(), h.size)) break;
        std::lock_guard<std::mutex> l(mutex_);
        came_.push_back(std::move(k));
    }
    std::lock_guard<std::mutex> l(mutex_);
    lost_ = true;
}

void RelayClient::exchange(std::vector<LanCable::Packet> &out, std::vector<LanCable::Packet> &in) {
    if (fd_ < 0) return;
    bool lost;
    {
        std::lock_guard<std::mutex> l(mutex_);
        in.insert(in.end(), std::make_move_iterator(came_.begin()), std::make_move_iterator(came_.end()));
        came_.clear();
        lost = lost_;
    }
    if (!lost)
        for (const LanCable::Packet &k : out) {
            const std::string bytes = packet_bytes(k);
            if (!put(bytes.data(), bytes.size())) { lost = true; break; }
        }
    if (lost) {
        fprintf(stderr, "relay: the connection is gone; NET TAISEN online is off now\n");
        net::shutdown(fd_);
        if (thread_.joinable()) thread_.join();
        net::close(fd_);
        fd_ = -1;
    }
}

// --- the server's side ---

bool RelayServer::start(const std::string &bind_addr, uint16_t port) {
    listen_fd_ = net::socket(AF_INET, SOCK_STREAM, 0);
    net::set_int(listen_fd_, SOL_SOCKET, SO_REUSEADDR, 1);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    if (inet_pton(AF_INET, bind_addr.c_str(), &sa.sin_addr) != 1 || bind(listen_fd_, (sockaddr *)&sa, sizeof sa) != 0 ||
        listen(listen_fd_, 16) != 0) {
        fprintf(stderr, "relay: %s:%u: %s\n", bind_addr.c_str(), port, net::error().c_str());
        net::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    socklen_t len = sizeof sa;
    getsockname(listen_fd_, (sockaddr *)&sa, &len);
    port_ = ntohs(sa.sin_port);
    thread_ = std::thread([this] { run(); });
    thread_.detach();
    return true;
}

void RelayServer::run() {
    struct Cabinet {
        uint32_t address = 0;
        bool greeted = false;
        std::string in;   // bytes not yet a whole hello or datagram
        // behind the key: the bytes as they came (not yet whole records), and the two directions
        uint32_t ip = 0;
        bool keyed = false;
        std::string raw;
        net::Sealer out;
        net::Opener opened;
        std::chrono::steady_clock::time_point came = std::chrono::steady_clock::now();
    };
    // At most kCabinets connections; one that hasn't said its hello kHelloSeconds after it came is let
    // go (connections that only take up places); a cabinet that takes nothing for kSendMs is let go
    // too, where the others' packets to it would wait on it for ever
    constexpr size_t kCabinets = 64;
    constexpr int kHelloSeconds = 10, kSendMs = 2000;
    std::map<int, Cabinet> cabinets;
    uint32_t next = FIRST;
    Lobby lobby;   // pop'n 12-14's
    std::map<uint16_t, Lobby> pc_lobbies;   // the PC games', one each (Lobby::pc_port)
    const auto t0 = std::chrono::steady_clock::now();
    auto say = [&](const std::string &s) { if (log) log("relay: " + s); };
    // bytes to a cabinet, behind the key if its connection is
    auto put = [&](int fd, const void *p, size_t n) {
        Cabinet &c = cabinets[fd];
        if (!c.keyed) return send_all(fd, p, n);
        std::string wire;
        c.out.seal(p, n, wire);
        return send_all(fd, wire.data(), wire.size());
    };
    auto put_packet = [&](int fd, const LanCable::Packet &k) {
        const std::string bytes = packet_bytes(k);
        return put(fd, bytes.data(), bytes.size());
    };
    auto away = [&](const Cabinet &c, const char *why) { if (turned_away) turned_away(c.ip, why); };
    for (;;) {
        std::vector<pollfd> fds{net::watch(listen_fd_, POLLIN)};
        for (auto &[fd, c] : cabinets) fds.push_back(net::watch(fd, POLLIN));
        if (net::poll(fds.data(), fds.size(), 1000) < 0) {
            if (net::interrupted()) continue;
            return;
        }
        if (fds[0].revents & POLLIN) {
            sockaddr_in peer{};
            const int fd = net::accept(listen_fd_, &peer);
            if (fd >= 0 && (cabinets.size() >= kCabinets || (admit && !admit(ntohl(peer.sin_addr.s_addr))))) {
                net::close(fd);   // (too many, or not an address the server takes: the refusal is the caller's to log)
            } else if (fd >= 0) {
                net::set_int(fd, IPPROTO_TCP, TCP_NODELAY, 1);
                net::set_timeouts(fd, kSendMs);   // (its receives come after poll says there is something)
                cabinets[fd].ip = ntohl(peer.sin_addr.s_addr);
            }
        }
        std::vector<int> gone;
        for (auto &[fd, c] : cabinets)
            if (!c.greeted && std::chrono::steady_clock::now() - c.came > std::chrono::seconds(kHelloSeconds)) gone.push_back(fd);
        for (size_t i = 1; i < fds.size(); i++) {
            if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            const int fd = net::fd_of(fds[i]);
            Cabinet &c = cabinets[fd];
            char buf[8192];
            const ssize_t n = net::recv(fd, buf, sizeof buf);
            if (n <= 0) { gone.push_back(fd); continue; }
            if (key.set()) {
                // Nothing of a connection is read as a cabinet's until it came behind the key
                c.raw.append(buf, (size_t)n);
                if (!c.keyed) {
                    if (c.raw.size() < 4) continue;
                    if (!net::keyed::is_hello(c.raw.data())) { away(c, "without the key"); gone.push_back(fd); continue; }
                    if (c.raw.size() < net::keyed::kHello) continue;
                    uint8_t answer[net::keyed::kServerHello];
                    if (!net::keyed::server_answer(key, (const uint8_t *)c.raw.data(), answer, c.out, c.opened) ||
                        !send_all(fd, answer, sizeof answer)) {
                        gone.push_back(fd);
                        continue;
                    }
                    c.raw.erase(0, net::keyed::kHello);
                    c.keyed = true;
                }
                if (!c.opened.open(c.raw, c.in)) {
                    away(c, c.opened.records() ? "with a record that isn't the key's" : "with another key");
                    gone.push_back(fd);
                    continue;
                }
            } else {
                c.in.append(buf, (size_t)n);
                if (!c.greeted && c.in.size() >= 4 && net::keyed::is_hello(c.in.data())) {   // a cabinet with a key, and none here
                    send_all(fd, net::keyed::kNoKey, 4);
                    away(c, "with a key, and this server has none");
                    gone.push_back(fd);
                    continue;
                }
            }
            if (!c.greeted) {
                if (c.in.size() < sizeof(Hello)) continue;
                Hello h;
                memcpy(&h, c.in.data(), sizeof h);
                c.in.erase(0, sizeof h);
                if (h.magic != MAGIC || h.version != VERSION) { gone.push_back(fd); continue; }
                c.address = next++;
                Hello mine{MAGIC, VERSION, c.address};
                if (!put(fd, &mine, sizeof mine)) { gone.push_back(fd); continue; }
                c.greeted = true;
                size_t here = 0;
                for (auto &[ofd, o] : cabinets) here += o.greeted;
                say("a cabinet came, " + dotted(c.address) + " (" + std::to_string(here) + " now)");
            }
            while (c.in.size() >= sizeof(PacketHeader)) {
                PacketHeader h;
                memcpy(&h, c.in.data(), sizeof h);
                if (h.size > 65536) { gone.push_back(fd); break; }
                if (c.in.size() < sizeof h + h.size) break;
                LanCable::Packet k{c.address, h.dst, h.sport, h.dport,
                                   std::vector<uint8_t>(c.in.begin() + sizeof h, c.in.begin() + (ptrdiff_t)(sizeof h + h.size))};
                c.in.erase(0, sizeof h + h.size);
                // to a cabinet: passed on, from the sender's address
                int to = -1;
                for (auto &[ofd, o] : cabinets)
                    if (o.greeted && o.address == k.dst) to = ofd;
                if (to >= 0) {
                    if (!put_packet(to, k)) gone.push_back(to);
                } else if (k.dport == Lobby::PORT || Lobby::is_pc_port(k.dport)) {   // a lobby, at the address the game sent to
                    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                    Lobby &l = k.dport == Lobby::PORT ? lobby : pc_lobbies.try_emplace(k.dport, true).first->second;
                    std::vector<uint8_t> r = l.handle(k.data.data(), k.data.size(), now);
                    if (!r.empty() && !put_packet(fd, {k.dst, k.src, k.dport, k.sport, std::move(r)})) gone.push_back(fd);
                    if (log) {
                        static const char *kinds[] = {"entry", "list", "leave", "participate"};
                        const int kind = k.data.size() > 10 ? k.data[10] : -1;
                        say(dotted(k.src) + " lobby " + (kind >= 0 && kind < 4 ? kinds[kind] : "?") + " (port " + std::to_string(k.dport) + ")");
                    }
                }
                // anywhere else (a broadcast, a shop's LAN): nowhere
            }
        }
        for (int fd : gone)
            if (auto it = cabinets.find(fd); it != cabinets.end()) {
                if (it->second.greeted) say(dotted(it->second.address) + " left");
                net::close(fd);
                cabinets.erase(it);
            }
    }
}

}  // namespace hk::py
