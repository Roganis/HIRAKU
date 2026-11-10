// SPDX-License-Identifier: GPL-3.0-or-later
// The PC game's process, as hiraku-pc keeps it (runtime/pc/core.cpp: Konami's program, under Wine on
// Linux). The game and whatever it starts are one group, and that group
//   - ends when hiraku-pc does, however hiraku-pc ends: stopped by the launcher's timer, a crash, the
//     system out of memory. A game left running alone can't be seen (its window is hidden) and keeps
//     its save folder and its ports: the next start would be a second game on the same saves;
//   - is stopped alone: other PC games on the same PC (two cabinets for NET TAISEN, in the one Wine
//     folder they share) go on.
// Linux: a process group, and a watcher (a shell on the other end of a socket only hiraku-pc holds)
// that kills the group when the socket closes. Windows: a job object that kills its processes when
// its last handle closes, hiraku-pc's.
#pragma once
#include <string>
#include <vector>

namespace hk::pc {

class GameProcess {
public:
    GameProcess() = default;
    GameProcess(const GameProcess &) = delete;
    GameProcess &operator=(const GameProcess &) = delete;
    ~GameProcess();   // stop()

    // A file to remove should hiraku-pc end without doing it (Linux: the link in /dev/shm). Before the
    // first start()
    void remove_too(const std::string &path) { remove_ = path; }
    // argv[0] is the program (Linux: looked for in PATH), run in dir. env (Linux): its environment,
    // nullptr-ended; nullptr: this process's. A game still running is stopped first. False: not started
    // (Linux: errno says why; Windows: GetLastError())
    bool start(const std::vector<std::string> &argv, const std::string &dir, char *const *env = nullptr);
    bool running();   // the program started is (not: what it started)
    void stop();      // the group ended, and waited for; nothing to do when none was started
    // Windows: the program's exit code once it has ended (0xc0000005 a fault...); false: not known
    bool exit_code(unsigned long &code);

private:
    std::string remove_;
#ifdef _WIN32
    void *job_ = nullptr, *process_ = nullptr, *thread_ = nullptr;   // (HANDLEs: <windows.h> stays out of headers)
#else
    bool watch();
    void tell(long group);
    int pid_ = 0, watcher_ = 0, watch_fd_ = -1;
    bool ended_ = false;   // the program has ended and been waited for (its group may not have)
#endif
};

}  // namespace hk::pc
