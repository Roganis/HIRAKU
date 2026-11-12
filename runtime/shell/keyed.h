// SPDX-License-Identifier: GPL-3.0-or-later
// A TCP connection behind a key: what a cabinet and hiraku-eamuse say to each other (the e-AMUSE3
// calls, NET TAISEN's relay) when the server was given a key, so that who listens on the way reads
// nothing (a PCBID, a card's number, a PIN) and who hasn't the key can say nothing: not a call, not
// a datagram into a match. e-AMUSE3's own cipher is no help there: its key is every copy's.
//
// The key is a piece of text both sides were given (the server: key.txt in its data folder,
// --new-key; the cabinet: -eakey, HK_EA_KEY, the launcher's setting). Over the connection:
//
//   cabinet  "HKK1", 16 bytes by chance
//   server   "HKK1", 16 bytes by chance, an empty record        (or "HKK0": I have no key)
//   then records each way: the bytes' count (2 bytes, 16384 at most), the bytes sealed, a 16-byte tag
//
// Each direction has a key of its own, made from the text and both sides' 16 bytes (HMAC-SHA-256),
// so nothing recorded from one connection means anything in another; a record is sealed with
// ChaCha20-Poly1305 (RFC 8439) under its number in its direction, so none can be changed, left out,
// said twice or turned round. The server's empty record tells the cabinet the keys are one; the
// cabinet's first record tells the server (nothing it sends is read before its tag holds).
// The text itself is stretched (PBKDF2, kRounds) so that each guess at a weak one costs.
//
// Not here: a new key for each connection that outlives the text being found out later (no
// Diffie-Hellman: who records the traffic and later learns the text reads it), and hiding who talks
// to whom or how much. For more than a key among friends, a VPN under it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// (no system header here, shell/net.h's neither: this one is seen from the boards' own files, where
// Windows' names and the emulated machines' meet)
namespace hk::net {

// The key as both sides hold it: 32 bytes made from the text (slowly, once)
class Key {
public:
    Key() = default;
    // text: as given, less the spaces around it. "" makes no key
    explicit Key(const std::string &text);
    bool set() const { return set_; }
    const uint8_t *bytes() const { return k_; }
    // A new key's text, nobody's guess: five groups of five ("k7m2x-..."); "" if the system gives
    // no random bytes
    static std::string make();
    // What is wrong with a text as a key ("" if nothing): too short to be worth having
    static std::string wrong_with(const std::string &text);
    static constexpr uint32_t kRounds = 100000;

private:
    uint8_t k_[32] = {};
    bool set_ = false;
};

// One direction's records. Sealer: bytes into records. Opener: records, as they come off the
// wire in pieces, into bytes.
class Sealer {
public:
    void start(const uint8_t key[32]);
    // n bytes (any count, none too: an empty record) as records, added to wire
    void seal(const void *p, size_t n, std::string &wire);

private:
    uint8_t key_[32] = {};
    uint64_t count_ = 0;
};
class Opener {
public:
    void start(const uint8_t key[32]);
    // Every whole record at raw's front is taken off it and its bytes added to plain. False: one
    // wasn't sealed with the key at its place in the order (the connection is to be dropped)
    bool open(std::string &raw, std::string &plain);
    uint64_t records() const { return count_; }

private:
    uint8_t key_[32] = {};
    uint64_t count_ = 0;
};

namespace keyed {
constexpr size_t kHello = 20;          // "HKK1" and 16 bytes
constexpr size_t kServerHello = 38;    // ... and the empty record
constexpr size_t kRecord = 16384;      // a record's bytes at most
bool is_hello(const void *first4);     // "HKK1": a connection that wants the key
extern const char kNoKey[4];           // "HKK0": a server without one says so, and closes
// The cabinet's hello; false if the system gives no random bytes
bool client_hello(uint8_t hello[kHello]);
// The server's answer to a hello (which is_hello), and its two directions started; false as above
bool server_answer(const Key &key, const uint8_t hello[kHello], uint8_t answer[kServerHello], Sealer &out, Opener &in);
// The cabinet's reading of that answer: 0 the keys are one (directions started), 1 the server's
// key is another, 2 it isn't such an answer
int client_take(const Key &key, const uint8_t hello[kHello], const uint8_t answer[kServerHello], Sealer &out, Opener &in);
}  // namespace keyed

// A connected socket behind the key, for code that reads and writes it in the plain blocking way
// (the socket's own timeouts hold). Sending and receiving may be two threads'.
class Keyed {
public:
    // The cabinet's side, on a socket just connected. False: *error says what ("the server has
    // another key", "the server has no key", ...)
    bool connect(int fd, const Key &key, std::string *error);
    // The server's side, the connection's first four bytes already read (and is_hello). False:
    // the rest of the hello didn't come
    bool accept(int fd, const Key &key, const uint8_t first4[4]);
    bool send(const void *p, size_t n);     // all of it, or false
    // As recv: some bytes (n at most), 0 at the other side's close, -1 at an error; a record that
    // isn't the key's is an error, and wrong() says it was that
    std::ptrdiff_t recv(void *p, size_t n);
    bool wrong() const { return wrong_; }
    // true once a record of the other side's has held (a server: the cabinet has the key)
    bool proven() const { return in_.records() > 0; }

private:
    int fd_ = -1;
    Sealer out_;
    Opener in_;
    std::string raw_, plain_;
    bool wrong_ = false;
};

// For a program that makes its own connections and can't be given the key (a PC game's network
// library, in its own process): a port on this computer whose connections are passed on, behind the
// key, to the server's. Each connection there is one here; closing one closes the other.
class KeyForward {
public:
    // Listens on 127.0.0.1 (any free port) for host:port. false: *error
    bool start(const std::string &host, uint16_t port, const Key &key, std::string *error);
    uint16_t port() const { return port_; }   // on this computer

private:
    uint16_t port_ = 0;
};

// A TCP connection to host:port within `seconds` (its sends and receives limited to as long), or
// -1 with *error
int connect_to(const std::string &host, uint16_t port, int seconds, std::string *error);

}  // namespace hk::net
