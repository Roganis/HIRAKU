// SPDX-License-Identifier: GPL-3.0-or-later
// hiraku-pc64 (PLAN.md section 32): the 64-bit PC pop'n (model M39) run
// by a loader of HIRAKU's own. The loader starts Konami's AVS2 libraries and the game's DLL as the
// cabinet's bootstrap does, and puts the cabinet around the game at the game's imports and AVS2's:
// two screens, the BI3A board, the AMI2000 reader, the license key, the network card, the touch
// screen, the sound. Konami's files stay as they are. After spice2x (GPL-3.0) where it says so.
#pragma once
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0601   // touch input
#include <windows.h>
#include <stdint.h>
#include <stdio.h>

// log.c: the loader's log (stderr, or HK_PC64_LOG's file)
void pc64_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
FILE *pc64_log_file(void);

// imports.c: a module's import slot of DLL!NAME (or DLL's ordinal) pointed at FN; the old target,
// or NULL when the module doesn't import it
void *pc64_import(HMODULE m, const char *dll, const char *name, void *fn);
void *pc64_import_ordinal(HMODULE m, const char *dll, WORD ordinal, void *fn);
void pc64_patch(void *slot, void *fn);   // a pointer (an import or a vtable's) pointed at FN

// The pieces, each hooked into the game's DLL (game) or AVS2's core (avs) before the game starts
void pc64_files_install(HMODULE game, const char *game_dir);   // files.c: the data mounted from the game's folder
const char *pc64_data_folder(void);        // files.c: the game's data folder's name (plain_data, or data)
const char *pc64_game_folder(void);        // ... the game's folder ("": the current one)
void pc64_display_install(HMODULE game);   // display.c: two screens on one adapter
void pc64_board_install(HMODULE game);     // board.c: the BI3A board, the AMI2000 reader, the license key
void pc64_net_install(HMODULE avs);        // net.c: the network card, e-AMUSEMENT's port, the keepalive
void pc64_touch_install(HMODULE game);     // touch.c: the touch screen
void pc64_sound_install(HMODULE game);     // sound.c: the game's WASAPI output
void pc64_script_start(void);              // script.c: HK_SCRIPT's presses and touches (tests)
void pc64_faults_install(void);            // log.c: C++ exceptions thrown in the game, logged

// The game's state, as the loader sees it
extern volatile int pc64_scene;            // the game's last CSequence::Attach (its log)
extern volatile unsigned pc64_frames;      // main screen pictures presented
extern volatile unsigned pc64_sub_frames;  // the sub screen's
extern volatile DWORD pc64_board_t0;       // when the game first read its board (GetTickCount)

// Inputs held (script.c sets them; later the link): bit 0 TEST, 1 SERVICE, 2 coin, 3-11 buttons
// 1-9, 12 and 13 the big red and blue buttons, 14 headphones
#define PC64_IN_TEST (1u << 0)
#define PC64_IN_SERVICE (1u << 1)
#define PC64_IN_COIN (1u << 2)
#define PC64_IN_BUTTON(n) (1u << (2 + (n)))
#define PC64_IN_RED (1u << 12)
#define PC64_IN_BLUE (1u << 13)
#define PC64_IN_HEADPHONES (1u << 14)
extern volatile unsigned pc64_held;

// link.c: hiraku-pc's link (runtime/pc/link.h), when HK_PC_LINK names it
struct hk_pc_link *pc64_link(void);
int pc64_host_shows(void);             // HIRAKU's window shows the main screen
unsigned pc64_link_inputs(void);       // the inputs it holds (PC64_IN_*), coins as pulses
unsigned pc64_link_buttons(void);      // the buttons alone held now (PC64_IN_BUTTON), nothing counted
unsigned pc64_link_keypad(void);       // player 1's 10-key held (link.h: bits 0-9 the digits, HK_PC_KEY_*)
extern volatile unsigned pc64_keys_held;   // ... and HK_SCRIPT's
uint32_t *pc64_link_pixels(void);      // where the main screen's picture goes (0x00RRGGBB)
void pc64_link_picture(unsigned w, unsigned h);   // it's there: counted, then wait for the next frame

// touch.c: a touch on the sub screen at (x, y) of its 1280x800: down, held 160 ms, up
void pc64_touch(int x, int y);
// touch.c: a mouse click posted to the sub screen's window there (the mouse's way to a touch)
void pc64_click(int x, int y);
// link.c: the card held to the reader (a count that moves on with each) and its UID; HK_SCRIPT's
// "card" adds one
uint32_t pc64_card(uint8_t uid[8]);
extern volatile unsigned pc64_card_presses;
extern volatile unsigned pc64_cards_given;   // board.c: cards given to the game
// log.c: HK_PC64_TRACE_STRUCTS's trace of the game's answer nodes and struct reads (tests)
void pc64_trace_install(HMODULE game);
// log.c: HK_PC64_TRACE_D3DX's trace of the game's D3DX textures, effects and shaders (tests)
void pc64_trace_d3dx_install(HMODULE game);
// link.c: the picture being drawn won't be shown by hiraku-pc (no need to read it back)
int pc64_link_not_shown(void);
// crt.c: the single-screen mode (HK_PC64_OUTPUT=crt): on; the board's inputs for its touches (those
// the game gets back: none of the buttons while the touch screen is shown); the touch screen shown
// instead of the main screen, and its cursor (1280x800)
int pc64_crt(void);
unsigned pc64_crt_inputs(unsigned in);
int pc64_crt_view(int *x, int *y);
void pc64_crt_cursor(int x, int y);   // (HK_SCRIPT's cursor:X:Y)
void pc64_crt_install(HMODULE game);
void pc64_songs_install(HMODULE game);   // songs.c: songs the game left out, back from the mods' music data   // crt.c: its play screen's characters, scaled to the 4:3 picture
// link.c: a fifth of a millisecond's nap
void pc64_nap(void);
