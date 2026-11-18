// SPDX-License-Identifier: GPL-3.0-or-later
// Recording, the launcher's side (launcher/recording.h): the game's options from Settings' choices,
// the files a conversion takes (a recording's later parts with its first), the converter's progress.
//
//   test_recording        Exit 0 when all hold
#include <cstdio>

#include "recording.h"

namespace {

int failed = 0;
void check(bool ok, const char *what) {
    printf("%s%s\n", ok ? "ok    " : "FAIL  ", what);
    failed += !ok;
}

}  // namespace

int main() {
    using recording::arguments;
    check(arguments("ffv1", "gpu", 2).isEmpty(), "exact recordings: nothing to say (the game's default)");
    check(arguments("h264", "auto", 0) == QStringList({"--record-format", "h264", "--record-encoder", "auto", "--record-scale", "auto"}),
          "for sharing: the format, the encoder, the size (auto)");
    check(arguments("h264", "x264", 1) == QStringList({"--record-format", "h264", "--record-encoder", "x264", "--record-scale", "1"}),
          "... the game's own size: 1");
    check(arguments("h264", "", 3) == QStringList({"--record-format", "h264", "--record-encoder", "auto", "--record-scale", "3"}),
          "... no encoder kept: auto");
    check(arguments("ffv1", "gpu", 2, true) == QStringList({"--record-encoder", "gpu", "--record-scale", "2"}),
          "a conversion: the encoder and size, whatever the game records in");

    const QStringList picked = {"/r/pop-1.mkv", "/r/pop-1-2.mkv", "/r/pop-1-3.mkv", "/r/ddr-2.mkv", "/r/iidx.mkv", "/r/iidx.mkv"};
    check(recording::firstParts(picked) == QStringList({"/r/pop-1.mkv", "/r/ddr-2.mkv", "/r/iidx.mkv"}),
          "a recording's parts go with its first; a part alone is converted; each once");

    check(recording::progressOf("convert: 42 %\n") == 42, "progress: the converter's line");
    check(recording::progressOf("convert: /r/a.mkv to /r/a.mp4\n") == -1, "... not another of its lines");
    check(recording::convertedName("/r/pop-1.mkv") == "/r/pop-1.mp4", "the MP4 beside the recording");
    check(recording::convertedName("/r/a.b/x.y.mkv") == "/r/a.b/x.y.mp4", "... its name up to the last dot");
    using recording::Result;
    check(recording::resultOf(true, 0) == Result::Done, "exit 0: converted");
    check(recording::resultOf(true, 3) == Result::UpToDamage, "exit 3: converted up to the damage");
    check(recording::resultOf(true, 1) == Result::Failed && recording::resultOf(false, 0) == Result::Failed, "else: not converted");
    printf(failed ? "%d FAILED\n" : "recording ok\n", failed);
    return failed ? 1 : 0;
}
