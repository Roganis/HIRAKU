// SPDX-License-Identifier: GPL-3.0-or-later
// Remembered runs (runtime/core/exec.cpp) with a mod's code patch (core/patched.cpp), on a made-up
// program: A and H are the functions patched, B calls A, C calls A through ctr, I calls H, D calls
// nothing. Each is called as an exception's handler is, a dozen times from the same state, and
// each call must give what a plain call (the steps, the patched functions interpreted) gives from
// that state: before the patch, with it, with it lifted and put back, and after a second function
// is patched later. A remembered run of B or C from before A's patch must not be taken after it;
// D and I go on being taken from memory while only A is patched. ctest runs it twice: as is, and
// with HK_EXEC_MEMO=check.
//
//   test_exec_patch <scratch folder>      exit 0 when all hold
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/exec.h"
#include "core/runtime.h"
#include "ppc/spec.h"

using namespace hk;

namespace {

int failed = 0;
void check(bool ok, const std::string &what) {
    printf("%s%s\n", ok ? "ok    " : "FAIL  ", what.c_str());
    failed += !ok;
}

constexpr uint32_t BASE = 0x80100000, A = BASE, B = BASE + 0x20, C = BASE + 0x40, D = BASE + 0x80, H = BASE + 0xa0,
                   I = BASE + 0xc0, OUT = 0x80180000, STACK = 0x80170000, BACK = 0x80100ff0;
// A: li r4, 1; stw r4, 0(r5); blr                     (patched: li r4, 2)
// B: a frame; bl A; return
// C: a frame; r12 = A; mtctr r12; bctrl; return
// D: li r7, 9; stw r7, 4(r5); blr
// H: li r9, 1; stw r9, 8(r5); blr                     (patched: li r9, 3)
// I: a frame; bl H; return
constexpr uint32_t LI_R4_1 = 0x38800001, LI_R4_2 = 0x38800002, LI_R9_1 = 0x39200001, LI_R9_3 = 0x39200003;
const uint32_t g_words[] = {
    LI_R4_1, 0x90850000, 0x4e800020, 0, 0, 0, 0, 0,
    0x7c0802a6, 0x9421fff0, 0x90010014, 0x4bffffd5, 0x80010014, 0x38210010, 0x7c0803a6, 0x4e800020,
    0x7c0802a6, 0x9421fff0, 0x90010014, 0x3d808010, 0x618c0000, 0x7d8903a6, 0x4e800421, 0x80010014,
    0x38210010, 0x7c0803a6, 0x4e800020, 0, 0, 0, 0, 0,
    0x38e00009, 0x90e50004, 0x4e800020, 0, 0, 0, 0, 0,
    LI_R9_1, 0x91250008, 0x4e800020, 0, 0, 0, 0, 0,
    0x7c0802a6, 0x9421fff0, 0x90010014, 0x4bffffd5, 0x80010014, 0x38210010, 0x7c0803a6, 0x4e800020,
};
const char *g_spec =
    "base 80100000\n"
    "func 80100000\nrange 80100000 8010000c\nend\n"
    "func 80100020\nrange 80100020 80100040\nend\n"
    "func 80100040\nrange 80100040 8010006c\nend\n"
    "func 80100080\nrange 80100080 8010008c\nend\n"
    "func 801000a0\nrange 801000a0 801000ac\nend\n"
    "func 801000c0\nrange 801000c0 801000e0\nend\n";

uint32_t ram32(uint32_t a) {
    uint32_t v;
    memcpy(&v, hk_ram + HK_PHYS(a), 4);
    return __builtin_bswap32(v);
}
void put32(uint32_t a, uint32_t w) {
    const uint32_t be = __builtin_bswap32(w);
    memcpy(hk_ram + HK_PHYS(a), &be, 4);
}

// (pc: the handler, as run_vector has it; remembered runs are looked up by it)
hk_ctx fresh(uint32_t f) {
    hk_ctx c{};
    c.pc = f;
    c.r[1] = STACK;
    c.r[3] = 0x77;
    c.r[5] = OUT;
    c.lr = BACK;   // where the call returns to (nothing runs there)
    hk_budget = 1 << 30;
    memset(hk_ram + HK_PHYS(OUT), 0, 12);
    return c;
}

// what a call leaves that the test looks at
struct Result {
    uint32_t r4, r7, r9, pc, out[3];
    bool operator==(const Result &) const = default;
};
Result result(const hk_ctx &c) { return {c.r[4], c.r[7], c.r[9], c.pc, {ram32(OUT), ram32(OUT + 4), ram32(OUT + 8)}}; }

// a plain call from the fresh state, then a dozen as a handler: all the same as the plain one, and
// `word` (the word at `at` in the output) what the code there now writes
bool same_as_plain(uint32_t f, uint32_t at, uint32_t word) {
    hk_ctx c = fresh(f);
    exec::call(&c, f);
    const Result want = result(c);
    bool ok = want.pc == BACK && ram32(at) == word;
    for (int k = 0; k < 12; k++) {
        c = fresh(f);
        if (!exec::call_memo(&c, f)) exec::call(&c, f);
        ok &= result(c) == want;
    }
    return ok;
}

// the runs taken from memory over a dozen handler calls of f
uint64_t taken(uint32_t f) {
    const uint64_t before = exec::memo_used();
    for (int k = 0; k < 12; k++) {
        hk_ctx c = fresh(f);
        if (!exec::call_memo(&c, f)) exec::call(&c, f);
    }
    return exec::memo_used() - before;
}

// a patch put in or lifted, as a mod's is (host/mods.cpp: the bytes, then Core::code_changed)
void code(uint32_t at, uint32_t word) {
    put32(at, word);
    patch_functions(*exec::program(), at, at + 4);
}

}  // namespace

int main(int argc, char **argv) {
    const std::string dir = argc > 1 ? argv[1] : "exec-patch-test";
    std::filesystem::create_directories(dir);
    const std::string image = dir + "/program.bin";
    {
        std::ofstream f(image, std::ios::binary);
        for (uint32_t w : g_words) {
            const uint32_t be = __builtin_bswap32(w);
            f.write(reinterpret_cast<const char *>(&be), 4);
        }
    }
    ram_init();
    for (size_t k = 0; k < sizeof g_words / 4; k++) put32(BASE + 4 * (uint32_t)k, g_words[k]);
    check(exec::load_text(g_spec, image).empty(), "the made-up program loads");

    // no patch: every handler's runs remembered and taken
    check(same_as_plain(B, OUT, 1) && same_as_plain(C, OUT, 1) && same_as_plain(D, OUT + 4, 9) && same_as_plain(I, OUT + 8, 1),
          "no patch: B, C, D and I as handlers give what plain calls give");
    const uint64_t b0 = taken(B), c0 = taken(C), d0 = taken(D), i0 = taken(I);
    check(b0 && c0 && d0 && i0, "no patch: runs taken from memory (B " + std::to_string(b0) + ", C " + std::to_string(c0) + ", D " +
                                    std::to_string(d0) + ", I " + std::to_string(i0) + " of 12)");

    // A patched: B's and C's remembered runs (A's old code in them) no longer taken
    code(A, LI_R4_2);
    check(patched(A) && !patched(B) && !patched(D), "A patched: A marked, B and D not");
    check(same_as_plain(B, OUT, 2), "A patched: B (a remembered caller of A) gives the patched A's result");
    check(same_as_plain(C, OUT, 2), "A patched: C (calling A through ctr) gives the patched A's result");
    check(same_as_plain(A, OUT, 2), "A patched: A itself as a handler gives its patched result");
    check(taken(B) == 0 && taken(C) == 0, "A patched: no run of B or C taken from memory");
    const uint64_t d1 = taken(D), i1 = taken(I);
    check(d1 && i1, "A patched: D and I still taken from memory (" + std::to_string(d1) + ", " + std::to_string(i1) + " of 12)");
    check(same_as_plain(D, OUT + 4, 9) && same_as_plain(I, OUT + 8, 1), "A patched: D and I as before");

    // the patch lifted and put back, a few times (a while_rate patch, a script's write)
    bool ok = true;
    for (int k = 0; k < 3; k++) {
        code(A, LI_R4_1);
        ok &= same_as_plain(B, OUT, 1) && same_as_plain(C, OUT, 1);
        code(A, LI_R4_2);
        ok &= same_as_plain(B, OUT, 2) && same_as_plain(C, OUT, 2);
    }
    check(ok, "A's patch lifted and put back three times: B and C follow it each time");

    // a second function patched later, after I's runs were remembered again with H's old code
    check(taken(I) > 0, "before H's patch: I's runs taken from memory");
    code(H, LI_R9_3);
    check(same_as_plain(I, OUT + 8, 3), "H patched later: I (a remembered caller of H) gives the patched H's result");
    check(same_as_plain(D, OUT + 4, 9) && taken(D) > 0, "H patched later: D still as before, still taken from memory");
    check(same_as_plain(B, OUT, 2) && same_as_plain(C, OUT, 2), "H patched later: B and C still the patched A's");

    printf("%s\n", failed ? "FAILED" : "all ok");
    return failed ? 1 : 0;
}
