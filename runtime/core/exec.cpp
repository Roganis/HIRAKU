// SPDX-License-Identifier: GPL-3.0-or-later
// The executor (exec.h): each function of the spec as steps, built by a mirror of
// recomp/main.cpp's Emitter. Where the emitter writes a line of C, this appends a step that does
// what the line does, so the two must change together.
#include "exec.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <vector>

#include "hiraku/recomp.h"
#include "ppc/cycles.h"
#include "ppc/decode.h"
#include "ppc/spec.h"
#include "exec_names.h"
#include "exec_steps.h"
#include "runtime.h"

namespace hk::exec {

namespace {

spec::Program g_p;
std::vector<std::unique_ptr<Code>> g_codes;
std::unordered_map<uint32_t, const Code *> g_entry;   // entry (or secondary entry) -> its code
bool g_loaded = false;

// recomp/main.cpp's Emitter, appending steps instead of C
struct Builder {
    const spec::Func &f;
    Code &out;
    std::set<uint32_t> body, labels;
    std::map<uint32_t, uint32_t> label_at;              // label address -> step index
    std::vector<std::pair<size_t, uint32_t>> go_fix;    // go steps to point at a label
    std::vector<std::pair<size_t, uint32_t>> table_fix; // (table, case) -> label, by flat index
    std::vector<std::pair<size_t, const Code **>> fn_fix;
    std::vector<std::pair<size_t, uint32_t>> callee;    // steps calling a function, by address
    int pending = 0;

    Builder(const spec::Func &fn_, Code &o) : f(fn_), out(o) {
        for (auto [lo, hi] : f.ranges)
            for (uint32_t a = lo; a < hi; a += 4) body.insert(a);
    }
    bool inside(uint32_t a) const { return body.count(a) != 0; }
    size_t add(const Step &s) { out.steps.push_back(s); return out.steps.size() - 1; }

    void flush() {
        if (pending) {
            Step s;
            s.k = K_tick;
            s.a = (uint32_t)pending;
            add(s);
        }
        pending = 0;
    }
    void find_labels() {
        for (uint32_t a : body) {
            Insn i = decode(g_p.word_at(a), a);
            if ((i.op == Op::b || i.op == Op::bc) && !i.lk && inside(i.target)) labels.insert(i.target);
        }
        for (auto &[at, ts] : f.jumptables)
            for (uint32_t t : ts)
                if (inside(t)) labels.insert(t);
        for (auto [lo, hi] : f.ranges)
            if (inside(hi)) labels.insert(hi);
    }
    void poll(uint32_t at) {
        if (f.nopoll) return;
        Step s;
        s.k = K_poll;
        s.a = at;
        add(s);
    }
    // a branch's condition, the commonest ways (the BO bits hk_bc_ok doesn't look at for them
    // left out): 0 any, 1 on a condition bit set, 2 clear, 3 on the count register not zero
    static int shape(const Insn &i) {
        return (i.bo & 0x1c) == 0x0c ? 1 : (i.bo & 0x1c) == 0x04 ? 2 : (i.bo & 0x16) == 0x10 ? 3 : 0;
    }
    void goto_label(uint32_t target) {
        Step s;
        s.k = K_go;
        go_fix.emplace_back(add(s), target);
    }
    // inside the function: a poll where it goes backwards, and the goto
    bool polls(uint32_t from, uint32_t target) const { return target <= from && !f.nopoll; }
    void go(uint32_t from, uint32_t target) {
        if (inside(target)) {
            Step s;
            s.k = polls(from, target) ? K_gop : K_go;
            s.y.b = from;
            go_fix.emplace_back(add(s), target);
        } else if (g_p.funcs.count(target) || g_p.secondary.count(target)) {
            Step s;
            s.k = K_tail;
            callee.emplace_back(add(s), target);
        } else {
            Step s;
            s.k = K_jump;
            s.a = target;
            add(s);
        }
    }
    // if (cond) the same, in one step
    void go_if(const Insn &i, uint32_t from, uint32_t target) {
        static const uint16_t plain[] = {K_br, K_br_true, K_br_false, K_br_dnz}, polled[] = {K_brp, K_brp_true, K_brp_false, K_brp_dnz};
        Step s;
        s.k = (polls(from, target) ? polled : plain)[shape(i)];
        s.rt = i.bo, s.ra = i.bi;
        s.y.b = from;
        go_fix.emplace_back(add(s), target);
    }
    void call(uint32_t ret, uint32_t target) {
        Step s;
        s.a = ret;
        if (g_p.return_up.count(target)) {
            s.k = K_call_up;
            callee.emplace_back(add(s), target);
        } else if (g_p.funcs.count(target) || g_p.secondary.count(target)) {
            s.k = K_call;
            callee.emplace_back(add(s), target);
        } else {
            s.k = K_call_dyn;
            s.y.b = target;
            add(s);
        }
    }
    // if (cond) { ...then }: a bc step skipping the then-steps, its target set once they are in
    size_t if_cond(const Insn &i) {
        Step s;
        static const uint16_t kinds[] = {K_bc, K_bc_true, K_bc_false, K_bc_dnz};
        s.k = kinds[shape(i)];
        s.rt = i.bo, s.ra = i.bi;
        return add(s);
    }
    void end_if(size_t at) { out.steps[at].a = (uint32_t)out.steps.size(); }

    void insn(uint32_t a) {
        Insn i = decode(g_p.word_at(a), a);
        if (labels.count(a)) {
            flush();
            label_at[a] = (uint32_t)out.steps.size();
        }
        {
            uint32_t prev = 0;
            bool has_prev = a - 4 >= g_p.base && a - g_p.base <= g_p.image.size();
            if (has_prev) has_prev = !hk::ppc::ends_sequence(prev = g_p.word_at(a - 4));
            pending += hk::ppc::cycles(i.word, a, has_prev, prev);
        }
        if (i.is_branch() || i.op == Op::sc || i.op == Op::rfi || i.op == Op::rfci) flush();
        const uint32_t next = a + 4;
        Step s;
        switch (i.op) {
        case Op::b:
            if (i.lk) {
                if (i.target == next) {
                    s.k = K_set_lr;
                    s.a = next;
                    add(s);
                } else {
                    call(next, i.target);
                }
            } else {
                go(a, i.target);
            }
            break;
        case Op::bc:
            if (i.lk) {
                s.k = K_set_lr;
                s.a = next;
                add(s);
                size_t at = if_cond(i);
                call(next, i.target);
                end_if(at);
            } else if (i.always()) {
                go(a, i.target);
            } else if (inside(i.target)) {
                go_if(i, a, i.target);
            } else {
                size_t at = if_cond(i);
                go(a, i.target);
                end_if(at);
            }
            break;
        case Op::bclr:
            if (i.lk) {
                s.k = K_bclrl;
                s.rt = i.bo, s.ra = i.bi;
                s.a = next;
            } else if (i.always()) {
                s.k = K_ret;
            } else {
                s.k = K_ret_if;
                s.rt = i.bo, s.ra = i.bi;
            }
            add(s);
            break;
        case Op::bcctr:
            s.rt = i.bo, s.ra = i.bi;
            s.rb = i.always();
            if (i.lk) {
                s.k = K_bcctrl;
                s.a = next;
                add(s);
            } else if (f.jumptables.count(a)) {
                s.k = K_table;
                s.y.b = (uint32_t)out.tables.size();
                auto &cases = out.tables.emplace_back();
                for (uint32_t t : std::set<uint32_t>(f.jumptables.at(a).begin(), f.jumptables.at(a).end())) {
                    cases.emplace_back(t, ~0u);
                    if (inside(t)) table_fix.emplace_back(((size_t)s.y.b << 32) | (cases.size() - 1), t);
                }
                add(s);
            } else {
                s.k = K_jump_ctr;
                add(s);
            }
            break;
        case Op::sc:
            s.k = K_sc;
            s.a = next;
            add(s);
            break;
        case Op::rfi:
            s.k = K_rfi;
            add(s);
            break;
        case Op::rfci:
            s.k = K_rfci;
            add(s);
            break;
        case Op::unimpl:
        case Op::invalid:
            s.k = K_unimpl;
            s.a = a;
            s.y.b = i.word;
            add(s);
            break;
        case Op::nop:
            break;
        default:
            add(ordinary(i));
            break;
        }
    }

    // the steps whose `a` is a step to go on at
    static bool goes(uint16_t k) {
        switch (k) {
        case K_go: case K_gop: case K_bc: case K_bc_true: case K_bc_false: case K_bc_dnz:
        case K_br: case K_br_true: case K_br_false: case K_br_dnz: case K_brp: case K_brp_true: case K_brp_false: case K_brp_dnz:
            return true;
        default:
            return false;
        }
    }

    // an ordinary instruction's step: its fields, and the shape of it (exec_steps.h, kind())
    static Step ordinary(const Insn &i) {
        Step s;
        s.rt = i.rt, s.ra = i.ra, s.rb = i.rb, s.sh = i.sh;
        s.x.f = {i.mb, i.me, i.nb, i.crm, i.to, i.e, i.oe, i.rc};
        s.y.f = {i.crfd, i.crfs, (int16_t)i.simm};   // (uimm: the same 16 bits)
        s.a = i.op == Op::tw || i.op == Op::twi ? i.addr : i.spr;                                    // (dcr: the same field)
        const unsigned n = names(i.op);
        const bool flagged = ((n & F_rc) && i.rc) || ((n & F_oe) && i.oe);
        s.k = kind(i.op, flagged ? 0 : i.ra ? 1 : 2);
        if (i.op == Op::cmpi && i.crfd == 0) s.k = K_cmpi0;
        else if (i.op == Op::cmpli && i.crfd == 0) s.k = K_cmpli0;
        else if (i.op == Op::cmp && i.crfd == 0) s.k = K_cmp0;
        else if (i.op == Op::cmpl && i.crfd == 0) s.k = K_cmpl0;
        else if (i.op == Op::mfspr && (i.spr == 8 || i.spr == 9)) s.k = i.spr == 8 ? K_mflr : K_mfctr;
        else if (i.op == Op::mtspr && (i.spr == 8 || i.spr == 9)) s.k = i.spr == 8 ? K_mtlr : K_mtctr;
        return s;
    }

    static bool falls_through(const Insn &i) {
        switch (i.op) {
        case Op::b: return i.lk && !g_p.return_up.count(i.target);
        case Op::bc: case Op::bclr: case Op::bcctr: return i.lk || !i.always();
        case Op::rfi: case Op::rfci: return false;
        default: return true;
        }
    }

    // recomp's run(entry): the whole body, entered at `entry`
    void run(uint32_t entry) {
        find_labels();
        for (uint32_t e : f.entries) labels.insert(e);
        pending = 0;
        const bool entry_first = !f.ranges.empty() && f.ranges.front().first == entry;
        poll(entry);
        if (!entry_first) {
            labels.insert(entry);
            goto_label(entry);
        }
        for (auto [lo, hi] : f.ranges) {
            for (uint32_t a = lo; a < hi; a += 4) insn(a);
            Insn last = decode(g_p.word_at(hi - 4), hi - 4);
            flush();
            if (falls_through(last)) go(hi - 4, hi);
        }
        for (auto [at, target] : go_fix) out.steps[at].a = label_at.at(target);
        for (auto [key, target] : table_fix) out.tables[key >> 32][key & 0xffffffffu].second = label_at.at(target);
        fold();
    }

    // A tick step charges the cycles of the run before it, and what follows it runs next: unless
    // something jumps to that next step (a label, a branch's "else"), the charge moves into it
    // (charged when it is reached), the same moment. The callee lists are fixed up along.
    void fold() {
        std::vector<Step> &v = out.steps;
        std::vector<char> target(v.size() + 1, 0);
        for (const Step &s : v)
            if (goes(s.k)) target[s.a] = 1;
        for (const auto &cases : out.tables)
            for (auto [val, at] : cases)
                if (at != ~0u) target[at] = 1;
        std::vector<uint32_t> to(v.size() + 1);
        std::vector<Step> w;
        uint32_t carry = 0;
        for (size_t k = 0; k < v.size(); k++) {
            to[k] = (uint32_t)w.size();
            if (v[k].k == K_tick && k + 1 < v.size() && !target[k + 1] && carry + v[k].a <= 0xffff) {
                carry += v[k].a;   // folded into the next step
                continue;
            }
            Step s = v[k];
            s.tick = (uint16_t)carry;
            carry = 0;
            w.push_back(s);
        }
        to[v.size()] = (uint32_t)w.size();
        for (Step &s : w)
            if (goes(s.k)) s.a = to[s.a];
        for (auto &cases : out.tables)
            for (auto &c : cases)
                if (c.second != ~0u) c.second = to[c.second];
        for (auto &[at, addr] : callee) at = to[at];
        v.swap(w);
    }
};


std::string prepare();
void forget();   // another program: no run remembered holds (below)

// run()'s handlers, by a step's kind: for one without cycles folded in, and for one with
const void *g_plain[KINDS], *g_ticked[KINDS];

}  // namespace

#define HK_RUN run
#define HK_RUN_FAST 1
#define HK_RUN_OUTSIDE() ((void)0)
#define HK_RUN_CALL_DYN(c, t) hk_call_dyn((c), (t))
#define HK_RUN_JUMP(c, t) hk_jump((c), (t))
#include "exec_run.inc"
#undef HK_RUN
#undef HK_RUN_FAST
#undef HK_RUN_OUTSIDE
#undef HK_RUN_CALL_DYN
#undef HK_RUN_JUMP

std::string load(const std::string &spec_path, const std::string &image) {
    g_p = spec::Program{};
    if (std::string e = spec::load(g_p, spec_path, image); !e.empty()) return e;
    return prepare();
}

std::string load_text(const std::string &spec_text, const std::string &image) {
    g_p = spec::Program{};
    if (std::string e = spec::load_text(g_p, spec_text, image); !e.empty()) return e;
    return prepare();
}

namespace {
// the loaded spec's functions turned into steps
std::string prepare() {
    forget();
    g_codes.clear();
    g_entry.clear();
    g_loaded = false;
    if (!g_plain[K_tick]) run(nullptr, nullptr);   // (the handlers' addresses)
    spec::find_return_up(g_p);
    // every function, and each secondary entry as its own copy of the body (as the recompiler
    // emits it), then the calls pointed at their callees
    std::vector<std::pair<Code *, std::vector<std::pair<size_t, uint32_t>>>> calls;
    for (auto &[e, f] : g_p.funcs) {
        std::vector<uint32_t> entries{e};
        entries.insert(entries.end(), f.entries.begin(), f.entries.end());
        for (uint32_t entry : entries) {
            auto code = std::make_unique<Code>();
            code->entry = entry;
            Builder b(f, *code);
            b.run(entry);
            g_entry[entry] = code.get();
            calls.emplace_back(code.get(), std::move(b.callee));
            g_codes.push_back(std::move(code));
        }
    }
    for (auto &[code, list] : calls)
        for (auto [at, target] : list) code->steps[at].x.fn = g_entry.at(target);
    // each step's handler in run(), now that its kind and its cycles are settled
    for (auto &code : g_codes)
        for (Step &s : code->steps) s.h = s.tick ? g_ticked[s.k] : g_plain[s.k];
    g_loaded = true;
    return "";
}
}  // namespace

bool loaded() { return g_loaded; }
size_t count() { return g_entry.size(); }
bool has(uint32_t addr) { return g_entry.count(addr) != 0; }
const spec::Program *program() { return g_loaded ? &g_p : nullptr; }

const Code *find(uint32_t addr) {
    auto it = g_entry.find(addr);
    return it == g_entry.end() ? nullptr : it->second;
}

bool call(hk_ctx *c, uint32_t addr) {
    const Code *k = find(addr);
    if (!k) return false;
    run(c, k);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Runs remembered (exec_steps.h, Memo). A Firebeat game's threads wait by giving the processor
// up: a system call each, and the kernel's scheduler looks through its tables for who is next,
// a thousand instructions to hand over to a thread that gives up in turn. Most of a frame goes
// that way, and one such call is like the one before it: the same registers in, the same tables
// read. So an exception's handler (glue.cpp's run_vector, the one place this is used) is looked
// up by the registers it starts with; met a second time it is run by the recorder, and from then
// on, wherever the bytes it read are as they were and the budget outlasts it (so that no poll
// inside would have run the scheduler), its writes, registers and cycles are put in place of
// running it. Nothing here knows a game or its kernel.
// ---------------------------------------------------------------------------------------------
namespace {

constexpr size_t SLOTS = 1024;   // states looked up, direct-mapped by the registers' hash
constexpr size_t WAYS = 4;       // runs kept for one state (the same registers, other bytes read)

struct Slot {
    uint64_t key = 0;       // the registers' hash (0: empty)
    uint32_t wait = 0;      // meetings to let pass before recording again
    uint32_t fails = 0;     // recordings since one was last used
    uint32_t strikes = 0;   // other states met here since this one's runs were last used
    uint32_t next = 0;      // the run replaced next
    std::vector<Memo> memos;
};

struct MemoStats {
    uint64_t calls = 0, used = 0, cycles = 0, recorded = 0, kept = 0, changed = 0, short_budget = 0, waited = 0, fresh = 0;
};

enum class Mode { off, on, check };

struct Memos {
    Mode mode = Mode::on;
    bool stats = false;
    std::vector<Slot> slots;
    uint32_t last_addr = 1;        // the handler looked up last (1: none, never an address of one)
    const Code *last = nullptr;
    MemoStats st;
    Memos() {
        if (const char *e = getenv("HK_EXEC_MEMO")) mode = !strcmp(e, "0") ? Mode::off : !strcmp(e, "check") ? Mode::check : Mode::on;
        stats = getenv("HK_EXEC_STATS") != nullptr;
    }
    ~Memos() {
        if (!stats || !st.calls) return;
        fprintf(stderr, "executor: %llu handlers, %llu taken from a remembered run (%.1f %%, %llu cycles), %llu recorded (%llu kept), "
                        "%llu found other bytes, %llu too near an event, %llu let pass, %llu new states\n",
                (unsigned long long)st.calls, (unsigned long long)st.used, 100.0 * (double)st.used / (double)st.calls,
                (unsigned long long)st.cycles, (unsigned long long)st.recorded, (unsigned long long)st.kept,
                (unsigned long long)st.changed, (unsigned long long)st.short_budget, (unsigned long long)st.waited,
                (unsigned long long)st.fresh);
    }
} g_memo;

void forget() {
    g_memo.slots.clear();
    g_memo.last_addr = 1, g_memo.last = nullptr;
}

// The state a handler is looked up by: every register but the general ones (the program counter
// among them: which handler), and of those the stack pointer and the first argument. The other
// general registers count where the run depended on them (Memo::used).
uint64_t hash_ctx(const hk_ctx *c) {
    const unsigned char *p = reinterpret_cast<const unsigned char *>(c) + CTX_GPRS;
    uint64_t h = 0x9e3779b97f4a7c15ull ^ ((uint64_t)c->r[1] << 32 | c->r[3]);
    size_t n = CTX_BYTES - CTX_GPRS;
    for (; n >= 8; n -= 8, p += 8) {
        uint64_t v;
        memcpy(&v, p, 8);
        h = (h ^ v) * 0xff51afd7ed558ccdull;
        h ^= h >> 29;
    }
    uint64_t v = 0;
    memcpy(&v, p, n);
    h = (h ^ v) * 0xff51afd7ed558ccdull;
    h ^= h >> 32;
    return h ? h : 1;
}

bool same_registers(const Memo &m, const hk_ctx *c) {
    if (memcmp(reinterpret_cast<const char *>(&m.in) + CTX_GPRS, reinterpret_cast<const char *>(c) + CTX_GPRS, CTX_BYTES - CTX_GPRS) != 0)
        return false;
    for (uint32_t u = m.used; u; u &= u - 1)
        if (m.in.r[__builtin_ctz(u)] != c->r[__builtin_ctz(u)]) return false;
    return true;
}

// (a stretch is a few bytes, a word or a frame's worth: compared and copied here, eight at a time,
// where a call into the C library for each costs more than the bytes do)
inline bool same_bytes(const uint8_t *a, const uint8_t *b, uint32_t n) {
    for (; n >= 8; n -= 8, a += 8, b += 8) {
        uint64_t x, y;
        memcpy(&x, a, 8), memcpy(&y, b, 8);
        if (x != y) return false;
    }
    if (n >= 4) {
        uint32_t x, y;
        memcpy(&x, a, 4), memcpy(&y, b, 4);
        if (x != y) return false;
        n -= 4, a += 4, b += 4;
    }
    for (; n; n--)
        if (*a++ != *b++) return false;
    return true;
}
inline void copy_bytes(uint8_t *to, const uint8_t *from, uint32_t n) {
    for (; n >= 8; n -= 8, to += 8, from += 8) memcpy(to, from, 8);
    if (n >= 4) memcpy(to, from, 4), n -= 4, to += 4, from += 4;
    for (; n; n--) *to++ = *from++;
}

bool same_reads(const Memo &m) {
    const uint8_t *data = m.rdata.data();
    for (const Span &r : m.reads)
        if (!same_bytes(hk_ram + r.at, data + r.data, r.n)) return false;
    return true;
}

// what the recorded run did, done: its writes, the words it moved, its registers, its cycles
void apply(hk_ctx *c, const Memo &m) {
    uint32_t moved[MAX_COPIES];   // (as RAM holds them; all fetched before anything is put down)
    size_t n = 0;
    for (const Copy &k : m.copies) {
        if (k.from_ram) memcpy(&moved[n++], hk_ram + k.from, 4);
        else moved[n++] = hk_bswap32(c->r[k.from]);
    }
    const uint8_t *data = m.wdata.data();
    for (const Span &w : m.writes) copy_bytes(hk_ram + w.at, data + w.data, w.n);
    memcpy(c, &m.out, CTX_BYTES);
    n = 0;
    for (const Copy &k : m.copies) {
        if (k.to_ram) memcpy(hk_ram + k.to, &moved[n++], 4);
        else c->r[k.to] = hk_bswap32(moved[n++]);
    }
    hk_budget -= m.cycles;
}

// the bytes of RAM a remembered run writes, as stretches in address order
std::vector<std::pair<uint32_t, uint32_t>> written(const Memo &m) {
    std::vector<std::pair<uint32_t, uint32_t>> v, out;
    for (const Span &w : m.writes) v.emplace_back(w.at, w.n);
    for (const Copy &k : m.copies)
        if (k.to_ram) v.emplace_back(k.to, 4);
    std::sort(v.begin(), v.end());
    for (auto [at, n] : v) {
        if (!out.empty() && out.back().first + out.back().second == at) out.back().second += n;
        else out.emplace_back(at, n);
    }
    return out;
}

// HK_EXEC_MEMO=check: a run that would be taken from memory is run and compared with it instead
void check(hk_ctx *c, const Code *k, const Memo &m) {
    // what taking it from memory would leave: the registers, the cycles, the bytes written
    hk_ctx want = *c;
    std::vector<uint8_t> bytes;
    {
        uint32_t moved[MAX_COPIES];
        size_t n = 0;
        for (const Copy &q : m.copies) {
            if (q.from_ram) memcpy(&moved[n++], hk_ram + q.from, 4);
            else moved[n++] = hk_bswap32(c->r[q.from]);
        }
        memcpy(&want, &m.out, CTX_BYTES);
        n = 0;
        for (const Copy &q : m.copies) {
            if (!q.to_ram) want.r[q.to] = hk_bswap32(moved[n]);
            n++;
        }
        // (the bytes: in written()'s order)
        std::vector<std::pair<uint32_t, const uint8_t *>> at;
        for (const Span &w : m.writes)
            for (uint32_t b = 0; b < w.n; b++) at.emplace_back(w.at + b, m.wdata.data() + w.data + b);
        n = 0;
        for (const Copy &q : m.copies) {
            if (q.to_ram)
                for (uint32_t b = 0; b < 4; b++) at.emplace_back(q.to + b, reinterpret_cast<const uint8_t *>(&moved[n]) + b);
            n++;
        }
        std::sort(at.begin(), at.end());
        for (auto &[a, p] : at) bytes.push_back(*p);
    }
    const int32_t budget = hk_budget;
    Memo now;
    const bool ok = record(c, k, now);
    const auto w0 = written(m), w1 = written(now);
    const char *bad = nullptr;
    if (!ok) bad = "it isn't a run that can be remembered";
    else if (budget - hk_budget != m.cycles) bad = "the cycles differ";
    else if (memcmp(&want, c, CTX_BYTES) != 0) bad = "the registers differ";
    else if (w0 != w1) bad = "it wrote other bytes of RAM";
    else {
        size_t n = 0;
        for (auto [at, len] : w0) {
            if (memcmp(hk_ram + at, bytes.data() + n, len) != 0) bad = "what it wrote differs";
            n += len;
        }
    }
    if (!bad) return;
    fprintf(stderr, "executor: the handler at %08x did not do what its remembered run did: %s\n", k->entry, bad);
    if (ok && memcmp(&want, c, CTX_BYTES) != 0) {
        const uint32_t *a = reinterpret_cast<const uint32_t *>(&want), *b = reinterpret_cast<const uint32_t *>(c);
        for (size_t q = 0; q < CTX_BYTES / 4; q++)
            if (a[q] != b[q]) fprintf(stderr, "  register word %zu: remembered %08x, run %08x\n", q, a[q], b[q]);
    }
    fatal(c, "HK_EXEC_MEMO=check failed");
}

}  // namespace

bool call_memo(hk_ctx *c, uint32_t addr) {
    Memos &g = g_memo;
    // not with a mod's code patch in (the steps are the program as it was; the patched functions
    // run interpreted), nor where the game's own binary runs its functions
    if (g.mode == Mode::off || hk_patched_any || hk_func_count || recording()) return false;
    if (addr != g.last_addr) g.last_addr = addr, g.last = find(addr);   // (a handful of vectors, one mostly)
    const Code *k = g.last;
    if (!k) return false;
    if (g.slots.empty()) g.slots.resize(SLOTS);
    g.st.calls++;
    const uint64_t h = hash_ctx(c);
    Slot &s = g.slots[(h >> 20) & (SLOTS - 1)];
    if (s.key != h) {   // a state not met before (or long ago): noted, and recorded if it comes again
        if (!s.memos.empty() && ++s.strikes < 8) return false;   // (the slot's own is still in use)
        s = Slot{};
        s.key = h;
        g.st.fresh++;
        return false;
    }
    bool other = false;
    for (const Memo &m : s.memos) {
        if (!same_registers(m, c)) continue;
        if (!same_reads(m)) { other = true; continue; }
        if (hk_budget <= m.cycles) {   // an event is due before it would end: a poll in it runs the scheduler
            g.st.short_budget++;
            return false;
        }
        if (g.mode == Mode::check) check(c, k, m);
        else apply(c, m);
        s.fails = 0, s.strikes = 0;
        g.st.used++;
        g.st.cycles += (uint64_t)m.cycles;
        return true;
    }
    if (other) g.st.changed++;
    if (s.wait) {
        s.wait--;
        g.st.waited++;
        return false;
    }
    if (hk_budget < 4096) {   // too near an event to be worth recording: a poll would end it
        g.st.short_budget++;
        return false;
    }
    Memo m;
    const bool ok = record(c, k, m);
    g.st.recorded++;
    if (ok) {
        g.st.kept++;
        if (s.memos.size() < WAYS) s.memos.push_back(std::move(m));
        else s.memos[s.next++ % WAYS] = std::move(m);
    }
    // recorded and not used since: a state whose runs differ every time (or can't be kept) is
    // looked at less and less often
    if (++s.fails > 2) s.wait = 1u << std::min<uint32_t>(s.fails, 12);
    return true;
}

}  // namespace hk::exec
