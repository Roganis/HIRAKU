// SPDX-License-Identifier: GPL-3.0-or-later
// The picture's shader engine (runtime/host/picture.h): FrameCount, the frames before this one
// (OriginalHistoryN) and a pass's output a frame ago (PassFeedbackN, <alias>Feedback), for new
// frames and for refreshes of the same frame (smooth motion's in-between pictures). Small presets
// written here, drawn into a hidden window, the window's pixel read back. Needs a display and
// OpenGL 3.3: skipped (77) without them.
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "host/picture.h"

namespace fs = std::filesystem;
using hk::host::picture::Mode;

namespace {

int g_fail = 0;
fs::path g_dir;
const int W = 64, H = 64;
decltype(&::glReadPixels) g_read = nullptr;

const char *const VS = R"(#version 330
uniform mat4 MVP;
layout(location = 0) in vec4 Position;
layout(location = 1) in vec2 TexCoord;
out vec2 vTex;
void main() { gl_Position = MVP * Position; vTex = TexCoord; }
)";

// A preset of passes (fragment shaders, "key = value" lines for each) as the "full" mode's folder
void preset(const std::vector<std::pair<std::string, std::string>> &passes) {
    fs::path d = g_dir / "crt-royale";
    fs::remove_all(d);
    fs::create_directories(d);
    std::ofstream p(d / "preset.slangp");
    p << "shaders = " << passes.size() << "\n";
    for (size_t i = 0; i < passes.size(); i++) {
        p << "shader" << i << " = pass" << i << "\nfilter_linear" << i << " = false\n" << passes[i].second;
        std::ofstream(d / ("pass" + std::to_string(i) + ".vert")) << VS;
        std::ofstream(d / ("pass" + std::to_string(i) + ".frag")) << passes[i].first;
    }
    std::ofstream(d / "parameters.txt");
}

std::vector<uint32_t> solid(uint8_t r) { return std::vector<uint32_t>(4 * 4, (uint32_t)r << 16); }

// Draw a 4x4 frame of red `r` as game frame `frame`; the window's red, green and blue
void draw(uint8_t r, uint64_t frame, int out[3]) {
    auto px = solid(r);
    if (hk::host::picture::draw(px.data(), 4, 4, 0, 0, W, H, W, H, Mode::full, 0, frame) != Mode::full) {
        fprintf(stderr, "the test preset didn't run\n");
        exit(1);
    }
    uint8_t rgb[4];
    g_read(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgb);
    for (int i = 0; i < 3; i++) out[i] = rgb[i];
}

void expect(const char *what, const int got[3], int r, int g, int b) {
    if (got[0] == r && got[1] == g && got[2] == b) return;
    fprintf(stderr, "FAIL %s: (%d, %d, %d), expected (%d, %d, %d)\n", what, got[0], got[1], got[2], r, g, b);
    g_fail++;
}

// The beam's formula (beam_shader.h) in C++: the window's value (0-1) at tube position `tube` (0
// top), for this refresh, from the two latest pictures' values
double beam_ref(double now, double before, double tube, double phase, double fph, double gamma, double gain) {
    auto lin = [&](double c) { return std::pow(c, gamma); };
    auto enc = [&](double c) { return std::pow(std::max(c, 0.0), 1.0 / gamma); };
    auto ov = [](double a0, double a1, double b0, double b1) { return std::max(0.0, std::min(a1, b1) - std::max(a0, b0)); };
    const double b = lin(before) * fph * gain, n = lin(now) * fph * gain, t = tube * fph, f0 = phase * fph, f1 = f0 + 1;
    return std::clamp(enc(ov(t - fph, t - fph + b, f0, f1) + ov(t, t + n, f0, f1) + ov(t + fph, t + fph + n, f0, f1)), 0.0, 1.0);
}

// The window's value at row `row` from the top (0-255), red, after a beam refresh of a grey frame
int beam_draw(uint8_t grey, uint64_t frame, double phase, double fph, int row) {
    auto px = std::vector<uint32_t>(4 * 4, 0x010101u * grey);
    hk::host::picture::draw(px.data(), 4, 4, 0, 0, W, H, W, H, Mode::off, 0, frame, phase, fph);
    uint8_t rgb[4];
    g_read(W / 2, H - 1 - row, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgb);
    return rgb[0];
}

void start() {
    if (!hk::host::picture::init(g_dir.string(), {})) {
        fprintf(stderr, "picture::init failed\n");
        exit(1);
    }
}

}  // namespace

int main() {
    // (HK_TESTS_NO_SKIP: where a display is promised, the workflow's Xvfb, its lack is a failure)
    const int skipped = getenv("HK_TESTS_NO_SKIP") ? 1 : 77;
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        printf("skipped: no display (%s)\n", SDL_GetError());
        return skipped;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_Window *win = SDL_CreateWindow("test_picture", W, H, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    SDL_GLContext ctx = win ? SDL_GL_CreateContext(win) : nullptr;
    if (!ctx) {
        printf("skipped: no OpenGL 3.3 (%s)\n", SDL_GetError());
        return skipped;
    }
    g_read = reinterpret_cast<decltype(g_read)>(SDL_GL_GetProcAddress("glReadPixels"));
    g_dir = fs::temp_directory_path() / ("hk-picture-test-" + std::to_string(SDL_GetTicksNS()));
    int c[3];

    // FrameCount: the game's frame number; a refresh of the same frame keeps it; without one, draws counted
    preset({{R"(#version 330
uniform uint FrameCount;
in vec2 vTex;
layout(location = 0) out vec4 FragColor;
void main() { FragColor = vec4(float(FrameCount % 256u) / 255.0, 0.0, 0.0, 1.0); }
)", ""}});
    start();
    draw(0, 5, c), expect("FrameCount, frame 5", c, 5, 0, 0);
    draw(0, 5, c), expect("FrameCount, frame 5 again", c, 5, 0, 0);
    draw(0, 7, c), expect("FrameCount, frame 7", c, 7, 0, 0);
    hk::host::picture::shutdown();
    start();
    draw(0, UINT64_MAX, c), expect("FrameCount, first draw", c, 0, 0, 0);
    draw(0, UINT64_MAX, c), expect("FrameCount, second draw", c, 1, 0, 0);
    hk::host::picture::shutdown();

    // OriginalHistory1 and 2: the frames before, black until there are some; a refresh of the
    // same frame replaces the frame's picture, not the history's
    preset({{R"(#version 330
uniform sampler2D Original;
uniform sampler2D OriginalHistory1;
uniform sampler2D OriginalHistory2;
in vec2 vTex;
layout(location = 0) out vec4 FragColor;
void main() {
    FragColor = vec4(texture(Original, vTex).r, texture(OriginalHistory1, vTex).r, texture(OriginalHistory2, vTex).r, 1.0);
}
)", ""}});
    start();
    draw(10, 0, c), expect("history, frame 0", c, 10, 0, 0);
    draw(20, 1, c), expect("history, frame 1", c, 20, 10, 0);
    draw(25, 1, c), expect("history, frame 1 again", c, 25, 10, 0);
    draw(30, 2, c), expect("history, frame 2", c, 30, 25, 10);
    draw(40, 3, c), expect("history, frame 3", c, 40, 30, 25);
    hk::host::picture::shutdown();

    // A pass's own output a frame ago (<alias>Feedback), the pass before the last: it counts the
    // frames (black at first); a refresh of the same frame reads the frame before's again. The
    // last pass reads pass 0's feedback through PassFeedback0 too.
    preset({{R"(#version 330
uniform sampler2D AccFeedback;
in vec2 vTex;
layout(location = 0) out vec4 FragColor;
void main() { FragColor = vec4(texture(AccFeedback, vTex).r + 1.0 / 255.0, 0.0, 0.0, 1.0); }
)", "alias0 = Acc\n"},
             {R"(#version 330
uniform sampler2D Source;
uniform sampler2D PassFeedback0;
in vec2 vTex;
layout(location = 0) out vec4 FragColor;
void main() { FragColor = vec4(texture(Source, vTex).r, texture(PassFeedback0, vTex).r, 0.0, 1.0); }
)", ""}});
    start();
    draw(0, 0, c), expect("feedback, frame 0", c, 1, 0, 0);
    draw(0, 0, c), expect("feedback, frame 0 again", c, 1, 0, 0);
    draw(0, 1, c), expect("feedback, frame 1", c, 2, 1, 0);
    draw(0, 2, c), expect("feedback, frame 2", c, 3, 2, 0);
    hk::host::picture::shutdown();

    // The last pass's own output a frame ago: it draws into a target, then the window gets it
    preset({{R"(#version 330
uniform sampler2D PassFeedback0;
uniform sampler2D Original;
in vec2 vTex;
layout(location = 0) out vec4 FragColor;
void main() { FragColor = vec4(texture(PassFeedback0, vTex).r + 1.0 / 255.0, texture(Original, vTex).r, 0.0, 1.0); }
)", ""}});
    start();
    draw(50, 0, c), expect("last pass feedback, frame 0", c, 1, 50, 0);
    draw(60, 1, c), expect("last pass feedback, frame 1", c, 2, 60, 0);
    draw(70, 1, c), expect("last pass feedback, frame 1 again", c, 2, 70, 0);
    draw(80, 2, c), expect("last pass feedback, frame 2", c, 3, 80, 0);
    hk::host::picture::shutdown();

    // The CRT beam on a display's refresh clock (refresh j at j / fph frames, fph refreshes a frame:
    // 4, 2.4 as at 144 Hz, 4.004 as 59.94 Hz at 240): each refresh against the formula, for the top
    // and bottom rows; and over a long run of the same frame, the light the refreshes give, summed in
    // linear terms, is the frames' times the gain (the "photon budget"; the run's ends shift it by
    // less than a frame's)
    const double gamma = 2.2, gain = 0.7;
    auto lin = [&](double c) { return std::pow(c, gamma); };   // the display's response, as the beam takes it
    for (double fph : {4.0, 2.4, 4.004}) {
        start();   // nothing kept: the frame before the first is black
        if (!hk::host::picture::set_beam(true, {(float)gamma, (float)gain, false, 1})) {
            fprintf(stderr, "FAIL the beam's shader didn't build\n");
            g_fail++;
            break;
        }
        const int N = 26;
        auto grey = [&](int f) { return f < N ? 64 : 200; };
        const int rows[2] = {0, H - 1};
        const double tube[2] = {0.5 / H, (H - 0.5) / H};
        double light[2][2] = {};
        for (long j = 0;; j++) {
            const double t = j / fph;
            const int f = (int)std::floor(t + 1e-9);
            if (f >= 2 * N) break;
            const double phase = t - f;
            const int before = f ? grey(f - 1) : 0;
            for (int r = 0; r < 2; r++) {
                int got = beam_draw((uint8_t)grey(f), 1000 + f, phase, fph, rows[r]);
                int want = (int)std::lround(beam_ref(grey(f) / 255.0, before / 255.0, tube[r], phase, fph, gamma, gain) * 255);
                if (std::abs(got - want) > 1) {
                    fprintf(stderr, "FAIL beam fph %.3f frame %d phase %.3f row %d: %d, the formula %d\n", fph, f, phase, rows[r], got, want);
                    g_fail++;
                }
                const int run = f < N ? 0 : 1;   // the steady parts of each run (its first frame's refreshes left out)
                if (f % N >= 1 && f % N < N - 1) light[run][r] += lin(got / 255.0);
            }
        }
        for (int run = 0; run < 2; run++) {
            const double budget = lin(grey(run * N) / 255.0) * fph * gain * (N - 2);
            for (int r = 0; r < 2; r++)
                if (std::fabs(light[run][r] / budget - 1) > 0.05) {
                    fprintf(stderr, "FAIL beam fph %.3f grey %d row %d: light %.3f, the frames' %.3f\n", fph, grey(run * N),
                            rows[r], light[run][r], budget);
                    g_fail++;
                }
        }
        hk::host::picture::set_beam(false);
        hk::host::picture::shutdown();
    }

    fs::remove_all(g_dir);
    SDL_GL_DestroyContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    if (g_fail) {
        fprintf(stderr, "%d failed\n", g_fail);
        return 1;
    }
    printf("picture engine: FrameCount, frame history, feedback and the CRT beam as expected\n");
    return 0;
}
