// SPDX-License-Identifier: GPL-3.0-or-later
// The PC game's process (runtime/pc/process.h), with this program standing in for hiraku-pc, for the
// game, and for a program the game starts: the game and what it started end when hiraku-pc is killed
// outright (and the file it would have removed is removed), and one game stopped leaves another
// running. No Wine and no game: the same holds for any program.
//
//   test_game_process      exit 0 when all hold
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "pc/process.h"

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

std::string g_self;

int failed = 0;
void check(bool ok, const char *what) {
    printf("%s%s\n", ok ? "ok    " : "FAIL  ", what);
    fflush(stdout);
    failed += !ok;
}

long my_pid() {
#ifdef _WIN32
    return (long)GetCurrentProcessId();
#else
    return (long)getpid();
#endif
}

// A program started the plain way (no group, not waited for): its number, 0 if it didn't start
long spawn(const std::vector<std::string> &argv) {
#ifdef _WIN32
    std::string cmd;
    for (const std::string &a : argv) cmd += (cmd.empty() ? "\"" : " \"") + a + "\"";
    STARTUPINFOA si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return 0;
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

bool alive(long pid) {
#ifdef _WIN32
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)pid);
    if (!h) return false;
    const bool yes = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return yes;
#else
    if (kill((pid_t)pid, 0) != 0) return false;
    // (ended and not yet waited for by whoever it was left to: not alive)
    std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    std::getline(stat, line);
    const size_t at = line.rfind(')');
    return !(at != std::string::npos && at + 2 < line.size() && line[at + 2] == 'Z');
#endif
}

void kill_outright(long pid) {
#ifdef _WIN32
    if (HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)pid)) {
        TerminateProcess(h, 9);
        CloseHandle(h);
    }
#else
    kill((pid_t)pid, SIGKILL);
    int status;
    waitpid((pid_t)pid, &status, 0);
#endif
}

template <class F> bool within(std::chrono::seconds limit, F &&f) {
    for (const auto until = std::chrono::steady_clock::now() + limit; std::chrono::steady_clock::now() < until;) {
        if (f()) return true;
        std::this_thread::sleep_for(20ms);
    }
    return f();
}

// The number a stand-in wrote of itself (0: not yet)
long pid_in(const fs::path &file) {
    std::ifstream in(file);
    long pid = 0;
    in >> pid;
    return pid;
}
void write_pid(const fs::path &file) {
    std::ofstream(fs::path(file).concat(".part")) << my_pid() << "\n";
    fs::rename(fs::path(file).concat(".part"), file);
}
[[noreturn]] void idle() {
    for (;;) std::this_thread::sleep_for(1h);
}

// The game and the program it started, once both have said their numbers
bool both(const fs::path &dir, const std::string &tag, long &game, long &started) {
    return within(60s, [&] { return (game = pid_in(dir / (tag + ".game"))) && (started = pid_in(dir / (tag + ".started"))); });
}

}  // namespace

int main(int argc, char **argv) {
#ifdef _WIN32
    char self[MAX_PATH];
    GetModuleFileNameA(nullptr, self, MAX_PATH);
    g_self = self;
#else
    g_self = fs::read_symlink("/proc/self/exe").string();
#endif
    const std::string mode = argc > 1 ? argv[1] : "";
    const fs::path dir = argc > 2 ? fs::path(argv[2]) : fs::path(g_self).concat(".d");
    const std::string tag = argc > 3 ? argv[3] : "";

    if (mode == "started") {   // a program the game started
        write_pid(dir / (tag + ".started"));
        idle();
    }
    if (mode == "game") {
        spawn({g_self, "started", dir.string(), tag});
        write_pid(dir / (tag + ".game"));
        idle();
    }
    if (mode == "quits") return 3;   // a game that ends by itself
    if (mode == "hiraku-pc") {
        hk::pc::GameProcess game;
        game.remove_too((dir / "link").string());
        if (!game.start({g_self, "game", dir.string(), tag}, dir.string())) return 1;
        idle();
    }

    fs::remove_all(dir);
    fs::create_directories(dir);

    // hiraku-pc killed outright
    {
        std::ofstream(dir / "link") << "the link\n";
        const long host = spawn({g_self, "hiraku-pc", dir.string(), "a"});
        long game = 0, started = 0;
        check(host && both(dir, "a", game, started) && alive(game) && alive(started), "hiraku-pc, its game and a program the game started: running");
        kill_outright(host);
        check(within(10s, [&] { return !alive(game); }), "hiraku-pc killed outright: the game ends");
        check(within(10s, [&] { return !alive(started); }), "... and the program it started");
#ifndef _WIN32
        check(within(10s, [&] { return !fs::exists(dir / "link"); }), "... and the link hiraku-pc would have removed is removed");
#endif
    }

    // two games, one stopped
    {
        hk::pc::GameProcess b, c;
        long b_game = 0, b_started = 0, c_game = 0, c_started = 0;
        const bool up = b.start({g_self, "game", dir.string(), "b"}, dir.string()) && c.start({g_self, "game", dir.string(), "c"}, dir.string());
        check(up && both(dir, "b", b_game, b_started) && both(dir, "c", c_game, c_started) && b.running() && c.running(), "two games: running");
        b.stop();
        check(!b.running() && !alive(b_game), "one stopped: ended by the time stop() is back");
        check(within(10s, [&] { return !alive(b_started); }), "... and the program it started");
        std::this_thread::sleep_for(300ms);
        check(c.running() && alive(c_game) && alive(c_started), "... and the other game goes on, with the program it started");

        // the stopped one started again (a new save folder's restart)
        long d_game = 0, d_started = 0;
        check(b.start({g_self, "game", dir.string(), "d"}, dir.string()) && both(dir, "d", d_game, d_started) && b.running(), "started again: running");
        b.stop();
        check(within(10s, [&] { return !alive(d_game) && !alive(d_started); }), "... and stopped again");
        check(c.running(), "... the other still going on");
        c.stop();
        check(within(10s, [&] { return !alive(c_game) && !alive(c_started); }), "the other stopped: ended");
    }

    // a game that ends by itself
    {
        hk::pc::GameProcess g;
        check(g.start({g_self, "quits"}, dir.string()), "a game that ends by itself: started");
        check(within(30s, [&] { return !g.running(); }), "... seen to have ended");
        unsigned long code = 0;
#ifdef _WIN32
        check(g.exit_code(code) && code == 3, "... with its exit code");
#else
        (void)code;
#endif
        g.stop();
        check(!g.running(), "... and stop() after it is harmless");
    }
    check(!hk::pc::GameProcess().start({(dir / "no-such-program").string()}, dir.string()), "a program that isn't there: not started");

    std::error_code ec;
    fs::remove_all(dir, ec);
    printf(failed ? "%d failed\n" : "all hold\n", failed);
    return failed ? 1 : 0;
}
