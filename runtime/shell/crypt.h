// SPDX-License-Identifier: GPL-3.0-or-later
// The few standard pieces a connection behind a key is made of (shell/keyed.h), written out here so
// that no library comes into the build for them: SHA-256 (FIPS 180-4), HMAC (RFC 2104), PBKDF2
// (RFC 8018), and ChaCha20 with Poly1305 as RFC 8439 puts them together. Nothing of HIRAKU's own
// invention: tests/sys/test_crypt.cpp holds each to its standard's own examples.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace hk::crypt {

struct Sha256 {
    Sha256();
    void add(const void *p, size_t n);
    void end(uint8_t out[32]);

private:
    void block(const uint8_t *p);
    uint32_t h_[8];
    uint8_t buf_[64] = {};
    uint64_t bytes_ = 0;
};
void sha256(const void *p, size_t n, uint8_t out[32]);
void hmac_sha256(const uint8_t *key, size_t key_n, const void *p, size_t n, uint8_t out[32]);
// 32 bytes from a password, `rounds` HMACs deep (what makes each guess at the password cost)
void pbkdf2_sha256(const std::string &password, const std::string &salt, uint32_t rounds, uint8_t out[32]);

// The keystream's block `counter` for a key and a 12-byte nonce
void chacha20_block(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], uint8_t out[64]);
void poly1305(const uint8_t key[32], const void *p, size_t n, uint8_t tag[16]);

// RFC 8439's AEAD. seal: n bytes of p into out (n bytes) and their 16-byte tag, which also covers
// `aad` (bytes sent in clear). open: false, and nothing in out to be used, if the tag isn't theirs
void seal(const uint8_t key[32], const uint8_t nonce[12], const void *aad, size_t aad_n, const void *p, size_t n, uint8_t *out,
          uint8_t tag[16]);
bool open(const uint8_t key[32], const uint8_t nonce[12], const void *aad, size_t aad_n, const void *p, size_t n,
          const uint8_t tag[16], uint8_t *out);

// n bytes nobody can guess, from the system (false if it gives none: nothing is made up in their place)
bool random_bytes(void *p, size_t n);

}  // namespace hk::crypt
