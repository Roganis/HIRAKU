// SPDX-License-Identifier: GPL-3.0-or-later
// The performance overlay (runtime/host/statsview.h, sysload.h) without a window: what it reads of
// the system's files (made-up ones), its panel drawn from made-up frames, its levels and its place
// (the key, the corners, a drag with the mouse, the place kept in a file and read back).
//
//   test_statsview DIR                 DIR: a folder to make files in. Exit 0 when all hold
//   test_statsview DIR --ppm FILE LEVEL   the panel at LEVEL (line, panel, full) over grey, as a PPM
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "host/statsview.h"
#include "host/sysload.h"
#include "shell/core.h"

namespace {

using namespace hk::host;

// a machine with lines of its own for the overlay
class Machine final : public hk::Core {
public:
    bool init(const Options &) override { return true; }
    const std::string &family() const override { static const std::string f = "popn"; return f; }
    void set_frame_hook(FrameHook) override {}
    void set_audio_sink(AudioSink) override {}
    void set_on_fatal(std::function<void()>) override {}
    [[noreturn]] void run() override { abort(); }
    void shutdown() override {}
    void report() override {}
    double seconds() const override { return 0; }
    const uint8_t *mem(uint32_t, uint32_t) const override { return nullptr; }
    uint64_t frame_count() const override { return 0; }
    bool set_input(const std::string &, int) override { return true; }
    void turntable_add(int, int) override {}
    const std::vector<uint8_t> &lamp_state() const override { static const std::vector<uint8_t> none; return none; }
    uint64_t lamp_changes() const override { return 0; }
    void overlay_lines(std::vector<std::string> &lines) override { asked++, lines.push_back("the machine's own line"); }
    int asked = 0;
};
Machine g_machine;

int failed = 0;
void check(bool ok, const char *what) {
    printf("%s%s\n", ok ? "ok    " : "FAIL  ", what);
    failed += !ok;
}

uint64_t g_now = 1'000'000'000ull;   // the overlay's clock, moved by hand
uint64_t now() { return g_now; }

void window_numbers(statsview::Window &w) {
    w.sound = true;
    w.output = "the sound card itself (ALSA hw:CARD=Generic,DEV=0), 128 frames";
    w.queue_ms = 8.2f, w.target_ms = 8.0f, w.device_ms = 2.9f;
    w.breaks = 1;
    w.picture = "CRT light (crt-guest-advanced-fast)";
    w.smooth = true;
    w.picture_p50 = 3.2f, w.picture_p99 = 4.1f, w.sound_p50 = 9.9f, w.sound_p99 = 11.0f;
}

// `n` frames of a 60 Hz game, 2 ms of the machine's work each; every 50th didn't fit its time
void run_frames(int n) {
    static uint64_t number = 0;
    for (int i = 0; i < n; i++) {
        statsview::Frame f;
        f.number = ++number;
        f.game_ms = number % 50 == 0 ? 21.0f : 2.0f + (float)(number % 7) * 0.2f;
        f.draw_ms = 0.5f;
        f.period_ms = 16.667f;
        f.late = number % 50 == 0;
        f.sync = true;
        f.breaks = number == 120 ? 1 : 0;
        f.part_ms[hk::prof::CPU] = f.game_ms * 0.7f, f.part_ms[hk::prof::GPU] = f.game_ms * 0.2f;
        g_now += 16'666'667ull;
        statsview::frame(f);
    }
}

SDL_Event key_f7(bool shift) {
    SDL_Event e{};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.key = SDLK_F7;
    e.key.mod = shift ? SDL_KMOD_LSHIFT : SDL_KMOD_NONE;
    return e;
}

SDL_Event mouse(uint32_t type, float x, float y) {
    SDL_Event e{};
    e.type = type;
    if (type == SDL_EVENT_MOUSE_MOTION) e.motion.x = x, e.motion.y = y;
    else e.button.button = SDL_BUTTON_LEFT, e.button.x = x, e.button.y = y;
    return e;
}

void write(const std::filesystem::path &p, const std::string &text) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p) << text;
}

void test_reading(const std::filesystem::path &dir) {
    using namespace sysload;
    {
        const DrmClient c = parse_fdinfo("pos:\t0\nflags:\t02100002\ndrm-driver:\tamdgpu\ndrm-client-id:\t42\ndrm-pdev:\t0000:03:00.0\n"
                                         "drm-engine-gfx:\t123456789 ns\ndrm-engine-compute:\t11 ns\ndrm-memory-vram:\t1024 KiB\n");
        check(c.drm && c.id == 42 && c.driver == "amdgpu" && c.pdev == "0000:03:00.0" && c.engine_ns == 123456800ull,
              "an amdgpu client's file: its number, its card, its engines' time added up");
    }
    {
        const DrmClient c = parse_fdinfo("drm-driver: i915\ndrm-client-id: 7\ndrm-engine-render: 5000 ns\ndrm-engine-capacity-video: 2\n"
                                         "drm-engine-video: 20 ns\n");
        check(c.drm && c.engine_ns == 5020 && c.pdev.empty(), "an i915 client's: an engine's count (capacity) isn't time");
    }
    {
        const DrmClient c = parse_fdinfo("drm-driver: xe\ndrm-client-id: 9\ndrm-cycles-rcs: 300\ndrm-total-cycles-rcs: 1000\n");
        check(c.drm && c.cycles == 300 && c.total_cycles == 1000 && c.engine_ns == 0, "an xe client's: cycles of the render engine's own");
    }
    check(!parse_fdinfo("pos:\t0\nflags:\t0100002\nmnt_id:\t26\n").drm, "a file that is no card's");
    check(parse_sclk("0: 500Mhz \n1: 1200Mhz *\n2: 2400Mhz \n") == 1200 && parse_sclk("") == 0, "the card's clock: the level in use");
    check(parse_schedstat("987654321 1234 56\n") == 987654321ull, "a thread's time run");
    {
        unsigned long pid = 0;
        std::string type;
        check(parse_gpu_engine("pid_4312_luid_0x00000000_0x0000C3A1_phys_0_eng_0_engtype_3D", pid, type) && pid == 4312 && type == "3D" &&
                  !parse_gpu_engine("_Total", pid, type),
              "Windows' counter names: the program and the engine's kind");
    }
    {
        ThreadTimes a{{1, {"main", 1'000'000'000ull}}, {2, {"GS", 500'000'000ull}}, {3, {"idle", 10}}};
        ThreadTimes b{{1, {"main", 1'300'000'000ull}}, {2, {"GS", 1'000'000'000ull}}, {3, {"idle", 20}}, {4, {"new", 900'000'000ull}}};
        const std::vector<Thread> t = busiest(a, b, 1e9, 4);
        check(t.size() == 2 && t[0].name == "GS" && std::fabs(t[0].share - 50) < 0.01 && t[1].name == "main" && std::fabs(t[1].share - 30) < 0.01,
              "the busiest threads: by share, none under 1 %, a new one next time");
    }
#ifdef __linux__
    {   // a /sys with an Intel card and an AMD one
        const std::filesystem::path sys = dir / "sys";
        std::filesystem::remove_all(sys);
        write(sys / "devices/0000:00:02.0/vendor", "0x8086\n");
        write(sys / "devices/0000:03:00.0/vendor", "0x1002\n");
        write(sys / "devices/0000:03:00.0/gpu_busy_percent", "37\n");
        write(sys / "devices/0000:03:00.0/pp_dpm_sclk", "0: 500Mhz \n1: 1830Mhz *\n");
        write(sys / "class/drm/card0/gt_act_freq_mhz", "950\n");
        std::filesystem::create_directories(sys / "class/drm/card1");
        std::filesystem::create_directories(sys / "class/drm/card1-DP-1");
        std::filesystem::create_directory_symlink("../../../devices/0000:00:02.0", sys / "class/drm/card0/device");
        std::filesystem::create_directory_symlink("../../../devices/0000:03:00.0", sys / "class/drm/card1/device");
        Now n;
        check(read_card(sys.string(), "0000:03:00.0", n) && n.gpu == "AMD" && n.gpu_busy == 37 && n.gpu_mhz == 1830,
              "the card the window is on, an AMD one: its load and clock");
        n = {};
        check(read_card(sys.string(), "0000:00:02.0", n) && n.gpu == "Intel" && n.gpu_busy < 0 && n.gpu_mhz == 950,
              "an Intel one: its clock, no load said");
        n = {};
        check(read_card(sys.string(), "", n) && n.gpu == "AMD", "no card named: one that says its load");
        n = {};
        check(!read_card((dir / "nothing").string(), "", n), "no cards at all");
    }
#else
    (void)dir;
#endif
}

void test_panel(const std::filesystem::path &dir) {
    const std::string store = (dir / "stats-overlay.txt").string();
    std::filesystem::remove(store);
    statsview::set_clock(now);
    statsview::set_source(window_numbers);
    statsview::set_store(store);
    statsview::Panel p{};
    check(statsview::level() == statsview::Level::off && !statsview::panel(p), "hidden at first");
    run_frames(200);
    // F7 through the levels
    SDL_Event f7 = key_f7(false);
    check(statsview::event(f7, 1) && statsview::level() == statsview::Level::line && statsview::panel(p) && p.h < 30, "F7: one line");
    const int line_w = p.w;
    check(statsview::event(f7, 1) && statsview::level() == statsview::Level::panel && statsview::panel(p) && p.w == 396 && p.h > 100,
          "F7 again: the panel");
    const int panel_h = p.h;
    check(statsview::event(f7, 1) && statsview::level() == statsview::Level::full && statsview::panel(p) && p.h > panel_h && g_machine.asked,
          "F7 again: everything, the machine's lines asked for");
    check(statsview::event(f7, 1) && statsview::level() == statsview::Level::off && !statsview::panel(p), "F7 again: hidden");
    check(line_w > 100 && (line_w - 12) % 8 == 0, "the line is as wide as its text");
    // the panel is a new picture when its numbers move, not at every asking
    statsview::set_level(statsview::Level::panel);
    statsview::panel(p);
    const unsigned id = p.id;
    statsview::panel(p);
    check(p.id == id, "asked again at once: the same picture");
    run_frames(70);
    statsview::panel(p);
    check(p.id != id, "a second on: a new one");
    {   // the graph shows the slow frame: red in the panel
        size_t red = 0;
        for (uint32_t px : *p.pixels) red += (px & 0xffffff) == 0xff5a46;
        check(red > 0, "a frame that didn't fit shows in red");
    }
    // its place: the top right at first, the corners with Shift+F7
    int x, y, w, h;
    statsview::place(p, 1280, 960, x, y, w, h);
    check(w == p.w && h == p.h && x == 1280 - w - 8 && y == 8, "at the top right, its own size in a 960-line window");
    statsview::place(p, 2560, 1440, x, y, w, h);
    check(w == 2 * p.w && x == 2560 - w - 16, "twice the size on 1440 lines");
    statsview::place(p, 1920, 1080, x, y, w, h);
    check(w == p.w, "its own size on 1080");
    statsview::place(p, 320, 240, x, y, w, h);
    check(x == 0 && y >= 0, "a window smaller than it: at its edge");
    SDL_Event corner = key_f7(true);
    statsview::event(corner, 1);
    statsview::place(p, 1280, 960, x, y, w, h);
    check(x == 1280 - w - 8 && y == 960 - h - 8 && statsview::level() == statsview::Level::panel, "Shift+F7: the bottom right");
    statsview::event(corner, 1);
    statsview::place(p, 1280, 960, x, y, w, h);
    check(x == 8 && y == 960 - h - 8, "again: the bottom left");
    statsview::event(corner, 1);
    statsview::event(corner, 1);
    statsview::place(p, 1280, 960, x, y, w, h);
    check(x == 1280 - w - 8 && y == 8, "twice more: the top right again");
    // the mouse: a press beside it is the game's; a press on it takes it along
    SDL_Event down = mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, 100, 500);
    check(!statsview::event(down, 1) && !statsview::dragging(), "a click beside the panel isn't taken");
    down = mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, (float)(x + 30), (float)(y + 20));
    SDL_Event move = mouse(SDL_EVENT_MOUSE_MOTION, 430, 320), up = mouse(SDL_EVENT_MOUSE_BUTTON_UP, 430, 320);
    check(statsview::event(down, 1) && statsview::dragging() && statsview::event(move, 1), "a press on it, and a move: taken");
    statsview::place(p, 1280, 960, x, y, w, h);
    check(x == 400 && y == 300, "it follows the pointer, held where it was taken");
    move = mouse(SDL_EVENT_MOUSE_MOTION, -500, 5000);
    statsview::event(move, 1);
    statsview::place(p, 1280, 960, x, y, w, h);
    check(x == 8 && y == 960 - h - 8, "pulled out of the window: it stays inside");
    move = mouse(SDL_EVENT_MOUSE_MOTION, 430, 320);
    statsview::event(move, 1);
    check(statsview::event(up, 1) && !statsview::dragging(), "let go");
    move = mouse(SDL_EVENT_MOUSE_MOTION, 900, 700);
    check(!statsview::event(move, 1), "the mouse moving after that is the game's again");
    {   // a window whose events count in points, two pixels each
        statsview::place(p, 1280, 960, x, y, w, h);   // (at 400, 300)
        down = mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, (float)(x + 30) / 2, (float)(y + 20) / 2);
        move = mouse(SDL_EVENT_MOUSE_MOTION, 300, 200), up = mouse(SDL_EVENT_MOUSE_BUTTON_UP, 300, 200);
        const bool took = statsview::event(down, 2);
        statsview::event(move, 2), statsview::event(up, 2);
        statsview::place(p, 1280, 960, x, y, w, h);
        check(took && x == 570 && y == 380, "events in points on a dense display: the same in pixels");
    }
    // kept, and read back
    double fx = 0, fy = 0, gx = 0, gy = 0;
    statsview::where(fx, fy);
    check(std::filesystem::exists(store), "its place is written when let go");
    std::string kept;
    {
        std::ifstream in(store);
        kept.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    statsview::event(corner, 1);   // (elsewhere, and the file with it)
    std::ofstream(store) << kept;
    statsview::set_store(store);
    statsview::where(gx, gy);
    check(fx > 0 && fx < 1 && std::fabs(gx - fx) < 1e-3 && std::fabs(gy - fy) < 1e-3, "and read back by the next game");
    std::ofstream(store) << "x=7\ny=-3\n";
    statsview::set_store(store);
    statsview::where(gx, gy);
    check(gx == 1 && gy == 0, "a file with a place outside the window: held to its edge");
    // fast-forward: nothing counted, and the first frame after starts afresh
    statsview::gap();
    run_frames(3);
    statsview::panel(p);
    check(p.pixels && !p.pixels->empty(), "after a gap it still draws");
}

// what a redraw costs (ten a second at most while it shows)
void cost() {
    statsview::set_level(statsview::Level::full);
    statsview::Panel p{};
    const uint64_t t0 = SDL_GetTicksNS();
    const int n = 200;
    for (int i = 0; i < n; i++) {
        g_now += 100'000'000ull;
        statsview::panel(p);
    }
    const double graph_us = (double)(SDL_GetTicksNS() - t0) / n / 1e3;
    const uint64_t t1 = SDL_GetTicksNS();
    for (int i = 0; i < n; i++) {
        run_frames(60);   // (a second: the text's numbers move)
        statsview::panel(p);
    }
    printf("note  a redraw of everything: %.0f us with its text (once a second), %.0f us the graph alone (ten a second)\n",
           (double)(SDL_GetTicksNS() - t1) / n / 1e3, graph_us);
}

int ppm(const std::string &file, const std::string &level) {
    statsview::Level l;
    if (!statsview::parse_level(level, l)) return 2;
    statsview::set_clock(now);
    statsview::set_source(window_numbers);
    run_frames(10);
    statsview::set_level(l);
    if (l == statsview::Level::full) SDL_Delay(2300);   // (the PC's own numbers: a second apart)
    run_frames(230);
    statsview::Panel p{};
    if (!statsview::panel(p)) return 1;
    FILE *f = fopen(file.c_str(), "wb");
    if (!f) return 1;
    fprintf(f, "P6\n%d %d\n255\n", p.w, p.h);
    for (uint32_t px : *p.pixels) {   // over a mid grey, as over a picture
        const double a = (px >> 24) / 255.0;
        for (int s = 16; s >= 0; s -= 8) fputc((int)std::lround(((px >> s) & 255) * a + 110 * (1 - a)), f);
    }
    fclose(f);
    return 0;
}

}  // namespace

hk::Core &hk::core() { return g_machine; }

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    const std::filesystem::path dir = argv[1];
    std::filesystem::create_directories(dir);
    if (argc == 5 && std::string(argv[2]) == "--ppm") return ppm(argv[3], argv[4]);
    test_reading(dir);
    test_panel(dir);
    cost();
    printf(failed ? "%d FAILED\n" : "all hold\n", failed);
    return failed ? 1 : 0;
}
