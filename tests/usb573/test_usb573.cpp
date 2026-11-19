// SPDX-License-Identifier: GPL-3.0-or-later
// A System 573 cabinet's USB_2_573 board (runtime/host/usb573.h) against the board's own firmware
// (tests/usb573/firmware: its core, as the board runs it) and a pretend machine: the cabinet's
// switches reach the machine, a pulse shorter than a frame too; the outputs' changes reach the
// board's pins, every one and in order; a DDR stage's panels count only once its board has heard
// the game's edges out, at a pace it follows; a board that comes late, goes and comes back; the
// outputs off when the game ends. And the 573's registers against its pins (sys573/wiredpins.h):
// MAME's lamp numbers land on the pins the cabinets' wiring has them on.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "host/usb573.h"
#include "shell/cabinet.h"
#include "shell/core.h"
#include "shell/wired573.h"
#include "sys573/seriallamps.h"
#include "sys573/wiredpins.h"

extern "C" {
#include "u573_core.h"
}

static int failures = 0;
#define CHECK(c)                                                            \
    do {                                                                    \
        if (!(c)) {                                                         \
            fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #c); \
            failures++;                                                     \
        }                                                                   \
    } while (0)

using namespace hk;
using namespace hk::host::usb573;
using Clock = std::chrono::steady_clock;

static uint64_t micros() {
    static const auto start = Clock::now();
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count() + 1000;
}

// The board: its firmware's core, with a report a millisecond and the pins' every state kept
class Board : public Link {
public:
    std::atomic<bool> plugged{true};
    std::atomic<uint32_t> switches{0};   // the cabinet's, 1 = closed
    struct Driven {
        uint64_t state, at_us;
    };

    Board() { u573_init(&m_fw, micros()); }
    bool open(const std::string &where, std::string &) override { return where == "auto" && plugged; }
    void close() override {}
    int read(uint8_t *report, int max, int) override {
        if (!plugged || max < U573_REPORT_SIZE) return -1;
        // the firmware's loop, for a millisecond: the outputs due, the switches
        const uint64_t until = micros() + 1000;
        for (;;) {
            {
                std::lock_guard<std::mutex> l(m_mutex);
                const uint64_t now = micros();
                while (u573_task(&m_fw, now, &m_drive)) m_driven.push_back({m_drive, now});
                u573_sample(&m_fw, now, switches);
                if (now >= until) {
                    u573_raw_in(&m_fw, now, report);
                    return U573_REPORT_SIZE;
                }
            }
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }
    bool write(const uint8_t *report, int n) override {
        if (!plugged) return false;
        std::lock_guard<std::mutex> l(m_mutex);
        const uint64_t now = micros();
        u573_raw_out(&m_fw, now, report, (size_t)n);
        while (u573_task(&m_fw, now, &m_drive)) m_driven.push_back({m_drive, now});   // (the firmware's loop comes by at once)
        writes++;
        return true;
    }
    std::vector<Driven> driven() {
        std::lock_guard<std::mutex> l(m_mutex);
        return m_driven;
    }
    uint64_t pins() {
        std::lock_guard<std::mutex> l(m_mutex);
        return m_drive;
    }
    bool overflowed() {
        std::lock_guard<std::mutex> l(m_mutex);
        return m_fw.status & U573_STATUS_OVERFLOW;
    }
    // Power off and on: the firmware starts again, its outputs off
    void power_cycle() {
        std::lock_guard<std::mutex> l(m_mutex);
        u573_init(&m_fw, micros());
        m_drive = 0;
    }
    std::atomic<int> writes{0};

private:
    std::mutex m_mutex;
    u573_t m_fw;
    uint64_t m_drive = 0;
    std::vector<Driven> m_driven;
};

// A pretend System 573: what the driver gives it, and the outputs' changes the test has it make
struct Machine : hk::Core {
    std::string fam;
    bool attached = false;
    uint32_t sw = 0;
    std::vector<uint32_t> seen;   // the switches, each frame
    uint8_t analog[4] = {};
    bool had_analog = false;
    std::vector<wired573::Output> changes;
    uint64_t state = 0, time_us = 1000000;

    explicit Machine(std::string family) : fam(std::move(family)) {}
    void out(uint64_t s, uint64_t after_us) {
        state = s;
        time_us += after_us;
        changes.push_back({s, time_us, stages(s)});
    }

    // A DDR machine's stage boards as the 573 core emulates them (sys573/board.cpp, MAME's
    // gn845pwbb_clk_w): data and clock are the outputs' levels, high while the pin doesn't sink;
    // a rising clock shifts the data in; 0xc90 starts a dialogue of 22 more bits
    struct Stage {
        int clk = 1, shift = 0, bit = 0;
        bool busy = false, open = false;
    } stage[2];
    uint8_t stages(uint64_t s) {
        uint8_t tag = 0;
        if (fam != "ddr") return tag;
        for (int p = 0; p < 2; p++) {
            Stage &st = stage[p];
            const int data = !(s & (p ? wired573::out_b(4) : wired573::out_a(4))), clk = !(s & (p ? wired573::out_b(5) : wired573::out_a(5)));
            if (clk && !st.clk) {
                st.shift = (st.shift >> 1) | (data << 12);
                if (!st.busy) {
                    if (st.shift == 0xc90) st.busy = true, st.bit = 0;
                } else if (++st.bit >= 22) {
                    st.busy = false, st.open = true;
                }
            }
            st.clk = clk;
            tag |= (uint8_t)((st.busy ? wired573::STAGE_BUSY << p : 0) | (st.open ? wired573::STAGE_OPEN << p : 0));
        }
        return tag;
    }

    bool init(const Options &) override { return true; }
    const std::string &family() const override { return fam; }
    void set_frame_hook(FrameHook) override {}
    void set_audio_sink(AudioSink) override {}
    void set_on_fatal(std::function<void()>) override {}
    [[noreturn]] void run() override { abort(); }
    void shutdown() override {}
    void report() override {}
    double seconds() const override { return 0; }
    const uint8_t *mem(uint32_t, uint32_t) const override { return nullptr; }
    uint64_t frame_count() const override { return 0; }
    bool set_input(const std::string &, int) override { return false; }
    void turntable_add(int, int) override {}
    const std::vector<uint8_t> &lamp_state() const override {
        static const std::vector<uint8_t> none;
        return none;
    }
    uint64_t lamp_changes() const override { return 0; }
    bool wired_attach(bool on) override {
        attached = on;
        if (on) changes.push_back({state, time_us, stages(state)});
        return true;
    }
    void wired_inputs(uint32_t switches, const uint8_t *a) override {
        sw = switches;
        seen.push_back(switches);
        had_analog = a != nullptr;
        if (a) memcpy(analog, a, 4);
    }
    void wired_outputs(std::vector<wired573::Output> &out) override {
        out.insert(out.end(), changes.begin(), changes.end());
        changes.clear();
    }
};

// A machine without a 573's connectors: another board's, or the game menu in a 573's cabinet
struct Other : Machine {
    std::vector<std::string> inputs;
    explicit Other(std::string family = "popn") : Machine(std::move(family)) {}
    bool wired_attach(bool) override { return false; }
    bool set_input(const std::string &name, int value) override {
        inputs.push_back(name + "=" + std::to_string(value));
        return true;
    }
};

static void frames(Machine &m, int n) {
    for (int k = 0; k < n; k++) {
        host::usb573_lamps(m);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        host::usb573_frame(m);
    }
}

static bool frames_until(Machine &m, const std::function<bool()> &done, int max = 300) {
    for (int k = 0; k < max && !done(); k++) frames(m, 1);
    return done();
}

static Board *start(Machine &m, bool plugged = true) {
    auto board = std::make_unique<Board>();
    Board *b = board.get();
    b->plugged = plugged;
    use_link(std::move(board));
    CHECK(host::usb573_option("--usb573", [] { return std::string("auto"); }));
    host::usb573_start(m);
    return b;
}

using namespace hk::wired573;

// ---- the reports, against the firmware's reading of them -------------------------------------
static void test_reports() {
    u573_t fw;
    uint64_t drive = 0;
    u573_init(&fw, 0);
    const Entry e[3] = {{out_a(0) | out_d(3), 0}, {out_ext(7) | OUT_COUNTER1, 505}, {~0ull, 700000}};
    const auto r = outputs_report(e, 3);
    u573_raw_out(&fw, 1000, r.data(), r.size());
    CHECK(fw.count == 3);
    CHECK(fw.fifo[0].state == (U573_OUT_A(0) | U573_OUT_D(3)) && fw.fifo[0].delay == 0);
    CHECK(fw.fifo[1].state == (U573_OUT_EXT(7) | U573_OUT_COUNTER1) && fw.fifo[1].delay == 51);   // rounded up: never sooner
    CHECK(fw.fifo[2].state == U573_OUT_ALL && fw.fifo[2].delay == 0xffff);
    CHECK(OUT_ALL == U573_OUT_ALL && (OUT_STAGE1 | OUT_STAGE2) == (U573_OUT_A(4) | U573_OUT_A(5) | U573_OUT_B(4) | U573_OUT_B(5)));
    CHECK(OUT_CART == U573_OUT_CART_ALL && out_cart(4) == U573_OUT_CART(4) && out_cart(7) == U573_OUT_CART(7));
    CHECK(u573_task(&fw, 1000, &drive) && drive == e[0].state);

    const auto flush = outputs_report(e, 1, true);
    u573_raw_out(&fw, 1100, flush.data(), flush.size());
    CHECK(fw.count == 1);
    const auto cfg = config_report(250, 1500, 300);
    u573_raw_out(&fw, 1200, cfg.data(), cfg.size());
    CHECK(fw.debounce_us == 250 && fw.failsafe_ms == 1500 && fw.counter_max_ms == 300);
    const auto reset = reset_report();
    u573_raw_out(&fw, 1300, reset.data(), reset.size());
    CHECK(fw.count == 0 && fw.outputs == 0);
    const auto alive = keepalive_report();
    u573_raw_out(&fw, 900000, alive.data(), alive.size());
    CHECK(u573_host_active(&fw, 1800000));

    // the board's report, and the switches' names on both sides
    u573_sample(&fw, 2000000, U573_SW_P1_LEFT | U573_SW_P2_START | U573_SW_TEST | U573_SW_COIN2 | U573_SW_OPT3);
    const uint8_t ch[4] = {1, 2, 3, 4};
    u573_set_analog(&fw, ch);
    uint8_t in[U573_REPORT_SIZE];
    u573_raw_in(&fw, 2000000, in);
    In got;
    CHECK(parse_in(in, sizeof in, got));
    CHECK(got.version == 1 && got.capacity == U573_FIFO_SIZE && got.queued == 0);
    CHECK(got.switches == (P1_LEFT | P2_START | TEST | COIN2 | U573_SW_OPT3));
    CHECK(!memcmp(got.analog, ch, 4));
    CHECK(P1_STAGE == (U573_SW_P1_LEFT | U573_SW_P1_RIGHT | U573_SW_P1_UP | U573_SW_P1_DOWN) && SERVICE == U573_SW_SERVICE &&
          P2_B6 == U573_SW_P2_B6 && CABINET == U573_SW_CABINET);
    in[0] = 0;
    CHECK(!parse_in(in, sizeof in, got) && !parse_in(in, 10, got));
    CHECK(VID == U573_USB_VID && PID == U573_USB_PID && USAGE_PAGE == U573_HID_USAGE_PAGE && REPORT == U573_REPORT_SIZE);
    CHECK(std::string(U573_USB_PRODUCT).rfind(PRODUCT, 0) == 0);
}

// ---- a DrumMania cabinet: switches and lamps, the board going and coming back -----------------
static void test_cabinet() {
    Machine m("drmn");
    m.state = out_a(0) | out_b(6);   // the hi-hat's lamp and Start's, lit before the board is there
    Board *b = start(m);
    CHECK(m.attached);
    CHECK(host::usb573_owns(VID, PID, "USB_2_573 System 573 cabinet interface"));
    CHECK(!host::usb573_owns(VID, PID, "Somebody's test device") && !host::usb573_owns(0x1ccf, 0x8048, "USB_2_573"));

    // the outputs as they stand go to the board
    CHECK(frames_until(m, [&] { return b->pins() == (out_a(0) | out_b(6)); }));
    // the cabinet's switches: the stage lines are the drum pads here, never held back
    b->switches = P1_LEFT | P1_B1 | TEST;
    CHECK(frames_until(m, [&] { return m.sw == (P1_LEFT | P1_B1 | TEST); }));
    CHECK(m.had_analog);
    b->switches = 0;
    CHECK(frames_until(m, [&] { return m.sw == 0; }));
    // a pad's pulse, shorter than a frame: seen, for one frame at least
    m.seen.clear();
    frames(m, 1);
    b->switches = P1_UP;
    std::this_thread::sleep_for(std::chrono::milliseconds(4));
    b->switches = 0;
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    frames(m, 3);
    int hits = 0;
    for (uint32_t s : m.seen) hits += s == P1_UP;
    CHECK(hits >= 1 && hits <= 2 && m.sw == 0);

    // lamps: each change of a frame on the pins, in order, with EXT-OUT and a coin counter
    const size_t before = b->driven().size();
    const uint64_t seq[5] = {out_a(1), out_a(1) | out_c(7), out_ext(0) | out_ext(7), OUT_COUNTER1 | out_d(0), 0};
    for (uint64_t s : seq) m.out(s, 100);
    CHECK(frames_until(m, [&] { return b->driven().size() >= before + 5; }));
    auto d = b->driven();
    CHECK(d.size() == before + 5);
    for (size_t k = 0; k < 5 && before + k < d.size(); k++) CHECK(d[before + k].state == seq[k]);

    // nothing changes for two seconds: the lamps stay lit (the board's failsafe is kept away)
    m.out(out_b(0) | out_b(2), 100);
    frames(m, 125);
    CHECK(b->pins() == (out_b(0) | out_b(2)));

    // more changes at once than the board's queue holds: none is dropped by it, the last state stands
    for (int k = 0; k < 600; k++) m.out(k & 1 ? out_c(k % 8) : out_c(0) | out_a(k % 8), 20);
    m.out(out_d(2), 20);
    CHECK(frames_until(m, [&] { return b->pins() == out_d(2); }));
    CHECK(!b->overflowed());

    // the board unplugged: every switch open for the game; plugged again (power-cycled, its
    // outputs off): the outputs as the game has them now
    b->switches = P1_B2 | SERVICE;
    CHECK(frames_until(m, [&] { return m.sw == (P1_B2 | SERVICE); }));
    b->plugged = false;
    CHECK(frames_until(m, [&] { return m.sw == 0; }));
    CHECK(!m.had_analog);
    m.out(out_a(3) | out_ext(4), 100);   // a change while it is away
    frames(m, 10);
    b->power_cycle();
    b->plugged = true;
    CHECK(frames_until(m, [&] { return b->pins() == (out_a(3) | out_ext(4)) && m.sw == (P1_B2 | SERVICE); }));

    // the game ends: nothing stays lit
    host::usb573_stop();
    CHECK(b->pins() == 0);
}

// ---- a DDR cabinet: the stages' boards ---------------------------------------------------------
// The game's side of the stage bus: `bits` bits of `word` (the lowest first, round and round), each
// put on the data line and clocked (the clock pin sinks, then lets go: the edge that counts),
// `gap` microseconds of the machine's time between edges, a lamp changing among them. The edges it
// makes are added to `edges`. 0xc90 and 22 more bits are a dialogue (35 bits); zeroes are a bus
// clocked with nothing said.
static void speak(Machine &m, int p, int bits, std::vector<uint64_t> &edges, int word = 0xc90, uint64_t gap = 40) {
    const uint64_t data = p ? out_b(4) : out_a(4), clock = p ? out_b(5) : out_a(5);
    uint64_t s = m.state;
    for (int k = 0; k < bits; k++) {
        const uint64_t want = (word >> (k % 13) & 1) ? 0 : data;   // (a 1 is the pin let go)
        if ((s & data) != want) m.out(s = (s & ~data) | want, gap), edges.push_back(s);
        m.out(s |= clock, gap), edges.push_back(s);
        if (k == 5) m.out(s ^= out_c(2), gap);   // (a lamp, no edge of the stage's)
        m.out(s &= ~clock, gap), edges.push_back(s);
    }
}

// What the board drove on stage p's lines, as its states where they changed, and the least time between two
static std::vector<uint64_t> stage_edges(const std::vector<Board::Driven> &d, size_t from, uint64_t lines, uint64_t &least) {
    std::vector<uint64_t> out;
    uint64_t last = from ? d[from - 1].state & lines : 0, at = 0;
    least = UINT64_MAX;
    for (size_t k = from; k < d.size(); k++) {
        if ((d[k].state & lines) == last) continue;
        last = d[k].state & lines;
        if (at) least = std::min(least, d[k].at_us - at);
        at = d[k].at_us;
        out.push_back(last);
    }
    return out;
}

static void test_ddr() {
    Machine m("ddr");
    Board *b = start(m);
    CHECK(frames_until(m, [&] { return b->writes > 0; }));

    // before the game has spoken to them, the stages' boards hold right and up: not panels yet.
    // The buttons beside them count as ever.
    b->switches = P1_RIGHT | P1_UP | P2_RIGHT | P2_UP | P1_START | P2_B1 | COIN1;
    frames(m, 12);
    CHECK(m.sw == (P1_START | P2_B1 | COIN1));

    // the game speaks to stage 1: 35 bits. Every edge reaches the board's pins, in order, no two
    // closer than half a millisecond, and while they are on their way the panels still don't count
    const size_t before = b->driven().size();
    std::vector<uint64_t> edges;
    speak(m, 0, 35, edges);
    const size_t n_edges = edges.size();
    CHECK(n_edges > 70);
    b->switches = P1_LEFT | P2_LEFT | P1_START;
    m.seen.clear();
    frames(m, 2);
    CHECK(m.sw == P1_START);
    uint64_t least;
    CHECK(frames_until(m, [&] { return stage_edges(b->driven(), before, OUT_STAGE1, least).size() >= n_edges; }));
    auto got = stage_edges(b->driven(), before, OUT_STAGE1, least);
    CHECK(got.size() == n_edges);
    for (size_t k = 0; k < n_edges && k < got.size(); k++) CHECK(got[k] == (edges[k] & OUT_STAGE1));
    CHECK(least >= STAGE_GAP_MIN);
    CHECK((b->pins() & out_c(2)) != 0);   // the lamp among them came through too
    // ... then stage 1's panels count; stage 2, which the game hasn't spoken to, still doesn't
    CHECK(frames_until(m, [&] { return m.sw == (P1_LEFT | P1_START); }));
    // (a tenth of a second after its board had the last edge, by the board's own clock)
    uint64_t last_edge = 0;
    {
        const auto d = b->driven();
        for (size_t k = before; k < d.size(); k++)
            if (((d[k].state ^ (k ? d[k - 1].state : 0)) & OUT_STAGE1) != 0) last_edge = d[k].at_us;
    }
    CHECK(last_edge && micros() - last_edge >= (uint64_t)(STAGE_SETTLE * 1e6));
    b->switches = P1_LEFT | P1_DOWN | P2_LEFT | P2_DOWN;
    CHECK(frames_until(m, [&] { return m.sw == (P1_LEFT | P1_DOWN); }));

    // stage 2 in its turn; stage 1's panels go on counting meanwhile
    const size_t before2 = b->driven().size();
    std::vector<uint64_t> edges2;
    speak(m, 1, 35, edges2);
    frames(m, 1);
    CHECK(m.sw == (P1_LEFT | P1_DOWN));
    CHECK(frames_until(m, [&] { return m.sw == (P1_LEFT | P1_DOWN | P2_LEFT | P2_DOWN); }));
    got = stage_edges(b->driven(), before2, OUT_STAGE2, least);
    CHECK(got.size() == edges2.size() && least >= STAGE_GAP_MIN);
    for (size_t k = 0; k < edges2.size() && k < got.size(); k++) CHECK(got[k] == (edges2[k] & OUT_STAGE2));

    // the game speaks to stage 1 again (test mode's check): its panels are held back at once, and
    // count again when that dialogue is over and on the board
    std::vector<uint64_t> again;
    speak(m, 0, 35, again);
    frames(m, 1);
    CHECK(m.sw == (P2_LEFT | P2_DOWN));
    CHECK(frames_until(m, [&] { return m.sw == (P1_LEFT | P1_DOWN | P2_LEFT | P2_DOWN); }));

    // the bus clocked with nothing said, as a game does while it is played, at a 573's own pace
    // (3 ms an edge): the panels go on counting all along, and the edges reach the board in order
    // at that pace, not held up and not bunched
    const size_t before3 = b->driven().size();
    std::vector<uint64_t> idle;
    const uint64_t began = micros();
    for (int burst = 0; burst < 10; burst++) {   // (a frame's worth of the game's time at a time, as the core makes them)
        speak(m, 0, 2, idle, 0, 3000);
        frames(m, 1);
        CHECK(m.sw == (P1_LEFT | P1_DOWN | P2_LEFT | P2_DOWN));
    }
    CHECK(frames_until(m, [&] { return stage_edges(b->driven(), before3, OUT_STAGE1, least).size() >= idle.size(); }));
    got = stage_edges(b->driven(), before3, OUT_STAGE1, least);
    CHECK(got.size() == idle.size());
    for (size_t k = 0; k < idle.size() && k < got.size(); k++) CHECK(got[k] == (idle[k] & OUT_STAGE1));
    CHECK(least >= 2900);                                          // the game's 3 ms, not the least allowed
    CHECK(micros() - began < 10 * 16000 + 1500000);                // ... and not falling behind the game
    CHECK(m.sw == (P1_LEFT | P1_DOWN | P2_LEFT | P2_DOWN));
    CHECK(!b->overflowed());
    host::usb573_stop();
    CHECK(b->pins() == 0);
}

// The board plugged in after the game has spoken to its stages: it is told all of it then
static void test_ddr_late() {
    Machine m("ddr");
    Board *b = start(m, false);
    std::vector<uint64_t> edges, edges2;
    speak(m, 0, 35, edges);
    for (int k = 0; k < 40; k++) m.out(m.state ^ out_c(4), 16000), frames(m, 1);   // lamps blink meanwhile
    speak(m, 1, 35, edges2);
    frames(m, 20);
    CHECK(m.sw == 0 && b->driven().empty());

    b->switches = P1_UP | P2_DOWN;
    b->plugged = true;
    CHECK(frames_until(m, [&] { return m.sw == (P1_UP | P2_DOWN); }));
    uint64_t least, least2;
    const auto got = stage_edges(b->driven(), 0, OUT_STAGE1, least), got2 = stage_edges(b->driven(), 0, OUT_STAGE2, least2);
    CHECK(got.size() == edges.size() && got2.size() == edges2.size());
    for (size_t k = 0; k < edges.size() && k < got.size(); k++) CHECK(got[k] == (edges[k] & OUT_STAGE1));
    for (size_t k = 0; k < edges2.size() && k < got2.size(); k++) CHECK(got2[k] == (edges2[k] & OUT_STAGE2));
    CHECK(least >= STAGE_GAP_MIN && least2 >= STAGE_GAP_MIN);
    CHECK((b->pins() & ~(OUT_STAGE1 | OUT_STAGE2)) == (m.state & ~(OUT_STAGE1 | OUT_STAGE2)));
    host::usb573_stop();
}

// A Step Champ cabinet: its lamps hang on the security cartridge, which takes them as serial words
// on the slot's d5 (clock), d6 (reset) and d7 (data). Every edge reaches the slot in order, however
// many come at once and whatever lamps change among them.
static void test_cartridge() {
    Machine m("stepchmp");
    Board *b = start(m);
    CHECK(frames_until(m, [&] { return b->writes > 0; }));
    b->switches = P1_LEFT | P1_UP | P1_B2;   // the third player's red and Start, the first's green: no stage here
    CHECK(frames_until(m, [&] { return m.sw == (P1_LEFT | P1_UP | P1_B2); }));

    const size_t before = b->driven().size();
    std::vector<uint64_t> edges;
    uint64_t s = m.state;
    auto put = [&](uint64_t now) { m.out(s = now, 30), edges.push_back(now & OUT_CART); };
    for (int word = 0; word < 12; word++) {          // twelve words in one frame: some 300 edges
        put(s | out_cart(6)), put(s & ~out_cart(6)); // reset
        for (int k = 7; k >= 0; k--) {
            const uint64_t want = ((0xa5 + word) >> k & 1) ? out_cart(7) : 0;
            if ((s & out_cart(7)) != want) put((s & ~out_cart(7)) | want);
            put(s | out_cart(5)), put(s & ~out_cart(5));
            if (k == 4) m.out(s ^= out_a(word % 8), 30);   // (a lamp of the I/O board's, were there one)
        }
    }
    CHECK(edges.size() > 250);
    uint64_t least;
    CHECK(frames_until(m, [&] { return stage_edges(b->driven(), before, OUT_CART, least).size() >= edges.size(); }));
    const auto got = stage_edges(b->driven(), before, OUT_CART, least);
    CHECK(got.size() == edges.size());
    for (size_t k = 0; k < edges.size() && k < got.size(); k++) CHECK(got[k] == edges[k]);
    CHECK(frames_until(m, [&] { return b->pins() == m.state; }));
    CHECK(!b->overflowed());
    host::usb573_stop();
    CHECK(b->pins() == 0);
}

// A long time without the board, the game clocking a stage's bus all the while: what waits for the
// board stays small, and still has the whole dialogue in it
static void test_ddr_away() {
    Machine m("ddr");
    Board *b = start(m, false);
    std::vector<uint64_t> edges, idle;
    speak(m, 0, 35, edges);
    frames(m, 2);
    for (int k = 0; k < 60; k++) {   // 60 frames of a clocked bus and blinking lamps: 600 changes and more
        speak(m, 0, 3, idle, 0, 3000);
        m.out(m.state ^ out_c(4), 100);
        frames(m, 1);
    }
    CHECK(idle.size() > 300);
    b->switches = P1_DOWN;
    b->plugged = true;
    CHECK(frames_until(m, [&] { return m.sw == P1_DOWN; }, 600));
    uint64_t least;
    const auto d = b->driven();
    const auto got = stage_edges(d, 0, OUT_STAGE1, least);
    CHECK(got.size() >= edges.size() && got.size() < edges.size() + 130);   // the dialogue, and the bus's last moves
    for (size_t k = 0; k < edges.size() && k < got.size(); k++) CHECK(got[k] == (edges[k] & OUT_STAGE1));
    CHECK(d.size() < edges.size() + 160);
    CHECK(frames_until(m, [&] { return b->pins() == m.state; }));
    host::usb573_stop();
}

// A machine that isn't a System 573, and no option at all: nothing is started
static void test_refused() {
    Other o;
    Board *b = start(o);
    frames(o, 5);
    CHECK(b->writes == 0 && o.seen.empty());
    CHECK(!host::usb573_owns(VID, PID, "USB_2_573 System 573 cabinet interface"));   // its gamepad is anybody's then
    host::usb573_stop();

    Machine m("ddr");
    auto board = std::make_unique<Board>();
    b = board.get();
    use_link(std::move(board));
    CHECK(host::usb573_option("--usb573", [] { return std::string("off"); }));
    host::usb573_start(m);
    frames(m, 5);
    CHECK(b->writes == 0 && !m.attached && m.seen.empty());
    host::usb573_stop();
    std::string help = host::usb573_help();
    CHECK(help.find("--usb573 ") != std::string::npos && !host::usb573_option("--popn-io", [] { return std::string(); }));
}

// The game menu in a 573's cabinet: the cabinet's switches by the names of its controls
static void test_menu() {
    auto took = [](Other &o, const std::string &what) {
        for (auto &i : o.inputs)
            if (i == what) return true;
        return false;
    };
    {
        Other menu("drmn");
        Board *b = start(menu);
        b->switches = P1_UP | P2_RIGHT | P1_START | TEST;   // the hi-hat, Select R, Start, Test
        CHECK(frames_until(menu, [&] { return took(menu, "Test=1"); }));
        CHECK(took(menu, "Hi-Hat=1") && took(menu, "Select R=1") && took(menu, "1 Player Start=1"));
        CHECK(menu.inputs.size() == 4 && menu.seen.empty());
        b->switches = P1_START;
        CHECK(frames_until(menu, [&] { return took(menu, "Hi-Hat=0") && took(menu, "Select R=0") && took(menu, "Test=0"); }));
        CHECK(!took(menu, "1 Player Start=0"));
        host::usb573_stop();
        CHECK(b->pins() == 0);
    }
    {
        // a DDR cabinet whose stages no game has spoken to yet: stage 1's board holds right and up,
        // which are no panels; stage 2 has been spoken to, and its panels choose
        Other menu("ddr");
        Board *b = start(menu);
        b->switches = P1_RIGHT | P1_UP | P1_LEFT | P2_DOWN | P2_START;
        CHECK(frames_until(menu, [&] { return took(menu, "2 Players Start=1"); }));
        CHECK(took(menu, "P2 Down=1") && menu.inputs.size() == 2);
        host::usb573_stop();
    }
    {
        // Step Champ: the third player's controls are on player 1's joystick pins
        Other menu("stepchmp");
        Board *b = start(menu);
        b->switches = P1_LEFT | P1_UP | P2_B3;
        CHECK(frames_until(menu, [&] { return took(menu, "P2 Blue=1"); }));
        CHECK(took(menu, "P3 Red=1") && took(menu, "3 Players Start=1") && menu.inputs.size() == 3);
        host::usb573_stop();
    }
    {
        // a conga pad's two sensors are one control; a DDR Solo panel's too
        Other menu("mambo");
        Board *b = start(menu);
        b->switches = P1_B2;
        CHECK(frames_until(menu, [&] { return took(menu, "Left Pad 1=1"); }));
        b->switches = P1_B2 | P2_START;
        frames(menu, 4);
        b->switches = P2_START;
        frames(menu, 4);
        CHECK(menu.inputs.size() == 1);
        b->switches = 0;
        CHECK(frames_until(menu, [&] { return took(menu, "Left Pad 1=0"); }));
        host::usb573_stop();
    }
}

// ---- the 573's registers and its pins ---------------------------------------------------------
// A lamp of a family's table (shell/cabinet.cpp: MAME's output number) by name
static int lamp_number(const std::string &family, const std::string &name) {
    for (auto &l : cab::lamps(family))
        if (l.name == name) return l.bit;
    return -1;
}

// MAME's gx700pwbf_output (ksys573.cpp): output number bank * 8 + i is the latch's bit shift[i].
// The pin an analog board's output number is on, through the latch's wiring.
static uint64_t analog_pin(int number) {
    static const int shift[8] = {7, 6, 1, 0, 5, 4, 3, 2};
    uint64_t mask;
    return k573::pins::analog_bank(number / 8, (uint8_t)~(1u << shift[number % 8]), mask);
}

static void test_pins() {
    using namespace k573::pins;
    uint64_t mask;
    // a latch of all ones grounds nothing, of all zeroes its whole bank (four outputs on D)
    CHECK(analog_bank(0, 0xff, mask) == 0 && mask == 0xffull);
    CHECK(analog_bank(1, 0x00, mask) == 0xff00ull && mask == 0xff00ull);
    CHECK(analog_bank(3, 0x00, mask) == 0x0f000000ull && mask == 0x0f000000ull);
    CHECK(analog_bank(2, 0x7f, mask) == out_c(0) && analog_bank(2, 0xfe, mask) == out_c(1));   // bit 7: C0; bit 0: C1
    CHECK(analog_bank(3, 0xfe, mask) == out_d(3) && analog_bank(3, 0xf7, mask) == out_d(0));
    CHECK(digital_pin(24) == 0 && digital_pin(27) == 0 && digital_pin(32) == 0);            // the register k573dio has no pins for

    // Dance Dance Revolution, as its cabinet is wired (psx-spx, "DDR light mapping"): A0-A3 player
    // 1's up, down, left, right arrows, A4 and A5 data and clock to its stage; B the same for
    // player 2; C2 and C3 the players' buttons; C4-C7 the marquee's bottom right, top right, bottom
    // left, top left; D0 the speakers' neon. On both I/O boards.
    const std::map<std::string, uint64_t> ddr = {
        {"P1 Foot Up", out_a(0)}, {"P1 Foot Down", out_a(1)}, {"P1 Foot Left", out_a(2)}, {"P1 Foot Right", out_a(3)},
        {"P2 Foot Up", out_b(0)}, {"P2 Foot Down", out_b(1)}, {"P2 Foot Left", out_b(2)}, {"P2 Foot Right", out_b(3)},
        {"P1 Start Lamp", out_c(2)}, {"P2 Start Lamp", out_c(3)},
        {"Body Right Low", out_c(4)}, {"Body Right High", out_c(5)}, {"Body Left Low", out_c(6)}, {"Body Left High", out_c(7)},
    };
    for (auto &[name, pin] : ddr) {
        const int n = lamp_number("ddr", name);
        CHECK(n >= 0 && analog_pin(n) == pin);
        CHECK(n >= 0 && digital_pin((unsigned)n) == pin);
    }
    CHECK(analog_pin(30) == out_d(0) && digital_pin(28) == out_d(0));   // the speakers' neon: 30 on the analog board, 28 on the digital
    // the stages' lines (MAME's ddr_output_callback: 4 and 7 data and clock, 12 and 15 player 2's)
    CHECK((analog_pin(4) | analog_pin(7)) == OUT_STAGE1 && (analog_pin(12) | analog_pin(15)) == OUT_STAGE2);
    CHECK((digital_pin(4) | digital_pin(7)) == OUT_STAGE1 && (digital_pin(12) | digital_pin(15)) == OUT_STAGE2);
    CHECK(analog_pin(4) == out_a(4) && digital_pin(15) == out_b(5));

    // DrumMania from 2nd Mix on (psx-spx, "DrumMania light mapping", the later cabinets): A0-A3
    // hi-hat, snare, high tom, low tom; B0 spotlight, B1 bottom neon, B2 top neon, B4 cymbal, B6
    // Start, B7 Select
    const std::map<std::string, uint64_t> drmn = {
        {"Hi-Hat", out_a(0)}, {"Snare", out_a(1)}, {"High Tom", out_a(2)}, {"Low Tom", out_a(3)},
        {"Spot Left/Right", out_b(0)}, {"Neon Woofer", out_b(1)}, {"Neon Top", out_b(2)},
        {"Cymbal", out_b(4)}, {"Start Lamp", out_b(6)}, {"Select Lamp", out_b(7)},
    };
    for (auto &[name, pin] : drmn) {
        const int n = lamp_number("drmn", name);
        CHECK(n >= 0 && digital_pin((unsigned)n) == pin);
    }
    // DrumMania 1st, on the analog board (the first cabinets): C0-C4 hi-hat, snare, high tom, low
    // tom, cymbal; C6 Start, C7 Select; D0 spotlight, D1 top neon, D3 bottom neon. MAME's analog
    // output numbers for them, as the board's drmn_output_callback takes them (sys573/board.cpp).
    const std::map<int, uint64_t> drmn1 = {{16, out_c(0)}, {19, out_c(1)}, {17, out_c(2)}, {18, out_c(3)}, {20, out_c(4)},
                                           {21, out_c(6)}, {22, out_c(7)}, {30, out_d(0)}, {31, out_d(1)}, {27, out_d(3)}};
    for (auto &[n, pin] : drmn1) CHECK(analog_pin(n) == pin);

    // Step Champ's lamps: a word on the cassette's shift registers, the first bit the highest
    // (MAME's stepchmp_lamp_clock: bits 7-5 the Start lamps, 3-1 the halogens), lamp k its bit k
    {
        k573::SerialLamps lamps;
        int word = -1, words = 0;
        auto send = [&](int w, int bits) {
            lamps.reset(1), lamps.reset(0);
            for (int k = bits - 1; k >= 0; k--) {
                lamps.set_data(w >> k & 1);
                words += lamps.clock(1, 8, word);
                words += lamps.clock(1, 8, word);   // (held high: no second edge)
                words += lamps.clock(0, 8, word);
            }
        };
        send(0xa8, 8);
        CHECK(words == 1 && word == 0xa8);
        CHECK(lamp_number("stepchmp", "P1 Start Lamp") == 7 && lamp_number("stepchmp", "P3 Start Lamp") == 5 &&
              lamp_number("stepchmp", "Halogen 1") == 3 && lamp_number("stepchmp", "Halogen 3") == 1);
        send(0x07, 5);   // a word cut short, then started afresh by the reset
        CHECK(words == 1);
        send(0x4e, 8);
        CHECK(words == 2 && word == 0x4e);
    }
    // ... and its three players on the 573's pins (MAME's hyperbbc ports): the third on player 1's joystick
    CHECK(inputs(P1_B1 | P1_B2 | P1_B3).in2 == 0x7000u && inputs(P2_B1 | P2_B2 | P2_B3).in2 == 0x0070u);
    CHECK(inputs(P1_LEFT | P1_RIGHT | P1_UP | P1_DOWN).in2 == 0x0f00u);   // 3P red, blue, Start, green

    // the switches on the ASIC's registers (psx-spx: 0x1f400008's bits 0-7 player 2's left, right,
    // up, down, buttons 1-3, start, 8-15 player 1's; 0x1f40000c's 8-11 player 1's buttons 4 and 5,
    // Test, button 6; 0x1f40000e's the same for player 2; 0x1f400006's 8, 9 the coins, 12 Service)
    Inputs i = inputs(P1_LEFT | P1_START | P2_RIGHT | P2_B3);
    CHECK(i.in2 == (0x0100u | 0x8000u | 0x0002u | 0x0040u) && i.in1 == 0 && i.in3 == 0);
    i = inputs(P1_B4 | P1_B5 | P1_B6 | TEST);
    CHECK(i.in3 == 0x0f00u && i.in2 == 0 && i.in1 == 0);
    i = inputs(P2_B4 | P2_B5 | P2_B6);
    CHECK(i.in3 == 0x0b000000u);   // (0x1f40000e's bit 10 is no switch: the main RAM's layout)
    i = inputs(COIN1 | COIN2 | SERVICE);
    CHECK(i.in1 == 0x13000000u && i.in2 == 0 && i.in3 == 0);
    CHECK(inputs(0).in1 == 0 && inputs(0x3c000000u).in2 == 0 && inputs(0x3c000000u).in3 == 0);   // the board's option switches: no pin
}

int main() {
    test_reports();
    test_pins();
    test_cabinet();
    test_ddr();
    test_ddr_late();
    test_ddr_away();
    test_cartridge();
    test_menu();
    test_refused();
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("usb573: all checks passed\n");
    return 0;
}
