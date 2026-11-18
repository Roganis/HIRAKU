// SPDX-License-Identifier: GPL-3.0-or-later
// The game recorded to a video file: host/record.h
#include "host/record.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <iterator>
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
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
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

// The parts of a recording: NAME.ext, NAME-2.ext...
[[maybe_unused]] static std::string numbered(const std::string &path, int n) {
    if (n <= 1) return path;
    const size_t dot = path.rfind('.'), slash = path.find_last_of("/\\");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return path + "-" + std::to_string(n);
    return path.substr(0, dot) + "-" + std::to_string(n) + path.substr(dot);
}

std::string converted_name(const std::string &in, const std::string &out) {
    if (!out.empty()) return out;
    const size_t dot = in.rfind('.'), slash = in.find_last_of("/\\");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return in + ".mp4";
    return in.substr(0, dot) + ".mp4";
}

// --- which H.264 encoder ----------------------------------------------------------------------
// The graphics cards' encoders, in the order tried: NVIDIA's, AMD's (Windows; Linux with AMD's own
// driver), Intel's Quick Sync, VAAPI (Linux: AMD and Intel with Mesa), Vulkan video, Media Foundation
// with the card's encoder ("h264_mf/hw": Windows, any card's). h264_mf alone: Microsoft's software one
static const char *const kCards[] = {"h264_nvenc", "h264_amf", "h264_qsv", "h264_vaapi", "h264_vulkan", "h264_mf/hw"};

std::vector<std::string> h264_order(const std::string &pref, bool wait) {
    std::vector<std::string> cards(std::begin(kCards), std::end(kCards)), o;
    if (pref != "auto" && pref != "gpu" && pref != "x264") return {pref};   // an encoder by its name: that one
    if (pref == "x264" || (wait && pref == "auto")) {   // (no real time kept: the same file every time)
        o.push_back("libx264");
        o.insert(o.end(), cards.begin(), cards.end());
    } else {
        o = cards;
        o.push_back("libx264");
    }
    o.push_back("h264_mf");   // (no x264 in this FFmpeg: Windows' own, in software)
    return o;
}

std::string choose_h264(const std::string &pref, bool wait, const std::function<std::string(const std::string &)> &try_one, std::string &why) {
    why.clear();
    for (const std::string &name : h264_order(pref, wait)) {
        const std::string no = try_one(name);
        if (no.empty()) return name;
        why += (why.empty() ? "" : "; ") + name + ": " + no;
    }
    return {};
}

int scale_for(const std::string &format, int scale, int w, int h, const std::string &encoder) {
    if (format != "h264" || w <= 0 || h <= 0) return 1;   // (lossless: the game's own pixels)
    if (scale <= 0) {   // auto: a card's encoder enlarges for nothing; the processor's at the game's own size
        if (encoder == "libx264" || encoder == "h264_mf") return 1;
        return std::max(1, std::min(1920 / w, 1440 / h));
    }
    int k = std::min(scale, 8);
    while (k > 1 && (w * k > 4096 || h * k > 4096)) k--;   // (H.264's largest pictures)
    return k;
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
bool convert(const std::string &, const std::string &, const Capture::Setup &, const std::function<void(double)> &, std::string &,
             std::string &error, bool *) {
    error = "this build can't convert recordings (it was built without FFmpeg's libraries)";
    return false;
}
#else

namespace {

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

// h264_order's name to FFmpeg's ("h264_mf/hw": h264_mf)
std::string ffmpeg_name(const std::string &cand) {
    const size_t slash = cand.find('/');
    return slash == std::string::npos ? cand : cand.substr(0, slash);
}

// FFmpeg's log quieter while encoders are tried (one that can't open here says why at length)
// FFmpeg's messages: on the recorder's own threads (the encoder's, a conversion's), and from its
// codecs on FFmpeg's threads (kOurs), its warnings and
// errors become one "record: NAME: ..." line each on stderr (no context address in them, so two
// runs' logs compare), its information is dropped, and nothing at all while encoders are tried
// (Quiet: one that can't open here says why at length). av_log's callback is the process's one, so
// for everything else (Twinkle's video decoder, PCSX2's own capture in the Python core, the
// test's own reading back) it does exactly what FFmpeg's own does: av_log_default_callback at
// av_log_get_level's level, as before.
// The recorder's encoders and decoders have this as their opaque: their messages from FFmpeg's own
// threads (a decoder's slices, x264's frames) are the recorder's too
char kOurs;
std::atomic<int> g_input_trouble{0};   // warnings and errors from what a conversion reads (any thread)
thread_local bool t_ours = false, t_quiet = false;
thread_local std::string t_line;   // a message's parts until its end of line

AVCodecContext *ours(AVCodecContext *c) {
    if (c) c->opaque = &kOurs;
    return c;
}

void log_line(void *avcl, int level, const char *fmt, va_list vl) {
    const AVClass *cls = avcl ? *static_cast<const AVClass *const *>(avcl) : nullptr;
    const bool codec_ours = cls == avcodec_get_class() && static_cast<AVCodecContext *>(avcl)->opaque == &kOurs;
    if (!t_ours && !codec_ours) {
        av_log_default_callback(avcl, level, fmt, vl);
        return;
    }
    if (level <= AV_LOG_WARNING && !t_quiet && cls &&   // (a file read that complains: convert()'s "ended early")
        ((cls == avcodec_get_class() && av_codec_is_decoder(static_cast<AVCodecContext *>(avcl)->codec)) ||
         (cls == avformat_get_class() && static_cast<AVFormatContext *>(avcl)->iformat)))
        g_input_trouble++;
    if (t_quiet || level > AV_LOG_WARNING) return;
    char b[1024];
    vsnprintf(b, sizeof b, fmt, vl);
    t_line += b;
    if (t_line.empty() || t_line.back() != '\n') return;
    t_line.pop_back();
    const char *name = cls && cls->item_name ? cls->item_name(avcl) : nullptr;
    fprintf(stderr, "record: %s%s%s\n", name ? name : "", name ? ": " : "", t_line.c_str());
    t_line.clear();
}

// the callback, once, for the process (Capture::setup, convert)
void own_log() {
    static std::once_flag once;
    std::call_once(once, [] { av_log_set_callback(log_line); });
}

// this thread's FFmpeg messages the recorder's (the encoder's thread, a conversion) for its life
struct Ours {
    bool was = t_ours;
    Ours() { t_ours = true; }
    ~Ours() { t_ours = was; }
};

struct Quiet {
    bool was = t_quiet;
    Quiet() { t_quiet = true; }
    ~Quiet() { t_quiet = was; }
};

// An H.264 encoder, open: its context, and for a card's that takes only its own frames (VAAPI,
// Vulkan) the device and frames the pictures are copied into
struct Venc {
    AVCodecContext *c = nullptr;
    AVBufferRef *device = nullptr, *frames = nullptr;
    AVPixelFormat sw = AV_PIX_FMT_NONE;   // the frames HIRAKU fills: YUV420P or NV12
    void free() {
        avcodec_free_context(&c);
        av_buffer_unref(&frames);
        av_buffer_unref(&device);
    }
};

// cand (h264_order's) opened at w x h, in a constant-quality mode, as the recorder sets it up
bool open_h264(const std::string &cand, int w, int h, AVRational rate, AVRational sar, bool global_header, Venc &e, std::string &why) {
    const std::string name = ffmpeg_name(cand);
    const AVCodec *codec = avcodec_find_encoder_by_name(name.c_str());
    if (!codec) return why = "not in this build's FFmpeg", false;
    const std::vector<AVPixelFormat> pf = pixel_formats(codec);
    auto has = [&](AVPixelFormat f) { return std::find(pf.begin(), pf.end(), f) != pf.end(); };
    AVPixelFormat hw = AV_PIX_FMT_NONE;
    AVHWDeviceType type = AV_HWDEVICE_TYPE_NONE;
    if (has(AV_PIX_FMT_YUV420P)) e.sw = AV_PIX_FMT_YUV420P;
    else if (has(AV_PIX_FMT_NV12)) e.sw = AV_PIX_FMT_NV12;
    else if (has(AV_PIX_FMT_VAAPI)) e.sw = AV_PIX_FMT_NV12, hw = AV_PIX_FMT_VAAPI, type = AV_HWDEVICE_TYPE_VAAPI;
    else if (has(AV_PIX_FMT_VULKAN)) e.sw = AV_PIX_FMT_NV12, hw = AV_PIX_FMT_VULKAN, type = AV_HWDEVICE_TYPE_VULKAN;
    else return why = "takes no picture format HIRAKU gives", false;
    int r;
    if (hw != AV_PIX_FMT_NONE) {   // the card's own frames: the device (its first), a pool of frames on it
        if ((r = av_hwdevice_ctx_create(&e.device, type, nullptr, nullptr, 0)) < 0)
            return why = std::string("no ") + av_hwdevice_get_type_name(type) + " device here (" + av_error(r) + ")", false;
        e.frames = av_hwframe_ctx_alloc(e.device);
        auto *fc = reinterpret_cast<AVHWFramesContext *>(e.frames->data);
        fc->format = hw, fc->sw_format = AV_PIX_FMT_NV12, fc->width = w, fc->height = h;
        fc->initial_pool_size = 16;
        if ((r = av_hwframe_ctx_init(e.frames)) < 0) return why = "its frames can't be made (" + av_error(r) + ")", false;
    }
    AVCodecContext *v = e.c = ours(avcodec_alloc_context3(codec));
    v->width = w, v->height = h;
    v->time_base = {rate.den, rate.num};
    v->framerate = rate;
    v->sample_aspect_ratio = sar;
    v->pix_fmt = hw != AV_PIX_FMT_NONE ? hw : e.sw;
    if (e.frames) v->hw_frames_ctx = av_buffer_ref(e.frames);
    v->flags |= AV_CODEC_FLAG_BITEXACT;
    if (global_header) v->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    v->color_range = AVCOL_RANGE_MPEG;   // BT.601's limited range: a standard-definition picture's
    v->colorspace = AVCOL_SPC_SMPTE170M, v->color_primaries = AVCOL_PRI_SMPTE170M, v->color_trc = AVCOL_TRC_SMPTE170M;
    v->gop_size = (int)std::lround(av_q2d(rate) * 2);   // a key frame every 2 s
    v->max_b_frames = 0;
    // constant quality, each encoder its own way (what's left out of an option an encoder doesn't
    // have is left to its default: av_opt_set says so and nothing more)
    void *o = v->priv_data;
    if (name == "libx264") {
        av_opt_set(o, "preset", "veryfast", 0);
        av_opt_set(o, "crf", "18", 0);
        av_opt_set(o, "x264-params", "log=-1", 0);   // (its summary at the end: not for the game's log)
    } else if (name == "h264_nvenc") {
        av_opt_set(o, "preset", "p4", 0);
        av_opt_set(o, "rc", "vbr", 0);
        av_opt_set_int(o, "cq", 19, 0);
        v->bit_rate = 0;
    } else if (name == "h264_amf") {
        av_opt_set(o, "rc", "cqp", 0);
        av_opt_set_int(o, "qp_i", 18, 0);
        av_opt_set_int(o, "qp_p", 20, 0);
        av_opt_set(o, "quality", "quality", 0);
    } else if (name == "h264_qsv") {
        v->global_quality = 20;   // (ICQ)
    } else if (name == "h264_vaapi" || name == "h264_vulkan") {
        av_opt_set(o, "rc_mode", name == "h264_vaapi" ? "CQP" : "cqp", 0);
        av_opt_set_int(o, "qp", 20, 0);
        v->global_quality = 20;
    } else if (name == "h264_mf") {
        av_opt_set_int(o, "hw_encoding", cand == "h264_mf/hw", 0);   // (0: Microsoft's own, in software)
        av_opt_set(o, "rate_control", "quality", 0);
        av_opt_set_int(o, "quality", 90, 0);
        av_opt_set(o, "scenario", "archive", 0);
    }
    if ((r = avcodec_open2(v, codec, nullptr)) < 0) return why = "doesn't open here (" + av_error(r) + ")", false;
    return true;
}

// A frame for an encoder in the frames it takes: as it is, or copied into the card's
bool hw_frame(const Venc &e, const AVFrame *sw, AVFrame *&out, std::string &error) {
    out = nullptr;
    if (!e.frames) return true;
    out = av_frame_alloc();
    int r;
    if ((r = av_hwframe_get_buffer(e.frames, out, 0)) < 0 || (r = av_hwframe_transfer_data(out, sw, 0)) < 0 ||
        (r = av_frame_copy_props(out, sw)) < 0) {
        av_frame_free(&out);
        return error = "the picture can't be copied to the graphics card (" + av_error(r) + ")", false;
    }
    return true;
}

// rgb (0x00RRGGBB, w x h) into f's 4:2:0 planes (YUV420P or NV12) at its own size, nearest pixels
// (a whole-number enlarging, or a frame of another size scaled in): BT.601, limited range
void to_yuv420(AVFrame *f, const uint32_t *rgb, int w, int h) {
    const int fw = f->width, fh = f->height;
    std::vector<int> xs((size_t)fw), ys((size_t)fh);
    for (int x = 0; x < fw; x++) xs[(size_t)x] = (int)((int64_t)x * w / fw);
    for (int y = 0; y < fh; y++) ys[(size_t)y] = (int)((int64_t)y * h / fh);
    auto Y = [](uint32_t c) {
        return (uint8_t)(16 + ((66 * (int)((c >> 16) & 255) + 129 * (int)((c >> 8) & 255) + 25 * (int)(c & 255) + 128) >> 8));
    };
    std::vector<uint8_t> row_y((size_t)w);
    const ptrdiff_t ly = f->linesize[0];
    for (int y = 0, last = -1; y < fh; y++) {   // (a row the same source row as the one above: copied)
        uint8_t *o = f->data[0] + y * ly;
        if (ys[(size_t)y] == last) {
            memcpy(o, o - ly, (size_t)fw);
            continue;
        }
        last = ys[(size_t)y];
        const uint32_t *in = rgb + (size_t)last * w;
        for (int x = 0; x < w; x++) row_y[(size_t)x] = Y(in[x]);
        for (int x = 0; x < fw; x++) o[x] = row_y[(size_t)xs[(size_t)x]];
    }
    const bool nv12 = f->format == AV_PIX_FMT_NV12;
    const ptrdiff_t lu = f->linesize[1], lv = nv12 ? 0 : f->linesize[2];
    for (int cy = 0, l0 = -1, l1 = -1; cy < fh / 2; cy++) {
        const int r0 = ys[(size_t)(2 * cy)], r1 = ys[(size_t)(2 * cy + 1)];
        uint8_t *u = f->data[1] + cy * lu, *v = nv12 ? nullptr : f->data[2] + cy * lv;
        if (r0 == l0 && r1 == l1) {
            memcpy(u, u - lu, (size_t)(nv12 ? fw : fw / 2));
            if (v) memcpy(v, v - lv, (size_t)fw / 2);
            continue;
        }
        l0 = r0, l1 = r1;
        const uint32_t *a = rgb + (size_t)r0 * w, *b = rgb + (size_t)r1 * w;
        for (int cx = 0; cx < fw / 2; cx++) {
            const int x0 = xs[(size_t)(2 * cx)], x1 = xs[(size_t)(2 * cx + 1)];
            const uint32_t c[4] = {a[x0], a[x1], b[x0], b[x1]};
            int r = 0, g = 0, bl = 0;
            for (uint32_t p : c) r += (p >> 16) & 255, g += (p >> 8) & 255, bl += p & 255;
            r = (r + 2) / 4, g = (g + 2) / 4, bl = (bl + 2) / 4;
            const uint8_t U = (uint8_t)(128 + ((-38 * r - 74 * g + 112 * bl + 128) >> 8));
            const uint8_t V = (uint8_t)(128 + ((112 * r - 94 * g - 18 * bl + 128) >> 8));
            if (nv12) u[2 * cx] = U, u[2 * cx + 1] = V;
            else u[cx] = U, v[cx] = V;
        }
    }
}

// cand tried as a recording would use it: opened at w x h, a first picture encoded and out ("": it
// works here, else why not)
std::string try_h264(const std::string &cand, int w, int h, AVRational rate) {
    Quiet quiet;
    Venc e;
    std::string why;
    if (!open_h264(cand, w, h, rate, {1, 1}, false, e, why)) {
        e.free();
        return why;
    }
    AVFrame *f = av_frame_alloc(), *hw = nullptr;
    AVPacket *p = av_packet_alloc();
    f->format = e.sw, f->width = w, f->height = h;
    int r = av_frame_get_buffer(f, 0);
    if (r >= 0) {
        const std::vector<uint32_t> grey((size_t)w * h, 0x808080u);
        to_yuv420(f, grey.data(), w, h);
        f->pts = 0;
        if (!hw_frame(e, f, hw, why)) r = -1;
        else if ((r = avcodec_send_frame(e.c, hw ? hw : f)) >= 0) r = avcodec_send_frame(e.c, nullptr);
        else why = "a picture can't go in (" + av_error(r) + ")";
    } else {
        why = "no memory for a picture";
    }
    int packets = 0;
    while (r >= 0 && avcodec_receive_packet(e.c, p) >= 0) packets++, av_packet_unref(p);
    av_frame_free(&hw);
    av_frame_free(&f);
    av_packet_free(&p);
    e.free();
    return packets ? std::string() : why.empty() ? "no picture came out" : why;
}

std::string kind(const std::string &cand) {   // for the log
    if (cand == "libx264") return "x264, on the processor";
    if (cand == "h264_mf") return "Media Foundation, Microsoft's own in software";
    if (cand == "h264_mf/hw") return "Media Foundation, the graphics card's";
    return "the graphics card's";
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
    Venc h264;                  // (h264: venc is its context)
    AVStream *vs = nullptr, *as = nullptr;
    AVFrame *vf = nullptr, *af = nullptr;
    AVPacket *pkt = nullptr;
    int w = 0, h = 0;
    int sw = 0, sh = 0;         // the frames' size it was begun for (w, h: that enlarged, made even for 4:2:0)
    int64_t pts0 = 0;           // the recording's frame number this part starts at
    int64_t samples = 0;        // sound samples written (each channel)
    std::vector<float> pending; // sound not yet a whole encoder frame
    bool begun = false;         // its header written: packets may go in, a trailer ends it
};

}  // namespace

struct Capture::Impl {
    Setup s;
    std::string h264;                         // the H.264 encoder taken (h264_order's name; the worker's, under m)
    AVRational rate_q{0, 1};                  // the file's frame rate
    AVRational sar_fixed{0, 1};               // convert(): the pixels' shape as the file read had it

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
    bool pick_h264(int w, int h);
    bool open_part(int w, int h, double cw, double ch);
    bool close_part();
    bool encode_video(const Item &it);
    bool encode_sound(const float *lr, size_t frames);
    bool send(AVCodecContext *c, AVStream *s, const AVFrame *f);
    std::string part_path(int n) const { return numbered(path, n); }
};

bool available() { return true; }

std::string formats() {   // (by name: whether a card's opens here is found when a recording begins)
    std::string h;
    for (const std::string &c : h264_order("x264", false))
        if (c != "h264_mf/hw" && avcodec_find_encoder_by_name(c.c_str())) h += (h.empty() ? "" : ", ") + c;
    return h.empty() ? "ffv1 (this build's FFmpeg has no H.264 encoder)" : "ffv1, h264 (" + h + ")";
}

Capture::Capture() : d(std::make_unique<Impl>()) {}
Capture::~Capture() { stop(); }

bool Capture::setup(const Setup &s, std::string &error) {
    own_log();
    d->s = s;
    d->wait = s.wait;
    {
        std::lock_guard<std::mutex> l(d->m);
        d->h264.clear();
    }
    if (s.format == "h264") {
        bool any = false;
        for (const std::string &c : h264_order(s.encoder, s.wait)) any = any || avcodec_find_encoder_by_name(ffmpeg_name(c).c_str());
        if (!any) {
            error = s.encoder == "auto" || s.encoder == "gpu" || s.encoder == "x264"
                        ? "--record-format h264: this build's FFmpeg has no H.264 encoder (ffv1 is always there)"
                        : "--record-encoder " + s.encoder + ": not in this build's FFmpeg";
            return false;
        }
        if (!avcodec_find_encoder(AV_CODEC_ID_AAC) || !av_guess_format("mp4", nullptr, nullptr)) {
            error = "--record-format h264: this build's FFmpeg has no AAC encoder or MP4 writer";
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
    auto next = std::make_unique<Impl>();   // (the setup, the encoder taken, the frames' times and the threads finishing go on)
    next->s = d->s, next->wait = d->s.wait;
    {
        std::lock_guard<std::mutex> l(d->m);
        next->h264 = d->h264;
    }
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
             "record: %s: %llu frames %dx%d at %.3f fps by %s, %.2f s long, %.2f s of sound, %d file%s; left out: %llu (encoder behind), "
             "%llu (faster than the rate); queue at most %zu frames (%zu MB); encoder %.2f ms a frame, its thread's CPU %.2f ms a frame%s%s",
             I.path.c_str(), (unsigned long long)s.frames, s.w, s.h, s.rate, s.encoder.empty() ? "-" : s.encoder.c_str(),
             s.rate > 0 ? (double)s.length / s.rate : 0.0, I.s.audio_rate > 0 ? (double)s.samples / I.s.audio_rate : 0.0, s.parts,
             s.parts == 1 ? "" : "s", (unsigned long long)s.dropped, (unsigned long long)s.too_soon, s.queue_high, s.queue_high_bytes >> 20,
             s.frames ? s.encode_ms / (double)s.frames : 0.0, s.frames ? s.worker_cpu_ms / (double)s.frames : 0.0,
             I.error.empty() ? "" : "; FAILED: ", I.error.c_str());
    return line;
}

// --- the encoder's thread --------------------------------------------------------------------

void Capture::Impl::run() {
    Ours ours;
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

// The H.264 encoder, the first time one is needed (the first part's size, w x h, before enlarging;
// each tried at the size it would take: scale_for): tried here, on the encoder's thread, so a card's
// that takes a moment to open doesn't stop the game
bool Capture::Impl::pick_h264(int w, int h) {
    std::string taken, why;
    bool wait_now;
    {
        std::lock_guard<std::mutex> l(m);
        taken = h264, wait_now = wait;
    }
    if (!taken.empty()) return true;
    taken = choose_h264(s.encoder, wait_now, [&](const std::string &c) {
        const int k = scale_for(s.format, s.scale, w, h, c);
        return try_h264(c, (w * k + 1) & ~1, (h * k + 1) & ~1, rate_q);
    }, why);
    if (taken.empty()) return error = "no H.264 encoder works here (" + why + ")", false;
    printf("record: H.264 by %s (%s)%s%s\n", ffmpeg_name(taken).c_str(), kind(taken).c_str(), why.empty() ? "" : "; not: ", why.c_str());
    std::lock_guard<std::mutex> l(m);
    h264 = taken;
    return true;
}

bool Capture::Impl::open_part(int w, int h, double cw, double ch) {
    const bool h264v = s.format == "h264";
    Part p;
    p.sw = w, p.sh = h;
    if (h264v && !pick_h264(w, h)) return false;   // (first: the enlarging depends on the encoder)
    const int k = scale_for(s.format, s.scale, w, h, h264);
    w *= k, h *= k;
    if (h264v) w = (w + 1) & ~1, h = (h + 1) & ~1;   // (4:2:0: even sizes)
    p.w = w, p.h = h;
    const std::string file = part_path(st.parts + 1);
    int e = avformat_alloc_output_context2(&p.fmt, nullptr, h264v ? "mp4" : "matroska", file.c_str());
    if (e < 0 || !p.fmt) return error = "no " + std::string(h264v ? "MP4" : "Matroska") + " writer: " + av_error(e), false;
    p.fmt->flags |= AVFMT_FLAG_BITEXACT;   // (no date or random id: a run's file the same every time)
    part = p;   // (closed by close_part from here on, whatever fails)
    Part &P = part;
    const bool global = P.fmt->oformat->flags & AVFMT_GLOBALHEADER;

    // the picture
    double sar = 1.0;   // (cw, ch: the part of the screen a frame of this size covers)
    if (s.aspect > 0 && cw > 0 && ch > 0) sar = s.aspect * cw / ch * h / w;
    const AVRational sar_q = sar_fixed.num > 0 ? sar_fixed : av_d2q(sar, 1000);
    if (h264v) {
        std::string why;
        {
            Quiet quiet;
            if (!open_h264(h264, w, h, rate_q, sar_q, global, P.h264, why)) return error = ffmpeg_name(h264) + " " + why, false;
        }
        P.venc = P.h264.c;
    } else {
        const AVCodec *vc = avcodec_find_encoder(AV_CODEC_ID_FFV1);
        P.venc = ours(avcodec_alloc_context3(vc));
        AVCodecContext *v = P.venc;
        v->width = w, v->height = h;
        v->time_base = {rate_q.den, rate_q.num};
        v->framerate = rate_q;
        v->sample_aspect_ratio = sar_q;
        v->flags |= AV_CODEC_FLAG_BITEXACT;
        v->pix_fmt = AV_PIX_FMT_0RGB32;   // the cores' 0x00RRGGBB as it is: no colour conversion
        v->level = 3;
        v->gop_size = 1;
        v->slices = 4;
        // slices on FFmpeg's own threads: pop'n music 7's 640x480 in 1.6 ms a frame with 4 (4.2 with
        // one), for about 10 % more processor time in all
        v->thread_count = (int)std::clamp(std::thread::hardware_concurrency() / 4, 1u, 4u);
        v->thread_type = FF_THREAD_SLICE;
        av_opt_set_int(v->priv_data, "slicecrc", 1, 0);
        if (global) v->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if ((e = avcodec_open2(v, vc, nullptr)) < 0) return error = "the video encoder doesn't open: " + av_error(e), false;
    }
    AVCodecContext *v = P.venc;
    P.vs = avformat_new_stream(P.fmt, nullptr);
    avcodec_parameters_from_context(P.vs->codecpar, v);
    P.vs->time_base = v->time_base;
    P.vs->avg_frame_rate = P.vs->r_frame_rate = rate_q;
    P.vs->sample_aspect_ratio = v->sample_aspect_ratio;

    // the sound
    const AVCodec *ac = avcodec_find_encoder(h264v ? AV_CODEC_ID_AAC : AV_CODEC_ID_FLAC);
    if (!ac) return error = "no sound encoder", false;
    P.aenc = ours(avcodec_alloc_context3(ac));
    AVCodecContext *a = P.aenc;
    a->sample_rate = s.audio_rate;
    av_channel_layout_default(&a->ch_layout, 2);
    a->sample_fmt = h264v ? AV_SAMPLE_FMT_FLTP : AV_SAMPLE_FMT_S16;
    a->time_base = {1, s.audio_rate};
    a->flags |= AV_CODEC_FLAG_BITEXACT;
    if (h264v) a->bit_rate = 256'000;
    if (global) a->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if ((e = avcodec_open2(a, ac, nullptr)) < 0) return error = "the sound encoder doesn't open: " + av_error(e), false;
    P.as = avformat_new_stream(P.fmt, nullptr);
    avcodec_parameters_from_context(P.as->codecpar, a);
    P.as->time_base = a->time_base;

    P.vf = av_frame_alloc();
    P.vf->format = h264v ? P.h264.sw : v->pix_fmt, P.vf->width = w, P.vf->height = h;
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
    st.encoder = h264v ? ffmpeg_name(h264) : "ffv1";
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
    if (s.format == "h264") {   // to 4:2:0, enlarged (or a frame of another size scaled in) on the way
        to_yuv420(P.vf, it.px.data(), it.w, it.h);
    } else {   // the frame at the part's size: as it is, or a smaller one scaled up to it (nearest pixels)
        uint32_t *dst = reinterpret_cast<uint32_t *>(P.vf->data[0]);
        const size_t stride = (size_t)P.vf->linesize[0] / 4;
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
    }
    P.vf->pts = it.pts - P.pts0;
    P.vf->duration = 1;
    AVFrame *hw = nullptr;
    if (!hw_frame(P.h264, P.vf, hw, error)) return false;
    const bool ok = send(P.venc, P.vs, hw ? hw : P.vf);
    av_frame_free(&hw);
    if (!ok) return false;
    std::lock_guard<std::mutex> l(m);
    st.frames++;
    st.length = it.pts + 1;
    return true;
}

bool Capture::Impl::encode_sound(const float *lr, size_t frames) {
    Part &P = part;
    if (lr) P.pending.insert(P.pending.end(), lr, lr + 2 * frames);
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
    if (P.h264.c) P.venc = nullptr;   // (h264: venc is P.h264's context, freed with it)
    avcodec_free_context(&P.venc);
    P.h264.free();
    avcodec_free_context(&P.aenc);
    av_frame_free(&P.vf);
    av_frame_free(&P.af);
    av_packet_free(&P.pkt);
    avformat_free_context(P.fmt);
    P = Part{};
    return ok;
}

// --- a lossless recording to H.264 afterwards ----------------------------------------------

namespace {

// One part: in (FFV1 and FLAC in Matroska) to out (H.264 and AAC in MP4). done/total: the progress
// over all parts, in seconds of video
bool convert_part(const std::string &in, const std::string &out, const Capture::Setup &setup, double done, double total,
                  const std::function<void(double)> &progress, Capture::Stats &st, double &sound_s, std::string &ended,
                  std::string &error) {
    g_input_trouble = 0;
    AVFormatContext *fmt = nullptr;
    int e;
    if ((e = avformat_open_input(&fmt, in.c_str(), nullptr, nullptr)) < 0) return error = "can't read " + in + ": " + av_error(e), false;
    struct Closer {
        AVFormatContext *&f;
        AVCodecContext *dec[2] = {};
        AVFrame *frame = av_frame_alloc();
        AVPacket *pkt = av_packet_alloc();
        ~Closer() {
            avcodec_free_context(&dec[0]);
            avcodec_free_context(&dec[1]);
            av_frame_free(&frame);
            av_packet_free(&pkt);
            avformat_close_input(&f);
        }
    } c{fmt};
    if ((e = avformat_find_stream_info(fmt, nullptr)) < 0) return error = in + ": " + av_error(e), false;
    int vi = -1, ai = -1;
    for (unsigned k = 0; k < fmt->nb_streams; k++) {
        const AVCodecParameters *p = fmt->streams[k]->codecpar;
        const int w = p->codec_type == AVMEDIA_TYPE_VIDEO ? 0 : p->codec_type == AVMEDIA_TYPE_AUDIO ? 1 : -1;
        if (w < 0 || (w == 0 ? vi : ai) >= 0) continue;
        const AVCodec *codec = avcodec_find_decoder(p->codec_id);
        if (!codec) return error = in + ": no decoder for its " + (w ? "sound" : "picture") + " (" + avcodec_get_name(p->codec_id) + ") in this build", false;
        c.dec[w] = ours(avcodec_alloc_context3(codec));
        avcodec_parameters_to_context(c.dec[w], p);
        c.dec[w]->thread_count = 0;
        if ((e = avcodec_open2(c.dec[w], codec, nullptr)) < 0) return error = in + ": " + av_error(e), false;
        (w ? ai : vi) = (int)k;
    }
    if (vi < 0) return error = in + ": no picture in it", false;
    if (c.dec[0]->pix_fmt != AV_PIX_FMT_0RGB32 && c.dec[0]->pix_fmt != AV_PIX_FMT_RGB32 && c.dec[0]->pix_fmt != AV_PIX_FMT_BGR0)
        return error = in + ": not a lossless recording of HIRAKU's (its picture is " + av_get_pix_fmt_name(c.dec[0]->pix_fmt) + ")", false;
    const AVStream *vs = fmt->streams[vi];

    Capture::Impl I;
    I.s = setup;
    I.s.format = "h264";
    I.s.cover = nullptr;
    I.wait = setup.wait;
    I.s.audio_rate = ai >= 0 ? c.dec[1]->sample_rate : 48000;
    I.path = out;
    I.rate_q = vs->avg_frame_rate.num > 0 ? vs->avg_frame_rate : vs->r_frame_rate;
    if (I.rate_q.num <= 0) return error = in + ": no frame rate", false;
    const double rate = av_q2d(I.rate_q);
    I.sar_fixed = vs->sample_aspect_ratio.num > 0 ? vs->sample_aspect_ratio : AVRational{1, 1};
    const AVRational vtb = vs->time_base;
    int64_t last = -1;
    std::vector<float> early;   // sound before the first picture began the file
    double reported = -1;
    bool ok = true;
    auto take = [&](int which) -> bool {
        while (avcodec_receive_frame(c.dec[which], c.frame) >= 0) {
            AVFrame *f = c.frame;
            if (which == 0) {
                Item it;
                it.video = true, it.w = f->width, it.h = f->height;
                int64_t n = f->pts == AV_NOPTS_VALUE ? last + 1 : std::llround((double)f->pts * vtb.num / vtb.den * rate);
                it.pts = std::max(n, last + 1);
                last = it.pts;
                it.px.resize((size_t)it.w * it.h);
                for (int y = 0; y < it.h; y++) memcpy(it.px.data() + (size_t)y * it.w, f->data[0] + (size_t)y * f->linesize[0], (size_t)it.w * 4);
                const bool first = !I.part.begun;
                if (!I.encode(it)) return false;
                if (first && !early.empty()) {
                    if (!I.encode_sound(early.data(), early.size() / 2)) return false;
                    early.clear();
                }
                const double t = done + (double)it.pts / rate;
                if (progress && total > 0 && t - reported >= total / 200) progress(std::min(1.0, t / total)), reported = t;
            } else {
                std::vector<float> lr(2 * (size_t)f->nb_samples);
                const int ch = f->ch_layout.nb_channels;
                for (int k = 0; k < f->nb_samples; k++)
                    for (int j = 0; j < 2; j++) {
                        const int src = std::min(j, ch - 1);
                        float v = 0;
                        switch (f->format) {
                        case AV_SAMPLE_FMT_S16: v = reinterpret_cast<const int16_t *>(f->data[0])[k * ch + src] / 32768.0f; break;
                        case AV_SAMPLE_FMT_S32: v = (float)(reinterpret_cast<const int32_t *>(f->data[0])[k * ch + src] / 2147483648.0); break;
                        case AV_SAMPLE_FMT_S16P: v = reinterpret_cast<const int16_t *>(f->data[src])[k] / 32768.0f; break;
                        case AV_SAMPLE_FMT_S32P: v = (float)(reinterpret_cast<const int32_t *>(f->data[src])[k] / 2147483648.0); break;
                        case AV_SAMPLE_FMT_FLT: v = reinterpret_cast<const float *>(f->data[0])[k * ch + src]; break;
                        case AV_SAMPLE_FMT_FLTP: v = reinterpret_cast<const float *>(f->data[src])[k]; break;
                        default: break;
                        }
                        lr[2 * (size_t)k + j] = v;
                    }
                if (!I.part.begun) early.insert(early.end(), lr.begin(), lr.end());
                else if (!I.encode_sound(lr.data(), (size_t)f->nb_samples)) return false;
            }
            av_frame_unref(f);
        }
        return true;
    };
    int r;
    while (ok && (r = av_read_frame(fmt, c.pkt)) >= 0) {
        const int which = c.pkt->stream_index == vi ? 0 : c.pkt->stream_index == ai ? 1 : -1;
        if (which >= 0 && avcodec_send_packet(c.dec[which], c.pkt) < 0) g_input_trouble++;   // (a damaged packet: left out)
        else if (which >= 0) ok = take(which);
        av_packet_unref(c.pkt);
    }
    if (ok && r != AVERROR_EOF) g_input_trouble++;   // (the file can't be read on)
    for (int w = 0; ok && w < 2; w++)
        if (c.dec[w]) avcodec_send_packet(c.dec[w], nullptr), ok = take(w);
    if (I.part.fmt && !I.close_part()) ok = false;
    if (!ok) {
        error = out + ": " + I.error;
        return false;
    }
    if (!I.st.frames) return error = in + ": no picture could be read", false;
    if (g_input_trouble > 0) {   // a recording cut short (a crash, a power cut) or damaged: what could be read is kept
        char b[512];
        snprintf(b, sizeof b, "convert: %s ended early: %llu frames (%.2f s) kept", in.c_str(), (unsigned long long)I.st.frames,
                 (double)I.st.length / rate);
        ended = b;
    }
    st.frames += I.st.frames, st.samples += I.st.samples, st.parts++;
    sound_s += (double)I.st.samples / I.s.audio_rate;
    st.length += I.st.length, st.w = I.st.w, st.h = I.st.h, st.rate = rate, st.encoder = I.st.encoder;
    return true;
}

double seconds_of(const std::string &path) {   // a file's length (0: unknown)
    AVFormatContext *fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) return 0;
    double s = 0;
    if (avformat_find_stream_info(fmt, nullptr) >= 0 && fmt->duration > 0) s = (double)fmt->duration / AV_TIME_BASE;
    avformat_close_input(&fmt);
    return s;
}

}  // namespace

bool convert(const std::string &in, const std::string &out_, const Capture::Setup &s, const std::function<void(double)> &progress,
             std::string &line, std::string &error, bool *ended_early) {
    if (ended_early) *ended_early = false;
    const std::string out = converted_name(in, out_);
    if (!hk::sys::exists(in)) return error = "no " + in, false;
    if (out == in) return error = "the converted file would be " + in + " itself", false;
    own_log();
    Ours ours;
    Capture::Setup h = s;
    h.format = "h264";
    if (Capture probe; !probe.setup(h, error)) return false;
    std::vector<std::string> parts;
    double total = 0;
    for (int n = 1; n == 1 || hk::sys::exists(numbered(in, n)); n++) {
        parts.push_back(numbered(in, n));
        total += seconds_of(parts.back());
    }
    if (const size_t slash = out.find_last_of("/\\"); slash != std::string::npos && slash > 0) hk::sys::make_dirs(out.substr(0, slash));
    Capture::Stats st;
    const uint64_t t = prof::now_ns();
    double done = 0, sound_s = 0;
    std::string early;   // the parts that ended early: a line each
    for (size_t k = 0; k < parts.size(); k++) {
        std::string e;
        if (!convert_part(parts[k], numbered(out, (int)k + 1), h, done, total, progress, st, sound_s, e, error)) return false;
        if (!e.empty()) early += e + "\n";
        done = 0;
        for (size_t j = 0; j <= k; j++) done += seconds_of(parts[j]);
    }
    if (progress) progress(1.0);
    const double secs = (double)(prof::now_ns() - t) / 1e9;
    char b[1024];
    snprintf(b, sizeof b, "convert: %s: %llu frames %dx%d at %.3f fps by %s, %.2f s long, %.2f s of sound, %d file%s, in %.1f s (%.2f ms a frame)",
             out.c_str(), (unsigned long long)st.frames, st.w, st.h, st.rate, st.encoder.c_str(), st.rate > 0 ? (double)st.length / st.rate : 0.0,
             sound_s, st.parts, st.parts == 1 ? "" : "s", secs, st.frames ? secs * 1000 / (double)st.frames : 0.0);
    line = early + b;
    if (ended_early) *ended_early = !early.empty();
    return true;
}

#endif  // HK_WITH_RECORD

}  // namespace hk::host::record
