// SPDX-License-Identifier: GPL-3.0-or-later
// In-between pictures' guess (include/hiraku/scroll_guess.h): notes scrolling a steady speed in
// whole pixels (pop'n music 8's 2, 3, 3...; 1; 3.5; 4.6 a frame), shown on 240 and 144 Hz
// displays (4.1 and 2.46 refreshes a frame) as the window does, each refresh the frame carried on
// to its moment. They should stay on a steady line within the whole pixels' rounding and never
// step back; the last-move guess they replace does both (checked here too, so the test sees it).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

#include "hiraku/scroll_guess.h"

namespace {

struct Result { double sd; int back; };

// a pattern of steps a frame, seen on a display with `fph` refreshes a frame; `smooth`: the guess,
// else the last move. Three notes scroll together (the scroll needs two at least)
Result run(const std::vector<int> &pattern, double fph, bool smooth) {
    std::vector<int> pos{0};
    for (int i = 0; i < 300; i++) pos.push_back(pos.back() + pattern[i % pattern.size()]);
    hk::ScrollGuess g;
    std::vector<double> ts, ys;
    for (long j = 0;; j++) {
        const double t = j / fph;
        const int f = (int)std::floor(t);
        if (f >= 290) break;
        if (f < 1) continue;
        const int dy = pos[f] - pos[f - 1];
        g.frame((uint64_t)f, {{0, dy}, {0, dy}, {0, dy}});
        int ox = 0, oy = 0;
        if (smooth) g.offset(0, dy, t - f, ox, oy);
        else oy = (int)std::lround(dy * (t - f));
        if (f >= 10) ts.push_back(t), ys.push_back(pos[f] + oy);
    }
    // off the straight line through them (least squares)
    const double n = (double)ts.size();
    double st = 0, sy = 0, stt = 0, sty = 0;
    for (size_t i = 0; i < ts.size(); i++) st += ts[i], sy += ys[i], stt += ts[i] * ts[i], sty += ts[i] * ys[i];
    const double b = (n * sty - st * sy) / (n * stt - st * st), a = (sy - b * st) / n;
    double e = 0;
    int back = 0;
    for (size_t i = 0; i < ts.size(); i++) {
        e += std::pow(ys[i] - (a + b * ts[i]), 2);
        if (i && ys[i] < ys[i - 1]) back++;
    }
    return {std::sqrt(e / n), back};
}

}  // namespace

int main() {
    int fail = 0;
    const std::vector<std::vector<int>> patterns = {{2, 3, 3}, {1}, {3, 4}, {4, 5, 5, 4, 5}};
    for (const auto &p : patterns)
        for (double fph : {240 / 58.49, 144 / 58.49}) {
            const Result s = run(p, fph, true), l = run(p, fph, false);
            printf("steps %d%s at %.2f refreshes a frame: off the line %.2f px, %d back (the last move: %.2f px, %d back)\n",
                   p[0], p.size() > 1 ? "..." : "", fph, s.sd, s.back, l.sd, l.back);
            if (s.back || s.sd > 0.42) fail++;
        }
    // pop'n music 8's demo play: falling notes step 2 or 3 in the same frame (4 and 3 of them),
    // while 5 objects (a lyric) rise 3: the notes are the scroll, the rising ones go by their own move
    {
        hk::ScrollGuess g;
        for (uint64_t f = 1; f < 10; f++)
            g.frame(f, {{0, 2}, {0, 2}, {0, 2}, {0, 2}, {0, 3}, {0, 3}, {0, 3}, {0, -3}, {0, -3}, {0, -3}, {0, -3}, {0, -3}});
        int ox, oy, rx, ry;
        g.offset(0, 2, 0.5, ox, oy);
        g.offset(0, -3, 0.5, rx, ry);
        printf("mixed steps under a rising group: a note goes %d (the line: 2.43 a frame), a rising object %d\n", oy, ry);
        if (oy != 1 || ry != -2) fail++;   // 0.5 x 2.43 -> 1; -1.5 -> -2
    }
    // A note alone at hi-speed under three slower objects (pop'n music 8 at x4: the note steps 8,
    // 10, 8, 8, 10, 8, 7 as the game's own positions jitter; the three step 2 and are the scroll):
    // the note goes along its own track's steady line, at 240 Hz never back and near the line
    {
        const std::vector<int> pattern = {8, 10, 8, 8, 10, 8, 7};
        std::vector<int> pos{0};
        for (int i = 0; i < 200; i++) pos.push_back(pos.back() + pattern[i % pattern.size()]);
        for (bool tracks : {true, false}) {
            hk::ScrollGuess g;
            std::vector<double> ts, ys;
            const double fph = 240 / 58.49;
            for (long j = 0;; j++) {
                const double t = j / fph;
                const int f = (int)std::floor(t);
                if (f >= 190) break;
                if (f < 1) continue;
                const int dy = pos[f] - pos[f - 1];
                // objects 0-2 the slow ones, 3 the note; each the same object as the frame before
                g.frame((uint64_t)f, {{0, 2}, {0, 2}, {0, 2}, {0, dy}}, tracks ? std::vector<int>{0, 1, 2, 3} : std::vector<int>{});
                int ox = 0, oy = 0;
                g.offset(0, dy, t - f, ox, oy, 3);
                if (f >= 10) ts.push_back(t), ys.push_back(pos[f] + oy);
            }
            const double n = (double)ts.size();
            double st = 0, sy = 0, stt = 0, sty = 0;
            for (size_t i = 0; i < ts.size(); i++) st += ts[i], sy += ys[i], stt += ts[i] * ts[i], sty += ts[i] * ys[i];
            const double b = (n * sty - st * sy) / (n * stt - st * st), a = (sy - b * st) / n;
            double e = 0;
            int back = 0;
            for (size_t i = 0; i < ts.size(); i++) {
                e += std::pow(ys[i] - (a + b * ts[i]), 2);
                if (i && ys[i] < ys[i - 1]) back++;
            }
            printf("a note alone at 8.4 a frame (8, 10, 8...) under slower objects, %s: off the line %.2f px, %d back\n",
                   tracks ? "its own track" : "its last move", std::sqrt(e / n), back);
            if (tracks && (back || std::sqrt(e / n) > 0.8)) fail++;
        }
    }
    // ... and when the speed halves (a tempo change): back once at most (the frame before carried it
    // on at the old speed: no guess without delay sees a change coming), on its new line within 10 frames
    {
        std::vector<int> pos{0};
        for (int i = 0; i < 200; i++) pos.push_back(pos.back() + (i < 100 ? 8 + (i % 7 == 1 || i % 7 == 4 ? 2 : 0) : 4));
        hk::ScrollGuess g;
        int back = 0, prev = -1000, worst = 0;
        for (long j = 0;; j++) {
            const double t = j / (240 / 58.49);
            const int f = (int)std::floor(t);
            if (f >= 190) break;
            if (f < 1) continue;
            const int dy = pos[f] - pos[f - 1];
            g.frame((uint64_t)f, {{0, 2}, {0, 2}, {0, 2}, {0, dy}}, {0, 1, 2, 3});
            int ox = 0, oy = 0;
            g.offset(0, dy, t - f, ox, oy, 3);
            const int y = pos[f] + oy;
            if (y < prev) back++;
            prev = y;
            if (f >= 110) worst = std::max(worst, std::abs(y - (pos[100] + (int)std::lround((t - 100) * 4))));
        }
        printf("... its speed halving: %d back, 10 frames on at most %d px off its new line\n", back, worst);
        if (back > 1 || worst > 2) fail++;
    }
    // Pairing at hi-speed (pair): three lanes of notes, the same look in each, 40 pixels apart and
    // falling 18 or 19 a frame (each could be taken for the note ahead's last place, 21 back, or
    // be unpaired past 16), a bar line, and two still objects that look like the notes. Every note
    // that was there the frame before is paired with itself, the still ones stand still
    {
        hk::ScrollGuess g;
        struct Obj { int look, x, y; };
        auto at = [](int f) { return f * 18 + f / 3; };   // 18, 18, 19...
        auto field = [&](int f) {
            std::vector<Obj> v;
            for (int lane = 0; lane < 3; lane++)
                for (int n = 0; n < 12; n++) {   // note n enters at the top (y 0) at frame 2n + lane
                    const int y = at(f) - at(2 * n + lane);
                    if (y >= 0 && y < 400) v.push_back({1, 40 * lane, y});
                }
            const int bar = at(f) % 400;
            v.push_back({2, 0, bar});
            v.push_back({1, 200, 300});   // still, as a note looks
            v.push_back({1, 260, 300});
            return v;
        };
        int wrong = 0, paired = 0, frames = 0;
        for (int f = 1; f < 30; f++) {
            const std::vector<Obj> now = field(f), was = field(f - 1);
            std::vector<hk::ScrollGuess::Candidate> cands;
            for (size_t i = 0; i < now.size(); i++)
                for (size_t j = 0; j < was.size(); j++)
                    if (now[i].look == was[j].look && std::abs(now[i].x - was[j].x) <= hk::ScrollGuess::REACH &&
                        std::abs(now[i].y - was[j].y) <= hk::ScrollGuess::REACH)
                        cands.push_back({(int)i, (int)j, now[i].x - was[j].x, now[i].y - was[j].y, 0, true});
            const std::vector<int> pick = g.pair((uint64_t)f, (int)now.size(), cands);
            std::vector<std::pair<int, int>> moves(now.size(), {0, 0});
            for (size_t i = 0; i < now.size(); i++) {
                if (pick[i] >= 0) moves[i] = {cands[(size_t)pick[i]].dx, cands[(size_t)pick[i]].dy};
                const bool still = now[i].x >= 200;
                const int step = at(f) - at(f - 1);
                const bool was_there = still || now[i].y - step >= 0;   // entered this frame: nothing to pair
                if (!was_there) continue;
                paired++;
                if (moves[i] != (still ? std::pair<int, int>{0, 0} : std::pair<int, int>{0, step})) wrong++;
            }
            g.frame((uint64_t)f, moves);
            frames++;
        }
        printf("hi-speed pairing (18-19 pixels a frame, notes 40 apart): %d of %d objects paired wrong over %d frames\n",
               wrong, paired, frames);
        if (wrong) fail++;
    }
    // A row of identical sprites 20 pixels apart whose picture flickers each frame (beatmania
    // IIDX's gauge): each could have stood still (its picture changed) or taken its neighbour's
    // place (the same picture); it stands still, no scroll sideways
    {
        hk::ScrollGuess g;
        int wrong = 0;
        for (int f = 1; f < 10; f++) {
            std::vector<hk::ScrollGuess::Candidate> cands;
            for (int i = 0; i < 12; i++)
                for (int j = std::max(0, i - 3); j <= std::min(11, i + 3); j++)   // within REACH: 3 each side
                    cands.push_back({i, j, (i - j) * 20, 0, i == j ? 8 : 0, true});
            const std::vector<int> pick = g.pair((uint64_t)f, 12, cands);
            std::vector<std::pair<int, int>> moves(12, {0, 0});
            for (int i = 0; i < 12; i++) {
                if (pick[(size_t)i] >= 0) moves[(size_t)i] = {cands[(size_t)pick[(size_t)i]].dx, cands[(size_t)pick[(size_t)i]].dy};
                if (moves[(size_t)i] != std::pair<int, int>{0, 0}) wrong++;
            }
            g.frame((uint64_t)f, moves);
        }
        printf("a flickering row of look-alikes 20 apart: %d paired with a neighbour\n", wrong);
        if (wrong) fail++;
    }
    // A key flash lit in one lane for 9 frames, then in the next, 16 pixels on (beatmania IIDX, keys
    // played in turn): a jump, not a move (paired, it was drawn between the lanes); notes falling 1
    // a frame beside it still move
    {
        hk::ScrollGuess g;
        int jumped = 0, notes = 0;
        for (int f = 1; f < 40; f++) {
            const int lane = f / 9, was_lane = (f - 1) / 9;
            // object 0: the flash (it was object 0 too), objects 1-2: notes falling 1 a frame
            std::vector<hk::ScrollGuess::Candidate> cands = {{0, 0, (lane - was_lane) * 16, 0, 0, true},
                                                             {1, 1, 0, 1, 0, true}, {2, 2, 0, 1, 0, true}};
            const std::vector<int> pick = g.pair((uint64_t)f, 3, cands);
            std::vector<std::pair<int, int>> moves(3, {0, 0});
            for (int i = 0; i < 3; i++)
                if (pick[(size_t)i] >= 0) moves[(size_t)i] = {cands[(size_t)pick[(size_t)i]].dx, cands[(size_t)pick[(size_t)i]].dy};
            if (moves[0].first) jumped++;
            if (moves[1].second == 1 && moves[2].second == 1) notes++;
            g.frame((uint64_t)f, moves, {pick[0] >= 0 ? 0 : -1, 1, 2});
        }
        printf("a key flash lit in the next lane: carried there %d times of 4, notes moving %d frames of 39\n", jumped, notes);
        if (jumped || notes != 39) fail++;
    }
    // Following the line, a list of rows going a pixel a frame that then snaps 29 at once (beatmania
    // III's song list): the jump isn't the line's. On that frame the rows stay where the game drew
    // them (taken through the line, they were 15 pixels off on the frame's own picture) and aren't
    // carried on; a few frames after, the line goes on again
    {
        hk::ScrollGuess g;
        g.follow = true;
        const int rows = 8;
        std::vector<int> from(rows);
        for (int i = 0; i < rows; i++) from[(size_t)i] = i;
        double worst_jump = 0, after = 0;
        for (int f = 1; f < 30; f++) {
            const int step = f == 20 ? -29 : -1;
            std::vector<std::pair<int, int>> moves(rows, {0, step});
            g.frame((uint64_t)f, moves, from);
            double ox, oy;
            if (f == 20)
                for (const double t : {0.0, 0.25, 0.5, 0.75}) {
                    g.offset_exact(0, step, t, ox, oy, 0);
                    worst_jump = std::max(worst_jump, std::fabs(oy) + std::fabs(ox));
                }
            if (f == 29) g.offset_exact(0, step, 0.5, ox, oy, 0), after = oy;
        }
        printf("a list snapping 29 pixels after going 1 a frame: %.2f pixels off on that frame's pictures, %.2f half a "
               "frame on 9 frames after\n", worst_jump, after);
        if (worst_jump > 0.01 || std::fabs(after + 0.5) > 0.3) fail++;
    }
    if (fail) {
        fprintf(stderr, "%d failed\n", fail);
        return 1;
    }
    return 0;
}
