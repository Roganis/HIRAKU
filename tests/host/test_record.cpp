// SPDX-License-Identifier: GPL-3.0-or-later
// The video recorder (runtime/host/record.h) alone: made-up frames and sound in, a file out, read
// back with the same FFmpeg libraries: the frames' count, size, rate, shape and times, every pixel
// (FFV1 is lossless), the sound's every sample (as --wav has it), a smaller picture scaled into the
// file and a larger one starting the next, frames left out when the encoder is behind (with the
// times kept), a run the same file twice, a machine whose frame times vary.
//
//   test_record DIR     DIR: a folder to write the files in. Exit 0 when all hold, 77 (skipped) when
//                       the FFmpeg linked has no FFV1, FLAC, Matroska or file writing
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "host/record.h"
#include "shell/sys.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace {

using hk::host::record::Capture;

int g_fail = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
            g_fail++;                                                             \
        }                                                                         \
    } while (0)

// frame n's picture, w x h: a pattern that moves with n (and differs in every channel)
std::vector<uint32_t> picture(int n, int w, int h) {
    std::vector<uint32_t> px((size_t)w * h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const uint32_t r = (uint32_t)(x * 3 + n) & 255, g = (uint32_t)(y * 5 + 2 * n) & 255, b = (uint32_t)((x ^ y) + 7 * n) & 255;
            px[(size_t)y * w + x] = r << 16 | g << 8 | b;
        }
    return px;
}

// the sound after frame n: `count` stereo samples of two tones, now and then past full scale
std::vector<float> sound(int64_t first, int count) {
    std::vector<float> s(2 * (size_t)count);
    for (int k = 0; k < count; k++) {
        const double t = (double)(first + k);
        s[2 * k] = (float)(1.1 * std::sin(t * 0.031));
        s[2 * k + 1] = (float)(0.5 * std::sin(t * 0.007) + 0.25 * std::sin(t * 0.13));
    }
    return s;
}
int16_t as_wav(float v) {   // runtime/main.cpp's --wav
    const float f = v * 32768.0f;
    return (int16_t)(f > 32767 ? 32767 : f < -32768 ? -32768 : f);
}

struct Decoded {
    int w = 0, h = 0;
    AVRational rate{0, 1}, sar{0, 1}, vtb{0, 1};
    std::vector<int64_t> pts;                 // the frames' times, in vtb
    std::vector<std::vector<uint32_t>> frames;
    std::vector<int16_t> pcm;                 // interleaved stereo (16-bit sound: FLAC's)
    int64_t samples = 0;                      // the sound's length, each channel
    int audio_rate = 0;
    double duration = 0;                      // the file's, seconds
    std::string vcodec, acodec;
};

bool decode(const std::string &path, Decoded &out) {
    AVFormatContext *fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) return false;
    if (avformat_find_stream_info(fmt, nullptr) < 0) return false;
    out.duration = fmt->duration > 0 ? (double)fmt->duration / AV_TIME_BASE : 0;
    AVCodecContext *dec[2] = {};
    int vi = -1, ai = -1;
    for (unsigned k = 0; k < fmt->nb_streams; k++) {
        AVStream *s = fmt->streams[k];
        const AVCodec *c = avcodec_find_decoder(s->codecpar->codec_id);
        if (!c) return false;
        AVCodecContext *x = avcodec_alloc_context3(c);
        avcodec_parameters_to_context(x, s->codecpar);
        if (avcodec_open2(x, c, nullptr) < 0) return false;
        if (s->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            vi = (int)k, dec[0] = x;
            out.w = s->codecpar->width, out.h = s->codecpar->height;
            out.rate = s->avg_frame_rate, out.sar = s->sample_aspect_ratio, out.vtb = s->time_base;
            out.vcodec = c->name;
        } else {
            ai = (int)k, dec[1] = x;
            out.audio_rate = s->codecpar->sample_rate;
            out.acodec = c->name;
        }
    }
    if (vi < 0 || ai < 0) return false;
    AVPacket *p = av_packet_alloc();
    AVFrame *f = av_frame_alloc();
    auto drain = [&](int which) {
        while (avcodec_receive_frame(dec[which], f) >= 0) {
            if (which == 0 && f->format != AV_PIX_FMT_BGR0 && f->format != AV_PIX_FMT_0RGB32) {   // (H.264's: times only)
                out.pts.push_back(f->pts);
                out.frames.emplace_back();
            } else if (which == 0) {
                out.pts.push_back(f->pts);
                std::vector<uint32_t> px((size_t)f->width * f->height);
                for (int y = 0; y < f->height; y++)
                    for (int x = 0; x < f->width; x++)
                        px[(size_t)y * f->width + x] = reinterpret_cast<const uint32_t *>(f->data[0] + (size_t)y * f->linesize[0])[x] & 0xFFFFFF;
                out.frames.push_back(std::move(px));
            } else {
                out.samples += f->nb_samples;
                if (f->format == AV_SAMPLE_FMT_S16)
                    for (int k = 0; k < f->nb_samples * 2; k++) out.pcm.push_back(reinterpret_cast<const int16_t *>(f->data[0])[k]);
            }
            av_frame_unref(f);
        }
    };
    while (av_read_frame(fmt, p) >= 0) {
        const int which = p->stream_index == vi ? 0 : p->stream_index == ai ? 1 : -1;
        if (which >= 0 && avcodec_send_packet(dec[which], p) >= 0) drain(which);
        av_packet_unref(p);
    }
    for (int w = 0; w < 2; w++) avcodec_send_packet(dec[w], nullptr), drain(w);
    av_packet_free(&p);
    av_frame_free(&f);
    avcodec_free_context(&dec[0]);
    avcodec_free_context(&dec[1]);
    avformat_close_input(&fmt);
    return true;
}

std::string read_all(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// A recording of `frames` pictures at `hz` (the machine's seconds n / hz), the sound after each
// (rate / hz samples, the remainder carried), the recorder started before the first. What went in
// from the file's first picture on (the third: two frame times before it) is kept for comparing
struct Run {
    std::vector<std::vector<uint32_t>> pics;   // recorded ones
    std::vector<int16_t> pcm;
    Capture::Stats stats;
    std::string line;
};
Run record(const std::string &path, int frames, double hz, int rate, Capture::Setup setup, int w = 320, int h = 240) {
    Run r;
    Capture c;
    std::string err;
    setup.audio_rate = rate;
    if (!c.setup(setup, err) || !c.start(path, err)) {
        fprintf(stderr, "%s\n", err.c_str());
        g_fail++;
        return r;
    }
    int64_t made = 0;
    double owed = 0;
    for (int n = 0; n < frames; n++) {
        const std::vector<uint32_t> px = picture(n, w, h);
        c.picture(px.data(), w, h, n / hz);
        if (n >= 2) r.pics.push_back(px);
        owed += rate / hz;
        const int count = (int)owed;
        owed -= count;
        const std::vector<float> s = sound(made, count);
        made += count;
        c.sound(s.data(), count);
        if (n >= 2)
            for (float v : s) r.pcm.push_back(as_wav(v));
    }
    r.line = c.stop();
    r.stats = c.stats();
    return r;
}

void basic(const std::string &dir) {
    const std::string path = dir + "/basic.mkv";
    Capture::Setup s;
    s.wait = true;
    s.aspect = 4.0 / 3.0;
    const Run r = record(path, 182, 58.0, 48000, s);   // 180 frames recorded: 3.103 s at 58 Hz
    printf("%s\n", r.line.c_str());
    CHECK(r.stats.frames == 180);
    CHECK(r.stats.dropped == 0 && r.stats.too_soon == 0);
    CHECK(r.stats.parts == 1);
    Decoded d;
    CHECK(decode(path, d));
    CHECK(d.vcodec == "ffv1" && d.acodec == "flac");
    CHECK(d.w == 320 && d.h == 240);
    CHECK(d.rate.den > 0 && std::fabs(av_q2d(d.rate) - 58) < 1e-4);
    CHECK(d.sar.num == 0 || d.sar.num == d.sar.den);   // 320x240 on a 4:3 screen: square pixels
    CHECK(d.frames.size() == 180);
    bool times = d.pts.size() == 180, pixels = d.frames.size() == r.pics.size();
    for (size_t k = 0; times && k < d.pts.size(); k++)   // frame k at k / 58 s (the file's own clock: ms)
        times = std::llabs(d.pts[k] * d.vtb.num * 1000 / d.vtb.den - (int64_t)std::llround(k * 1000.0 / 58)) <= 1;
    for (size_t k = 0; pixels && k < d.frames.size(); k++) pixels = d.frames[k] == r.pics[k];
    CHECK(times);
    CHECK(pixels);
    CHECK(d.audio_rate == 48000);
    CHECK(d.pcm == r.pcm);   // every sample, as --wav has it
    printf("basic: %zu frames %dx%d at %d/%d fps, %.3f s (frames / rate %.3f), sound %.3f s, file %.3f s\n", d.frames.size(), d.w, d.h,
           d.rate.num, d.rate.den, d.pts.empty() ? 0.0 : (double)d.pts.back() * d.vtb.num / d.vtb.den, 180 / 58.0,
           (double)d.pcm.size() / 2 / 48000, d.duration);
    CHECK(std::fabs(d.duration - 180 / 58.0) < 0.01);
    CHECK(std::fabs((double)d.pcm.size() / 2 / 48000 - 180 / 58.0) < 1.0 / 58);

    // the same run again: the same file, byte for byte
    const std::string again = dir + "/basic-again.mkv";
    record(again, 182, 58.0, 48000, s);
    CHECK(read_all(path) == read_all(again));
    CHECK(read_all(path).size() > 1000);
}

// 512x480 on a 4:3 screen: the pixels' shape in the file; a smaller picture for a moment (under
// half a second) scaled up into it; a larger one that stays in a second file, from its first frame
void sizes(const std::string &dir) {
    const std::string path = dir + "/sizes.mkv";
    Capture c;
    Capture::Setup s;
    s.wait = true;
    std::string err;
    CHECK(c.setup(s, err) && c.start(path, err));
    std::vector<std::vector<uint32_t>> want1, want2;
    std::vector<int16_t> pcm1, pcm2;
    double t = 0;
    for (int n = 0; n < 80; n++, t += 1 / 59.94) {
        const int w = n < 20 ? 512 : n < 30 ? 256 : 640, h = n < 20 ? 480 : n < 30 ? 240 : 480;
        const std::vector<uint32_t> px = picture(n, w, h);
        c.picture(px.data(), w, h, t);
        if (n < 2) continue;
        if (n < 30) {
            if (w == 512) want1.push_back(px);
            else {   // 256x240 doubled each way
                std::vector<uint32_t> big((size_t)512 * 480);
                for (int y = 0; y < 480; y++)
                    for (int x = 0; x < 512; x++) big[(size_t)y * 512 + x] = px[(size_t)(y / 2) * 256 + x / 2];
                want1.push_back(big);
            }
        } else {
            want2.push_back(px);
        }
        const std::vector<float> snd = sound(n * 800, 800);
        c.sound(snd.data(), 800);
        for (float v : snd) (n < 30 ? pcm1 : pcm2).push_back(as_wav(v));
    }
    printf("%s\n", c.stop().c_str());
    CHECK(c.stats().parts == 2);
    Decoded a, b;
    CHECK(decode(path, a));
    CHECK(decode(dir + "/sizes-2.mkv", b));
    CHECK(a.w == 512 && a.h == 480 && b.w == 640 && b.h == 480);
    CHECK(a.sar.num * 4 == a.sar.den * 5);   // 512 pixels across a 4:3 screen of 480 lines: 5:4 each
    CHECK(b.sar.num == 0 || b.sar.num == b.sar.den);
    CHECK(a.frames == want1);
    CHECK(b.frames == want2);   // (every frame at the new size: the file begins at its first)
    CHECK(a.pcm == pcm1 && b.pcm == pcm2);   // the sound after each frame with it
    CHECK(!b.pts.empty() && b.pts[0] == 0);
    CHECK(a.rate.den > 0 && std::fabs(av_q2d(a.rate) - 59.94) < 1e-4);

    // a size for a moment only (under 30 frames), at the end: scaled into the file, no second one;
    // and a frame covering part of the screen (Setup::cover): the shape from it
    const std::string p2 = dir + "/flicker.mkv";
    Capture f;
    Capture::Setup s2;
    s2.wait = true;
    s2.cover = [](int, int, double &cw, double &ch) { cw = 0.9, ch = 0.9333; };   // 512x448 in a 4:3 raster
    CHECK(f.setup(s2, err) && f.start(p2, err));
    for (int n = 0; n < 50; n++) {
        const int w = n < 40 ? 512 : 640, h = n < 40 ? 448 : 480;
        const std::vector<uint32_t> px = picture(n, w, h);
        f.picture(px.data(), w, h, n / 60.0);
    }
    printf("%s\n", f.stop().c_str());
    CHECK(f.stats().parts == 1 && f.stats().frames == 48);
    Decoded d;
    CHECK(decode(p2, d));
    CHECK(d.w == 512 && d.h == 448 && d.frames.size() == 48);
    // 4/3 * 0.9 / 0.9333 * 448 / 512 = 1.125: 9:8
    CHECK(d.sar.num * 8 == d.sar.den * 9);
}

// the encoder behind with the game in real time (a queue of one frame): frames left out, the
// times of those kept right, every sample of the sound there
void behind(const std::string &dir) {
    const std::string path = dir + "/behind.mkv";
    Capture::Setup s;
    s.wait = false;
    s.memory = 640 * 480 * 4;
    const int w = 640, h = 480;
    const Run r = record(path, 302, 60.0, 44100, s, w, h);
    printf("%s\n", r.line.c_str());
    CHECK(r.stats.frames + r.stats.dropped == 300);
    Decoded d;
    CHECK(decode(path, d));
    CHECK(d.frames.size() == r.stats.frames);
    bool in_order = true, at_their_times = true;
    for (size_t k = 0; k < d.pts.size(); k++) {
        if (k && d.pts[k] <= d.pts[k - 1]) in_order = false;
        // each frame kept is its picture of the run's, at that picture's time
        const int64_t ms = d.pts[k] * d.vtb.num * 1000 / d.vtb.den;
        const size_t n = (size_t)std::llround(ms * 60.0 / 1000);
        if (n >= r.pics.size() || d.frames[k] != r.pics[n]) at_their_times = false;
    }
    CHECK(in_order);
    CHECK(at_their_times);
    CHECK(d.pcm == r.pcm);
    printf("behind: %llu frames kept, %llu left out, the sound %.3f s\n", (unsigned long long)r.stats.frames,
           (unsigned long long)r.stats.dropped, (double)d.pcm.size() / 2 / 44100);
}

// --record-format h264, where this build's FFmpeg has an encoder that opens here: MP4, H.264, AAC
void h264(const std::string &dir) {
    if (hk::host::record::formats().find("h264") == std::string::npos) {
        printf("h264: no H.264 encoder here, not tried\n");
        return;
    }
    const std::string path = dir + "/h264.mp4";
    Capture::Setup s;
    s.format = "h264";
    s.wait = true;
    const Run r = record(path, 122, 60.0, 44100, s, 321, 239);   // (odd sizes: 4:2:0 wants even ones)
    printf("%s\n", r.line.c_str());
    CHECK(r.stats.frames == 120 && r.stats.parts == 1);
    Decoded d;
    CHECK(decode(path, d));
    CHECK(d.vcodec == "h264" && d.acodec == "aac");
    CHECK(d.w == 322 && d.h == 240);
    CHECK(d.rate.den > 0 && std::fabs(av_q2d(d.rate) - 60) < 1e-4);
    CHECK(d.frames.size() == 120);
    CHECK(std::fabs(d.duration - 2.0) < 0.05);
    CHECK(std::llabs(d.samples - 88200) <= 2048);   // (AAC's priming and last frame)
    printf("h264: %zu frames %dx%d, %.3f s, sound %lld samples\n", d.frames.size(), d.w, d.h, d.duration, (long long)d.samples);
}

// frame times that vary (a PC game's): the core's rate, and frames sooner than it left out
void uneven(const std::string &dir) {
    const std::string path = dir + "/uneven.mkv";
    Capture c;
    Capture::Setup s;
    s.wait = true;
    s.native_rate = 60;
    std::string err;
    CHECK(c.setup(s, err) && c.start(path, err));
    const double times[] = {0, 0.015, 0.035, 0.050, 0.0667, 0.070, 0.0833, 0.1167, 0.1333};
    for (int n = 0; n < 9; n++) {
        const std::vector<uint32_t> px = picture(n, 64, 48);
        c.picture(px.data(), 64, 48, times[n]);
    }
    printf("%s\n", c.stop().c_str());
    // from 0.035 s: 0, 0.9, 1.9, 2.1 (too soon), 2.9, 4.9, 5.9 -> frames 0 1 2 3 5 6
    CHECK(c.stats().too_soon == 1);
    Decoded d;
    CHECK(decode(path, d));
    CHECK(d.rate.den > 0 && std::fabs(av_q2d(d.rate) - 60) < 1e-4);
    std::vector<int64_t> ms;
    for (int64_t p : d.pts) ms.push_back(p * d.vtb.num * 1000 / d.vtb.den);
    CHECK((ms == std::vector<int64_t>{0, 17, 33, 50, 83, 100}));
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "test_record DIR\n");
        return 2;
    }
    const std::string dir = argv[1];
    hk::sys::make_dirs(dir);
    CHECK(hk::host::record::available());
    {   // an FFmpeg built before tools/ffmpeg-min.sh had the recorder's parts (a cached build folder): skipped, said
        Capture c;
        std::string err;
        if (!c.setup(Capture::Setup{}, err)) {
            printf("test_record: %s: skipped\n", err.c_str());
            return 77;
        }
    }
    printf("formats: %s\n", hk::host::record::formats().c_str());
    basic(dir);
    sizes(dir);
    behind(dir);
    uneven(dir);
    h264(dir);
    // a file's name: the game's, the time, and a number when one is there already
    const std::string a = hk::host::record::file_name(dir, "game", "ffv1");
    CHECK(a.size() > dir.size() + 20 && a.compare(a.size() - 4, 4, ".mkv") == 0);
    if (FILE *f = fopen(a.c_str(), "wb")) fclose(f);
    const std::string b = hk::host::record::file_name(dir, "game", "ffv1");
    CHECK(b != a && b.compare(b.size() - 6, 6, "-2.mkv") == 0);
    remove(a.c_str());
    if (g_fail) fprintf(stderr, "test_record: %d failed\n", g_fail);
    else printf("test_record: all hold\n");
    return g_fail ? 1 : 0;
}
