// SPDX-License-Identifier: GPL-3.0-or-later
// Recompiled R3000 blocks: the dispatcher MAME's execute_run() calls, and the recorder
// (runtime/sys573/mips.h).
#include "sys573/mips.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <unordered_map>
#include <vector>

#include "sys573/mips_ops.h"
#include "machine/ram.h"

#include "shell/sys.h"

// the game's blocks (tools/mipsgen.py; none without a recording)
extern const hk573_block *const hk573_parts[];
extern const uint32_t hk573_part_sizes[];
extern const uint32_t hk573_nparts;

namespace hk::k573::mips {
namespace {

using X = hk_psx;
constexpr uint32_t TAB_SIZE = 1u << 22;   // indexed by (pc >> 2) & 0x3fffff, as in execute_run()
constexpr int MAX_LEN = 128;              // the longest block the recorder makes

struct Node {
    uint32_t pc, len, nwords;
    void (*fn)(psxcpu_device &);
    const hk573_block *b;
    const Node *next;             // other blocks at the same index: other code at this pc, or another mirror
    mutable uint64_t epoch = 0;   // the code matched at this epoch (0: check it on every run)
    // the blocks that ran after this one, by their pc (the two exits of a conditional branch)
    mutable uint32_t link_pc[2] = {1, 1};
    mutable const Node *link[2] = {nullptr, nullptr};
    mutable uint32_t link_next = 0;
};

// Blocks are trusted from when their code was checked against memory until the epoch moves on:
// whenever the CPU's memory map changes, or anything writes to a page of main RAM holding
// checked code. Those pages (the host's: 4 KiB on x86-64, 16 KiB on some ARM hosts) are write-protected: a write to one, from a
// store, DMA or a device, faults once and is let through, and the epoch moves on, so every block
// is checked again before it next runs (writes to code are rare: a few dozen in a 150 s run).
// A page that keeps being written (code and busy data side by side) is left unprotected, and
// its blocks are checked on every run.
uint64_t &g_epoch = X::epoch;
uint32_t g_map_version = 0;

struct Guard {
    static constexpr uint32_t GIVE_UP = 64;   // faults before a page stays unprotected
    size_t page_size = 4096;                  // the host's (set in init)
    uint8_t *base = nullptr;
    size_t size = 0;
    std::vector<uint32_t> faults;
    std::vector<uint8_t> prot;
    uint64_t total_faults = 0, protects = 0;

    bool has(const uint8_t *p) const { return p >= base && p < base + size; }
    size_t page(const uint8_t *p) const { return (size_t)(p - base) / page_size; }
    // protect the page if it may be; true if it is now protected
    bool protect(size_t i) {
        if (prot[i]) return true;
        if (faults[i] >= GIVE_UP) return false;
        if (!sys::protect_pages(base + i * page_size, page_size, false)) return false;
        prot[i] = 1;
        protects++;
        return true;
    }
} g_guard;

// a write to a protected page (sys::catch_write_faults): true if it was one of ours
bool on_write(void *address) {
    Guard &gd = g_guard;
    const uint8_t *a = static_cast<const uint8_t *>(address);
    if (gd.has(a)) {
        const size_t i = gd.page(a);
        if (gd.prot[i]) {
            sys::protect_pages(gd.base + i * gd.page_size, gd.page_size, true);
            gd.prot[i] = 0;
            gd.faults[i]++;
            gd.total_faults++;
            g_epoch++;
            return true;   // the write runs again, now allowed
        }
    }
    return false;
}

// which instructions end a block: a jump (after its delay slot), or one a block always hands to
// the interpreter. A conditional branch ends it only where it is taken. tools/mipsgen.py compiles
// what the recorder stored, in the same terms.
enum Kind { NORMAL, BRANCH, TERMINAL };
Kind kind(uint32_t w) {
    const uint32_t op = w >> 26, rs = (w >> 21) & 31, fn = w & 63;
    switch (op) {
    case 0:
        if (fn == 8 || fn == 9) return BRANCH;   // jr, jalr
        switch (fn) {
        case 0: case 2: case 3: case 4: case 6: case 7: case 16: case 17: case 18: case 19:
        case 24: case 25: case 26: case 27: case 32: case 33: case 34: case 35: case 36: case 37:
        case 38: case 39: case 42: case 43:
            return NORMAL;
        default:
            return TERMINAL;   // syscall, break, reserved
        }
    case 1: case 2: case 3: case 4: case 5: case 6: case 7:
        return BRANCH;
    case 16:
        return rs == 0 ? NORMAL : TERMINAL;   // mfc0; mtc0, rfe and the rest go to the interpreter
    case 18:
        return rs == 8 || rs == 12 ? TERMINAL : NORMAL;   // bc2f/bc2t
    case 8: case 9: case 10: case 11: case 12: case 13: case 14: case 15:
    case 32: case 33: case 34: case 35: case 36: case 37: case 38:
    case 40: case 41: case 42: case 43: case 46: case 50: case 58:
        return NORMAL;
    default:
        return TERMINAL;   // COP1, COP3, LWC/SWC 0, 1, 3, reserved
    }
}

struct Rec {
    std::vector<uint32_t> words;
    uint64_t count = 0;
};

struct Stats {
    uint64_t runs = 0, insns = 0, short_slice = 0, not_fast = 0, mismatch = 0, checked = 0, unwatched = 0;
};

struct State {
    psxcpu_device *cpu = nullptr;
    std::vector<const void *> tab;
    std::vector<Node> nodes;
    size_t nblocks = 0;
    bool stats_on = false;
    Stats st;
    // recording
    std::string rec_path;
    std::unordered_map<uint32_t, std::vector<Rec>> rec;   // pc -> the code seen there
    uint32_t last = 0, cur_end = 0;
} g;

// the instruction words at a, through the CPU's page table (RAM and ROM only)
bool words_at(psxcpu_device &d, uint32_t a, uint32_t *out, int n) {
    const X::page_t *pg = X::pages(d);
    for (int i = 0; i < n; i++, a += 4) {
        const uint8_t *p = pg[a >> X::PAGE_BITS].direct_r;
        if (!p) return false;
        memcpy(&out[i], p + (a & X::PAGE_MASK), 4);
    }
    return true;
}

// a range of code against memory, through the CPU's page table; host: where it starts
bool same_code(psxcpu_device &d, uint32_t pc, uint32_t nwords, const uint32_t *words, const uint8_t *&host) {
    const X::page_t *pg = X::pages(d);
    uint32_t a = pc;
    host = nullptr;
    for (uint32_t i = 0; i < nwords;) {
        const uint8_t *p = pg[a >> X::PAGE_BITS].direct_r;
        if (!p) return false;
        const uint32_t off = a & X::PAGE_MASK;
        if (!host) host = p + off;
        else if (p + off != host + 4 * i) return false;   // not contiguous on the host: never trusted
        const uint32_t n = std::min<uint32_t>(nwords - i, (X::PAGE_MASK + 1 - off) / 4);
        const uint8_t *q = p + off;
        for (uint32_t k = 0; k < n; k++) {
            uint32_t w;
            memcpy(&w, q + 4 * k, 4);
            if (w != words[i + k]) return false;
        }
        i += n;
        a += n * 4;
    }
    return true;
}

// the code is there and can be watched (main RAM, write-protected) or never changes (ROM)
bool same_and_watched(psxcpu_device &d, uint32_t pc, uint32_t nwords, const uint32_t *words, bool &watched) {
    const uint8_t *host;
    if (!same_code(d, pc, nwords, words, host)) return false;
    Guard &gd = g_guard;
    const uint8_t *end = host + 4 * nwords - 1;
    if (gd.has(host)) {
        if (!(gd.has(end) && gd.protect(gd.page(host)) && gd.protect(gd.page(end)))) watched = false;
    } else if (X::pages(d)[pc >> X::PAGE_BITS].direct_w) {
        watched = false;   // writable, and not main RAM: can't be watched
    }
    return true;
}

// the block's code (and its loop's) is at pc now: trust it until the epoch moves on, if it can be
// watched
bool check(psxcpu_device &d, const Node *n) {
    const hk573_block *b = n->b;
    bool watched = true;
    if (!same_and_watched(d, b->pc, b->nwords, b->words, watched)) return false;
    for (uint32_t i = 0; i < b->nextra; i++)
        if (!same_and_watched(d, b->extra[i].pc, b->extra[i].nwords, b->extra[i].words, watched)) return false;
    n->epoch = watched ? g_epoch : 0;
    if (g.stats_on) (watched ? g.st.checked : g.st.unwatched)++;
    return true;
}

// the CPU's memory map changed: check every block's code again before it runs; find the scratchpad
void map_changed(const address_space &space) {
    g_map_version = space.version();
    g_epoch++;
    uint8_t *r = space.mem_ptr(X::SP_BASE, false), *w = space.mem_ptr(X::SP_BASE, true);
    const bool whole = r && r == w && space.mem_ptr(X::SP_BASE + X::SP_MASK, false) == r + X::SP_MASK &&
                       space.mem_ptr(X::SP_BASE + X::SP_MASK, true) == r + X::SP_MASK;
    X::sp = whole ? r : nullptr;
}

// the block for the code at pc among head's, if its code is there
const Node *find(psxcpu_device &d, const Node *head, uint32_t pc) {
    for (const Node *n = head; n; n = n->next) {
        if (n->pc != pc) continue;
        if (n->epoch == g_epoch || check(d, n)) return n;
        if (g.stats_on) g.st.mismatch++;
    }
    return nullptr;
}

// MAME's execute_run() found a block at m_pc: run it, and the blocks after it, until the
// timeslice ends, a branch delay slot or an instruction needs the interpreter, or there is no
// block for the code
bool run(psxcpu_device &d, const void *p) {
    if (!X::fast_mode(d)) {
        if (g.stats_on) g.st.not_fast++;
        return false;
    }
    const address_space &space = X::program(d);
    if (space.version() != g_map_version) map_changed(space);
    const Node *n = find(d, static_cast<const Node *>(p), X::pc(d));
    if (!n) return false;
    bool ran = false;
    for (;;) {
        if ((int)n->len > X::icount(d)) {   // the timeslice ends inside it
            if (g.stats_on) g.st.short_slice++;
            return ran;
        }
        X::bailed = false;
        const int before = X::icount(d);
        const int stolen = d.m_cycles_stolen;
        n->fn(d);
        if (g.stats_on) {   // what it ran: the cycles it took, less any a device cut from the timeslice
            g.st.runs++;
            g.st.insns += before - X::icount(d) - (d.m_cycles_stolen - stolen);
        }
        ran = true;
        if (X::icount(d) <= 0 || X::bailed || X::delayr(d) >= (uint32_t)X::DELAY_PC) return true;
        if (space.version() != g_map_version) return true;   // a device changed the map: start over
        // the next block: one that ran after this one before, or from the table
        const uint32_t pc = X::pc(d);
        const Node *next;
        if (n->link_pc[0] == pc && (next = n->link[0])->epoch == g_epoch) {
        } else if (n->link_pc[1] == pc && (next = n->link[1])->epoch == g_epoch) {
        } else {
            next = find(d, static_cast<const Node *>(g.tab[(pc >> 2) & (TAB_SIZE - 1)]), pc);
            if (!next) return true;
            const uint32_t k = n->link_next;
            n->link_pc[k] = pc;
            n->link[k] = next;
            n->link_next = k ^ 1;
        }
        n = next;
    }
}

// ---- recording ----
void record(psxcpu_device &d) {
    const uint32_t pc = X::pc(d);
    const bool start = pc != g.last + 4 || pc >= g.cur_end;
    g.last = pc;
    if (!start) return;
    g.cur_end = 0;
    if (X::delayr(d) >= (uint32_t)X::DELAY_PC || !X::fast_mode(d)) return;   // blocks never start here
    uint32_t w[MAX_LEN + 1];
    int n = 0;
    while (n < MAX_LEN && words_at(d, pc + 4 * n, &w[n], 1)) {
        const Kind k = kind(w[n++]);
        if (k == TERMINAL) break;
        if (k != BRANCH) continue;
        const uint32_t op = w[n - 1] >> 26;
        const bool conditional = op == 1 || (op >= 4 && op <= 7);
        if (!words_at(d, pc + 4 * n, &w[n], 1)) break;
        const Kind slot = kind(w[n]);
        if (slot == BRANCH) break;   // a branch in the delay slot: left to the interpreter
        n++;
        if (!conditional || slot == TERMINAL) break;
        // a conditional branch: the block runs on where it isn't taken
    }
    if (n == 0) return;
    g.cur_end = pc + 4 * n;
    auto &v = g.rec[pc];
    for (Rec &r : v)
        if ((int)r.words.size() == n && memcmp(r.words.data(), w, 4 * n) == 0) { r.count++; return; }
    v.push_back(Rec{std::vector<uint32_t>(w, w + n), 1});
}

void load_recording(const std::string &path) {
    std::ifstream f(path);
    for (std::string line; std::getline(f, line);) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream s(line);
        uint32_t pc;
        uint64_t count;
        s >> std::hex >> pc >> std::dec >> count;
        Rec r;
        r.count = count;
        for (uint32_t w; s >> std::hex >> w;) r.words.push_back(w);
        if (!s.eof() || r.words.empty()) continue;
        auto &v = g.rec[pc];
        bool merged = false;
        for (Rec &o : v)
            if (o.words == r.words) { o.count += r.count; merged = true; }
        if (!merged) v.push_back(std::move(r));
    }
}

std::unordered_map<uint32_t, uint64_t> g_slow;   // slow accesses from blocks, per 16 bytes
// ... by region (flash window, the PS1's I/O page, other) and kind (1, 2, 4: reads of that size;
// 3: lwl/lwr; 8: word writes; 7: narrower writes)
uint64_t g_slow_kind[3][9];

}  // namespace

void init(psxcpu_device &cpu) {
    g.cpu = &cpu;
    g.stats_on = getenv("HK_573_MIPS_STATS") != nullptr;
    X::note_on = g.stats_on;
    if (X::program(cpu).page_bits() != X::PAGE_BITS) {
        fprintf(stderr, "mips: the shim's pages are %d bits, the blocks expect %d: interpreting\n",
                X::program(cpu).page_bits(), X::PAGE_BITS);
        return;
    }
    if (const char *p = getenv("HK_573_RECORD"); p && *p) {
        g.rec_path = p;
        load_recording(g.rec_path);
        cpu.m_hk_rec = record;
        return;   // a recording run interprets everything
    }
    if (cpu.m_hk_trace) return;   // HK_573_ITRACE: every instruction, so all interpreted
    idle_init(cpu);               // waiting loops counted, with blocks or without (idle.cpp)
    if (const char *b = getenv("HK_573_BLOCKS"); b && strcmp(b, "0") == 0) return;
    size_t total = 0;
    for (uint32_t i = 0; i < hk573_nparts; i++) total += hk573_part_sizes[i];
    if (!total) return;
    g.tab.assign(TAB_SIZE, nullptr);
    g.nodes.reserve(total);
    for (uint32_t i = 0; i < hk573_nparts; i++)
        for (uint32_t j = 0; j < hk573_part_sizes[i]; j++) {
            const hk573_block *b = &hk573_parts[i][j];
            const void *&slot = g.tab[(b->pc >> 2) & (TAB_SIZE - 1)];
            g.nodes.push_back(Node{b->pc, b->len, b->nwords, b->fn, b, static_cast<const Node *>(slot)});
            slot = &g.nodes.back();
        }
    g.nblocks = total;
    // write-protection of main RAM's code pages (Guard), unless HK_573_GUARD=0
    if (const char *gv = getenv("HK_573_GUARD"); !gv || strcmp(gv, "0") != 0) {
        ram_device &ram = X::ram(cpu);
        Guard &gd = g_guard;
        gd.page_size = sys::page_size();
        gd.base = ram.pointer();
        gd.size = ram.size() & ~(gd.page_size - 1);
        gd.faults.assign(gd.size / gd.page_size, 0);
        gd.prot.assign(gd.size / gd.page_size, 0);
        sys::catch_write_faults(on_write);
    }
    map_changed(X::program(cpu));
    cpu.m_hk_tab = g.tab.data();
    cpu.m_hk_run = run;
}

void finish() {
    if (g.rec_path.empty()) return;
    std::vector<std::pair<uint32_t, const Rec *>> all;
    for (auto &[pc, v] : g.rec)
        for (const Rec &r : v) all.emplace_back(pc, &r);
    std::sort(all.begin(), all.end(), [](auto &a, auto &b) {
        return a.first != b.first ? a.first < b.first : a.second->words < b.second->words;
    });
    const std::string tmp = g.rec_path + ".tmp";
    FILE *f = fopen(tmp.c_str(), "w");
    if (!f) { perror(tmp.c_str()); return; }
    fprintf(f, "# System 573 R3000 blocks (runtime/sys573/mips.h): pc, times run, the instruction words\n");
    for (auto &[pc, r] : all) {
        fprintf(f, "%08x %" PRIu64, pc, r->count);
        for (uint32_t w : r->words) fprintf(f, " %08x", w);
        fputc('\n', f);
    }
    fclose(f);
    sys::replace_file(tmp, g.rec_path);
    printf("mips: %zu blocks recorded in %s\n", all.size(), g.rec_path.c_str());
}

std::string report() {
    if (!g.stats_on) return idle_report();
    char b[400];
    const uint64_t total = g.cpu ? static_cast<cpu_device *>(g.cpu)->total_cycles() : 0;
    snprintf(b, sizeof b,
             "mips: %zu blocks built in; %" PRIu64 " runs, %" PRIu64 " instructions (%.1f %% of %" PRIu64
             "); refused: %" PRIu64 " short slice, %" PRIu64 " mode, %" PRIu64 " other code; %" PRIu64
             " code pages protected, %" PRIu64 " written",
             g.nblocks, g.st.runs, g.st.insns, total ? 100.0 * (double)g.st.insns / (double)total : 0.0, total,
             g.st.short_slice, g.st.not_fast, g.st.mismatch, g_guard.protects, g_guard.total_faults);
    std::string out = b;
    snprintf(b, sizeof b, "\nmips: code checked %" PRIu64 " times, %" PRIu64 " of them unwatched", g.st.checked, g.st.unwatched);
    out += b;
    std::map<uint32_t, uint64_t> by64k;
    uint64_t all = 0;
    for (auto &[a, n] : g_slow) { by64k[a >> 16] += n; all += n; }
    snprintf(b, sizeof b, "\nmips: %" PRIu64 " slow accesses;", all);
    out += b;
    for (auto &[pg, n] : by64k)
        if (n * 100 >= all) { snprintf(b, sizeof b, " %04xxxxx %" PRIu64, pg, n); out += b; }
    static const char *const region[3] = {"flash", "I/O", "other"};
    for (int r = 0; r < 3; r++) {
        snprintf(b, sizeof b, "\nmips: slow %s: lb %" PRIu64 " lh %" PRIu64 " lw %" PRIu64 " lwl/r %" PRIu64 " sw %" PRIu64 " sb/sh %" PRIu64,
                 region[r], g_slow_kind[r][1], g_slow_kind[r][2], g_slow_kind[r][4], g_slow_kind[r][3], g_slow_kind[r][8], g_slow_kind[r][7]);
        out += b;
    }
    std::vector<std::pair<uint64_t, uint32_t>> top;
    for (auto &[a, n] : g_slow) top.emplace_back(n, a);
    std::sort(top.rbegin(), top.rend());
    for (size_t i = 0; i < top.size() && i < 6; i++) {
        snprintf(b, sizeof b, "\nmips: slow accesses at %08x-%08x: %" PRIu64, top[i].second, top[i].second + 15, top[i].first);
        out += b;
    }
    if (std::string s = idle_report(); !s.empty()) out += "\n" + s;
    return out;
}

}  // namespace hk::k573::mips

void hk_psx::note(uint32_t a, int size) {
    hk::k573::mips::g_slow[a & ~15u]++;
    hk::k573::mips::g_slow_kind[(a >> 16) == 0x1f80 ? 1 : (a >> 22) == (0x1f000000u >> 22) ? 0 : 2][size]++;
}
