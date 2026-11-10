// SPDX-License-Identifier: GPL-3.0-or-later
// Mods' [[patch]] (runtime/host/mods.h): where two mods patch the same bytes, the first in the list
// wins, as with their files (docs/MODS.md). Made-up mods and memory.
//
//   test_patch_order      exit 0 when all hold
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "host/mods.h"
#include "shell/core.h"

namespace {

// a machine of 64 bytes of memory, which counts what it is told of code changed
class Machine final : public hk::Core {
public:
    uint8_t ram[64] = {};
    int changed = 0;
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
    void code_changed(uint32_t, uint32_t) override { changed++; }
    uint64_t frame_count() const override { return 0; }
    bool set_input(const std::string &, int) override { return false; }
    void turntable_add(int, int) override {}
    const std::vector<uint8_t> &lamp_state() const override { static const std::vector<uint8_t> none; return none; }
    uint64_t lamp_changes() const override { return 0; }
};
Machine g_machine;

int failed = 0;
void check(bool ok, const char *what) {
    printf("%s%s\n", ok ? "ok    " : "FAIL  ", what);
    failed += !ok;
}

std::string mod(const std::string &root, const char *name, const char *patches) {
    const std::string dir = root + "/" + name;
    std::filesystem::create_directories(dir);
    std::ofstream(dir + "/mod.toml") << "name = \"" << name << "\"\ngame = \"test\"\n" << patches;
    return dir;
}

}  // namespace

hk::Core &hk::core() { return g_machine; }

int main() {
    namespace fs = std::filesystem;
    const std::string root = (fs::temp_directory_path() / "hk-patch-order-test").string();
    fs::remove_all(root);
    // both find the game's bytes (11 22) at 0x10; both write 0x20 every frame; b alone has 0x30
    const std::string a = mod(root, "a", "[[patch]]\naddress = 0x10\noriginal = \"11 22\"\nreplace = \"aa aa\"\n"
                                         "[[patch]]\naddress = 0x20\nreplace = \"a1\"\n");
    const std::string b = mod(root, "b", "[[patch]]\naddress = 0x10\noriginal = \"11 22\"\nreplace = \"bb bb\"\n"
                                         "[[patch]]\naddress = 0x20\nreplace = \"b1\"\n"
                                         "[[patch]]\naddress = 0x30\nreplace = \"b2\"\n");
    g_machine.ram[0x10] = 0x11, g_machine.ram[0x11] = 0x22;
    std::string err;
    check(hk::mods::load({a, b}, "test", root, err), "two mods load");
    if (!err.empty()) printf("      %s\n", err.c_str());
    hk::mods::frame();
    check(g_machine.ram[0x10] == 0xaa && g_machine.ram[0x11] == 0xaa, "the same bytes found by both: the first mod's are in place");
    check(g_machine.ram[0x20] == 0xa1, "the same byte written every frame by both: the first mod's is in place");
    check(g_machine.ram[0x30] == 0xb2, "a byte only the second mod writes: in place");
    const int before = g_machine.changed;
    hk::mods::frame();
    hk::mods::frame();
    check(g_machine.changed == before, "frames later: nothing written again (the two aren't put in in turn)");
    check(g_machine.ram[0x10] == 0xaa && g_machine.ram[0x20] == 0xa1, "frames later: still the first mod's");
    fs::remove_all(root);
    printf(failed ? "%d failed\n" : "all hold\n", failed);
    return failed ? 1 : 0;
}
