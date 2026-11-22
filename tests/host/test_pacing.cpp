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
// A modelled display (model()), a deadline that moves for a while or for good: the lead follows it
// in a few raises and stops, at 60, 120 and 240 Hz (judged by commit to screen, 240 Hz ran away).
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

// one picture shown `d` after its commit, `late` after the refresh it was meant for (by default the
// refresh BEST after the commit: a picture shown later than BEST came refreshes late)
void picture(int64_t d, bool overrun = false, int64_t late = -1) {
    if (late < 0) late = d - BEST;
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
    g_skip = 5, g_prev_t = 0, g_best = 0, g_best_late = 0, g_overrun_seen = false;
    g_window.clear();
    g_late.clear();
    g_target = 0, n_moved = 0;
    window(BEST);   // the best commit to screen
    CHECK(g_best == BEST);
}

// A modelled display, run for 90 s at `hz`: real vblanks every refresh, the compositor's deadline
// `need` before each (4.6 ms, wandering +-0.4 ms from picture to picture); a commit is shown at the
// first vblank its deadline allows, and with fifo never before the refresh after the previous held
// picture's (a picture late once, all after it late too, until one is taken unheld: g_drop). The
// presentation feedback comes at the vblank. Frames as frame() makes them (one refresh each, the
// grid started 0.3 ms off the real vblanks), commits as wait_commit() makes them (up to 0.1 ms after
// their time). `stretch`: from 20 to 30 s the deadline 2 ms further off; `moved`: from 40 s on, 3 ms
// further off for good. The excess: the last 10 s' commit to screen less the most the deadline
// needed in the run (a lead raised for a stretch stays up: what it costs is the excess)
struct Model {
    double lead_ms;        // the lead at the end
    double excess_ms;      // the last 10 s' median commit to screen, less the most the deadline needed then
    uint64_t moved;        // the lead's raises
    int catch_ups;         // refreshes skipped (the grid moved a refresh) or pictures dropped
    uint64_t moved_late;   // raises in the last 20 s (the deadline followed, then left alone)
};
Model model(double hz, bool fifo, bool exact, bool stretch, bool moved) {
    const int64_t P = (int64_t)(1e9 / hz + 0.5);
    g_active = true, g_T = P, g_T_exact = true, g_k = 1;
    g_fifo_on = fifo, g_dxgi = false, g_exact = exact, g_sweep = false;
    g_lead = P / 2, g_lead_set = true, g_settled = false, g_streak = 0, g_quiet = 0;
    g_skip = 5, g_prev_t = 0, g_best = 0, g_overrun_seen = false, g_drop = false;
    g_best_late = 0;
    g_window.clear();
    g_late.clear();
    n_moved = 0;
    uint32_t rnd = 12345;
    auto uniform = [&](double a, double b) { rnd = rnd * 1664525u + 1013904223u; return a + (b - a) * (rnd >> 8) / 16777216.0; };
    struct Shown { int64_t r, c, target; };
    std::vector<Shown> queue;   // committed, feedback not yet come
    const int64_t t0 = 1'000'000'000 / P * P;   // a real vblank
    g_target = t0 + 2 * P + 3 * MS / 10;
    int64_t last_r = 0;
    Model m{};
    std::vector<int64_t> tail;
    const int frames = (int)(90 * hz);
    for (int i = 0; i < frames; i++) {
        const double s = (double)i / hz;
        g_target += P;
        const int64_t c = g_target - g_lead + (int64_t)uniform(0, 0.1 * MS);
        for (size_t n = 0; n < queue.size();) {   // the feedback come by this commit
            if (queue[n].r > c) break;
            const int64_t before = g_target;
            const bool dropped = g_drop;
            presented(queue[n].r, queue[n].c, queue[n].target, false, P, 1);
            if (g_target - before > P / 2 || (g_drop && !dropped)) m.catch_ups++;
            if (s >= 80) tail.push_back(queue[n].r - queue[n].c);
            queue.erase(queue.begin());
        }
        if (i == (int)(70 * hz)) m.moved_late = n_moved;   // (the count at 70 s: a difference below)
        const double need = 4.6 + uniform(-0.4, 0.4) + (stretch && s >= 20 && s < 30 ? 2.0 : 0) + (moved && s >= 40 ? 3.0 : 0);
        int64_t r = (c + (int64_t)(need * MS) + P - 1) / P * P;
        const bool held = fifo && !g_drop;
        g_drop = false;   // (fifo_barrier: no late commits here)
        if (held) r = std::max(r, last_r + P);
        else if (fifo && !queue.empty() && queue.back().r >= r) queue.pop_back(), n_discarded++, g_prev_t = 0;   // taken with it
        last_r = r;
        queue.push_back({r, c, g_target});
    }
    std::nth_element(tail.begin(), tail.begin() + (long)tail.size() / 2, tail.end());
    m.lead_ms = (double)g_lead / MS;
    m.excess_ms = (double)tail[tail.size() / 2] / MS - (4.6 + 0.4 + (stretch ? 2.0 : 0) + (moved ? 3.0 : 0));
    m.moved = n_moved;
    m.moved_late = n_moved - m.moved_late;
    return m;
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

    // The modelled display (model()): the lead found, raised as far as the deadline needs, and left
    // there, at 60, 120 and 240 Hz, fifo or not, at the game's own speed or not. Now: 2 to 4 raises,
    // none in the last 20 s, the commit 0.0 to 0.9 ms ahead of the deadline at the end; at 240 Hz a
    // lead of 2.9-3.2 ms (the 2 ms stretch) and 4.4-4.7 ms (the 3 ms move). Judged by commit to screen
    // (as before 2026-11-22), 60 and 120 Hz came out the same in these 90 s, but 240 Hz ran away: 140
    // to 150 raises and a lead of 106-114 ms after the stretch, 742 to 751 raises and 558-565 ms with
    // the deadline moved (296-300 of them in the last 20 s), each raise a refresh skipped
    for (const double hz : {60.0, 120.0, 240.0})
        for (const bool fifo : {true, false})
            for (const bool exact : {false, true})
                for (const bool moved : {false, true}) {
                    const Model m = model(hz, fifo, exact, !moved, moved);
                    printf("pacing model: %3.0f Hz, %s, %s, %s: lead %5.2f ms, commit to screen %+.2f ms over the need, "
                           "moved %llu times (%llu in the last 20 s), %d refreshes skipped or pictures dropped\n",
                           hz, fifo ? "fifo   " : "no fifo", exact ? "exact speed" : "locked     ",
                           moved ? "deadline 3 ms later from 40 s" : "deadline 2 ms later 20-30 s ", m.lead_ms, m.excess_ms,
                           (unsigned long long)m.moved, (unsigned long long)m.moved_late, m.catch_ups);
                    CHECK(m.moved <= 6);
                    CHECK(m.moved_late == 0);
                    CHECK(m.excess_ms > -0.5 && m.excess_ms < 2.5);
                }

    if (g_failed) fprintf(stderr, "%d failed\n", g_failed);
    else printf("pacing: all hold (%s)\n", drain ? "the next commit dropped" : "HK_PACING_DRAIN=0: the grid moved");
    return g_failed ? 1 : 0;
}
