// SPDX-License-Identifier: GPL-3.0-or-later
// Code for the player's processor, made while the game runs (PLAN.md section 45). A release has
// nothing of a game in it, so what a development build compiles ahead from a game's own programs
// (the MEG's effect programs, the PlayStation's blocks, Firebeat's functions) is not there; this
// makes it on the player's machine instead, from the program as the player's own files hold it.
//
// The method is copy and patch. The steps a program can be made of are written once as small C
// functions, the stencils (stencil.h), which HIRAKU's own compiler turns into machine code when
// HIRAKU is built (tools/stencilgen.py makes tables of them). At run time a program is those
// pieces copied one after another with their holes filled in: the offsets of what each works on,
// small constants. Nothing is assembled here and nothing is compiled on the player's machine, and
// what a stencil does is what its C says, the same C the interpreter's step is written in.
//
// A stencil works on a state (s) and three values kept in registers from one stencil to the next
// (stencil.h). Code leaves only by returning from its last stencil.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// How code made here is called, and how it calls out: one convention on every x86-64 system (the
// stencils are compiled once for all of them), the platform's elsewhere
#if defined(__x86_64__)
#define HK_JIT_ABI __attribute__((sysv_abi))
#else
#define HK_JIT_ABI
#endif

// On the functions that enter made code: UBSan's function check reads a type mark in front of
// whatever is called through a pointer, and made code has none (nor readable memory there)
#if defined(__clang__)
#define HK_JIT_ENTERS __attribute__((no_sanitize("function")))
#else
#define HK_JIT_ENTERS
#endif

namespace hk::jit {

// a hole's place in a stencil and how its value is written there
enum Kind : uint8_t {
    ABS32,    // 32 bits, a number (zero-extended by the instruction)
    ABS32S,   // 32 bits, an offset (sign-extended by the instruction)
    REL32,    // 32 bits: the next stencil's distance from here (a jump to it)
};
struct Reloc {
    uint16_t at;      // from the stencil's start
    Kind kind;
    uint8_t hole;     // which of the stencil's holes (ABS32, ABS32S)
    int32_t addend;
};
struct Stencil {
    uint32_t code;    // its bytes' place in the set's
    uint16_t size;
    uint8_t tail;     // the bytes of a last jump to the next stencil, not copied
    uint16_t relocs, nrelocs;
};
struct Set {
    const unsigned char *code;
    const Reloc *relocs;
    const Stencil *stencils;
    int count;
};

// what a made program is entered as: the state and the three values, the first of them returned
using Entry = int64_t (HK_JIT_ABI *)(char *s, int64_t p, int64_t x, int64_t y);

// A program being put together
class Code {
public:
    explicit Code(const Set &set) : set_(set) {}
    // the stencil `st` next, its holes given these values
    void put(int st, int64_t h0 = 0, int64_t h1 = 0, int64_t h2 = 0, int64_t h3 = 0);
    size_t size() const { return bytes_.size(); }   // where the next stencil will start: a place to enter at
    bool ok() const { return ok_; }                 // false: a value did not fit its hole
    const std::vector<unsigned char> &bytes() const { return bytes_; }

private:
    const Set &set_;
    std::vector<unsigned char> bytes_;
    bool ok_ = true;
};

// Memory the processor may run, holding `code`; nullptr where the system gives none (then the
// caller keeps interpreting). Never written again once it can run.
void *executable(const std::vector<unsigned char> &code);
void release(void *mem, size_t size);

// False with HK_JIT=0 in the environment: every user then interprets, as where this processor
// has no stencils. For comparing the two and for ruling the generator out of a problem.
bool wanted();

}  // namespace hk::jit
