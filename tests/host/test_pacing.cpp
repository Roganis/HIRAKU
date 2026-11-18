// SPDX-License-Identifier: GPL-3.0-or-later
// Display pacing's judgement of the compositor's deadline (runtime/host/pacing.cpp, presented())
// with no display: made-up presentation feedback, as Wayland's fifo-v1 would give it, fed to the
// file's own function (the file is included here, so its state can be set and read).
//
// A miss in a window of ten pictures after a late commit is the game's (the pictures after it still
// a refresh behind), not the deadline's: caught up, and the lead neither raised nor let down, however
// many such windows come. A miss with no late commit before it raises the lead; without fifo (DXGI's
// swap chain, or none) a miss raises it at once, late commit or not. A late commit among the pictures
// skipped (after a catch-up, or after another late commit) is seen as one outside them is.
//
//   test_pacing    exit 0 when all hold (HK_PACING_DRAIN=0: the old catch-up, the grid moved a
//                  refresh, checked instead of the next commit dropped)
#include "host/pacing.cpp"

#include <cstdio>

using namespace hk::host::pacing;

static int g_failed = 0;
#define CHECK(x) \
    do { \
        if (!(x)) g_failed++, fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
    } while (0)

namespace {

constexpr int64_t T = 4'166'667;            // 240 Hz
constexpr int64_t BEST = 8 * MS;            // commit to screen at best (KWin at 240 Hz)
int64_t g_t = 1'000'000'000;                // the made-up commits' clock

// one picture shown `d` after its commit, at the refresh it was meant for (or `late` after it)
void picture(int64_t d, bool overrun = false, int64_t late = 0) {
    presented(g_t + d, g_t, g_t + d - late, overrun, T, 1);
    g_t += T;
}

// pictures until a window of ten is judged (the ones skipped first: not in it)
void window(int64_t d) {
    for (int n = 0; n < 100; n++) {
        const bool skipped = g_skip > 0;
        picture(d);
        if (!skipped && g_window.empty()) return;
    }
    CHECK(!"no window judged");
}

struct Seen {
    int64_t lead, target;
    uint64_t moved;
};
Seen now() { return {g_lead, g_target, n_moved}; }

// a window that misses (commit to screen a refresh slower than the best), after a late commit or not;
// whether the lead went up for it, the catch-up checked either way
bool miss_window(bool late) {
    while (g_skip > 0) picture(BEST);
    if (late) picture(BEST, true);
    const Seen a = now();
    g_drop = false;
    window(BEST + T);
    const Seen b = now();
    // caught up: the next commit dropped (fifo, draining), else the grid a refresh later
    if (draining()) CHECK(g_drop);
    else CHECK(b.target == a.target + T);
    if (b.moved == a.moved) {
        CHECK(b.lead == a.lead);
        return false;
    }
    CHECK(b.moved == a.moved + 1 && b.lead > a.lead);
    return true;
}

void start(bool fifo, bool dxgi = false) {
    g_active = true, g_T = T, g_T_exact = true, g_k = 1;
    g_fifo_on = fifo, g_dxgi = dxgi, g_exact = false, g_sweep = false;
    g_lead = T / 2, g_lead_set = true, g_settled = false, g_streak = 0, g_quiet = 0;
    g_skip = 5, g_prev_t = 0, g_best = 0, g_overrun_seen = false;
    g_window.clear();
    g_late.clear();
    g_target = 0, n_moved = 0;
    window(BEST);   // the best commit to screen
    CHECK(g_best == BEST);
}

}  // namespace

int main() {
    const bool drain = !getenv("HK_PACING_DRAIN") || strcmp(getenv("HK_PACING_DRAIN"), "0") != 0;

    // fifo: windows missed after late commits leave the lead where it was, and are caught up
    start(true);
    CHECK(draining() == drain);
    int64_t lead = g_lead;
    for (int k = 0; k < 10; k++) CHECK(!miss_window(true));
    CHECK(g_lead == lead);
    CHECK(n_moved == 0);

    // a miss with no late commit before it is the deadline's: the lead up (a 32nd and the margin,
    // not settled yet), and after it a window missed after a late commit holds it again
    start(true);
    CHECK(!miss_window(true));
    lead = g_lead;
    CHECK(miss_window(false));
    CHECK(g_lead == lead + T / 32 + MARGIN);
    CHECK(g_settled);
    lead = g_lead;
    CHECK(!miss_window(true));
    CHECK(g_lead == lead);

    // while the deadline is still being looked for (not settled), a window missed after a late commit
    // doesn't count towards the commit later, and starts the count of pictures for it again: with two
    // such windows to each clean one the lead stays put (they fell through to that step: the lead came
    // down a 32nd every other window, to two refreshes after the vblank, and settled there)
    start(true);
    lead = g_lead;
    for (int k = 0; k < 20; k++) {
        CHECK(!miss_window(true));
        CHECK(!miss_window(true));
        while (g_skip > 0) picture(BEST);
        picture(BEST, true);
        window(BEST);
    }
    CHECK(!g_settled);
    CHECK(g_lead == lead);
    CHECK(n_moved == 0);

    // no fifo (DXGI's swap chain has its own rule; a display without fifo): a miss is the deadline's
    start(false);
    lead = g_lead;
    CHECK(miss_window(true));
    CHECK(g_lead == lead + T / 32 + MARGIN);

    // fifo: a late commit among the pictures skipped after a catch-up is seen: the window missed after
    // it is the game's, and the lead stays. (It went unseen: the next window's miss raised the lead,
    // and with a stall every 16-20 frames at 240 Hz it went up at each, without end)
    start(true);
    CHECK(!miss_window(true));
    CHECK(g_skip == 6);
    lead = g_lead;
    picture(BEST);
    picture(BEST);
    picture(BEST, true);
    CHECK(g_overrun_seen);
    CHECK(g_skip >= 4 && g_window.empty());   // (the pictures after it may still be behind it)
    {
        const Seen a = now();
        g_drop = false;
        window(BEST + T);
        if (draining()) CHECK(g_drop);
        else CHECK(g_target == a.target + T);
        CHECK(g_lead == lead && n_moved == a.moved);
    }
    // ... and among the four skipped after another late commit, the last of them too
    start(true);
    picture(BEST, true);
    CHECK(g_skip == 4);
    picture(BEST);
    picture(BEST);
    picture(BEST);
    picture(BEST, true);
    CHECK(g_overrun_seen && g_skip == 4);
    lead = g_lead;
    CHECK(!miss_window(false));
    CHECK(g_lead == lead && n_moved == 0);
    // a late commit every fourth picture: nothing is judged (each window would be the game's), and the
    // lead stays where it was, not raised and not let down
    start(true);
    lead = g_lead;
    for (int k = 0; k < 400; k++) picture(k % 4 ? BEST + T : BEST, k % 4 == 0);
    CHECK(g_lead == lead && n_moved == 0 && g_window.empty());

    // no fifo, not DXGI (a display without fifo): a late commit among the skipped ones is one of them
    // (counted down as before), and a miss after it is still the deadline's
    start(false);
    CHECK(miss_window(false));
    CHECK(g_skip == 6);
    picture(BEST, true);
    CHECK(g_skip == 5);
    lead = g_lead;
    CHECK(miss_window(false));
    CHECK(g_lead > lead);

    // DXGI: the pictures after a late commit may be queued behind it, shown a refresh late with their
    // own commits on time; a late commit among the skipped ones keeps the four after it out of the
    // window as one outside them does
    start(false, true);
    g_skip = 6;   // (as after a catch-up)
    picture(BEST);
    picture(BEST);
    picture(BEST);
    picture(BEST);
    picture(BEST);
    picture(BEST, true);
    CHECK(g_skip == 4 && g_late.empty());
    for (int k = 0; k < 4; k++) picture(BEST, false, T);
    CHECK(g_skip == 0 && g_late.empty());

    if (g_failed) fprintf(stderr, "%d failed\n", g_failed);
    else printf("pacing: all hold (%s)\n", drain ? "the next commit dropped" : "HK_PACING_DRAIN=0: the grid moved");
    return g_failed ? 1 : 0;
}
