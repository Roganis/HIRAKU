// SPDX-License-Identifier: GPL-3.0-or-later
// The picture in the window: sharp pixels, or a CRT through a libretro slang shader preset
// converted to GLSL 3.30 by tools/slang2glsl.py (shaders/<name>/: preset.slangp, passN.vert,
// passN.frag, parameters.txt, lut/). Three of them:
//
//   light  crt-guest-advanced-fast (guest(r)): scanlines, mask, glow; cheap
//   full   crt-royale (TroggleMonkey): beam profiles, real phosphor masks, bloom, halation
//   lola   Lola Retro (Zoki): the analog chain (the signal, the TV's decoder, the beam, the mask,
//          the glass), its presets as looks (shaders/lola-retro/looks/); the heaviest
//
// The engine does what RetroArch's filter chain does: pass sizes (source, viewport,
// absolute), 8-bit / sRGB / fp16 targets, each output sampled with the next pass's filter,
// wrap mode and mipmaps, the preset's textures, and the semantics (MVP, SourceSize,
// OriginalSize, OutputSize, FinalViewportSize, <alias>Size, FrameCount, parameters), the frames
// before this one (OriginalHistoryN) and a pass's output a frame ago (PassFeedbackN,
// <alias>Feedback; black at first, as in RetroArch).
//
// Parameters: parameters.txt's defaults, then shaders/<name>/hiraku.params (HIRAKU's own
// defaults), shaders/<name>/hiraku-<k>khz.params (for the cabinet's monitor: set_monitor), then
// <dir>/<name>.params for each config dir, in order (the launcher's
// global settings, then the game's). Those files are `name = value` lines (a RetroArch
// preset works); they are read again when they change, so a running game follows them.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hk::host::picture {

enum class Mode { off, light, full, lola };
const char *mode_name(Mode m);            // "off", "light", "full", "lola"
bool parse_mode(const std::string &s, Mode &m);
const char *mode_label(Mode m);           // "sharp pixels", "CRT light (crt-guest-advanced-fast)", ...

// Needs a current OpenGL 3.3 core context. The presets are loaded the first time they are drawn.
bool init(const std::string &shaders_dir, const std::vector<std::string> &config_dirs);
// The CRT beam simulator (Blur Busters, MIT: beam_shader.h): on a display faster than the game, each
// refresh lights the part of the picture the electron beam is sweeping then and lets the rest fade,
// each pixel's light spread over as few refreshes as its brightness allows: a CRT's motion clarity
// on an LCD or OLED. It goes after the picture mode (any of them: the mode runs once a frame, the
// beam pass, three texture reads a pixel, every refresh). The window decides when it applies
// (window.h); false if it can't run here.
struct Beam {
    float gamma = 2.2f;      // the display's gamma: a mismatch shows as bands and washed-out colour
    float gain = 0.7f;       // brightness against motion blur (1: as bright as without, less sharp)
    bool sideways = false;   // the monitor turned on its side: the beam runs left to right in each tube
    int tubes = 1;           // monitors side by side in the frame (Keyboardmania: 2)
};
bool set_beam(bool on, const Beam &b = {});
// The cabinet's monitor, in kHz of line rate (15: TV-like arcade monitors; 24: medium resolution,
// DJ Main; 31: VGA-like, Firebeat): the shaders' settings for it (hiraku-<k>khz.params). 0: none.
void set_monitor(int khz);
// The settings of a mode's preset (its parameters), for changing them in the game (host/shaderview.h):
// false while that preset isn't loaded. A heading in the list ("--- Signal ---") has min == max.
struct Setting {
    std::string name, label;
    float value;           // in use
    float saved;           // as the files give it (the shader's, HIRAKU's, the settings folders')
    float preset;          // the shader's and HIRAKU's alone (for this monitor): the default
    float min, max, step;
    bool changed;          // set_setting since the last save
};
bool settings(Mode m, std::vector<Setting> &out);
void set_setting(Mode m, const std::string &name, float value);   // at once, until saved or the game ends
// Write the settings changed here into the settings folder's file (init's config_dirs): all games'
// (the first; the game's own file then lets go of them) or this game's (the last); message says how
// it went. The launcher's Picture dialog keeps the same files.
bool save_settings(Mode m, bool this_game, std::string &message);
unsigned settings_generation();   // turns when a preset's settings are read again (their files changed)
// Load a mode's preset now (compiling its shaders takes up to a second the first time; the
// draw that needs it otherwise does). False if it can't run here.
bool prepare(Mode m);
// Draw a w x h XRGB8888 frame into the rectangle (x, y, vw, vh), from the top left, of a
// win_w x win_h default framebuffer; the rest is black. Returns the mode drawn: `m`, or off
// when its preset can't run here.
// view: which window (0, or 1 for a second one: its own frame texture and preset state)
// frame: the game's frame number, the shaders' FrameCount. The same number as the last draw's is a
// refresh of that frame (smooth motion's in-between pictures): its picture is replaced, but
// FrameCount, the frames before it and the passes' output a frame ago stay as they were, so a
// shader's interlaced fields and afterglow keep the game's pace on a faster display.
// UINT64_MAX: every draw a new frame, counted from the first.
// beam_phase, frames_per_hz: with the CRT beam on (set_beam), this draw is one display refresh of
// the beam: beam_phase is where the beam is when the refresh starts, in frames after the frame's
// moment (pacing::phase: 0 its top, 1 the next frame's), frames_per_hz the display's refreshes in
// a frame of the game. A new frame's picture (the mode's) is kept, not shown; each refresh shows the
// beam's pass over the last two. -1: no beam for this draw.
Mode draw(const uint32_t *rgb, int w, int h, int x, int y, int vw, int vh, int win_w, int win_h, Mode m, int view = 0,
          uint64_t frame = UINT64_MAX, double beam_phase = -1, double frames_per_hz = 0);
// Where the window's picture is drawn: 0, the window's own framebuffer, or (Windows: swapchain.h) one
// presented through a DXGI swap chain
void set_window_fbo(unsigned fbo);
// After draw: an ARGB picture (w x h, with its own alpha, times `alpha`) blended over the window,
// drawn at (x, y, dw, dh) from the top left, its pixels sharp (host/notice.h's banner)
void overlay(const uint32_t *argb, int w, int h, int x, int y, int dw, int dh, float alpha, int win_w, int win_h);
void shutdown();

// HK_PICTURE_SNAP=FRAME[-LAST],FILE.ppm writes the window's picture at that game frame (its first
// draw: checking a shader; a range: FILE-<frame>.ppm each, and a frame's later refreshes
// FILE-<frame>-<n>.ppm: smooth motion's in-between pictures as the window shows them).
// HK_PICTURE_PASSES=1 prints each pass's GPU time at exit (it reads the timers back every frame: for
// finding a preset's costly passes, not for playing).

}  // namespace hk::host::picture
