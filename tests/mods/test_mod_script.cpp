// SPDX-License-Identifier: GPL-3.0-or-later
// Mods' scripts (runtime/host/modscript.h; docs/MODS.md, "Scripts"), on a made-up machine: the
// sandbox (no io, os, package, debug; load text only; a fixed math.random), the instruction budget
// and the memory cap stopping a script while the game goes on, errors at load refusing the mod and
// in a callback stopping the script, on_input's edges and times, a [[hook]]'s call changing a
// register, settings read and set, both drawing layers checked by their pixels, and the same run
// twice giving the same memory and pictures. Made-up mods and addresses, no game.
//
//   test_mod_script <scratch folder> <tests/mods/sample-script>      exit 0 when all hold
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "host/mods.h"
#include "host/modscript.h"
#include "shell/core.h"
#include "shell/saves.h"

namespace {

// 256 bytes of memory, 8 registers, big-endian, with hooks
class Machine final : public hk::Core {
public:
    uint8_t ram[256] = {};
    double now = 0;
    uint64_t frame = 0;
    struct Hooked { uint32_t at; ModHook fn; void *ctx; };
    std::vector<Hooked> hooked;
    bool init(const Options &) override { return true; }
    const std::string &family() const override { static const std::string f = "test"; return f; }
    void set_frame_hook(FrameHook) override {}
    void set_audio_sink(AudioSink) override {}
    void set_on_fatal(std::function<void()>) override {}
    [[noreturn]] void run() override { abort(); }
    void shutdown() override {}
    void report() override {}
    double seconds() const override { return now; }
    const uint8_t *mem(uint32_t addr, uint32_t len) const override { return (uint64_t)addr + len <= sizeof ram ? ram + addr : nullptr; }
    uint64_t frame_count() const override { return frame; }
    bool set_input(const std::string &, int) override { return true; }
    void turntable_add(int, int) override {}
    const std::vector<uint8_t> &lamp_state() const override { static const std::vector<uint8_t> none; return none; }
    uint64_t lamp_changes() const override { return 0; }
    std::vector<std::string> mod_capabilities() const override { return {"hook"}; }
    std::vector<std::string> mod_registers() const override { return {"r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7"}; }
    bool mod_big_endian() const override { return true; }
    bool mod_hook(uint32_t at, ModHook fn, void *ctx, const std::string &, std::string &) override {
        hooked.push_back({at, fn, ctx});
        return true;
    }
    void reach(uint32_t at, uint32_t *regs) {
        for (const Hooked &h : hooked)
            if (h.at == at) h.fn(h.ctx, regs);
    }
};
Machine g_machine;

int failed = 0;
void check(bool ok, const std::string &what) {
    printf("%s%s\n", ok ? "ok    " : "FAIL  ", what.c_str());
    failed += !ok;
}
bool has(const std::string &s, const std::string &part) { return s.find(part) != std::string::npos; }

std::string g_root;
const char *HEAD = "name = \"x\"\ngame = \"test\"\napi = 1\n";
// a mod of its own folder: mod.toml (after HEAD) and mod.lua
std::string mod(const char *name, const std::string &toml, const std::string &lua) {
    const std::string dir = g_root + "/mods/" + name;
    std::filesystem::create_directories(dir);
    std::ofstream(dir + "/mod.toml") << HEAD << toml;
    if (!lua.empty()) std::ofstream(dir + "/mod.lua") << lua;
    return dir;
}
std::string load(const std::vector<std::string> &dirs) {
    hk::mods::clear();
    g_machine.hooked.clear();
    memset(g_machine.ram, 0, sizeof g_machine.ram);
    g_machine.now = 0, g_machine.frame = 0;
    std::string err;
    return hk::mods::load(dirs, "test", g_root, err) ? std::string() : (err.empty() ? std::string("(refused, no message)") : err);
}
std::string stopped() {
    std::string s;
    for (const std::string &l : hk::modscript::stopped()) s += l + "\n";
    return s;
}
uint32_t be(int at, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v = v << 8 | g_machine.ram[at + i];
    return v;
}
// frames of the machine: its time on by 1/60 s each
void frames(int n, int w = 16, int h = 16, bool draw = true) {
    for (int i = 0; i < n; i++) {
        g_machine.frame++;
        g_machine.now += 1.0 / 60;
        hk::modscript::frame(g_machine.frame, w, h, draw);
    }
}
const std::string SCRIPT_TOML = "requires = [\"script\", \"memory\"]\nscript = \"mod.lua\"\n";

// a font for the tests: every glyph a solid block but the space
uint8_t g_font[96][8];

// The sample's whole run (tests/mods/sample-script): presses, hooks, a setting, frames drawn; what it
// left (RAM and both layers' pixels) for a second run to compare with
struct Run {
    std::vector<uint8_t> ram;
    std::vector<uint32_t> game, window;
};
Run sample_run(const std::string &dir, bool checks) {
    Run r;
    std::string e = load({dir});
    if (checks) check(e.empty(), "the sample script mod loads" + (e.empty() ? "" : ": " + e));
    if (!e.empty()) return r;
    if (checks) check(g_machine.hooked.size() == 2, "its two [[hook]]s with call handed to the core");
    hk::modscript::set_view(10, 0, 20, 20, 40, 20);   // a 40 x 20 window, the picture 20 wide in its middle
    frames(1);
    // on_input: the edges, with the machine's time when the game was given each
    g_machine.now = 1.0;
    hk::modscript::input("P1 Button 1", 1);
    hk::modscript::input("P1 Button 1", 1);   // (the same again: no edge)
    g_machine.now = 1.25;
    hk::modscript::input("P1 Button 1", 0);
    hk::modscript::input("P1 Button 2", 1);
    hk::modscript::input("Turntable", 37);   // (not a button: not given)
    frames(1);
    if (checks) {
        check(be(0x40, 2) == 2 && be(0x42, 2) == 1, "on_input: 2 presses, 1 release (a repeat and an analog value left out)");
        check(be(0x44, 4) == 1250, "on_input: the last one's time, the machine's (1.25 s: " + std::to_string(be(0x44, 4)) + " ms)");
        check(be(0x4c, 2) == 250, "on_input: a hold's length from its two times (250 ms)");
    }
    // a hook's call changes a register, by a setting
    uint32_t regs[8] = {0, 0, 0, 1000, 0, 0, 0, 0};
    g_machine.reach(0x1000, regs);
    if (checks) check(regs[3] == 1000, "a hook's call at shift 0: r3 as it was");
    int64_t now;
    hk::mods::set_setting("sample-script", "shift", 15, now);
    g_machine.reach(0x1000, regs);
    if (checks) check(regs[3] == 985, "a hook's call: r3 earlier by the setting, 15 (" + std::to_string(regs[3]) + ")");
    for (int32_t off : {-5, 3, -1, 0}) {
        uint32_t rr[8] = {0, (uint32_t)off, 4, 0, 0, 0, 0, 0};
        g_machine.reach(0x2000, rr);
    }
    uint32_t best[8] = {0, (uint32_t)-9, 10, 0, 0, 0, 0, 0};
    g_machine.reach(0x2000, best);
    frames(1);
    if (checks) check(be(0x48, 2) == 2 && be(0x4a, 2) == 2, "hooks read registers (signed r1): 2 early, 2 late, the best not counted");
    // hk.set_setting from on_frame (RAM 0x30 = 1): the cover to its max
    g_machine.ram[0x30] = 1;
    frames(1);
    g_machine.ram[0x30] = 0;
    int64_t cover = -1;
    for (auto &[n, v] : hk::mods::settings())
        if (n == "sample-script/cover") cover = v;
    if (checks) check(cover == 200, "hk.set_setting from on_frame: cover = 200 (" + std::to_string(cover) + ")");
    hk::modscript::input("P1 Button 2", 0);
    frames(1);
    // the game layer, over a picture of 16 x 16 grey: the cover (all of the left half: 200/200),
    // the blue square at half alpha, the text (3 presses) at the bottom left
    std::vector<uint32_t> pic(16 * 16, 0x00808080), out;
    const uint32_t *shown = hk::modscript::compose(pic.data(), 16, 16, out);
    if (checks) {
        check(shown != pic.data() && pic[0] == 0x00808080, "compose: a copy, the game's picture left as it was");
        check(shown[0] == 0x00404040 && shown[7 * 16 + 7] == 0x00404040 && shown[15 * 16 + 8] == 0x00808080,
              "the cover: the left half, the whole height (setting 200 of 200)");
        // (the text over the cover: '3' a block of the test font at x 0-7, y 8-15)
        check(shown[9 * 16 + 1] == 0x00ffffff, "the text over the cover, at the bottom left");
        char b[64];
        snprintf(b, sizeof b, "%08x", shown[0 * 16 + 8]);
        check(shown[0 * 16 + 8] == 0x004040c0, std::string("the blue square at alpha 128 over grey: 404040c0 (") + b + ")");
    }
    r.game.assign(shown, shown + 16 * 16);
    // the window layer: the left bar (0-9 of 40 x 20) filled, its edge, a line along its bottom
    std::vector<uint32_t> win(40 * 20, 0xff000000u);
    hk::modscript::paint(hk::modscript::window_pieces(), win.data(), 40, 20);
    if (checks) {
        check((win[5 * 40 + 5] & 0xffffff) == 0x10141c, "the window layer: the bar's panel");
        check((win[0] & 0xffffff) == 0xff8000 && (win[19 * 40 + 9] & 0xffffff) == 0xff8000, "the window layer: its edge (a box)");
        check((win[18 * 40 + 3] & 0xffffff) == 0x00ff00, "the window layer: the line");
        check((win[5 * 40 + 20] & 0xffffff) == 0, "the window layer: nothing over the picture");
    }
    r.window = win;
    r.ram.assign(g_machine.ram, g_machine.ram + sizeof g_machine.ram);
    return r;
}

}  // namespace

hk::Core &hk::core() { return g_machine; }

int main(int argc, char **argv) {
    namespace fs = std::filesystem;
    g_root = argc > 1 ? argv[1] : (fs::temp_directory_path() / "hk-mod-script-test").string();
    fs::remove_all(g_root);
    fs::create_directories(g_root + "/user");
    for (int c = 1; c < 96; c++)
        for (int y = 0; y < 8; y++) g_font[c][y] = 0xff;
    hk::modscript::set_font(g_font);
    hk::mods::offer("draw");
    hk::mods::offer("actions");

    // ---- the sandbox ----------------------------------------------------------------------------
    std::string e = load({mod("sandbox", SCRIPT_TOML,
                              "assert(io == nil and os == nil and package == nil and debug == nil, 'a library there')\n"
                              "assert(dofile == nil and loadfile == nil and require == nil and string.dump == nil, 'a loader there')\n"
                              "assert(not pcall(collectgarbage, 'collect') and collectgarbage('count') > 0, 'collectgarbage')\n"
                              "assert(not pcall(math.randomseed), 'randomseed without a seed')\n"
                              "assert(load('return 2 + 2')() == 4, 'load of text')\n"
                              "assert(not load('\\27Lua', 'x', 'b'), 'a binary chunk')\n"
                              "assert(string and table and math and utf8 and hk and hk.api == 1, 'the libraries')\n"
                              "first = math.random(1, 1000000)\n"
                              "function on_frame() hk.write32(0, first) end\n")});
    check(e.empty(), "a script with only base, string, table, math, utf8 and hk: its asserts hold" + (e.empty() ? "" : ": " + e));
    frames(1);
    const uint32_t first_random = be(0, 4);
    load({mod("sandbox", SCRIPT_TOML, "")});
    frames(1);
    check(first_random != 0 && be(0, 4) == first_random, "math.random: the same first number in two runs (" + std::to_string(first_random) + ")");

    // ---- refused at start, with the message --------------------------------------------------------
    e = load({mod("syntax", SCRIPT_TOML, "function on_frame(\n")});
    check(has(e, "syntax/mod.lua:"), "a syntax error: the mod refused, the file and line named (" + e + ")");
    e = load({mod("load-error", SCRIPT_TOML, "local x = nil\nx.y = 1\n")});
    check(has(e, "mod.lua:2:"), "an error as the file first runs: refused (" + e + ")");
    e = load({mod("load-loop", SCRIPT_TOML, "while true do end\n")});
    check(has(e, "over its budget"), "a file that never ends as it first runs: refused (" + e + ")");
    e = load({mod("no-input", "requires = [\"script\"]\nscript = \"mod.lua\"\n", "function on_input() end\n")});
    check(has(e, "on_input") && has(e, "\"input\""), "on_input without \"input\" in requires: refused (" + e + ")");
    e = load({mod("no-draw", "requires = [\"script\"]\nscript = \"mod.lua\"\n", "function draw_game() end\n")});
    check(has(e, "\"draw\""), "draw_game without \"draw\" in requires: refused (" + e + ")");
    e = load({mod("not-listed", "requires = [\"memory\"]\nscript = \"mod.lua\"\n", "x = 1\n")});
    check(has(e, "doesn't list \"script\""), "script = without \"script\" in requires: refused (" + e + ")");
    e = load({mod("outside", "requires = [\"script\"]\nscript = \"../sandbox/mod.lua\"\n", "")});
    check(has(e, "a file in the mod's folder"), "a script outside the mod's folder: refused (" + e + ")");
    e = load({mod("no-fn", "requires = [\"script\", \"hook\"]\nscript = \"mod.lua\"\n[[hook]]\nat = 0x1000\ncall = \"gone\"\n", "x = 1\n")});
    check(has(e, "no function gone"), "a hook's call of a function the script hasn't: refused (" + e + ")");
    e = load({mod("no-memory", "requires = [\"script\"]\nscript = \"mod.lua\"\n", "function on_frame() hk.write8(0, 1) end\n")});
    frames(1);
    check(e.empty() && has(stopped(), "doesn't list \"memory\"") && g_machine.ram[0] == 0, "hk.write8 without \"memory\": the script stopped, nothing written");

    // ---- stopped while the game goes on --------------------------------------------------------------
    e = load({mod("budget", SCRIPT_TOML, "function on_frame() hk.write8(1, hk.read8(1) + 1) if hk.frame() >= 3 then while true do end end end\n")});
    frames(6);
    check(e.empty() && g_machine.ram[1] == 3 && has(stopped(), "over its budget of 1000000 instructions") && has(stopped(), "on_frame"),
          "an on_frame that never ends: stopped at its budget, frames go on (" + std::to_string(g_machine.ram[1]) + " runs)");
    e = load({mod("budget-pcall", SCRIPT_TOML, "function on_frame() for i = 1, 100 do pcall(function() while true do end end) end hk.write8(2, 1) end\n")});
    frames(2);
    check(e.empty() && g_machine.ram[2] == 0 && has(stopped(), "over its budget"), "... caught by the script's own pcall: stopped all the same");
    e = load({mod("memory", SCRIPT_TOML, "t = {}\nfunction on_frame() for i = 1, 200000 do t[i] = string.rep('x', 1000) .. i end end\n")});
    frames(2);
    check(e.empty() && has(stopped(), "over its memory, 32 MB"), "a script holding more than 32 MB: stopped (" + stopped() + ")");
    e = load({mod("error", SCRIPT_TOML, "function on_frame()\n  if hk.frame() == 2 then error('made up') end\n  hk.write8(3, hk.frame())\nend\n")});
    frames(4);
    check(e.empty() && g_machine.ram[3] == 1 && has(stopped(), "mod.lua:2: made up"), "an error in on_frame: the script stopped, its message kept (" + stopped() + ")");
    e = load({mod("draw-write", "requires = [\"script\", \"memory\", \"draw\"]\nscript = \"mod.lua\"\n", "function draw_game(g) hk.write8(4, 1) end\n")});
    frames(1);
    check(e.empty() && g_machine.ram[4] == 0 && has(stopped(), "not while drawing"), "draw_game writing memory: refused, stopped");
    e = load({mod("hook-error", "requires = [\"script\", \"hook\"]\nscript = \"mod.lua\"\n[[hook]]\nat = 0x1000\ncall = \"h\"\n",
                  "function h(regs) regs.r9 = 1 end\n")});
    uint32_t regs[8] = {};
    g_machine.reach(0x1000, regs);
    g_machine.reach(0x1000, regs);
    check(e.empty() && has(stopped(), "no register 'r9'") && has(stopped(), "a hook's call"), "a hook's call on a register the machine hasn't: stopped");

    // the exit line: a callback too small for the count hook's step is "under 1000", not 0
    e = load({mod("small", SCRIPT_TOML, "function on_frame() hk.write8(6, 1) end\n")});
    frames(3);
    {
        const auto lines = hk::modscript::report_lines();
        const std::string l = lines.empty() ? "" : lines[0];
        check(e.empty() && has(l, "4 calls"), "the exit line counts the calls, the first run included (" + l + ")");
        check(has(l, "the most instructions in one under 1000"), "... and a small callback's instructions as under 1000, not 0");
    }
    e = load({mod("bigger", SCRIPT_TOML, "function on_frame() local n = 0 for i = 1, 5000 do n = n + i end hk.write32(8, n) end\n")});
    frames(1);
    {
        const auto lines = hk::modscript::report_lines();
        const std::string l = lines.empty() ? "" : lines[0];
        check(e.empty() && has(l, "the most instructions in one ") && !has(l, "under") && !has(l, " one 0"),
              "a bigger callback: its count, to the step (" + l + ")");
    }

    // a finalizer (__gc) run as the state closes: hk refused then (at exit the machine may be gone)
    e = load({mod("finalizer", SCRIPT_TOML, "keep = setmetatable({}, {__gc = function() hk.write8(5, 1) end})\n")});
    hk::mods::clear();
    check(e.empty() && g_machine.ram[5] == 0, "a __gc finalizer as the script closes: hk does nothing there");

    // ---- the sample, twice ----------------------------------------------------------------------
    if (argc > 2) {
        const Run a = sample_run(argv[2], true);
        hk::saves::writer().drain();
        // (its settings as the first run left them, kept: the second starts from the same)
        std::filesystem::remove(g_root + "/user/data/mod-settings/test/sample-script.txt");
        const Run b = sample_run(argv[2], false);
        check(!a.ram.empty() && a.ram == b.ram && a.game == b.game && a.window == b.window,
              "the same run twice: the same memory and the same pictures");
        hk::saves::writer().drain();
    }

    printf("%s\n", failed ? "FAILED" : "all ok");
    return failed ? 1 : 0;
}
