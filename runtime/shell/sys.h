// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// The few things HIRAKU asks of the system that Linux and Windows do differently (PLAN.md section
// 26): page memory and its protection, reading a file at an offset, folders, the calendar. On
// Linux each is what the code called before; on Windows, Win32's equivalent. Kept out of the
// header: <windows.h>, whose macros (IN, OUT, ERROR, min, max...) collide with HIRAKU's names.
#include <fcntl.h>
#include <sys/types.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

#ifndef O_BINARY
#define O_BINARY 0   // Windows' open() translates line ends without it
#endif
#if defined(_MSC_VER) && !defined(__MINGW32__)
typedef intptr_t ssize_t;   // MSVC's C library has none (the Python core builds with clang-cl)
#endif

namespace hk::sys {

// This build: its commit and date ("37d10c2 2026-11-08"; "+" after the commit when built from changed
// files; "unknown" built without git), from cmake/build_id.cmake
const char *build_id();
// A crash's report (Windows: the exception, where, and the calls on the stack, each as module+offset;
// Linux: the signal and the stack) written to stderr before the program dies as it would have
void report_crashes(const char *program);

size_t page_size();
void *alloc_pages(size_t n);   // page-aligned, read-write; nullptr if there's no memory
void free_pages(void *p);
bool protect_pages(void *p, size_t n, bool writable);

// A write to a page protect_pages made read-only calls handler(the address written). It returns
// true when it has made the write possible (the write then runs again), false to let the fault
// go on as if nobody had caught it.
void catch_write_faults(bool (*handler)(void *address));

const void *map_file(int fd, uint64_t size);   // read-only; nullptr if it can't be mapped
void unmap_file(const void *p, uint64_t size);
ssize_t pread(int fd, void *buf, size_t n, uint64_t off);
void will_need(int fd, uint64_t off, uint64_t n);   // a hint: read that ahead
// The files read through once on a thread of their own, at a low priority (a .cue with its .bin), up
// to max_bytes in all: a machine's disk images then come from the system's cache, not the drive,
// when the emulation reads them (a cold read waited for on the emulation thread is a frozen frame)
void warm_files(const std::vector<std::string> &paths, uint64_t max_bytes);

bool make_dir(const std::string &path);   // true if it is there now
bool make_dirs(const std::string &path);  // and the folders it is in; true if it is there now
bool is_dir(const std::string &path);
bool exists(const std::string &path);     // a file or a folder
// from's bytes to `to`, whole or not at all (written beside it, then put in its place)
bool copy_file(const std::string &from, const std::string &to);
// rename(), and the file at `to` replaced if there is one (Windows' rename() fails then)
bool replace_file(const std::string &from, const std::string &to);
// A save: n bytes as the file at `path`, whole or not at all. Written beside it (path + ".tmp"), sent
// to the disk and checked, then put in its place: a full disk, a disk gone read-only or a kill half
// way leaves the file from before as it was. false then (and no .tmp left behind)
bool write_file(const std::string &path, const void *data, size_t n);
inline bool write_file(const std::string &path, const std::string &data) { return write_file(path, data.data(), data.size()); }
// The file at `path` (made if it isn't there) taken as this process's until the process ends, however
// it ends: a second process asking for the same file is told Held. For a folder only one program at a
// time may write in (a PC game's saves). None: the file can't be made, or this file system has no
// locks: nothing is known, and the caller goes on as before there were any
enum class Lock { Taken, Held, None };
Lock lock_file(const std::string &path);
// The file at `path` (made if it isn't there) held for as long as this object lives, by one holder
// at a time over every process and thread: the constructor waits for it. For a folder several
// programs read, change and write back (the e-AMUSEMENT servers on one data folder). A holder that
// dies lets go. "" or a file that can't be made: nothing is held, and the caller goes on as before
class FileLock {
public:
    explicit FileLock(const std::string &path);
    ~FileLock();
    FileLock(const FileLock &) = delete;
    FileLock &operator=(const FileLock &) = delete;
    bool held() const { return h_ != -1; }

private:
    intptr_t h_ = -1;
};
// Whether someone holds the file at `path` that way right now (a FileLock, or another program's lock
// of the same kind: tools/update.py's on a package while it puts an update in). Takes nothing, makes
// nothing; false when there is no such file
bool file_lock_held(const std::string &path);
std::string exe_path();   // this program's file, with / between folders; empty if unknown
// Runs a program (argv[0]: its file) with no window of its own and its output thrown away, and
// waits for it: its exit code, -1 if it couldn't be started
int run_quiet(const std::vector<std::string> &argv);
double thread_cpu_ms();   // the calling thread's CPU time
double process_cpu_ms();  // all of the process's threads' CPU time
long major_faults();      // the process's page faults that read the disk (0 where unknown)
// A wait, in place of std::this_thread's: the Windows build's (mingw's winpthreads) counts in whole
// milliseconds of a clock that moves every 15.6 ms, so sleep_for(100 us) doesn't wait at all (the
// loop around it spins) and sleep_for(2 ms) lasts 15.6 ms, whatever timer the process asked for
// (measured on Windows 11, 2026-11-12). Here Windows waits on a timer of its own that ends when
// asked, to half a millisecond (Windows 10 1803 on; before: Sleep, whole milliseconds and a tick);
// elsewhere it is the standard wait.
void sleep_ns(int64_t ns);
template <class Rep, class Period>
void sleep_for(std::chrono::duration<Rep, Period> d) {
    sleep_ns(std::chrono::duration_cast<std::chrono::nanoseconds>(d).count());
}
template <class Clock, class Duration>
void sleep_until(std::chrono::time_point<Clock, Duration> t) {
    sleep_for(t - Clock::now());
}
// Windows: the code page of the PC's programs that aren't Unicode ones (a PC game, and what HIRAKU
// puts inside or beside it), whatever this program's own is (UTF-8, by its manifest). 0 elsewhere.
unsigned ansi_code_page();
// A name for this file or folder (UTF-8, as every path of HIRAKU's) that such a program can write
// in its code page. Windows: the path itself when it can be written; else with the short names
// Windows keeps for the parts that can't (C:\Users\0E0F~1\...); else, on a drive that keeps
// none, through a link to the folder (a junction) made under ProgramData\hiraku\paths. `ok`
// false when no such name could be made (the path comes back as it was). `ways`: 1 short names,
// 2 links (tests take one away), 4 a name in plain letters only (ASCII), whatever the code page
// can write: for a program whose libraries read names their own way (AVS2). Elsewhere: the path
// itself.
std::string plain_path(const std::string &path, bool *ok = nullptr, unsigned ways = 3);
// Windows: a junction at `at` to the folder `to` (no rights needed, unlike a symbolic link); one
// already there is made again (`to` may have moved), a real folder there is left alone. True if
// `at` now leads to `to`. Elsewhere: false.
bool make_junction(const std::string &at, const std::string &to);

// One thread woken by another: wait() comes back when signal() has been called since the last
// wait came back (true), or after `ns` (false: a wait for nothing or less doesn't wait). signal()
// takes no lock and may be called from a sound device's thread; one thread waits at a time.
class Wake {
public:
    Wake();
    ~Wake();
    Wake(const Wake &) = delete;
    Wake &operator=(const Wake &) = delete;
    void signal();
    bool wait(int64_t ns);

private:
    struct Impl;
    Impl *impl_;
};
// Windows isn't to slow this process down, nor make its timer coarse, while its window is covered
// or minimized (Windows 11 may: shell/wintimer.h). True: told; nothing to tell elsewhere
bool full_speed();
std::tm utc(std::time_t t);
std::tm local(std::time_t t);
std::time_t from_utc(const std::tm &t);   // utc() backwards (timegm)

#ifdef _WIN32
// Win32 fibers, for the Firebeat core's guest threads (runtime/core/glue.cpp): Windows checks
// the stack a thread runs on against what the thread says its stack is, which a hand-made switch
// wouldn't update.
void *fiber_this_thread();                        // the calling thread as a fiber
void *fiber_new(size_t stack, void (*entry)());
void fiber_switch(void *to);
void fiber_delete(void *f);
#endif

}  // namespace hk::sys
