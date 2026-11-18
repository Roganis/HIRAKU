// SPDX-License-Identifier: GPL-3.0-or-later
// A mod's hooks on the program's instructions (runtime/core/hooks.h), on a made-up program of three
// functions: each hook runs before its instruction, in the executor's steps and in the interpreter
// (a function a patch changed), a jumped-to instruction's at each pass; what it writes to a register
// or to RAM is what the instruction and the ones after it see; two at one address run in the order
// added; an address in none of the functions is refused, as is a hook added too late; and a run that
// passes a hook is never taken from memory (exec.cpp, "runs remembered": each call runs the hook).
// ctest runs it twice: as is, and with HK_EXEC_MEMO=check.
//
//   test_exec_hook <scratch folder>      exit 0 when all hold
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/exec.h"
#include "core/hooks.h"
#include "core/runtime.h"
#include "ppc/spec.h"

using namespace hk;

namespace {

int failed = 0;
void check(bool ok, const std::string &what) {
    printf("%s%s\n", ok ? "ok    " : "FAIL  ", what.c_str());
    failed += !ok;
}

constexpr uint32_t BASE = 0x80100000, A = BASE, B = BASE + 0x20, C = BASE + 0x40, OUT = 0x80180000, STACK = 0x80170000;
// A: r3 = 5; r4 = r3 + 1; [r5] = r4; r6 = [r5]; return
// B: calls A (a frame on the stack)
// C: r7 = 0, r8 = 3; L: r7 += 1; r8 -= 1 (addic.); bne L; return
const uint32_t g_words[] = {
    0x38600005, 0x38830001, 0x90850000, 0x80c50000, 0x4e800020, 0, 0, 0,
    0x7c0802a6, 0x9421fff0, 0x90010014, 0x4bffffd5, 0x80010014, 0x38210010, 0x7c0803a6, 0x4e800020,
    0x38e00000, 0x39000003, 0x38e70001, 0x3508ffff, 0x4082fff8, 0x4e800020,
};
const char *g_spec =
    "base 80100000\n"
    "func 80100000\nrange 80100000 80100014\nend\n"
    "func 80100020\nrange 80100020 80100040\nend\n"
    "func 80100040\nrange 80100040 80100058\nend\n";

uint32_t ram32(uint32_t a) {
    uint32_t v;
    memcpy(&v, hk_ram + HK_PHYS(a), 4);
    return __builtin_bswap32(v);
}

hk_ctx fresh() {
    hk_ctx c{};
    c.r[1] = STACK;
    c.r[3] = 0x77;
    c.r[5] = OUT;
    c.lr = 0x80100ff0;   // where the call returns to (nothing runs there)
    hk_budget = 1 << 30;
    memset(hk_ram + HK_PHYS(OUT), 0, 4);
    return c;
}

// the hooks: each one's own context
struct Seen {
    int n = 0;
    uint32_t r3 = 0, r4 = 0;
};
void note(void *ctx, uint32_t *regs) {
    Seen &s = *static_cast<Seen *>(ctx);
    s.n++;
    s.r3 = regs[3], s.r4 = regs[4];
}
void set_r3_100(void *, uint32_t *regs) { regs[3] = 100; }
void add_r3_1(void *, uint32_t *regs) { regs[3] += 1; }
void poke_out(void *, uint32_t *) {   // the word A is about to read, in the machine's byte order
    const uint32_t v = __builtin_bswap32(0x12345678u);
    memcpy(hk_ram + HK_PHYS(OUT), &v, 4);
}

std::string g_image;

std::string load() { return exec::load_text(g_spec, g_image); }

bool add(uint32_t at, hooks::Fn fn, void *ctx) {
    std::string err;
    const bool ok = hooks::add(at, fn, ctx, "test", err);
    if (!ok) printf("      %s\n", err.c_str());
    return ok;
}

}  // namespace

int main(int argc, char **argv) {
    const std::string dir = argc > 1 ? argv[1] : "exec-hook-test";
    std::filesystem::create_directories(dir);
    g_image = dir + "/program.bin";
    {
        std::ofstream f(g_image, std::ios::binary);
        for (uint32_t w : g_words) {
            const uint32_t be = __builtin_bswap32(w);
            f.write(reinterpret_cast<const char *>(&be), 4);
        }
    }
    ram_init();
    for (size_t k = 0; k < sizeof g_words / 4; k++) {
        const uint32_t be = __builtin_bswap32(g_words[k]);
        memcpy(hk_ram + HK_PHYS(BASE) + 4 * k, &be, 4);
    }

    // no hooks: the program as it is
    check(load().empty(), "the made-up program loads");
    hk_ctx c = fresh();
    exec::call(&c, B);
    check(c.r[4] == 6 && c.r[6] == 6 && ram32(OUT) == 6 && c.pc == 0x80100ff0, "without hooks: r4 = r6 = [out] = 6, back at the caller");
    check(!hooks::g_any, "without hooks: the interpreter's flag stays down");

    // hooks: before A's first instruction (it sees the caller's r3), two at A+4 (set 100, then add
    // 1: r4 = 102), one at A+12 writing the word A reads there, two in C's loop
    hooks::clear();
    Seen entry, loop, loop_head;
    check(add(A, note, &entry) && add(A + 4, set_r3_100, nullptr) && add(A + 4, add_r3_1, nullptr) &&
              add(A + 12, poke_out, nullptr) && add(C + 8, note, &loop) && add(C, note, &loop_head),
          "hooks added before the program is loaded");
    check(load().empty(), "the program loads with its hooks");
    std::string err;
    check(!hooks::add(A, note, &entry, "test", err), "a hook added after the program is loaded is refused (" + err + ")");
    c = fresh();
    exec::call(&c, B);
    check(entry.n == 1 && entry.r3 == 0x77, "executor: the hook at A's entry runs before A's li r3, 5 (it sees the caller's r3)");
    check(c.r[4] == 102, "executor: two hooks at one address in the order added, before their instruction (r4 = 100 + 1 + 1)");
    check(c.r[6] == 0x12345678 && ram32(OUT) == 0x12345678, "executor: a hook's write to RAM is what the following load reads");
    c = fresh();
    exec::call(&c, C);
    check(loop.n == 3 && loop_head.n == 1 && c.r[7] == 3, "executor: a hook on a branch's target runs at each of the 3 passes");

    // remembered runs: B called as an exception's handler is, 12 times from the same state. Each call
    // runs the hook at A's entry: a run with a hook in it is never kept and replayed
    entry.n = 0;
    int ran = 0;
    bool same = true;
    for (int k = 0; k < 12; k++) {
        c = fresh();
        if (!exec::call_memo(&c, B)) exec::call(&c, B);
        ran++;
        same &= c.r[4] == 102 && c.r[6] == 0x12345678 && c.pc == 0x80100ff0;
    }
    check(entry.n == ran && same, "remembered runs: the hook ran at each of " + std::to_string(ran) + " calls (" + std::to_string(entry.n) +
                                      "), each with the same result");
    // ... and C, with no hook, as a run that can be remembered (HK_EXEC_MEMO=check compares each)
    hooks::clear();
    check(load().empty(), "the program loads again without hooks");
    bool c_same = true;
    for (int k = 0; k < 12; k++) {
        c = fresh();
        if (!exec::call_memo(&c, C)) exec::call(&c, C);
        c_same &= c.r[7] == 3 && c.r[8] == 0;
    }
    check(c_same, "remembered runs without a hook: the same result each time");

    // a bad address: between A and B, in no function
    hooks::clear();
    Seen nowhere;
    check(add(A + 0x18, note, &nowhere), "a hook at an address in no function is taken until the program loads");
    const std::string e = load();
    check(e.find("0x80100018") != std::string::npos && e.find("none of the program's functions") != std::string::npos,
          "... where it's refused, the message naming it: \"" + e + "\"");
    hooks::clear();
    check(!hooks::add(A + 2, note, &nowhere, "test", err), "an address that isn't a multiple of 4 is refused (" + err + ")");

    // interpreted: A changed by a patch runs in the interpreter from its next call (core/patched.cpp);
    // the same hooks, the same results
    hooks::clear();
    entry = {};
    check(add(A, note, &entry) && add(A + 4, set_r3_100, nullptr) && add(A + 4, add_r3_1, nullptr) && add(A + 12, poke_out, nullptr),
          "the same hooks again");
    check(load().empty(), "the program loads with them");
    patch_functions(*exec::program(), A, A + 4);
    check(patched(A), "A marked changed: interpreted");
    const uint64_t insns = g_stats.insns;
    c = fresh();
    exec::call(&c, B);
    check(g_stats.insns - insns == 5, "A ran in the interpreter (" + std::to_string(g_stats.insns - insns) + " instructions)");
    check(entry.n == 1 && entry.r3 == 0x77, "interpreter: the hook at A's entry runs before its instruction");
    check(c.r[4] == 102 && c.r[6] == 0x12345678, "interpreter: two hooks at one address in order, and a RAM write seen by the load");

    printf("%s\n", failed ? "FAILED" : "all ok");
    return failed ? 1 : 0;
}
