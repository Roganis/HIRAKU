// SPDX-License-Identifier: GPL-3.0-or-later
// In-between pictures (smooth motion, docs/SMOOTH.md): where a moving object is a fraction of a
// frame on. The cores match each object with the frame before's and know its last move; carried on
// by that move, notes judder, because a game scrolls at a steady speed in whole pixels: pop'n music
// 8's notes step 2, 3, 3, 2, 3, 3 pixels a frame (2.67 on average). After a 3, the in-between
// pictures carry a note 3 on, the next frame moves it 2, and it steps back a pixel; at 240 Hz about
// one refresh in twenty went back, and the notes were off a steady line by 0.57 px (sd).
//
// So the move most of a frame's moving objects share (within a pixel) is taken as the scroll (the notes, the bar
// lines: they move together), and its position kept over the last frames. What moves with it
// (within a pixel of its move, the same way) is carried along the steady line through those
// positions: the average speed, from where the line puts this frame, which can be up to a pixel
// before the frame's own position (the first refresh of a frame then draws it a little back).
// Simulated at 240 and 144 Hz for 1, 2.67, 3.5 and 4.6 pixels a frame: 0.29 px off the line (the
// whole pixels' own rounding), no step back. Everything else keeps its last move.
//
// Which object was which in the frame before (pair): each core lists, for each object, those of the
// frame before that look the same within REACH pixels. The first pairing took the nearest within 16
// pixels; something fast can be further on, and nearer another look-alike's last place. So the
// scroll is voted first, every object counted once near each move it could have made, and each
// object then takes the look-alike nearest to standing still or to the scroll's move (within NEAR
// of either).
//
// At hi-speed a note or two may be all that scrolls, and slower objects outnumber them
// (pop'n music 8 at x4, FRESH: one note at 8.4 pixels a frame against three at 2, which became
// the scroll); the notes then went by their last step, which jitters (8, 10, 8, 8, 10, 8, 7: the
// game's own positions), 1 pixel off their line in the window's pictures. So each object also keeps
// its own track, by the pairing (frame's `from`): the least-squares line through its positions
// since it appeared (Track), along which it goes when it isn't with the scroll: 0.35 pixels off
// (whole pixels alone: 0.29).
//
// Header-only: the Firebeat GCU (runtime/dev/k057714.cpp), DJ Main (runtime/djmain/board.cpp) and
// the PlayStation GPU of Twinkle and System 573 (third_party/mame/hiraku-edits.py) use it.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <utility>
#include <vector>

namespace hk {

class ScrollGuess {
public:
    static constexpr int N = 6;   // frames the line goes through
    static constexpr int REACH = 64;   // the furthest an object is looked for, each way, in the frame before
    static constexpr int NEAR = 16;    // ... and taken: this near standing still or the scroll's move
    static constexpr double JUMP = 8;  // ... and this near the look-alike's own speed, if it has one

    // That object `obj` of this frame may be `from` of the frame before, moved (dx, dy); `cost`, how
    // different it looks (0: the same); `whole`, whether it moved as one piece (only those vote).
    // A core lists each object's candidates together, objects in order
    struct Candidate { int obj, from, dx, dy, cost; bool whole; };

    // Each object's candidate (an index into `cands`), or -1; `unique`: each of the frame before's
    // objects taken once (in object order). The same for every picture of a frame (`frame`)
    std::vector<int> pair(uint64_t frame, int objects, const std::vector<Candidate> &cands, bool unique = false) {
        if (frame != m_pair_frame) {   // the frame before's scroll, a hint when two moves tie
            m_pair_frame = frame;
            m_hint_x = m_cx, m_hint_y = m_cy;
        }
        // votes: for each move, the objects with a candidate within a pixel of it. An object with its
        // exact look-alike at its own place stands still and doesn't vote: a screen of identical
        // tiles (beatmania IIDX between scenes, 598) listed 68000 candidates, 15 ms of voting
        std::vector<bool> still((size_t)objects, false);
        for (const Candidate &c : cands)
            if (!c.dx && !c.dy && !c.cost && c.obj >= 0 && c.obj < objects) still[(size_t)c.obj] = true;
        // (counted in a grid of the moves within REACH, a pixel more each way: a map of them took 10 ms
        // for 60000 candidates of moving tiles)
        constexpr int R = REACH + 1, W = 2 * R + 1;
        m_votes.assign((size_t)W * W, 0);
        m_voter.assign((size_t)W * W, 0);   // the last object counted there, + 1
        for (const Candidate &c : cands) {
            if (!c.whole || (!c.dx && !c.dy) || still[(size_t)c.obj] || std::abs(c.dx) > REACH || std::abs(c.dy) > REACH) continue;
            for (int ex = -1; ex <= 1; ex++)
                for (int ey = -1; ey <= 1; ey++) {
                    const size_t k = (size_t)(c.dx + ex + R) * W + (size_t)(c.dy + ey + R);
                    if (m_voter[k] != c.obj + 1) m_votes[k]++, m_voter[k] = c.obj + 1;
                }
        }
        int best = 0;
        for (const int v : m_votes) best = std::max(best, v);
        // and against standing still: an object that may be where it was (a look-alike at its own
        // place, its picture changed) votes for that. A row of identical sprites flickering to the
        // beat (beatmania IIDX's gauge, 20 pixels apart) otherwise voted for a move of 20 sideways,
        // each then paired with its neighbour and drawn up to a lane over: ghost notes
        int stay = 0, counted = -1;
        for (const Candidate &c : cands)
            if (!c.dx && !c.dy && !still[(size_t)c.obj] && c.obj != counted) stay++, counted = c.obj;
        bool scroll = best >= 2 && best > stay;
        int sx = 0, sy = 0;
        if (scroll) {
            // of the moves with the most, the one nearest the frame before's scroll, else the smallest;
            // the frame before's own kept while it has two thirds of the most (a stream of notes
            // evenly spaced also votes for each note having taken the place of the one ahead)
            const bool hint = m_hint_x || m_hint_y;
            auto far = [&](int mx, int my) {
                return hint ? std::fabs(mx - m_hint_x) + std::fabs(my - m_hint_y) : std::abs(mx) + std::abs(my) + 0.0;
            };
            int pick = -1, kept = -1;   // grid indexes, x major (as the moves' order)
            for (int k = 0; k < W * W; k++) {
                const int v = m_votes[(size_t)k];
                if (!v) continue;
                const int mx = k / W - R, my = k % W - R;
                if (v == best && (pick < 0 || far(mx, my) < far(pick / W - R, pick % W - R))) pick = k;
                if (hint && std::fabs(mx - m_hint_x) <= 1.5 && std::fabs(my - m_hint_y) <= 1.5 && v >= 2 && v * 3 >= best * 2 &&
                    (kept < 0 || m_votes[(size_t)kept] < v || (m_votes[(size_t)kept] == v && far(mx, my) < far(kept / W - R, kept % W - R))))
                    kept = k;
            }
            if (kept >= 0) pick = kept;
            sx = pick / W - R, sy = pick % W - R;
        }
        std::vector<int> out((size_t)objects, -1);
        std::vector<int> score((size_t)objects, 1 << 30);
        // the frame before's tracks (frame() for this frame may have come already)
        const std::vector<Track> &was = frame == m_frame ? m_was : m_tracks;
        auto rate = [&](const Candidate &c) {
            const int still = std::max(std::abs(c.dx), std::abs(c.dy));
            const int along = scroll ? std::max(std::abs(c.dx - sx), std::abs(c.dy - sy)) : 1 << 20;
            if (still > NEAR && along > NEAR) return -1;
            // not a move but a jump, unless with the scroll: away from the look-alike's own steady
            // line by more than JUMP (beatmania IIDX's key flash, lit in one lane and then the next,
            // 16 pixels on, was drawn between the two lanes: a ghost note)
            double lx, ly, vx, vy;
            if (along > 1 && c.from >= 0 && (size_t)c.from < was.size() && was[(size_t)c.from].line(lx, ly, vx, vy) &&
                (std::fabs(c.dx - vx) > JUMP || std::fabs(c.dy - vy) > JUMP))
                return -1;
            return c.cost + std::min(std::abs(c.dx) + std::abs(c.dy), scroll ? std::abs(c.dx - sx) + std::abs(c.dy - sy) : 1 << 20);
        };
        if (!unique) {
            for (size_t i = 0; i < cands.size(); i++) {
                const int r = rate(cands[i]);
                const int o = cands[i].obj;
                if (r >= 0 && r < score[(size_t)o]) score[(size_t)o] = r, out[(size_t)o] = (int)i;
            }
            return out;
        }
        std::vector<bool> taken;
        for (size_t i = 0; i < cands.size();) {   // an object's candidates, the best not yet taken
            size_t e = i;
            int at = -1, r_at = 1 << 30;
            for (; e < cands.size() && cands[e].obj == cands[i].obj; e++) {
                const int f = cands[e].from;
                if (f >= 0 && (size_t)f < taken.size() && taken[(size_t)f]) continue;
                const int r = rate(cands[e]);
                if (r >= 0 && r < r_at) r_at = r, at = (int)e;
            }
            if (at >= 0) {
                const int f = cands[(size_t)at].from;
                if (f >= 0) {
                    if ((size_t)f >= taken.size()) taken.resize((size_t)f + 1, false);
                    taken[(size_t)f] = true;
                }
                out[(size_t)cands[i].obj] = at;
            }
            i = e;
        }
        return out;
    }

    // A frame's objects' last moves (those that moved), before its in-between pictures; `frame`
    // tells a new frame from another picture of the same one (the scroll is counted once a frame).
    // `from`: each object's index in the frame before (-1: unpaired), by which each keeps its own
    // track; the same numbering the next frame's `from` uses (empty: no tracks)
    void frame(uint64_t frame, const std::vector<std::pair<int, int>> &moves, const std::vector<int> &from = {}) {
        if (frame == m_frame) return;
        const bool next = frame == m_frame + 1;
        m_frame = frame;
        std::vector<Track> tracks(from.size() == moves.size() ? moves.size() : 0);
        for (size_t k = 0; k < tracks.size(); k++) {
            if (from[k] < 0) continue;
            if (next && (size_t)from[k] < m_tracks.size()) tracks[k] = m_tracks[(size_t)from[k]];
            tracks[k].add(moves[k].first, moves[k].second);
        }
        m_was.swap(m_tracks);
        m_tracks.swap(tracks);
        std::map<std::pair<int, int>, int> count;
        for (const auto &m : moves)
            if (m.first || m.second) count[m]++;
        // the scroll: the move with the most moving objects within a pixel of it, the same way (notes
        // step 2 or 3 in the same frame, each rounding its own position; pop'n music 8's shift a
        // pixel sideways as they squash to the beat), and its speed their average
        int best = 0, best_exact = 0;
        double sx = 0, sy = 0;
        for (const auto &[m, n] : count) {
            int in = 0;
            double ax = 0, ay = 0;
            for (const auto &[o, k] : count)
                if (std::abs(o.first - m.first) <= 1 && std::abs(o.second - m.second) <= 1 &&
                    o.first * m.first + o.second * m.second > 0)
                    in += k, ax += (double)o.first * k, ay += (double)o.second * k;
            if (in > best || (in == best && n > best_exact)) best = in, best_exact = n, sx = ax / in, sy = ay / in;
        }
        m_cx = sx, m_cy = sy;
        if (best < 2) {   // nothing scrolls (one object moving is not a scroll): start again
            m_pos.clear();
            return;
        }
        if (m_pos.size() >= 2) {   // a new speed: the line starts again
            const double vx = (m_pos.back().first - m_pos.front().first) / double(m_pos.size() - 1);
            const double vy = (m_pos.back().second - m_pos.front().second) / double(m_pos.size() - 1);
            if (std::fabs(sx - vx) > 1.5 || std::fabs(sy - vy) > 1.5) m_pos.clear();
        }
        if (m_pos.empty()) m_pos.push_back({0, 0});   // the frame before
        m_pos.push_back({m_pos.back().first + sx, m_pos.back().second + sy});
        if ((int)m_pos.size() > N + 1) m_pos.erase(m_pos.begin());
    }

    // Whether an object whose last move was (dx, dy) goes with the scroll (offset() then carries it
    // along the scroll's steady line): the PC games move only those (runtime/pc/core.cpp)
    bool scrolls(int dx, int dy) const {
        return (dx || dy) && m_pos.size() >= 3 && std::fabs(dx - m_cx) <= 1.5 && std::fabs(dy - m_cy) <= 1.5 && dx * m_cx + dy * m_cy > 0;
    }

    // Where an object whose last move was (dx, dy) goes, `t` of a frame on (t from 0; a frame shown
    // for two vblanks: the fraction of its move): (ox, oy) from its place in the frame. `obj`: its
    // index in frame()'s moves, for its own track
    void offset(int dx, int dy, double t, int &ox, int &oy, int obj = -1) const {
        const bool scrolls = (dx || dy) && m_pos.size() >= 3 && std::fabs(dx - m_cx) <= 1.5 && std::fabs(dy - m_cy) <= 1.5 &&
                             dx * m_cx + dy * m_cy > 0;
        if (!scrolls) {
            // not with the scroll: along its own track's steady line (Track), else by its last move.
            // At hi-speed a note or two may be all that scroll (pop'n music 8 at x4 had three slower
            // objects outnumber them: the notes then went by their last step)
            if ((dx || dy) && obj >= 0 && (size_t)obj < m_tracks.size()) {
                double lx, ly, vx, vy;
                if (m_tracks[(size_t)obj].line(lx, ly, vx, vy) && vx * dx + vy * dy > 0) {
                    ox = (int)std::lround(lx + vx * t), oy = (int)std::lround(ly + vy * t);
                    return;
                }
            }
            ox = (int)std::lround(dx * t), oy = (int)std::lround(dy * t);
            return;
        }
        // the line through the kept positions: their average speed, through their mean
        const int n = (int)m_pos.size();
        const double vx = (m_pos.back().first - m_pos.front().first) / double(n - 1);
        const double vy = (m_pos.back().second - m_pos.front().second) / double(n - 1);
        double lx = 0, ly = 0;   // where the line puts this frame: the mean of each position carried on to it
        for (int i = 0; i < n; i++) lx += m_pos[i].first + (n - 1 - i) * vx, ly += m_pos[i].second + (n - 1 - i) * vy;
        lx /= n, ly /= n;
        ox = (int)std::lround(lx - m_pos.back().first + vx * t), oy = (int)std::lround(ly - m_pos.back().second + vy * t);
    }

private:
    // An object's own steady line: the least-squares line through its positions since it appeared,
    // the older ones counted less (FORGET a frame: a speed change is followed within a few frames),
    // started again when a position is off by more than OFF. A game's own positions jitter at
    // hi-speed (pop'n music 8's notes at x4 step 8, 10, 8, 8, 10, 8, 7: up to 2 pixels off their
    // line); a line through the last six positions only moved with them, each frame's pictures
    // starting up to 2 pixels off the last's
    static constexpr double FORGET = 0.9, OFF = 4.0;
    struct Track {
        int n = 0;                                        // positions
        double w = 0, st = 0, stt = 0, sx = 0, stx = 0, sy = 0, sty = 0;   // their weighted sums (t: 0, 1...)
        double px = 0, py = 0;                            // the last position (the first at 0)
        void put(double t, double x, double y) {
            w = w * FORGET + 1, st = st * FORGET + t, stt = stt * FORGET + t * t;
            sx = sx * FORGET + x, stx = stx * FORGET + t * x, sy = sy * FORGET + y, sty = sty * FORGET + t * y;
            n++;
        }
        bool fit(double t, double &x, double &y, double &vx, double &vy) const {
            const double d = w * stt - st * st;
            if (n < 4 || d <= 1e-9) return false;   // three moves at least
            vx = (w * stx - st * sx) / d, vy = (w * sty - st * sy) / d;
            x = (sx - vx * st) / w + vx * t, y = (sy - vy * st) / w + vy * t;
            return true;
        }
        void add(int dx, int dy) {
            if (n == 0) put(0, 0, 0);   // where it was the frame before
            const double t = n, x = px + dx, y = py + dy;
            double lx, ly, vx, vy;
            if (fit(t, lx, ly, vx, vy) && (std::fabs(x - lx) > OFF || std::fabs(y - ly) > OFF)) {
                *this = Track{};   // off its line (it turned, or changed speed): from its last two positions
                put(0, 0, 0);
                put(1, dx, dy);
                px = dx, py = dy;
                return;
            }
            put(t, x, y);
            px = x, py = y;
        }
        // where its line puts it now, from its own position, and its speed
        bool line(double &ox, double &oy, double &vx, double &vy) const {
            double x, y;
            if (!fit(n - 1, x, y, vx, vy)) return false;
            ox = x - px, oy = y - py;
            return true;
        }
    };

    uint64_t m_frame = UINT64_MAX, m_pair_frame = UINT64_MAX;
    double m_hint_x = 0, m_hint_y = 0;              // the frame before's scroll, for pair()
    std::vector<int> m_votes, m_voter;               // pair()'s grid of moves
    double m_cx = 0, m_cy = 0;                     // the scroll's move this frame (its objects' average)
    std::vector<std::pair<double, double>> m_pos;   // the scroll's position, frame by frame (the last: this frame)
    std::vector<Track> m_tracks;   // each object's own line (frame()'s `from`)
    std::vector<Track> m_was;      // ... the frame before's
};

}  // namespace hk
