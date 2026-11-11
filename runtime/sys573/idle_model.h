// SPDX-License-Identifier: GPL-3.0-or-later
// The arithmetic of the waiting loops' model (runtime/sys573/idle.cpp), apart so that it can be
// tested on its own (tests/sys573/test_idle_model.cpp): a value as it is on turn k of a loop,
// v + k * s modulo 2^32, and for how many turns a comparison of two such values keeps the result
// it has on turn 0. A count may be short of the truth (the loop's remaining turns are then run by
// the interpreter), never over it.
#pragma once

#include <algorithm>
#include <cstdint>

namespace hk::k573::mips {

constexpr uint64_t FOREVER = 1ull << 40;   // more turns than a timeslice has instructions

struct AV { uint32_t v, s; };

// for how many turns from now (k = 0, 1, ...) a comparison of two such values keeps the result it
// has on turn 0
inline uint64_t stable_eq(AV a, AV b) {
    const uint32_t d0 = a.v - b.v;
    const int32_t ds = (int32_t)(a.s - b.s);
    if (ds == 0) return FOREVER;
    if (d0 == 0) return 1;
    // unequal until d0 + k * ds reaches a multiple of 2^32
    if (ds > 0) return ((1ull << 32) - d0 + (uint64_t)ds - 1) / (uint64_t)ds;
    return ((uint64_t)d0 + (uint64_t)(-(int64_t)ds) - 1) / (uint64_t)(-(int64_t)ds);
}
// turns for which v + k * s stays inside [lo, hi] (no wrap-around)
inline uint64_t in_range(int64_t v, int64_t s, int64_t lo, int64_t hi) {
    if (s == 0) return FOREVER;
    return (uint64_t)(s > 0 ? (hi - v) / s : (v - lo) / -s) + 1;
}
inline uint64_t stable_lt(AV a, AV b, bool is_signed) {
    const int64_t a0 = is_signed ? (int64_t)(int32_t)a.v : (int64_t)a.v, b0 = is_signed ? (int64_t)(int32_t)b.v : (int64_t)b.v;
    const int64_t sa = (int32_t)a.s, sb = (int32_t)b.s;
    const int64_t lo = is_signed ? INT32_MIN : 0, hi = is_signed ? INT32_MAX : (int64_t)UINT32_MAX;
    uint64_t k = std::min(in_range(a0, sa, lo, hi), in_range(b0, sb, lo, hi));
    const int64_t f0 = a0 - b0, df = sa - sb;
    if (f0 < 0) {   // true while f0 + k * df < 0
        if (df > 0) k = std::min<uint64_t>(k, (uint64_t)((-f0 + df - 1) / df));
    } else {        // false while f0 + k * df >= 0
        if (df < 0) k = std::min<uint64_t>(k, (uint64_t)(f0 / -df) + 1);
    }
    return k;
}

}  // namespace hk::k573::mips
