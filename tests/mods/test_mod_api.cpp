// SPDX-License-Identifier: GPL-3.0-or-later
// The mod API's side in mod.toml (runtime/host/mods.h; docs/MODS.md, "Hooks, settings and
// actions"), on a made-up machine: api and requires accepted and refused with the messages that
// name what's missing, the program's SHA-1, [[hook]]s as the core is handed them (a register or RAM
// in the machine's byte order changed by a setting, scaled; a condition; a counter), [[setting]]s
// clamped, stepped, kept in the user's folder and read back, [[action]]s moving them, and
// --mod-setting; quiet actions (notify = false). Made-up mods and addresses, no game.
//
//   test_mod_api <scratch folder>      exit 0 when all hold
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "host/mods.h"
#include "shell/core.h"
#include "shell/saves.h"
#include "shell/sha1.h"

namespace {

// a machine of 64 bytes of memory and 4 registers, big-endian, with hooks (as Firebeat's executor)
class Machine final : public hk::Core {
public:
    uint8_t ram[64] = {};
    bool has_hooks = true;
    std::string other, program;
    struct Hooked { uint32_t at; ModHook fn; void *ctx; std::string label; };
    std::vector<Hooked> hooked;
    bool init(const Options &) override { return true; }
    const std::string &family() const override { static const std::string f = "test"; return f; }
    void set_frame_hook(FrameHook) override {}
    void set_audio_sink(AudioSink) override {}
    void set_on_fatal(std::function<void()>) override {}
    [[noreturn]] void run() override { abort(); }
    void shutdown() override {}
    void report() override {}
    double seconds() const override { return 0; }
    const uint8_t *mem(uint32_t addr, uint32_t len) const override { return (uint64_t)addr + len <= sizeof ram ? ram + addr : nullptr; }
    uint64_t frame_count() const override { return 0; }
    bool set_input(const std::string &, int) override { return false; }
    void turntable_add(int, int) override {}
    const std::vector<uint8_t> &lamp_state() const override { static const std::vector<uint8_t> none; return none; }
    uint64_t lamp_changes() const override { return 0; }
    std::vector<std::string> mod_capabilities() const override { return has_hooks ? std::vector<std::string>{"hook"} : std::vector<std::string>{}; }
    std::vector<std::string> mod_registers() const override { return {"r0", "r1", "r2", "r3"}; }
    bool mod_big_endian() const override { return true; }
    bool mod_hook(uint32_t at, ModHook fn, void *ctx, const std::string &label, std::string &err) override {
        if (!has_hooks) {
            err = "no hooks";
            return false;
        }
        hooked.push_back({at, fn, ctx, label});
        return true;
    }
    std::string mod_program_file(const std::string &, const std::string &) const override { return program; }
    std::string mod_other_program() const override { return other; }
    // the hooks at `at`, run as the core would before its instruction
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

std::string g_root;
std::string mod(const char *name, const std::string &body) {
    const std::string dir = g_root + "/mods/" + name;
    std::filesystem::create_directories(dir);
    std::ofstream(dir + "/mod.toml") << body;
    return dir;
}
// load() from nothing: what it said (empty when it took the mods)
std::string load(const std::vector<std::string> &dirs, bool fresh = true) {
    if (fresh) hk::mods::clear();   // (--mod-setting's values too: given after it, before load)
    g_machine.hooked.clear();
    std::string err;
    const bool ok = hk::mods::load(dirs, "test", g_root, err);
    return ok ? std::string() : (err.empty() ? std::string("(refused, no message)") : err);
}
bool has(const std::string &s, const std::string &part) { return s.find(part) != std::string::npos; }
int64_t setting(const std::string &name) {
    for (auto &[n, v] : hk::mods::settings())
        if (n == name) return v;
    return -99999;
}
uint64_t counter(const std::string &name) {
    for (auto &[n, v] : hk::mods::counters())
        if (n == name) return v;
    return ~0ull;
}
std::string read(const std::string &path) {
    std::ifstream f(path);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

const char *TIMING =
    "name = \"Timing\"\ngame = \"test\"\napi = 1\nrequires = [\"hook\", \"settings\", \"actions\"]   # what it uses\n"
    "[[setting]]\nname = \"shift\"\nlabel = \"Shift (ms)\"\nmin = -100\nmax = 100\ndefault = 0\nstep = 5\n"
    "[[setting]]\nname = \"zones\"\nmin = 0\nmax = 1\ndefault = 1\n"
    "[[action]]\nname = \"later\"\nbinding = \"key:Keypad 8\"\nsetting = \"shift\"\ndo = \"up\"\n"
    "[[action]]\nname = \"earlier\"\nbinding = \"key:Keypad 2\"\nsetting = \"shift\"\ndo = \"down\"\n"
    "[[action]]\nname = \"zones\"\nbinding = \"key:Keypad 0\"\nsetting = \"zones\"\ndo = \"toggle\"\n"
    "[[hook]]          # the time a press is judged by, in r3: earlier by the setting\n"
    "at = 0x1000\nsub = \"r3\"\nvalue = \"setting:shift\"\n"
    "[[hook]]          # ... and the word at 0x10 (2 bytes), by the setting times 2\n"
    "at = 0x1000\nadd = 0x10\nsize = 2\nvalue = \"setting:shift\"\nscale = 2\n"
    "[[hook]]          # judgments: r2 holds the kind; 2 to 9 counted\n"
    "at = 0x2000\ncount = \"not-best\"\nwhen = \"r2\"\nmin = 2\nmax = 9\n"
    "[[hook]]\nat = 0x2000\ncount = \"all\"\n"
    "[[hook]]          # a set, by a number\n"
    "at = 0x3000\nset = \"r1\"\nvalue = -1\n";

// a mod that shows its own values: its keys move a setting without HIRAKU's box (notify = false)
const char *QUIET =
    "name = \"Own panel\"\ngame = \"test\"\napi = 1\nrequires = [\"settings\", \"actions\", \"quiet\"]\n"
    "[[setting]]\nname = \"level\"\nlabel = \"Level\"\nmin = 0\nmax = 3\ndefault = 1\n"
    "[[action]]\nname = \"up\"\nbinding = \"key:Keypad 9\"\nsetting = \"level\"\ndo = \"up\"\nnotify = false\n"
    "[[action]]\nname = \"down\"\nbinding = \"key:Keypad 3\"\nsetting = \"level\"\ndo = \"down\"\nnotify = true\n";

}  // namespace

hk::Core &hk::core() { return g_machine; }

int main(int argc, char **argv) {
    namespace fs = std::filesystem;
    g_root = argc > 1 ? argv[1] : (fs::temp_directory_path() / "hk-mod-api-test").string();
    fs::remove_all(g_root);
    fs::create_directories(g_root + "/user");   // a portable folder: the settings in <root>/user/data
    {
        hk::Sha1 s;
        s.add("abc", 3);
        check(s.hex() == "a9993e364706816aba3e25717850c26c9cd0d89d", "SHA-1 of \"abc\" (FIPS 180's example)");
    }

    // ---- accepted, and what the core is handed -------------------------------------------------
    const std::string timing = mod("timing", TIMING);
    std::string e = load({timing});
    check(has(e, "this mod needs actions; this core/build has none"), "before main() offers actions (a window): refused (" + e + ")");
    auto offered = [](const char *c) {
        const auto have = hk::mods::capabilities();
        return std::find(have.begin(), have.end(), c) != have.end();
    };
    check(!offered("quiet"), "no \"quiet\" without \"actions\"");
    hk::mods::offer("actions");
    check(offered("quiet"), "\"quiet\" offered with \"actions\"");
    e = load({timing});
    check(e.empty(), "a mod with api = 1, requires and all three tables loads" + (e.empty() ? "" : ": " + e));
    check(g_machine.hooked.size() == 5, "its 5 hooks handed to the core (" + std::to_string(g_machine.hooked.size()) + ")");
    check(!g_machine.hooked.empty() && has(g_machine.hooked[0].label, "mod.toml:"), "each with its place in mod.toml for messages");
    check(setting("timing/shift") == 0 && setting("timing/zones") == 1, "settings at the mod's defaults (nothing kept yet)");
    uint32_t regs[4] = {0, 7, 9, 1000};
    g_machine.ram[0x10] = 0x01, g_machine.ram[0x11] = 0x00;   // 0x0100, big-endian
    g_machine.reach(0x1000, regs);
    check(regs[3] == 1000 && g_machine.ram[0x10] == 0x01 && g_machine.ram[0x11] == 0x00, "at 0: the hooks change nothing");

    // actions: up by the step, kept; down; to the ends and no further; toggle
    const auto bindings = hk::mods::action_bindings();
    check(bindings.size() == 3 && bindings[0] == "key:Keypad 8", "the actions' bindings, in order");
    auto said = hk::mods::action(0, true);
    check(setting("timing/shift") == 5 && said.size() == 2 && said[0] == "Timing" && said[1] == "Shift (ms): +5",
          "an action moves its setting up by its step, and says it (\"" + (said.size() > 1 ? said[1] : "") + "\")");
    check(hk::mods::action(0, false).empty() && setting("timing/shift") == 5, "let go: nothing");
    regs[3] = 1000;
    g_machine.reach(0x1000, regs);
    check(regs[3] == 995, "the register: 1000 less the setting, 5 (" + std::to_string(regs[3]) + ")");
    check(g_machine.ram[0x10] == 0x01 && g_machine.ram[0x11] == 0x0a, "RAM, 2 bytes big-endian: 0x0100 + 5 x 2 = 0x010a");
    for (int k = 0; k < 30; k++) hk::mods::action(0, true, k > 0);
    check(setting("timing/shift") == 100, "held up: stops at max (100)");
    for (int k = 0; k < 50; k++) hk::mods::action(1, true);
    check(setting("timing/shift") == -100, "down: stops at min (-100)");
    hk::mods::action(2, true);
    check(setting("timing/zones") == 0, "toggle: from max to min");
    hk::mods::action(2, true, true);
    check(setting("timing/zones") == 0, "toggle: a key held down (repeat) doesn't toggle again");
    hk::mods::action(2, true);
    hk::mods::action(1, true, true);   // -100 still
    hk::mods::action(0, true);         // -95
    check(setting("timing/zones") == 1 && setting("timing/shift") == -95, "toggle back to max; shift at -95");
    regs[3] = 1000;
    g_machine.reach(0x1000, regs);
    check(regs[3] == 1095, "a negative setting: the register later (1000 + 95)");

    // counters, with and without a condition
    for (uint32_t kind : {1u, 2u, 9u, 10u, 5u}) {
        regs[2] = kind;
        g_machine.reach(0x2000, regs);
    }
    check(counter("timing/not-best") == 3 && counter("timing/all") == 5, "counters: 3 of 5 with r2 from 2 to 9, all 5");
    regs[1] = 7;
    g_machine.reach(0x3000, regs);
    check(regs[1] == 0xffffffffu, "set r1 = -1");

    // kept: the settings file (written by the saves' writer), read back at the next load
    hk::saves::writer().drain();
    const std::string kept = g_root + "/user/data/mod-settings/test/timing.txt";
    const std::string text = read(kept);
    check(has(text, "shift=-95\n") && has(text, "zones=1\n") && has(text, "action.later=key:Keypad 8\n"),
          "kept in <user data>/mod-settings/<game>/<mod folder>.txt: values and bindings");
    {
        std::ofstream(kept) << "shift=250\nzones=1\naction.later=key:F5\n";   // out of range; a binding changed
    }
    e = load({timing});
    check(e.empty() && setting("timing/shift") == 100, "read back at the next start, clamped to max (250 -> 100)");
    check(hk::mods::action_bindings()[0] == "key:F5", "an action's binding as the settings file has it");
    // --mod-setting: this run's value, not kept
    hk::mods::clear();
    hk::mods::override_setting("shift=-7");
    e = load({timing}, false);
    check(e.empty() && setting("timing/shift") == -7, "--mod-setting shift=-7");
    hk::saves::writer().drain();
    check(has(read(kept), "shift=250"), "... not written to the file");
    hk::mods::clear();
    hk::mods::override_setting("nothing=1");
    e = load({timing}, false);
    check(has(e, "--mod-setting nothing") && has(e, "no mod"), "--mod-setting of a name no mod has: refused (" + e + ")");

    // quiet actions: the setting moved, clamped and kept as any action's, but no lines for the box
    hk::saves::writer().drain();
    e = load({mod("quiet", QUIET)});
    check(e.empty(), "a mod with notify = false and \"quiet\" in requires loads" + (e.empty() ? "" : ": " + e));
    check(hk::mods::action_bindings().size() == 2 && hk::mods::action_bindings()[0] == "key:Keypad 9", "a quiet action's binding as any");
    said = hk::mods::action(0, true);
    check(setting("quiet/level") == 2 && said.empty(), "a quiet action moves its setting (1 -> 2) and returns no lines");
    for (int k = 0; k < 5; k++) said = hk::mods::action(0, true, true);
    check(setting("quiet/level") == 3 && said.empty(), "held: repeats, stops at max (3), still no lines");
    said = hk::mods::action(1, true);
    check(setting("quiet/level") == 2 && said.size() == 2 && said[0] == "Own panel" && said[1] == "Level: 2",
          "notify = true written out: the box's lines as before (\"" + (said.size() > 1 ? said[1] : "") + "\")");
    hk::saves::writer().drain();
    check(has(read(g_root + "/user/data/mod-settings/test/quiet.txt"), "level=2\n") &&
              has(read(g_root + "/user/data/mod-settings/test/quiet.txt"), "action.up=key:Keypad 9\n"),
          "a quiet action's change kept in the settings file, its binding too");
    hk::mods::action(0, true);
    hk::saves::writer().drain();
    check(has(read(g_root + "/user/data/mod-settings/test/quiet.txt"), "level=3\n"), "... each change written");
    const std::string quiet_head =
        "name = \"x\"\ngame = \"test\"\napi = 1\nrequires = [\"settings\", \"actions\"]\n"
        "[[setting]]\nname = \"s\"\nmin = 0\nmax = 1\ndefault = 0\n[[action]]\nname = \"a\"\nbinding = \"key:F1\"\nsetting = \"s\"\ndo = \"toggle\"\n";
    e = load({mod("quiet-not-listed", quiet_head + "notify = false\n")});
    check(has(e, "notify = false") && has(e, "doesn't list \"quiet\""), "notify = false without \"quiet\" in requires: refused (" + e + ")");
    e = load({mod("loud-written", quiet_head + "notify = true\n")});
    check(e.empty(), "notify = true needs nothing in requires" + (e.empty() ? "" : ": " + e));
    e = load({mod("quiet-string", quiet_head + "notify = \"false\"\n")});
    check(has(e, "mod.toml:10") && has(e, "'notify' is true or false"), "notify in quotes: refused, with its line (" + e + ")");
    e = load({mod("quiet-number", quiet_head + "notify = 0\n")});
    check(has(e, "mod.toml:10") && has(e, "'notify' is true or false"), "notify = 0: refused (" + e + ")");

    // ---- refused, with what's missing named --------------------------------------------------
    e = load({mod("no-api", "name = \"x\"\ngame = \"test\"\n[[hook]]\nat = 0x1000\ncount = \"n\"\n")});
    check(has(e, "[[hook]] is the mod API's") && has(e, "api = 1"), "[[hook]] without api = 1: refused (" + e + ")");
    e = load({mod("not-listed", "name = \"x\"\ngame = \"test\"\napi = 1\nrequires = [\"settings\"]\n[[hook]]\nat = 0x1000\ncount = \"n\"\n")});
    check(has(e, "doesn't list \"hook\""), "[[hook]] not in requires: refused (" + e + ")");
    e = load({mod("api2", "name = \"x\"\ngame = \"test\"\napi = 2\n")});
    check(has(e, "mod API 2") && has(e, "this HIRAKU's is 1"), "api = 2: refused (" + e + ")");
    e = load({mod("unknown-cap", "name = \"x\"\ngame = \"test\"\napi = 1\nrequires = [\"hook\", \"draw\"]\n")});
    check(has(e, "this mod needs draw; this core/build has none"), "requires a capability nobody has: named (" + e + ")");
    g_machine.has_hooks = false;
    g_machine.other = "hiraku-other";
    e = load({timing});
    check(has(e, "this mod needs hook; this core/build has none") && has(e, "hiraku-other has it"),
          "a build without hooks: \"needs hook\", and the program that has them (" + e + ")");
    g_machine.has_hooks = true;
    g_machine.other.clear();
    e = load({mod("bad-reg", "name = \"x\"\ngame = \"test\"\napi = 1\nrequires = [\"hook\"]\n[[hook]]\nat = 0x1000\nadd = \"r9\"\nvalue = 1\n")});
    check(has(e, "no register 'r9'") && has(e, "r0 r1 r2 r3"), "a register the machine hasn't: refused, its names listed (" + e + ")");
    e = load({mod("no-setting", "name = \"x\"\ngame = \"test\"\napi = 1\nrequires = [\"hook\"]\n[[hook]]\nat = 0x1000\nadd = \"r1\"\nvalue = \"setting:gone\"\n")});
    check(has(e, "no [[setting]] 'gone'"), "a hook's setting the mod hasn't: refused (" + e + ")");
    e = load({mod("two-things", "name = \"x\"\ngame = \"test\"\napi = 1\nrequires = [\"hook\"]\n[[hook]]\nat = 0x1000\nadd = \"r1\"\ncount = \"n\"\nvalue = 1\n")});
    check(has(e, "one thing"), "a hook doing two things: refused (" + e + ")");
    e = load({mod("bad-default", "name = \"x\"\ngame = \"test\"\napi = 1\nrequires = [\"settings\"]\n[[setting]]\nname = \"s\"\nmin = 0\nmax = 10\ndefault = 11\n")});
    check(has(e, "'default' is from 'min' to 'max'"), "a default outside min..max: refused (" + e + ")");

    // the program: its SHA-1 against the file the core says it runs
    const std::string program = g_root + "/program.bin";
    std::ofstream(program, std::ios::binary) << "abc";
    g_machine.program = program;
    e = load({mod("for-abc", "name = \"x\"\ngame = \"test\"\napi = 1\nprogram = \"A9993E364706816ABA3E25717850C26C9CD0D89D\"\n")});
    check(e.empty(), "program = the program's SHA-1 (any case): taken" + (e.empty() ? "" : ": " + e));
    e = load({mod("for-other", "name = \"x\"\ngame = \"test\"\napi = 1\nprogram = \"0000000000000000000000000000000000000000\"\n")});
    check(has(e, "this run's (" + program + ") is a9993e36"), "program = another one's: refused, both named (" + e + ")");
    const std::string text_api = hk::mods::api_text(g_root, "test");
    check(has(text_api, "mod API 1\n") && has(text_api, "capabilities: hook settings") && has(text_api, " actions quiet") && has(text_api, "registers: r0 r1 r2 r3") &&
              has(text_api, "big-endian") && has(text_api, "a9993e36"),
          "--mod-api's text");

    // the sample under tests/mods/sample-hook (made-up addresses): loads as it is
    if (argc > 2) {
        e = load({argv[2]});
        check(e.empty() && g_machine.hooked.size() >= 4, "tests/mods/sample-hook loads" + (e.empty() ? "" : ": " + e));
    }

    printf("%s\n", failed ? "FAILED" : "all ok");
    return failed ? 1 : 0;
}
