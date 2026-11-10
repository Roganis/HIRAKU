// SPDX-License-Identifier: GPL-3.0-or-later
// hk::saves (runtime/shell/saves.h): a machine's saved memories written while a game is played. A set
// of files is the folder's whole or not at all, whenever the program is killed: a set cut short is
// finished or dropped at the next start, never left half one moment's and half another's.
//
//   test_saves      exit 0 when all hold
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "shell/saves.h"

#ifndef _WIN32
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;
using hk::saves::File;

int failed = 0;
void check(bool ok, const char *what) {
    printf("%s%s\n", ok ? "ok    " : "FAIL  ", what);
    fflush(stdout);
    failed += !ok;
}

void put(const fs::path &p, const std::string &d) { std::ofstream(p, std::ios::binary) << d; }
std::string get(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}
size_t count(const fs::path &dir) {
    size_t n = 0;
    for (const auto &e : fs::directory_iterator(dir)) (void)e, n++;
    return n;
}

// The files of one moment: each all of one letter (the moment's), of different sizes
std::vector<File> moment(int n) {
    const char c = (char)('A' + n % 26);
    return {{"clock", std::string(8192, c)}, {"flash1", std::string(1 << 20, c)}, {"flash2", std::string(1 << 20, c)}, {"eeprom", std::string(140, c)}};
}

}  // namespace

int main() {
    const fs::path top = fs::temp_directory_path() / ("hk-saves-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(top);
    const std::string dir = top.string();

    // a set
    check(hk::saves::write_set(dir, moment(0)), "a set of four files is written");
    check(get(top / "clock") == std::string(8192, 'A') && get(top / "flash2") == std::string(1 << 20, 'A') && count(top) == 4, "... each in its place, nothing else left");
    check(hk::saves::write_set(dir, {{"clock", "new clock"}}) && get(top / "clock") == "new clock" && count(top) == 4, "one file alone is a set too");
    check(hk::saves::write_set(dir, {}) && count(top) == 4, "... and so is none");
    check(!hk::saves::write_set(dir + "/not-there", moment(1)), "a folder that can't be written: said, not done");

    // cut short before the set was whole: the files beside their places are dropped
    hk::saves::write_set(dir, moment(2));
    put(top / "clock.hk-new", "half");
    put(top / "flash1.hk-new", "half");
    hk::saves::recover(dir);
    check(get(top / "clock") == std::string(8192, 'C') && get(top / "flash1") == std::string(1 << 20, 'C') && count(top) == 4, "cut before its list: the set before stays, whole");

    // cut short after: its list is there, some files already in place
    put(top / "clock.hk-new", "next clock");
    put(top / "flash2.hk-new", "next flash2");
    put(top / "eeprom", "next eeprom");   // (this one was already renamed)
    put(top / "save-set.pending", "clock\nflash2\neeprom\n../outside\n");
    put(top.parent_path() / "outside.hk-new", "not this folder's");
    hk::saves::recover(dir);
    check(get(top / "clock") == "next clock" && get(top / "flash2") == "next flash2" && get(top / "eeprom") == "next eeprom" && count(top) == 4,
          "cut after its list: finished at the next start, the list gone");
    check(fs::exists(top.parent_path() / "outside.hk-new") && !fs::exists(top.parent_path() / "outside"), "... a list names files of its own folder only");
    fs::remove(top.parent_path() / "outside.hk-new");
    hk::saves::recover(dir);
    check(count(top) == 4, "a folder with nothing cut: left as it is");

    // what tells a change
    {
        std::string a(2 << 20, 'x'), b = a;
        b.back() = 'y';
        std::string c = a;
        c[12345] = 'y';
        check(hk::saves::hash(a) != hk::saves::hash(b) && hk::saves::hash(a) != hk::saves::hash(c) && hk::saves::hash(a) == hk::saves::hash(std::string(2 << 20, 'x')) &&
              hk::saves::hash(std::string(7, 'x')) != hk::saves::hash(std::string(8, 'x')) && hk::saves::hash("") != hk::saves::hash(std::string(1, '\0')),
              "a changed byte or length gives another hash, the same bytes the same");
    }

    // the writer's thread
    {
        hk::saves::Writer w;
        for (int n = 3; n < 9; n++) w.write(dir, moment(n));
        w.drain();
        check(get(top / "clock") == std::string(8192, 'I') && get(top / "flash1") == std::string(1 << 20, 'I') && count(top) == 4, "the writer: sets written in the order given, all there after drain");
        // sets that come while one is being written: joined, the folder ends as the last moment
        for (int n = 9; n < 60; n++) {
            std::vector<File> set = moment(n);
            if (n % 2) set.erase(set.begin() + 1);   // (not every set has every file: flash1 as the set before had it)
            w.write(dir, std::move(set));
        }
        w.drain();
        check(get(top / "clock") == std::string(8192, (char)('A' + 59 % 26)) && get(top / "flash2") == std::string(1 << 20, (char)('A' + 59 % 26)) &&
              get(top / "flash1") == std::string(1 << 20, (char)('A' + 58 % 26)) && count(top) == 4,
              "... sets given faster than they are written: each file as the last set that had it");
        hk::saves::write_set(dir, moment(8));
        w.write(dir + "/not-there", moment(9));
        w.drain();
        check(w.failed() == 1 && get(top / "clock") == std::string(8192, 'I'), "... one it couldn't write is counted");
    }

#ifndef _WIN32
    // killed at any moment while it writes set after set: the folder is one moment's, whole
    {
        int bad = 0, finished = 0, moments = 0;
        char last = 0;
        for (int round = 0; round < 40; round++) {
            const pid_t child = fork();
            if (child == 0) {
                for (int n = round;; n++) hk::saves::write_set(dir, moment(n));
            }
            std::this_thread::sleep_for(std::chrono::microseconds(3000 + 1733 * round));
            kill(child, SIGKILL);
            waitpid(child, nullptr, 0);
            finished += fs::exists(top / "save-set.pending");
            hk::saves::recover(dir);
            const std::string clock = get(top / "clock"), f1 = get(top / "flash1"), f2 = get(top / "flash2"), ee = get(top / "eeprom");
            const char c = clock.empty() ? 0 : clock[0];
            const bool whole = clock == std::string(8192, c) && f1 == std::string(1 << 20, c) && f2 == std::string(1 << 20, c) && ee == std::string(140, c) && count(top) == 4;
            bad += !whole;
            moments += c != last;
            last = c;
        }
        check(bad == 0, "killed 40 times while writing sets: the folder was one moment's each time, whole");
        printf("      (%d of the kills fell between a set's list and its last rename and were finished; %d different moments seen)\n", finished, moments);
        check(moments > 5, "... and the sets did get written between the kills");
    }
#endif
    fs::remove_all(top);
    printf(failed ? "%d failed\n" : "all hold\n", failed);
    return failed ? 1 : 0;
}
