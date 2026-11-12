// SPDX-License-Identifier: GPL-3.0-or-later
#include "shell/sys.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#include <direct.h>

#include "shell/wintimer.h"
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__GLIBC__)
#include <execinfo.h>
#endif
#if defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#include <time.h>
#endif

extern char **environ;
#endif

namespace hk::sys {

namespace {
bool (*g_write_fault)(void *) = nullptr;
}

#ifdef _WIN32
// Each thread its timer (CREATE_WAITABLE_TIMER_HIGH_RESOLUTION: 2), made at its first wait and set
// here to end `ns` from now; null where Windows has none (before Windows 10 1803) or it can't be set
static HANDLE fine_timer(int64_t ns) {
    struct Fine {
        HANDLE h = CreateWaitableTimerExW(nullptr, nullptr, 2, TIMER_ALL_ACCESS);
        ~Fine() {
            if (h) CloseHandle(h);
        }
    };
    static thread_local Fine fine;
    LARGE_INTEGER due;
    due.QuadPart = -std::max<int64_t>(1, (ns + 99) / 100);   // from now, 100 ns units
    return fine.h && SetWaitableTimer(fine.h, &due, 0, nullptr, nullptr, FALSE) ? fine.h : nullptr;
}
#endif

void sleep_ns(int64_t ns) {
    if (ns <= 0) return;
#ifdef _WIN32
    if (HANDLE t = fine_timer(ns); t && WaitForSingleObject(t, INFINITE) == WAIT_OBJECT_0) return;
    Sleep((DWORD)std::min<int64_t>((ns + 999'999) / 1'000'000, 0x7fffffff));
#else
    std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
#endif
}

// Wake. Linux: a word and the kernel's wait on it (0 nothing, 1 signalled, 2 a thread asleep on
// it), so signal() is one exchange unless a thread is asleep. Windows: an event that resets itself,
// waited for together with the thread's fine timer. Elsewhere: a lock and a condition.
#if defined(__linux__)
struct Wake::Impl {
    std::atomic<uint32_t> word{0};
    long futex(int op, uint32_t value, const timespec *t) {
        return syscall(SYS_futex, reinterpret_cast<uint32_t *>(&word), op | FUTEX_PRIVATE_FLAG, value, t, nullptr, 0);
    }
};
void Wake::signal() {
    if (impl_->word.exchange(1, std::memory_order_acq_rel) == 2) impl_->futex(FUTEX_WAKE, 1, nullptr);
}
bool Wake::wait(int64_t ns) {
    uint32_t was = 1;
    if (impl_->word.compare_exchange_strong(was, 0, std::memory_order_acq_rel)) return true;
    if (ns <= 0) return false;
    was = 0;
    if (impl_->word.compare_exchange_strong(was, 2, std::memory_order_acq_rel)) {
        const timespec t{(time_t)(ns / 1'000'000'000), (long)(ns % 1'000'000'000)};
        impl_->futex(FUTEX_WAIT, 2, &t);   // (woken, the word no longer 2, the time up, or a signal handler: all one here)
    }
    return impl_->word.exchange(0, std::memory_order_acq_rel) == 1;
}
#elif defined(_WIN32)
struct Wake::Impl {
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ~Impl() {
        if (event) CloseHandle(event);
    }
};
void Wake::signal() {
    if (impl_->event) SetEvent(impl_->event);
}
bool Wake::wait(int64_t ns) {
    if (!impl_->event) return sleep_ns(ns), false;
    if (ns <= 0) return WaitForSingleObject(impl_->event, 0) == WAIT_OBJECT_0;
    if (HANDLE t = fine_timer(ns)) {
        const HANDLE both[2] = {impl_->event, t};
        return WaitForMultipleObjects(2, both, FALSE, INFINITE) == WAIT_OBJECT_0;
    }
    return WaitForSingleObject(impl_->event, (DWORD)std::min<int64_t>((ns + 999'999) / 1'000'000, 0x7fffffff)) == WAIT_OBJECT_0;
}
#else
struct Wake::Impl {
    std::mutex m;
    std::condition_variable c;
    bool set = false;
};
void Wake::signal() {
    {
        std::lock_guard<std::mutex> l(impl_->m);
        impl_->set = true;
    }
    impl_->c.notify_one();
}
bool Wake::wait(int64_t ns) {
    std::unique_lock<std::mutex> l(impl_->m);
    if (ns > 0) impl_->c.wait_for(l, std::chrono::nanoseconds(ns), [&] { return impl_->set; });
    return std::exchange(impl_->set, false);
}
#endif
Wake::Wake() : impl_(new Impl) {}
Wake::~Wake() { delete impl_; }

bool full_speed() {
#ifdef _WIN32
    return hk_win_full_speed() != 0;
#else
    return false;
#endif
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

namespace {
std::wstring widen(const std::string &utf8) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
    std::wstring w((size_t)std::max(n, 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), w.data(), n);
    return w;
}
std::string narrow(const std::wstring &w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)std::max(n, 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
// Can the code page write this, letter for letter?
bool fits(const std::wstring &w, UINT page) {
    bool ascii = true;
    for (wchar_t c : w) ascii = ascii && c < 0x80;
    if (ascii || page == CP_UTF8) return true;
    if (page == 20127) return false;   // (US-ASCII: plain letters only)
    BOOL lost = FALSE;
    return WideCharToMultiByte(page, WC_NO_BEST_FIT_CHARS, w.data(), (int)w.size(), nullptr, 0, nullptr, &lost) > 0 && !lost;
}
}  // namespace

unsigned ansi_code_page() {
    wchar_t number[8];
    if (!GetLocaleInfoW(LOCALE_SYSTEM_DEFAULT, LOCALE_IDEFAULTANSICODEPAGE, number, 8)) return GetACP();
    return (unsigned)_wtoi(number);
}

bool make_junction(const std::string &at, const std::string &to) {
    std::wstring a = widen(at), t = widen(to);
    for (wchar_t &c : a)
        if (c == L'/') c = L'\\';
    for (wchar_t &c : t)
        if (c == L'/') c = L'\\';
    const DWORD attr = GetFileAttributesW(a.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES) {
        if (!(attr & FILE_ATTRIBUTE_REPARSE_POINT)) return false;   // a folder of someone's own
        RemoveDirectoryW(a.c_str());                                // the junction, not what it leads to
    }
    if (!CreateDirectoryW(a.c_str(), nullptr)) return false;
    wchar_t full[MAX_PATH];
    const DWORD len = GetFullPathNameW(t.c_str(), MAX_PATH, full, nullptr);
    if (!len || len >= MAX_PATH) return RemoveDirectoryW(a.c_str()), false;
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
    HANDLE h = CreateFileW(a.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                           nullptr);
    DWORD got = 0;
    const bool ok = h != INVALID_HANDLE_VALUE && DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, &rp, 8 + rp.data_length, nullptr, 0, &got, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (!ok) RemoveDirectoryW(a.c_str());
    return ok;
}

std::string plain_path(const std::string &path, bool *ok, unsigned ways) {
    if (ok) *ok = true;
    // (4: plain letters only, for a program whose own libraries read names in yet another way)
    const UINT page = ways & 4 ? 20127 : ansi_code_page();
    std::wstring w = widen(path);
    for (wchar_t &c : w)
        if (c == L'/') c = L'\\';
    if (fits(w, page)) return path;
    const auto failed = [&] {
        if (ok) *ok = false;
        return path;
    };
    if (!(w.size() >= 3 && w[1] == L':' && w[2] == L'\\') && w.rfind(L"\\\\", 0) != 0) {   // from the current folder: whole first
        wchar_t full[1024];
        const DWORD n = GetFullPathNameW(w.c_str(), 1024, full, nullptr);
        if (!n || n >= 1024) return failed();
        w = full;
    }
    // its parts, after the drive (C:\) or the share (\\server\share\), which stay as they are
    size_t root = 0;
    if (w.size() >= 3 && w[1] == L':' && w[2] == L'\\') root = 3;
    else if (w.rfind(L"\\\\", 0) == 0) {
        const size_t server = w.find(L'\\', 2), share = server == std::wstring::npos ? server : w.find(L'\\', server + 1);
        if (share == std::wstring::npos) return failed();
        root = share + 1;
    } else return failed();   // (not a whole path: nothing to look its parts up in)
    if (!fits(w.substr(0, root), page)) return failed();
    struct Part {
        std::wstring real, plain;   // its name, and one the code page can write ("" if there is none)
    };
    std::vector<Part> parts;
    std::wstring so_far = w.substr(0, root);
    int last_without = -1;   // the last part with no such name
    for (size_t at = root; at < w.size();) {
        size_t end = w.find(L'\\', at);
        if (end == std::wstring::npos) end = w.size();
        if (end > at) {
            Part p{w.substr(at, end - at), {}};
            so_far += p.real;
            if (fits(p.real, page)) p.plain = p.real;
            else if (ways & 1) {
                WIN32_FIND_DATAW d;
                const HANDLE h = FindFirstFileW(so_far.c_str(), &d);
                if (h != INVALID_HANDLE_VALUE) {
                    FindClose(h);
                    if (d.cAlternateFileName[0] && fits(d.cAlternateFileName, page)) p.plain = d.cAlternateFileName;
                }
            }
            if (p.plain.empty()) last_without = (int)parts.size();
            parts.push_back(p);
            so_far += L'\\';
        }
        at = end + 1;
    }
    std::wstring out = w.substr(0, root);
    size_t from = 0;
    if (last_without >= 0) {
        // a link to the last folder that has no such name, and the rest from there
        if (!(ways & 2)) return failed();
        std::wstring folder = w.substr(0, root);
        for (int i = 0; i <= last_without; i++) folder += parts[(size_t)i].real + (i < last_without ? L"\\" : L"");
        const DWORD attr = GetFileAttributesW(folder.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) return failed();   // a file's own name: nothing to link
        wchar_t data[MAX_PATH];
        const DWORD n = GetEnvironmentVariableW(L"ProgramData", data, MAX_PATH);
        if (!n || n >= MAX_PATH || !fits(data, page)) return failed();
        uint64_t h = 0xcbf29ce484222325ull;   // FNV-1a of the folder's name, small letters: the same folder, the same link
        for (wchar_t c : folder) h = (h ^ (uint64_t)towlower(c)) * 0x100000001b3ull;
        wchar_t number[20];
        swprintf(number, 20, L"%016llx", (unsigned long long)h);
        const std::wstring links = std::wstring(data) + L"\\hiraku\\paths";
        CreateDirectoryW((std::wstring(data) + L"\\hiraku").c_str(), nullptr);
        CreateDirectoryW(links.c_str(), nullptr);
        const std::wstring link = links + L"\\" + number;
        if (!make_junction(narrow(link), narrow(folder))) return failed();
        out = link + L"\\";
        from = (size_t)last_without + 1;
    }
    for (size_t i = from; i < parts.size(); i++) out += parts[i].plain + (i + 1 < parts.size() ? L"\\" : L"");
    if (from == parts.size() && !out.empty() && out.back() == L'\\' && out.size() > 3) out.pop_back();
    std::string s = narrow(out);
    if (path.find('\\') == std::string::npos)   // as it came: with / between folders
        for (char &c : s)
            if (c == '\\') c = '/';
    return s;
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

namespace {
const char *g_crash_program = "hiraku";
// an address as its module's file name and the offset in it ("hiraku-pc.exe+1a2b3c")
void where(uintptr_t a, char *out, size_t n) {
    HMODULE m = nullptr;
    char path[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(a), &m) && m)
        GetModuleFileNameA(m, path, MAX_PATH);
    const char *base = std::max(strrchr(path, '\\'), strrchr(path, '/'));
    snprintf(out, n, "%s+%llx", base ? base + 1 : path, (unsigned long long)(a - reinterpret_cast<uintptr_t>(m)));
}
LONG WINAPI on_crash(EXCEPTION_POINTERS *e) {
    const EXCEPTION_RECORD *r = e->ExceptionRecord;
    char at[300];
    where(reinterpret_cast<uintptr_t>(r->ExceptionAddress), at, sizeof at);
    fprintf(stderr, "crash: %s (build %s): exception %08lx at %s", g_crash_program, build_id(), (unsigned long)r->ExceptionCode, at);
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2)
        fprintf(stderr, " (%s %p)", r->ExceptionInformation[0] == 1 ? "writing" : r->ExceptionInformation[0] == 8 ? "running" : "reading",
                reinterpret_cast<void *>(r->ExceptionInformation[1]));
    fprintf(stderr, "\n");
#if defined(_M_X64) || defined(__x86_64__)
    // the calls on the stack, unwound from the crash's own registers by the modules' unwind tables
    CONTEXT c = *e->ContextRecord;
    fprintf(stderr, "crash: the calls:");
    for (int i = 0; i < 24 && c.Rip; i++) {
        where(c.Rip, at, sizeof at);
        fprintf(stderr, " %s", at);
        DWORD64 base = 0;
        if (PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(c.Rip, &base, nullptr)) {
            void *handler_data = nullptr;
            DWORD64 frame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, f, &c, &handler_data, &frame, nullptr);
        } else {   // a leaf: its return address on top of the stack
            MEMORY_BASIC_INFORMATION mi;
            if (!c.Rsp || !VirtualQuery(reinterpret_cast<void *>(c.Rsp), &mi, sizeof mi) || mi.State != MEM_COMMIT) break;
            c.Rip = *reinterpret_cast<DWORD64 *>(c.Rsp);
            c.Rsp += 8;
        }
    }
    fprintf(stderr, "\n");
#endif
    fflush(stderr);
    fflush(stdout);
    return EXCEPTION_CONTINUE_SEARCH;   // the program ends as it would have (the launcher says why)
}
}  // namespace

void report_crashes(const char *program) {
    g_crash_program = program;
    SetUnhandledExceptionFilter(on_crash);
}

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

namespace {
const char *g_crash_program = "hiraku";
void on_crash(int sig, siginfo_t *si, void *) {
    // (stdio from a signal handler: the program is ending anyway)
    fprintf(stderr, "crash: %s (build %s): signal %d (%s) at %p\n", g_crash_program, build_id(), sig, strsignal(sig), si ? si->si_addr : nullptr);
#if defined(__GLIBC__)
    void *calls[32];
    const int n = backtrace(calls, 32);
    fprintf(stderr, "crash: the calls:\n");
    fflush(stderr);
    backtrace_symbols_fd(calls, n, 2);
#endif
    fflush(stderr);
    fflush(stdout);
    signal(sig, SIG_DFL);   // the fault again on return, now the system's: the program ends as it would have
    if (sig == SIGABRT) raise(sig);
}
}  // namespace

void report_crashes(const char *program) {
    g_crash_program = program;
    struct sigaction sa {};
    sa.sa_sigaction = on_crash;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) sigaction(sig, &sa, nullptr);
}

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

unsigned ansi_code_page() { return 0; }
std::string plain_path(const std::string &path, bool *ok, unsigned) {
    if (ok) *ok = true;
    return path;
}
bool make_junction(const std::string &, const std::string &) { return false; }

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
    if (out.good() && replace_file(to + ".part", to)) return true;
    std::remove((to + ".part").c_str());
    return false;
}

bool write_file(const std::string &path, const void *data, size_t n) {
    const std::string tmp = path + ".tmp";
    const char *bytes = static_cast<const char *>(data);
    bool ok = true;
#ifdef _WIN32
    HANDLE h = CreateFileA(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    for (size_t at = 0; ok && at < n;) {
        DWORD put = 0;
        ok = WriteFile(h, bytes + at, (DWORD)std::min<size_t>(n - at, 0x40000000), &put, nullptr) && put > 0;
        at += put;
    }
    ok = ok && FlushFileBuffers(h);
    ok = CloseHandle(h) && ok;
#else
    const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fd < 0) return false;
    for (size_t at = 0; ok && at < n;) {
        const ssize_t put = ::write(fd, bytes + at, n - at);
        if (put < 0 && errno == EINTR) continue;
        ok = put > 0;
        if (ok) at += (size_t)put;
    }
    ok = ok && fsync(fd) == 0;   // (a full disk can show only here, or at close)
    ok = close(fd) == 0 && ok;
#endif
    if (ok && replace_file(tmp, path)) return true;
    std::remove(tmp.c_str());
    return false;
}

FileLock::FileLock(const std::string &path) {
    if (path.empty()) return;
#ifdef _WIN32
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    OVERLAPPED at{};
    if (!LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &at)) {   // (waits)
        CloseHandle(h);
        return;
    }
    h_ = (intptr_t)h;
#else
    const int fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0666);
    if (fd < 0) return;
    while (flock(fd, LOCK_EX) != 0)
        if (errno != EINTR) {
            close(fd);
            return;
        }
    h_ = fd;
#endif
}

FileLock::~FileLock() {
    if (h_ == -1) return;
#ifdef _WIN32
    CloseHandle((HANDLE)h_);   // (the lock goes with the handle)
#else
    close((int)h_);
#endif
}

bool file_lock_held(const std::string &path) {
#ifdef _WIN32
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    OVERLAPPED at{};
    const bool held = !LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &at) && GetLastError() == ERROR_LOCK_VIOLATION;
    CloseHandle(h);   // (a lock taken goes with the handle)
    return held;
#else
    const int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) return false;
    int r;
    while ((r = flock(fd, LOCK_EX | LOCK_NB)) != 0 && errno == EINTR) {}
    const bool held = r != 0 && errno == EWOULDBLOCK;
    close(fd);
    return held;
#endif
}

Lock lock_file(const std::string &path) {
#ifdef _WIN32
    // (others may read it, none may write: a second process asking the same is refused; not inherited
    // by what this process starts)
    HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) return Lock::Taken;   // (kept open: Windows closes it with the process)
    return GetLastError() == ERROR_SHARING_VIOLATION ? Lock::Held : Lock::None;
#else
    // (close-on-exec: a program this one starts mustn't hold the lock on after it)
    const int fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0666);
    if (fd < 0) return Lock::None;
    while (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        if (errno == EINTR) continue;
        const bool held = errno == EWOULDBLOCK;
        close(fd);
        return held ? Lock::Held : Lock::None;
    }
    return Lock::Taken;   // (kept open: the system lets go of it with the process)
#endif
}

}  // namespace hk::sys
