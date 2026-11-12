// SPDX-License-Identifier: GPL-3.0-or-later
// SDL3 front end: shows the GCU output, paces virtual time to the wall clock,
// and maps the keyboard onto the cabinet's inputs.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hk::host {

// Before window_open: the picture (runtime/host/picture.h): mode off, light or full, the
// presets in shaders_dir, settings layered from config_dirs. F10 cycles the modes.
void window_set_picture(const std::string &mode, const std::string &shaders_dir, const std::vector<std::string> &config_dirs);
// Before window_open: display pacing (runtime/host/pacing.h), on by default.
void window_set_display_sync(bool on);
// Before window_open: the sound's way out. `device` "" is the system's default (the sound server);
// an ALSA device name (hw:CARD=Generic,DEV=0) is that card alone, with no sound server in between
// (if it's busy, the default instead). `frames` is the device buffer (0: 128 frames).
// device: Linux, an ALSA device to have alone ("": the system's default output); Windows, an
// output device by the name Windows shows. exclusive (Windows): that device, or the default one,
// in WASAPI exclusive mode (host/wasapi.h). frames: the device buffer (0: 128)
void window_set_audio_output(const std::string &device, int frames, bool exclusive = false);
// Windows: a card through its ASIO driver by name (host/asio.h; "": none), before the others
void window_set_audio_asio(const std::string &driver);
// Before window_open: the volume in dB (0: as the machine makes it). The sound to the device also
// loses any steady offset (a DC blocker, as a cabinet's amplifier's coupling capacitors), so a chip
// stuck at full scale can't push the rest into clipping. Neither changes --wav or --digest.
void window_set_volume(double db);
// Before window_open: the audio queue target stays where window_open set it (no self-tuning)
void window_set_audio_fixed(bool on);
// Before the machine starts its threads (they inherit it): the game ahead of other programs.
// Linux: the calling thread at nice -11, or as near as RLIMIT_NICE or RealtimeKit allow, timer
// slack 1 us, and GameMode asked for if it's installed (until window_close). Windows: the process
// above normal. A line for the log. (A sound card of its own,
// --audio-device, is written by a real-time thread regardless.)
std::string raise_priority();
// After the machine has opened its media: the game's binary and the files it has open, up to 1 GB
// each, read into the page cache by a thread of the lowest CPU and disk priority, so that play
// doesn't wait on the disk for a page of code run the first time or a disc's sector (on a hard disk,
// a seek each: 10-60 ms). Linux
void preload_media();
// Before window_open: each monitor of a machine with several (Core::screens: Keyboardmania's
// two) in a window of its own, for a monitor each; player 2's goes on the next display
void window_set_split(bool on);
// --smooth-motion: with display pacing and a display faster than the game (k refreshes a frame),
// the k - 1 refreshes after each frame show the core's in-between pictures (Core::render_between)
// instead of holding the frame
void window_set_smooth_motion(bool on);
bool window_smooth_motion();   // --smooth-motion asked for
// --crt-beam: the Blur Busters CRT beam simulator (host/picture.h, set_beam) over the picture on a
// display faster than the game: each refresh lights the part of the tube the beam sweeps then. It
// needs what smooth motion does (display pacing, a picture every refresh: Wayland's fifo-v1 or
// Windows' DWM) and takes the place of its in-between pictures. gamma: the display's; gain:
// brightness against motion blur.
void window_set_beam(bool on, float gamma = 2.2f, float gain = 0.7f);
// --lights-overlay: the game's lights over its picture from the start (F9 toggles it; host/lampview.h)
void window_set_lights_overlay(bool on);
// --shader-key ACTION=BINDING: one of the in-game shader settings' controls (host/shaderview.h:
// show, up, down, less, more, saved, default, save, save_game), as a bindings file spells it
// ("key:F8", "joy[DAO FPS]:button3"; empty: none); repeatable. False: no such action.
bool window_set_shader_key(const std::string &spec);
bool window_open(const char *title, int scale, const std::string &bindings, int audio_target_ms, bool fullscreen = false,
                 int audio_rate = 44100);
// Present one frame; then sleep so that `virtual_seconds` does not run ahead
// of real time. Returns false when the user closed the window.
bool window_frame(const uint32_t *rgb, int w, int h, double virtual_seconds);
// window_frame in two halves, for a core that shows its picture at the end of vertical blank
// (Core::set_present_hook): window_next at the frame's start (vblank start: wait for the frame's
// time, read the controls; false when the user closed the window), window_present at vblank end.
bool window_next(double virtual_seconds);
void window_present(const uint32_t *rgb, int w, int h, double virtual_seconds);
void window_close();
// From a signal handler (Ctrl-C): the waits on the sound's queue end, so that the frame in hand
// finishes and the game can leave at its end. Nothing else changes.
void window_interrupt();
// Queue stereo float audio for playback (at window_open()'s audio_rate) and pace the machine on it:
// the emulation waits while more than the target is queued, and controls are
// polled meanwhile. Audio is then the master clock (low, steady latency);
// without an audio device, video frames pace time instead.
void audio_push(const float *lr, int frames);
std::string audio_describe();   // device buffer / target, for the latency report

}  // namespace hk::host
