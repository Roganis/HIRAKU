// SPDX-License-Identifier: GPL-3.0-or-later
// Screenshots (F5 in the window): the game's own picture as a PNG file, the frame the recorder
// takes (host/record.h), before any picture mode, smooth motion or what the window draws over it
// (the lights, the performance overlay, notices), in the shape of the game's screen: where its
// pixels aren't square (the shape a recording of the frame says, Core::display_aspect with
// Core::display_cover) the picture is enlarged along one axis only, lines added where the pixels
// are taller than wide (a 640x240 frame on a 4:3 screen: 640x480) and columns where they are wider
// (512x448 on 4:3: 597x448), each new pixel the mean of the source pixels it covers. Square pixels
// leave the frame as it is. One file a press, named by the game and the time:
// <game>-YYYYMMDD-HHMMSS.png in the screenshots folder (made with the first one), -2, -3... for
// more in the same second.
//
// The frame is copied on the game's thread (a fraction of a millisecond); the picture is shaped,
// made a PNG and written by a thread of its own, one after another, so the game never waits for it.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hk::host::screenshot {

// A new file's name in `folder` for a screenshot taken at `when`: <game>-YYYYMMDD-HHMMSS.png (local
// time), -2, -3... when that one is there already
std::string file_name(const std::string &folder, const std::string &game, std::time_t when);

// The pixels' shape, width over height, of a w x h frame on a screen of `aspect` (Core::display_aspect,
// 0: square pixels) that the frame covers cw x ch of (Core::display_cover): the recorder's sample
// aspect ratio (host/record.cpp)
double pixel_shape(double aspect, double cw, double ch, int w, int h);
// The size a w x h frame of pixels `shape` (pixel_shape) is saved at: enlarged along one axis to
// square pixels (the nearest whole pixel; at most 8192), else w x h
void shaped_size(int w, int h, double shape, int &W, int &H);
// rgb (w x h) enlarged to W x H, one axis at a time (W >= w, H >= h): each new pixel the mean of the
// part of the source it covers, a channel rounded to the nearest; a whole-number factor repeats
// pixels exactly
std::vector<uint32_t> stretch(const uint32_t *rgb, int w, int h, int W, int H);

class Saver {
public:
    // done: on the saver's thread, after each picture: ok and the file's path, or not and why
    using Done = std::function<void(bool ok, const std::string &path_or_why)>;
    ~Saver();   // waits for the pictures still being written
    // This frame (0x00RRGGBB, w x h, pixels `shape`: pixel_shape) into `folder` as game's next
    // screenshot; returns at once
    void take(const uint32_t *rgb, int w, int h, double shape, const std::string &folder, const std::string &game, Done done);
    void wait();   // every picture taken so far written (or failed)

private:
    struct Job {
        std::vector<uint32_t> rgb;
        int w = 0, h = 0;
        double shape = 1.0;
        std::time_t when = 0;
        std::string folder, game;
        Done done;
    };
    void run();
    std::mutex m_;
    std::condition_variable cv_, idle_;
    std::deque<Job> jobs_;
    bool busy_ = false, quit_ = false;
    std::thread thread_;
};

// One picture made and written now, on this thread (the saver's work; tests): true and its path in
// `out`, else false and why
bool save(const uint32_t *rgb, int w, int h, double shape, const std::string &folder, const std::string &game,
          std::time_t when, std::string &out);

}  // namespace hk::host::screenshot
