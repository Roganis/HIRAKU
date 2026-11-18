// SPDX-License-Identifier: GPL-3.0-or-later
// Guest hooks: the table (hooks.h).
#include "hooks.h"

#include <cstddef>
#include <cstdio>
#include <unordered_map>
#include <vector>

namespace hk::hooks {

// (hk_ctx's registers as the words a hook is given: r0-r31, cr, xer, lr, ctr in that order)
static_assert(offsetof(hk_ctx, r) == 0 && offsetof(hk_ctx, cr) == 32 * 4 && offsetof(hk_ctx, xer) == 33 * 4 &&
                  offsetof(hk_ctx, lr) == 34 * 4 && offsetof(hk_ctx, ctr) == 35 * 4,
              "a hook's registers are hk_ctx's first words");

bool g_any = false;

namespace {

struct Hook {
    Fn fn;
    void *ctx;
};
struct Slot {
    uint32_t at = 0;
    std::string label;   // the first hook's
    std::vector<Hook> hooks;
    bool placed = false;
};

std::vector<Slot> g_slots;
std::unordered_map<uint32_t, uint32_t> g_by_addr;   // address -> slot
bool g_loaded = false;

}  // namespace

bool add(uint32_t at, Fn fn, void *ctx, const std::string &label, std::string &err) {
    if (g_loaded) {
        err = "a hook is added before the program's functions are built";
        return false;
    }
    if (at & 3) {
        char b[96];
        snprintf(b, sizeof b, "0x%08x isn't an instruction's address (a multiple of 4)", at);
        err = b;
        return false;
    }
    auto [it, fresh] = g_by_addr.try_emplace(at, (uint32_t)g_slots.size());
    if (fresh) {
        g_slots.emplace_back();
        g_slots.back().at = at;
        g_slots.back().label = label;
    }
    g_slots[it->second].hooks.push_back({fn, ctx});
    g_any = true;
    return true;
}

void clear() {
    g_slots.clear();
    g_by_addr.clear();
    g_any = false;
    g_loaded = false;
}

uint32_t slot(uint32_t at) {
    if (!g_any) return ~0u;
    auto it = g_by_addr.find(at);
    return it == g_by_addr.end() ? ~0u : it->second;
}

void run(hk_ctx *c, uint32_t s) {
    for (const Hook &h : g_slots[s].hooks) h.fn(h.ctx, reinterpret_cast<uint32_t *>(c));
}

void run_at(hk_ctx *c, uint32_t at) {
    auto it = g_by_addr.find(at);
    if (it != g_by_addr.end()) run(c, it->second);
}

void placed(uint32_t s) { g_slots[s].placed = true; }

std::string unplaced() {
    std::string out;
    char b[160];
    for (const Slot &s : g_slots) {
        if (s.placed) continue;
        snprintf(b, sizeof b, "a hook at 0x%08x is in none of the program's functions: it would never run", s.at);
        out += (out.empty() ? "" : "\n") + std::string(b) + (s.label.empty() ? "" : " (" + s.label + ")");
    }
    return out;
}

void loaded(bool yes) {
    g_loaded = yes;
    if (!yes)   // (functions about to be built: none placed yet)
        for (Slot &s : g_slots) s.placed = false;
}

}  // namespace hk::hooks
