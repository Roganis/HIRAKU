// SPDX-License-Identifier: GPL-3.0-or-later
// The cabinet around a PC game, from inside its process (PLAN.md section 30): HIRAKU's own
// ezusb.dll and libacio.dll take the place of Konami's, and the first of them to load also turns
// the game's paths and its Direct3D window into what HIRAKU wants. Set by hiraku-pc through the
// environment:
//   HK_PC_LINK    the file shared with hiraku-pc (runtime/pc/link.h); without it the DLLs run
//                 on their own, on a built-in keyboard layout
//   HK_PC_MAP     where the game's paths go: "FROM=TO;FROM=TO" (D:\popn15\data=Z:\...), a FROM
//                 matching a path's start, any case
//   HK_PC_WINDOW  "WxH": the game in a window of that size, its picture scaled to it; "full": a
//                 window covering the screen, the picture scaled to its height; "" or unset: as the
//                 game asks (full screen at its own mode)
//   HK_PC_HZ      the most pictures a second the game shows (its monitor's rate, default 60): a
//                 window shows as many as the desktop's refresh would let a game made for 60 Hz
//   HK_PC_LOG     a file for the DLLs' log (default: standard error)
//   HK_PC_FILES   mods: a file of "GAME'S PATH<tab>MOD'S FILE" lines, the game's files a mod's take
//                 the place of (whole)
//   HK_PC_PATCHES mods: a file of "ADDRESS ONCE ORIGINAL REPLACE" lines (hex; ORIGINAL "-": none),
//                 memory patches put in place as docs/MODS.md says, before each picture
//   HK_PC_THREADLOG for bringing a game up: each thread the game's modules start (its routine,
//                 and where it was started from), to the log
//   HK_PC_PRESS_TIME 1: presses judged by when they happened, not by the frame the game read them
//                 in (pop'n 15-18; press.c)
//   HK_PC_JUDGE_OFFSET milliseconds taken off each press's time before it's judged (positive: the
//                 player presses later, for a display or sound that comes late)
//   HK_PAD_SCRIPT for tests: a file of "SECONDS BITS MS" lines, the pad bits held MS milliseconds
//                 that many seconds after the board started (bit 22: a coin)
#pragma once
#include <stdint.h>

#include "link.h"

void hk_log(const char *fmt, ...);
// The game's program: the module HK_PC_PROGRAM names (a DLL a loader starts: popn22.dll, PLAN.md
// section 35), else the process's EXE. Its code is where the game's own routines are looked for
void *hk_program(void);   // (an HMODULE)
struct hk_pc_link *hk_link(void);   // never null: an empty one when hiraku-pc gave none
void hk_cabinet_start(void);        // once: the path and window hooks (DllMain, process attach)
// The pad as the game reads it: hiraku-pc's, the keys bound to it, the test script's
uint32_t hk_pad_now(void);
// hiraku-pc's time now, in microseconds (link.h: go_host_us): 0 when it can't be told (no frame
// started yet, no hiraku-pc), else *us
int hk_host_us(uint32_t *us);
// The board's latest samples, newest first, for usbPadReadLast (pop'n 16-18): buttons 1-9 in bits
// 0-8, from the pad's timed changes (link.h: pad_ev) at the board's rate; how many are new since
// the last call. press.c
int hk_pad_samples(uint16_t last[10]);
// pop'n 15-18: presses judged by when they happened (HK_PC_PRESS_TIME=1), moved by
// HK_PC_JUDGE_OFFSET milliseconds (later presses: a display that shows late); press.c
void hk_press_patch_program(void);
uint32_t hk_coins_now(void);
void hk_set_coin_mode(int mode);   // usbCoinMode: 1, coins as the coin switch's pulses on the pad
uint32_t hk_keypad_now(void);   // HK_PC_KEY_* and digits: hiraku-pc's and the keys bound to them
uint32_t hk_card_seq(void);     // moves on with each card: hiraku-pc's and the card key's
uint32_t hk_keypad2_now(void);  // the same for a second reader (the C02 cabinet's player 2): hiraku-pc's only
uint32_t hk_card_seq2(void);
// The network's calls that lead to e-AMUSEMENT (net.c): the hook for a ws2_32 call, or NULL
void *hk_net_hook(const char *fn);
void *hk_net_card_hook(const char *fn);   // iphlpapi's
// NET TAISEN through HIRAKU's relay (vsock.c): the hook for one of libsocket's calls, or for
// libavs's ea3_query and avs_net_ctrl, or NULL
void *hk_vsock_hook(const char *fn);
// AVS2's network calls, for the loader of the 32-bit AVS2 games (vsock.c; exported from ezusb.dll)
__declspec(dllexport) void *hk_avs_net(const char *what, void *real);
void *hk_comm_hook(const char *fn);   // libcomm's, for HK_PC_NET_LOG
// pop'n 16-18: libsocket built into the program, its calls made jumps to the relay's (vsock.c)
void hk_vsock_patch_program(void);
// The pictures, in HIRAKU's window: what the Direct3D 9 hooks (cabinet.c) and the Direct3D 8 ones
// (d3d8.c) share
int hk_host_shows(void);                     // hiraku-pc's window shows the game (the link says so)
void *hk_patch_slot(void **slot, void *to);  // a pointer (a vtable's, an import's) set; what it was, or NULL
void hk_place_window(void *hwnd);            // the game's window, hidden while HIRAKU's shows it
void hk_put_pixels(const void *bits, int pitch, unsigned w, unsigned h, int format);   // D3DFORMAT's numbers
void hk_present_begin(void);                 // each Present: the mods' patches, the debug watch
void hk_picture_drawn(int captured);         // a picture (in the link if captured): counted, then wait for the next frame
void *__stdcall hk_Direct3DCreate8(unsigned sdk);   // d3d8.c: the game's Direct3D 8 (beatmania IIDX 9th style)
// HIRAKU's DirectSound (dsound.c): the game's, when HIRAKU's window shows it
long __stdcall hk_DirectSoundCreate8(const void *device, void **out, void *outer);
long __stdcall hk_DirectSoundCreate(const void *device, void **out, void *outer);
// A picture's sound mixed into the link's ring at once, as the picture is counted (link.h:
// audio_frame_end); nothing before the game makes its DirectSound
void hk_dsound_picture(void);
