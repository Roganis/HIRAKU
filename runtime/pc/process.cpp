// SPDX-License-Identifier: GPL-3.0-or-later
// The PC game's process (process.h): a group that ends with hiraku-pc and is stopped alone.
#include "process.h"

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;
#endif

namespace hk::pc {

GameProcess::~GameProcess() {
    stop();
#ifdef _WIN32
    if (job_) CloseHandle((HANDLE)job_);
#else
    if (watch_fd_ >= 0) {
        close(watch_fd_);   // the watcher's end: it has no group to kill now, and goes
        int status;
        for (int i = 0; i < 100 && waitpid(watcher_, &status, WNOHANG) == 0; i++) {
            const timespec t{0, 2'000'000};
            nanosleep(&t, nullptr);
        }
    }
#endif
}

#ifdef _WIN32

// An argument as Windows' programs take their command line apart (CommandLineToArgvW's rules)
static std::string quoted(const std::string &a, bool always) {
    if (!always && !a.empty() && a.find_first_of(" \t\"") == std::string::npos) return a;
    std::string q = "\"";
    size_t slashes = 0;
    for (char c : a) {
        if (c == '\\') slashes++;
        else if (c == '"') q.append(slashes + 1, '\\'), slashes = 0;
        else slashes = 0;
        q += c;
    }
    q.append(slashes, '\\');
    return q + "\"";
}

bool GameProcess::start(const std::vector<std::string> &argv, const std::string &dir, char *const *) {
    stop();
    if (argv.empty()) return false;
    if (!job_) {
        job_ = CreateJobObjectA(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (job_ && !SetInformationJobObject((HANDLE)job_, JobObjectExtendedLimitInformation, &limits, sizeof limits))
            CloseHandle((HANDLE)job_), job_ = nullptr;
    }
    std::string cmd;
    for (size_t i = 0; i < argv.size(); i++) cmd += (i ? " " : "") + quoted(argv[i], i == 0);
    STARTUPINFOA si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    // (suspended until it is in the job: nothing it starts is outside)
    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, dir.empty() ? nullptr : dir.c_str(), &si, &pi))
        return false;
    if (!job_ || !AssignProcessToJobObject((HANDLE)job_, pi.hProcess)) {
        static bool said;
        if (!said) fprintf(stderr, "hiraku-pc: the game isn't tied to hiraku-pc (error %lu): it would go on if hiraku-pc were killed\n", GetLastError());
        said = true;
    }
    ResumeThread(pi.hThread);
    process_ = pi.hProcess, thread_ = pi.hThread;
    return true;
}

bool GameProcess::running() { return process_ && WaitForSingleObject((HANDLE)process_, 0) == WAIT_TIMEOUT; }

bool GameProcess::exit_code(unsigned long &code) {
    DWORD c = 0;
    if (!process_ || !GetExitCodeProcess((HANDLE)process_, &c) || c == STILL_ACTIVE) return false;
    code = c;
    return true;
}

void GameProcess::stop() {
    if (!process_) return;
    if (job_) TerminateJobObject((HANDLE)job_, 0);   // the game and what it started
    TerminateProcess((HANDLE)process_, 0);           // (the game, were it not in the job)
    WaitForSingleObject((HANDLE)process_, 5000);
    CloseHandle((HANDLE)process_), CloseHandle((HANDLE)thread_);
    process_ = thread_ = nullptr;
}

#else

// The watcher: a shell that reads the game's group from hiraku-pc (a line each time one starts, 0
// when it has been stopped) and, when hiraku-pc's end of the socket closes - hiraku-pc has ended,
// however - kills the last group named and removes the file. In a group of its own: a Ctrl-C in
// hiraku-pc's terminal isn't for it. (kill -KILL -GROUP: the one spelling bash, dash and busybox's
// shell all take; busybox's stumbles on "--")
bool GameProcess::watch() {
    if (watch_fd_ >= 0) return true;
    static const char script[] = "trap '' INT TERM HUP\n"
                                 "g=0\n"
                                 "while read -r x; do g=$x; done\n"
                                 "[ \"$g\" -gt 0 ] && kill -KILL \"-$g\"\n"
                                 "[ -n \"$1\" ] && rm -f -- \"$1\"\n";
    int s[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, s) != 0) return false;   // (close-on-exec: the game has neither end)
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, s[0], 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
#if defined(__GLIBC__) && __GLIBC_PREREQ(2, 34)
    posix_spawn_file_actions_addclosefrom_np(&fa, 3);   // none of hiraku-pc's other files (its window's, its sound's)
#endif
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&at, 0);
    const char *args[] = {"/bin/sh", "-c", script, "hiraku-pc-watch", remove_.c_str(), nullptr};
    pid_t pid = 0;
    const int r = posix_spawn(&pid, "/bin/sh", &fa, &at, (char *const *)args, environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    close(s[0]);
    if (r != 0) {
        close(s[1]);
        errno = r;
        return false;
    }
    watch_fd_ = s[1], watcher_ = pid;
    return true;
}

void GameProcess::tell(long group) {
    if (watch_fd_ < 0) return;
    char line[32];
    const int n = snprintf(line, sizeof line, "%ld\n", group);
    send(watch_fd_, line, (size_t)n, MSG_NOSIGNAL);   // (a watcher gone: nothing to tell, and no SIGPIPE)
}

bool GameProcess::start(const std::vector<std::string> &argv, const std::string &dir, char *const *env) {
    stop();
    if (argv.empty()) return false;
    if (!watch()) {
        static bool said;
        if (!said) fprintf(stderr, "hiraku-pc: no watcher for the game (%s): it would go on if hiraku-pc were killed\n", strerror(errno));
        said = true;
    }
    std::vector<char *> args;
    for (const std::string &a : argv) args.push_back((char *)a.c_str());
    args.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (!dir.empty()) posix_spawn_file_actions_addchdir_np(&fa, dir.c_str());
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);   // a group of its own, its number the program's
    posix_spawnattr_setpgroup(&at, 0);
    pid_t pid = 0;
    const int r = posix_spawnp(&pid, args[0], &fa, &at, args.data(), env ? env : environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    if (r != 0) {
        errno = r;
        return false;
    }
    pid_ = pid, ended_ = false;
    tell(pid_);
    return true;
}

bool GameProcess::running() {
    if (pid_ <= 0 || ended_) return false;
    int status;
    if (waitpid(pid_, &status, WNOHANG) == 0) return true;
    ended_ = true;   // (waited for: its number is no longer ours to signal, its group's still is)
    return false;
}

bool GameProcess::exit_code(unsigned long &) { return false; }

// The group asked to end, then (two seconds on, or at once when the program has ended) killed: what
// the program started isn't ours to wait for, and a Wine process ends at either
void GameProcess::stop() {
    if (pid_ <= 0) return;
    kill(-pid_, SIGTERM);
    for (int i = 0; i < 200 && running(); i++) {
        const timespec t{0, 10'000'000};
        nanosleep(&t, nullptr);
    }
    kill(-pid_, SIGKILL);
    if (!ended_) {
        int status;
        while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
    }
    tell(0);
    pid_ = 0, ended_ = false;
}

#endif

}  // namespace hk::pc
