// SPDX-License-Identifier: GPL-3.0-or-later
// The Twinkle core (runtime/shell/core.h): the board in runtime/twinkle/board.cpp. The machine
// comes from dumps/<game>/machine.txt (tools/ingest.py): the ROM files of MAME's set (BIOS, SPU
// program, dongle), the program CD, the Video CD and the SPU board's hard disk.
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>
#include <string>

#include "shell/kvfile.h"
#include "shell/sys.h"
#include "shell/core.h"
#include "sys573/mips.h"
#include "twinkle/board.h"

namespace hk {
namespace {

class TwinkleCore final : public Core {
public:
    bool option(const std::string &name, const std::function<std::string()> &value) override {
        if (name == "--rtc") { rtc_ = std::stoll(value()); return true; }
        if (name == "--mame-mix") { mame_mix_ = true; return true; }
        if (name == "--fine-sound") { fine_sound_ = true; return true; }   // the default now
        if (name == "--mame-sound") { fine_sound_ = false; return true; }
        if (name == "--mame-cd") { mame_cd_ = true; return true; }
        if (name == "--session-host") { session_host_ = value(); return true; }
        if (name == "--session-join") { session_join_ = value(); return true; }
        if (name == "--session-quantum") { session_quantum_ = std::stoi(value()); return true; }
        if (name == "--video") {
            const std::string v = value();
            if (v != "on" && v != "off") { fprintf(stderr, "--video: on or off\n"); exit(2); }
            video_ = v == "on";
            return true;
        }
        return false;
    }
    std::string options_help() const override {
        return "  --rtc SECONDS     the RTC's time at power-on (default: now), seconds since 1970\n"
               "  --mame-mix        sound at MAME's level (it can clip on loud stretches; default: 0.8 of it)\n"
               "  --mame-sound      hand the sound to the host at MAME's 50 Hz stream updates, as MAME does\n"
               "                    (default: every millisecond, lower and steadier latency; regress uses this)\n"               "                    them (MAME's RF5C400 starts them at its previous update, up to 20 ms early)\n"
               "  --video on|off    the background video from the Video CD (default on; MAME shows none)\n"
               "  --mame-cd         MAME's program CD drive: no read time, every byte at the SCSI bus's pace\n"
               "                    (default: the XM-5401's 4x, a sector at a time as the drive reads it)\n"
               "  --session-host SOCKET  the club kits (bmiidxc, bmiidxca, bmiidxc2): this IIDX cabinet waits at\n"
               "                    SOCKET (a path, or host:port) for the DDR cabinet's process (ddr2mc, ddr2mc2,\n"
               "                    with --session-join), joined by a serial cable; the DDR cabinet plays this\n"
               "                    one's music too\n"
               "  --session-join SOCKET  the same, joining a DDR cabinet that hosts (--session-host)\n"
               "  --session-quantum US   the lockstep quantum in microseconds (default 500; both sides alike)\n";
    }

    bool init(const Options &o) override {
        auto machine = read_kv(o.root + "/dumps/" + o.game + "/machine.txt");
        if (machine.empty() || machine["core"] != "twinkle") {
            fprintf(stderr, "no Twinkle dumps/%s/machine.txt - run tools/ingest.py with the set and its CHDs first\n",
                    o.game.c_str());
            return false;
        }
        twinkle::Config cfg;
        cfg.game = o.game;
        cfg.family = machine["family"];
        cfg.rom_dir = o.root + "/dumps/" + o.game + "/rom";
        auto get = [&](const char *k, std::string &dst) { if (!machine[k].empty()) dst = machine[k]; };
        get("bios", cfg.bios);
        get("spu", cfg.spu);
        get("dongle", cfg.dongle);
        get("dongle_type", cfg.dongle_type);
        get("dvd_port", cfg.dvd_port);
        cfg.cd = o.data_path(machine["cd"]);
        if (!machine["video_cd"].empty()) cfg.video_cd = o.data_path(machine["video_cd"]);
        if (!machine["video_dvd"].empty()) cfg.video_dvd = o.data_path(machine["video_dvd"]);
        cfg.hdd = o.data_path(machine["hdd"]);
        unsigned c = 0, h = 0, s = 0;
        if (sscanf(machine["hdd_chs"].c_str(), "%u,%u,%u", &c, &h, &s) != 3) {
            fprintf(stderr, "dumps/%s/machine.txt has no hdd_chs (run tools/ingest.py again)\n", o.game.c_str());
            return false;
        }
        cfg.hdd_cylinders = c;
        cfg.hdd_heads = h;
        cfg.hdd_sectors = s;
        cfg.nvram_dir = o.nvram_dir;
        cfg.rtc_time = rtc_ >= 0 ? rtc_ : (int64_t)time(nullptr);
        if (o.trace) cfg.trace_dir = o.out;
        if (mame_mix_) cfg.mix_gain = 1.0f;
        cfg.fine_sound = fine_sound_;
        if (mame_cd_) cfg.cd_rate = 0;
        cfg.video = video_;
        cfg.session_host = session_host_;
        cfg.session_join = session_join_;
        cfg.session_quantum_us = session_quantum_;
        if (!twinkle::init(cfg)) return false;
        printf("hiraku %s: Twinkle, MAME's PS1 (its recompiled blocks) and 68000\n", o.game.c_str());
        // its disks into the system's cache now (a song's load read cold from the drive, or a CHD's
        // compressed pieces, froze a cabinet's frames 50-80 ms): up to 6 GB, the raw disk images
        hk::sys::warm_files({cfg.hdd, cfg.cd, cfg.video_cd, cfg.video_dvd}, 6ull << 30);
        return true;
    }

    const std::string &family() const override { return twinkle::family(); }
    // beatmania IIDX's cabinet has a widescreen CRT: the game's 4:3-sized frames fill a 16:9 tube,
    // so its pixels are wider than tall
    double display_aspect() const override { return 16.0 / 9.0; }
    void set_frame_hook(FrameHook h) override { twinkle::set_frame_hook(std::move(h)); }
    bool set_present_hook(PresentHook h) override { twinkle::set_present_hook(std::move(h)); return true; }
    void set_audio_sink(AudioSink s) override { twinkle::set_audio_sink(std::move(s)); }
    void set_on_fatal(std::function<void()> f) override { on_fatal_ = std::move(f); }
    [[noreturn]] void run() override { twinkle::run(); }
    void shutdown() override { twinkle::save_nvram(); }
    void save_now() override { twinkle::save_nvram_now(); }
    void report() override {
        printf("frames %llu, virtual time %.3f s, pc %08x, 68000 pc %06x, %llu lamp changes, display [%s]\n",
               (unsigned long long)twinkle::frame_count(), twinkle::seconds(), twinkle::pc(), twinkle::audio_pc(),
               (unsigned long long)twinkle::lamp_changes(), twinkle::display_text().c_str());
        if (std::string s = twinkle::sound_report(); !s.empty()) printf("%s\n", s.c_str());
        if (std::string s = twinkle::video_report(); !s.empty()) printf("%s\n", s.c_str());
        if (std::string s = k573::mips::report(); !s.empty()) printf("%s\n", s.c_str());
    }
    void debug_line(uint64_t f) override {
        printf("f%llu pc %08x 68000 %06x\n", (unsigned long long)f, twinkle::pc(), twinkle::audio_pc());
    }

    double seconds() const override { return twinkle::seconds(); }
    const uint32_t *render_between(double t, int &w, int &h) override { return twinkle::render_between(t, w, h); }
    const uint8_t *mem(uint32_t addr, uint32_t len) const override { return twinkle::mem(addr, len); }
    uint64_t frame_count() const override { return twinkle::frame_count(); }
    bool set_input(const std::string &name, int value) override { return twinkle::set_input(name, value); }
    int script_button_delay() const override { return 1; }   // MAME latches digital inputs a frame late
    void turntable_add(int player, int delta) override { twinkle::turntable_add(player, delta); }
    std::string display_text() const override { return twinkle::display_text(); }
    const std::vector<uint8_t> &lamp_state() const override { return twinkle::lamp_state(); }
    uint64_t lamp_changes() const override { return twinkle::lamp_changes(); }

private:
    std::function<void()> on_fatal_;
    long long rtc_ = -1;
    bool mame_mix_ = false;
    bool fine_sound_ = true;
    bool mame_cd_ = false;
    bool video_ = true;
    std::string session_host_, session_join_;
    int session_quantum_ = 500;
};

}  // namespace

Core &core() {
    static TwinkleCore c;
    return c;
}

}  // namespace hk
