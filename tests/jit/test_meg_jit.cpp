// SPDX-License-Identifier: GPL-3.0-or-later
// The SWP30's MEG programs made into the host's code at run time (runtime/dev/xt446_meg.cpp, the
// stencils of runtime/jit/meg_stencils.c) against the interpreter they stand in for, with no ROM:
// programs and states drawn at random, every register, delay slot, flag, the delay memory and the
// random number compared after three samples (Xt446::meg_selftest). A build with no stencils for
// its processor has nothing to compare and says so.
//
//   test_meg_jit [ROUNDS [FIRST_SEED]]     (500 rounds for each of four seeds)
#include <cstdio>
#include <cstdlib>
#include <string>

#include "dev/xt446.h"

int main(int argc, char **argv) {
    const int rounds = argc > 1 ? atoi(argv[1]) : 500;
    const unsigned first = argc > 2 ? (unsigned)strtoul(argv[2], nullptr, 0) : 1;
    for (unsigned seed = first; seed < first + 4; seed++) {
        const std::string r = hk::Xt446::meg_selftest(seed, rounds);
        if (r.rfind("skip", 0) == 0) {
            printf("test_meg_jit: %s\n", r.c_str());
            return 0;
        }
        if (!r.empty()) {
            printf("test_meg_jit: FAILED: %s\n", r.c_str());
            return 1;
        }
    }
    printf("test_meg_jit: %d programs, the made code and the interpreter agree\n", 4 * rounds);
    return 0;
}
