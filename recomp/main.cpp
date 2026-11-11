// SPDX-License-Identifier: GPL-3.0-or-later
// hiraku-recomp: translate a Firebeat PowerPC program into C.
//
//   hiraku-recomp <recomp.txt> <out dir> [--files N]
//
// Reads the spec written by analyze/prep.py and emits:
//   funcs.h          prototypes for every function
//   code_NNN.c       function bodies, split over exactly N files (default 32), so
//                    the build system knows the outputs before they exist
//   table.c          sorted guest-address -> host-function table
// Each guest function becomes `void f_XXXXXXXX(hk_ctx *c)`. Instructions are
// calls to the op_* semantics in include/hiraku/ppc.h with constant operands;
// control flow is gotos, direct calls and switch-based jump tables.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "../src/ppc/cycles.h"
#include "../src/ppc/decode.h"
#include "../src/ppc/analyze.h"
#include "../src/ppc/spec.h"

using namespace hk;

namespace {

using spec::Func;

spec::Program g_p;   // the spec and its image (src/ppc/spec.h, shared with the executor)
std::vector<uint8_t> &g_image = g_p.image;
uint32_t &g_base = g_p.base;
std::map<uint32_t, Func> &g_funcs = g_p.funcs;
std::map<uint32_t, uint32_t> &g_secondary = g_p.secondary;
std::set<uint32_t> &g_return_up = g_p.return_up;

[[noreturn]] void die(const std::string &msg) {
    fprintf(stderr, "hiraku-recomp: %s\n", msg.c_str());
    exit(1);
}

uint32_t word_at(uint32_t addr) {
    if (!g_p.in_image(addr)) die("address outside image");
    return g_p.word_at(addr);
}

void load_spec(const char *path) {
    if (std::string e = spec::load(g_p, path); !e.empty()) die(e);
}

std::string fn(uint32_t a) {
    char b[16];
    snprintf(b, sizeof b, "f_%08x", a);
    return b;
}
std::string lbl(uint32_t a) {
    char b[16];
    snprintf(b, sizeof b, "L_%08x", a);
    return b;
}
std::string hex(uint32_t v) {
    char b[16];
    snprintf(b, sizeof b, "0x%08xu", v);
    return b;
}

// Argument formatting for the op list: every field is an integer.
std::string arg(int64_t v) { return std::to_string(v); }

std::string ordinary(const Insn &i) {
    std::string s;
    switch (i.op) {
#define HK_E0(n) case Op::n: s = "op_" #n "(c"; break;
#define HK_E1(n, a) case Op::n: s = "op_" #n "(c, " + arg(i.a); break;
#define HK_E2(n, a, b) case Op::n: s = "op_" #n "(c, " + arg(i.a) + ", " + arg(i.b); break;
#define HK_E3(n, a, b, d) case Op::n: s = "op_" #n "(c, " + arg(i.a) + ", " + arg(i.b) + ", " + arg(i.d); break;
#define HK_E4(n, a, b, d, e) case Op::n: s = "op_" #n "(c, " + arg(i.a) + ", " + arg(i.b) + ", " + arg(i.d) + ", " + arg(i.e); break;
#define HK_E5(n, a, b, d, e, f) case Op::n: s = "op_" #n "(c, " + arg(i.a) + ", " + arg(i.b) + ", " + arg(i.d) + ", " + arg(i.e) + ", " + arg(i.f); break;
#define HK_E6(n, a, b, d, e, f, g) case Op::n: s = "op_" #n "(c, " + arg(i.a) + ", " + arg(i.b) + ", " + arg(i.d) + ", " + arg(i.e) + ", " + arg(i.f) + ", " + arg(i.g); break;
        HK_OPLIST(HK_E0, HK_E1, HK_E2, HK_E3, HK_E4, HK_E5, HK_E6)
#undef HK_E0
#undef HK_E1
#undef HK_E2
#undef HK_E3
#undef HK_E4
#undef HK_E5
#undef HK_E6
    default:
        return "";
    }
    // tw/twi take the instruction address as their last field already
    return s + ");";
}

// The condition expression for a conditional branch (decrements CTR as a side effect).
std::string cond(const Insn &i) {
    char b[64];
    snprintf(b, sizeof b, "hk_bc_ok(c, %u, %u)", i.bo, i.bi);
    return b;
}

void find_return_up() { spec::find_return_up(g_p); }

struct Emitter {
    const Func &f;
    std::ostringstream o;
    std::set<uint32_t> body;      // every instruction address in the function
    std::set<uint32_t> labels;    // addresses that need a label
    int problems = 0;
    int pending = 0;              // cycles not yet charged (HK_TICK), as MAME counts them (src/ppc/cycles.h)

    void flush() {
        if (pending) o << "    HK_TICK(" << pending << ");\n";
        pending = 0;
    }

    explicit Emitter(const Func &fn_) : f(fn_) {
        for (auto [lo, hi] : f.ranges)
            for (uint32_t a = lo; a < hi; a += 4) body.insert(a);
    }

    bool inside(uint32_t a) const { return body.count(a) != 0; }

    void find_labels() {
        for (uint32_t a : body) {
            Insn i = decode(word_at(a), a);
            if ((i.op == Op::b || i.op == Op::bc) && !i.lk && inside(i.target)) labels.insert(i.target);
        }
        for (auto &[at, ts] : f.jumptables)
            for (uint32_t t : ts)
                if (inside(t)) labels.insert(t);
        // a range that ends by falling through continues at the next range
        for (auto [lo, hi] : f.ranges)
            if (inside(hi)) labels.insert(hi);
    }

    // A poll lets due events and interrupts in: at a back-edge and at a function
    // entry. It charges nothing (HK_TICK does); `cost` is unused.
    void poll(uint32_t at, uint32_t cost) {
        if (!f.nopoll) o << "    HK_POLL(c, " << hex(at) << ", " << cost << ");\n";
    }

    // Leave the function towards `target`: a jump inside, a tail call, or the dynamic path.
    void go(uint32_t from, uint32_t target) {
        if (inside(target)) {
            if (target <= from) poll(from, (from - target) / 4 + 1);
            o << "goto " << lbl(target) << ";";
        } else if (g_funcs.count(target) || g_secondary.count(target)) {
            o << fn(target) << "(c); return;";
        } else {
            o << "hk_jump(c, " << hex(target) << "); return;";
            problems++;
        }
    }

    void call(uint32_t ret, uint32_t target) {
        if (g_return_up.count(target))
            o << "c->lr = " << hex(ret) << "; " << fn(target) << "(c); return;  /* returns to our caller */";
        else if (g_funcs.count(target) || g_secondary.count(target))
            o << "HK_CALL(c, " << fn(target) << ", " << hex(ret) << ");";
        else
            o << "HK_CALL_DYN(c, " << hex(target) << ", " << hex(ret) << ");";
    }

    void insn(uint32_t a) {
        Insn i = decode(word_at(a), a);
        if (labels.count(a)) { flush(); o << lbl(a) << ":\n"; }
        {
            uint32_t prev = 0;
            bool has_prev = a - 4 >= g_base && a - g_base <= g_image.size();
            if (has_prev) has_prev = !hk::ppc::ends_sequence(prev = word_at(a - 4));
            pending += hk::ppc::cycles(i.word, a, has_prev, prev);
        }
        // charge before anything that may leave straight-line code (branches,
        // calls, returns, syscalls) or touch the outside world in time (MMIO)
        if (i.is_branch() || i.op == Op::sc || i.op == Op::rfi || i.op == Op::rfci) flush();
        o << "    ";
        const uint32_t next = a + 4;
        switch (i.op) {
        case Op::b:
            if (i.lk) {
                if (i.target == next) o << "c->lr = " << hex(next) << ";";  // bl . + 4: read own address
                else call(next, i.target);
            } else {
                go(a, i.target);
            }
            break;
        case Op::bc:
            if (i.lk) {
                o << "c->lr = " << hex(next) << "; if (" << cond(i) << ") { ";
                call(next, i.target);
                o << " }";
            } else if (i.always()) {
                go(a, i.target);
            } else {
                o << "if (" << cond(i) << ") { ";
                go(a, i.target);
                o << " }";
            }
            break;
        case Op::bclr:
            if (i.lk) {
                o << "{ uint32_t t = c->lr & ~3u; c->lr = " << hex(next) << "; if (" << cond(i)
                  << ") HK_CALL_IND(c, t, " << hex(next) << "); }";
            } else if (i.always()) {
                o << "HK_RET(c);";
            } else {
                o << "if (" << cond(i) << ") HK_RET(c);";
            }
            break;
        case Op::bcctr: {
            std::string guard = i.always() ? "" : "if (" + cond(i) + ") ";
            if (i.lk) {
                o << "{ uint32_t t = c->ctr & ~3u; c->lr = " << hex(next) << "; " << guard
                  << "HK_CALL_IND(c, t, " << hex(next) << "); }";
            } else if (f.jumptables.count(a)) {
                o << guard << "switch (c->ctr & ~3u) {";
                for (uint32_t t : std::set<uint32_t>(f.jumptables.at(a).begin(), f.jumptables.at(a).end())) {
                    if (inside(t)) o << " case " << hex(t) << ": goto " << lbl(t) << ";";
                    else o << " case " << hex(t) << ": hk_jump(c, " << hex(t) << "); return;";   // a case Ghidra split off
                }
                o << " default: hk_jump(c, c->ctr & ~3u); return; }";
            } else {
                o << guard << "{ hk_jump(c, c->ctr & ~3u); return; }";
            }
            break;
        }
        case Op::sc:
            o << "c->pc = " << hex(next) << "; hk_syscall(c);";
            break;
        case Op::rfi:      // the end of an exception vector: back to the glue, which resumes at SRR0
            o << "c->msr = c->srr1; c->pc = c->srr0 & ~3u; return;";
            break;
        case Op::rfci:
            o << "c->msr = c->srr3; c->pc = c->srr2 & ~3u; return;";
            break;
        case Op::unimpl:
        case Op::invalid:
            o << "hk_unimplemented(c, " << hex(a) << ", " << hex(i.word) << "); return;";
            problems += i.op == Op::invalid;
            break;
        case Op::nop:
            o << ";";
            break;
        default:
            o << ordinary(i);
            break;
        }
        char cmt[24];
        snprintf(cmt, sizeof cmt, "  /* %08x */\n", a);
        o << cmt;
    }

    // Can control fall out of this instruction to the next address?
    static bool falls_through(const Insn &i) {
        switch (i.op) {
        case Op::b: return i.lk && !g_return_up.count(i.target);
        case Op::bc: case Op::bclr: case Op::bcctr: return i.lk || !i.always();
        case Op::rfi: case Op::rfci: return false;
        default: return true;
        }
    }

    // Emit the whole body as a C function entered at `entry` (the function's
    // own entry, or a secondary one: then the body is duplicated, entered by goto).
    std::string run(uint32_t entry) {
        o.str("");
        labels.clear();
        find_labels();
        for (uint32_t e : f.entries) labels.insert(e);
        o << "void " << fn(entry) << "(hk_ctx *c) {\n";
        pending = 0;
        // the entry need not be the first range (Ghidra lists ranges by address)
        bool entry_first = !f.ranges.empty() && f.ranges.front().first == entry;
        poll(entry, 8);
        // A function without polls still asks, at its entry, whether a mod changed its code (HK_POLL's
        // second half, hiraku/recomp.h): the executor asks for every function (core/exec.cpp, run()),
        // and a patch that works there must not be skipped here
        if (f.nopoll)
            o << "    if (__builtin_expect(hk_patched_any, 0) && hk_patched(" << hex(entry) << ")) { hk_interp_function(c, "
              << hex(entry) << "); return; }\n";
        if (!entry_first) {
            labels.insert(entry);
            o << "    goto " << lbl(entry) << ";\n";
        }
        for (auto [lo, hi] : f.ranges) {
            for (uint32_t a = lo; a < hi; a += 4) insn(a);
            Insn last = decode(word_at(hi - 4), hi - 4);
            flush();
            if (falls_through(last)) {
                o << "    ";
                go(hi - 4, hi);  // into the next range, or a tail call into the next function
                o << "  /* fall-through */\n";
            }
        }
        o << "}\n\n";
        return o.str();
    }
};

}  // namespace

// hiraku-recomp --analyze PROGRAM [--base HEX] [--entry HEX] [--extra HEX,...] [--nopoll LO-HI,...]:
// the recompile spec from HIRAKU's own analysis (src/ppc/analyze.h), on stdout
int analyze_main(int argc, char **argv) {
    std::ifstream f(argv[2], std::ios::binary);
    if (!f) die(std::string("cannot open ") + argv[2]);
    std::vector<uint8_t> img((std::istreambuf_iterator<char>(f)), {});
    hk::analyze::Options o;
    o.image_path = argv[2];
    for (int k = 3; k + 1 < argc; k += 2) {
        std::string a = argv[k], v = argv[k + 1];
        if (a == "--base") o.base = std::stoul(v, nullptr, 16);
        else if (a == "--entry") o.entry = std::stoul(v, nullptr, 16);
        else if (a == "--image-path") o.image_path = v;
        else if (a == "--extra" || a == "--nopoll") {
            std::stringstream ss(v);
            for (std::string item; std::getline(ss, item, ',');) {
                if (item.empty()) continue;
                if (a == "--extra") o.extra.push_back(std::stoul(item, nullptr, 16));
                else {
                    auto dash = item.find('-');
                    o.nopoll.emplace_back(std::stoul(item.substr(0, dash), nullptr, 16), std::stoul(item.substr(dash + 1), nullptr, 16));
                }
            }
        } else die("unknown option " + a);
    }
    hk::analyze::Result r = hk::analyze::run(img, o);
    fputs(r.spec.c_str(), stdout);
    for (const std::string &n : r.notes) fprintf(stderr, "note: %s\n", n.c_str());
    fprintf(stderr, "%zu functions, %zu secondary entries, %zu jump tables\n", r.functions, r.secondary, r.tables);
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 3 && std::string(argv[1]) == "--analyze") return analyze_main(argc, argv);
    if (argc < 3) {
        fprintf(stderr, "usage: hiraku-recomp <recomp.txt> <out dir> [--files N]\n"
                        "       hiraku-recomp --analyze <program.bin> [--base HEX] [--entry HEX] [--extra HEX,...] [--nopoll LO-HI,...]\n");
        return 2;
    }
    size_t nfiles = 32;
    for (int k = 3; k + 1 < argc; k++)
        if (std::string(argv[k]) == "--files") nfiles = std::stoul(argv[k + 1]);
    load_spec(argv[1]);
    find_return_up();
    std::string out = argv[2];
    // A file is written only when what it holds changes: a recompiler built again that gives the
    // same C leaves the files' times alone, and the build doesn't compile every game's code again
    size_t written = 0;
    auto put = [&](const std::string &name, const std::string &text) {
        {
            std::ifstream was(out + name, std::ios::binary);
            if (was && std::string(std::istreambuf_iterator<char>(was), {}) == text) return;
        }
        std::ofstream f(out + name, std::ios::binary);
        f << text;
        if (!f.flush()) die("cannot write " + out + name);
        written++;
    };

    {
        std::ostringstream h;
        h << "/* generated by hiraku-recomp - do not edit, do not commit */\n#pragma once\n"
             "#include \"hiraku/recomp.h\"\n";
        for (auto &[e, f] : g_funcs) h << "void " << fn(e) << "(hk_ctx *c);\n";
        for (auto &[e, owner] : g_secondary) h << "void " << fn(e) << "(hk_ctx *c);  /* inside " << fn(owner) << " */\n";
        put("/funcs.h", h.str());
    }

    int problems = 0;
    const size_t per_file = (g_funcs.size() + nfiles - 1) / nfiles;
    std::vector<std::ostringstream> files(nfiles);
    for (size_t k = 0; k < nfiles; k++)
        files[k] << "/* generated by hiraku-recomp - do not edit, do not commit */\n#include \"funcs.h\"\n\n";
    size_t n = 0;
    for (auto &[e, f] : g_funcs) {
        Emitter em(f);
        auto &file = files[n++ / per_file];
        file << em.run(e);
        problems += em.problems;
        for (uint32_t se : f.entries) file << em.run(se);
    }
    for (size_t k = 0; k < nfiles; k++) {
        char name[32];
        snprintf(name, sizeof name, "/code_%03zu.c", k);
        put(name, files[k].str());
    }
    const size_t fileno = nfiles;

    std::ostringstream t;
    t << "/* generated by hiraku-recomp - do not edit, do not commit */\n#include \"funcs.h\"\n\n"
         "const hk_func_entry hk_func_table[] = {\n";
    std::map<uint32_t, bool> all;
    for (auto &[e, f] : g_funcs) all[e] = true;
    for (auto &[e, owner] : g_secondary) all[e] = true;
    for (auto &[e, _] : all) t << "    { " << hex(e) << ", " << fn(e) << " },\n";
    t << "};\nconst unsigned hk_func_count = " << all.size() << ";\n";
    put("/table.c", t.str());

    printf("%zu functions -> %zu files (%zu changed), %zu return-up helpers, %d unresolved control-flow sites\n", g_funcs.size(),
           fileno, written, g_return_up.size(), problems);
    return 0;
}
