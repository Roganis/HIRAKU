// SPDX-License-Identifier: GPL-3.0-or-later
// A TCP connection behind a key (keyed.h).
#include "shell/keyed.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

#include "shell/crypt.h"
#include "shell/net.h"

namespace hk::net {

namespace {

const char kMagic[4] = {'H', 'K', 'K', '1'};

bool send_all(int fd, const void *p, size_t n) {
    const char *c = (const char *)p;
    while (n) {
        const ssize_t k = net::send(fd, c, n);
        if (k < 0 && interrupted()) continue;
        if (k <= 0) return false;
        c += k, n -= (size_t)k;
    }
    return true;
}

bool recv_all(int fd, void *p, size_t n) {
    char *c = (char *)p;
    while (n) {
        const ssize_t k = net::recv(fd, c, n);
        if (k < 0 && interrupted()) continue;
        if (k <= 0) return false;
        c += k, n -= (size_t)k;
    }
    return true;
}

// A record's nonce: its number in its direction
void nonce_of(uint64_t count, uint8_t nonce[12]) {
    memset(nonce, 0, 4);
    for (int i = 0; i < 8; i++) nonce[4 + i] = (uint8_t)(count >> (8 * i));
}

// The two directions' keys for a connection: the key, which way, and both sides' 16 bytes
void direction_keys(const Key &key, const uint8_t *client16, const uint8_t *server16, uint8_t c2s[32], uint8_t s2c[32]) {
    uint8_t m[3 + 32];
    memcpy(m + 3, client16, 16);
    memcpy(m + 19, server16, 16);
    memcpy(m, "c2s", 3);
    crypt::hmac_sha256(key.bytes(), 32, m, sizeof m, c2s);
    memcpy(m, "s2c", 3);
    crypt::hmac_sha256(key.bytes(), 32, m, sizeof m, s2c);
}

}  // namespace

// --- the key ---

Key::Key(const std::string &given) {
    const size_t from = given.find_first_not_of(" \t\r\n"), to = given.find_last_not_of(" \t\r\n");
    if (from == std::string::npos) return;
    crypt::pbkdf2_sha256(given.substr(from, to - from + 1), "HIRAKU e-AMUSEMENT key 1", kRounds, k_);
    set_ = true;
}

std::string Key::make() {
    static const char letters[] = "abcdefghjkmnpqrstuvwxyz023456789";   // 32: none of i, l, o, 1
    uint8_t r[25];
    if (!crypt::random_bytes(r, sizeof r)) return "";
    std::string s;
    for (int i = 0; i < 25; i++) {
        if (i && i % 5 == 0) s += '-';
        s += letters[r[i] & 31];
    }
    return s;
}

std::string Key::wrong_with(const std::string &given) {
    const size_t from = given.find_first_not_of(" \t\r\n"), to = given.find_last_not_of(" \t\r\n");
    const size_t n = from == std::string::npos ? 0 : to - from + 1;
    if (n == 0) return "no key in it";
    if (n < 12) return "a key of " + std::to_string(n) + " characters is soon guessed: 12 at least (hiraku-eamuse --new-key makes one)";
    return "";
}

// --- records ---

void Sealer::start(const uint8_t key[32]) {
    memcpy(key_, key, 32);
    count_ = 0;
}

void Sealer::seal(const void *p, size_t n, std::string &wire) {
    const uint8_t *c = (const uint8_t *)p;
    do {
        const size_t take = n < keyed::kRecord ? n : keyed::kRecord;
        const uint8_t len[2] = {(uint8_t)(take >> 8), (uint8_t)take};
        uint8_t nonce[12], tag[16];
        nonce_of(count_++, nonce);
        const size_t at = wire.size();
        wire.resize(at + 2 + take + 16);
        memcpy(&wire[at], len, 2);
        crypt::seal(key_, nonce, len, 2, c, take, (uint8_t *)&wire[at + 2], tag);
        memcpy(&wire[at + 2 + take], tag, 16);
        c += take, n -= take;
    } while (n);
}

void Opener::start(const uint8_t key[32]) {
    memcpy(key_, key, 32);
    count_ = 0;
}

bool Opener::open(std::string &raw, std::string &plain) {
    size_t at = 0;
    bool good = true;
    while (raw.size() - at >= 2) {
        const size_t n = (size_t)(uint8_t)raw[at] << 8 | (uint8_t)raw[at + 1];
        if (n > keyed::kRecord) { good = false; break; }
        if (raw.size() - at < 2 + n + 16) break;
        uint8_t nonce[12];
        nonce_of(count_, nonce);
        const size_t to = plain.size();
        plain.resize(to + n);
        if (!crypt::open(key_, nonce, &raw[at], 2, &raw[at + 2], n, (const uint8_t *)&raw[at + 2 + n], (uint8_t *)&plain[to])) {
            plain.resize(to);
            good = false;
            break;
        }
        count_++;
        at += 2 + n + 16;
    }
    raw.erase(0, at);
    return good;
}

// --- the hellos ---

namespace keyed {

const char kNoKey[4] = {'H', 'K', 'K', '0'};

bool is_hello(const void *first4) { return memcmp(first4, kMagic, 4) == 0; }

bool client_hello(uint8_t hello[kHello]) {
    memcpy(hello, kMagic, 4);
    return crypt::random_bytes(hello + 4, 16);
}

bool server_answer(const Key &key, const uint8_t hello[kHello], uint8_t answer[kServerHello], Sealer &out, Opener &in) {
    memcpy(answer, kMagic, 4);
    if (!key.set() || !crypt::random_bytes(answer + 4, 16)) return false;
    uint8_t c2s[32], s2c[32];
    direction_keys(key, hello + 4, answer + 4, c2s, s2c);
    out.start(s2c);
    in.start(c2s);
    std::string record;
    out.seal(nullptr, 0, record);   // the empty record: this side has the key
    memcpy(answer + 20, record.data(), 18);
    return true;
}

int client_take(const Key &key, const uint8_t hello[kHello], const uint8_t answer[kServerHello], Sealer &out, Opener &in) {
    if (!is_hello(answer)) return 2;
    uint8_t c2s[32], s2c[32];
    direction_keys(key, hello + 4, answer + 4, c2s, s2c);
    out.start(c2s);
    in.start(s2c);
    std::string raw((const char *)answer + 20, 18), plain;
    return in.open(raw, plain) && in.records() == 1 && raw.empty() ? 0 : 1;
}

}  // namespace keyed

// --- a blocking socket behind the key ---

bool Keyed::connect(int fd, const Key &key, std::string *error) {
    fd_ = fd;
    uint8_t hello[keyed::kHello], answer[keyed::kServerHello];
    if (!keyed::client_hello(hello)) { *error = "the system gives no random numbers for the key's use"; return false; }
    if (!send_all(fd, hello, sizeof hello) || !recv_all(fd, answer, 4)) {
        *error = "it gave no answer to the key (not a hiraku-eamuse, or one older than keys)";
        return false;
    }
    if (memcmp(answer, keyed::kNoKey, 4) == 0) {
        *error = "the server has no key, and a key was given here (-eakey): a key on both sides, or on neither";
        return false;
    }
    if (!keyed::is_hello(answer) || !recv_all(fd, answer + 4, sizeof answer - 4)) {
        *error = "it gave no answer to the key (not a hiraku-eamuse, or one older than keys)";
        return false;
    }
    if (keyed::client_take(key, hello, answer, out_, in_) != 0) {
        *error = "the server's key is another than the one given here (-eakey)";
        return false;
    }
    return true;
}

bool Keyed::accept(int fd, const Key &key, const uint8_t first4[4]) {
    fd_ = fd;
    uint8_t hello[keyed::kHello], answer[keyed::kServerHello];
    memcpy(hello, first4, 4);
    return recv_all(fd, hello + 4, 16) && keyed::server_answer(key, hello, answer, out_, in_) && send_all(fd, answer, sizeof answer);
}

bool Keyed::send(const void *p, size_t n) {
    if (n == 0) return true;
    std::string wire;
    out_.seal(p, n, wire);
    return send_all(fd_, wire.data(), wire.size());
}

std::ptrdiff_t Keyed::recv(void *p, size_t n) {
    while (plain_.empty()) {
        if (wrong_) return -1;
        char buf[4096];
        ssize_t k = net::recv(fd_, buf, sizeof buf);
        if (k < 0 && interrupted()) continue;
        if (k <= 0) return k;   // (a record cut short by the close ends as the close does: its bytes were never given out)
        raw_.append(buf, (size_t)k);
        if (!in_.open(raw_, plain_)) wrong_ = true;   // (what held before it is still given out)
    }
    const size_t take = n < plain_.size() ? n : plain_.size();
    memcpy(p, plain_.data(), take);
    plain_.erase(0, take);
    return (std::ptrdiff_t)take;
}

// --- connections of a program's own, passed on behind the key ---

int connect_to(const std::string &host, uint16_t port, int seconds, std::string *error) {
    const std::string where = host + ":" + std::to_string(port);
    error->clear();
    start();
    addrinfo hints{}, *res = nullptr;
    hints.ai_socktype = SOCK_STREAM;
    if (const int e = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res); e != 0) {
        *error = host + ": " + resolve_error(e);
        return -1;
    }
    int fd = -1;
    for (addrinfo *a = res; a; a = a->ai_next) {
        fd = net::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        if (connect_within(fd, a->ai_addr, a->ai_addrlen, seconds * 1000)) break;
        *error = where + ": " + net::error();
        net::close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0 && error->empty()) *error = where + ": no address to connect to";
    if (fd >= 0) set_timeouts(fd, seconds * 1000);
    return fd;
}

namespace {

// Why a connection couldn't be passed on: said when it is news (a game asks again and again), or
// half a minute after it was last said
void said(const std::string &why) {
    static std::mutex one;
    static std::string last;
    static std::chrono::steady_clock::time_point when;
    std::lock_guard<std::mutex> l(one);
    const auto now = std::chrono::steady_clock::now();
    if (why == last && now - when < std::chrono::seconds(30)) return;
    last = why, when = now;
    fprintf(stderr, "e-AMUSEMENT: %s\n", why.c_str());
}

}  // namespace

bool KeyForward::start(const std::string &host, uint16_t port, const Key &key, std::string *error) {
    net::start();
    const int ls = net::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof sa;
    if (ls < 0 || bind(ls, (sockaddr *)&sa, sizeof sa) != 0 || listen(ls, 16) != 0 || getsockname(ls, (sockaddr *)&sa, &len) != 0) {
        *error = "a port on this computer for the server's key: " + net::error();
        if (ls >= 0) net::close(ls);
        return false;
    }
    port_ = ntohs(sa.sin_port);
    std::thread([ls, host, port, key] {
        for (;;) {
            const int here = net::accept(ls);
            if (here < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            // One connection: two threads, one each way; either's end closes both sockets' traffic,
            // and the sockets themselves go when both threads have
            std::thread([here, host, port, key] {
                std::string why;
                const int there = connect_to(host, port, 15, &why);
                auto k = std::make_shared<Keyed>();
                if (there < 0 || !k->connect(there, key, &why)) {
                    if (there >= 0) why = host + ":" + std::to_string(port) + ": " + why;
                    said(why);
                    if (there >= 0) net::close(there);
                    net::close(here);
                    return;
                }
                set_timeouts(there, 0);   // (a relay's connection is quiet for as long as nobody plays a match)
                std::thread back([here, there, k] {
                    char buf[8192];
                    for (;;) {
                        const ssize_t n = k->recv(buf, sizeof buf);
                        if (n <= 0 || !send_all(here, buf, (size_t)n)) break;
                    }
                    net::shutdown(here);
                    net::shutdown(there);
                });
                char buf[8192];
                for (;;) {
                    const ssize_t n = net::recv(here, buf, sizeof buf);
                    if (n < 0 && interrupted()) continue;
                    if (n <= 0 || !k->send(buf, (size_t)n)) break;
                }
                net::shutdown(there);
                net::shutdown(here);
                back.join();
                net::close(there);
                net::close(here);
            }).detach();
        }
    }).detach();
    return true;
}

}  // namespace hk::net
