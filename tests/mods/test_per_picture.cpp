// SPDX-License-Identifier: GPL-3.0-or-later
// Mods' per_picture [[patch]] (runtime/host/mods.h, docs/MODS.md): a float of one cabinet frame's
// time holds the rate's picture's while a [[rate]] is on (half of it at twice the machine's rate),
// and its original once the rate is off. Made-up mod and memory.
//
//   test_per_picture      exit 0 when all hold
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "host/mods.h"
#include "shell/core.h"

namespace {

// a machine of 64 bytes of memory whose vsync rate a mod may change
class Machine final : public hk::Core {
public:
    uint8_t ram[64] = {};
    double hz = -1;   // the rate asked for (0: its own)
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
    void code_changed(uint32_t, uint32_t) override {}
    uint64_t frame_count() const override { return 0; }
    bool set_input(const std::string &, int) override { return false; }
    void turntable_add(int, int) override {}
    const std::vector<uint8_t> &lamp_state() const override { static const std::vector<uint8_t> none; return none; }
    uint64_t lamp_changes() const override { return 0; }
    bool set_vsync_rate(double h) override { hz = h; return true; }
    double native_rate() const override { return 60.0; }
};
Machine g_machine;

int failed = 0;
void check(bool ok, const char *what) {
    printf("%s%s\n", ok ? "ok    " : "FAIL  ", what);
    failed += !ok;
}

float at(uint32_t a) {
    float f;
    memcpy(&f, g_machine.ram + a, 4);
    return f;
}

}  // namespace

hk::Core &hk::core() { return g_machine; }

int main() {
    namespace fs = std::filesystem;
    const std::string root = (fs::temp_directory_path() / "hk-per-picture-test").string();
    fs::remove_all(root);
    // the rate (x2) while the word at 0 changes; the frame's time (1/60 s) at 0x20
    const float frame = 1.0f / 60;
    memcpy(g_machine.ram + 0x20, &frame, 4);
    char orig[16];
    snprintf(orig, sizeof orig, "%02x%02x%02x%02x", g_machine.ram[0x20], g_machine.ram[0x21], g_machine.ram[0x22], g_machine.ram[0x23]);
    const std::string dir = root + "/m";
    fs::create_directories(dir);
    std::ofstream(dir + "/mod.toml") << "name = \"m\"\ngame = \"test\"\n[[rate]]\nhz = \"x2\"\nwhile_changing = 0\n"
                                     << "[[patch]]\naddress = 0x20\noriginal = \"" << orig << "\"\nper_picture = true\n";
    std::string err;
    check(hk::mods::load({dir}, "test", root, err), "a per_picture patch loads");
    if (!err.empty()) printf("      %s\n", err.c_str());
    hk::mods::frame();
    check(at(0x20) == frame, "the rate off: the cabinet frame's time");
    for (int i = 0; i < 3; i++) g_machine.ram[0]++, hk::mods::frame();
    check(g_machine.hz == 120.0, "the word changing: the rate on, at twice the machine's");
    check(at(0x20) == frame / 2, "the rate on: half the frame's time");
    for (int i = 0; i < 3; i++) hk::mods::frame();
    check(g_machine.hz == 0, "the word still: the machine's own rate again (0)");
    check(at(0x20) == frame, "the rate off again: the original back");
    // a per_picture patch with a replace, or not a float, is refused
    std::ofstream(dir + "/mod.toml", std::ios::trunc) << "name = \"m\"\ngame = \"test\"\n[[patch]]\naddress = 0x20\noriginal = \"0011\"\n"
                                                         "per_picture = true\n";
    check(!hk::mods::load({dir}, "test", root, err), "two bytes: refused");
    fs::remove_all(root);
    printf(failed ? "%d failed\n" : "all hold\n", failed);
    return failed ? 1 : 0;
}
