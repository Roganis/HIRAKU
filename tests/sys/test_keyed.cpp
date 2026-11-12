// SPDX-License-Identifier: GPL-3.0-or-later
// A connection behind a key (runtime/shell/keyed.h), without a network: the two hellos, records
// each way, and everything someone on the way could do to them (change, leave out, say twice, turn
// round, say again in another connection), which must all end the connection.
//
//   test_keyed      exit 0 when all hold
#include <cstdio>
#include <cstring>
#include <string>

#include "shell/keyed.h"

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
    printf("%s\n", failed ? "FAILED" : "all hold");
    return failed ? 1 : 0;
}
