// SPDX-License-Identifier: GPL-3.0-or-later
// host/sysload.h's reading of what the system says, apart from where it is read: the texts' own
// forms (Linux's /proc and /sys files, Windows' counter names), so tests/host/test_statsview.cpp
// can give them made-up ones.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace hk::host::sysload {

// A program's hold on a graphics card, as Linux says it in /proc/<pid>/fdinfo/<fd> of a file under
// /dev/dri (the kernel's drm-usage-stats): the client's number (two files of one client count
// once), the card (its PCI address), and the time its engines worked for this client. amdgpu and
// i915 say nanoseconds; xe says cycles of the render engine against the engine's own total.
struct DrmClient {
    bool drm = false;
    uint64_t id = 0;
    std::string driver, pdev;
    uint64_t engine_ns = 0;
    uint64_t cycles = 0, total_cycles = 0;
};

inline DrmClient parse_fdinfo(const std::string &text) {
    DrmClient c;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        const size_t colon = line.find(':');
        if (colon == std::string::npos || line.compare(0, 4, "drm-") != 0) continue;
        const std::string key = line.substr(0, colon);
        std::string val = line.substr(colon + 1);
        val.erase(0, val.find_first_not_of(" \t"));
        while (!val.empty() && (val.back() == '\r' || val.back() == ' ')) val.pop_back();
        if (key == "drm-driver") c.driver = val, c.drm = true;
        else if (key == "drm-client-id") c.id = strtoull(val.c_str(), nullptr, 10);
        else if (key == "drm-pdev") c.pdev = val;
        else if (key.compare(0, 11, "drm-engine-") == 0 && key.compare(11, 9, "capacity-") != 0) {
            if (val.size() > 2 && val.compare(val.size() - 2, 2, "ns") == 0) c.engine_ns += strtoull(val.c_str(), nullptr, 10);
        } else if (key == "drm-cycles-rcs") c.cycles = strtoull(val.c_str(), nullptr, 10);
        else if (key == "drm-total-cycles-rcs") c.total_cycles = strtoull(val.c_str(), nullptr, 10);
    }
    return c;
}

// amdgpu's pp_dpm_sclk: the clock's levels, the one in use starred ("1: 1200Mhz *"): that one, MHz
inline int parse_sclk(const std::string &text) {
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        if (line.find('*') == std::string::npos) continue;
        const size_t colon = line.find(':');
        return atoi(line.c_str() + (colon == std::string::npos ? 0 : colon + 1));
    }
    return 0;
}

// A thread's schedstat: the nanoseconds it has run, first
inline uint64_t parse_schedstat(const std::string &text) { return strtoull(text.c_str(), nullptr, 10); }

// Windows' "GPU Engine" counter, an instance's name: "pid_4312_luid_0x0_0x1234_phys_0_eng_0_engtype_3D"
inline bool parse_gpu_engine(const std::string &name, unsigned long &pid, std::string &type) {
    if (name.compare(0, 4, "pid_") != 0) return false;
    pid = strtoul(name.c_str() + 4, nullptr, 10);
    const size_t at = name.find("engtype_");
    type = at == std::string::npos ? std::string() : name.substr(at + 8);
    return true;
}

// The busiest threads between two readings (a thread's id: its name and the nanoseconds it has
// run), over `wall_ns`: each one's share of one processor, the `most` largest of at least 1 %
struct Thread {
    std::string name;
    float share = 0;   // percent of one processor
};
using ThreadTimes = std::map<uint64_t, std::pair<std::string, uint64_t>>;
inline std::vector<Thread> busiest(const ThreadTimes &before, const ThreadTimes &now, double wall_ns, size_t most) {
    std::vector<Thread> out;
    if (wall_ns <= 0) return out;
    for (const auto &[id, t] : now) {
        const auto was = before.find(id);
        if (was == before.end() || t.second < was->second.second) continue;   // (new since: next time)
        const float share = (float)(100.0 * (double)(t.second - was->second.second) / wall_ns);
        if (share >= 1.0f) out.push_back({t.first, share});
    }
    std::sort(out.begin(), out.end(), [](const Thread &a, const Thread &b) { return a.share > b.share; });
    if (out.size() > most) out.resize(most);
    return out;
}

}  // namespace hk::host::sysload
