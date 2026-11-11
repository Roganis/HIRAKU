// SPDX-License-Identifier: GPL-3.0-or-later
// The waiting loops' arithmetic (runtime/sys573/idle_model.h) against the plain thing: a value
// v + k * s modulo 2^32 on every turn k, compared as the R3000 compares. For each pair of values
// the count of turns a comparison is said to keep its result must never be more than it does:
// every turn below the count (all of them where the count is small, the last ones and a sample
// where it is large) is worked out and compared with turn 0's.
#include <cstdint>
#include <cstdio>

#include "sys573/idle_model.h"

using namespace hk::k573::mips;

namespace {

uint64_t g_rng = 0x9e3779b97f4a7c15ull;
uint32_t rnd() {
    g_rng ^= g_rng << 13, g_rng ^= g_rng >> 7, g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 16);
}
uint32_t at(AV a, uint64_t k) { return a.v + (uint32_t)k * a.s; }   // (the product modulo 2^32)

// values around the edges where a comparison flips or a value wraps, and anywhere
uint32_t value() {
    static const uint32_t edge[] = {0, 1, 2, 0x7fffffff, 0x80000000, 0x80000001, 0xffffffff, 0xfffffffe, 0x1000, 0x00800000};
    const uint32_t r = rnd();
    if (r % 4 == 0) return rnd();
    return edge[r % 10] + (int32_t)(rnd() % 4001) - 2000;
}
uint32_t step() {
    static const int32_t s[] = {0, 0, 1, -1, 1, -1, 2, -2, 3, -3, 4, -4, 7, -8, 0x1000, -0x1000, 0x10000000, -0x10000000,
                                0x7fffffff, INT32_MIN};
    const uint32_t r = rnd();
    return r % 16 == 0 ? rnd() : (uint32_t)s[r % 20];
}

int g_failed = 0;
template <typename F> void check(const char *what, AV a, AV b, uint64_t turns, F outcome) {
    if (turns == 0) {
        printf("%s: 0 turns for %08x+%08x k, %08x+%08x k (turn 0 always has its own result)\n", what, a.v, a.s, b.v, b.s);
        g_failed++;
        return;
    }
    const bool first = outcome(at(a, 0), at(b, 0));
    auto turn = [&](uint64_t k) {
        if (outcome(at(a, k), at(b, k)) == first) return true;
        printf("%s: %08x+%08x k against %08x+%08x k said to hold %llu turns, differs on turn %llu\n", what, a.v, a.s, b.v, b.s,
               (unsigned long long)turns, (unsigned long long)k);
        g_failed++;
        return false;
    };
    const uint64_t all = turns < 3000 ? turns : 3000;
    for (uint64_t k = 0; k < all; k++)
        if (!turn(k)) return;
    for (uint64_t k = turns > 3000 ? turns - 3000 : 0; k < turns; k++)   // the last ones before it may change
        if (!turn(k)) return;
    for (int i = 0; i < 2000; i++)                                        // and anywhere between
        if (!turn(((uint64_t)rnd() << 16 ^ rnd()) % turns)) return;
}

}  // namespace

int main() {
    for (int i = 0; i < 300000 && g_failed < 20; i++) {
        AV a{value(), step()}, b{value(), step()};
        if (i % 3 == 0) b.v = a.v + (int32_t)(rnd() % 41) - 20;   // close together: the flip is near
        if (i % 5 == 0) b.s = 0;                                   // against a value that stays (the usual case)
        check("equal", a, b, stable_eq(a, b), [](uint32_t x, uint32_t y) { return x == y; });
        check("less than (signed)", a, b, stable_lt(a, b, true), [](uint32_t x, uint32_t y) { return (int32_t)x < (int32_t)y; });
        check("less than (unsigned)", a, b, stable_lt(a, b, false), [](uint32_t x, uint32_t y) { return x < y; });
    }
    // the loop the model was written for: a timeout counted down against -1, from any start
    for (uint32_t m : {1u, 2u, 100u, 0x7fffffffu, 0x80000000u, 0xfffffffeu}) {
        const AV a{m, 0xffffffffu}, b{0xffffffffu, 0};
        if (stable_eq(a, b) != (uint64_t)m + 1) {
            printf("a count down from %u against -1: %llu turns, not %llu\n", m, (unsigned long long)stable_eq(a, b), (unsigned long long)m + 1);
            g_failed++;
        }
    }
    if (g_failed) return 1;
    printf("waiting loops' arithmetic: 900,000 comparisons, none said to hold longer than it does\n");
    return 0;
}
