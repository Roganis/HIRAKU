// SPDX-License-Identifier: GPL-3.0-or-later
#include "shell/saves.h"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>

#include "shell/sys.h"

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace hk::saves {

namespace {

const char *const NEW = ".hk-new";            // a set's file, beside its place
const char *const LIST = "save-set.pending";  // the set's files, a line each: written = the set is the folder's

// The folder's own entry on the disk (the list's and the renames' order kept through a power cut)
void sync_dir(const std::string &dir) {
#ifndef _WIN32
    const int fd = open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return;
    fsync(fd);
    close(fd);
#else
    (void)dir;
#endif
}

// (a list's line names a file of this folder, nothing else)
bool plain_name(const std::string &n) {
    return !n.empty() && n != "." && n != ".." && n.find_first_of("/\\") == std::string::npos;
}

}  // namespace

uint64_t hash(const void *data, size_t n) {
    const auto *p = static_cast<const unsigned char *>(data);
    uint64_t h = 0x9e3779b97f4a7c15ULL ^ (uint64_t)n;
    auto mix = [&h](uint64_t w) {
        h = (h ^ w) * 0xff51afd7ed558ccdULL;
        h ^= h >> 29;
    };
    for (; n >= 8; p += 8, n -= 8) {
        uint64_t w;
        memcpy(&w, p, 8);
        mix(w);
    }
    uint64_t tail = 0;
    memcpy(&tail, p, n);
    mix(tail);
    h = (h ^ (h >> 32)) * 0xc4ceb9fe1a85ec53ULL;
    return h ^ (h >> 33);
}

bool write_set(const std::string &dir, const std::vector<File> &set) {
    if (set.empty()) return true;
    if (set.size() == 1) return sys::write_file(dir + "/" + set[0].name, set[0].data);   // (one file is whole by itself)
    auto drop = [&](size_t n) {
        for (size_t i = 0; i < n; i++) std::remove((dir + "/" + set[i].name + NEW).c_str());
    };
    // 1. each beside its place, on the disk
    std::string list;
    for (size_t i = 0; i < set.size(); i++) {
        if (!sys::write_file(dir + "/" + set[i].name + NEW, set[i].data)) {
            drop(i);
            return false;
        }
        list += set[i].name + "\n";
    }
    // 2. the list: from here the set is the folder's, and recover() finishes it if this doesn't
    if (!sys::write_file(dir + "/" + LIST, list)) {
        drop(set.size());
        return false;
    }
    sync_dir(dir);
    // 3. each into its place
    bool ok = true;
    for (const File &f : set) ok = sys::replace_file(dir + "/" + f.name + NEW, dir + "/" + f.name) && ok;
    // 4. done (a file that wouldn't go in keeps the list: the next start tries again)
    if (ok) std::remove((dir + "/" + LIST).c_str());
    return ok;
}

void recover(const std::string &dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    const std::string list_path = dir + "/" + LIST;
    if (fs::exists(list_path, ec)) {
        std::ifstream f(list_path, std::ios::binary);
        const std::string text{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
        f.close();
        std::istringstream lines(text);
        size_t put = 0;
        bool ok = true;
        for (std::string name; std::getline(lines, name);) {
            if (!plain_name(name) || !fs::exists(dir + "/" + name + NEW, ec)) continue;   // (already in its place)
            if (sys::replace_file(dir + "/" + name + NEW, dir + "/" + name)) put++;
            else ok = false;
        }
        if (ok) std::remove(list_path.c_str());
        printf("saves: the last save was cut short after it was whole on the disk: finished (%zu files)%s\n", put,
               ok ? "" : "; some couldn't be put in place");
        if (!ok) return;   // (their files stay for the next start)
    }
    // files of a set cut before its list: the folder's own are the whole ones
    std::vector<fs::path> stray;
    // (and what sys::write_file leaves of one of them, or of the list, when it is killed: NAME.tmp)
    auto ends = [](const std::string &name, const std::string &end) {
        return name.size() > end.size() && name.compare(name.size() - end.size(), end.size(), end) == 0;
    };
    for (const fs::directory_entry &e : fs::directory_iterator(dir, ec)) {
        const std::string name = e.path().filename().string();
        if (ends(name, NEW) || ends(name, std::string(NEW) + ".tmp") || name == std::string(LIST) + ".tmp") stray.push_back(e.path());
    }
    for (const fs::path &p : stray) fs::remove(p, ec);
    if (!stray.empty()) printf("saves: the last save was cut short before it was whole: the one before it is kept\n");
}

struct Writer::State {
    std::mutex m;
    std::condition_variable wake, idle;
    std::deque<std::pair<std::string, std::vector<File>>> queue;
    bool busy = false, stop = false;
    std::atomic<unsigned> failed{0};
    std::thread thread;

    void run() {
        std::unique_lock<std::mutex> lock(m);
        for (;;) {
            wake.wait(lock, [this] { return stop || !queue.empty(); });
            if (queue.empty()) return;   // (stop, and nothing left)
            auto job = std::move(queue.front());
            queue.pop_front();
            busy = true;
            lock.unlock();
            if (!write_set(job.first, job.second)) {
                failed++;
                fprintf(stderr, "saves: cannot write to %s: the save from before is kept (the next is tried as usual)\n", job.first.c_str());
            }
            lock.lock();
            busy = false;
            if (queue.empty()) idle.notify_all();
        }
    }
};

Writer::Writer() : s_(new State) {}

Writer::~Writer() {
    {
        std::lock_guard<std::mutex> lock(s_->m);
        s_->stop = true;
    }
    s_->wake.notify_all();
    if (s_->thread.joinable()) s_->thread.join();
    delete s_;
}

void Writer::write(std::string dir, std::vector<File> set) {
    if (set.empty()) return;
    std::lock_guard<std::mutex> lock(s_->m);
    if (!s_->thread.joinable()) s_->thread = std::thread([s = s_] { s->run(); });
    // A set still waiting for the same folder (the disk is slower than the sets come): the two are
    // one set, each file as the later has it. The later set holds what changed since the earlier
    // was taken, so together they are the later moment, whole; and no more than one set ever waits.
    if (!s_->queue.empty() && s_->queue.back().first == dir) {
        std::vector<File> &waiting = s_->queue.back().second;
        for (File &f : set) {
            bool there = false;
            for (File &w : waiting)
                if (w.name == f.name) w.data = std::move(f.data), there = true;
            if (!there) waiting.push_back(std::move(f));
        }
        return;
    }
    s_->queue.emplace_back(std::move(dir), std::move(set));
    s_->wake.notify_one();
}

void Writer::drain() {
    std::unique_lock<std::mutex> lock(s_->m);
    s_->idle.wait(lock, [this] { return s_->queue.empty() && !s_->busy; });
}

unsigned Writer::failed() const { return s_->failed.load(); }

Writer &writer() {
    static Writer w;
    return w;
}

}  // namespace hk::saves
