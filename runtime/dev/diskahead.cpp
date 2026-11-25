// SPDX-License-Identifier: GPL-3.0-or-later
#include "diskahead.h"

#include "../core/prof.h"
#include "shell/sys.h"

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <unistd.h>
#ifdef _WIN32
#include <io.h>
#endif

namespace hk {

namespace {
// A read the game makes where nothing was read ahead (the first, or one somewhere else) is made in
// one go of at least this much, by the caller: a hard disk's sectors are then read in one request,
// not one page per 512-byte sector (the mapped image's), and the thread goes on from its end
constexpr uint64_t FIRST = 64 << 10;

uint64_t window_from_env() {
    const char *e = getenv("HK_DISK_AHEAD");
    const long mib = e && *e ? strtol(e, nullptr, 10) : 4;
    return mib > 0 ? (uint64_t)mib << 20 : 4ull << 20;
}

std::string base_name(const std::string &path) {
    auto slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}
}  // namespace

struct DiskAhead::State {
    std::string name;
    uint64_t size = 0, window = 0, piece = 0;
    bool on = false;
    int fd = -1;   // the thread's own (on Windows a handle of its own too: a synchronous handle's
                   // reads queue one behind the other, and the game's would wait for the thread's)
    mutable std::mutex mu;
    mutable std::condition_variable cv, caught_up;
    // the stream being followed: [from, done) read (by the thread or the game), the game's latest
    // read ending at `at`; gen changes when the game goes somewhere else (a piece read for the old
    // place then counts for nothing)
    uint64_t from = 0, done = 0, at = 0, gen = 0;
    uint64_t run = 0;   // bytes the game has read along this stream
    bool started = false, stop = false, waiting = false, printed = false;
    std::vector<char> first;   // the caller's buffer for FIRST
    Stats st;
    uint64_t t0 = 0, cur_off = 0;
    bool cur_hit = false;

    // how far to read: twice what the game has read along the stream so far, up to the window. Most
    // of a game's reads are a few KB somewhere (beatmania IIDX 8th style: 248 of 255 streams of
    // 8 KB, the rest 25 MB songs): 4 MB after each would be read for nothing, and the drive busy
    // with them when the game's next read comes
    uint64_t end() const { return std::min(at + std::min(window, 2 * run), size); }
    void follow();   // the thread
};

namespace {
std::mutex g_live_mu;
std::vector<std::weak_ptr<DiskAhead::State>> &live() {
    static auto *v = new std::vector<std::weak_ptr<DiskAhead::State>>;   // (never destroyed: read at exit)
    return *v;
}
void print_at_exit();
}  // namespace

void DiskAhead::State::follow() {
    std::vector<char> buf(piece);
    std::unique_lock<std::mutex> lk(mu);
    while (!stop) {
        if (!started || done >= end()) {
            waiting = true;
            caught_up.notify_all();
            cv.wait(lk);
            waiting = false;
            continue;
        }
        const uint64_t p = done, len = std::min(piece, end() - p), g = gen;
        lk.unlock();
        const ssize_t r = sys::pread(fd, buf.data(), (size_t)len, p);
        lk.lock();
        st.ahead_bytes += r > 0 ? (uint64_t)r : 0;
        // (a failed read counts as read: the game's own read of it will say)
        if (g == gen) done = std::max(done, p + len);
    }
    if (fd >= 0) ::close(fd);
    fd = -1;
    caught_up.notify_all();
}

bool DiskAhead::switched_on() {
    const char *e = getenv("HK_DISK_AHEAD");
    return !(e && e[0] == '0' && e[1] == 0);
}

DiskAhead::DiskAhead(const std::string &path, uint64_t size, uint64_t window, uint64_t piece, int on)
    : s_(std::make_shared<State>()) {
    s_->name = base_name(path);
    s_->size = size;
    s_->window = window ? window : window_from_env();
    s_->piece = std::max<uint64_t>(piece, 4096);
    s_->on = on < 0 ? switched_on() : on != 0;
    if (s_->on) {
        s_->fd = ::open(path.c_str(), O_RDONLY | O_BINARY);
        s_->on = s_->fd >= 0;
    }
    if (s_->on) {
#if !defined(_WIN32) && !defined(__APPLE__)
        // the kernel reads only what the thread asks: its own read-ahead would put MBs more on the
        // drive at once (btrfs reads ahead 4 MB), and a read of the game's arriving behind them waits
        posix_fadvise(s_->fd, 0, 0, POSIX_FADV_RANDOM);
#endif
        s_->first.resize(FIRST);
        std::thread([s = s_] { s->follow(); }).detach();   // (it holds the state until it ends)
    }
    std::lock_guard<std::mutex> lk(g_live_mu);
    static const bool hooked = (atexit(print_at_exit), true);
    (void)hooked;
    live().push_back(s_);
}

DiskAhead::~DiskAhead() {
    const std::string line = report();
    bool print = false;
    {
        std::lock_guard<std::mutex> lk(s_->mu);
        s_->stop = true;
        s_->cv.notify_all();
        print = !s_->printed && s_->st.reads;
        s_->printed = true;
    }
    if (print) fprintf(stderr, "%s\n", line.c_str());
}

bool DiskAhead::on() const { return s_->on; }

void DiskAhead::before(uint64_t off, uint64_t n) {
    State &s = *s_;
    const uint64_t t0 = prof::now_ns();
    bool fill = false;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        s.t0 = t0;
        s.cur_off = off;
        s.st.reads++;
        s.st.bytes += n;
        s.cur_hit = s.started && off >= s.from && off + n <= s.done;
        if (s.cur_hit) s.st.hit_bytes += n;
        if (!s.on) return;
        if (s.started && off >= s.from && off <= s.done) {
            // on along the stream (or back into it)
            s.at = std::max(s.at, off + n);
            s.run += n;
        } else if (s.started && off > s.done && off <= s.end()) {
            // a little further on (no further than it would have read ahead): the same stream, the
            // bytes skipped left out
            s.from = off;
            s.at = off + n;
            s.done = off;
            s.run += n;
        } else {
            // somewhere else: a new stream from here
            static const bool log = getenv("HK_DISK_AHEAD_LOG") != nullptr;
            if (log && s.started)
                fprintf(stderr, "disk ahead: %s: stream %.1f MiB + %llu KiB, then %.1f MiB\n", s.name.c_str(),
                        (double)s.from / 1048576.0, (unsigned long long)((s.at - s.from) >> 10), (double)off / 1048576.0);
            if (s.started) s.st.jumps++;
            s.started = true;
            s.gen++;
            s.from = s.done = off;
            s.at = off + n;
            s.run = n;
        }
        fill = !s.cur_hit && s.done < off + n;
        if (fill) {
            // read here, in one go, what the game is about to read and a little after; the thread
            // goes on from the end of it. (A piece of the thread's already on its way to these bytes:
            // this waits for it, as the game's own read would; the system reads them once.)
            s.done = std::max(s.done, std::min(s.size, off + std::max<uint64_t>(n, FIRST)));
        }
        if (s.waiting && s.done < s.end()) s.cv.notify_one();
    }
    if (fill) {
        // (outside the lock: the thread goes on meanwhile; the buffer is the caller's alone)
        for (uint64_t p = off, e = std::min(s.size, off + std::max<uint64_t>(n, FIRST)); p < e;) {
            const size_t k = (size_t)std::min<uint64_t>(e - p, s.first.size());
            if (sys::pread(s.fd, s.first.data(), k, p) <= 0) break;
            p += k;
        }
    }
}

void DiskAhead::after() {
    State &s = *s_;
    const uint64_t t1 = prof::now_ns();
    std::lock_guard<std::mutex> lk(s.mu);
    const uint64_t dt = t1 - s.t0;
    if (dt >= 4'000'000) s.st.over_4ms++, s.st.waited_ns += dt;
    if (dt >= 16'000'000) s.st.over_16ms++;
    if (dt >= 50'000'000) s.st.over_50ms++;
    if (dt > s.st.longest_ns) s.st.longest_ns = dt, s.st.longest_at = s.cur_off, s.st.longest_hit = s.cur_hit;
}

DiskAhead::Stats DiskAhead::stats() const {
    std::lock_guard<std::mutex> lk(s_->mu);
    return s_->st;
}

void DiskAhead::wait_caught_up() const {
    std::unique_lock<std::mutex> lk(s_->mu);
    if (!s_->on) return;
    s_->caught_up.wait(lk, [&] { return s_->stop || (s_->waiting && (!s_->started || s_->done >= s_->end())); });
}

namespace {
std::string report_of(const DiskAhead::State &s, const DiskAhead::Stats &t) {
    char b[640];
    const double mib = 1048576.0;
    if (s.on)
        snprintf(b, sizeof b,
                 "disk ahead: %s: %.1f MiB ahead; the game read %.1f MiB in %llu reads, %.0f %% of it read ahead "
                 "already; %llu jumps; %.1f MiB read ahead",
                 s.name.c_str(), (double)s.window / mib, (double)t.bytes / mib, (unsigned long long)t.reads,
                 t.bytes ? 100.0 * (double)t.hit_bytes / (double)t.bytes : 0.0, (unsigned long long)t.jumps,
                 (double)t.ahead_bytes / mib);
    else
        snprintf(b, sizeof b, "disk ahead: %s: off (HK_DISK_AHEAD=0: the old hints); the game read %.1f MiB in %llu reads",
                 s.name.c_str(), (double)t.bytes / mib, (unsigned long long)t.reads);
    std::string line = b;
    snprintf(b, sizeof b,
             "; the emulation waited on it: longest %.1f ms (at %.1f MiB, %s), %llu reads over 4 ms (%.0f ms in all), "
             "%llu over 16 ms, %llu over 50 ms",
             (double)t.longest_ns / 1e6, (double)t.longest_at / mib, t.longest_hit ? "read ahead" : "not read ahead",
             (unsigned long long)t.over_4ms, (double)t.waited_ns / 1e6, (unsigned long long)t.over_16ms,
             (unsigned long long)t.over_50ms);
    return line + b;
}

void print_at_exit() {
    std::vector<std::shared_ptr<DiskAhead::State>> all;
    {
        std::lock_guard<std::mutex> lk(g_live_mu);
        for (auto &w : live())
            if (auto s = w.lock()) all.push_back(s);
    }
    for (auto &s : all) {
        DiskAhead::Stats t;
        {
            std::lock_guard<std::mutex> lk(s->mu);
            if (s->printed || !s->st.reads) continue;
            s->printed = true;
            t = s->st;
        }
        fprintf(stderr, "%s\n", report_of(*s, t).c_str());
    }
}
}  // namespace

std::string DiskAhead::report() const { return report_of(*s_, stats()); }

}  // namespace hk
