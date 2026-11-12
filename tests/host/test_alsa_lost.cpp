// SPDX-License-Identifier: GPL-3.0-or-later
// host/alsa.h against the pretend libasound (fake_asound.c, through LD_LIBRARY_PATH): a card
// pulled out is said to be lost at once, and a card that is gone or stuck still closes at once.
// Arguments: the file whose presence pulls the card out, the file whose presence makes it play
// nothing (as the test's environment names them to the pretend library).
#include <SDL3/SDL.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "host/alsa.h"

namespace alsa = hk::host::alsa;
using Clock = std::chrono::steady_clock;

static int g_failed = 0;
#define CHECK(what, ok) \
    do { \
        if (!(ok)) printf("FAILED: %s (line %d)\n", what, __LINE__), g_failed++; \
    } while (0)

static void touch(const char *f) {
    if (FILE *fp = fopen(f, "w")) fclose(fp);
}
static double ms_since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }
static void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
// the share of its rate's frames the card took, from describe()'s line
static double took(const std::string &line) {
    double pct = -1;
    sscanf(line.c_str(), "audio: the card took %lf", &pct);
    return pct;
}

int main(int argc, char **argv) {
    if (argc < 3) return printf("usage: test_alsa_lost GONE-FILE STUCK-FILE\n"), 2;
    const char *gone = argv[1], *stuck = argv[2];
    remove(gone), remove(stuck);
    const SDL_AudioSpec src{SDL_AUDIO_F32, 2, 44100};
    SDL_AudioStream *stream = nullptr;
    std::string what;

    std::string why = alsa::open("hw:CARD=Pretend,DEV=0", src, 128, stream, what);
    CHECK("the pretend card opens", why.empty() && stream);
    if (!why.empty()) return printf("  (%s: is the pretend libasound on LD_LIBRARY_PATH?)\n", why.c_str()), 1;
    sleep_ms(400);
    CHECK("a card that plays isn't lost", !alsa::lost());
    const double pace = took(alsa::describe());
    CHECK("it takes its rate's frames", pace > 90 && pace < 110);

    // pulled out
    touch(gone);
    auto t = Clock::now();
    while (!alsa::lost() && ms_since(t) < 1000) sleep_ms(1);
    const double said_ms = ms_since(t);
    CHECK("pulled out: lost", alsa::lost());
    CHECK("... said within 150 ms", said_ms < 150);
    t = Clock::now();
    alsa::close();
    CHECK("a card that is gone closes at once", ms_since(t) < 300);
    CHECK("closed: no card to report on, and none lost (what plays next isn't)", alsa::describe().empty() && !alsa::lost());
    SDL_DestroyAudioStream(stream), stream = nullptr;

    // still out: it doesn't open (the window then plays on the system's default)
    why = alsa::open("hw:CARD=Pretend,DEV=0", src, 128, stream, what);
    CHECK("still out: no card to open", !why.empty() && !stream);

    // back in: it opens again, and its report is the new card's alone
    remove(gone);
    why = alsa::open("hw:CARD=Pretend,DEV=0", src, 128, stream, what);
    CHECK("back in: it opens", why.empty() && stream && !alsa::lost());
    sleep_ms(400);
    const double pace2 = took(alsa::describe());
    CHECK("... and its pace is counted from this opening", pace2 > 90 && pace2 < 110);

    // stuck (it takes nothing, and says nothing): that isn't this file's to judge (the window sees
    // no pull from the queue: host/soundwatch.h), but closing mustn't wait on it
    touch(stuck);
    sleep_ms(500);
    CHECK("stuck: not said to be lost", !alsa::lost());
    t = Clock::now();
    alsa::close();
    CHECK("a stuck card closes at once", ms_since(t) < 300);
    SDL_DestroyAudioStream(stream);
    remove(stuck);

    printf(g_failed ? "%d checks FAILED\n" : "alsa, a card lost: all checks hold\n", g_failed);
    return g_failed ? 1 : 0;
}
