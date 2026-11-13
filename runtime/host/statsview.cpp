// SPDX-License-Identifier: GPL-3.0-or-later
#include "statsview.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "../shell/core.h"
#include "../shell/kvfile.h"
#include "../shell/sys.h"
#include "pacing.h"
#include "sysload.h"

namespace hk::host::statsview {

namespace {

constexpr int COLS = 48;               // a line's characters at most
constexpr int BARS = COLS * 4;         // the graph's frames, 2 pixels each (three seconds at 60 a second)
constexpr int PAD = 6, LINE = 11, GRAPH_H = 40;
constexpr uint64_t SEC = 1'000'000'000ull;
constexpr uint32_t WHITE = 0xe6e6e6, GREY = 0x9696a0, AMBER = 0xffbe3c, RED = 0xff5a46, GREEN = 0x46c86e, BLUE = 0x3c8cff;

Level g_level = Level::off;
uint64_t (*g_clock)() = prof::now_ns;
void (*g_source)(Window &) = nullptr;
std::string g_store;
double g_fx = 1, g_fy = 0;             // the panel's place: its share of the free room, from the left and the top

// the frames of the graph, the oldest overwritten
struct Bar {
    float game = 0, draw = 0, period = 0;
    bool late = false, broke = false, again = false;
};
Bar g_bars[BARS];
uint64_t g_bar_n = 0;

// the second in hand, and the last whole one as the text shows it
struct {
    uint64_t from = 0;
    int frames = 0;
    double game = 0, draw = 0, worst = 0, period = 0;
} g_acc;
struct {
    double fps = 0, game = 0, draw = 0, worst = 0, period = 0, thread_cpu = -1, process_cpu = -1;
} g_sec;
double g_thread_ms = 0, g_process_ms = 0;   // the CPU times at the second's start
int g_late10[10] = {}, g_break10[10] = {}, g_at10 = 0;   // the last ten seconds' late frames and breaks
uint64_t g_frames = 0, g_late = 0;
Window g_window;
pacing::Stats g_pacing;
std::vector<std::string> g_core_lines;

// a frame that didn't fit, kept on the panel for a while; and the log's
struct {
    Frame f;
    uint64_t at = 0;
} g_slow;
bool g_slow_shown = false;   // ... its lines are in the text as drawn
uint64_t g_started = 0, g_logged_at = 0, g_unsaid = 0;
int g_logged = 0;
constexpr int LOG_MOST = 40;

// presses waiting for their frame's start, and the last ones' delays
uint64_t g_pressed[16];
int g_pressed_n = 0;
float g_press_ms[64];
uint64_t g_press_n = 0;

// the picture
std::vector<uint32_t> g_pixels;
int g_w = 0, g_h = 0;
unsigned g_id = 0;
uint64_t g_drawn_at = 0;
bool g_dirty = true;

// where the window last drew it, for the mouse
struct {
    int x = 0, y = 0, w = 0, h = 0, win_w = 0, win_h = 0, margin = 0;
} g_rect;
bool g_dragging = false;
float g_grab_x = 0, g_grab_y = 0;

std::string fmt(const char *f, ...) {
    char b[256];
    va_list ap;
    va_start(ap, f);
    vsnprintf(b, sizeof b, f, ap);
    va_end(ap);
    return b;
}

std::string clip(std::string s, size_t cols = COLS) {
    if (s.size() > cols) s.resize(cols - 2), s += "..";
    return s;
}

// s in lines of COLS characters, broken at spaces, `most` lines at most (the last cut short)
std::vector<std::string> wrap(const std::string &s, size_t most) {
    std::vector<std::string> out;
    std::string rest = s;
    while (!rest.empty() && out.size() < most) {
        const size_t room = out.empty() ? COLS : COLS - 2;   // (a continued line starts two in)
        if (rest.size() <= room || out.size() + 1 == most) {
            out.push_back((out.empty() ? "" : "  ") + clip(rest, room));
            break;
        }
        size_t cut = rest.rfind(' ', room);
        if (cut == std::string::npos || cut < room / 2) cut = room;
        out.push_back((out.empty() ? "" : "  ") + rest.substr(0, cut));
        rest.erase(0, rest.find_first_not_of(' ', cut));
    }
    return out;
}

// where a frame's work went: "cpu 14.1, gpu 5.0, disk 2.1, window 0.8"
std::string parts(const Frame &f) {
    std::string out;
    double named = 0;
    auto add = [&](const char *name, double ms) {
        if (ms >= 0.1) out += fmt("%s%s %.1f", out.empty() ? "" : ", ", name, ms);
    };
    for (int c = 0; c < prof::NCAT; c++) {
        if (c == prof::PRESENT) continue;   // (the window's, waits and all: draw_ms is its work)
        add(prof::NAMES[c], f.part_ms[c]);
        named += f.part_ms[c];
    }
    add("other", f.game_ms - named);
    add("window", f.draw_ms);
    return out;
}

void save_place() {
    if (g_store.empty()) return;
    if (FILE *f = fopen(g_store.c_str(), "w")) {
        fprintf(f, "x=%.4f\ny=%.4f\n", g_fx, g_fy);
        fclose(f);
    }
}

struct Row {
    std::string text;
    uint32_t rgb = WHITE;
    bool graph = false;   // the graph here, not a line of text
};

int sum10(const int *v) {
    int n = 0;
    for (int i = 0; i < 10; i++) n += v[i];
    return n;
}

std::vector<Row> rows() {
    std::vector<Row> out;
    const pacing::Stats &ps = g_pacing;
    const int late10 = sum10(g_late10), break10 = sum10(g_break10);
    const uint64_t now = g_clock();
    const bool slow = g_slow.at && now - g_slow.at < 6 * SEC;
    g_slow_shown = slow;
    if (g_level == Level::line) {
        std::string s = fmt("%.1f fps  %.1f/%.1f ms", g_sec.fps, g_sec.game + g_sec.draw, g_sec.period);
        if (g_sec.thread_cpu >= 0) s += fmt("  CPU %.0f %%", g_sec.thread_cpu);
        if (g_window.sound) s += fmt("  sound %.0f ms", g_window.queue_ms);
        s += fmt("  late %d", late10);
        if (break10) s += fmt("  breaks %d", break10);
        out.push_back({s, slow || late10 || break10 ? AMBER : WHITE});
        return out;
    }
    out.push_back({"Performance (F7: more, drag to move)", GREY});
    {   // the rates
        std::string s = fmt("game %.2f fps", g_sec.fps);
        if (ps.refresh_hz > 0) s += fmt("  display %.2f Hz", ps.refresh_hz);
        if (ps.active && std::fabs(ps.speed - 1.0) >= 0.00005) s += fmt("  speed %+.2f %%", (ps.speed - 1.0) * 100.0);
        out.push_back({clip(s)});
    }
    out.push_back({clip(fmt("frames: %llu late of %llu (%d in the last 10 s)", (unsigned long long)g_late, (unsigned long long)g_frames, late10)),
                   late10 ? AMBER : WHITE});
    out.push_back({clip(fmt("work %.2f ms of %.2f (worst %.2f), window %.2f", g_sec.game, g_sec.period, g_sec.worst, g_sec.draw)),
                   g_sec.worst > g_sec.period && g_sec.period > 0 ? AMBER : WHITE});
    if (g_sec.thread_cpu >= 0)
        out.push_back({clip(fmt("CPU: game thread %.0f %%, all threads %.0f %% of one", g_sec.thread_cpu, g_sec.process_cpu))});
    out.push_back({"", WHITE, true});
    out.push_back({"green: game, blue: window, line: a frame's time", GREY});
    if (slow) {
        const Frame &f = g_slow.f;
        out.push_back({clip(fmt("slow: frame %llu, %.1f ms of %.1f%s%s", (unsigned long long)f.number, f.game_ms + f.draw_ms, f.period_ms,
                                f.late && f.sync ? ", late" : "", f.breaks ? ", sound broke" : "")), RED});
        if (const std::string p = parts(f); !p.empty()) out.push_back({"  " + clip(p, COLS - 2), RED});
    }
    if (g_window.sound) {
        out.push_back({clip(fmt("sound: %.1f ms queued (target %.1f), %llu break%s", g_window.queue_ms, g_window.target_ms,
                                (unsigned long long)g_window.breaks, g_window.breaks == 1 ? "" : "s")), break10 ? AMBER : WHITE});
        if (!g_window.output.empty()) out.push_back({"  " + clip(g_window.output, COLS - 2), GREY});
    } else {
        out.push_back({"sound: no device", AMBER});
    }
    {   // display sync, and what else is on
        std::string s = "display sync ";
        if (ps.active) s += ps.exact ? "on: the game at its own speed" : fmt("on: %d refresh%s a frame", ps.refreshes, ps.refreshes == 1 ? "" : "es");
        else s += ps.state.empty() ? (ps.enabled ? "not in use" : "off") : ps.state;
        for (const std::string &l : wrap(s, 2)) out.push_back({l});
        std::string on;
        auto add = [&](const std::string &t) { on += (on.empty() ? "" : ", ") + t; };
        if (g_window.smooth)
            add(ps.extra ? fmt("smooth motion (%llu of %llu late)", (unsigned long long)ps.extra_late, (unsigned long long)ps.extra) : "smooth motion");
        if (g_window.beam) add("CRT beam");
        if (!g_window.picture.empty()) add(g_window.picture);
        if (g_sec.period > 0 && 1000.0 / (g_sec.period * ps.speed) > core().native_rate() * 1.4) add("high frame rate");
        for (const std::string &l : wrap(on, 2)) out.push_back({l});
    }
    if (g_level != Level::full) return out;
    // the latencies
    if (g_press_n) {
        double sum = 0;
        const int n = (int)std::min<uint64_t>(g_press_n, 64);
        for (int i = 0; i < n; i++) sum += g_press_ms[i];
        out.push_back({clip(fmt("a press to its frame's start: %.1f ms (last %d)", sum / n, n))});
    } else {
        out.push_back({"a press to its frame's start: none yet", GREY});
    }
    if (g_window.picture_p50 > 0) {
        std::string s = fmt("frame start to picture: %.1f ms (p99 %.1f)", g_window.picture_p50, g_window.picture_p99);
        if (ps.active && ps.to_screen_ms > 0) s += fmt(", +%.1f", ps.to_screen_ms);   // ... the display's own, at best
        out.push_back({clip(s)});
    }
    if (g_window.sound_p50 > 0)
        out.push_back({clip(fmt("frame start to sound: %.1f ms (p99 %.1f), +%.1f", g_window.sound_p50, g_window.sound_p99, g_window.device_ms))});
    // the PC
    const sysload::Now sl = sysload::now();
    if (!sl.n) {
        out.push_back({"the PC: reading..", GREY});
    } else {
        if (!sl.threads.empty()) {
            std::string s = "threads: ";
            for (const sysload::Thread &t : sl.threads) s += fmt("%s%s %.0f %%", &t == &sl.threads[0] ? "" : ", ", t.name.c_str(), t.share);
            for (const std::string &l : wrap(s, 2)) out.push_back({l});
        }
        std::string s = "GPU" + (sl.gpu.empty() ? std::string() : " (" + sl.gpu + ")") + ": ";
        if (sl.gpu_busy < 0 && sl.gpu_own < 0) s += "its load isn't known here";
        else {
            if (sl.gpu_busy >= 0) s += fmt("%.0f %%", sl.gpu_busy);
            if (sl.gpu_own >= 0) s += fmt("%sthis window %.0f %%", sl.gpu_busy >= 0 ? ", " : "", sl.gpu_own);
        }
        if (sl.gpu_mhz) s += fmt(", %d MHz", sl.gpu_mhz);
        out.push_back({clip(s), sl.gpu_busy < 0 && sl.gpu_own < 0 ? GREY : WHITE});
        if (sl.cpu_mhz) out.push_back({fmt("processors: up to %d MHz now", sl.cpu_mhz)});
    }
    for (const std::string &l : g_core_lines) out.push_back({clip(l)});
    return out;
}

// The picture, drawn by hand (a redraw is some tens of microseconds): its text when a number in it
// changes, about once a second, and the graph over a copy of that ten times a second.
std::vector<uint32_t> g_text;
int g_graph_x = -1, g_graph_y = 0;

// SDL's 8x8 font, a bit a pixel: read once from what its renderer draws
uint8_t g_font[96][8];
bool g_font_read = false;
void read_font() {
    g_font_read = true;
    SDL_Surface *s = SDL_CreateSurface(96 * 8, 8, SDL_PIXELFORMAT_ARGB8888);
    SDL_Renderer *r = s ? SDL_CreateSoftwareRenderer(s) : nullptr;
    if (r) {
        SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
        SDL_RenderClear(r);
        SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
        for (int c = 33; c < 127; c++) {
            const char t[2] = {(char)c, 0};
            SDL_RenderDebugText(r, (float)((c - 32) * 8), 0, t);
        }
        SDL_RenderPresent(r);
        for (int c = 0; c < 96; c++)
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++)
                    if (((const uint32_t *)((const uint8_t *)s->pixels + (size_t)y * s->pitch))[c * 8 + x] & 0xffffff) g_font[c][y] |= (uint8_t)(1 << x);
        SDL_DestroyRenderer(r);
    }
    if (s) SDL_DestroySurface(s);
}

void fill(std::vector<uint32_t> &to, int x, int y, int w, int h, uint32_t rgb, uint32_t a = 255) {
    const int x1 = std::min(x + w, g_w), y1 = std::min(y + h, g_h);
    x = std::max(x, 0), y = std::max(y, 0);
    for (int yy = y; yy < y1; yy++)
        for (int xx = x; xx < x1; xx++) to[(size_t)yy * g_w + xx] = a << 24 | rgb;
}

void text(std::vector<uint32_t> &to, int x, int y, const std::string &t, uint32_t rgb) {
    for (size_t i = 0; i < t.size() && x + 8 <= g_w; i++, x += 8) {
        const unsigned c = (unsigned char)t[i];
        if (c <= 32 || c >= 127 || y + 8 > g_h) continue;
        for (int gy = 0; gy < 8; gy++)
            for (int gx = 0; gx < 8; gx++)
                if (g_font[c - 32][gy] >> gx & 1) to[(size_t)(y + gy) * g_w + x + gx] = 0xff000000u | rgb;
    }
}

void draw_graph(int x0, int y0) {
    fill(g_pixels, x0, y0, BARS * 2, GRAPH_H, 0x1e1e26, 225);
    const uint64_t n = std::min<uint64_t>(g_bar_n, BARS);
    if (!n) return;
    const double period = g_bars[(g_bar_n - 1) % BARS].period;
    if (period <= 0) return;
    const double top = period * 1.25;   // the frame's time at four fifths of the height
    auto px = [&](double ms) { return (int)std::lround(std::clamp(ms / top, 0.0, 1.0) * GRAPH_H); };
    const int base = y0 + GRAPH_H, line_y = base - px(period);
    for (uint64_t i = 0; i < n; i++) {
        const Bar &b = g_bars[(g_bar_n - n + i) % BARS];
        const int x = x0 + (int)(BARS - n + i) * 2;
        const int hg = px(b.game), ha = px(b.game + b.draw);
        fill(g_pixels, x, base - hg, 2, hg, b.again ? 0x787880 : GREEN);   // (grey: the game drew no picture for it)
        fill(g_pixels, x, base - ha, 2, ha - hg, BLUE);
        if (base - ha < line_y && !b.again) fill(g_pixels, x, base - ha, 2, line_y - (base - ha), RED);   // the part over the frame's time
        if (b.broke) fill(g_pixels, x, y0 + 4, 2, 4, AMBER);
        if (b.late) fill(g_pixels, x, y0, 2, 4, RED);
    }
    fill(g_pixels, x0, line_y, BARS * 2, 1, 0xdcdcdc);
}

void draw_text() {
    if (!g_font_read) read_font();
    const std::vector<Row> rs = rows();
    size_t cols = 0;
    int h = 2 * PAD - (LINE - 8);
    for (const Row &row : rs) {
        cols = std::max(cols, row.text.size());
        h += row.graph ? GRAPH_H + (LINE - 8) + 1 : LINE;
    }
    g_w = (g_level == Level::line ? (int)cols * 8 : BARS * 2) + 2 * PAD, g_h = h;
    g_text.assign((size_t)g_w * g_h, 235u << 24 | 0x5a5a64);   // an edge, and a dark inside the picture shows through
    fill(g_text, 1, 1, g_w - 2, g_h - 2, 0x101014, 205);
    int y = PAD;
    g_graph_x = -1;
    for (const Row &row : rs) {
        if (row.graph) {
            g_graph_x = PAD, g_graph_y = y;
            y += GRAPH_H + (LINE - 8) + 1;
            continue;
        }
        text(g_text, PAD, y, row.text, row.rgb);
        y += LINE;
    }
}

void draw() {
    if (g_dirty || g_text.empty()) draw_text();
    if (g_text.empty()) return;
    g_pixels = g_text;
    if (g_graph_x >= 0) draw_graph(g_graph_x, g_graph_y);
    g_id++;
}

// a second has passed: the text's numbers
void second(uint64_t now) {
    const double wall_ms = (double)(now - g_acc.from) / 1e6;
    if (g_acc.frames && wall_ms > 0) {
        g_sec.fps = g_acc.frames * 1000.0 / wall_ms;
        g_sec.game = g_acc.game / g_acc.frames, g_sec.draw = g_acc.draw / g_acc.frames;
        g_sec.worst = g_acc.worst, g_sec.period = g_acc.period;
    }
    const double thread_ms = hk::sys::thread_cpu_ms(), process_ms = hk::sys::process_cpu_ms();
    if (g_acc.from && wall_ms > 0) {
        g_sec.thread_cpu = 100.0 * (thread_ms - g_thread_ms) / wall_ms;
        g_sec.process_cpu = 100.0 * (process_ms - g_process_ms) / wall_ms;
    }
    g_thread_ms = thread_ms, g_process_ms = process_ms;
    g_acc = {};
    g_acc.from = now;
    g_at10 = (g_at10 + 1) % 10;
    g_late10[g_at10] = g_break10[g_at10] = 0;
    if (g_level == Level::off) return;
    g_pacing = pacing::stats();
    if (g_source) g_source(g_window);
    if (g_level == Level::full) {
        g_core_lines.clear();
        core().overlay_lines(g_core_lines);
    }
    g_dirty = true;
}

}  // namespace

bool parse_level(const std::string &s, Level &l) {
    if (s == "off") l = Level::off;
    else if (s == "line") l = Level::line;
    else if (s == "panel") l = Level::panel;
    else if (s == "full") l = Level::full;
    else return false;
    return true;
}

void set_level(Level l) {
    g_level = l;
    sysload::run(l == Level::full);
    if (l != Level::off) {   // the numbers as they stand, not a second from now
        g_pacing = pacing::stats();
        if (g_source) g_source(g_window);
        g_core_lines.clear();
        if (l == Level::full) core().overlay_lines(g_core_lines);
    }
    g_dirty = true;
}

Level level() { return g_level; }

void set_store(const std::string &file) {
    g_store = file;
    if (file.empty()) return;
    const auto kv = hk::read_kv(file);
    if (const auto x = kv.find("x"); x != kv.end()) g_fx = std::clamp(atof(x->second.c_str()), 0.0, 1.0);
    if (const auto y = kv.find("y"); y != kv.end()) g_fy = std::clamp(atof(y->second.c_str()), 0.0, 1.0);
}

void set_source(void (*fill)(Window &)) { g_source = fill; }
void set_clock(uint64_t (*now)()) { g_clock = now ? now : prof::now_ns; }
void where(double &fx, double &fy) { fx = g_fx, fy = g_fy; }

void gap() {
    g_acc.from = 0;
    g_pressed_n = 0;
}

void frame(const Frame &f) {
    const uint64_t now = g_clock();
    if (!g_started) g_started = now;
    // presses read since the last frame: this frame is the one the game reads them in
    for (int i = 0; i < g_pressed_n; i++)
        if (now >= g_pressed[i] && now - g_pressed[i] < SEC / 10) g_press_ms[g_press_n++ % 64] = (float)((double)(now - g_pressed[i]) / 1e6);
    g_pressed_n = 0;
    if (!g_acc.from) {   // the first, or the first after a gap: counted from here
        g_acc = {};
        g_acc.from = now;
        g_thread_ms = hk::sys::thread_cpu_ms(), g_process_ms = hk::sys::process_cpu_ms();
        return;
    }
    const float work = f.game_ms + f.draw_ms;
    g_bars[g_bar_n++ % BARS] = {f.game_ms, f.draw_ms, f.period_ms, f.late, f.breaks > 0, f.again};
    g_frames++;
    g_acc.frames++, g_acc.game += f.game_ms, g_acc.draw += f.draw_ms, g_acc.period = f.period_ms;
    g_acc.worst = std::max(g_acc.worst, (double)work);
    if (f.late) g_late++, g_late10[g_at10]++;
    if (f.breaks) g_break10[g_at10] += f.breaks;
    // a frame that didn't fit: on the panel for a while (the worst of them), and in the log
    if (f.period_ms > 0 && !f.again && (work > f.period_ms || (f.late && f.sync) || f.breaks)) {
        if (!g_slow.at || now - g_slow.at >= 6 * SEC || work >= g_slow.f.game_ms + g_slow.f.draw_ms) g_slow = {f, now}, g_dirty = true;
        if (now - g_started >= 10 * SEC && g_logged < LOG_MOST) {   // (not the start-up's)
            if (g_logged_at && now - g_logged_at < 5 * SEC) {
                g_unsaid++;
            } else {
                std::string more;
                if (g_unsaid) more = fmt("; %llu more since the last such line", (unsigned long long)g_unsaid);
                const std::string where = parts(f);
                std::string clock;   // (a power plan that lets the processors slow between a frame's bursts of work)
                if (f.cpu_mhz && f.cpu_mhz * 10 < f.cpu_nominal_mhz * 9) clock = fmt("; the CPU at %d of its %d MHz", f.cpu_mhz, f.cpu_nominal_mhz);
                fprintf(stderr, "slow frame %llu: %.1f ms of work for a %.1f ms frame%s%s%s%s%s%s\n", (unsigned long long)f.number, work,
                        f.period_ms, where.empty() ? "" : (" (" + where + ")").c_str(), f.late && f.sync ? "; shown late" : "",
                        f.breaks ? "; the sound broke" : "", clock.c_str(),
                        more.c_str(), ++g_logged == LOG_MOST ? "; no more of these lines in this run" : "");
                g_logged_at = now, g_unsaid = 0;
            }
        }
    }
    if (now - g_acc.from >= SEC) second(now);
}

bool event(const SDL_Event &e, float scale) {
    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_F7) {
        if (e.key.mod & SDL_KMOD_SHIFT) {   // the next corner, clockwise from the top right
            const bool right = g_fx >= 0.5, low = g_fy >= 0.5;
            g_fx = (right != low ? right : !right) ? 1 : 0;   // top right, bottom right, bottom left, top left
            g_fy = right ? 1 : 0;
            save_place();
        } else {
            set_level(g_level == Level::off ? Level::line : g_level == Level::line ? Level::panel
                      : g_level == Level::panel ? Level::full : Level::off);
        }
        return true;
    }
    if (g_level == Level::off) return false;
    // a press, for its delay to the frame that reads it (as the event says when: SDL's own clock)
    if ((e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) || e.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN) {
        const uint64_t sdl_now = SDL_GetTicksNS();
        const uint64_t age = e.common.timestamp && sdl_now > e.common.timestamp ? std::min<uint64_t>(sdl_now - e.common.timestamp, SEC / 20) : 0;
        if (g_pressed_n < 16) g_pressed[g_pressed_n++] = g_clock() - age;
        return false;
    }
    if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
        const float px = e.button.x * scale, py = e.button.y * scale;
        if (px < (float)g_rect.x || py < (float)g_rect.y || px >= (float)(g_rect.x + g_rect.w) || py >= (float)(g_rect.y + g_rect.h)) return false;
        g_dragging = true;
        g_grab_x = px - (float)g_rect.x, g_grab_y = py - (float)g_rect.y;
        return true;
    }
    if (e.type == SDL_EVENT_MOUSE_MOTION && g_dragging) {
        const int m = g_rect.margin;
        const double room_x = g_rect.win_w - g_rect.w - 2 * m, room_y = g_rect.win_h - g_rect.h - 2 * m;
        if (room_x > 0) g_fx = std::clamp((e.motion.x * scale - g_grab_x - m) / room_x, 0.0, 1.0);
        if (room_y > 0) g_fy = std::clamp((e.motion.y * scale - g_grab_y - m) / room_y, 0.0, 1.0);
        return true;
    }
    if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT && g_dragging) {
        g_dragging = false;
        save_place();
        return true;
    }
    return false;
}

bool dragging() { return g_dragging; }

bool panel(Panel &p) {
    if (g_level == Level::off) return false;
    const uint64_t now = g_clock();
    if (g_slow_shown && now - g_slow.at >= 6 * SEC) g_slow_shown = false, g_dirty = true;   // (its lines go)
    if (g_dirty || g_pixels.empty() || now - g_drawn_at >= SEC / 10) {
        draw();
        g_drawn_at = now, g_dirty = false;
    }
    if (g_pixels.empty()) return false;
    p.pixels = &g_pixels;
    p.w = g_w, p.h = g_h;
    p.id = g_id;
    return true;
}

void place(const Panel &p, int win_w, int win_h, int &x, int &y, int &dw, int &dh) {
    int k = std::max(1, win_h / 720);   // (up to 1080 lines: its own size; 1440: twice; 2160: three times)
    while (k > 1 && p.w * k > win_w) k--;
    dw = p.w * k, dh = p.h * k;
    int m = 8 * k;                      // a margin, where there is room for one
    if (win_w - dw < 2 * m || win_h - dh < 2 * m) m = 0;
    x = m + (int)std::lround(g_fx * std::max(0, win_w - dw - 2 * m));
    y = m + (int)std::lround(g_fy * std::max(0, win_h - dh - 2 * m));
    g_rect = {x, y, dw, dh, win_w, win_h, m};
}

}  // namespace hk::host::statsview
