// SPDX-License-Identifier: GPL-3.0-or-later
// Mods' files at their paths over a folder medium (runtime/host/mods.h; docs/MODS.md, "The PC
// games"): a file the game's data folder has is replaced (folder_files()), any case; one it lacks
// is added (added_files()), new folders too, the game's case kept for the folders it has; the first
// mod in the list wins for both; mod.toml, macros.txt, the files a table names and hidden files and
// folders are never the game's; [[file]] with medium = "data" replaces or adds; a folder medium
// that isn't "data" takes no new files (they're left out, as on a disc). Made-up files, no game.
//
//   test_mod_files <scratch folder>      exit 0 when all hold
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "host/mods.h"
#include "shell/core.h"

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

std::string g_root;
void put(const std::string &path, const std::string &text) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream(path, std::ios::binary) << text;
}
std::string load(const std::vector<std::string> &dirs, const std::string &game = "test") {
    hk::mods::clear();
    std::string err;
    return hk::mods::load(dirs, game, g_root, err) ? std::string() : (err.empty() ? std::string("(refused, no message)") : err);
}
// the lists as maps: the game's path (under the data folder, from "data/") -> the mod's (from the mods' folder)
std::map<std::string, std::string> rel(const std::vector<std::pair<std::string, std::string>> &v) {
    std::map<std::string, std::string> m;
    const std::string game = g_root + "/dumps/test/", mods = g_root + "/mods/";
    for (const auto &[g, f] : v)
        m[g.rfind(game, 0) == 0 ? g.substr(game.size()) : g] = f.rfind(mods, 0) == 0 ? f.substr(mods.size()) : f;
    return m;
}
std::string show(const std::map<std::string, std::string> &m) {
    std::string s;
    for (const auto &[g, f] : m) s += (s.empty() ? "" : ", ") + g + " <- " + f;
    return s;
}

}  // namespace

hk::Core &hk::core() { return g_machine; }

int main(int argc, char **argv) {
    namespace fs = std::filesystem;
    g_root = argc > 1 ? argv[1] : (fs::temp_directory_path() / "hk-mod-files-test").string();
    fs::remove_all(g_root);
    const std::string game = g_root + "/dumps/test", mods = g_root + "/mods";
    put(game + "/machine.txt", "core=pc\ndata=data\nprogram=prog\n");
    put(game + "/data/Sound/old.bin", "game");
    put(game + "/data/Sound/keep.bin", "game");
    put(game + "/data/graph/sys/system.idx", "game");
    put(game + "/data/a_folder/x", "game");
    put(game + "/prog/game.exe", "game");

    // two mods: A first in the list (it wins), B second
    put(mods + "/A/sound/OLD.bin", "A");            // the game's Sound/old.bin, in another case
    put(mods + "/A/sound/0001/0001.1", "A");        // new, in a new folder under one of the game's
    put(mods + "/A/movie/0001.4", "A");             // new, in a new folder at the top
    put(mods + "/A/graph/sys/in_new/a.bin", "A");   // new, in a new folder two deep
    put(mods + "/A/graph/sys/system.idx", "A");     // replaced
    put(mods + "/A/readme.txt", "A");               // new at the data folder's top too
    put(mods + "/A/mod.toml", "name = \"A\"\ngame = \"test\"\n[[file]]\nmedium = \"data\"\npath = \"info\\\\eout.bin\"\nfrom = \"extra/eout.bin\"\n");
    put(mods + "/A/extra/eout.bin", "A");           // named by [[file]]: data/info/eout.bin, not data/extra/
    put(mods + "/A/macros.txt", "A");
    put(mods + "/A/.git/config", "A");              // hidden folder
    put(mods + "/A/sound/.hidden", "A");            // hidden file
    put(mods + "/B/Sound/old.bin", "B");            // A's wins
    put(mods + "/B/SOUND/0001/0001.1", "B");        // A's wins (another spelling of the same new path)
    put(mods + "/B/sound/0001/0001.2", "B");        // new, beside A's
    put(mods + "/B/movie/0002.4", "B");
    put(mods + "/B/Sound/keep.bin/inside", "B");    // under a file of the game's: can't be
    put(mods + "/B/a_folder", "B");                 // over a folder of the game's: can't be

    std::string e = load({mods + "/A", mods + "/B"});
    check(e.empty(), "two mods of files load" + (e.empty() ? "" : ": " + e));
    const auto replaced = rel(hk::mods::folder_files());
    const auto added = rel(hk::mods::added_files());
    printf("      replaced: %s\n      added: %s\n", show(replaced).c_str(), show(added).c_str());
    auto is = [](const std::map<std::string, std::string> &m, const std::string &k, const std::string &v) {
        auto it = m.find(k);
        return it != m.end() && it->second == v;
    };
    check(is(replaced, "data/Sound/old.bin", "A/sound/OLD.bin"), "a game's file in another case: replaced, the first mod's");
    check(is(replaced, "data/graph/sys/system.idx", "A/graph/sys/system.idx"), "a game's file: replaced");
    check(replaced.size() == 2, "nothing else replaced (" + std::to_string(replaced.size()) + ")");
    check(is(added, "data/Sound/0001/0001.1", "A/sound/0001/0001.1"),
          "a new file in a new folder: added, under the game's own folder's case, the first mod's of two spellings");
    check(is(added, "data/Sound/0001/0001.2", "B/sound/0001/0001.2"), "... and the second mod's new file beside it");
    check(is(added, "data/movie/0001.4", "A/movie/0001.4") && is(added, "data/movie/0002.4", "B/movie/0002.4"),
          "a new folder at the top, from both mods");
    check(is(added, "data/graph/sys/in_new/a.bin", "A/graph/sys/in_new/a.bin"), "a new folder two deep");
    check(is(added, "data/readme.txt", "A/readme.txt"), "a file at the mod's top: data/ too");
    check(is(added, "data/info/eout.bin", "A/extra/eout.bin"), "[[file]] at medium = \"data\" on a new path (written with a \\): added");
    bool none_other = true;
    for (const auto &[g, f] : added)
        if (g.find("mod.toml") != std::string::npos || g.find("macros.txt") != std::string::npos || g.find(".git") != std::string::npos ||
            g.find(".hidden") != std::string::npos || g.find("extra/") != std::string::npos || g.find("keep.bin") != std::string::npos ||
            g.find("a_folder") != std::string::npos)
            none_other = false;
    check(none_other, "never added: mod.toml, macros.txt, a table's file, hidden files and folders, a path under a game's file or over its folder");
    check(added.size() == 7, "seven new files in all (" + std::to_string(added.size()) + ")");

    // B first: B's spelling and files win where both have one
    e = load({mods + "/B", mods + "/A"});
    const auto replaced2 = rel(hk::mods::folder_files());
    const auto added2 = rel(hk::mods::added_files());
    check(e.empty() && is(replaced2, "data/Sound/old.bin", "B/Sound/old.bin"), "the other order: B's replaced file wins");
    check(is(added2, "data/Sound/0001/0001.1", "B/SOUND/0001/0001.1") && added2.size() == 7, "... and B's new file, once");

    // [[file]] at a path that can't be: refused; a medium that is a folder but not "data": no new files
    put(mods + "/C/mod.toml", "name = \"C\"\ngame = \"test\"\n[[file]]\nmedium = \"data\"\npath = \"../x.bin\"\nfrom = \"x.bin\"\n");
    put(mods + "/C/x.bin", "C");
    e = load({mods + "/C"});
    check(e.find("no ../x.bin in") != std::string::npos, "[[file]] with a \"..\" in its path: refused (" + e + ")");
    put(mods + "/D/mod.toml", "name = \"D\"\ngame = \"test\"\n[[file]]\nmedium = \"program\"\npath = \"new.dll\"\nfrom = \"new.dll\"\n");
    put(mods + "/D/new.dll", "D");
    e = load({mods + "/D"});
    check(e.find("no new.dll in") != std::string::npos, "[[file]] new to a folder medium that isn't data: refused (" + e + ")");
    put(g_root + "/dumps/nodata/machine.txt", "core=pc\nprogram=prog\n");
    fs::create_directories(g_root + "/dumps/nodata/prog");
    put(mods + "/E/new.bin", "E");
    e = load({mods + "/E"}, "nodata");
    check(e.empty() && hk::mods::added_files().empty() && hk::mods::folder_files().empty(),
          "no data folder: a file matching none of the game's is left out, as before");

    printf("%s\n", failed ? "FAILED" : "all ok");
    return failed ? 1 : 0;
}
