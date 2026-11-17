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
//            (or "HKK2", 16 bytes by chance, the machine's PCBID's 10 bytes: a machine's own key)
//   server   "HKK1", 16 bytes by chance, an empty record        (or "HKK0": I have no key; four
//            bytes and a close for the other refusals: "HKKS", "HKKM", "HKKU" below)
//   then records each way: the bytes' count (2 bytes, 16384 at most), the bytes sealed, a 16-byte tag
//
// Each direction has a key of its own, made from the text and both sides' 16 bytes (HMAC-SHA-256),
// so nothing recorded from one connection means anything in another; a record is sealed with
// ChaCha20-Poly1305 (RFC 8439) under its number in its direction, so none can be changed, left out,
// said twice or turned round. The server's empty record tells the cabinet the keys are one; the
// cabinet's first record tells the server (nothing it sends is read before its tag holds): Keyed's
// cabinet side sends an empty one at once, so a connection closed without one had another key.
// The text itself is stretched (PBKDF2, kRounds) so that each guess at a weak one costs.
//
// A key for one machine (hiraku-server's, machine add / machine key): "hk2.PCBID.SECRET", the
// PCBID its 20 hex digits, the secret as Key::make()'s. Its cabinet says "HKK2" with the PCBID, and
// the server looks that machine's key up (the stretched text, kept in its database); all the rest is
// HKK1's. Such a key also is the cabinet's PCBID (shell/eaoptions.h), and the server takes from that
// connection only calls whose srcid is that PCBID.
//
// Not here: a new key for each connection that outlives the text being found out later (no
// Diffie-Hellman: who records the traffic and later learns the text reads it), and hiding who talks
// to whom or how much. For more than a key among friends, a VPN under it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

// (no system header here, shell/net.h's neither: this one is seen from the boards' own files, where
// Windows' names and the emulated machines' meet)
namespace hk::net {

// The key as both sides hold it: 32 bytes made from the text (slowly, once)
class Key {
public:
    Key() = default;
    // text: as given, less the spaces around it. "" makes no key. "hk2.PCBID.SECRET": a machine's
    explicit Key(const std::string &text);
    // A machine's key as a server keeps it: its PCBID and the 32 bytes its text makes (bytes())
    static Key kept(const std::string &pcbid, const uint8_t bytes[32]);
    bool set() const { return set_; }
    const uint8_t *bytes() const { return k_; }
    // A machine's key: its PCBID (20 hex digits, in capitals); "" a key for all (or none)
    const std::string &id() const { return id_; }
    // A new key's text, nobody's guess: five groups of five ("k7m2x-..."); "" if the system gives
    // no random bytes
    static std::string make();
    // ... and a new key for one machine: "hk2.PCBID." and such a text ("" as above, or no PCBID)
    static std::string make_for(const std::string &pcbid);
    // The PCBID a machine's key names (in capitals); "" if the text isn't one
    static std::string id_in(const std::string &text);
    // What is wrong with a text as a key ("" if nothing): too short to be worth having, or "hk2."
    // and not a machine's key
    static std::string wrong_with(const std::string &text);
    static constexpr uint32_t kRounds = 100000;

private:
    uint8_t k_[32] = {};
    bool set_ = false;
    std::string id_;
};

// A server's machines' keys: the key of the machine with this PCBID (true), or none (false: no such
// machine, a disabled one, one without a key)
using MachineKeys = std::function<bool(const std::string &pcbid, Key &key)>;

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
constexpr size_t kHello2 = 30;         // "HKK2", 16 bytes and a machine's PCBID (10 bytes)
constexpr size_t kServerHello = 38;    // "HKK1", 16 bytes and the empty record
constexpr size_t kRecord = 16384;      // a record's bytes at most
bool is_hello(const void *first4);     // "HKK1" or "HKK2": a connection that wants a key
size_t hello_size(const void *first4); // kHello, kHello2, or 0: not a hello
extern const char kNoKey[4];           // "HKK0": a server without one says so, and closes
extern const char kOneKey[4];          // "HKKS": a server with one key for all, to a machine's key
extern const char kMachinesOnly[4];    // "HKKM": a server that takes machines' keys only, to another
extern const char kUnknownMachine[4];  // "HKKU": a machine's key ("HKK2") whose PCBID the server doesn't
                                       // take (no such machine, a disabled one, one without a key)
// The cabinet's hello ("HKK1"); false if the system gives no random bytes
bool client_hello(uint8_t hello[kHello]);
// ... with key's: "HKK2" (*size kHello2) for a machine's key, else "HKK1" (kHello)
bool client_hello(const Key &key, uint8_t hello[kHello2], size_t *size);
// The PCBID in an "HKK2" hello (kHello2 bytes): 20 hex digits
std::string hello_id(const uint8_t *hello);
// The server's answer to a hello (which is_hello, either), and its two directions started; false as above
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
    // The server's side, the connection's first four bytes already read (and is_hello): the rest
    // of the hello read (hear; false: it didn't come), its machine's PCBID then id() ("" an HKK1
    // hello), and the answer sent with the key for it (answer; false: not sent). accept: both
    bool accept(int fd, const Key &key, const uint8_t first4[4]);
    bool hear(int fd, const uint8_t first4[4]);
    bool answer(const Key &key);
    const std::string &id() const { return id_; }
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
    uint8_t hello_[keyed::kHello2] = {};
    std::string id_;
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
