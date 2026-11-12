// SPDX-License-Identifier: GPL-3.0-or-later
// runtime/shell/crypt.h against its standards' own examples: SHA-256 (FIPS 180-4), HMAC-SHA-256
// (RFC 4231), PBKDF2 (the values every implementation gives for "password"/"salt"), ChaCha20,
// Poly1305 and the two together (RFC 8439, sections 2.3.2, 2.5.2, 2.8.2), and a sealed message
// that was touched.
//
//   test_crypt      exit 0 when all hold
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "shell/crypt.h"

using namespace hk::crypt;

static int failed = 0;

static std::string hex(const uint8_t *p, size_t n) {
    std::string s;
    char b[3];
    for (size_t i = 0; i < n; i++) snprintf(b, sizeof b, "%02x", p[i]), s += b;
    return s;
}
static std::vector<uint8_t> bytes(const char *hex) {
    std::vector<uint8_t> v;
    for (; hex[0] && hex[1]; hex += 2) {
        unsigned x;
        sscanf(hex, "%2x", &x);
        v.push_back((uint8_t)x);
    }
    return v;
}
static void is(const char *what, const std::string &got, const char *want) {
    const bool ok = got == want;
    printf("%s%s\n", ok ? "ok    " : "FAIL  ", what);
    if (!ok) printf("      got  %s\n      want %s\n", got.c_str(), want);
    failed += !ok;
}

int main() {
    uint8_t d[32];
    sha256("abc", 3, d);
    is("SHA-256 of \"abc\"", hex(d, 32), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const char *two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256(two, strlen(two), d);
    is("SHA-256 of two blocks", hex(d, 32), "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    {
        Sha256 s;   // a million "a", in pieces of 7, 64, 1000 by turns
        const std::string a(1000, 'a');
        size_t left = 1000000;
        for (int k = 0; left; k++) {
            const size_t sizes[3] = {7, 64, 1000}, n = sizes[k % 3] < left ? sizes[k % 3] : left;
            s.add(a.data(), n), left -= n;
        }
        s.end(d);
        is("SHA-256 of a million \"a\", in pieces", hex(d, 32), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    }
    const std::vector<uint8_t> k1(20, 0x0b), k6(131, 0xaa);
    hmac_sha256(k1.data(), k1.size(), "Hi There", 8, d);
    is("HMAC, RFC 4231 case 1", hex(d, 32), "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    const char *m6 = "Test Using Larger Than Block-Size Key - Hash Key First";
    hmac_sha256(k6.data(), k6.size(), m6, strlen(m6), d);
    is("HMAC, RFC 4231 case 6 (a key longer than a block)", hex(d, 32), "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
    pbkdf2_sha256("password", "salt", 1, d);
    is("PBKDF2, 1 round", hex(d, 32), "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
    pbkdf2_sha256("password", "salt", 4096, d);
    is("PBKDF2, 4096 rounds", hex(d, 32), "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a");

    {
        uint8_t key[32], out[64];
        for (int i = 0; i < 32; i++) key[i] = (uint8_t)i;
        chacha20_block(key, 1, bytes("000000090000004a00000000").data(), out);
        is("ChaCha20's block, RFC 8439 2.3.2", hex(out, 64),
           "10f1e7e4d13b5915500fdd1fa32071c4c7d1f4c733c068030422aa9ac3d46c4ed2826446079faa0914c2d705d98b02a2b5129cd1de164eb9cbd083e8a2503c4e");
    }
    {
        const auto key = bytes("85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b");
        const char *m = "Cryptographic Forum Research Group";
        uint8_t tag[16];
        poly1305(key.data(), m, strlen(m), tag);
        is("Poly1305, RFC 8439 2.5.2", hex(tag, 16), "a8061dc1305136c6c22b8baf0c0127a9");
    }
    {
        uint8_t key[32];
        for (int i = 0; i < 32; i++) key[i] = (uint8_t)(0x80 + i);
        const auto nonce = bytes("070000004041424344454647"), aad = bytes("50515253c0c1c2c3c4c5c6c7");
        const std::string m = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
        std::vector<uint8_t> sealed(m.size()), back(m.size());
        uint8_t tag[16];
        seal(key, nonce.data(), aad.data(), aad.size(), m.data(), m.size(), sealed.data(), tag);
        is("sealed, RFC 8439 2.8.2", hex(sealed.data(), sealed.size()),
           "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d63dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b"
           "3692ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc3ff4def08e4b7a9de576d26586cec64b6116");
        is("... and its tag", hex(tag, 16), "1ae10b594f09e26a7e902ecbd0600691");
        const bool opened = open(key, nonce.data(), aad.data(), aad.size(), sealed.data(), sealed.size(), tag, back.data());
        is("opened again", opened ? std::string(back.begin(), back.end()) : "(refused)", m.c_str());
        // one bit of anything changed: refused
        int refused = 0;
        sealed[5] ^= 1;
        refused += !open(key, nonce.data(), aad.data(), aad.size(), sealed.data(), sealed.size(), tag, back.data());
        sealed[5] ^= 1, tag[15] ^= 0x80;
        refused += !open(key, nonce.data(), aad.data(), aad.size(), sealed.data(), sealed.size(), tag, back.data());
        tag[15] ^= 0x80;
        auto aad2 = aad;
        aad2[0] ^= 4;
        refused += !open(key, nonce.data(), aad2.data(), aad2.size(), sealed.data(), sealed.size(), tag, back.data());
        auto nonce2 = nonce;
        nonce2[11] ^= 1;
        refused += !open(key, nonce2.data(), aad.data(), aad.size(), sealed.data(), sealed.size(), tag, back.data());
        key[0] ^= 1;
        refused += !open(key, nonce.data(), aad.data(), aad.size(), sealed.data(), sealed.size(), tag, back.data());
        is("a changed byte, tag, clear part, nonce or key: refused", std::to_string(refused), "5");
    }
    {
        // sizes around the blocks' edges seal and open
        uint8_t key[32] = {1, 2, 3}, nonce[12] = {9};
        int ok = 0, all = 0;
        for (size_t n : {0u, 1u, 15u, 16u, 17u, 63u, 64u, 65u, 127u, 128u, 1000u, 16384u}) {
            std::vector<uint8_t> m(n), s(n), b(n);
            for (size_t i = 0; i < n; i++) m[i] = (uint8_t)(i * 7 + n);
            uint8_t tag[16];
            seal(key, nonce, "x", 1, m.data(), n, s.data(), tag);
            all++, ok += open(key, nonce, "x", 1, s.data(), n, tag, b.data()) && b == m && (n < 8 || s != m);
        }
        is("every size sealed and opened", std::to_string(ok), std::to_string(all).c_str());
    }
    {
        uint8_t a[32] = {}, b[32] = {};
        const bool ok = random_bytes(a, 32) && random_bytes(b, 32);
        is("the system's random bytes", ok && memcmp(a, b, 32) != 0 ? "two different lots" : "none, or the same twice", "two different lots");
    }
    printf("%s\n", failed ? "FAILED" : "all hold");
    return failed ? 1 : 0;
}
