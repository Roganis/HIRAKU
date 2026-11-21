// SPDX-License-Identifier: GPL-3.0-or-later
// The XT446 card's H8S/2655 made into host code as it runs (xt446_h8jit.cpp, PLAN.md section 45).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

class h8_device;

namespace hk::h8jit {

// Made code from now on for this CPU's program in ROM, where this build has stencils and neither
// HK_JIT=0 nor HK_H8_JIT=0 is set (false: it stays interpreted). After the machine has started.
bool attach(h8_device &cpu);
// "" or a line of counts (HK_H8_STATS=1)
std::string stats(h8_device &cpu);
void detach(h8_device &cpu);
uint64_t made_cycles(h8_device &cpu);   // the cycles made code has run

// A made-up program for the card's ROM (tests/jit/test_h8_jit.cpp): random instructions of the
// kinds made into code, in a loop, with a timer interrupting and the serial port receiving
std::vector<uint8_t> test_program(unsigned seed);

// The two CPUs' registers, flags, where they are and their cycles: "" when equal, else what differs
std::string compare(h8_device &a, h8_device &b);

}  // namespace hk::h8jit
