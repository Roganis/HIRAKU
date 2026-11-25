// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// A PC game's files read into the page cache ahead of the game, when its data is on a hard disk
// (Linux): the last 32-bit PC pop'n reads a song's preview on its frame thread as the song list
// moves (its sound pack data/sd/<folder>/<song>.ifs: the pack's header, then near its end), and
// each read cold on a hard disk is a seek: 15-35 ms a song, the list held up 2-4 frames.
// Read here behind everything (nice 19, the disk's idle class: the game's own reads go first),
// while memory allows; HK_PC_READ_AHEAD=0 leaves it out (test switch).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#ifdef __linux__
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#endif

namespace hk::pc {

// Whether `path` is on a rotational disk (its block device's, or its partition's parent's, queue/rotational)
inline bool on_hard_disk(const std::string &path) {
#ifdef __linux__
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;
    char dev[64];
    snprintf(dev, sizeof dev, "/sys/dev/block/%u:%u", major(st.st_dev), minor(st.st_dev));
    for (const char *q : {"/queue/rotational", "/../queue/rotational"})
        if (FILE *f = fopen((std::string(dev) + q).c_str(), "r")) {
            const int c = fgetc(f);
            fclose(f);
            return c == '1';
        }
#endif
    (void)path;
    return false;
}

// Each file's last `.second` bytes and its first 64 KB (a pack's header: the game reads it first), or
// (0) all of it, in order, in a thread of its own; up to a third of the memory free now
inline void read_ahead(std::vector<std::pair<std::string, uint64_t>> files, std::string what) {
#ifdef __linux__
    if (const char *e = getenv("HK_PC_READ_AHEAD"); e && *e == '0') return;
    std::thread([files = std::move(files), what = std::move(what)] {
        setpriority(PRIO_PROCESS, (id_t)gettid(), 19);
        syscall(SYS_ioprio_set, 1 /* IOPRIO_WHO_PROCESS */, (int)gettid(), 3 << 13 /* IOPRIO_CLASS_IDLE */);
        uint64_t room = 0, total = 0;
        if (FILE *mi = fopen("/proc/meminfo", "r")) {
            char line[256];
            unsigned long long kb;
            while (fgets(line, sizeof line, mi))
                if (sscanf(line, "MemAvailable: %llu kB", &kb) == 1) room = kb * 1024 / 3;
            fclose(mi);
        }
        std::vector<char> buf(256 << 10);
        int n = 0;
        for (const auto &[path, tail] : files) {
            const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
            struct stat st;
            if (fd < 0) continue;
            const uint64_t size = fstat(fd, &st) == 0 ? (uint64_t)st.st_size : 0, from = tail && tail < size ? size - tail : 0;
            if (total + (size - from) > room) {
                close(fd);
                break;
            }
            if (from > 0 && pread(fd, buf.data(), 64 << 10, 0) > 0) total += 64 << 10;   // (its header)
            for (uint64_t at = from; at < size;) {   // (pieces of 256 KB: a request of the game's waits behind one at most)
                const ssize_t got = pread(fd, buf.data(), buf.size(), (off_t)at);
                if (got <= 0) break;
                at += (uint64_t)got;
            }
            close(fd);
            total += size - from, n++;
        }
        fprintf(stderr, "hiraku-pc: %s read ahead: %d files, %.0f MB\n", what.c_str(), n, total / 1e6);
    }).detach();
#else
    (void)files, (void)what;
#endif
}

}  // namespace hk::pc
