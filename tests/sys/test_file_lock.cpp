// SPDX-License-Identifier: GPL-3.0-or-later
// hk::sys::FileLock (runtime/shell/sys.h): a file held by one holder at a time, over threads and
// processes, each waiting its turn: a count read, changed and written back by many is never lost.
//
//   test_file_lock              exit 0 when all hold
//   test_file_lock held FILE    exit 0 if FILE is held right now, 1 if not (file_lock_held: for
//                               tests/tools/test_update_apply.py, whose lock is tools/update.py's)
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "shell/sys.h"

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;
int failed = 0;
void check(bool ok, const char *what) {
    printf("%s%s\n", ok ? "ok    " : "FAIL  ", what);
    fflush(stdout);
    failed += !ok;
}

long read_count(const std::string &file) {
    std::ifstream f(file);
    long n = 0;
    f >> n;
    return n;
}

// The count in `file` one more, `times` times: read, a moment for another to get in, written back
void count_up(const std::string &file, const std::string &lock, int times) {
    for (int i = 0; i < times; i++) {
        const hk::sys::FileLock held(lock);
        const long n = read_count(file);
        std::this_thread::yield();
        std::ofstream(file, std::ios::trunc) << n + 1 << "\n";
    }
}

}  // namespace

int main(int argc, char **argv) {
    if (argc == 3 && std::string(argv[1]) == "held") return hk::sys::file_lock_held(argv[2]) ? 0 : 1;
    const fs::path top = fs::temp_directory_path() / ("hk-filelock-" + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())));
    fs::create_directories(top);
    const std::string file = (top / "count").string(), lock = (top / ".lock").string();

    {
        const hk::sys::FileLock a(lock);
        check(a.held() && fs::exists(lock), "the lock's file is made and held");
        const hk::sys::FileLock none("");
        check(!none.held(), "no file: nothing held, and no wait");
        const hk::sys::FileLock bad((top / "no-such-folder" / "lock").string());
        check(!bad.held(), "a file that can't be made: nothing held, and no wait");
        check(hk::sys::file_lock_held(lock), "asked while it is held: held");
    }
    check(!hk::sys::file_lock_held(lock), "... and once let go: not held");
    check(!hk::sys::file_lock_held((top / "no-such-file").string()) && !fs::exists(top / "no-such-file"),
          "... a file that isn't there: not held, and not made");
    {
        const hk::sys::FileLock again(lock);
        check(again.held(), "... asking took nothing: the lock is there to take");
    }
    // threads of one process
    std::ofstream(file) << "0\n";
    {
        std::vector<std::thread> t;
        for (int k = 0; k < 4; k++) t.emplace_back(count_up, file, lock, 500);
        for (std::thread &x : t) x.join();
    }
    check(read_count(file) == 2000, "4 threads counting 500 each in one file: 2000");
    // ... and without it, the count is lost (what the lock is for; a machine with one CPU may get it right)
    std::ofstream(file) << "0\n";
    {
        std::vector<std::thread> t;
        for (int k = 0; k < 4; k++) t.emplace_back(count_up, file, std::string(), 500);
        for (std::thread &x : t) x.join();
    }
    printf("      (without the lock: %ld of 2000)\n", read_count(file));
#ifndef _WIN32
    // processes
    std::ofstream(file) << "0\n";
    std::vector<pid_t> kids;
    for (int k = 0; k < 4; k++) {
        const pid_t p = fork();
        if (p == 0) {
            count_up(file, lock, 500);
            _exit(0);
        }
        kids.push_back(p);
    }
    for (pid_t p : kids) waitpid(p, nullptr, 0);
    check(read_count(file) == 2000, "4 processes counting 500 each: 2000");
    // a holder that dies lets go
    const pid_t p = fork();
    if (p == 0) {
        const hk::sys::FileLock held(lock);
        abort();
    }
    waitpid(p, nullptr, 0);
    {
        const hk::sys::FileLock after(lock);
        check(after.held(), "a holder that died: the lock is free");
    }
#endif
    fs::remove_all(top);
    printf(failed ? "%d failed\n" : "all hold\n", failed);
    return failed ? 1 : 0;
}
