// SPDX-License-Identifier: GPL-3.0-or-later
// hk::sys::lock_file (runtime/shell/sys.h): a file one process has until it ends, however it ends.
// A second process is told it is held; the lock goes with a process killed outright, and isn't
// carried by a program the holder started. This program is also the holder and what it starts.
//
//   test_lock_file      exit 0 when all hold
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "shell/sys.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <csignal>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using hk::sys::Lock;

int failed = 0;
void check(bool ok, const char *what) {
    printf("%s%s\n", ok ? "ok    " : "FAIL  ", what);
    fflush(stdout);
    failed += !ok;
}

// A program started the plain way, inheriting what a started program inherits: its number, 0 if not
long spawn(const std::vector<std::string> &argv) {
#ifdef _WIN32
    std::string cmd;
    for (const std::string &a : argv) cmd += (cmd.empty() ? "\"" : " \"") + a + "\"";
    STARTUPINFOA si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) return 0;
    CloseHandle(pi.hProcess), CloseHandle(pi.hThread);
    return (long)pi.dwProcessId;
#else
    std::vector<char *> args;
    for (const std::string &a : argv) args.push_back((char *)a.c_str());
    args.push_back(nullptr);
    pid_t pid = 0;
    return posix_spawn(&pid, args[0], nullptr, nullptr, args.data(), environ) == 0 ? (long)pid : 0;
#endif
}

void kill_outright(long pid) {
#ifdef _WIN32
    if (HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, (DWORD)pid)) {
        TerminateProcess(h, 9);
        WaitForSingleObject(h, 10000);
        CloseHandle(h);
    }
#else
    kill((pid_t)pid, SIGKILL);
    int status;
    waitpid((pid_t)pid, &status, 0);   // (not ours when the holder started it: gone soon all the same)
#endif
}

template <class F> bool within(std::chrono::seconds limit, F &&f) {
    for (const auto until = std::chrono::steady_clock::now() + limit; std::chrono::steady_clock::now() < until;) {
        if (f()) return true;
        std::this_thread::sleep_for(20ms);
    }
    return f();
}

long number_in(const fs::path &file) {
    std::ifstream in(file);
    long n = 0;
    in >> n;
    return n;
}
void say(const fs::path &file, long n) {
    std::ofstream(fs::path(file).concat(".part")) << n << "\n";
    fs::rename(fs::path(file).concat(".part"), file);
}
long my_pid() {
#ifdef _WIN32
    return (long)GetCurrentProcessId();
#else
    return (long)getpid();
#endif
}
[[noreturn]] void idle() {
    for (;;) std::this_thread::sleep_for(1h);
}

}  // namespace

int main(int argc, char **argv) {
    const std::string self = hk::sys::exe_path();
    const std::string mode = argc > 1 ? argv[1] : "";
    const fs::path dir = argc > 2 ? fs::path(argv[2]) : fs::path(self).concat(".d");
    const std::string lock = (dir / "lock").string();

    if (mode == "started") {   // a program the holder started
        say(dir / "started", my_pid());
        idle();
    }
    if (mode == "holder") {   // takes the lock, starts a program, says so, and stays
        const Lock got = hk::sys::lock_file(lock);
        if (got == Lock::Taken) spawn({self, "started", dir.string()});
        say(dir / "holder", got == Lock::Taken ? my_pid() : -1);
        idle();
    }

    fs::remove_all(dir);
    fs::create_directories(dir);

    check(hk::sys::lock_file((dir / "no-such-folder" / "lock").string()) == Lock::None, "a file that can't be made: nothing known, not \"held\"");

    const long holder = spawn({self, "holder", dir.string()});
    long said = 0, started = 0;
    check(holder && within(30s, [&] { return (said = number_in(dir / "holder")) != 0 && (started = number_in(dir / "started")) != 0; }) && said == holder,
          "one process takes the lock (the file made), and starts a program");
    check(hk::sys::lock_file(lock) == Lock::Held, "a second process: told it is held");
    check(hk::sys::lock_file(lock) == Lock::Held, "... each time it asks");

    kill_outright(holder);
    // (the program the holder started is still running: it didn't get the lock with what it inherited)
    check(within(10s, [&] { return hk::sys::lock_file(lock) == Lock::Taken; }), "the holder killed outright, the program it started still running: the lock can be taken");
    check(hk::sys::lock_file(lock) == Lock::Held, "... and is held now, here");
    kill_outright(started);

    std::error_code ec;
    fs::remove_all(dir, ec);
    printf(failed ? "%d failed\n" : "all hold\n", failed);
    return failed ? 1 : 0;
}
