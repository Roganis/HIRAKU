// SPDX-License-Identifier: GPL-3.0-or-later
// A connection behind a key (runtime/shell/keyed.h), without a network: the two hellos, records
// each way, and everything someone on the way could do to them (change, leave out, say twice, turn
// round, say again in another connection), which must all end the connection. Then a machine's own
// key (hk2.PCBID.SECRET): its "HKK2" hello, and the server's looking up the machine it names.
//
//   test_keyed      exit 0 when all hold
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include "shell/keyed.h"

#ifndef _WIN32
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace hk::net;

static int failed = 0;
static void check(bool ok, const char *what) {
    printf("%s%s\n", ok ? "ok    " : "FAIL  ", what);
    failed += !ok;
}

// One connection's two ends after their hellos
struct Ends {
    Sealer c_out, s_out;
    Opener c_in, s_in;
    uint8_t hello[keyed::kHello], answer[keyed::kServerHello];
    int taken = -1;
    Ends(const Key &cabinet, const Key &server) {
        const bool ok = keyed::client_hello(hello) && keyed::server_answer(server, hello, answer, s_out, s_in);
        taken = ok ? keyed::client_take(cabinet, hello, answer, c_out, c_in) : -1;
    }
};

int main() {
    const Key key("k7m2x-pq9rs-tuv3w-xyz45-abcde"), other("k7m2x-pq9rs-tuv3w-xyz45-abcdf");
    check(key.set() && !Key("").set() && !Key("  \n").set(), "a key from text; none from an empty one");
    check(memcmp(key.bytes(), Key("  k7m2x-pq9rs-tuv3w-xyz45-abcde\r\n").bytes(), 32) == 0, "the spaces and line end around a key don't count");
    check(memcmp(key.bytes(), other.bytes(), 32) != 0, "another text, another key");
    {
        const std::string a = Key::make(), b = Key::make();
        check(a.size() == 29 && a != b && Key::wrong_with(a).empty(), "a made key: five groups of five, another each time");
        check(!Key::wrong_with("short").empty() && !Key::wrong_with("").empty(), "a key of a few characters is refused");
    }

    Ends e(key, key);
    check(e.taken == 0, "the hellos: same key on both sides, taken");
    check(Ends(other, key).taken == 1, "... a cabinet with another key sees that the server's is another");
    {
        uint8_t junk[keyed::kServerHello] = {'H', 'T', 'T', 'P'};
        Sealer o;
        Opener i;
        check(keyed::client_take(key, e.hello, junk, o, i) == 2, "... an answer that isn't one");
        // the answer of another connection (a recording said again to a new hello)
        Ends later(key, key);
        check(keyed::client_take(key, later.hello, e.answer, o, i) == 1, "... a server's answer recorded from another connection: refused");
    }

    // records each way, and in pieces
    const std::string call = "POST / HTTP/1.0\r\n\r\n<call model=\"C00:J:A:A\" srcid=\"01A1B2C3D4E5F6071899\"><cardmng pass=\"1234\"/></call>";
    std::string wire, plain;
    e.c_out.seal(call.data(), call.size(), wire);
    check(wire.find("01A1B2C3D4E5F6071899") == std::string::npos && wire.find("1234") == std::string::npos && wire.find("call") == std::string::npos,
          "a call sealed: no PCBID, PIN or tag name in what goes over the network");
    {
        std::string raw, got;
        bool ok = true;
        for (char c : wire) {   // a byte at a time
            raw += c;
            ok = ok && e.s_in.open(raw, got);
        }
        check(ok && got == call, "... opened by the server, arriving a byte at a time");
    }
    {
        const std::string big(100000, 'x');
        std::string w, got;
        e.s_out.seal(big.data(), big.size(), w);
        check(w.size() == big.size() + 7 * 18 && e.c_in.open(w, got) && got == big && w.empty(), "100000 bytes the other way: seven records, whole again");
    }

    // what someone on the way could do
    auto fresh = [&](std::string &w1, std::string &w2, Ends &n) {
        w1.clear(), w2.clear();
        n.c_out.seal("first", 5, w1);
        n.c_out.seal("second", 6, w2);
    };
    {
        Ends n(key, key);
        std::string w1, w2, got;
        fresh(w1, w2, n);
        w1[7] ^= 0x10;
        check(!n.s_in.open(w1, got) && got.empty(), "a record with a bit changed: refused, nothing of it given out");
    }
    {
        Ends n(key, key);
        std::string w1, w2, got;
        fresh(w1, w2, n);
        w1[1] ^= 1;   // its count
        w1 += std::string(40, '\0');
        check(!n.s_in.open(w1, got) && got.empty(), "a record whose count was changed: refused");
    }
    {
        Ends n(key, key);
        std::string w1, w2, got;
        fresh(w1, w2, n);
        check(!n.s_in.open(w2, got) && got.empty(), "the second record without the first (one left out): refused");
    }
    {
        Ends n(key, key);
        std::string w1, w2, got;
        fresh(w1, w2, n);
        std::string twice = w1 + w1;
        check(!n.s_in.open(twice, got) && got == "first", "a record said twice: the second time refused");
    }
    {
        Ends n(key, key);
        std::string w1, w2, got;
        fresh(w1, w2, n);
        check(!n.c_in.open(w1, got) && got.empty(), "a cabinet's record turned round to the cabinet: refused");
    }
    {
        Ends a(key, key), b(key, key);
        std::string w1, w2, got;
        fresh(w1, w2, a);
        check(!b.s_in.open(w1, got) && got.empty(), "a record of one connection said in another: refused");
    }
    {
        Ends n(other, key);   // (a cabinet that goes on in spite of what the hello told it)
        std::string w, got;
        n.c_out.seal("call", 4, w);
        check(!n.s_in.open(w, got) && got.empty() && n.s_in.records() == 0, "a cabinet with another key: its first record refused");
    }
    {
        Ends n(key, key);
        std::string w(2, '\xff'), got;   // a count past the limit
        w += std::string(100, 'a');
        check(!n.s_in.open(w, got), "a record longer than records are: refused before any of it is waited for");
    }

    // --- a machine's own key ---
    auto hex = [](const uint8_t *p) {
        std::string s;
        for (int i = 0; i < 32; i++) s += "0123456789abcdef"[p[i] >> 4], s += "0123456789abcdef"[p[i] & 15];
        return s;
    };
    // (both from Python's hashlib.pbkdf2_hmac('sha256', text, b'HIRAKU e-AMUSEMENT key 1', 100000, 32))
    check(hex(key.bytes()) == "cfd70b2fc836632c4224c9ba923b4f85d6b15071e4485ffdcc3f1b485bfd924e" && key.id().empty(),
          "a key for all: the same 32 bytes as ever (PBKDF2 of its text), no id");
    const std::string id = "0120ABCDEF0123456789", text = "hk2." + id + ".k7m2x-pq9rs-tuv3w-xyz45-abcde";
    const Key mine(text), lower("  hk2.0120abcdef0123456789.k7m2x-pq9rs-tuv3w-xyz45-abcde\n");
    check(mine.set() && mine.id() == id && hex(mine.bytes()) == "5da8cceaec74e64b7aae6d28fe6f9b529e44dcdebc3a63cf5245327503a1da67",
          "a machine's key: its PCBID, and PBKDF2 of its whole text");
    check(lower.id() == id && memcmp(lower.bytes(), mine.bytes(), 32) == 0, "... its PCBID in small letters and spaces around it: the same key");
    check(Key::id_in(text) == id && Key::id_in("k7m2x-pq9rs-tuv3w-xyz45-abcde").empty() && Key::id_in("hk2.0120ABCDEF012345678.k7m2x-pq9rs-tuv3w-xyz45-abcde").empty() &&
              Key::id_in("hk2." + id + ".short").empty(),
          "id_in: the PCBID of a machine's key only (20 hex digits, then a secret worth having)");
    check(Key::wrong_with(text).empty() && !Key::wrong_with("hk2.0120ABCDEF.k7m2x-pq9rs-tuv3w-xyz45-abcde").empty(),
          "wrong_with: a machine's key is a key; \"hk2.\" with no PCBID isn't");
    {
        const std::string a = Key::make_for(id), b = Key::make_for(id);
        check(a.size() == 4 + 20 + 1 + 29 && a.compare(0, 25, "hk2." + id + ".") == 0 && a != b && Key(a).id() == id && Key::make_for("12").empty(),
              "make_for: hk2., the PCBID, a key as make()'s; another each time; none for what isn't a PCBID");
    }
    {
        uint8_t h1[keyed::kHello2], h2[keyed::kHello2];
        size_t n1 = 0, n2 = 0;
        check(keyed::client_hello(key, h1, &n1) && n1 == keyed::kHello && memcmp(h1, "HKK1", 4) == 0 && keyed::hello_size(h1) == keyed::kHello,
              "a key for all: the hello is HKK1's, 20 bytes, as before");
        check(keyed::client_hello(mine, h2, &n2) && n2 == keyed::kHello2 && memcmp(h2, "HKK2", 4) == 0 && keyed::hello_size(h2) == keyed::kHello2 &&
                  keyed::is_hello(h2) && keyed::hello_id(h2) == id,
              "a machine's key: HKK2, 30 bytes, its PCBID's 10 bytes at the end");
        check(keyed::hello_size("HKK0") == 0 && keyed::hello_size("HKKS") == 0 && keyed::hello_size("HTTP") == 0, "HKK0, HKKS, HTTP: not hellos");
        const char *refusals[] = {keyed::kNoKey, keyed::kOneKey, keyed::kMachinesOnly, keyed::kUnknownMachine};
        bool distinct = true;
        for (const char *a : refusals) {
            distinct = distinct && keyed::hello_size(a) == 0;
            for (const char *b : refusals) distinct = distinct && (a == b || memcmp(a, b, 4) != 0);
        }
        check(distinct && memcmp(keyed::kUnknownMachine, "HKKU", 4) == 0, "HKKU, a machine not taken: a refusal of its own, not a hello");
#ifndef _WIN32
        // the cabinet's side told each refusal apart (a server's four bytes, then its close)
        auto told = [&](const Key &k, const char *said) {
            int sv[2];
            if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return std::string("no socketpair");
            if (write(sv[1], said, 4) != 4) return std::string("no write");
            shutdown(sv[1], SHUT_WR);
            Keyed c;
            std::string why;
            const bool ok = c.connect(sv[0], k, &why);
            close(sv[0]), close(sv[1]);
            return ok ? std::string("connected") : why;
        };
        const std::string unknown = told(mine, keyed::kUnknownMachine);
        check(unknown.find("doesn't take this machine (unknown or switched off") != std::string::npos && unknown.find("another") == std::string::npos,
              "HKKU: the cabinet says the server doesn't take this machine, not a wrong key");
        check(told(mine, keyed::kMachinesOnly).find("own key only") != std::string::npos && told(mine, keyed::kOneKey).find("one key for all") != std::string::npos &&
                  told(key, keyed::kNoKey).find("no key") != std::string::npos && told(mine, "XXXX").find("gave no answer") != std::string::npos,
              "... HKKM, HKKS, HKK0 and no answer each said as before");
#endif

        // the server's machines: one with the key above, one with another
        const std::string other_id = "0120000000000000AB00";
        std::map<std::string, Key> machines{{id, Key::kept(id, mine.bytes())}, {other_id, Key::kept(other_id, other.bytes())}};
        auto look = [&](const std::string &pcbid, Key &k) {
            const auto it = machines.find(pcbid);
            if (it == machines.end()) return false;
            k = it->second;
            return true;
        };
        auto connect = [&](const Key &cabinet, int *taken) {   // false: the server knows no such machine
            uint8_t hello[keyed::kHello2], answer[keyed::kServerHello];
            size_t n = 0;
            Key k;
            Sealer so, co;
            Opener si, ci;
            if (!keyed::client_hello(cabinet, hello, &n) || !look(keyed::hello_id(hello), k)) return false;
            *taken = keyed::server_answer(k, hello, answer, so, si) ? keyed::client_take(cabinet, hello, answer, co, ci) : -1;
            return true;
        };
        int taken = -1;
        check(Key::kept(id, mine.bytes()).id() == id && connect(mine, &taken) && taken == 0, "HKK2 with the machine's key: the server finds it, the keys are one");
        check(!connect(Key("hk2.0120FFFFFFFFFFFFFFFF.k7m2x-pq9rs-tuv3w-xyz45-abcde"), &taken), "... a PCBID the server has no machine for: nothing to answer with");
        check(connect(Key("hk2." + other_id + ".k7m2x-pq9rs-tuv3w-xyz45-abcde"), &taken) && taken == 1,
              "... a key that names another machine and isn't its: the keys differ");
        machines[id] = Key::kept(id, Key(Key::make_for(id)).bytes());   // the machine given a new key
        check(connect(mine, &taken) && taken == 1, "... the old key of a machine given a new one: the keys differ");
    }
    printf("%s\n", failed ? "FAILED" : "all hold");
    return failed ? 1 : 0;
}
