// SPDX-License-Identifier: GPL-3.0-or-later
#include "shell/sys.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <fstream>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
#endif

namespace hk::sys {

namespace {
bool (*g_write_fault)(void *) = nullptr;
}

#ifdef _WIN32

size_t page_size() {
    static const size_t n = [] {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        return (size_t)si.dwPageSize;
    }();
    return n;
}

void *alloc_pages(size_t n) { return VirtualAlloc(nullptr, n ? n : 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); }

void free_pages(void *p) {
    if (p) VirtualFree(p, 0, MEM_RELEASE);
}

bool protect_pages(void *p, size_t n, bool writable) {
    DWORD old;
    return VirtualProtect(p, n, writable ? PAGE_READWRITE : PAGE_READONLY, &old) != 0;
}

namespace {
LONG CALLBACK on_fault(EXCEPTION_POINTERS *e) {
    const EXCEPTION_RECORD *r = e->ExceptionRecord;
    // an access violation's first parameter is 1 for a write, the second the address
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2 && r->ExceptionInformation[0] == 1 &&
        g_write_fault && g_write_fault((void *)r->ExceptionInformation[1]))
        return EXCEPTION_CONTINUE_EXECUTION;
    return EXCEPTION_CONTINUE_SEARCH;
}
}  // namespace

void catch_write_faults(bool (*handler)(void *)) {
    if (!g_write_fault) AddVectoredExceptionHandler(1, on_fault);
    g_write_fault = handler;
}

const void *map_file(int fd, uint64_t size) {
    if (!size) return nullptr;
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) return nullptr;
    HANDLE m = CreateFileMappingW(h, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!m) return nullptr;
    const void *p = MapViewOfFile(m, FILE_MAP_READ, 0, 0, (SIZE_T)size);
    CloseHandle(m);   // the view keeps the mapping
    return p;
}

void unmap_file(const void *p, uint64_t) {
    if (p) UnmapViewOfFile(p);
}

ssize_t pread(int fd, void *buf, size_t n, uint64_t off) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    OVERLAPPED o{};
    o.Offset = (DWORD)off;
    o.OffsetHigh = (DWORD)(off >> 32);
    DWORD got = 0;
    if (!ReadFile(h, buf, (DWORD)std::min<size_t>(n, 0x40000000), &got, &o))
        return GetLastError() == ERROR_HANDLE_EOF ? 0 : -1;
    return (ssize_t)got;
}

void will_need(int, uint64_t, uint64_t) {}

bool make_dir(const std::string &path) {
    struct _stat64 st;
    return _mkdir(path.c_str()) == 0 || (_stat64(path.c_str(), &st) == 0 && (st.st_mode & _S_IFDIR));
}

bool is_dir(const std::string &path) {
    struct _stat64 st;
    return _stat64(path.c_str(), &st) == 0 && (st.st_mode & _S_IFDIR);
}

bool exists(const std::string &path) {
    struct _stat64 st;
    return _stat64(path.c_str(), &st) == 0;
}

bool replace_file(const std::string &from, const std::string &to) {
    return MoveFileExA(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

std::string exe_path() {
    wchar_t w[32768];
    const DWORD n = GetModuleFileNameW(nullptr, w, 32768);
    if (!n || n >= 32768) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, w, (int)n, nullptr, 0, nullptr, nullptr);
    std::string s((size_t)len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, (int)n, s.data(), len, nullptr, nullptr);
    for (char &c : s)
        if (c == '\\') c = '/';
    return s;
}

int run_quiet(const std::vector<std::string> &argv) {
    if (argv.empty()) return -1;
    // one command line, each argument quoted as the C runtime reads it back
    std::wstring cmd;
    for (const std::string &a : argv) {
        const int n = MultiByteToWideChar(CP_UTF8, 0, a.c_str(), (int)a.size(), nullptr, 0);
        std::wstring w((size_t)n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, a.c_str(), (int)a.size(), w.data(), n);
        if (!cmd.empty()) cmd += L' ';
        cmd += L'"';
        size_t slashes = 0;
        for (wchar_t c : w) {
            if (c == L'\\') { slashes++; continue; }
            cmd.append(c == L'"' ? 2 * slashes + 1 : slashes, L'\\');
            slashes = 0;
            cmd += c;
        }
        cmd.append(2 * slashes, L'\\');
        cmd += L'"';
    }
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

double thread_cpu_ms() {
    FILETIME c, e, k, u;
    if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) return 0;
    auto ms = [](const FILETIME &f) { return (double)((uint64_t)f.dwHighDateTime << 32 | f.dwLowDateTime) / 1e4; };
    return ms(k) + ms(u);
}

long major_faults() { return 0; }   // Windows counts soft and hard faults together

std::tm utc(std::time_t t) {
    std::tm r{};
    gmtime_s(&r, &t);
    return r;
}

std::tm local(std::time_t t) {
    std::tm r{};
    localtime_s(&r, &t);
    return r;
}

std::time_t from_utc(const std::tm &t) {
    std::tm c = t;
    return _mkgmtime(&c);
}

void *fiber_this_thread() {
    return IsThreadAFiber() ? GetCurrentFiber() : ConvertThreadToFiber(nullptr);
}

namespace {
void WINAPI fiber_start(void *entry) { reinterpret_cast<void (*)()>(entry)(); }
}  // namespace

void *fiber_new(size_t stack, void (*entry)()) { return CreateFiber(stack, fiber_start, reinterpret_cast<void *>(entry)); }
void fiber_switch(void *to) { SwitchToFiber(to); }
void fiber_delete(void *f) { DeleteFiber(f); }

#else

size_t page_size() { return (size_t)sysconf(_SC_PAGESIZE); }

void *alloc_pages(size_t n) {
    const size_t page = page_size();
    return aligned_alloc(page, n ? (n + page - 1) & ~(page - 1) : page);
}

void free_pages(void *p) { free(p); }

bool protect_pages(void *p, size_t n, bool writable) {
    return mprotect(p, n, writable ? PROT_READ | PROT_WRITE : PROT_READ) == 0;
}

namespace {
struct sigaction g_old_segv {};

void on_segv(int sig, siginfo_t *si, void *uc) {
    if (g_write_fault && g_write_fault(si->si_addr)) return;   // the write runs again, now allowed
    // not ours: as if we weren't here
    sigaction(SIGSEGV, &g_old_segv, nullptr);
    if (g_old_segv.sa_flags & SA_SIGINFO) {
        if (g_old_segv.sa_sigaction) g_old_segv.sa_sigaction(sig, si, uc);
    } else if (g_old_segv.sa_handler != SIG_DFL && g_old_segv.sa_handler != SIG_IGN) {
        g_old_segv.sa_handler(sig);
    }
}
}  // namespace

void catch_write_faults(bool (*handler)(void *)) {
    g_write_fault = handler;
    struct sigaction sa {};
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &g_old_segv);
}

const void *map_file(int fd, uint64_t size) {
    void *p = size ? mmap(nullptr, (size_t)size, PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
    return p == MAP_FAILED ? nullptr : p;
}

void unmap_file(const void *p, uint64_t size) {
    if (p) munmap(const_cast<void *>(p), (size_t)size);
}

ssize_t pread(int fd, void *buf, size_t n, uint64_t off) { return ::pread(fd, buf, n, (off_t)off); }

void will_need(int fd, uint64_t off, uint64_t n) { posix_fadvise(fd, (off_t)off, (off_t)n, POSIX_FADV_WILLNEED); }

bool make_dir(const std::string &path) {
    struct stat st;
    return mkdir(path.c_str(), 0755) == 0 || (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode));
}

bool is_dir(const std::string &path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool exists(const std::string &path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

bool replace_file(const std::string &from, const std::string &to) { return std::rename(from.c_str(), to.c_str()) == 0; }

std::string exe_path() {
    char exe[4096];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    return n > 0 ? std::string(exe, (size_t)n) : std::string();
}

int run_quiet(const std::vector<std::string> &argv) {
    if (argv.empty()) return -1;
    std::vector<char *> args;
    for (const std::string &a : argv) args.push_back(const_cast<char *>(a.c_str()));
    args.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid;
    const int err = posix_spawn(&pid, args[0], &fa, nullptr, args.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (err) return -1;
    int st = 0;
    while (waitpid(pid, &st, 0) < 0)
        if (errno != EINTR) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

double thread_cpu_ms() {
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

long major_faults() {
    rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_majflt;
}

std::tm utc(std::time_t t) {
    std::tm r{};
    gmtime_r(&t, &r);
    return r;
}

std::tm local(std::time_t t) {
    std::tm r{};
    localtime_r(&t, &r);
    return r;
}

std::time_t from_utc(const std::tm &t) {
    std::tm c = t;
    return timegm(&c);
}

#endif

bool make_dirs(const std::string &path) {
    bool ok = false;
    for (size_t k = 1; k <= path.size(); k++)
        if (k == path.size() || path[k] == '/' || path[k] == '\\') ok = make_dir(path.substr(0, k));
    return ok;
}

void warm_files(const std::vector<std::string> &paths, uint64_t max_bytes) {
    std::vector<std::string> list;
    for (const std::string &p : paths) {
        if (p.empty()) continue;
        list.push_back(p);
        // a .cue's track file beside it, named the same (as ingest writes them)
        if (p.size() > 4 && (p.compare(p.size() - 4, 4, ".cue") == 0 || p.compare(p.size() - 4, 4, ".CUE") == 0))
            list.push_back(p.substr(0, p.size() - 4) + ".bin");
    }
    std::thread([list, max_bytes] {
#ifdef _WIN32
        // (not THREAD_MODE_BACKGROUND_BEGIN: it lowers the memory priority of the pages read too, the
        // first the system's cache lets go of)
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#else
        setpriority(PRIO_PROCESS, 0, 19);   // (this thread: Linux's nice is per thread)
#endif
        std::vector<char> buf(4 << 20);
        uint64_t total = 0;
        const auto t0 = std::chrono::steady_clock::now();
        for (const std::string &p : list) {
            FILE *f = fopen(p.c_str(), "rb");
            if (!f) continue;
            size_t n;
            while (total < max_bytes && (n = fread(buf.data(), 1, buf.size(), f)) > 0) total += n;
            fclose(f);
        }
        if (total)
            fprintf(stderr, "disk images: %.0f MB read into the system's cache in %.1f s (no waiting on the drive when the game reads them)\n",
                    (double)total / (1 << 20), std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    }).detach();
}

bool copy_file(const std::string &from, const std::string &to) {
    std::ifstream in(from, std::ios::binary);
    if (!in) return false;
    std::ofstream out(to + ".part", std::ios::binary | std::ios::trunc);
    out << in.rdbuf();
    out.close();
    return out.good() && replace_file(to + ".part", to);
}

}  // namespace hk::sys
