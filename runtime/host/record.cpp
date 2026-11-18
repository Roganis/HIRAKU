// SPDX-License-Identifier: GPL-3.0-or-later
// The game recorded to a video file: host/record.h
#include "host/record.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <thread>

#include "core/prof.h"
#include "shell/sys.h"

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef HK_WITH_RECORD
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
}
#endif

namespace hk::host::record {

std::string file_name(const std::string &folder, const std::string &game, const std::string &format) {
    const std::tm t = hk::sys::local(std::time(nullptr));
    char stamp[32];
    snprintf(stamp, sizeof stamp, "%04d%02d%02d-%02d%02d%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    const std::string ext = format == "h264" ? ".mp4" : ".mkv";
    const std::string base = folder + "/" + game + "-" + stamp;
    std::string p = base + ext;
    for (int k = 2; hk::sys::exists(p) && k < 1000; k++) p = base + "-" + std::to_string(k) + ext;
    return p;
}

// --- the mark over the window's picture -------------------------------------------------------
namespace {
std::atomic<bool> g_mark{false};

// R, E, C in 5x7 (a row a byte, the left pixel the high bit of five)
constexpr uint8_t kGlyphs[3][7] = {
    {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11},   // R
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F},   // E
    {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E},   // C
};

const std::vector<uint32_t> &mark_pixels(int &w, int &h) {
    static std::vector<uint32_t> px;
    static int pw = 0, ph = 0;
    if (px.empty()) {
        pw = 36, ph = 13;
        px.assign((size_t)pw * ph, 0xB0101010u);   // dark, partly see-through
        for (int y = 0; y < ph; y++)   // the dot: radius 4.5 at (7, 6)
            for (int x = 0; x < 14; x++) {
                const double dx = x - 6.5, dy = y - 6.0;
                if (dx * dx + dy * dy <= 4.6 * 4.6) px[(size_t)y * pw + x] = 0xFFE5262Au;
            }
        for (int g = 0; g < 3; g++)
            for (int y = 0; y < 7; y++)
                for (int x = 0; x < 5; x++)
                    if (kGlyphs[g][y] & (0x10 >> x)) px[(size_t)(y + 3) * pw + 15 + g * 7 + x] = 0xFFF2F2F2u;
    }
    w = pw, h = ph;
    return px;
}
}  // namespace

void set_mark(bool on) { g_mark = on; }

bool mark(Mark &m) {
    if (!g_mark.load(std::memory_order_relaxed)) return false;
    m.pixels = &mark_pixels(m.w, m.h);
    m.id = 1;
    return true;
}

#ifndef HK_WITH_RECORD
// --- a build without FFmpeg: nothing records -------------------------------------------------
bool available() { return false; }
std::string formats() { return "none (this build has no FFmpeg)"; }
struct Capture::Impl {};
Capture::Capture() = default;
Capture::~Capture() = default;
bool Capture::setup(const Setup &, std::string &error) {
    error = "this build can't record video (it was built without FFmpeg's libraries)";
    return false;
}
void Capture::set_wait(bool) {}
void Capture::stop_later(std::function<void(const std::string &, const std::string &)>) {}
void Capture::picture(const uint32_t *, int, int, double) {}
void Capture::sound(const float *, int) {}
bool Capture::start(const std::string &, std::string &error) {
    error = "this build can't record video";
    return false;
}
std::string Capture::stop() { return {}; }
bool Capture::on() const { return false; }
std::string Capture::path() const { return {}; }
Capture::Stats Capture::stats() const { return {}; }
#else

namespace {

// the H.264 encoders tried, in order (the first that opens here is taken)
constexpr const char *kH264[] = {"libx264", "h264_nvenc", "h264_amf", "h264_qsv", "h264_mf"};

std::string av_error(int e) {
    char b[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(e, b, sizeof b);
    return b;
}

// The memory free now, bytes (0 if unknown)
uint64_t memory_free() {
#ifdef _WIN32
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof ms;
    return GlobalMemoryStatusEx(&ms) ? (uint64_t)ms.ullAvailPhys : 0;
#else
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return 0;
    char line[256];
    unsigned long long kb = 0;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "MemAvailable: %llu kB", &kb) == 1) break;
    fclose(f);
    return (uint64_t)kb * 1024;
#endif
}

// The pixel formats an encoder takes
std::vector<AVPixelFormat> pixel_formats(const AVCodec *c) {
    const void *list = nullptr;
    int n = 0;
    std::vector<AVPixelFormat> out;
    if (avcodec_get_supported_config(nullptr, c, AV_CODEC_CONFIG_PIX_FORMAT, 0, &list, &n) >= 0 && list)
        for (int k = 0; k < n; k++) out.push_back(static_cast<const AVPixelFormat *>(list)[k]);
    return out;
}

// An H.264 encoder that opens on this PC, its name and pixel format ("" none)
std::string find_h264(AVPixelFormat &fmt) {
    struct Quiet {   // (an encoder that can't open here says why at length: not for the log)
        int was = av_log_get_level();
        Quiet() { av_log_set_level(AV_LOG_FATAL); }
        ~Quiet() { av_log_set_level(was); }
    } quiet;
    for (const char *name : kH264) {
        const AVCodec *c = avcodec_find_encoder_by_name(name);
        if (!c) continue;
        const std::vector<AVPixelFormat> pf = pixel_formats(c);
        fmt = std::find(pf.begin(), pf.end(), AV_PIX_FMT_YUV420P) != pf.end() ? AV_PIX_FMT_YUV420P
            : std::find(pf.begin(), pf.end(), AV_PIX_FMT_NV12) != pf.end()    ? AV_PIX_FMT_NV12
                                                                               : AV_PIX_FMT_NONE;
        if (fmt == AV_PIX_FMT_NONE) continue;
        AVCodecContext *x = avcodec_alloc_context3(c);
        x->width = 256, x->height = 224, x->pix_fmt = fmt, x->time_base = {1, 60}, x->framerate = {60, 1};
        x->bit_rate = 2'000'000;
        const int r = avcodec_open2(x, c, nullptr);   // (a graphics card's encoder fails here without its card)
        avcodec_free_context(&x);
        if (r >= 0) return name;
    }
    return {};
}

struct Item {
    bool video = false;
    std::vector<uint32_t> px;   // video: w x h
    int w = 0, h = 0;
    int64_t pts = 0;            // in frames of the file's rate, from the recording's first picture
    double cw = 1, ch = 1;      // the part of the screen it covers (Setup::cover, asked with the frame)
    std::vector<float> pcm;     // sound: stereo
    size_t bytes() const { return px.size() * 4 + pcm.size() * 4; }
};

// One file being written (a part: a recording's frames at one size)
struct Part {
    AVFormatContext *fmt = nullptr;
    AVCodecContext *venc = nullptr, *aenc = nullptr;
    AVStream *vs = nullptr, *as = nullptr;
    AVFrame *vf = nullptr, *af = nullptr;
    AVPacket *pkt = nullptr;
    int w = 0, h = 0;
    int sw = 0, sh = 0;         // the frames' size it was begun for (w, h: that, made even for 4:2:0)
    int64_t pts0 = 0;           // the recording's frame number this part starts at
    int64_t samples = 0;        // sound samples written (each channel)
    std::vector<float> pending; // sound not yet a whole encoder frame
    std::vector<uint32_t> canvas;   // h264: the frame at the part's size before conversion
    bool begun = false;             // its header written: packets may go in, a trailer ends it
};

}  // namespace

struct Capture::Impl {
    Setup s;
    std::string h264;                         // the H.264 encoder taken
    AVPixelFormat h264_fmt = AV_PIX_FMT_NONE;
    AVRational rate_q{0, 1};                  // the file's frame rate

    // the producer's (the core's thread)
    double t1 = -1, t2 = -1;                  // the last two pictures' seconds
    bool armed = false;                       // recording: from the next picture with two frame times before it
    double t0 = 0, rate = 0;
    int64_t last_pts = -1;
    bool said_drop = false;

    std::atomic<bool> live{false};            // the sound goes in
    std::string path;

    std::mutex m;
    std::condition_variable work, room;
    std::deque<Item> q;
    std::vector<std::vector<uint32_t>> pool;
    size_t q_bytes = 0, budget = 0, q_frames = 0;
    bool closing = false, failed = false, wait = false;
    std::thread worker;
    Stats st;
    std::vector<std::thread> finishing;       // recordings ended by stop_later, being finished

    // the worker's
    Part part;
    std::string error;

    void run();
    static constexpr int kSizeHeld = 30;      // frames a new size stays before it has a file of its own
    std::deque<Item> held;                    // from a frame of another size on, until that's decided
    int held_w = 0, held_h = 0, held_frames = 0;
    bool take(Item &&it);
    bool put_back_held();
    bool encode(Item &it);
    bool open_part(int w, int h, double cw, double ch);
    bool close_part();
    bool encode_video(const Item &it);
    bool encode_sound(const float *lr, size_t frames);
    bool send(AVCodecContext *c, AVStream *s, const AVFrame *f);
    std::string part_path(int n) const {
        if (n <= 1) return path;
        const size_t dot = path.rfind('.'), slash = path.find_last_of("/\\");
        if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return path + "-" + std::to_string(n);
        return path.substr(0, dot) + "-" + std::to_string(n) + path.substr(dot);
    }
};

bool available() { return true; }

std::string formats() {
    AVPixelFormat f;
    const std::string h = find_h264(f);
    return h.empty() ? "ffv1 (this build's FFmpeg has no H.264 encoder that opens here)" : "ffv1, h264 (" + h + ")";
}

Capture::Capture() : d(std::make_unique<Impl>()) {}
Capture::~Capture() { stop(); }

bool Capture::setup(const Setup &s, std::string &error) {
    d->s = s;
    d->wait = s.wait;
    if (s.format == "h264") {
        d->h264 = find_h264(d->h264_fmt);
        if (d->h264.empty()) {
            error = "--record-format h264: this build's FFmpeg has no H.264 encoder that works on this PC (ffv1 is always there)";
            return false;
        }
    } else if (s.format != "ffv1") {
        error = "--record-format " + s.format + ": not ffv1 or h264";
        return false;
    } else if (!avcodec_find_encoder(AV_CODEC_ID_FFV1) || !avcodec_find_encoder(AV_CODEC_ID_FLAC) || !av_guess_format("matroska", nullptr, nullptr)) {
        error = "this build's FFmpeg has no FFV1 or FLAC encoder, or no Matroska writer (tools/ffmpeg-min.sh builds them in)";
        return false;
    }
    if (!avio_find_protocol_name("file:x")) {
        error = "this build's FFmpeg can't write files (no file protocol: tools/ffmpeg-min.sh builds it in)";
        return false;
    }
    return true;
}

void Capture::set_wait(bool w) {
    std::lock_guard<std::mutex> l(d->m);
    d->wait = w;
    d->room.notify_all();
}

bool Capture::on() const { return d->armed; }
std::string Capture::path() const { return d->path; }
Capture::Stats Capture::stats() const { return d->st; }

bool Capture::start(const std::string &path, std::string &error) {
    if (d->armed) stop();
    if (const size_t slash = path.find_last_of("/\\"); slash != std::string::npos && slash > 0) hk::sys::make_dirs(path.substr(0, slash));
    FILE *f = fopen(path.c_str(), "wb");   // (can it be written: said now, not by the encoder's thread later)
    if (!f) {
        error = "can't write " + path;
        return false;
    }
    fclose(f);
    Impl &I = *d;
    I.path = path;
    I.st = Stats{};
    I.error.clear();
    I.part = Part{};
    I.held.clear();
    I.held_frames = 0;
    I.q.clear();
    I.q_bytes = I.q_frames = 0;
    I.closing = I.failed = false;
    I.last_pts = -1;
    I.said_drop = false;
    const uint64_t free_now = memory_free();
    I.budget = I.s.memory ? I.s.memory
                          : (size_t)std::clamp<uint64_t>(free_now / 2, 256ull << 20, 8ull << 30);
    I.armed = true;
    I.worker = std::thread([&I] { I.run(); });
    return true;
}

void Capture::picture(const uint32_t *rgb, int w, int h, double seconds) {
    Impl &I = *d;
    const double p1 = I.t1, p2 = I.t2;
    I.t2 = p1, I.t1 = seconds;
    if (!I.armed) return;
    if (!rgb || w <= 0 || h <= 0) return;
    if (!I.live.load(std::memory_order_relaxed)) {   // the file's first picture
        if (p2 < 0) return;   // (two frame times before it: the rate)
        const double d1 = seconds - p1, d2 = p1 - p2;
        double rate = I.s.native_rate > 0 ? I.s.native_rate : 60.0;
        if (d1 > 0 && d2 > 0 && std::fabs(d1 - d2) <= 1e-4 * d1 && 2.0 / (d1 + d2) > 10 && 2.0 / (d1 + d2) < 1000)
            rate = 2.0 / (d1 + d2);   // the machine's frames come steadily: their own rate
        I.rate_q = av_d2q(rate, 1'000'000);
        I.rate = av_q2d(I.rate_q);
        I.t0 = seconds;
        I.last_pts = -1;
        std::lock_guard<std::mutex> l(I.m);
        I.st.rate = I.rate;
        I.live = true;
    }
    const int64_t pts = std::llround((seconds - I.t0) * I.rate);
    if (pts <= I.last_pts) {   // within the last frame's time: faster than the file's rate
        std::lock_guard<std::mutex> l(I.m);
        I.st.too_soon++;
        return;
    }
    const size_t n = (size_t)w * h, bytes = n * 4;
    std::vector<uint32_t> buf;
    {
        std::unique_lock<std::mutex> l(I.m);
        if (I.failed) return;
        // waiting: a few frames ahead are enough (the game has no time to keep)
        const size_t limit = I.wait ? std::min<size_t>(I.budget, 48 * bytes) : I.budget;
        if (I.q_bytes + bytes > limit && !I.q.empty()) {
            if (!I.wait) {
                I.st.dropped++;
                if (!I.said_drop) {
                    I.said_drop = true;
                    fprintf(stderr, "record: the encoder is %zu frames (%zu MB) behind: frames left out from %.1f s in (the sound keeps its time)\n",
                            I.q.size(), I.q_bytes >> 20, seconds - I.t0);
                }
                return;
            }
            I.room.wait(l, [&] { return I.q_bytes + bytes <= std::min<size_t>(I.budget, 48 * bytes) || I.q.empty() || I.failed || !I.wait; });
            if (I.failed) return;
        }
        if (!I.pool.empty()) buf = std::move(I.pool.back()), I.pool.pop_back();
    }
    buf.resize(n);
    memcpy(buf.data(), rgb, bytes);
    Item it;
    it.video = true, it.px = std::move(buf), it.w = w, it.h = h, it.pts = pts;
    if (I.s.cover) I.s.cover(w, h, it.cw, it.ch);   // (now: a core's answer is its picture's of the moment)
    I.last_pts = pts;
    std::lock_guard<std::mutex> l(I.m);
    I.q_bytes += bytes;
    I.q.push_back(std::move(it));
    I.st.queue_high = std::max(I.st.queue_high, ++I.q_frames);
    I.st.queue_high_bytes = std::max(I.st.queue_high_bytes, I.q_bytes);
    I.work.notify_one();
}

void Capture::sound(const float *lr, int frames) {
    Impl &I = *d;
    if (!I.live.load(std::memory_order_relaxed) || frames <= 0) return;
    Item it;
    it.pcm.assign(lr, lr + 2 * (size_t)frames);
    std::lock_guard<std::mutex> l(I.m);
    if (I.closing || I.failed) return;
    I.q_bytes += it.bytes();
    I.q.push_back(std::move(it));
    I.work.notify_one();
}

void Capture::stop_later(std::function<void(const std::string &, const std::string &)> done) {
    if (!d->armed) return;
    auto next = std::make_unique<Impl>();   // (the setup, the frames' times and the threads finishing go on)
    next->s = d->s, next->wait = d->s.wait, next->h264 = d->h264, next->h264_fmt = d->h264_fmt;
    next->t1 = d->t1, next->t2 = d->t2;
    next->finishing = std::move(d->finishing);
    auto old = std::make_shared<Capture>();
    old->d.swap(d);   // (this one is now `next`'s, below)
    d = std::move(next);
    d->finishing.emplace_back([old, done] {
        const std::string line = old->stop();
        if (done) done(line, old->path());
    });
}

std::string Capture::stop() {
    Impl &I = *d;
    for (std::thread &t : I.finishing)
        if (t.joinable()) t.join();
    I.finishing.clear();
    if (!I.armed) return {};
    I.armed = false;
    I.live = false;
    size_t left;
    {
        std::lock_guard<std::mutex> l(I.m);
        I.closing = true;
        left = I.q.size();
        I.work.notify_all();
        I.room.notify_all();
    }
    if (left > 120) printf("record: finishing the %zu frames and sounds still queued\n", left);
    if (I.worker.joinable()) I.worker.join();
    I.pool.clear();
    I.pool.shrink_to_fit();
    const Stats &s = I.st;
    char line[1024];
    snprintf(line, sizeof line,
             "record: %s: %llu frames %dx%d at %.3f fps, %.2f s long, %.2f s of sound, %d file%s; left out: %llu (encoder behind), "
             "%llu (faster than the rate); queue at most %zu frames (%zu MB); encoder %.2f ms a frame, its thread's CPU %.2f ms a frame%s%s",
             I.path.c_str(), (unsigned long long)s.frames, s.w, s.h, s.rate, s.rate > 0 ? (double)s.length / s.rate : 0.0,
             I.s.audio_rate > 0 ? (double)s.samples / I.s.audio_rate : 0.0, s.parts, s.parts == 1 ? "" : "s",
             (unsigned long long)s.dropped, (unsigned long long)s.too_soon, s.queue_high, s.queue_high_bytes >> 20,
             s.frames ? s.encode_ms / (double)s.frames : 0.0, s.frames ? s.worker_cpu_ms / (double)s.frames : 0.0,
             I.error.empty() ? "" : "; FAILED: ", I.error.c_str());
    return line;
}

// --- the encoder's thread --------------------------------------------------------------------

void Capture::Impl::run() {
    const double cpu0 = hk::sys::thread_cpu_ms();
    for (;;) {
        Item it;
        {
            std::unique_lock<std::mutex> l(m);
            work.wait(l, [&] { return !q.empty() || closing; });
            if (q.empty()) break;
            it = std::move(q.front());
            q.pop_front();
            q_bytes -= it.bytes();
            if (it.video) q_frames--;
            room.notify_all();
        }
        if (failed) continue;
        const uint64_t t = prof::now_ns();
        const bool ok = take(std::move(it));
        const double ms = (double)(prof::now_ns() - t) / 1e6;
        std::lock_guard<std::mutex> l(m);
        st.encode_ms += ms;
        if (!ok) {
            failed = true;
            fprintf(stderr, "record: %s: %s; recording stopped\n", path.c_str(), error.c_str());
            room.notify_all();
        }
    }
    if (!failed && !held.empty() && !put_back_held()) fprintf(stderr, "record: %s: %s\n", path.c_str(), error.c_str());
    if (part.fmt && !close_part()) fprintf(stderr, "record: %s: %s\n", path.c_str(), error.c_str());
    std::lock_guard<std::mutex> l(m);
    st.worker_cpu_ms = hk::sys::thread_cpu_ms() - cpu0;
}

// An item in order, a frame of another size than the file's held back (with what comes after it)
// until that size has stayed kSizeHeld frames: then a file at it begins with them; else they go
// into this file, scaled
bool Capture::Impl::take(Item &&it) {
    if (held.empty()) {
        if (!it.video || !part.begun || (it.w == part.sw && it.h == part.sh)) return encode(it);
        held_w = it.w, held_h = it.h, held_frames = 1;
        held.push_back(std::move(it));
        return true;
    }
    if (!it.video) {
        held.push_back(std::move(it));
        return true;
    }
    if (it.w == held_w && it.h == held_h) {
        held.push_back(std::move(it));
        if (++held_frames < kSizeHeld) return true;
        // the new size stays: the next file, at it, from its first frame (the sound after it with it)
        const Item *first = nullptr;
        for (const Item &h : held)
            if (h.video) { first = &h; break; }
        if (!close_part() || !open_part(held_w, held_h, first->cw, first->ch)) return false;
        part.pts0 = first->pts;
        return put_back_held();
    }
    // back to the file's size (or yet another): what was held goes into this file, scaled
    return put_back_held() && take(std::move(it));
}

bool Capture::Impl::put_back_held() {
    std::deque<Item> h;
    h.swap(held);
    held_frames = 0;
    for (Item &i : h)
        if (!encode(i)) return false;
    return true;
}

// One item into the file (the first picture begins it)
bool Capture::Impl::encode(Item &it) {
    if (!it.video) return part.begun ? encode_sound(it.pcm.data(), it.pcm.size() / 2) : true;
    bool ok = true;
    if (!part.fmt) {
        ok = open_part(it.w, it.h, it.cw, it.ch);
        if (ok) part.pts0 = it.pts;
    }
    ok = ok && encode_video(it);
    std::lock_guard<std::mutex> l(m);
    if (pool.size() < 8) pool.push_back(std::move(it.px));
    return ok;
}

bool Capture::Impl::open_part(int w, int h, double cw, double ch) {
    const bool h264v = s.format == "h264";
    Part p;
    p.sw = w, p.sh = h;
    if (h264v) w = (w + 1) & ~1, h = (h + 1) & ~1;   // (4:2:0: even sizes)
    p.w = w, p.h = h;
    const std::string file = part_path(st.parts + 1);
    int e = avformat_alloc_output_context2(&p.fmt, nullptr, h264v ? "mp4" : "matroska", file.c_str());
    if (e < 0 || !p.fmt) return error = "no " + std::string(h264v ? "MP4" : "Matroska") + " writer: " + av_error(e), false;
    p.fmt->flags |= AVFMT_FLAG_BITEXACT;   // (no date or random id: a run's file the same every time)
    part = p;   // (closed by close_part from here on, whatever fails)
    Part &P = part;

    // the picture
    const AVCodec *vc = h264v ? avcodec_find_encoder_by_name(h264.c_str()) : avcodec_find_encoder(AV_CODEC_ID_FFV1);
    P.venc = avcodec_alloc_context3(vc);
    AVCodecContext *v = P.venc;
    v->width = w, v->height = h;
    v->time_base = {rate_q.den, rate_q.num};
    v->framerate = rate_q;
    double sar = 1.0;   // (cw, ch: the part of the screen a frame of this size covers)
    if (s.aspect > 0 && cw > 0 && ch > 0) sar = s.aspect * cw / ch * h / w;
    v->sample_aspect_ratio = av_d2q(sar, 1000);
    v->flags |= AV_CODEC_FLAG_BITEXACT;
    if (h264v) {
        v->pix_fmt = h264_fmt;
        v->color_range = AVCOL_RANGE_MPEG;
        v->colorspace = AVCOL_SPC_SMPTE170M, v->color_primaries = AVCOL_PRI_SMPTE170M, v->color_trc = AVCOL_TRC_SMPTE170M;
        v->gop_size = (int)std::lround(rate_q.num / (double)rate_q.den * 2);   // a key frame every 2 s
        v->max_b_frames = 0;
        if (h264 == "libx264") {
            av_opt_set(v->priv_data, "preset", "veryfast", 0);
            av_opt_set(v->priv_data, "crf", "14", 0);
        } else {
            v->bit_rate = std::max<int64_t>(4'000'000, (int64_t)((double)w * h * av_q2d(rate_q) * 0.5));
        }
    } else {
        v->pix_fmt = AV_PIX_FMT_0RGB32;   // the cores' 0x00RRGGBB as it is: no colour conversion
        v->level = 3;
        v->gop_size = 1;
        v->slices = 4;
        // slices on FFmpeg's own threads: pop'n music 7's 640x480 in 1.6 ms a frame with 4 (4.2 with
        // one), for about 10 % more processor time in all
        v->thread_count = (int)std::clamp(std::thread::hardware_concurrency() / 4, 1u, 4u);
        v->thread_type = FF_THREAD_SLICE;
        av_opt_set_int(v->priv_data, "slicecrc", 1, 0);
    }
    if (P.fmt->oformat->flags & AVFMT_GLOBALHEADER) v->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if ((e = avcodec_open2(v, vc, nullptr)) < 0) return error = "the video encoder doesn't open: " + av_error(e), false;
    P.vs = avformat_new_stream(P.fmt, nullptr);
    avcodec_parameters_from_context(P.vs->codecpar, v);
    P.vs->time_base = v->time_base;
    P.vs->avg_frame_rate = P.vs->r_frame_rate = rate_q;
    P.vs->sample_aspect_ratio = v->sample_aspect_ratio;

    // the sound
    const AVCodec *ac = avcodec_find_encoder(h264v ? AV_CODEC_ID_AAC : AV_CODEC_ID_FLAC);
    if (!ac) return error = "no sound encoder", false;
    P.aenc = avcodec_alloc_context3(ac);
    AVCodecContext *a = P.aenc;
    a->sample_rate = s.audio_rate;
    av_channel_layout_default(&a->ch_layout, 2);
    a->sample_fmt = h264v ? AV_SAMPLE_FMT_FLTP : AV_SAMPLE_FMT_S16;
    a->time_base = {1, s.audio_rate};
    a->flags |= AV_CODEC_FLAG_BITEXACT;
    if (h264v) a->bit_rate = 256'000;
    if (P.fmt->oformat->flags & AVFMT_GLOBALHEADER) a->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if ((e = avcodec_open2(a, ac, nullptr)) < 0) return error = "the sound encoder doesn't open: " + av_error(e), false;
    P.as = avformat_new_stream(P.fmt, nullptr);
    avcodec_parameters_from_context(P.as->codecpar, a);
    P.as->time_base = a->time_base;

    P.vf = av_frame_alloc();
    P.vf->format = v->pix_fmt, P.vf->width = w, P.vf->height = h;
    P.vf->sample_aspect_ratio = v->sample_aspect_ratio;
    if (h264v) {
        P.vf->color_range = v->color_range, P.vf->colorspace = v->colorspace;
        P.vf->color_primaries = v->color_primaries, P.vf->color_trc = v->color_trc;
    }
    if ((e = av_frame_get_buffer(P.vf, 0)) < 0) return error = "no memory for a frame", false;
    P.af = av_frame_alloc();
    P.pkt = av_packet_alloc();

    if ((e = avio_open(&P.fmt->pb, file.c_str(), AVIO_FLAG_WRITE)) < 0) return error = "can't write " + file + ": " + av_error(e), false;
    AVDictionary *mux = nullptr;
    if (h264v) av_dict_set(&mux, "movflags", "+faststart", 0);
    e = avformat_write_header(P.fmt, &mux);
    av_dict_free(&mux);
    if (e < 0) return error = "can't begin " + file + ": " + av_error(e), false;
    P.begun = true;
    std::lock_guard<std::mutex> l(m);
    st.parts++;
    st.w = w, st.h = h;
    if (st.parts > 1) printf("record: the picture %dx%d from here: on in %s\n", w, h, file.c_str());
    return true;
}

// a frame (nullptr: the end) to an encoder, and its packets to the file
bool Capture::Impl::send(AVCodecContext *c, AVStream *st_, const AVFrame *f) {
    int e = avcodec_send_frame(c, f);
    if (e < 0 && e != AVERROR_EOF) return error = "encoding: " + av_error(e), false;
    for (;;) {
        e = avcodec_receive_packet(c, part.pkt);
        if (e == AVERROR(EAGAIN) || e == AVERROR_EOF) return true;
        if (e < 0) return error = "encoding: " + av_error(e), false;
        av_packet_rescale_ts(part.pkt, c->time_base, st_->time_base);
        part.pkt->stream_index = st_->index;
        if ((e = av_interleaved_write_frame(part.fmt, part.pkt)) < 0) return error = "writing: " + av_error(e), false;
    }
}

bool Capture::Impl::encode_video(const Item &it) {
    Part &P = part;
    if (av_frame_make_writable(P.vf) < 0) return error = "no memory for a frame", false;
    const bool h264v = s.format == "h264";
    // the frame at the part's size: as it is, or a smaller one scaled up to it (nearest pixels)
    uint32_t *dst;
    size_t stride;
    if (h264v) {
        P.canvas.resize((size_t)P.w * P.h);
        dst = P.canvas.data(), stride = (size_t)P.w;
    } else {
        dst = reinterpret_cast<uint32_t *>(P.vf->data[0]), stride = (size_t)P.vf->linesize[0] / 4;
    }
    if (it.w == P.w && it.h == P.h) {
        for (int y = 0; y < P.h; y++) memcpy(dst + y * stride, it.px.data() + (size_t)y * it.w, (size_t)P.w * 4);
    } else {
        std::vector<int> xs((size_t)P.w);
        for (int x = 0; x < P.w; x++) xs[(size_t)x] = (int)((int64_t)x * it.w / P.w);
        for (int y = 0; y < P.h; y++) {
            const uint32_t *row = it.px.data() + (size_t)((int64_t)y * it.h / P.h) * it.w;
            uint32_t *o = dst + y * stride;
            for (int x = 0; x < P.w; x++) o[x] = row[xs[(size_t)x]];
        }
    }
    if (h264v) {   // to 4:2:0, BT.601's limited range (a standard-definition picture's)
        auto Y = [](uint32_t c) { return (uint8_t)(16 + ((66 * (int)((c >> 16) & 255) + 129 * (int)((c >> 8) & 255) + 25 * (int)(c & 255) + 128) >> 8)); };
        AVFrame *f = P.vf;
        for (int y = 0; y < P.h; y++)
            for (int x = 0; x < P.w; x++) f->data[0][(size_t)y * f->linesize[0] + x] = Y(P.canvas[(size_t)y * P.w + x]);
        for (int y = 0; y < P.h / 2; y++)
            for (int x = 0; x < P.w / 2; x++) {
                int r = 0, g = 0, b = 0;
                for (int k = 0; k < 4; k++) {
                    const uint32_t c = P.canvas[(size_t)(2 * y + k / 2) * P.w + 2 * x + k % 2];
                    r += (c >> 16) & 255, g += (c >> 8) & 255, b += c & 255;
                }
                r = (r + 2) / 4, g = (g + 2) / 4, b = (b + 2) / 4;
                const uint8_t u = (uint8_t)(128 + ((-38 * r - 74 * g + 112 * b + 128) >> 8));
                const uint8_t vv = (uint8_t)(128 + ((112 * r - 94 * g - 18 * b + 128) >> 8));
                if (f->format == AV_PIX_FMT_NV12) {
                    f->data[1][(size_t)y * f->linesize[1] + 2 * x] = u;
                    f->data[1][(size_t)y * f->linesize[1] + 2 * x + 1] = vv;
                } else {
                    f->data[1][(size_t)y * f->linesize[1] + x] = u;
                    f->data[2][(size_t)y * f->linesize[2] + x] = vv;
                }
            }
    }
    P.vf->pts = it.pts - P.pts0;
    P.vf->duration = 1;
    if (!send(P.venc, P.vs, P.vf)) return false;
    std::lock_guard<std::mutex> l(m);
    st.frames++;
    st.length = it.pts + 1;
    return true;
}

bool Capture::Impl::encode_sound(const float *lr, size_t frames) {
    Part &P = part;
    P.pending.insert(P.pending.end(), lr, lr + 2 * frames);
    const bool last = lr == nullptr;   // (the end: what is left, as a shorter frame)
    const int fs = P.aenc->frame_size > 0 ? P.aenc->frame_size : 4096;
    size_t at = 0;
    while (P.pending.size() - at >= 2 * (size_t)fs || (last && P.pending.size() > at)) {
        int n = (int)std::min<size_t>((size_t)fs, (P.pending.size() - at) / 2);
        const bool pad = n < fs && !(P.aenc->codec->capabilities & AV_CODEC_CAP_SMALL_LAST_FRAME);
        av_frame_unref(P.af);
        P.af->format = P.aenc->sample_fmt;
        P.af->sample_rate = P.aenc->sample_rate;
        av_channel_layout_copy(&P.af->ch_layout, &P.aenc->ch_layout);
        P.af->nb_samples = pad ? fs : n;
        if (av_frame_get_buffer(P.af, 0) < 0) return error = "no memory for sound", false;
        const float *in = P.pending.data() + at;
        if (P.aenc->sample_fmt == AV_SAMPLE_FMT_S16) {
            int16_t *o = reinterpret_cast<int16_t *>(P.af->data[0]);
            for (int k = 0; k < 2 * n; k++) {   // as --wav writes it (runtime/main.cpp)
                const float f = in[k] * 32768.0f;
                o[k] = (int16_t)(f > 32767 ? 32767 : f < -32768 ? -32768 : f);
            }
            if (pad) memset(o + 2 * n, 0, (size_t)(fs - n) * 4);
        } else {   // planar float
            float *l = reinterpret_cast<float *>(P.af->data[0]), *r = reinterpret_cast<float *>(P.af->data[1]);
            for (int k = 0; k < n; k++) l[k] = in[2 * k], r[k] = in[2 * k + 1];
            for (int k = n; pad && k < fs; k++) l[k] = r[k] = 0;
        }
        P.af->pts = P.samples;
        P.samples += n;
        at += 2 * (size_t)n;
        if (!send(P.aenc, P.as, P.af)) return false;
        std::lock_guard<std::mutex> l(m);
        st.samples += (uint64_t)n;
    }
    P.pending.erase(P.pending.begin(), P.pending.begin() + (long)at);
    return true;
}

bool Capture::Impl::close_part() {
    Part &P = part;
    bool ok = true;
    if (P.begun) {
        ok = send(P.venc, P.vs, nullptr) && ok;
        ok = encode_sound(nullptr, 0) && ok;
        ok = send(P.aenc, P.as, nullptr) && ok;
        const int e = av_write_trailer(P.fmt);
        if (e < 0 && ok) ok = false, error = "finishing the file: " + av_error(e);
    }
    if (P.fmt && P.fmt->pb) avio_closep(&P.fmt->pb);
    avcodec_free_context(&P.venc);
    avcodec_free_context(&P.aenc);
    av_frame_free(&P.vf);
    av_frame_free(&P.af);
    av_packet_free(&P.pkt);
    avformat_free_context(P.fmt);
    P = Part{};
    return ok;
}

#endif  // HK_WITH_RECORD

}  // namespace hk::host::record
