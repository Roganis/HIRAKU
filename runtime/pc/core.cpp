// SPDX-License-Identifier: GPL-3.0-or-later
// The PC games' core (PLAN.md section 30): pop'n music 15 on, Konami's own Windows programs, run as
// they are (on Linux under Wine, on Windows directly) as a machine for HIRAKU's shell
// (runtime/main.cpp: hiraku-pc --game ID). HIRAKU's ezusb.dll and libacio.dll (runtime/pc/) take
// the place of Konami's in a copy of the program folder, and this core is the cabinet they talk to
// through a shared file (runtime/pc/link.h): the controls in, the lamps and the game's pictures out.
// The pictures come in lockstep: the game draws one and waits; the shell shows it in HIRAKU's window
// (scale, full screen, the CRT pictures, the beam simulator) and lets the next one start when its
// pacing says so, as for an emulated machine: the game reads its controls as the shell's frame
// starts and its picture goes to the next refresh. The game's own window stays hidden. Its sound
// is mixed by HIRAKU's DirectSound in the DLL (runtime/pc/dsound.c), 735 frames (44.1 kHz) with
// each picture, and played through HIRAKU's sound output like an emulated machine's.
//
// Options of its own: --wine PROGRAM (Linux: the Wine to run it with, default `wine` from the PATH),
// --game-ahead (the game draws its next picture while the shell shows this one: a frame's more
// time to draw, a frame later on the screen), and e-AMUSEMENT's, as spice2x's (shell/eaoptions.h): -url, -card0 / -card1. Without -url the
// e-AMUSEMENT server is HIRAKU's own (runtime/python/easerver.h, pop'n 9-14's), on this machine,
// with the pop'n games' shared folder (one card for all); the DLL leads the game's network library
// to it (runtime/pc/net.c). HK_PC_EA_LOG=1 prints each call in clear.
//
// dumps/<ID>/machine.txt (tools/ingest.py) says where the game is:
//   core=pc, family=pcpopn
//   exe=popn15.exe          the program
//   program=PATH            its folder (prog/<date>), copied into the save folder to run
//   data=PATH               the game's data, read where it is
//   drive=D:\popn15         where the game looks for its folders (program: <drive>\prog\<date>)
//   save_drive=E:\          its backup drive, a folder in the save folder
//   save_drive2=F:\         a second backup drive (beatmania IIDX 9th style), another folder there
//   data_as=..\data         the name the game gives its data folder (default <drive>\data)
//   board=c02               the C02 cabinet's board (runtime/pc/ezusb_c02.c, IIDX: family=iidx), not pop'n's
//   board=c02-driver        the same board under its driver (runtime/pc/ezusb_dev.c, IIDX RED: PLAN.md
//                           section 36), started from HIRAKU's RtEffect.dll; Konami's ezusb.dll kept
//   security=MODEL A:B:C    its security plug: the game's model code and the boot's seeds
//   sidcode=E11:J:A:D       the cabinet's software id, written as ..\sidcode.txt from the program
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <ctime>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// (before windows.h: its near and far macros)
#include "hiraku/scroll_guess.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#else
#include <csignal>
#include <dirent.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

#include "host/input.h"
#include "host/macro.h"
#include "host/mods.h"
#include "host/pacing.h"
#include "host/popnio.h"
#include "host/script.h"
#include "pc/link.h"
#include "python/easerver.h"
#include "python/lobby.h"
#include "python/netrelay.h"
#include "shell/cabinet.h"
#include "shell/core.h"
#include "shell/eaoptions.h"
#include "shell/net.h"
#include "shell/kvfile.h"
#include "shell/sys.h"
#include "shell/userdir.h"

namespace {

using hk::sys::copy_file;
inline void mkdirs(const std::string &path) { hk::sys::make_dirs(path); }

// The files (not folders) in a folder
std::vector<std::string> list_files(const std::string &dir) {
    std::vector<std::string> out;
#ifdef _WIN32
    WIN32_FIND_DATAA d;
    HANDLE h = FindFirstFileA((dir + "/*").c_str(), &d);
    if (h == INVALID_HANDLE_VALUE) return out;
    do
        if (!(d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) out.push_back(d.cFileName);
    while (FindNextFileA(h, &d));
    FindClose(h);
#else
    if (DIR *d = opendir(dir.c_str())) {
        while (dirent *e = readdir(d)) {
            struct stat st;
            if (stat((dir + "/" + e->d_name).c_str(), &st) == 0 && S_ISREG(st.st_mode)) out.push_back(e->d_name);
        }
        closedir(d);
    }
#endif
    return out;
}

uint64_t file_size(const std::string &p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    return f ? (uint64_t)f.tellg() : UINT64_MAX;
}

// MD5 (RFC 1321), of a file: the games' lists of their files (dat.bin) hold their files' MD5s
std::string md5_file(const std::string &path) {
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af,
        0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
        0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
        0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97,
        0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
        0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const int R[16] = {7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21};
    uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    auto block = [&](const uint8_t *b) {
        uint32_t m[16], a = h[0], bb = h[1], c = h[2], d = h[3];
        for (int i = 0; i < 16; i++) m[i] = (uint32_t)b[4 * i] | (uint32_t)b[4 * i + 1] << 8 | (uint32_t)b[4 * i + 2] << 16 | (uint32_t)b[4 * i + 3] << 24;
        for (int i = 0; i < 64; i++) {
            uint32_t f;
            int g;
            if (i < 16) f = (bb & c) | (~bb & d), g = i;
            else if (i < 32) f = (d & bb) | (~d & c), g = (5 * i + 1) % 16;
            else if (i < 48) f = bb ^ c ^ d, g = (3 * i + 5) % 16;
            else f = c ^ (bb | ~d), g = 7 * i % 16;
            const uint32_t t = d, x = a + f + K[i] + m[g];
            const int r = R[i / 16 * 4 + i % 4];
            d = c, c = bb, bb = bb + (x << r | x >> (32 - r)), a = t;
        }
        h[0] += a, h[1] += bb, h[2] += c, h[3] += d;
    };
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::vector<uint8_t> buf(1 << 16);
    uint64_t total = 0;
    size_t have = 0;
    for (;;) {
        f.read((char *)buf.data() + have, (std::streamsize)(buf.size() - have));
        const size_t got = (size_t)f.gcount();
        total += got;
        have += got;
        const size_t whole = have / 64 * 64;
        for (size_t i = 0; i < whole; i += 64) block(&buf[i]);
        memmove(buf.data(), buf.data() + whole, have - whole);
        have -= whole;
        if (!got) break;
    }
    uint8_t tail[128] = {};
    memcpy(tail, buf.data(), have);
    tail[have] = 0x80;
    const size_t len = have < 56 ? 64 : 128;
    for (int i = 0; i < 8; i++) tail[len - 8 + i] = (uint8_t)((total * 8) >> (8 * i));
    for (size_t i = 0; i < len; i += 64) block(tail + i);
    std::string out(16, '\0');
    for (int i = 0; i < 16; i++) out[i] = (char)(h[i / 4] >> (8 * (i % 4)));
    return out;
}

// The DLLs a Windows program file imports (its import table's names), and whether it is 64-bit;
// false when it isn't a PE file
[[maybe_unused]] bool pe_imports(const std::string &path, std::vector<std::string> &names, bool &pe64) {
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto u16 = [&](size_t o) { return o + 2 <= d.size() ? (uint32_t)(d[o] | d[o + 1] << 8) : 0u; };
    auto u32 = [&](size_t o) { return o + 4 <= d.size() ? (uint32_t)(u16(o) | u16(o + 2) << 16) : 0u; };
    if (u16(0) != 0x5a4d) return false;
    const size_t nt = u32(0x3c), opt = nt + 24;
    if (u32(nt) != 0x4550) return false;
    pe64 = u16(opt) == 0x20b;
    const size_t dirs = opt + (pe64 ? 112 : 96), sections = opt + u16(nt + 20);
    const uint32_t imports = u32(dirs + 8), count = u16(nt + 6);
    auto offset = [&](uint32_t rva) -> size_t {   // a virtual address's place in the file
        for (uint32_t i = 0; i < count; i++) {
            const size_t s = sections + 40 * i;
            const uint32_t va = u32(s + 12), size = std::max(u32(s + 8), u32(s + 16));
            if (rva >= va && rva < va + size) return rva - va + u32(s + 20);
        }
        return 0;
    };
    if (!imports) return true;
    for (size_t e = offset(imports); e && e + 20 <= d.size() && u32(e + 12); e += 20) {
        size_t n = offset(u32(e + 12));
        std::string name;
        while (n && n < d.size() && d[n]) name += (char)d[n++];
        if (!name.empty()) names.push_back(name);
    }
    return true;
}

// Windows' name for a path of this machine: Wine's Z: is the root
std::string win_path(std::string p) {
#ifndef _WIN32
    p = "Z:" + p;
#endif
    for (char &c : p)
        if (c == '/') c = '\\';
    return p;
}

void set_env(const char *k, const std::string &v) {
#ifdef _WIN32
    SetEnvironmentVariableA(k, v.c_str());
#else
    setenv(k, v.c_str(), 1);
#endif
}

class PcCore : public hk::Core {
public:
    bool option(const std::string &name, const std::function<std::string()> &value) override {
        if (name == "--wine") {
            wine_ = value();
            return true;
        }
        if (name == "--game-ahead") {
            ahead_ = true;
            return true;
        }
        if (name == "--single-screen") {
            crt_ = true;
            return true;
        }
        if (name == "--market") {
            market_ = value();
            for (char &c : market_) c = (char)std::toupper((unsigned char)c);
            if (market_ != "J" && market_ != "A" && market_ != "K" && market_ != "U") {
                fprintf(stderr, "--market: J (Japan), A (Asia), K (Korea) or U (the USA), not '%s'\n", market_.c_str());
                exit(2);
            }
            return true;
        }
        if (name == "--press-time") {
            press_time_ = true;
            return true;
        }
        if (name == "--skip-tutorial") {
            skip_tutorial_ = true;
            return true;
        }
        if (name == "--judge-offset") {
            const std::string v = value();
            char *end = nullptr;
            judge_offset_ = strtod(v.c_str(), &end);
            if (v.empty() || *end || judge_offset_ < -200 || judge_offset_ > 200) {
                fprintf(stderr, "--judge-offset: milliseconds, -200 to 200 (not '%s')\n", v.c_str());
                exit(2);
            }
            return true;
        }
        std::string error;
        if (ea_.option(name, value, &error)) {
            if (!error.empty()) {
                fprintf(stderr, "%s\n", error.c_str());
                exit(2);
            }
            return true;
        }
        return false;
    }
    std::string options_help() const override {
        return std::string("  --wine PROGRAM       Linux: the Wine that runs the game (default: wine, from the PATH)\n"
                           "  --game-ahead         the game draws its next picture while this one is shown: a frame\n"
                           "                       later from the controls to the screen, for a PC too slow otherwise\n"
                           "  --single-screen      the 64-bit PC games on one 4:3 screen with nine buttons (an older\n"
                           "                       cabinet's): the main screen's middle at 640x480, no touch screen\n"
                           "  --market J|A|K|U     the 64-bit PC games: the cabinet's market (Japan, Asia, Korea, the\n"
                           "                       USA) in place of the game's prop/ea3-config.xml's\n"
                           "  --press-time         pop'n 15-18: each press judged by when it happened, not by the frame\n"
                           "                       the game read it in (as the game does: up to a frame later)\n"
                           "  --judge-offset MS    pop'n 15-18: each press judged MS milliseconds earlier (positive:\n"
                           "                       for a display or sound that comes late), -200 to 200\n"
                           "  --skip-tutorial      the 32-bit PC pop'n on AVS2 2.15: its guidance and long-note\n"
                           "                       tutorials skipped, a guest's and a new player's\n") +
               hk::EaOptions::help();
    }

    bool init(const Options &o) override {
        game_ = o.game;
        window_ = o.window;
        const std::string dumps = o.root + "/dumps/" + game_;
        auto m = hk::read_kv(dumps + "/machine.txt");
        auto path_of = [&](const std::string &key) {
            const std::string p = m[key];
            return p.empty() || p[0] == '/' || (p.size() > 1 && p[1] == ':') ? p : dumps + "/" + p;
        };
        exe_ = m["exe"];
        program_ = path_of("program");
        if (m["loader"] == "pc64") return init_pc64(o, m, path_of("contents"));
        if (m["loader"] == "pc32") return init_pc32(o, m, path_of("contents"));
        const std::string data = path_of("data");
        const std::string drive = m.count("drive") ? m["drive"] : "D:\\" + game_;
        const std::string save_drive = m.count("save_drive") ? m["save_drive"] : "E:\\";
        if (m["core"] != "pc" || exe_.empty() || program_.empty() || data.empty()) {
            fprintf(stderr, "hiraku-pc: %s/machine.txt doesn't describe a PC game (core=pc, exe, program, data)\n", dumps.c_str());
            return false;
        }
        if (m.count("family")) family_ = m["family"];
        c02_ = m["board"] == "c02" || m["board"] == "c02-driver";
        c02_driver_ = m["board"] == "c02-driver";
        data_as_ = m.count("data_as") ? m["data_as"] : drive + "\\data";

        // The save folder: the program's copy (HIRAKU's DLLs for Konami's), the backup drive (e/)
        save_ = o.nvram_dir.empty() ? hk::user_data_dir(o.root) + "/" + game_ : o.nvram_dir;
        const std::string prog_name = program_.substr(program_.find_last_of("/\\") + 1);
        run_prog_ = save_ + "/prog/" + prog_name;
        run_e_ = save_ + "/e";
        mkdirs(run_prog_);
        mkdirs(run_e_);
        if (m.count("save_drive2")) mkdirs(save_ + "/f");
        if (!copy_program(program_, run_prog_)) return false;
        if (c02_driver_ && !set_up_driver_board(m)) return false;
        if (!link_data(data)) return false;
        if (!hand_mods(drive, data, prog_name)) return false;
        std::string error;
        ea_.read_card_files(save_, &error);
        if (!error.empty()) fprintf(stderr, "%s\n", error.c_str());
        if (!start_eamuse(o.root)) return false;
        if (c02_) {
            // the C02 cabinet's e-AMUSEMENT (PLAN.md section 33): its domain, which its library
            // asks "services." of (runtime/pc/net.c), and the board's PCB id (-p's, else HIRAKU's)
            set_env("HK_PC_EA_DOMAIN", "eamuse.konami.fun");
            set_env("HK_PC_PCBID", ea_.pcbid.empty() ? "01201000000000C02001" : ea_.pcbid);
            set_env("HK_PC_CARD1", win_path(save_ + "/card.bin"));   // the magnetic cards (ezusb_c02.c)
            set_env("HK_PC_CARD2", win_path(save_ + "/card2.bin"));
        }
        // a bare pop'n board's firmware: the game's own ezusb.dll, unless --popn-io-dll gave one
        if (!c02_) hk::host::popnio_default_dll(program_ + "/ezusb.dll");

        if (!make_link()) return false;

        // What the DLLs are told (runtime/pc/cabinet.h)
        set_env("HK_PC_MAP", data_as_ + "=" + win_path(data) + ";" + drive + "\\prog=" + win_path(save_ + "/prog") + ";" +
                                 save_drive + "=" + win_path(run_e_) + "\\" +
                                 (m.count("save_drive2") ? ";" + m["save_drive2"] + "=" + win_path(save_ + "/f") + "\\" : ""));
        set_env("HK_PC_HZ", "0");   // the shell paces
        set_env("HK_PC_PRESS_TIME", press_time_ ? "1" : "");
        set_env("HK_PC_JUDGE_OFFSET", judge_offset_ ? std::to_string(judge_offset_) : "");
        set_env("HK_PC_LOG", win_path(save_ + "/pc.log"));
        { std::ofstream(save_ + "/pc.log", std::ios::trunc); }
        printf("hiraku-pc: %s, %s from %s; saves in %s\n", game_.c_str(), exe_.c_str(), program_.c_str(), save_.c_str());
        fflush(stdout);
        return true;
    }

    // The program's folder copied into the save folder (FROM to TO), HIRAKU's DLLs in place of
    // Konami's board and reader: the game's files once, HIRAKU's each time (a new HIRAKU brings new
    // ones)
    static std::string bin_dir() { return hk::sys::exe_path().substr(0, hk::sys::exe_path().rfind('/')); }
    bool copy_program(const std::string &from_dir, const std::string &to_dir) {
        for (const std::string &f : list_files(from_dir)) {
            std::string lower = f;
            for (char &c : lower) c = (char)std::tolower((unsigned char)c);
            std::string from = from_dir + "/" + f;
            if (lower == "rteffect.dll" && c02_driver_) continue;   // HIRAKU's, below
            if ((lower == "ezusb.dll" && !c02_driver_) || lower == "libacio.dll")
                from = bin_dir() + "/pc/" + (c02_ && lower == "ezusb.dll" ? "c02/" : "") + lower;
            if (!copy_in(from, to_dir + "/" + f)) return false;
        }
        return true;
    }

    // The C02 board under its driver (IIDX RED): HIRAKU's RtEffect.dll in the program's copy (the
    // game imports it: HIRAKU's way in, ezusb_dev.c and rteffect.c), the plug's code for it, and the
    // cabinet's software id beside the program folders, which the cabinet's install writes (without it:
    // HDD DATA ERROR (SID))
    bool set_up_driver_board(std::map<std::string, std::string> &m) {
        // GOLD on imports no RtEffect.dll: the board starts from HIRAKU's libacio.dll instead
        bool rteffect = false;
        for (const std::string &f : list_files(program_)) {
            std::string lower = f;
            for (char &c : lower) c = (char)std::tolower((unsigned char)c);
            rteffect = rteffect || lower == "rteffect.dll";
        }
        if (rteffect && !copy_in(bin_dir() + "/pc/c02/RtEffect.dll", run_prog_ + "/RtEffect.dll")) return false;
        // 10th style checks its folder's files against bm2dx.sdx (else HDD DATA ERROR (PROG CHKSUM)):
        // Konami's RtEffect.dll is kept beside HIRAKU's, and what the game reads as a file (cabinet.c)
        if (rteffect) {
            std::string konami;
            for (const std::string &f : list_files(program_)) {
                std::string lower = f;
                for (char &c : lower) c = (char)std::tolower((unsigned char)c);
                if (lower == "rteffect.dll") konami = program_ + "/" + f;
            }
            if (!copy_in(konami, run_prog_ + "/RtEffect.konami")) return false;
            set_env("HK_PC_RTEFFECT_FILE", win_path(run_prog_ + "/RtEffect.konami"));
        }
        set_env("HK_PC_BOARD_IN_ACIO", rteffect ? "" : "1");
        set_env("HK_PC_SECURITY", m["security"]);
        set_env("HK_PC_BOARD", m["io"] == "fx2" ? "fx2" : "d01");   // io=fx2: the later IO2 board (DJ TROOPERS on)
        set_env("HK_PC_GPU_ID", "1002:7146");   // the cabinets' graphics card (EMPRESS checks it: cabinet.c)
        if (!m["sidcode"].empty()) {
            std::ofstream(save_ + "/prog/sidcode.txt", std::ios::binary | std::ios::trunc) << m["sidcode"] << "\n";
        }
        return true;
    }
    bool copy_in(const std::string &from, const std::string &to) {
        if (file_size(from) == UINT64_MAX) {
            fprintf(stderr, "hiraku-pc: no %s (HIRAKU's PC DLLs belong in a pc folder beside hiraku-pc)\n", from.c_str());
            return false;
        }
        if (file_size(to) != file_size(from) || from.rfind(bin_dir(), 0) == 0)
            if (!copy_file(from, to)) {
                fprintf(stderr, "hiraku-pc: can't copy %s to %s\n", from.c_str(), to.c_str());
                return false;
            }
        return true;
    }

    // The game's data where the game names it from its program folder (data_as=..\data, IIDX 9th
    // style), beside the program's copy: HIRAKU's path map (HK_PC_MAP) sees only the game's own
    // modules, and Windows' DirectShow opens the movies itself
    bool link_data(const std::string &data) {
        data_dir_ = data;
        if (data_as_.rfind("..\\", 0) != 0) return true;
        return link_folder(save_ + "/prog/" + data_as_.substr(3), data, data_as_);
    }
    // A folder AT that is the folder TO (made again each time: TO may have moved). Linux: a symbolic
    // link; Windows: a junction (no rights needed)
    static bool link_folder(const std::string &at, const std::string &to, const std::string &what) {
#ifdef _WIN32
        make_junction(at, to);
#else
        std::error_code ec;
        if (std::filesystem::is_symlink(at, ec)) std::filesystem::remove(at, ec);
        std::filesystem::create_directory_symlink(to, at, ec);
#endif
        if (!hk::sys::is_dir(at)) {
            fprintf(stderr, "hiraku-pc: can't make %s, the game's data (%s) where it looks for it\n", at.c_str(), what.c_str());
            return false;
        }
        return true;
    }

#ifdef _WIN32
    // A junction at `at` to the folder `to` (made again each time: the data may have moved): the
    // folder made, then a mount point set on it (no rights needed, unlike a symbolic link). Never
    // over a real folder
    static std::wstring wide(const std::string &utf8) {
        std::string p = utf8;
        for (char &c : p)
            if (c == '/') c = '\\';
        const int n = MultiByteToWideChar(CP_UTF8, 0, p.c_str(), (int)p.size(), nullptr, 0);
        std::wstring w((size_t)n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, p.c_str(), (int)p.size(), w.data(), n);
        return w;
    }
    static void make_junction(const std::string &at, const std::string &to) {
        const std::wstring a = wide(at), t = wide(to);
        const DWORD attr = GetFileAttributesW(a.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES) {
            if (!(attr & FILE_ATTRIBUTE_REPARSE_POINT)) return;   // a folder of the player's own
            RemoveDirectoryW(a.c_str());                          // the junction, not what it leads to
        }
        if (!CreateDirectoryW(a.c_str(), nullptr)) return;
        wchar_t full[MAX_PATH];
        if (!GetFullPathNameW(t.c_str(), MAX_PATH, full, nullptr)) return;
        const std::wstring print = full, subst = L"\\??\\" + print;
        struct {
            DWORD tag;
            WORD data_length, reserved, subst_offset, subst_length, print_offset, print_length;
            wchar_t path[2 * MAX_PATH + 8];
        } rp{};
        rp.tag = IO_REPARSE_TAG_MOUNT_POINT;
        rp.subst_length = (WORD)(subst.size() * 2);
        rp.print_offset = (WORD)(rp.subst_length + 2);
        rp.print_length = (WORD)(print.size() * 2);
        memcpy(rp.path, subst.c_str(), rp.subst_length);
        memcpy((char *)rp.path + rp.print_offset, print.c_str(), rp.print_length);
        rp.data_length = (WORD)(8 + rp.print_offset + rp.print_length + 2);
        HANDLE h = CreateFileW(a.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        DWORD got = 0;
        const bool ok = h != INVALID_HANDLE_VALUE && DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, &rp, 8 + rp.data_length, nullptr, 0, &got, nullptr);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        if (!ok) RemoveDirectoryW(a.c_str());
    }
#endif

    // The link: a file both map. Linux: in /dev/shm (memory); Windows: in the save folder
    bool make_link() {
#ifdef _WIN32
        link_path_ = save_ + "/link.bin";
        HANDLE lf = CreateFileA(link_path_.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
        HANDLE lm = lf != INVALID_HANDLE_VALUE ? CreateFileMappingA(lf, nullptr, PAGE_READWRITE, 0, HK_PC_LINK_SIZE, nullptr) : nullptr;
        link_ = lm ? (hk_pc_link *)MapViewOfFile(lm, FILE_MAP_ALL_ACCESS, 0, 0, HK_PC_LINK_SIZE) : nullptr;
#else
        link_path_ = "/dev/shm/hiraku-pc-" + std::to_string(getpid());
        const int lfd = open(link_path_.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
        if (lfd >= 0 && ftruncate(lfd, HK_PC_LINK_SIZE) == 0) {
            void *p = mmap(nullptr, HK_PC_LINK_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, lfd, 0);
            link_ = p == MAP_FAILED ? nullptr : (hk_pc_link *)p;
        }
        if (lfd >= 0) close(lfd);
#endif
        if (!link_) {
            fprintf(stderr, "hiraku-pc: can't make the link file %s\n", link_path_.c_str());
            return false;
        }
        memset((void *)link_, 0, sizeof *link_);
        link_->host_shows = 1;
        if (ea_.card[0].size() == 16)   // player 1's card, for a card key the game's DLL sees itself
            for (int i = 0; i < 8; i++) link_->card_uid[i] = (uint8_t)std::stoul(ea_.card[0].substr(2 * i, 2), nullptr, 16);
        if (ea_.card[1].size() == 16)   // and player 2's (-card1), for a second reader
            for (int i = 0; i < 8; i++) link_->card_uid2[i] = (uint8_t)std::stoul(ea_.card[1].substr(2 * i, 2), nullptr, 16);
        // the words the mods' [[rate]]s watch: the DLL copies them in before each picture (mem())
        const std::vector<uint32_t> words = hk::mods::rate_words();
        if (words.size() > HK_PC_WATCHES) fprintf(stderr, "mods: %zu words for [[rate]]s; a PC game watches %u\n", words.size(), HK_PC_WATCHES);
        for (size_t i = 0; i < words.size() && i < HK_PC_WATCHES; i++) link_->watch_addr[i] = words[i];
        link_->game_hz_milli = (uint32_t)std::lround(ghz() * 1000);
        link_->version = HK_PC_LINK_VERSION;
        link_->magic = HK_PC_LINK_MAGIC;
        black_.assign(640 * 480, 0);
        set_env("HK_PC_LINK", win_path(link_path_));
        return true;
    }

    // The 64-bit PC pop'n (PLAN.md section 32): hiraku-pc64.exe (runtime/pc64/) starts Konami's AVS2
    // and the game's DLL from the game's folder (contents=PATH, only read) and runs it in the save
    // folder's run/; the same link. machine.txt: core=pc, loader=pc64, contents=PATH, family=pcpopnhc
    bool init_pc64(const Options &o, std::map<std::string, std::string> &m, const std::string &contents) {
        pc64_ = true;
        contents_ = contents;
        if (contents_.empty() || !hk::sys::is_dir(contents_ + "/modules")) {
            fprintf(stderr, "hiraku-pc: %s isn't the game's folder (modules/, prop/, its data)\n", contents_.c_str());
            return false;
        }
        family_ = m.count("family") ? m["family"] : "pcpopnhc";
        // the controllers go on while the touch screen's window has the focus (a click there takes it
        // under Wine, whatever the window asks): SDL's joystick events in the background too
        set_env("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
        if (crt_) set_env("HK_PC64_OUTPUT", "crt");   // the loader's single-screen mode (runtime/pc64/crt.c)
        set_env("HK_PC64_MARKET", market_);           // (runtime/pc64/main.c)
        save_ = o.nvram_dir.empty() ? hk::user_data_dir(o.root) + "/" + game_ : o.nvram_dir;
        mkdirs(save_ + "/run");
        const std::string bin_dir = hk::sys::exe_path().substr(0, hk::sys::exe_path().rfind('/'));
        loader_ = bin_dir + "/pc/hiraku-pc64.exe";
        if (file_size(loader_) == UINT64_MAX) {
            fprintf(stderr, "hiraku-pc: no %s\n", loader_.c_str());
            return false;
        }
        std::string error;
        ea_.read_card_files(save_, &error);
        if (!error.empty()) fprintf(stderr, "%s\n", error.c_str());
        if (!start_eamuse(o.root)) return false;
        if (!make_link()) return false;
        set_env("HK_PC64_SAVE", win_path(save_ + "/run"));
        set_env("HK_PC64_LOG", win_path(save_ + "/pc.log"));
        // --single-screen: the save folder's single-screen/ (the game's files rearranged for one 4:3
        // screen, made from the player's own) read before the game's (the loader's HK_PC64_OVERLAY)
        if (crt_ && hk::sys::is_dir(save_ + "/single-screen")) {
            set_env("HK_PC64_OVERLAY", win_path(save_ + "/single-screen"));
            printf("hiraku-pc: the single-screen layout from %s/single-screen\n", save_.c_str());
        }
        // the mods (docs/MODS.md): each folder the game's data folder's files laid over it, new ones
        // too (a file at <mod>/a/b is the game's data/a/b), and the songs its music data describes
        // (the loader's HK_PC64_DATA: runtime/pc64/files.c, songs.c)
        std::string data_mods;
        const std::vector<std::string> mods = hk::mods::folders();
        for (const std::string &d : mods) data_mods += (data_mods.empty() ? "" : ";") + win_path(d);
        if (!data_mods.empty()) {
            set_env("HK_PC64_DATA", data_mods);
            printf("hiraku-pc: %zu mod%s over the game's data\n", mods.size(), mods.size() == 1 ? "" : "s");
        }
        // their memory patches ([[patch]], with module = "popn.dll" at an offset in it): the loader
        // puts them in (runtime/pc64/patches.c), a line each: "WHERE ONCE ORIGINAL REPLACE"
        patches_ = hk::mods::memory_patches();
        std::string plist;
        for (const auto &p : patches_) {
            char a[96];
            snprintf(a, sizeof a, "%s%s%08x %d ", p.module.c_str(), p.module.empty() ? "" : "+", p.address, p.once ? 1 : 0);
            std::string o, r;
            char x[3];
            for (uint8_t v : p.original) snprintf(x, sizeof x, "%02x", v), o += x;
            for (uint8_t v : p.replace) snprintf(x, sizeof x, "%02x", v), r += x;
            plist += a + (o.empty() ? std::string("-") : o) + " " + r + "\n";
        }
        if (patches_.size() > 256) {
            fprintf(stderr, "mods: %zu memory patches; a PC game takes 256\n", patches_.size());
            return false;
        }
        std::ofstream(save_ + "/mods-patches.txt", std::ios::trunc) << plist;
        set_env("HK_PC_PATCHES", plist.empty() ? "" : win_path(save_ + "/mods-patches.txt"));
        if (!plist.empty()) printf("mods: %zu memory patches go to the game\n", patches_.size());
        { std::ofstream(save_ + "/pc.log", std::ios::trunc); }
        printf("hiraku-pc: %s from %s; saves in %s\n", game_.c_str(), contents_.c_str(), save_.c_str());
        fflush(stdout);
        return true;
    }

    // The last 32-bit PC pop'n (PLAN.md section 35): hiraku-pc32.exe (runtime/pc32/) starts Konami's
    // AVS2 and popn22.dll. The save folder's run/ is AVS2's root, as the cabinet's contents folder:
    // modules/ copied with HIRAKU's DLLs in place of Konami's board and reader and the loader beside
    // them (the DLLs take the modules in the program's folder as the game's: runtime/pc/cabinet.c),
    // prop/ copied (the game's settings start from its defaults there), data/ the game's (a link),
    // dev/ the cabinet's NVRAM. machine.txt: core=pc, loader=pc32, contents=PATH, family=pcpopn
    bool init_pc32(const Options &o, std::map<std::string, std::string> &m, const std::string &contents) {
        pc32_ = true;
        contents_ = contents;
        if (contents_.empty() || !hk::sys::is_dir(contents_ + "/modules") || !hk::sys::is_dir(contents_ + "/data")) {
            fprintf(stderr, "hiraku-pc: %s isn't the game's folder (modules/, prop/, data/)\n", contents_.c_str());
            return false;
        }
        if (m.count("family")) family_ = m["family"];
        exe_ = "hiraku-pc32.exe";
        program_ = contents_ + "/modules";
        save_ = o.nvram_dir.empty() ? hk::user_data_dir(o.root) + "/" + game_ : o.nvram_dir;
        const std::string run = save_ + "/run";
        run_prog_ = run + "/modules";
        mkdirs(run_prog_);
        mkdirs(run + "/prop/defaults");
        if (!copy_program(program_, run_prog_) || !copy_in(bin_dir() + "/pc/" + exe_, run_prog_ + "/" + exe_)) return false;
        for (const char *dir : {"prop", "prop/defaults"})
            for (const std::string &f : list_files(contents_ + "/" + dir))
                if (!copy_in(contents_ + "/" + dir + "/" + f, run + "/" + dir + "/" + f)) return false;
        if (!link_folder(run + "/data", contents_ + "/data", "data")) return false;
        // DXVK's 32-bit Direct3D 9 when it is in bin/pc/dxvk32/ (Wine's own draws the game's pictures
        // after its start-up checks magnified into a corner, as the 64-bit game's: PLAN.md section 32)
        const std::string dxvk = bin_dir() + "/pc/dxvk32/d3d9.dll";
        dxvk32_ = file_size(dxvk) != UINT64_MAX && copy_in(dxvk, run_prog_ + "/d3d9.dll");
        std::string error;
        ea_.read_card_files(save_, &error);
        if (!error.empty()) fprintf(stderr, "%s\n", error.c_str());
        if (!start_eamuse(o.root)) return false;
        hk::host::popnio_default_dll(program_ + "/ezusb.dll");
        if (!make_link()) return false;
        set_env("HK_PC_HZ", "0");   // the shell paces
        set_env("HK_PC_PRESS_TIME", press_time_ ? "1" : "");
        set_env("HK_PC_JUDGE_OFFSET", judge_offset_ ? std::to_string(judge_offset_) : "");
        set_env("HK_PC_LOG", win_path(save_ + "/pc.log"));
        set_env("HK_PC32_LOG", win_path(save_ + "/pc.log"));
        set_env("HK_PC32_SKIP_TUTORIAL", skip_tutorial_ ? "1" : "");
        set_env("HK_PC_PROGRAM", m.count("exe") ? m["exe"] : "popn22.dll");   // the game's own code: the DLL, not the loader
        // the mods (docs/MODS.md): each folder laid over the game's data folder, new files too, and
        // inside its packs (<pack>_ifs/<member>), as ifs_layeredfs (the loader's HK_PC32_DATA: files.c)
        std::string data_mods;
        const std::vector<std::string> mods = hk::mods::folders();
        for (const std::string &d : mods) data_mods += (data_mods.empty() ? "" : ";") + win_path(d);
        set_env("HK_PC32_DATA", data_mods);
        if (!mods.empty()) printf("hiraku-pc: %zu mod%s over the game's data\n", mods.size(), mods.size() == 1 ? "" : "s");
        { std::ofstream(save_ + "/pc.log", std::ios::trunc); }
        printf("hiraku-pc: %s from %s; saves in %s\n", game_.c_str(), contents_.c_str(), save_.c_str());
        fflush(stdout);
        return true;
    }

    const std::string &family() const override { return family_; }
    void set_frame_hook(FrameHook h) override { frame_hook_ = std::move(h); }
    void set_audio_sink(AudioSink s) override { audio_sink_ = std::move(s); }
    int audio_rate() const override { return HK_PC_AUDIO_RATE; }
    bool audio_per_frame() const override { return true; }   // each picture brings its frame's sound
    void set_on_fatal(std::function<void()> f) override { on_fatal_ = std::move(f); }
    double native_rate() const override { return pc64_ ? 120.0 : HZ; }   // the cabinet's (a [[rate]]'s ticks)
    int monitor_khz() const override { return 31; }   // a PC's VGA monitor
    // the 64-bit PC pop'n: 1920x1080, square pixels; IIDX's 640x480 shown on its cabinet's widescreen
    // monitor (as Twinkle's IIDX)
    double display_aspect() const override { return pc64_ ? 0.0 : family_ == "iidx" ? 16.0 / 9.0 : 4.0 / 3.0; }
    double seconds() const override { return sec0_ + (double)(tick_ - t0_) / ghz(); }
    // The game's memory is its own process's: only the words a mod's [[rate]] watches, as the DLL
    // last copied them (link.h, v9)
    const uint8_t *mem(uint32_t addr, uint32_t len) const override {
        if (!link_ || len > 4) return nullptr;
        for (uint32_t i = 0; i < HK_PC_WATCHES; i++)
            if (link_->watch_addr[i] == addr && addr) {
                watch_copy_[i] = link_->watch_val[i];
                return (const uint8_t *)&watch_copy_[i];
            }
        return nullptr;
    }
    // A mod's [[rate]] (docs/MODS.md): the game draws hz pictures a second from the next shown frame
    // (0: the cabinet's 60 again). Its clock, its sound's samples and the shell's time go on
    // without a jump; the DLL counts the cabinet's ticks for the patches tied to the rate
    bool frame_repeated() const override { return repeating_; }
    bool set_vsync_rate(double hz) override {
        if (pc64_) return false;
        want_hz_ = hz > 0 ? hz : HZ;
        return true;
    }
    uint64_t frame_count() const override { return frame_; }

    // In-between pictures for a display faster than the game (--smooth-motion, docs/SMOOTH.md). The
    // PC IIDX games count everything in frames, so their notes can't be drawn more often by the game
    // itself: the game's own Direct3D draws its last frame again (runtime/pc/d3d8.c), with what goes
    // with the scroll (the notes, the bar lines) moved on along the scroll's steady line, and nothing
    // else (the movie, the gauge, the effects as the game drew them). The quads of the frame are paired
    // with the frame before's (the same texture and size, hiraku/scroll_guess.h votes the scroll).
    // Only while the game waits for its next frame (not with --game-ahead).
    const uint32_t *render_between(double t, int &w, int &h) override {
        if (!link_ || ahead_ || pc64_ || pc32_) return nullptr;
        if (!link_->smooth) {   // the DLL records from the next frame on
            link_->smooth = 1;
            return nullptr;
        }
        // the frame shown: the last picture counted (once its batch is drawn the game has gone on,
        // and may count the next while this one's refreshes are shown)
        const uint32_t seq = shown_seq_;
        if (seq != batch_seq_ && (link_->quads_seq != seq || link_->frame_go == seq)) return nullptr;
        if (seq != between_seq_) {   // a new frame: its quads paired with the frame before's
            const bool follows = seq == between_seq_ + 1;
            between_seq_ = seq;
            prev_quads_.swap(quads_);
            if (!follows) prev_quads_.clear();
            const auto *q = (const hk_pc_quad *)((const char *)link_ + HK_PC_QUAD_LIST);
            quads_.assign(q, q + std::min<uint32_t>((uint32_t)link_->quads_n, HK_PC_QUADS));
            // Only what can be a note: the IIDX games' fall straight down, so a quad is paired with a
            // look-alike at the same x above it (a menu's hundreds of letters and tiles of one texture,
            // each paired with all the others, made a million candidates: 20 ms a frame), or with
            // itself where it was: what stands still doesn't vote (a screen of text paired each letter
            // with the same letter a line up, all of it "falling" 24 pixels a frame)
            std::vector<hk::ScrollGuess::Candidate> cands;
            for (int i = 0; i < (int)quads_.size() && cands.size() < 20000; i++)
                for (int j = 0; j < (int)prev_quads_.size(); j++) {
                    const hk_pc_quad &c = quads_[(size_t)i], &p = prev_quads_[(size_t)j];
                    const int dy = c.y - p.y;
                    if (c.tex != p.tex || c.x != p.x || c.w != p.w || c.h != p.h || dy < 0 || dy > hk::ScrollGuess::REACH) continue;
                    cands.push_back({i, j, 0, dy, 0, true});
                }
            const std::vector<int> paired = scroll_.pair(seq, (int)quads_.size(), cands, true);
            std::vector<int> from(quads_.size(), -1);
            prev_falls_.swap(falls_);
            prev_moves_.swap(moves_);
            moves_.assign(quads_.size(), {0, 0});
            falls_.assign(quads_.size(), 0);
            for (size_t i = 0; i < quads_.size(); i++)
                if (paired[i] >= 0) {
                    const auto &c = cands[(size_t)paired[i]];
                    moves_[i] = {c.dx, c.dy}, from[i] = c.from;
                    // falling as it fell the frame before (a step 2 or 3 pixels: within one): a note. A
                    // blinking line of text paired with the same letters a line above moved once only
                    const size_t f = (size_t)c.from;
                    const bool same = f < prev_falls_.size() && std::abs(prev_moves_[f].second - c.dy) <= 1;
                    falls_[i] = same ? prev_falls_[f] + 1 : 1;
                }
            scroll_.frame(seq, moves_, from);
            // play under way: something has fallen steadily for half a second without a break (the
            // notes, at least the bar lines). A result screen's digits counting up fell a few frames
            bool falling = false;
            for (const int f : falls_) falling |= f >= 3;
            falling_for_ = falling && follows ? falling_for_ + 1 : 0;
            // The lanes: where a bar line (wide, a pixel or two tall) has fallen in the last 5
            // seconds; only what falls inside them is moved (a panel's rolling text below them fell at
            // the notes' speed too)
            for (size_t i = 0; i < quads_.size(); i++) {
                const hk_pc_quad &q = quads_[i];
                if (falls_[i] < 3 || q.h > 3 || q.w < 60) continue;
                bool known = false;
                for (Lane &l : lanes_)
                    if (std::abs(l.x0 - q.x) <= 2 && std::abs(l.x1 - (q.x + q.w)) <= 2) l.seen = seq, known = true;
                if (!known) lanes_.push_back({q.x, q.x + q.w, seq});
            }
            std::erase_if(lanes_, [&](const Lane &l) { return seq - l.seen > 300; });
        }
        // All the frame's refreshes' pictures at once, while the game waits for its next frame (its
        // go stays where display sync puts it, just in time: given early, its controls were read a
        // frame sooner); the window then asks for each by its moment (pacing::phase).
        if (batch_seq_ != seq) {
            batch_seq_ = seq;
            batch_.clear();
            const int k = std::max(1, hk::host::pacing::refreshes_per_frame());
            std::vector<double> ts;
            for (int i = 0; i < k; i++) ts.push_back(hk::host::pacing::phase(i));
            if (std::none_of(ts.begin(), ts.end(), [&](double x) { return std::fabs(x - t) < 1e-9; })) ts.push_back(t);
            for (const double bt : ts) {
                Between b{bt, {}};
                draw_between(bt, b.pixels);
                batch_.push_back(std::move(b));
            }
        }
        const Between *best = nullptr;
        for (const Between &b : batch_)
            if (!best || std::fabs(b.t - t) < std::fabs(best->t - t)) best = &b;
        if (!best || best->pixels.empty()) return nullptr;
        w = (int)link_->frame_w, h = (int)link_->frame_h;
        return best->pixels.data();
    }
    // One in-between picture, `t` of a frame on, drawn by the game (empty: nothing moved a whole
    // pixel, or it couldn't)
    void draw_between(double t, std::vector<uint32_t> &out) {
        out.clear();
        auto *move = (int16_t(*)[2])((char *)link_ + HK_PC_QUAD_MOVE);
        bool any = false;
        for (size_t i = 0; i < quads_.size(); i++) {
            int ox = 0, oy = 0;
            if (falling_for_ >= 30 && falls_[i] >= 3 && in_lanes(quads_[i]) && scroll_.scrolls(moves_[i].first, moves_[i].second))
                scroll_.offset(moves_[i].first, moves_[i].second, t, ox, oy, (int)i);
            move[i][0] = (int16_t)ox, move[i][1] = (int16_t)oy;
            any |= ox || oy;
        }
        if (!any) return;
        std::atomic_thread_fence(std::memory_order_release);
        const uint32_t req = link_->between_req + 1;
        link_->between_req = req;
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
        while (link_->between_done != req) {
            if (std::chrono::steady_clock::now() > until) {
                between_late_++;
                return;
            }
            std::this_thread::yield();
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        if (!link_->between_ok) return;
        const auto *px = (const uint32_t *)((const char *)link_ + HK_PC_BETWEEN);
        out.assign(px, px + (size_t)link_->frame_w * link_->frame_h);
        between_drawn_++;
    }
    struct Between {
        double t;
        std::vector<uint32_t> pixels;
    };
    std::vector<Between> batch_;
    uint32_t batch_seq_ = ~0u, shown_seq_ = ~0u;   // shown_seq_: the picture being shown
    hk::ScrollGuess scroll_;
    uint32_t between_seq_ = ~0u;
    std::vector<hk_pc_quad> quads_, prev_quads_;
    std::vector<std::pair<int, int>> moves_, prev_moves_;
    std::vector<int> falls_, prev_falls_;   // the frames each quad has fallen steadily
    int falling_for_ = 0;                    // the frames something has
    struct Lane { int x0, x1; uint32_t seen; };
    std::vector<Lane> lanes_;
    // a quad a note could be: no wider than the lanes, no taller than a few notes, no narrower than a
    // key's (a result screen's falling sparks, 6 pixels, came after a stage), inside them
    bool in_lanes(const hk_pc_quad &q) const {
        if (q.w < 12 || q.w > 160 || q.h > 64) return false;
        for (const Lane &l : lanes_)
            if (q.x >= l.x0 - 2 && q.x + q.w <= l.x1 + 2) return true;
        return false;
    }
    uint64_t between_drawn_ = 0, between_late_ = 0;
    // The C02 cabinet's turntables: the host's counts (1024 a turn) to the board's 0-255
    void turntable_add(int player, int delta) override {
        if (!c02_ || !link_ || player < 0 || player > 1) return;
        tt_frac_[player] += delta;
        const int whole = tt_frac_[player] / 4;
        tt_frac_[player] -= whole * 4;
        link_->turntable[player] = (uint8_t)(link_->turntable[player] + whole);
    }
    // The C02 cabinet's 16-segment ticker
    std::string display_text() const override {
        if (!c02_ || !link_) return {};
        std::string t((const char *)link_->ticker, 9);
        t.erase(std::find(t.begin(), t.end(), '\0'), t.end());
        return t;
    }
    void report() override {
        printf("hiraku-pc: %llu frames shown, %llu of them again while the game drew none\n", (unsigned long long)frame_,
               (unsigned long long)repeats_);
        if (made_up_ || not_made_up_)
            printf("hiraku-pc: the game late for %llu frames and drew them at once after (its notes kept with its music)%s\n",
                   (unsigned long long)made_up_,
                   not_made_up_ ? (", " + std::to_string(not_made_up_) + " more in pauses (as they came)").c_str() : "");
        if (between_drawn_ || between_late_)   // --smooth-motion (render_between)
            printf("hiraku-pc: smooth motion: %llu in-between pictures drawn by the game, %llu asked for and not answered\n",
                   (unsigned long long)between_drawn_, (unsigned long long)between_late_);
        // Where a frame's time goes, from the game's start (frame_go) to its picture in the shell's
        // hands; the shell's "video:" line goes on from its frame start (here: the same moment,
        // unless --game-ahead) to the display
        auto stat = [](std::vector<float> v) {
            if (v.empty()) return std::string("-");
            std::sort(v.begin(), v.end());
            double sum = 0;
            for (float x : v) sum += x;
            char b[64];
            snprintf(b, sizeof b, "mean %.2f ms, p99 %.2f", sum / (double)v.size(), (double)v[v.size() * 99 / 100]);
            return std::string(b);
        };
        if (!go_to_shell_.empty())
            printf("hiraku-pc: the game's frame (%s), from its start: its controls read %s; its picture drawn and read back %s; "
                   "in the shell's hands %s\n",
                   ahead_ ? "drawn ahead" : "started with the shell's", stat(pad_read_).c_str(), stat(drawn_).c_str(),
                   stat(go_to_shell_).c_str());
        if (!key_to_sound_.empty() && getenv("HK_PC_KEYLOG"))   // rough: the next sound the game starts after a press
            printf("hiraku-pc: a button down to its sound handed to the sound output (%zu presses): %s\n", key_to_sound_.size(),
                   stat(key_to_sound_).c_str());
    }

    // The game runs; each picture it draws goes to the shell, which shows it and paces. While it
    // draws none (starting, loading), the last is shown again a 60th of a second on, so the window
    // keeps answering.
    [[noreturn]] void run() override {
        if (!start_game()) fatal();
        // A new save folder: the game makes its backup files and asks to be switched off and on
        // (BACKUP ERROR 5-1502); that is done for it, once, when they've been there a few seconds
        bool first_boot = !c02_ && !pc32_ && list_files(run_e_ + "/settings").empty();
        auto backup_made = std::chrono::steady_clock::time_point{};
        auto last = std::chrono::steady_clock::now(), rate_from = last;
        uint32_t seen = link_->frame_seq, rate_pictures = seen;
        uint64_t rate_repeats = 0;
        for (;;) {
            if (!game_running()) {
                printf("hiraku-pc: the game ended%s\n", exit_note().c_str());
                fatal();
            }
            if (first_boot && !list_files(run_e_ + "/settings").empty()) {
                const auto now = std::chrono::steady_clock::now();
                if (backup_made == std::chrono::steady_clock::time_point{}) backup_made = now;
                else if (now - backup_made > std::chrono::seconds(5)) {
                    printf("hiraku-pc: the game has set up its backup: starting it again, as it asks\n");
                    fflush(stdout);
                    first_boot = false;
                    stop_game();
                    link_->frame_seq = 0;
                    link_->frame_go = 0;
                    link_->sound_on = 0;   // the game's sound starts again
                    link_->audio_frame_end = 0;
                    sound_on_ = false;
                    seen = 0;
                    pics_ = 0;
                    owed_ = 0;
                    if (!start_game()) fatal();
                }
            }
            const uint32_t seq = link_->frame_seq;
            const auto now = std::chrono::steady_clock::now();
            if (seq != seen) {
                std::atomic_thread_fence(std::memory_order_acquire);
                seen = seq;
                shown_seq_ = seq;
                // a picture for a frame already gone by, shown again without it (the game was late):
                // not shown, and the game's next frame started at once. It times its notes by its
                // frames, its music by its sound (the shell's, a frame's for every frame shown): a
                // frame never made up leaves the notes a frame behind the music, for the song
                if (owed_ > 0) {
                    owed_--;
                    made_up_++;
                    go(seq);
                    last = std::chrono::steady_clock::now();
                    continue;
                }
                if (++pics_ < per_frame_) {   // a picture between two shown (a 120 Hz game, a slower display): the next at once
                    go(seq);
                    last = std::chrono::steady_clock::now();
                    continue;
                }
                pics_ = 0;
                const int w = (int)link_->frame_w, h = (int)link_->frame_h;
                const bool ok = w > 0 && h > 0 && (uint32_t)w <= HK_PC_MAX_W && (uint32_t)h <= HK_PC_MAX_H;
                if (ok) picture_.assign(link_pixels(), link_pixels() + (size_t)w * h);
                timed(now);
                if (ahead_) go(seq);   // the next picture drawn while this one is shown, for the frame after
                if (ok) show(picture_.data(), w, h);
                else show(nullptr, 0, 0);
                // Without display sync the sound paces, as for the emulated machines; until the game's
                // sound starts, and with no window at all, the game's own rate on a steady grid
                if (!hk::host::pacing::active() && (!link_->audio_made || !window_)) {
                    const auto period = std::chrono::nanoseconds((long long)(1e9 * per_frame_ / ghz()));
                    const auto t = std::chrono::steady_clock::now();
                    due_ = due_ == std::chrono::steady_clock::time_point{} || t - due_ > std::chrono::milliseconds(100) ? t : due_ + period;
                    if (due_ > t) std::this_thread::sleep_until(due_);
                }
                per_frame_ = pictures_per_frame();
                // the shell's next frame starts: the game's too (it reads its controls, draws)
                if (!ahead_) go(seq);
                last = std::chrono::steady_clock::now();
            } else if (now - last > std::chrono::microseconds((long long)(1e6 * per_frame_ / ghz()))) {
                // no picture (starting, loading): the last again
                repeating_ = true;
                show(last_rgb_ ? last_rgb_ : black_.data(), last_w_, last_h_);
                repeating_ = false;
                repeats_++;
                // once the game's sound has started: owed (above), as long as it's a moment's
                // hold-up; a longer pause (loading) is the game's own, its frames as they come
                if (sound_on_ && (owed_ += per_frame_) > MAX_OWED) not_made_up_ += owed_, owed_ = 0;
                last = now;
            } else if (sound_on_ && !ahead_ && now - go_at_ > std::chrono::microseconds((long long)(5e5 * per_frame_ / ghz())) &&
                       (int32_t)(sound_for(tick_ + per_frame_) - link_->audio_made) > 0) {
                // the picture is late (half a frame): its frame's sound goes on without it, before
                // the sound output runs dry (the game's sounds of this frame come with the next)
                sound_until(sound_for(tick_ + per_frame_));
                take_sound();
            } else if (!take_sound()) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            if (!patches_.empty()) patches_said();
            if (getenv("HK_PC_RATE") && now - rate_from > std::chrono::seconds(10)) {   // how the game keeps up
                const double sec = std::chrono::duration<double>(now - rate_from).count();
                fprintf(stderr, "hiraku-pc: the game drew %.2f pictures a second, %llu shown again\n", (seq - rate_pictures) / sec,
                        (unsigned long long)(repeats_ - rate_repeats));
                rate_from = now, rate_pictures = seq, rate_repeats = repeats_;
            }
        }
    }

    void shutdown() override {
        stop_game();
#ifndef _WIN32
        if (!link_path_.empty()) unlink(link_path_.c_str());
#endif
    }

    bool set_input(const std::string &name, int value) override {
        if (!link_) return false;
        auto bit = [&](volatile uint32_t &w, uint32_t b) {
            if (value) w = w | b;
            else w = w & ~b;
        };
        if (c02_) return c02_input(name, value, bit);
        if (name.rfind("P1 Button ", 0) == 0) {
            const int k = std::atoi(name.c_str() + 10);
            if (k < 1 || k > 9) return false;
            if (value && !(link_->pad & HK_PC_PAD_BUTTON(k))) pressed_at_ = std::chrono::steady_clock::now();
            const uint32_t was = link_->pad;
            bit(link_->pad, HK_PC_PAD_BUTTON(k));
            if (link_->pad != was) {   // the change, with when it happened (link.h: pad_ev)
                const uint32_t n = link_->pad_ev_seq;
                link_->pad_ev[n % HK_PC_PAD_EVENTS].us = host_us(hk::host::input_time());
                link_->pad_ev[n % HK_PC_PAD_EVENTS].pad = link_->pad;
                std::atomic_thread_fence(std::memory_order_release);
                link_->pad_ev_seq = n + 1;
            }
        } else if (name == "P1 Big Red") {
            bit(link_->pad, HK_PC_PAD_RED);
        } else if (name == "P1 Big Blue") {
            bit(link_->pad, HK_PC_PAD_BLUE);
        } else if (name == "Test") {
            bit(link_->pad, HK_PC_PAD_TEST);
        } else if (name == "Service") {
            bit(link_->pad, HK_PC_PAD_SERVICE);
        } else if (name == "Coin 1") {
            if (value) link_->coins = link_->coins + 1;
        } else if (name == "Card") {
            if (value) present_card(ea_.card[0]);
        } else if (name == "Keypad 00" || name == "Keypad Enter") {
            bit(link_->keypad, HK_PC_KEY_00);
        } else if (name == "Keypad .") {
            bit(link_->keypad, HK_PC_KEY_DECIMAL);
        } else if (name.rfind("Keypad ", 0) == 0 && name.size() == 8 && name[7] >= '0' && name[7] <= '9') {
            bit(link_->keypad, 1u << (name[7] - '0'));
        } else {
            return false;
        }
        return true;
    }

    // A real reader's card: held to the game's reader
    // A real reader's card: player 1's, or player 2's (a second reader: the IIDX games with two,
    // the link's card_uid2 and card_seq2)
    bool card_read(int player, const std::string &number) override {
        if (player == 0) {
            present_card(number);
            return true;
        }
        if (player != 1 || number.size() != 16) return false;
        for (int i = 0; i < 8; i++) link_->card_uid2[i] = (uint8_t)std::stoul(number.substr(2 * i, 2), nullptr, 16);
        link_->card_seq2 = link_->card_seq2 + 1;
        printf("e-AMUSEMENT: card %s held to player 2's reader\n", number.c_str());
        return true;
    }

    // usbLamp's bits in the family's lamp order (shell/cabinet.cpp): the buttons' bits 23-31, the
    // top (HI-LAMP 1-5) bits 0-4, the sides (LEFT 1-2, RIGHT 1-2) bits 8-11, as spice2x has them.
    // The C02 cabinet's: the keys in usbLamp's bits 0-13 (player 1's first), the starts, VEFX and
    // Effect in usbLed's bits 0-3, the spotlights in the top unit's lamps, its neon
    const std::vector<uint8_t> &lamp_state() const override {
        const auto &l = hk::cab::lamps(family_);
        lamps_.resize(l.size());
        const uint32_t b = link_ ? link_->lamps : 0;
        if (c02_) {
            const uint32_t led = link_ ? link_->led : 0, top = link_ ? link_->top_lamp : 0, neon = link_ ? link_->top_neon : 0;
            const uint32_t all = b | led << 16 | top << 20 | neon << 28;
            for (size_t i = 0; i < l.size(); i++) {
                const std::string &n = l[i].name;
                const int k = std::atoi(n.c_str() + n.find_last_of(' ') + 1);
                int v = 0;
                if (n.rfind("P1 Key Lamp", 0) == 0) v = b >> (k - 1) & 1;
                else if (n.rfind("P2 Key Lamp", 0) == 0) v = b >> (6 + k) & 1;
                else if (n == "P1 Start Lamp") v = led & 1;
                else if (n == "P2 Start Lamp") v = led >> 1 & 1;
                else if (n == "VEFX Lamp") v = led >> 2 & 1;
                else if (n == "Effect Lamp") v = led >> 3 & 1;
                else if (n.rfind("Spotlight", 0) == 0) v = top >> (k - 1) & 1;
                else if (n == "Neon") v = neon != 0;
                lamps_[i] = (uint8_t)v;
            }
            if (all != last_lamps_) last_lamps_ = all, changes_++;
            return lamps_;
        }
        for (size_t i = 0; i < l.size(); i++) {
            const std::string &n = l[i].name;
            const int k = std::atoi(n.c_str() + n.find_last_of(' ') + 1);
            int at = -1;
            if (n.rfind("Button Lamp", 0) == 0) at = 22 + k;
            else if (n.rfind("Top Lamp", 0) == 0) at = k - 1;
            else if (n.rfind("Side Lamp", 0) == 0) at = 7 + k;
            lamps_[i] = at >= 0 && (b >> at & 1);
        }
        if (b != last_lamps_) last_lamps_ = b, changes_++;
        return lamps_;
    }
    uint64_t lamp_changes() const override {
        lamp_state();
        return changes_;
    }

private:
    static constexpr double HZ = 60.0;   // the cabinet's VGA monitor

    // The C02 cabinet's controls (the family iidx's names, shell/cabinet.cpp) to its pad (link.h)
    template <class Bit> bool c02_input(const std::string &name, int value, Bit &bit) {
        int k = 0;
        if ((name.rfind("P1 Button ", 0) == 0 || name.rfind("P2 Button ", 0) == 0) && (k = std::atoi(name.c_str() + 10)) >= 1 && k <= 7)
            bit(link_->pad, HK_PC_C02_KEY(name[1] - '0', k));
        else if (name == "1 Player Start") bit(link_->pad, HK_PC_C02_START(1));
        else if (name == "2 Players Start") bit(link_->pad, HK_PC_C02_START(2));
        else if (name == "VEFX") bit(link_->pad, HK_PC_C02_VEFX);
        else if (name == "Effect") bit(link_->pad, HK_PC_C02_EFFECT);
        else if (name == "Test") bit(link_->pad, HK_PC_C02_TEST);
        else if (name == "Service") bit(link_->pad, HK_PC_C02_SERVICE);
        else if (name == "Coin 1") {
            if (value) link_->coins = link_->coins + 1;
        } else if (name == "Card") {   // player 1's card held to the slot
            if (value) link_->card_seq = link_->card_seq + 1;
        } else if (name == "P2 Card") {
            if (value) link_->card_seq2 = link_->card_seq2 + 1;
        } else if (name.rfind("Keypad ", 0) == 0 || name.rfind("P2 Keypad ", 0) == 0) {   // the 10-keys
            const bool two = name[0] == 'P';
            volatile uint32_t &pad = two ? link_->keypad2 : link_->keypad;
            const std::string k = name.substr(two ? 10 : 7);
            if (k.size() == 1 && k[0] >= '0' && k[0] <= '9') bit(pad, 1u << (k[0] - '0'));
            else if (k == "Enter" || k == "00") bit(pad, HK_PC_KEY_00);
            else if (k == ".") bit(pad, HK_PC_KEY_DECIMAL);
            else if (k == "Clear") bit(pad, HK_PC_KEY_CLEAR);
            else return false;
        } else if (name == "P1 Turntable" || name == "P2 Turntable") {   // a position (scripts' `set`)
            link_->turntable[name[1] - '1'] = (uint8_t)value;
        } else if (name.rfind("Volume ", 0) == 0 && (k = std::atoi(name.c_str() + 7)) >= 1 && k <= 5)
            link_->slider[k - 1] = (uint8_t)std::clamp(value, 0, 15);
        else
            return false;
        return true;
    }

    // e-AMUSEMENT: -url's server, or HIRAKU's own on this machine; the DLL is told where (HK_PC_EA)
    bool start_eamuse(const std::string &root) {
        hk::net::start();
        if (ea_.online()) {   // host:port[/path] or http://host:port[/path]: its IPv4 address
            std::string url = ea_.http_url().substr(ea_.http_url().find("://") + 3);
            url = url.substr(0, url.find('/'));
            const std::string host = url.substr(0, url.find(':'));
            const std::string port = url.find(':') == std::string::npos ? "80" : url.substr(url.find(':') + 1);
            addrinfo hints{}, *res = nullptr;
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) {
                fprintf(stderr, "e-AMUSEMENT: can't find %s\n", host.c_str());
                return false;
            }
            char ip[INET_ADDRSTRLEN] = "";
            const sockaddr_in *in = (const sockaddr_in *)res->ai_addr;
            inet_ntop(AF_INET, &in->sin_addr, ip, sizeof ip);
            set_env("HK_PC_EA", std::string(ip) + ":" + std::to_string(ntohs(in->sin_port)));
            // NET TAISEN: hiraku-eamuse's relay, at its lobby's port (runtime/pc/vsock.c)
            set_env("HK_PC_RELAY", std::string(ip) + ":" + std::to_string(hk::py::Lobby::PORT));
            freeaddrinfo(res);
            printf("e-AMUSEMENT: online, %s\n", ea_.http_url().c_str());
            return true;
        }
        // the server's files: the pop'n games' shared folder beside the saves (in the saves' default
        // place), so a card's player carries from one to the next; with --nvram elsewhere, its own
        const std::string saves = hk::user_data_dir(root);
        // (a game with mods keeps its saves beside the clean game's, <game>+<mod>: its players are the
        // network's, the same)
        const std::string mine = saves + "/" + game_;
        const std::string dir = save_ == mine || save_.rfind(mine + "+", 0) == 0 ? saves + "/eamuse" : save_ + "/eamuse";
        hk::sys::make_dir(dir);
        server_ = std::make_unique<hk::py::EamuseServer>(dir);
        server_->now = [] { return (int64_t)time(nullptr) + 9 * 3600; };   // the cabinet's clock: Japan's
        if (getenv("HK_PC_EA_LOG")) server_->log = [](const std::string &s) { fprintf(stderr, "%s\n", s.c_str()); };
        server_->on_call = [](const std::string &call) { hk::script::event("ea:" + call); };   // for scripts' `on` lines
        // -autopin: the PIN screen comes 4 seconds after the card is read at the most, and wants Enter
        if (ea_.autopin)
            server_->on_card_pin = [](const std::string &pin) { hk::macro::post("PIN", hk::EaOptions::pin_steps(pin, 240, true)); };
        listen_ = hk::net::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t len = sizeof sa;
        if (listen_ < 0 || bind(listen_, (sockaddr *)&sa, sizeof sa) != 0 || listen(listen_, 16) != 0 ||
            getsockname(listen_, (sockaddr *)&sa, &len) != 0) {
            fprintf(stderr, "e-AMUSEMENT: no server: %s\n", hk::net::error().c_str());
            return false;
        }
        set_env("HK_PC_EA", "127.0.0.1:" + std::to_string(ntohs(sa.sin_port)));
        // NET TAISEN's lobby and relay, this cabinet's own (with nobody else there, the game waits
        // for others, then plays the credit in CHALLENGE mode); HK_PC_RELAY=HOST:PORT another's
        // (hiraku-eamuse's, port 5740: cabinets with servers of their own meeting there, as tests do)
        if (const char *r = getenv("HK_PC_RELAY"); r && *r) printf("NET TAISEN: the relay at %s\n", r);
        else if (relay_.start("127.0.0.1", 0)) set_env("HK_PC_RELAY", "127.0.0.1:" + std::to_string(relay_.port()));
        printf("e-AMUSEMENT: HIRAKU's server, data in %s\n", dir.c_str());
        std::thread([this] { serve(); }).detach();
        return true;
    }

    // One call per connection (HTTP/1.0), as the game makes them
    void serve() {
        for (;;) {
            const int fd = hk::net::accept(listen_);
            if (fd < 0) continue;
            hk::net::set_timeouts(fd, 10000);
            std::string req;
            char buf[4096];
            for (;;) {   // the head, then Content-Length's body
                const size_t he = req.find("\r\n\r\n");
                if (he != std::string::npos) {
                    std::string head = req.substr(0, he);
                    for (char &c : head) c = (char)std::tolower((unsigned char)c);
                    size_t body = 0;
                    if (const size_t cl = head.find("\r\ncontent-length:"); cl != std::string::npos)
                        body = std::strtoul(head.c_str() + cl + 17, nullptr, 10);
                    if (body > (1u << 20) || req.size() >= he + 4 + body) break;
                }
                const ssize_t n = req.size() > (1u << 21) ? -1 : hk::net::recv(fd, buf, sizeof buf);
                if (n <= 0) {
                    req.clear();
                    break;
                }
                req.append(buf, (size_t)n);
            }
            if (!req.empty()) {
                const std::string answer = server_->handle(req);
                for (size_t sent = 0; sent < answer.size();) {
                    const ssize_t n = hk::net::send(fd, answer.data() + sent, answer.size() - sent);
                    if (n <= 0) break;
                    sent += (size_t)n;
                }
            }
            hk::net::close(fd);
        }
    }

    // Mods (docs/MODS.md) to the game's process: their memory patches (HK_PC_PATCHES) and the
    // game's files a mod's take the place of (HK_PC_FILES). A data file changed so gets the mod's
    // MD5 in the program's copy of dat.bin, the list the game checks its data by (a twentieth of
    // the files each boot: 5-1503 otherwise)
    // The mods' [[text]]s, looked for in the program as Windows lays it out (its sections at their
    // addresses), made patches with the bytes found as their original
    bool texts_in_program(std::vector<hk::mods::MemoryPatch> &out) {
        const auto texts = hk::mods::text_patches();
        if (texts.empty()) return true;
        std::ifstream in(program_ + "/" + exe_, std::ios::binary);
        const std::string f((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        auto u32 = [&](size_t at) {
            uint32_t v = 0;
            if (at + 4 <= f.size()) memcpy(&v, f.data() + at, 4);
            return v;
        };
        const size_t pe = u32(0x3c);
        if (f.size() < 0x40 || pe + 0xf8 > f.size() || u32(pe) != 0x4550) {
            fprintf(stderr, "mods: %s isn't a Windows program ([[text]] not looked for)\n", exe_.c_str());
            return false;
        }
        const uint32_t sections = (uint8_t)f[pe + 6] | (uint8_t)f[pe + 7] << 8, opt = (uint8_t)f[pe + 20] | (uint8_t)f[pe + 21] << 8;
        const uint32_t image_base = u32(pe + 24 + 28), image_size = u32(pe + 24 + 56);
        if (image_size > (64u << 20)) return false;
        std::vector<uint8_t> image(image_size, 0);
        for (uint32_t k = 0; k < sections; k++) {
            const size_t h = pe + 24 + opt + 40 * k;
            const uint32_t va = u32(h + 12), raw = u32(h + 16), at = u32(h + 20);
            if (va < image_size && at < f.size()) {
                const size_t n = std::min<size_t>({raw, image_size - va, f.size() - at});
                memcpy(image.data() + va, f.data() + at, n);
            }
        }
        for (const auto &t : texts) {
            uint32_t from = t.to ? t.from : image_base, to = t.to ? t.to : image_base + image_size;
            from = std::max(from, image_base), to = std::min(to, image_base + image_size);
            std::string why;
            auto found = to > from ? hk::mods::find_text(t, image.data() + (from - image_base), to - from, from, &why)
                                   : std::vector<hk::mods::MemoryPatch>{};
            if (!why.empty()) fprintf(stderr, "mods: %s: %s\n", t.mod.c_str(), why.c_str());
            else if (found.empty()) fprintf(stderr, "mods: %s: \"%s\" isn't in %s (%s)\n", t.mod.c_str(), t.label.c_str(), exe_.c_str(), t.line.c_str());
            out.insert(out.end(), found.begin(), found.end());
        }
        return true;
    }

    bool hand_mods(const std::string &drive, const std::string &data, const std::string &prog_name) {
        auto slashes = [](std::string p) {
            for (char &c : p)
                if (c == '\\') c = '/';
            return p;
        };
        auto lower = [](std::string p) {
            for (char &c : p) c = (char)std::tolower((unsigned char)c);
            return p;
        };
        std::map<std::string, std::string> md5_of;   // the game's path, lower case -> the mod file's MD5
        std::string list;
        for (const auto &[game_file, mod_file] : hk::mods::folder_files()) {
            const std::string g = slashes(game_file);
            std::string as;
            for (const auto &[dir, name] : {std::pair{slashes(data), data_as_}, std::pair{slashes(program_), drive + "\\prog\\" + prog_name}})
                if (g.size() > dir.size() && g.compare(0, dir.size(), dir) == 0 && g[dir.size()] == '/') as = name + g.substr(dir.size());
            if (as.empty()) continue;
            for (char &c : as)
                if (c == '/') c = '\\';
            list += as + "\t" + win_path(mod_file) + "\n";
            md5_of[lower(as)] = md5_file(mod_file);
        }
        std::ofstream(save_ + "/mods-files.txt", std::ios::trunc) << list;
        set_env("HK_PC_FILES", list.empty() ? "" : win_path(save_ + "/mods-files.txt"));
        // dat.bin: a count, then each file's MD5 (16 bytes) and its path (D:\..., ending in a zero);
        // the program's own each time (no mods: as it was)
        if (file_size(program_ + "/dat.bin") != UINT64_MAX) {
            std::ifstream in(program_ + "/dat.bin", std::ios::binary);
            std::string d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            size_t changed = 0;
            for (size_t at = 4; at + 16 < d.size() && !md5_of.empty();) {
                const size_t end = d.find('\0', at + 16);
                if (end == std::string::npos) break;
                auto it = md5_of.find(lower(d.substr(at + 16, end - at - 16)));
                if (it != md5_of.end() && it->second.size() == 16) d.replace(at, 16, it->second), changed++;
                at = end + 1;
            }
            std::ofstream(run_prog_ + "/dat.bin", std::ios::binary | std::ios::trunc) << d;
            if (changed) printf("mods: %zu of the game's files changed in its list of its files (dat.bin)\n", changed);
        }
        // the patches: "ADDRESS ONCE ORIGINAL REPLACE" (hex; "-": no original, a cheat); those in a
        // module (the 64-bit loader's) aren't this DLL's
        patches_ = hk::mods::memory_patches();
        for (size_t i = patches_.size(); i-- > 0;)
            if (!patches_[i].module.empty()) {
                fprintf(stderr, "mods: %s: a patch in %s isn't for this game's loader (left out)\n", patches_[i].mod.c_str(),
                        patches_[i].module.c_str());
                patches_.erase(patches_.begin() + (long)i);
            }
        if (!texts_in_program(patches_)) return false;
        // ... then those tied to a [[rate]] ("rate", "ticks": between the cabinet's ticks only)
        const auto rated = hk::mods::rate_patches();
        const size_t plain = patches_.size();
        for (const auto &r : rated) patches_.push_back(r.patch);
        if (patches_.size() > 256) {
            fprintf(stderr, "mods: %zu memory patches; a PC game takes 256\n", patches_.size());
            return false;
        }
        std::string text;
        auto hex = [](const std::vector<uint8_t> &b) {
            std::string h;
            char x[3];
            for (uint8_t v : b) snprintf(x, sizeof x, "%02x", v), h += x;
            return h.empty() ? std::string("-") : h;
        };
        for (size_t i = 0; i < patches_.size(); i++) {
            const auto &p = patches_[i];
            char a[32];
            snprintf(a, sizeof a, "%08x %d ", p.address, p.once ? 1 : 0);
            text += a + hex(p.original) + " " + hex(p.replace) + (i < plain ? "" : rated[i - plain].between_ticks ? " ticks" : " rate") + "\n";
        }
        std::ofstream(save_ + "/mods-patches.txt", std::ios::trunc) << text;
        set_env("HK_PC_PATCHES", text.empty() ? "" : win_path(save_ + "/mods-patches.txt"));
        if (!list.empty() || !text.empty())
            printf("mods: %zu of the game's files and %zu memory patches go to the game\n", md5_of.size(), patches_.size());
        return true;
    }

    // A patch the game's process put in place: said once (as the other cores' mods::frame says it)
    void patches_said() {
        for (size_t i = 0; i < patches_.size(); i++)
            if (link_->patch_done[i] && !patch_said_[i]) {
                patch_said_[i] = true;
                fprintf(stderr, "mods: %s: patch at 0x%08x (%zu bytes) in place\n", patches_[i].mod.c_str(), patches_[i].address,
                        patches_[i].replace.size());
            }
    }

    const uint32_t *link_pixels() const { return (const uint32_t *)((const char *)link_ + HK_PC_PIXELS); }

    // The game may draw its next frame (it waits in its Present for this), to be shown in the
    // shell's next frame (with --game-ahead, the one after): that frame's sound comes with it
    void go(uint32_t seq) {
        link_->audio_frame_end = sound_on_ ? sound_for(tick_ + pics_ + 1 + (ahead_ ? per_frame_ : 0)) : 0;
        go_at_ = std::chrono::steady_clock::now();
        link_->go_host_us = host_us(go_at_);
        link_->not_shown = !ahead_ && pics_ + 1 < per_frame_;
        link_->game_hz_milli = (uint32_t)std::lround(ghz() * 1000);
        std::atomic_thread_fence(std::memory_order_release);
        link_->frame_go = seq;
    }

    // The game's time in its own frames (ticks: 60 a second, the 64-bit PC pop'n's 120), each
    // shown frame per_frame_ of them; its sound, 44.1 kHz: where it ends with tick `t`
    double ghz() const { return pc64_ ? 120.0 : hz_; }
    uint64_t samples_at(uint64_t t) const {
        return samp0_ + (uint64_t)std::floor((double)(t - t0_) * HK_PC_AUDIO_RATE / ghz() + 1e-9);
    }
    uint32_t sound_for(uint64_t t) const { return sound_base_ + (uint32_t)samples_at(t); }
    // The game's pictures for each frame shown: a 120 Hz game (the 64-bit pop'n) draws two on a
    // display under 100 Hz (the first not shown; display sync's grid then fits 60 Hz), else one; a
    // [[rate]]'s, the rate over the display's, rounded, at least one; a 60 Hz game, or no display
    // known, one (the core then paces itself)
    int pictures_per_frame() const {
        if (ghz() < 61) return 1;
        const double hz = hk::host::pacing::refresh_hz();
        if (pc64_) return hz > 0 && hz < 100 ? 2 : 1;
        return hz > 0 ? std::clamp((int)std::lround(ghz() / hz), 1, 8) : 1;
    }

    // The sound mixed at least to `end`: the DLL's mixer asked (audio_want moved on, never back),
    // and given a few milliseconds (it naps a millisecond)
    void sound_until(uint32_t end) {
        if (pc64_) return;   // the 64-bit loader's sound runs on its own device clock: taken as it comes
        for (uint32_t want = link_->audio_want; (int32_t)(end - want) > 0;)
            if (__atomic_compare_exchange_n(&link_->audio_want, &want, end, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) break;
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(3);
        while ((int32_t)(end - link_->audio_made) > 0 && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    // A picture came at `now`: its frame's times (the DLL's, and since go() here)
    void timed(std::chrono::steady_clock::time_point now) {
        if (go_at_ == std::chrono::steady_clock::time_point{} || go_to_shell_.size() >= 1u << 20) return;
        go_to_shell_.push_back(std::chrono::duration<float, std::milli>(now - go_at_).count());
        pad_read_.push_back(link_->pad_read_us / 1000.0f);
        drawn_.push_back(link_->drawn_us / 1000.0f);
    }

    // The sound mixed so far, to the shell's output (HIRAKU's: device, latency, pacing). true if any
    bool take_sound() {
        const uint32_t made = link_->audio_made, taken = link_->audio_taken;
        if (made == taken || !audio_sink_) return false;
        std::atomic_thread_fence(std::memory_order_acquire);
        const float *ring = (const float *)((const char *)link_ + HK_PC_AUDIO);
        uint32_t at = taken;
        while (at != made) {
            const uint32_t i = at & (HK_PC_AUDIO_FRAMES - 1), n = std::min(made - at, HK_PC_AUDIO_FRAMES - i);
            audio_sink_(ring + 2 * i, (int)n);
            at += n;
            link_->audio_taken = at;
        }
        // a button's sound handed on: the time since the button went down (here; the DLL's own
        // keys aren't timed)
        const uint32_t ks = link_->key_sound_seq;
        if (ks != key_seen_) {
            std::atomic_thread_fence(std::memory_order_acquire);
            if ((int32_t)(at - link_->key_sound_at) > 0) {
                key_seen_ = ks;
                if (pressed_at_ != std::chrono::steady_clock::time_point{} && key_to_sound_.size() < 1u << 20) {
                    key_to_sound_.push_back(std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - pressed_at_).count());
                    if (getenv("HK_PC_KEYLOG"))
                        fprintf(stderr, "key: frame %llu, %.2f ms\n", (unsigned long long)frame_, (double)key_to_sound_.back());
                }
                pressed_at_ = {};
            }
        }
        return true;
    }

    // The picture to the shell, with its frame's sound: a frame's for every frame shown (the DLL
    // mixed it as the game counted the picture; for a picture again, or a late one, it's asked for)
    void show(const uint32_t *rgb, int w, int h) {
        if (!sound_on_ && link_->sound_on) {   // the game's sound starts: from this frame on
            sound_on_ = true;
            sound_base_ = link_->audio_made - (uint32_t)samples_at(tick_);
        }
        if (sound_on_) {
            sound_until(sound_for(tick_ + per_frame_));
        } else if (audio_sink_) {   // no sound from the game yet (starting): a frame's silence
            const int n = (int)(samples_at(tick_ + per_frame_) - samples_at(tick_));
            silence_.assign(2 * (size_t)n, 0.0f);
            audio_sink_(silence_.data(), n);
        }
        take_sound();
        if (w <= 0 || h <= 0) rgb = black_.data(), w = 640, h = 480;
        last_rgb_ = rgb, last_w_ = w, last_h_ = h;
        tick_ += per_frame_;
        ++frame_;
        if (frame_hook_) frame_hook_(frame_, rgb, w, h);   // (mods::frame: set_vsync_rate)
        if (want_hz_ > 0 && want_hz_ != hz_) {   // the new rate from here on, its time and sound continued
            samp0_ = samples_at(tick_);
            sec0_ = seconds();
            t0_ = tick_;
            hz_ = want_hz_;
        }
        want_hz_ = 0;
    }

    [[noreturn]] void fatal() {
        if (on_fatal_) on_fatal_();
        exit(0);
    }

    void present_card(const std::string &number) {
        if (number.size() != 16) {
            printf("e-AMUSEMENT: no card number (-card0, or card0.txt in the save folder)\n");
            return;
        }
        for (int i = 0; i < 8; i++) link_->card_uid[i] = (uint8_t)std::stoul(number.substr(2 * i, 2), nullptr, 16);
        link_->card_seq = link_->card_seq + 1;
        printf("e-AMUSEMENT: card %s held to the reader\n", number.c_str());
    }

    // The data files the game's list (pop'n 15's dat.bin: each file's MD5 and path) names that its
    // data folder hasn't: a dump missing some, which the game faults on when it loads one. Said
    // when the game ends other than by being stopped.
    std::string data_dir_;   // the game's data folder (link_data)
    std::string missing_data() {
        std::ifstream in(program_ + "/dat.bin", std::ios::binary);
        const std::string d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::string prefix = data_as_ + "\\";
        for (char &c : prefix) c = (char)tolower((unsigned char)c);
        size_t listed = 0, missing = 0;
        std::string first;
        for (size_t at = 4; at + 16 < d.size();) {
            const size_t end = d.find('\0', at + 16);
            if (end == std::string::npos) break;
            std::string path = d.substr(at + 16, end - at - 16);
            at = end + 1;
            std::string low = path;
            for (char &c : low) c = (char)tolower((unsigned char)c);
            if (low.rfind(prefix, 0) != 0) continue;
            listed++;
            std::string rel = path.substr(prefix.size());
            for (char &c : rel)
                if (c == '\\') c = '/';
            if (file_size(data_dir_ + "/" + rel) == UINT64_MAX && !missing++) first = rel;
        }
        if (!missing) return "";
        return "; its data folder lacks " + std::to_string(missing) + " of the " + std::to_string(listed) +
               " files its list names (dat.bin; the first: " + first + "): a dump missing files, which the game faults on";
    }

#ifdef _WIN32
    PROCESS_INFORMATION pi_{};
    bool started_ = false;
    bool start_game() {
        STARTUPINFOA si{};
        si.cb = sizeof si;
        std::string cmd = "\"" + win_path(run_prog_ + "/" + exe_) + "\"";
        std::string dir = win_path(run_prog_);
        if (pc64_) cmd = "\"" + win_path(loader_) + "\" \"" + win_path(contents_) + "\"", dir = win_path(save_ + "/run");
        if (pc32_) cmd += " .", dir = win_path(save_ + "/run");   // the loader's game folder: run/, AVS2's root
        if (CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi_))
            return started_ = true;
        fprintf(stderr, "hiraku-pc: can't start %s (error %lu)\n", cmd.c_str(), GetLastError());
        return false;
    }
    bool game_running() { return started_ && WaitForSingleObject(pi_.hProcess, 0) == WAIT_TIMEOUT; }
    std::string exit_note() {   // its exit code: 0xc0000005 a fault, 0xc0000409 a fail-fast...
        DWORD code = 0;
        char s[48];
        if (!started_ || !GetExitCodeProcess(pi_.hProcess, &code)) return "";
        snprintf(s, sizeof s, " (exit code %#lx)", (unsigned long)code);
        if (code == 0xc0000135) return s + missing_dlls();
        return code ? s + missing_data() : s;
    }
    // A game Windows couldn't start for a DLL it can't find (0xc0000135): the ones its program and
    // the DLLs beside it import that are neither beside it nor in Windows' folder for its kind
    // (System32 for 64-bit, SysWOW64 for 32-bit on a 64-bit Windows). API sets aren't files.
    std::string missing_dlls() {
        if (pc64_ || pc32_) return "";
        char sys[MAX_PATH] = "", wow[MAX_PATH] = "";
        GetSystemDirectoryA(sys, MAX_PATH);
        const bool wow64 = GetSystemWow64DirectoryA(wow, MAX_PATH) > 0;
        std::vector<std::string> todo = {exe_}, missing;
        std::map<std::string, bool> seen;
        while (!todo.empty()) {
            const std::string file = todo.back();
            todo.pop_back();
            std::vector<std::string> names;
            bool pe64 = false;
            if (!pe_imports(run_prog_ + "/" + file, names, pe64)) continue;
            const std::string system = !pe64 && wow64 ? wow : sys;
            for (const std::string &n : names) {
                std::string low = n;
                for (char &c : low) c = (char)tolower((unsigned char)c);
                if (seen[low] || low.rfind("api-ms-", 0) == 0 || low.rfind("ext-ms-", 0) == 0) continue;
                seen[low] = true;
                if (GetFileAttributesA(win_path(run_prog_ + "/" + n).c_str()) != INVALID_FILE_ATTRIBUTES) todo.push_back(n);
                else if (GetFileAttributesA((system + "\\" + n).c_str()) == INVALID_FILE_ATTRIBUTES) missing.push_back(n + " (for " + file + ")");
            }
        }
        if (missing.empty()) return "";
        std::string s = ": Windows can't find";
        bool directx = false;
        for (const std::string &m : missing) {
            s += " " + m;
            std::string low = m;
            for (char &c : low) c = (char)tolower((unsigned char)c);
            directx |= low.rfind("d3dx", 0) == 0 || low.rfind("xinput", 0) == 0 || low.rfind("d3dcompiler", 0) == 0 ||
                       low.rfind("xaudio", 0) == 0 || low.rfind("x3daudio", 0) == 0;
        }
        s += ". It belongs in the game's program folder, as the game came";
        if (directx) s += "; Microsoft's DirectX End-User Runtime (June 2010) installs it too";
        return s;
    }
    void stop_game() {
        if (!started_) return;
        started_ = false;
        TerminateProcess(pi_.hProcess, 0);
        WaitForSingleObject(pi_.hProcess, 5000);
        CloseHandle(pi_.hProcess), CloseHandle(pi_.hThread);
    }
#else
    pid_t pid_ = 0;
    bool start_game() {
        // Wine's own folder for HIRAKU's PC games, made the first time without asking to install
        // Mono or Gecko (the games need neither)
        const std::string prefix = save_.substr(0, save_.rfind('/')) + "/pc-wine";
        setenv("WINEPREFIX", prefix.c_str(), 1);
        if (!getenv("WINEDEBUG")) setenv("WINEDEBUG", "-all", 1);
        if (!hk::sys::is_dir(prefix + "/drive_c")) {
            printf("hiraku-pc: preparing Wine's folder for the PC games (the first time only)\n");
            fflush(stdout);
            setenv("WINEDLLOVERRIDES", "mscoree=;mshtml=", 1);
            const int r = hk::sys::run_quiet({"/bin/sh", "-c", "exec \"$0\" wineboot -i", wine_});
            unsetenv("WINEDLLOVERRIDES");
            if (r == 127) {   // the shell's "not found"
                fprintf(stderr, "hiraku-pc: Wine isn't installed (\"%s\" not found): on Linux the PC games run under it. Install "
                                "it with your distribution's packages (Arch: sudo pacman -S wine; Debian, Ubuntu: sudo apt install "
                                "wine; Fedora: sudo dnf install wine), or name it with --wine PROGRAM\n", wine_.c_str());
                exit(2);
            }
            if (r != 0) fprintf(stderr, "hiraku-pc: wineboot said %d\n", r);
        }
        const std::string exe_path = pc64_ ? loader_ : run_prog_ + "/" + exe_, game_dir = pc32_ ? "." : win_path(contents_);
        const std::string dir = pc64_ || pc32_ ? save_ + "/run" : run_prog_;
        char *args[] = {(char *)wine_.c_str(), (char *)exe_path.c_str(), pc64_ || pc32_ ? (char *)game_dir.c_str() : nullptr, nullptr};
        // The AVS2 pop'n's environment: Japanese (its text is Shift-JIS: Wine's code page 932), and for
        // the 64-bit one DXVK's Direct3D 9 when it is beside the loader (bin/pc/d3d9.dll: Wine's own
        // misplaces its pictures, PLAN.md section 32)
        std::vector<std::string> env_own;
        std::vector<char *> env;
        // The IIDX games from GOLD on: Microsoft's d3dx9_24.dll in their folder, which Wine passes over for
        // its own (without D3DXLoadMeshFromXof: GOLD aborted loading its models)
        bool d3dx_own = false;
        if (family_ == "iidx" && !(pc64_ || pc32_))
            for (const std::string &f : list_files(run_prog_)) {
                std::string lower = f;
                for (char &c : lower) c = (char)std::tolower((unsigned char)c);
                d3dx_own = d3dx_own || lower == "d3dx9_24.dll";
            }
        // Every IIDX in Japanese: its text is Shift-JIS (Wine's code page 932), the operator menu's
        // messages too (10th style's were garbled without)
        const bool iidx_ja = family_ == "iidx" && !(pc64_ || pc32_);
        const bool own_lc = pc64_ || pc32_ || iidx_ja, own_dlls = pc64_ || pc32_ || d3dx_own;
        for (char **e = environ; *e; e++)
            if (!(own_lc && !strncmp(*e, "LC_ALL=", 7)) && !(own_dlls && !strncmp(*e, "WINEDLLOVERRIDES=", 17))) env.push_back(*e);
        if (iidx_ja) env_own.push_back("LC_ALL=ja_JP.UTF-8");
        if (d3dx_own) {
            const char *more = getenv("WINEDLLOVERRIDES");
            env_own.push_back(std::string("WINEDLLOVERRIDES=d3dx9_24=n,b") + (more && *more ? std::string(";") + more : ""));
        }
        if (iidx_ja)
            for (std::string &e : env_own) env.push_back(e.data());
        if (pc32_) {
            env_own.push_back("LC_ALL=ja_JP.UTF-8");
            env_own.push_back(std::string("WINEDLLOVERRIDES=mscoree=;mshtml=") + (dxvk32_ ? ";d3d9=n,b" : ""));
            for (std::string &e : env_own) env.push_back(e.data());
        }
        if (pc64_) {
            env_own.push_back("LC_ALL=ja_JP.UTF-8");
            const std::string dxvk = loader_.substr(0, loader_.rfind('/')) + "/d3d9.dll";
            env_own.push_back(std::string("WINEDLLOVERRIDES=mscoree=;mshtml=") + (file_size(dxvk) != UINT64_MAX ? ";d3d9=n,b" : ""));
            for (std::string &e : env_own) env.push_back(e.data());
        }
        env.push_back(nullptr);
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_addchdir_np(&fa, dir.c_str());
        const int r = posix_spawnp(&pid_, wine_.c_str(), &fa, nullptr, args, env.data());
        posix_spawn_file_actions_destroy(&fa);
        if (r == 0) return true;
        pid_ = 0;
        fprintf(stderr, "hiraku-pc: can't start %s (is Wine installed? --wine PROGRAM)\n", wine_.c_str());
        return false;
    }
    std::string exit_note() { return missing_data(); }
    bool game_running() {
        int status;
        return pid_ > 0 && waitpid(pid_, &status, WNOHANG) == 0;
    }
    // The game and whatever else Wine runs for it in this folder
    void stop_game() {
        if (pid_ <= 0) return;
        const std::string ws = wine_.size() >= 4 && wine_.compare(wine_.size() - 4, 4, "wine") == 0
                                   ? wine_.substr(0, wine_.size() - 4) + "wineserver" : "wineserver";
        hk::sys::run_quiet({"/bin/sh", "-c", "exec \"$0\" -k", ws});
        kill(pid_, SIGTERM);
        int status;
        waitpid(pid_, &status, 0);
        pid_ = 0;
    }
#endif

    std::string game_, family_ = "pcpopn", exe_, program_, save_, run_prog_, run_e_, link_path_, wine_ = "wine";
    bool pc64_ = false;               // the 64-bit loader runs the game (init_pc64)
    bool pc32_ = false;               // the 32-bit AVS2 loader runs the game (init_pc32)
    bool dxvk32_ = false;             // ... with DXVK's Direct3D 9 beside it
    bool c02_ = false;                // the C02 cabinet's board (beatmania IIDX 9th style, PLAN.md section 33)
    bool c02_driver_ = false;         // ... under its driver (IIDX RED, PLAN.md section 36)
    std::string data_as_;             // the name the game gives its data folder (HK_PC_MAP)
    int tt_frac_[2] = {};             // turntable_add's counts not yet a whole step
    bool crt_ = false;                // --single-screen: the 64-bit loader's single-screen mode
    std::string market_;              // --market: the 64-bit loader's HK_PC64_MARKET
    std::string contents_, loader_;   // its game folder, hiraku-pc64.exe (contents_: the 32-bit one's too)
    hk::EaOptions ea_;
    std::unique_ptr<hk::py::EamuseServer> server_;
    hk::py::RelayServer relay_;
    int listen_ = -1;
    hk_pc_link *link_ = nullptr;
    FrameHook frame_hook_;
    AudioSink audio_sink_;
    std::vector<hk::mods::MemoryPatch> patches_;   // the mods' (hand_mods)
    bool patch_said_[256] = {};
    bool ahead_ = false;   // --game-ahead
    bool window_ = false;  // HIRAKU's window shows the game (Options::window)
    bool sound_on_ = false;
    uint32_t sound_base_ = 0;   // sound_for()
    std::chrono::steady_clock::time_point go_at_{};
    std::vector<float> go_to_shell_, pad_read_, drawn_;   // ms, per picture (report)
    std::chrono::steady_clock::time_point pressed_at_{};   // a button went down (set_input)
    bool press_time_ = false;                              // --press-time
    bool skip_tutorial_ = false;                           // --skip-tutorial
    double judge_offset_ = 0;                              // --judge-offset, milliseconds
    // A time on the link (link.h): microseconds of the steady clock, its low 32 bits
    static uint32_t host_us(std::chrono::steady_clock::time_point t) {
        return (uint32_t)std::chrono::duration_cast<std::chrono::microseconds>(t.time_since_epoch()).count();
    }
    uint32_t key_seen_ = 0;
    std::vector<float> key_to_sound_;   // ms, per press (report)
    std::function<void()> on_fatal_;
    uint64_t frame_ = 0, repeats_ = 0;
    // frames shown again while the game's sound plays, its pictures for them still to come (run());
    // at most MAX_OWED (half a second at 60 Hz)
    static constexpr int MAX_OWED = 30;
    int owed_ = 0;
    uint64_t made_up_ = 0, not_made_up_ = 0;
    uint64_t tick_ = 0;    // the game's time in its frames (ghz())
    // ghz() and where it last changed (a [[rate]]): the tick, the time and the sound samples then
    double hz_ = HZ, want_hz_ = 0, sec0_ = 0;
    uint64_t t0_ = 0, samp0_ = 0;
    mutable uint32_t watch_copy_[HK_PC_WATCHES] = {};
    bool repeating_ = false;   // show() of the last picture again (frame_repeated)
    int per_frame_ = 1;    // its pictures for each frame shown (pictures_per_frame())
    int pics_ = 0;         // ... of the frame being made, received so far
    std::vector<uint32_t> black_, picture_;
    std::vector<float> silence_;
    std::chrono::steady_clock::time_point due_{};
    const uint32_t *last_rgb_ = nullptr;
    int last_w_ = 640, last_h_ = 480;
    mutable std::vector<uint8_t> lamps_;
    mutable uint32_t last_lamps_ = 0;
    mutable uint64_t changes_ = 0;
};

PcCore g_core;

}  // namespace

hk::Core &hk::core() { return g_core; }
