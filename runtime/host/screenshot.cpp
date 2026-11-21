// SPDX-License-Identifier: GPL-3.0-or-later
// Screenshots: host/screenshot.h
#include "host/screenshot.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "host/png.h"
#include "shell/sys.h"

namespace hk::host::screenshot {

std::string file_name(const std::string &folder, const std::string &game, std::time_t when) {
    // (the recordings' names' shape: host/record.cpp, file_name)
    const std::tm t = hk::sys::local(when);
    char stamp[32];
    snprintf(stamp, sizeof stamp, "%04d%02d%02d-%02d%02d%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    const std::string base = folder + "/" + game + "-" + stamp;
    std::string p = base + ".png";
    for (int k = 2; hk::sys::exists(p) && k < 1000; k++) p = base + "-" + std::to_string(k) + ".png";
    return p;
}

double pixel_shape(double aspect, double cw, double ch, int w, int h) {
    // (as the recorder writes it in a file: host/record.cpp, its sar)
    if (aspect > 0 && cw > 0 && ch > 0 && w > 0 && h > 0) return aspect * cw / ch * h / w;
    return 1.0;
}

void shaped_size(int w, int h, double shape, int &W, int &H) {
    W = w, H = h;
    if (w <= 0 || h <= 0 || !(shape > 0) || !std::isfinite(shape)) return;
    if (shape > 1) W = (int)std::clamp(std::lround(w * shape), (long)w, (long)std::max(w, 8192));   // wider: columns
    else H = (int)std::clamp(std::lround(h / shape), (long)h, (long)std::max(h, 8192));             // taller: lines
}

namespace {

// One axis: n source pixels to m >= n, each `step` apart in memory, `count` such runs `stride`
// apart. The source's pixel i covers [i*m, (i+1)*m) and the output's j [j*n, (j+1)*n): output j is
// the mean of what lies under it, at most two source pixels (m >= n)
void stretch_axis(const uint32_t *in, uint32_t *out, int n, int m, size_t step_in, size_t step_out, int count,
                  size_t stride_in, size_t stride_out) {
    for (int r = 0; r < count; r++) {
        const uint32_t *src = in + (size_t)r * stride_in;
        uint32_t *dst = out + (size_t)r * stride_out;
        for (int j = 0; j < m; j++) {
            const int64_t a = (int64_t)j * n, b = a + n;   // (in units of 1/m of a source pixel... times n)
            const int i = (int)(a / m), i2 = (int)((b - 1) / m);
            const uint32_t p = src[(size_t)i * step_in];
            if (i2 == i) {   // within one source pixel
                dst[(size_t)j * step_out] = p;
                continue;
            }
            const uint32_t q = src[(size_t)i2 * step_in];
            const int64_t wp = (int64_t)i2 * m - a, wq = b - (int64_t)i2 * m;   // (wp + wq = n)
            uint32_t v = 0;
            for (int sh = 0; sh <= 16; sh += 8) {
                const int64_t c = ((int64_t)(p >> sh & 0xff) * wp + (int64_t)(q >> sh & 0xff) * wq + n / 2) / n;
                v |= (uint32_t)c << sh;
            }
            dst[(size_t)j * step_out] = v;
        }
    }
}

}  // namespace

std::vector<uint32_t> stretch(const uint32_t *rgb, int w, int h, int W, int H) {
    std::vector<uint32_t> out;
    if (!rgb || w <= 0 || h <= 0 || W < w || H < h) return out;
    std::vector<uint32_t> mid((size_t)W * h);
    if (W == w) std::copy(rgb, rgb + (size_t)w * h, mid.begin());
    else stretch_axis(rgb, mid.data(), w, W, 1, 1, h, (size_t)w, (size_t)W);   // each line's columns
    if (H == h) return mid;
    out.resize((size_t)W * H);
    stretch_axis(mid.data(), out.data(), h, H, (size_t)W, (size_t)W, W, 1, 1);   // each column's lines
    return out;
}

bool save(const uint32_t *rgb, int w, int h, double shape, const std::string &folder, const std::string &game,
          std::time_t when, std::string &out) {
    int W = w, H = h;
    shaped_size(w, h, shape, W, H);
    std::vector<uint32_t> shaped;
    if (rgb && (W != w || H != h)) {   // (square pixels: the frame as it is)
        shaped = stretch(rgb, w, h, W, H);
        rgb = shaped.data(), w = W, h = H;
    }
    const std::vector<uint8_t> file = png::encode(rgb, w, h);
    if (file.empty()) return out = "no picture to save (" + std::to_string(w) + "x" + std::to_string(h) + ")", false;
    if (!hk::sys::make_dirs(folder) || !hk::sys::is_dir(folder))
        return out = (hk::sys::exists(folder) ? folder + " is not a folder" : "can't make the folder " + folder), false;
    const std::string path = file_name(folder, game, when);
    errno = 0;
    // (beside it first, then put in its place: no half-written picture under its name)
    if (!hk::sys::write_file(path, file.data(), file.size())) {
#ifdef _WIN32
        return out = path + " couldn't be written", false;
#else
        return out = path + " couldn't be written" + (errno ? std::string(": ") + strerror(errno) : std::string()), false;
#endif
    }
    out = path;
    return true;
}

Saver::~Saver() {
    {
        std::unique_lock lock(m_);
        quit_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();   // (the queue emptied first: run() leaves only then)
}

void Saver::take(const uint32_t *rgb, int w, int h, double shape, const std::string &folder, const std::string &game, Done done) {
    Job j;
    if (rgb && w > 0 && h > 0) j.rgb.assign(rgb, rgb + (size_t)w * h);
    j.w = w, j.h = h, j.shape = shape, j.when = std::time(nullptr), j.folder = folder, j.game = game, j.done = std::move(done);
    {
        std::unique_lock lock(m_);
        jobs_.push_back(std::move(j));
        if (!thread_.joinable()) thread_ = std::thread([this] { run(); });
    }
    cv_.notify_all();
}

void Saver::wait() {
    std::unique_lock lock(m_);
    idle_.wait(lock, [&] { return jobs_.empty() && !busy_; });
}

void Saver::run() {
    std::unique_lock lock(m_);
    for (;;) {
        cv_.wait(lock, [&] { return quit_ || !jobs_.empty(); });
        if (jobs_.empty()) return;   // (quit_, nothing left)
        Job j = std::move(jobs_.front());
        jobs_.pop_front();
        busy_ = true;
        lock.unlock();
        std::string out;
        const bool ok = save(j.rgb.data(), j.w, j.h, j.shape, j.folder, j.game, j.when, out);
        if (j.done) j.done(ok, out);
        lock.lock();
        busy_ = false;
        idle_.notify_all();
    }
}

}  // namespace hk::host::screenshot
