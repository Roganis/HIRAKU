// SPDX-License-Identifier: GPL-3.0-or-later
// host/soundwatch.h: when a sound device counts as stopped, and what is opened in its place.
#include <cstdio>

#include "host/soundwatch.h"

using hk::host::SoundWatch;
using Do = SoundWatch::Do;

static int g_failed = 0;
#define CHECK(what, ok) \
    do { \
        if (!(ok)) printf("FAILED: %s (line %d)\n", what, __LINE__), g_failed++; \
    } while (0)

constexpr uint64_t MS = 1'000'000, S = 1000 * MS;

// `ms` of looks every 5 ms at a device that pulls every look (or never): the first answer that
// isn't "nothing", and when it came
struct Run {
    SoundWatch &w;
    uint64_t now, pulls;
    Do last = Do::nothing;
    uint64_t at = 0;
    bool go(uint64_t ms, bool pulling, bool gone = false, bool restart = false) {
        for (const uint64_t end = now + ms * MS; now < end;) {
            now += 5 * MS;
            if (pulling) pulls++;
            last = w.look(now, pulls, gone, restart);
            if (last != Do::nothing) return at = now, true;
        }
        return false;
    }
};

int main() {
    {   // a device that plays is left alone, however long
        SoundWatch w;
        Run r{w, 50 * S, 7};
        w.opened(r.now, r.pulls);
        CHECK("an hour of pulls: nothing", !r.go(3600 * 1000, true));
    }
    {   // one that stops after it has settled: the same again, two seconds on
        SoundWatch w;
        Run r{w, 0, 0};
        w.opened(r.now, r.pulls);
        r.go(60 * 1000, true);
        const uint64_t stopped = r.now;
        CHECK("stopped: said", r.go(5000, false));
        CHECK("... to open the same again", r.last == Do::again);
        CHECK("... after two seconds", r.at - stopped >= 2 * S && r.at - stopped < 2 * S + 20 * MS);
        // opened again, it stops again at once: a lesser one
        w.opened(r.now, r.pulls);
        CHECK("stopped again", r.go(5000, false));
        CHECK("... within ten seconds of opening: a lesser one", r.last == Do::lesser);
        // the lesser one plays for a minute and stops: itself again, not a lesser one yet
        w.opened(r.now, r.pulls);
        r.go(60 * 1000, true);
        CHECK("the lesser one stopped", r.go(5000, false));
        CHECK("... after a minute: again", r.last == Do::again);
    }
    {   // a short break (a card's hiccup, 1.5 s) isn't a stop
        SoundWatch w;
        Run r{w, 0, 0};
        w.opened(r.now, r.pulls);
        r.go(20 * 1000, true);
        CHECK("1.5 s without a pull: nothing", !r.go(1500, false));
        CHECK("then pulls again, and 1.5 s without: still nothing", !r.go(100, true) && !r.go(1500, false));
    }
    {   // the back end says its device is gone: at once
        SoundWatch w;
        Run r{w, 0, 0};
        w.opened(r.now, r.pulls);
        r.go(30 * 1000, true);
        const uint64_t t = r.now;
        CHECK("gone: said", r.go(1000, false, true));
        CHECK("... at the first look", r.at - t == 5 * MS);
        CHECK("... the same again (which fails over by itself if the card is no more)", r.last == Do::again);
    }
    {   // the PC slept, or the game was stopped, for an hour: one look, a quarter second
        SoundWatch w;
        Run r{w, 0, 0};
        w.opened(r.now, r.pulls);
        r.go(30 * 1000, true);
        r.now += 3600 * S;
        CHECK("a look after an hour away: nothing", w.look(r.now, r.pulls, false, false) == Do::nothing);
        CHECK("and the device pulls again: nothing", !r.go(10 * 1000, true));
        // looks a second apart (a game loading) need eight of them
        int looks = 0;
        Do d = Do::nothing;
        while (d == Do::nothing && looks < 100) r.now += S, d = w.look(r.now, r.pulls, false, false), looks++;
        CHECK("looks a second apart: eight to call it stopped", looks == 8 && d == Do::again);
    }
    {   // a driver's request to be restarted: after half a second (they come in bursts), the same again
        SoundWatch w;
        Run r{w, 0, 0};
        w.opened(r.now, r.pulls);
        r.go(30 * 1000, true);
        const uint64_t t = r.now;
        CHECK("asked: said", r.go(5000, true, false, true));
        CHECK("... half a second on", r.at - t >= 500 * MS && r.at - t < 520 * MS);
        CHECK("... the same again", r.last == Do::again);
        // five in half a minute are granted; the sixth gets a lesser way out
        int granted = 1;
        for (; granted < 10; granted++) {
            w.opened(r.now, r.pulls);
            r.go(1000, true);
            r.go(5000, true, false, true);
            if (r.last != Do::again) break;
        }
        CHECK("five granted in a row, then a lesser one", granted == 5 && r.last == Do::lesser);
        // spread out (one a minute), they are granted for ever
        SoundWatch v;
        Run q{v, 0, 0};
        bool all = true;
        for (int i = 0; i < 20; i++) {
            v.opened(q.now, q.pulls);
            q.go(60 * 1000, true);
            q.go(5000, true, false, true);
            all = all && q.last == Do::again;
        }
        CHECK("one a minute: always the same again", all);
    }
    {   // a request taken back before the half second (the flag is the back end's): nothing
        SoundWatch w;
        Run r{w, 0, 0};
        w.opened(r.now, r.pulls);
        r.go(30 * 1000, true);
        CHECK("asked for 0.3 s, then not: nothing", !r.go(300, true, false, true) && !r.go(5000, true));
    }
    printf(g_failed ? "%d checks FAILED\n" : "soundwatch: all checks hold\n", g_failed);
    return g_failed ? 1 : 0;
}
