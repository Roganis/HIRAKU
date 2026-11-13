// SPDX-License-Identifier: GPL-3.0-or-later
#include "sysload.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

#ifdef __linux__
#include <dirent.h>
#include <dlfcn.h>
#include <unistd.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <powrprof.h>
#include <tlhelp32.h>
#ifdef _MSC_VER
#pragma comment(lib, "pdh")
#pragma comment(lib, "powrprof")
#endif
#endif

namespace hk::host::sysload {

namespace {

// (never destroyed: the reading thread is still there when the program leaves)
std::mutex &g_mu = *new std::mutex;
std::condition_variable &g_cv = *new std::condition_variable;
bool g_on = false, g_started = false;
Now &g_now = *new Now;

uint64_t steady_ns() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

#ifdef __linux__
std::string read_file(const std::string &path) {
    std::string out;
    if (FILE *f = fopen(path.c_str(), "r")) {
        char b[4096];
        for (size_t n; (n = fread(b, 1, sizeof b, f)) > 0;) out.append(b, n);
        fclose(f);
    }
    return out;
}

std::vector<std::string> list_dir(const std::string &path) {
    std::vector<std::string> out;
    if (DIR *d = opendir(path.c_str())) {
        while (const dirent *e = readdir(d))
            if (e->d_name[0] != '.') out.push_back(e->d_name);
        closedir(d);
    }
    return out;
}

std::string link_target(const std::string &path) {
    char b[512];
    const ssize_t n = readlink(path.c_str(), b, sizeof b - 1);
    return n > 0 ? std::string(b, (size_t)n) : std::string();
}

// NVIDIA's driver says its load through its own library: loaded by name if it is installed
struct Nvml {
    bool tried = false;
    void *dev = nullptr;
    int (*util)(void *, unsigned *) = nullptr;
    float busy() {
        if (!tried) {
            tried = true;
            void *lib = dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
            if (!lib) return -1;
            auto init = (int (*)())dlsym(lib, "nvmlInit_v2");
            auto by_index = (int (*)(unsigned, void **))dlsym(lib, "nvmlDeviceGetHandleByIndex_v2");
            auto u = (int (*)(void *, unsigned *))dlsym(lib, "nvmlDeviceGetUtilizationRates");
            if (init && by_index && u && init() == 0 && by_index(0, &dev) == 0) util = u;
        }
        unsigned rates[2] = {};   // the card's, its memory's
        return util && dev && util(dev, rates) == 0 ? (float)rates[0] : -1.0f;
    }
} g_nvml;

struct Prev {
    uint64_t at = 0, engine_ns = 0, cycles = 0, total_cycles = 0;
    ThreadTimes threads;
};
Prev &g_prev = *new Prev;

void sample(Now &n) {
    const uint64_t at = steady_ns();
    const double wall = g_prev.at ? (double)(at - g_prev.at) : 0;
    // this program's threads
    ThreadTimes threads;
    for (const std::string &tid : list_dir("/proc/self/task")) {
        std::string name = read_file("/proc/self/task/" + tid + "/comm");
        while (!name.empty() && name.back() == '\n') name.pop_back();
        threads[strtoull(tid.c_str(), nullptr, 10)] = {name, parse_schedstat(read_file("/proc/self/task/" + tid + "/schedstat"))};
    }
    n.threads = busiest(g_prev.threads, threads, wall, 4);
    g_prev.threads = std::move(threads);
    // the processors' speed: the fastest one now
    n.cpu_mhz = 0;
    for (int cpu = 0; cpu < 512; cpu++) {
        const std::string f = read_file("/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cpufreq/scaling_cur_freq");
        if (f.empty()) {
            if (access(("/sys/devices/system/cpu/cpu" + std::to_string(cpu)).c_str(), F_OK) != 0) break;
            continue;
        }
        n.cpu_mhz = std::max(n.cpu_mhz, atoi(f.c_str()) / 1000);
    }
    // this program's hold on the card: its files under /dev/dri, each client once
    std::map<uint64_t, DrmClient> clients;
    for (const std::string &fd : list_dir("/proc/self/fd")) {
        if (link_target("/proc/self/fd/" + fd).compare(0, 9, "/dev/dri/") != 0) continue;
        const DrmClient c = parse_fdinfo(read_file("/proc/self/fdinfo/" + fd));
        if (c.drm) clients[c.id] = c;
    }
    uint64_t ns = 0, cycles = 0, total = 0;
    std::string pdev;
    for (const auto &[id, c] : clients) {
        ns += c.engine_ns, cycles += c.cycles, total = std::max(total, c.total_cycles);
        if (pdev.empty()) pdev = c.pdev;
    }
    n.gpu_own = -1;
    if (wall > 0 && !clients.empty()) {
        if (total > g_prev.total_cycles && cycles >= g_prev.cycles)   // (xe: cycles of the engine's own)
            n.gpu_own = (float)(100.0 * (double)(cycles - g_prev.cycles) / (double)(total - g_prev.total_cycles));
        else if (ns >= g_prev.engine_ns && (ns || g_prev.engine_ns))
            n.gpu_own = (float)std::min(100.0, 100.0 * (double)(ns - g_prev.engine_ns) / wall);
    }
    g_prev.at = at, g_prev.engine_ns = ns, g_prev.cycles = cycles, g_prev.total_cycles = total;
    n.gpu.clear(), n.gpu_busy = -1, n.gpu_mhz = 0;
    read_card("/sys", pdev, n);
    if (n.gpu == "NVIDIA") n.gpu_busy = g_nvml.busy();
}
#elif defined(_WIN32)
struct ProcessorPower {   // PROCESSOR_POWER_INFORMATION (no SDK header declares it)
    ULONG number, max_mhz, current_mhz, mhz_limit, max_idle_state, current_idle_state;
};

struct Prev {
    uint64_t at = 0;
    ThreadTimes threads;
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER counter = nullptr;
    bool tried = false;
};
Prev &g_prev = *new Prev;

void sample(Now &n) {
    const uint64_t at = steady_ns();
    const double wall = g_prev.at ? (double)(at - g_prev.at) : 0;
    g_prev.at = at;
    // this program's threads, by the names they gave themselves
    ThreadTimes threads;
    const DWORD pid = GetCurrentProcessId();
    using Describe = HRESULT(WINAPI *)(HANDLE, PWSTR *);
    static const Describe describe = [] {
        HMODULE k = GetModuleHandleW(L"kernelbase.dll");
        return k ? (Describe)(void *)GetProcAddress(k, "GetThreadDescription") : nullptr;
    }();
    if (HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te{};
        te.dwSize = sizeof te;
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != pid) continue;
            HANDLE t = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
            if (!t) continue;
            FILETIME c, e, k, u;
            if (GetThreadTimes(t, &c, &e, &k, &u)) {
                auto hns = [](const FILETIME &f) { return (uint64_t)f.dwHighDateTime << 32 | f.dwLowDateTime; };
                std::string name;
                PWSTR w = nullptr;
                if (describe && SUCCEEDED(describe(t, &w)) && w) {
                    char utf8[64] = {};
                    WideCharToMultiByte(CP_UTF8, 0, w, -1, utf8, sizeof utf8 - 1, nullptr, nullptr);
                    name = utf8;
                    LocalFree(w);
                }
                if (name.empty()) name = "thread " + std::to_string(te.th32ThreadID);
                threads[te.th32ThreadID] = {name, (hns(k) + hns(u)) * 100};
            }
            CloseHandle(t);
        }
        CloseHandle(snap);
    }
    n.threads = busiest(g_prev.threads, threads, wall, 4);
    g_prev.threads = std::move(threads);
    // the processors' speed, as the power manager has it
    n.cpu_mhz = 0;
    static ProcessorPower p[256];
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const ULONG cpus = std::min<ULONG>(si.dwNumberOfProcessors, 256);
    if (cpus && CallNtPowerInformation(ProcessorInformation, nullptr, 0, p, cpus * (ULONG)sizeof p[0]) == 0)
        for (ULONG i = 0; i < cpus; i++) n.cpu_mhz = std::max(n.cpu_mhz, (int)p[i].current_mhz);
    // the card: the 3D engines' use, every program's and this one's
    n.gpu.clear(), n.gpu_busy = n.gpu_own = -1, n.gpu_mhz = 0;
    if (!g_prev.tried) {
        g_prev.tried = true;
        if (PdhOpenQueryW(nullptr, 0, &g_prev.query) == ERROR_SUCCESS) {
            if (PdhAddEnglishCounterW(g_prev.query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &g_prev.counter) != ERROR_SUCCESS)
                g_prev.counter = nullptr;
            else PdhCollectQueryData(g_prev.query);   // (a rate: the first reading only starts it)
        }
        return;
    }
    if (!g_prev.counter || PdhCollectQueryData(g_prev.query) != ERROR_SUCCESS) return;
    DWORD size = 0, count = 0;
    if (PdhGetFormattedCounterArrayW(g_prev.counter, PDH_FMT_DOUBLE, &size, &count, nullptr) != (PDH_STATUS)PDH_MORE_DATA || !size) return;
    std::vector<uint8_t> buf(size);
    auto *items = (PDH_FMT_COUNTERVALUE_ITEM_W *)buf.data();
    if (PdhGetFormattedCounterArrayW(g_prev.counter, PDH_FMT_DOUBLE, &size, &count, items) != ERROR_SUCCESS) return;
    double all = 0, own = 0;
    for (DWORD i = 0; i < count; i++) {
        char name[256] = {};
        WideCharToMultiByte(CP_UTF8, 0, items[i].szName, -1, name, sizeof name - 1, nullptr, nullptr);
        unsigned long p_id = 0;
        std::string type;
        if (!parse_gpu_engine(name, p_id, type) || type != "3D") continue;
        all += items[i].FmtValue.doubleValue;
        if (p_id == pid) own += items[i].FmtValue.doubleValue;
    }
    n.gpu_busy = (float)std::min(100.0, all), n.gpu_own = (float)std::min(100.0, own);
}
#else
void sample(Now &) {}
#endif

void loop() {
    std::unique_lock<std::mutex> lock(g_mu);
    for (;;) {
        g_cv.wait_for(lock, std::chrono::seconds(1));
        if (!g_on) {
            g_cv.wait(lock, [] { return g_on; });
            continue;   // (a second from being asked: the first reading starts the rates)
        }
        Now n = g_now;
        lock.unlock();
        sample(n);
        lock.lock();
        n.n = g_now.n + 1;
        g_now = std::move(n);
    }
}

}  // namespace

#ifdef __linux__
bool read_card(const std::string &sys, const std::string &pdev, Now &n) {
    // the card named; with none named, one that says its load before NVIDIA's before any other
    std::string card;
    int best = 0;
    for (const std::string &name : list_dir(sys + "/class/drm")) {
        if (name.compare(0, 4, "card") != 0 || name.find('-') != std::string::npos) continue;   // (card1-DP-1: an output)
        const std::string dev = sys + "/class/drm/" + name + "/device";
        int rank = 1;
        if (!pdev.empty()) {
            const std::string to = link_target(dev);
            if (to.size() < pdev.size() || to.compare(to.size() - pdev.size(), pdev.size(), pdev) != 0) continue;
            rank = 4;
        } else if (!read_file(dev + "/gpu_busy_percent").empty()) rank = 3;
        else if (read_file(dev + "/vendor").compare(0, 6, "0x10de") == 0) rank = 2;
        if (rank > best) best = rank, card = name;
    }
    if (card.empty()) return false;
    const std::string base = sys + "/class/drm/" + card, dev = base + "/device";
    const std::string vendor = read_file(dev + "/vendor");
    n.gpu = vendor.compare(0, 6, "0x1002") == 0 ? "AMD" : vendor.compare(0, 6, "0x8086") == 0 ? "Intel"
          : vendor.compare(0, 6, "0x10de") == 0 ? "NVIDIA" : "";
    if (const std::string busy = read_file(dev + "/gpu_busy_percent"); !busy.empty()) n.gpu_busy = (float)atoi(busy.c_str());
    if (const std::string sclk = read_file(dev + "/pp_dpm_sclk"); !sclk.empty()) n.gpu_mhz = parse_sclk(sclk);
    else if (const std::string act = read_file(base + "/gt_act_freq_mhz"); !act.empty()) n.gpu_mhz = atoi(act.c_str());
    return true;
}
#endif

void run(bool on) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (on == g_on) return;
    g_on = on;
    if (on && !g_started) {
        g_started = true;
        std::thread(loop).detach();
    }
    if (!on) g_now = Now{};   // (what it read goes stale: none shown when asked again)
    g_cv.notify_all();
}

Now now() {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_now;
}

}  // namespace hk::host::sysload
