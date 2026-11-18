// SPDX-License-Identifier: GPL-3.0-or-later
// The game recorded to a video file (--record, F6 in the window): the machine's own frames at their
// own size, before any picture mode, scaling or smooth motion's in-between pictures, with its sound,
// both timed by the machine's clock and not the wall clock. A frame the PC took long over is still
// one frame of the file at its time, and a run faster than real time (the fast start-up, a headless
// run) makes the same file as one at the game's speed.
//
// What is written: FFV1 (lossless, version 3 with checksums: every pixel as the machine made it) and
// FLAC (the sound as --wav has it, 16-bit) in Matroska (.mkv), with FFmpeg's own encoders, in every
// build: the PCs' players that use FFmpeg's decoders (VLC, mpv, MPC-HC, PotPlayer), its editors
// (Shotcut, Kdenlive, Olive, Avidemux) and FFmpeg itself (to convert) read it; Windows' own player
// and browsers don't. --record-format h264: H.264 and AAC in MP4, which plays everywhere and is about
// a sixth of the size, for sharing. Its encoder (--record-encoder): x264, on the processor, or
// a graphics card's own (NVIDIA's NVENC, AMD's AMF, Intel's Quick Sync, VAAPI or Vulkan video on
// Linux, Media Foundation on Windows: any card's), in a constant-quality mode; "auto" takes a card's
// when one opens here and encodes a first picture, else x264; a run that keeps no real time always
// takes x264 (its file the same every time). Before 4:2:0 halves the colour's resolution, the
// picture may be enlarged by a whole number (nearest pixels; --record-scale), so pixel art stays
// sharp: by default only with a card's encoder (it costs the processor nothing there).
// A lossless recording becomes such a file afterwards with convert() (--convert-recording).
//
// The FFmpeg: the other cores link tools/ffmpeg-min.sh's static libraries (CMakeLists.txt; x264 and
// the cards' encoders that load their driver only when asked, tools/enc-deps.sh); the Python core
// links the shared ones PCSX2's own video capture loads at run time, so the process has one copy
// (runtime/python/CMakeLists.txt).
//
// The frames: each picture's time is the machine's seconds since the file began, at the frames'
// own rate (measured from the machine's clock when its frames come steadily, else the core's
// Core::native_rate): frame N at N / rate. The sound: its samples in order, as the core gave them,
// from the file's first picture. One size a file: the first frame's. A frame of another size is
// held back until it is clear whether that size stays: when it has for half a second (30 frames)
// the next file (NAME-2.mkv, ...) begins at it, from its first frame; a size that comes and goes
// sooner (a mode switch's odd frame) is scaled into this file (nearest pixels). The pixels' shape
// (Core::display_aspect, and the part of the screen a frame covers: Core::display_cover) is in the
// file, so a player shows 512x480 on its 4:3 screen.
//
// Encoding is a thread of its own, behind a queue: the game never waits for it while it keeps the
// game's time (a window), the queue taking whatever the encoder hasn't done yet, up to half the
// memory free at the start (at most 8 GB). Past that a frame is left out (the one before it then
// shows longer, the sound keeps its time), said in the log. A run that keeps no real time (headless,
// the fast start-up) waits for the encoder instead, so it leaves nothing out and its file is the same
// every time.
//
// After a normal end, the stop key or a closed window the file is complete. After a crash, a
// Matroska file is there up to its last few seconds (no index: it plays, and `ffmpeg -i in.mkv -c
// copy out.mkv` writes it whole); an MP4 file is unreadable.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace hk::host::record {

// This build can record (it has FFmpeg's libraries)
bool available();
// The formats this build can write ("ffv1", and "h264" where an H.264 encoder is linked), for --help
std::string formats();

class Capture {
public:
    struct Setup {
        std::string format = "ffv1";   // ffv1 (.mkv) or h264 (.mp4)
        // h264's encoder: auto (a graphics card's that works here, else x264; x264 while waiting),
        // gpu (a card's, even while waiting; x264 if none works), x264, or an FFmpeg encoder's name
        // (that one only: h264_mf is then Microsoft's own, in software)
        std::string encoder = "auto";
        int scale = 0;                  // h264: enlarged this many times (0: auto, scale_for)
        int audio_rate = 44100;         // Core::audio_rate
        double native_rate = 59.94;     // Core::native_rate: the frames' rate when their times vary
        double aspect = 4.0 / 3.0;      // Core::display_aspect (0: square pixels)
        bool wait = false;              // the game waits for the encoder (no real time kept)
        size_t memory = 0;              // the queue's most bytes (0: half the memory free, 256 MB to 8 GB)
        // Core::display_cover: the part of the screen a w x h frame covers (none: all of it), asked on
        // picture()'s thread with each frame recorded
        std::function<void(int w, int h, double &cw, double &ch)> cover;
    };
    Capture();
    ~Capture();
    bool setup(const Setup &s, std::string &error);   // false: no such format here
    void set_wait(bool w);   // as Setup::wait, while recording (the fast start-up's end)

    // Every frame's picture, from the hook whose picture is recorded: cheap while nothing records.
    // rgb as the cores give it (0x00RRGGBB), w x h; seconds: Core::seconds
    void picture(const uint32_t *rgb, int w, int h, double seconds);
    // The sound, from the core's audio sink (any thread): stereo, at Setup::audio_rate
    void sound(const float *lr, int frames);

    // A file from the next picture on (its folder made); false and why if it can't be begun
    bool start(const std::string &path, std::string &error);
    // The end of it: waits for the encoder to finish the queue, closes the file(s). A line for the
    // log ("" if nothing was recording)
    std::string stop();
    // The end of it without waiting (the stop key: the game goes on meanwhile): a thread of its own
    // finishes the file and then calls done with the log's line, on that thread. A new recording can
    // begin at once; stop() and the destructor wait for every such thread
    void stop_later(std::function<void(const std::string &line, const std::string &path)> done);
    bool on() const;
    std::string path() const;   // the file being written (the first of its parts)

    struct Stats {
        uint64_t frames = 0;         // written, in all parts
        uint64_t dropped = 0;        // left out, the queue full
        uint64_t too_soon = 0;       // left out, a frame within the file's last frame time (faster than its rate)
        uint64_t samples = 0;        // sound samples (per channel) written
        int64_t length = 0;          // the video's length in frames of its rate (the last frame's time + 1, all parts)
        size_t queue_high = 0;       // the queue's most frames
        size_t queue_high_bytes = 0;
        double encode_ms = 0;        // the worker's time encoding and writing, all frames
        double worker_cpu_ms = 0;    // its thread's CPU time
        int parts = 0;
        int w = 0, h = 0;            // the last part's size (enlarged)
        double rate = 0;             // its frame rate
        std::string encoder;         // the video encoder taken (FFmpeg's name: ffv1, libx264, h264_vaapi...)
    };
    Stats stats() const;   // the last recording's (after stop)

    struct Impl;
private:
    std::unique_ptr<Impl> d;
};

// A new file's name in `folder`: <game>-YYYYMMDD-HHMMSS.<ext of the format> (local time), -2, -3...
// when one is there already
std::string file_name(const std::string &folder, const std::string &game, const std::string &format);

// The H.264 encoder taken for `pref` (Setup::encoder; wait: Setup::wait): each of h264_order's
// names tried in turn with try_one, which says "" when it opened here and encoded a first picture,
// else why not; the first that did. `why`: the ones passed over and why, for the log. "" if none did
std::vector<std::string> h264_order(const std::string &pref, bool wait);
std::string choose_h264(const std::string &pref, bool wait, const std::function<std::string(const std::string &name)> &try_one,
                        std::string &why);
// The whole number a w x h picture is enlarged by in `format` with Setup::scale `scale` and the
// encoder taken (h264_order's name): a number given, else (auto) the most within 1920x1440 with a
// graphics card's encoder, the game's own size with the processor's (x264, Microsoft's h264_mf)
int scale_for(const std::string &format, int scale, int w, int h, const std::string &encoder);

// A lossless recording (FFV1 in Matroska, this recorder's) to H.264 and AAC in MP4, as --record-format
// h264 would have written it: every frame at its time, the sound, the pixels' shape; `in`'s later
// parts (NAME-2.mkv...) to out's (OUT-2.mp4...). out "": in's name, .mp4. s: its encoder and scale
// (format, rates and shape come from the file). progress: 0..1, now and then. line: for the log.
// A recording cut short or damaged (a crash, a power cut: its reader or decoder complains) is
// converted as far as it can be read: true, *ended_early set, and line begins with a "convert: IN
// ended early: N frames (S s) kept" line for each such part
bool convert(const std::string &in, const std::string &out, const Capture::Setup &s, const std::function<void(double)> &progress,
             std::string &line, std::string &error, bool *ended_early = nullptr);
// The names convert() writes for in and out (the first part's; out "": in's, .mp4)
std::string converted_name(const std::string &in, const std::string &out);

// The mark over the window's picture while recording (not in the file: the window draws it over the
// game's picture): a red dot and REC. pixels ARGB, w x h, at a 360-line screen's size
struct Mark {
    const std::vector<uint32_t> *pixels;
    int w, h;
    unsigned id;
};
void set_mark(bool on);
bool mark(Mark &m);

}  // namespace hk::host::record
