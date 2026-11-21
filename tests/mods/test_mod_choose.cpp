// SPDX-License-Identifier: GPL-3.0-or-later
// Files and patches chosen by a mod's setting (runtime/host/mods.h, host/modsettings.h; docs/MODS.md,
// "Files and patches chosen by a setting"): a [[setting]] with named values (choices), `when` on
// [[patch]], [[bytes]] and [[file]], [[files]] folders laid over the game's data while their `when`
// holds (winning over the mod's own files, never laid at their own paths, new files added), the
// settings file and --mod-setting read before the `when`s, a change while playing said to apply
// from the next start, the layers a PC loader is handed, and the mods refused with their line.
// Made-up files and addresses, no game.
//
//   test_mod_choose <scratch folder>      exit 0 when all hold
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "host/mods.h"
#include "host/modsettings.h"
#include "shell/core.h"
#include "shell/overlay.h"
#include "shell/saves.h"

namespace {

class Machine final : public hk::Core {
public:
    bool init(const Options &) override { return true; }
    const std::string &family() const override { static const std::string f = "test"; return f; }
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
    const std::vector<uint8_t> &lamp_state() const override { static const std::vector<uint8_t> none; return none; }
    uint64_t lamp_changes() const override { return 0; }
};
Machine g_machine;

int failed = 0;
void check(bool ok, const std::string &what) {
    printf("%s%s\n", ok ? "ok    " : "FAIL  ", what.c_str());
    failed += !ok;
}
bool has(const std::string &s, const std::string &part) { return s.find(part) != std::string::npos; }

std::string g_root;
void put(const std::string &path, const std::string &text) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream(path, std::ios::binary) << text;
}
std::string read(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}
std::string load(const std::vector<std::string> &dirs, const std::vector<std::string> &overrides = {}) {
    hk::mods::clear();
    for (const std::string &o : overrides) hk::mods::override_setting(o);
    std::string err;
    return hk::mods::load(dirs, "test", g_root, err) ? std::string() : (err.empty() ? std::string("(refused, no message)") : err);
}
// the lists as maps: the game's path (from "data/") -> the mod's (from the mods' folder)
std::map<std::string, std::string> rel(const std::vector<std::pair<std::string, std::string>> &v) {
    std::map<std::string, std::string> m;
    const std::string game = g_root + "/dumps/test/", mods = g_root + "/mods/";
    for (const auto &[g, f] : v)
        m[g.rfind(game, 0) == 0 ? g.substr(game.size()) : g] = f.rfind(mods, 0) == 0 ? f.substr(mods.size()) : f;
    return m;
}
bool is(const std::map<std::string, std::string> &m, const std::string &k, const std::string &v) {
    auto it = m.find(k);
    return it != m.end() && it->second == v;
}
bool none_under(const std::map<std::string, std::string> &m, const std::string &prefix) {
    for (const auto &[g, f] : m)
        if (g.rfind(prefix, 0) == 0) return false;
    return true;
}
bool patched_at(uint32_t address) {
    for (const auto &p : hk::mods::memory_patches())
        if (p.address == address) return true;
    return false;
}
int64_t setting(const std::string &name) {
    for (auto &[n, v] : hk::mods::settings())
        if (n == name) return v;
    return -99999;
}
std::string disk_byte(const std::string &disk) {
    const int id = hk::overlay::find(disk);
    char b[4] = {'g', 'g', 'g', 'g'};
    if (id) hk::overlay::apply(id, 0, b, 3);
    return std::string(b, 3);
}

const char *TIMING =
    "name = \"Timing pick\"\ngame = \"test\"\napi = 1\nrequires = [\"settings\", \"choose\", \"actions\"]\n"
    "[[setting]]\nname = \"timing\"\nlabel = \"Backing track timing\"\nchoices = [\"Arcade\", \"PS2\"]\ndefault = \"Arcade\"\n"
    "[[setting]]\nname = \"cover\"\nmin = 0\nmax = 1\ndefault = 0\n"
    "[[action]]\nname = \"flip\"\nbinding = \"key:Keypad 5\"\nsetting = \"timing\"\ndo = \"toggle\"\n"
    "[[files]]                      # the other charts, in place of the mod's own\n"
    "folder = \"alt/ps2-timing\"\nwhen = \"timing=PS2\"\n"
    "[[files]]\nfolder = \"alt\\\\arcade-extra/\"\nwhen = \"timing=Arcade\"\n"
    "[[patch]]\naddress = 0x100\noriginal = \"00\"\nreplace = \"01\"\nwhen = \"timing=PS2\"\n"
    "[[patch]]\naddress = 0x200\noriginal = \"00\"\nreplace = \"02\"\nwhen = \"timing = 0\"\n"
    "[[patch]]\naddress = 0x300\noriginal = \"00\"\nreplace = \"03\"\n"
    "[[bytes]]\nmedium = \"disk\"\noffset = 0\nfrom = \"b/ps2.bin\"\nwhen = \"timing=ps2\"\n"
    "[[file]]\nmedium = \"data\"\npath = \"Sound/0001/0001.2\"\nfrom = \"b/two.bin\"\nwhen = \"cover=1\"\n";

}  // namespace

hk::Core &hk::core() { return g_machine; }

int main(int argc, char **argv) {
    namespace fs = std::filesystem;
    g_root = argc > 1 ? argv[1] : (fs::temp_directory_path() / "hk-mod-choose-test").string();
    fs::remove_all(g_root);
    fs::create_directories(g_root + "/user");   // a portable folder: the settings in <root>/user/data
    hk::mods::offer("actions");
    const std::string game = g_root + "/dumps/test", mods = g_root + "/mods", m = mods + "/timing";
    put(game + "/machine.txt", "core=pc\ndata=data\ndisk=disk.bin\n");
    put(game + "/disk.bin", "gggggggg");
    put(game + "/data/Sound/0001/0001.1", "game");
    put(game + "/data/Sound/0001/0001.2", "game");
    put(m + "/mod.toml", TIMING);
    put(m + "/sound/0001/0001.1", "own");
    put(m + "/alt/ps2-timing/sound/0001/0001.1", "ps2");        // wins over the mod's own while PS2
    put(m + "/alt/ps2-timing/sound/0002/0002.1", "ps2");        // new to the game's data
    put(m + "/alt/arcade-extra/readme2.txt", "arcade");
    put(m + "/b/ps2.bin", "PS2");
    put(m + "/b/two.bin", "two");
    const std::string kept = g_root + "/user/data/mod-settings/test/timing.txt";

    // ---- the default: Arcade -------------------------------------------------------------------
    std::string e = load({m});
    check(e.empty(), "a mod with choices, [[files]] and when loads" + (e.empty() ? "" : ": " + e));
    auto replaced = rel(hk::mods::folder_files()), added = rel(hk::mods::added_files());
    check(setting("timing/timing") == 0, "choices: the default by name (\"Arcade\") is 0");
    check(is(replaced, "data/Sound/0001/0001.1", "timing/sound/0001/0001.1"), "Arcade: the mod's own file");
    check(is(added, "data/readme2.txt", "timing/alt/arcade-extra/readme2.txt"),
          "Arcade: the Arcade [[files]] folder (written with a \\ and a / at its end) laid over the data, a new file added");
    check(none_under(replaced, "data/alt") && none_under(added, "data/alt") && !added.count("data/Sound/0002/0002.1"),
          "nothing of a [[files]] folder at its own path in the mod (data/alt/...), nor the PS2 folder's");
    check(!added.count("data/b/ps2.bin") && !added.count("data/b/two.bin") && !replaced.count("data/Sound/0001/0001.2"),
          "a table's file whose when doesn't hold: not laid at its path either, and its [[file]] not used");
    check(!patched_at(0x100) && patched_at(0x200) && patched_at(0x300), "patches: the Arcade one and the one without when");
    check(hk::overlay::find(game + "/disk.bin") == 0, "[[bytes]] with when = PS2: not laid on the disk");
    const auto layers0 = hk::mods::layers();
    check(layers0.size() == 2 && layers0[0].dir == m + "/alt/arcade-extra" && layers0[1].dir == m && layers0[1].left_out.size() == 2,
          "a PC loader's layers: the chosen [[files]] folder, then the mod's own, which leaves both folders out");

    // ---- --mod-setting before the whens: PS2 ---------------------------------------------------
    e = load({m}, {"timing=PS2"});
    replaced = rel(hk::mods::folder_files()), added = rel(hk::mods::added_files());
    check(e.empty() && setting("timing/timing") == 1, "--mod-setting timing=PS2 (by name)" + (e.empty() ? "" : ": " + e));
    check(is(replaced, "data/Sound/0001/0001.1", "timing/alt/ps2-timing/sound/0001/0001.1"),
          "PS2: the [[files]] folder's file wins over the mod's own at the same path");
    check(is(added, "data/Sound/0002/0002.1", "timing/alt/ps2-timing/sound/0002/0002.1"), "PS2: its new file added to the data");
    check(!added.count("data/readme2.txt") && none_under(added, "data/alt") && none_under(replaced, "data/alt"),
          "PS2: the Arcade folder not laid, nor either at its own path");
    check(patched_at(0x100) && !patched_at(0x200) && patched_at(0x300), "patches: the PS2 one and the one without when");
    check(disk_byte(game + "/disk.bin") == "PS2", "[[bytes]] with when = PS2 (\"ps2\", any case): laid on the disk");
    const auto layers1 = hk::mods::layers();
    check(layers1.size() == 2 && layers1[0].dir == m + "/alt/ps2-timing" && layers1[0].left_out.empty(), "a PC loader's layers: the PS2 folder first");
    check(load({m}, {"timing=1"}).empty() && setting("timing/timing") == 1, "--mod-setting by number");
    e = load({m}, {"timing=Sega"});
    check(has(e, "--mod-setting timing=Sega") && has(e, "choices"), "--mod-setting with a name it hasn't: refused (" + e + ")");

    // ---- kept: the settings file, by number or by name ------------------------------------------
    put(kept, "# the player's note\ntiming=ps2\ncover=1\naction.flip=key:F9\nretired=4\n");
    e = load({m});
    replaced = rel(hk::mods::folder_files());
    check(e.empty() && setting("timing/timing") == 1 && setting("timing/cover") == 1, "kept values read before the whens (a name too)");
    check(is(replaced, "data/Sound/0001/0001.2", "timing/b/two.bin"), "[[file]] with when = cover=1: used");
    check(hk::mods::action_bindings().size() == 1 && hk::mods::action_bindings()[0] == "key:F9", "the action's binding from the file");
    // a change while playing: from the next start, said
    const auto said = hk::mods::action(0, true);
    check(said.size() == 3 && said[1] == "Backing track timing: Arcade" && has(said[2], "next start"),
          "an action that changes a chosen-by setting: its value by name, and \"from the next start\"");
    hk::saves::writer().drain();
    const std::string text = read(kept);
    check(has(text, "timing=0\n") && has(text, "action.flip=key:F9\n"), "kept as a number, with the action's line");
    int64_t now = 0;
    check(hk::mods::set_setting("timing", "timing", 1, now) && now == 1, "a script sets it (the log and the window say next start)");
    hk::saves::writer().drain();
    replaced = rel(hk::mods::folder_files());
    check(is(replaced, "data/Sound/0001/0001.1", "timing/alt/ps2-timing/sound/0001/0001.1"), "the files this run started with (PS2's) stay as they were");
    fs::remove(kept);

    // ---- the shared reader's writer: other lines kept --------------------------------------------
    const std::string old = "# note\ntiming=1\naction.flip=key:F9\nretired=4\n";
    const std::string w = hk::modsettings::written(old, {{"timing", "0"}, {"cover", "1"}});
    check(w == "# note\ntiming=0\naction.flip=key:F9\nretired=4\ncover=1\n", "written(): its line changed, a new one added, the rest kept");
    check(hk::modsettings::written("", {{"a", "1"}}) == std::string(hk::modsettings::kFileHead) + "a=1\n", "written() of a new file: the head line first");

    // ---- refused, with the file's line ------------------------------------------------------------
    auto mod = [&](const char *name, const std::string &body) {
        put(mods + "/" + name + "/mod.toml", body);
        fs::create_directories(mods + "/" + name + "/alt/x");
        return mods + "/" + name;
    };
    const std::string head = "name = \"x\"\ngame = \"test\"\napi = 1\nrequires = [\"settings\", \"choose\"]\n"
                             "[[setting]]\nname = \"t\"\nchoices = [\"A\", \"B\"]\n";
    e = load({mod("no-setting", head + "[[patch]]\naddress = 1\nreplace = \"00\"\nwhen = \"nope=1\"\n")});
    check(has(e, "mod.toml:8") && has(e, "no [[setting]] 'nope'"), "a when naming a setting the mod hasn't: refused, with its line (" + e + ")");
    e = load({mod("bad-name", head + "[[files]]\nfolder = \"alt/x\"\nwhen = \"t=C\"\n")});
    check(has(e, "mod.toml:8") && has(e, "no value \"C\"") && has(e, "A, B"), "a value it hasn't: refused, its values named (" + e + ")");
    e = load({mod("out-of-range", head + "[[files]]\nfolder = \"alt/x\"\nwhen = \"t=2\"\n")});
    check(has(e, "no value \"2\""), "a number outside its range: refused (" + e + ")");
    e = load({mod("no-folder", head + "[[files]]\nfolder = \"alt/none\"\nwhen = \"t=A\"\n")});
    check(has(e, "isn't a folder in the mod's"), "a [[files]] folder that isn't there: refused (" + e + ")");
    e = load({mod("up-folder", head + "[[files]]\nfolder = \"../x\"\n")});
    check(has(e, "'folder' is a folder inside the mod's"), "a [[files]] folder outside the mod: refused (" + e + ")");
    e = load({mod("not-required", "name = \"x\"\ngame = \"test\"\napi = 1\nrequires = [\"settings\"]\n"
                                  "[[setting]]\nname = \"t\"\nmin = 0\nmax = 1\ndefault = 0\n[[files]]\nfolder = \"alt/x\"\nwhen = \"t=1\"\n")});
    check(has(e, "doesn't list \"choose\""), "when/[[files]] without \"choose\" in requires: refused (" + e + ")");
    e = load({mod("choices-min", "name = \"x\"\ngame = \"test\"\napi = 1\nrequires = [\"settings\", \"choose\"]\n"
                                 "[[setting]]\nname = \"t\"\nchoices = [\"A\", \"B\"]\nmin = 0\n")});
    check(has(e, "with 'choices', 'min' and 'max' are theirs"), "choices with min: refused (" + e + ")");
    e = load({mod("choices-one", "name = \"x\"\ngame = \"test\"\napi = 1\nrequires = [\"settings\", \"choose\"]\n"
                                 "[[setting]]\nname = \"t\"\nchoices = [\"A\"]\n")});
    check(has(e, "two values or more"), "one choice: refused (" + e + ")");
    e = load({mod("no-api", "name = \"x\"\ngame = \"test\"\n[[files]]\nfolder = \"alt/x\"\n")});
    check(has(e, "[[files]] is the mod API's"), "[[files]] without api = 1: refused (" + e + ")");
    // a [[hook]]'s when is still its register (not a setting's)
    hk::mods::clear();

    printf("%s\n", failed ? "FAILED" : "all ok");
    return failed ? 1 : 0;
}
