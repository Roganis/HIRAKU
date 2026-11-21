// SPDX-License-Identifier: GPL-3.0-or-later
// The XT446's H8S/2655 made into the host's code at run time (runtime/dev/xt446_h8jit.cpp, the
// stencils tools/h8jitgen.py writes from MAME's h8.lst) against MAME's interpreter, with no ROM:
// two cards run the same made-up program (random instructions of every kind made into code, in a
// loop, with calls, branches, a timer interrupting at a random period and bytes arriving on the
// serial port; tests/jit has no game code), one with made code, and their processors' registers,
// flags, place, cycles and pending interrupts are compared after every sample, RAM after every
// 16th (Xt446::h8_selftest). A build with no stencils for its processor says so.
//
//   test_h8_jit [PROGRAMS [SAMPLES [FIRST_SEED]]]     (48 programs of 3000 samples)
#include <cstdio>
#include <cstdlib>
#include <string>

#include "dev/xt446.h"

int main(int argc, char **argv) {
    const int programs = argc > 1 ? atoi(argv[1]) : 48;
    const int samples = argc > 2 ? atoi(argv[2]) : 3000;
    const unsigned first = argc > 3 ? (unsigned)strtoul(argv[3], nullptr, 0) : 1;
    const std::string r = hk::Xt446::h8_selftest(first, programs, samples);
    if (r.rfind("skip", 0) == 0) {
        printf("test_h8_jit: %s\n", r.c_str());
        return 0;
    }
    if (!r.empty()) {
        printf("test_h8_jit: FAILED: %s\n", r.c_str());
        return 1;
    }
    printf("test_h8_jit: %d programs of %d samples, the made code and the interpreter agree\n", programs, samples);
    return 0;
}
