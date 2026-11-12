// SPDX-License-Identifier: GPL-3.0-or-later
// The Firebeat core: a recompiled PowerPC program on the Firebeat board, with
// the SPU sound board or the board's YMZ280B, and Keyboardmania's synth card.
//
// Compiled into each hiraku-<game> binary (not a library), because the entry
// point and load address are per game: HK_ENTRY, HK_LOAD, HK_GAME_ID. Or, with HK_EXECUTOR,
// into hiraku-firebeat, a release's one binary for every Firebeat game (PLAN.md section 27):
// the game from --game, its entry and load address from games/<game>/game.toml ([program]),
// its functions run by the executor (core/exec.h) from dumps/<game>/recomp.txt.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <memory>
#include <vector>

#include "shell/kvfile.h"
#include "host/notice.h"
#include "../core/exec.h"
#include "ppc/analyze.h"   // the executor's functions, and any build's for code a mod changes
#include "ppc/spec.h"
#include "../core/runtime.h"
#include "../core/sched.h"
#include "../shell/core.h"
#include "../shell/userdir.h"
#include "../spu/spu.h"
#include "board.h"
#include "synth.h"

#ifndef HK_LOAD
#define HK_LOAD 0x80000000u   // where the BIOS puts the program (Keyboardmania 1st: 0x80080000)
#endif
#ifndef HK_ENTRY
#define HK_ENTRY 0x80003000u
#endif
#ifdef HK_EXECUTOR
#define HK_FB_LOAD load_
#define HK_FB_ENTRY entry_
#else
#define HK_FB_LOAD HK_LOAD
#define HK_FB_ENTRY HK_ENTRY
#endif

namespace hk {

namespace {

// `key = 0x...` in a game.toml's [section] (the executor build's entry and load address)
[[maybe_unused]] uint32_t toml_hex(const std::string &path, const std::string &section, const std::string &key, uint32_t dflt) {
    std::ifstream in(path);
    bool in_section = false;
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && line[0] == '[') { in_section = line == "[" + section + "]"; continue; }
        if (!in_section || line.compare(0, key.size(), key) != 0) continue;
        auto eq = line.find('=', key.size());
        if (eq == std::string::npos || line.find_first_not_of(" \t", key.size()) != eq) continue;
        return (uint32_t)std::stoul(line.substr(eq + 1), nullptr, 0);
    }
    return dflt;
}

// `key = [0x..., 0x...]` in a game.toml's [section]: the numbers (the executor build's extra entries)
std::vector<uint32_t> toml_hex_list(const std::string &path, const std::string &section, const std::string &key) {
    std::ifstream in(path);
    bool in_section = false;
    std::vector<uint32_t> out;
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line[0] == '[') { in_section = line.compare(0, section.size() + 2, "[" + section + "]") == 0; continue; }
        if (!in_section || line.compare(0, key.size(), key) != 0) continue;
        auto open = line.find('['), close = line.find(']');
        if (open == std::string::npos || close == std::string::npos) continue;
        std::string list = line.substr(open + 1, close - open - 1);
        for (size_t at = 0; (at = list.find("0x", at)) != std::string::npos; at += 2) out.push_back((uint32_t)std::stoul(list.substr(at), nullptr, 16));
    }
    return out;
}

class FirebeatCore final : public Core {
public:
    bool option(const std::string &a, const std::function<std::string()> &val) override {
        if (a == "--image") image_ = val();
        else if (a == "--spec") spec_ = val();
        else if (a == "--interp-only") { g_interp.call_native = false; g_interp.lle = true; }
        else if (a == "--shadow") g_interp.shadow_checks = std::stoul(val());
        else if (a == "--itrace") g_interp.trace = true;
        else if (a == "--bios-seconds") bios_seconds_ = std::stod(val());
        else if (a == "--soundfont") soundfont_ = val(), synth_stand_in_ = true;
        else if (a == "--synth-stand-in") synth_stand_in_ = true;
        else if (a == "--floppy") floppy_ = val();
        else if (a == "--mame-fdc") mame_fdc_ = true;
        else return false;
        return true;
    }
    std::string options_help() const override {
        return "  --image FILE      program image (default dumps/<game>/program.bin)\n"
#ifdef HK_EXECUTOR
               "  --spec FILE       its functions (default dumps/<game>/recomp.txt, else build/<game>/recomp.txt)\n"
#endif
               "  --interp-only     run everything in the interpreter (a plain emulator; no fibers)\n"
               "  --shadow K        compare native vs interpreted for the first K calls of each function\n"
               "  --itrace          print every interpreted instruction\n"
               "  --bios-seconds S  time the skipped BIOS would have taken (default 21.65, as measured in\n"
               "                    MAME for beatmania III); boards like the SPU boot during it\n"
               "  --floppy FILE     beatmania III: a floppy disk in the cabinet's drive, where the game keeps a player's\n"
               "                    name, options and scores: a 1.44 MB disk image (made, blank and formatted, if it isn't\n"
               "                    there; a file that can't be written is a write-protected disk). \"default\": floppy.img\n"
               "                    in the game's save folder. The Floppy control (Insert) takes the disk out and puts it in\n"
               "  --mame-fdc        ... with the floppy controller's FIFO as MAME has it (the oracle's answers), not as the\n"
               "                    chip's data sheet: the games' reads then fail and are tried again about every second time\n"
               "  --synth-stand-in  Keyboardmania: play the MIDI through FluidSynth even when the XT446 ROMs are there\n"
               "  --soundfont FILE  ... with this SoundFont (implies --synth-stand-in)\n";
    }

    bool init(const Options &o) override {
        if (image_.empty()) image_ = o.root + "/dumps/" + o.game + "/program.bin";
        root_ = o.root, game_ = o.game;
        {   // its size: the program's range in RAM (code_changed)
            std::error_code ec;
            program_bytes_ = (size_t)std::filesystem::file_size(image_, ec);
            if (ec) program_bytes_ = 0;
        }
        auto machine = read_kv(o.root + "/dumps/" + o.game + "/machine.txt");
        if (machine.empty()) {
            fprintf(stderr, "no dumps/%s/machine.txt - run tools/ingest.py on your program disc first\n", o.game.c_str());
            return false;
        }
#ifdef HK_EXECUTOR
        const std::string toml = o.root + "/games/" + o.game + "/game.toml";
        load_ = toml_hex(toml, "program", "load", HK_LOAD);
        entry_ = toml_hex(toml, "program", "entry", HK_ENTRY);
        // its functions: --spec, else a dumps/<game>/recomp.txt (an override), else HIRAKU's own
        // analysis of the program, now (src/ppc/analyze.h: a fifth of a second)
        if (spec_.empty() && std::ifstream(o.root + "/dumps/" + o.game + "/recomp.txt")) spec_ = o.root + "/dumps/" + o.game + "/recomp.txt";
        std::string err;
        if (!spec_.empty()) {
            err = exec::load(spec_, image_);
        } else {
            std::ifstream f(image_, std::ios::binary);
            std::vector<uint8_t> img((std::istreambuf_iterator<char>(f)), {});
            if (img.empty()) err = "cannot read " + image_;
            else {
                analyze::Options ao;
                ao.base = load_, ao.entry = entry_, ao.image_path = image_;
                ao.extra = toml_hex_list(toml, "recomp", "extra_entries");
                analyze::Result r = analyze::run(img, ao);
                err = exec::load_text(r.spec, image_);
            }
        }
        if (!err.empty()) {
            fprintf(stderr, "%s's functions: %s\n", o.game.c_str(), err.c_str());
            return false;
        }
#endif
        ram_init();
        if (!ram_load(image_, HK_PHYS(HK_FB_LOAD))) { fprintf(stderr, "cannot load %s\n", image_.c_str()); return false; }

        fb::Config cfg;
        cfg.game = o.game;
        cfg.family = machine["family"];
        cfg.rom_dir = o.root + "/dumps/" + o.game + "/rom";
        cfg.dongle_file = machine["dongle"];
        if (!machine["cabinet"].empty()) cfg.cabinet = std::stoi(machine["cabinet"]);
        cfg.cd_master = o.data_path(machine["cd_master"]);
        cfg.cd_slave = o.data_path(machine["cd_slave"]);
        cfg.nvram_dir = o.nvram_dir;
        if (floppy_ == "default") {   // the game's own disk, beside its saved memories
            if (o.nvram_dir.empty()) {
                fprintf(stderr, "--floppy default: the disk is kept in the game's save folder, and this run has none (--nvram DIR, or a file: --floppy FILE)\n");
                return false;
            }
            floppy_ = o.nvram_dir + "/floppy.img";
        }
        cfg.floppy = floppy_;
        cfg.mame_fdc = mame_fdc_;
        fb::set_floppy_said([](const std::string &said) { hk::host::notice::hint({said}, 4); });
        if (o.trace) cfg.trace_dir = o.out;
        cfg.ata_log = o.ata_log;
        fb::init(cfg);
        if (cfg.family == "kbm") synth::init(cfg.rom_dir, soundfont_, synth_stand_in_, hk::user_data_dir(o.root));

        // sound: the SPU board when the machine has one, otherwise the board's YMZ280B
        if (!machine["spu_rom"].empty()) {
            spu::Config sc;
            sc.rom = cfg.rom_dir + "/" + machine["spu_rom"];
            sc.hdd = o.data_path(machine["spu_hdd"]);
            sc.dvd = o.data_path(machine["spu_dvd"]);
            sc.mix_effect = cfg.family == "bm3";
            sc.log = o.ata_log;
            if (!spu::init(sc)) {
                fprintf(stderr, "cannot start the SPU (%s)\nthe SPU program comes from the MAME ROM set: tools/ingest.py DISC --romset %s.zip\n",
                        sc.rom.c_str(), o.game.c_str());
                return false;
            }
            if (sink_) spu::set_audio_sink(sink_);
        } else if (sink_) {
            fb::set_audio_sink(sink_);
        }

        // HIRAKU starts in the game, not the BIOS. On the real machine (and in MAME)
        // everything else powered up with the BIOS: the SPU's 68000, for one, tests
        // its 32 MiB of wave RAM meanwhile. Give the machine that time.
        if (bios_seconds_ > 0) {
            sched_idle((uint64_t)(bios_seconds_ * CPU_HZ));
            spu::catch_up();
        }

        memset(&ctx_, 0, sizeof ctx_);
        ctx_.pc = HK_FB_ENTRY;
        glue_init(&ctx_);
        fb::set_trace_ctx(&ctx_);
#ifdef HK_EXECUTOR
        printf("hiraku %s: %zu functions, %s\n", o.game.c_str(), exec::count(),
               g_interp.lle ? "interpreter only" : "executed (hiraku-firebeat)");
#else
        printf("hiraku %s: %u recompiled functions, %s\n", o.game.c_str(), hk_func_count,
               g_interp.lle ? "interpreter only" : "native");
#endif
        return true;
    }

    const std::string &family() const override { return fb::family(); }
    void set_frame_hook(FrameHook h) override { fb::set_frame_hook(std::move(h)); }
    bool set_present_hook(PresentHook h) override { fb::set_present_hook(std::move(h)); return true; }
    void frame_picture_unused() override { fb::frame_picture_unused(); }
    // Before init(): init routes it to the SPU or the board, before the BIOS time runs.
    void set_audio_sink(AudioSink s) override { sink_ = std::move(s); }
    void set_on_fatal(std::function<void()> f) override { g_on_fatal = std::move(f); }

    [[noreturn]] void run() override {
        // Boot as the BIOS leaves the CPU: supervisor state at the image's entry.
        // The boot code is a recompiled function like any other (it never returns);
        // --interp-only runs it, and everything else, in the interpreter.
        if (!g_interp.lle && has_guest(HK_FB_ENTRY)) call_guest(&ctx_, HK_FB_ENTRY);
        else interp_run(&ctx_, 0xffffffff, 0, UINT64_MAX);
        fatal(&ctx_, "the guest stopped running");
    }

    void shutdown() override { fb::shutdown(); }
    void save_now() override { fb::save_now(); }

    void report() override {
        printf("frames %llu, virtual time %.3f s, %llu interpreted insns, %llu native calls, %llu exceptions "
               "(%llu vectors native), %llu threads, %llu thread switches\n",
               (unsigned long long)fb::frame_count(), (double)now() / CPU_HZ, (unsigned long long)g_stats.insns,
               (unsigned long long)g_stats.native_calls, (unsigned long long)g_stats.exceptions,
               (unsigned long long)g_stats.native_vectors, (unsigned long long)g_stats.threads_started + 1, (unsigned long long)g_stats.thread_switches);
        if (g_interp.shadow_checks)
            printf("shadow: %llu identical, %llu MISMATCHED, %llu impure\n", (unsigned long long)g_stats.shadow_ok,
                   (unsigned long long)g_stats.shadow_bad, (unsigned long long)g_stats.shadow_impure);
        if (!spu::present()) printf("ymz280b: %llu key-ons\n", (unsigned long long)fb::ymz_key_ons());
        if (fb::family() == "kbm")
            printf("midi to the synth: %llu bytes (%s)\n", (unsigned long long)fb::midi_out_bytes(), synth::describe().c_str());
        if (spu::present()) {
            auto &st = spu::stats();
            printf("spu: pc %06x sr %04x, %.2f M 68000 cycles, IRQ2 %llu IRQ4 %llu IRQ6 %llu, %llu ATA commands, %.1f MiB DMA, %llu key-ons\n",
                   st.pc, st.sr, st.cycles / 1e6, (unsigned long long)st.irq2, (unsigned long long)st.irq4, (unsigned long long)st.irq6,
                   (unsigned long long)st.ata_commands, st.dma_words * 2 / 1048576.0, (unsigned long long)st.key_ons);
            if (st.lat_n) {
                printf("latency (player key -> RF5C400 key-on, virtual time): n=%llu mean %.2f ms, min %.2f, max %.2f; 2 ms buckets:",
                       (unsigned long long)st.lat_n, st.lat_sum_ms / st.lat_n, st.lat_min_ms, st.lat_max_ms);
                for (int k = 0; k < 16; k++) printf(" %u", st.lat_hist[k]);
                printf("\n");
            }
        }
    }
    int exit_code() const override { return g_stats.shadow_bad ? 1 : 0; }
    void debug_line(uint64_t f) override {
        printf("f%llu pc %08x lr %08x msr %08x r1 %08x | EXISR %08x EXIER %08x TCR %08x TSR %08x | insns %llu exc %llu sw %llu\n",
               (unsigned long long)f, ctx_.pc, ctx_.lr, ctx_.msr, ctx_.r[1], g_cpu.dcrs[0x40], g_cpu.dcrs[0x42],
               g_cpu.sprs[0x3da], g_cpu.sprs[0x3d8], (unsigned long long)g_stats.insns,
               (unsigned long long)g_stats.exceptions, (unsigned long long)g_stats.thread_switches);
    }

    double seconds() const override { return (double)now() / CPU_HZ; }
    double native_rate() const override { return fb::frame_rate(); }
    const uint32_t *render_between(double t, int &w, int &h) override { return fb::render_between(t, w, h); }
    const uint8_t *mem(uint32_t addr, uint32_t len) const override {
        const uint32_t p = HK_PHYS(addr);
        return p < HK_RAM_SIZE && len <= HK_RAM_SIZE - p ? hk_ram + p : nullptr;
    }
    uint64_t frame_count() const override { return fb::frame_count(); }

    bool set_input(const std::string &name, int value) override { return fb::set_input(name, value); }
    void turntable_add(int player, int delta) override { fb::turntable_add(player, delta); }
    // Keyboardmania's XT446, rendered on a worker, adds its part at the frame's end (board.cpp
    // audio_release()); otherwise the sound comes every 2 ms of machine time
    bool audio_per_frame() const override { return synth::deferred(); }
    const std::vector<uint8_t> &lamp_state() const override { return fb::lamp_state(); }
    uint64_t lamp_changes() const override { return fb::lamp_changes(); }
    // a mod's patch (host/mods.h): the program runs translated ahead of time (recompiled, or the
    // executor's steps), so the functions a patch to its code touches run interpreted from their
    // next call on (core/patched.cpp), reading the code as it is now; data patches need nothing
    void code_changed(uint32_t addr, uint32_t len) override {
        const uint32_t p = HK_PHYS(addr), lo = HK_PHYS(load_), hi = lo + (uint32_t)program_bytes_;
        if (!(p < hi && p + len > lo)) return;
        const spec::Program *prog = program();
        if (!prog) return;
        const uint32_t va = p - lo + load_;   // the program's addresses, as its functions have them
        hk::patch_functions(*prog, va, va + len);
    }
    int screens() const override { return fb::screens(); }
    // Keyboardmania: two 4:3 monitors turned upright (MAME ROT270), side by side: 3:2
    double display_aspect() const override { return fb::screens() == 2 ? 1.5 : 4.0 / 3.0; }
    int monitor_khz() const override { return 31; }   // 640x480, progressive
    bool monitor_turned() const override { return fb::screens() == 2; }   // Keyboardmania (board.cpp draw_picture)
    std::vector<std::string> setup_warning() const override {
        if (!fb::sound_flash_blank()) return {};
        // the YMZ280B then plays erased flash: a level stuck at full scale under the rest
        return {"The sound flash is blank: most sounds are missing.",
                "Test mode (F2): SOUND FLASH INIT (the launcher says how)."};
    }

private:
    // the program's functions, for code a mod changes: the executor's own; a recompiled build's
    // spec (build/<id>/recomp.txt, what it was compiled from); else HIRAKU's analysis, as the
    // executor makes it (a fifth of a second), once
    const spec::Program *program() {
        if (const spec::Program *p = exec::program()) return p;
        if (own_ || own_failed_) return own_.get();
        own_ = std::make_unique<spec::Program>();
        std::string err;
#ifdef HK_GAME_ID
        std::ifstream recomp(std::string(HK_ROOT) + "/build/" HK_GAME_ID "/recomp.txt");
        if (recomp) {
            err = spec::parse(*own_, recomp, image_);
        } else
#endif
        {
            std::ifstream f(image_, std::ios::binary);
            std::vector<uint8_t> img((std::istreambuf_iterator<char>(f)), {});
            analyze::Options ao;
            ao.base = load_, ao.entry = entry_, ao.image_path = image_;
            ao.extra = toml_hex_list(root_ + "/games/" + game_ + "/game.toml", "recomp", "extra_entries");
            std::istringstream spec(analyze::run(img, ao).spec);
            err = img.empty() ? "cannot read " + image_ : spec::parse(*own_, spec, image_);
        }
        if (!err.empty()) {
            fprintf(stderr, "mods: the program's functions: %s (code patches left out)\n", err.c_str());
            own_.reset();
            own_failed_ = true;
        }
        return own_.get();
    }

    std::unique_ptr<spec::Program> own_;
    bool own_failed_ = false;
    std::string root_, game_;
    std::string image_, soundfont_, spec_, floppy_;
    uint32_t load_ = HK_LOAD, entry_ = HK_ENTRY;   // the executor build's, from game.toml
    size_t program_bytes_ = 0;   // the program image's size (code_changed)
    bool synth_stand_in_ = false, mame_fdc_ = false;
    double bios_seconds_ = 21.65;
    AudioSink sink_;
    hk_ctx ctx_{};
};

}  // namespace

Core &core() {
    static FirebeatCore c;
    return c;
}

}  // namespace hk
