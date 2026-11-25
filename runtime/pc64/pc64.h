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
void **pc64_import_slot(HMODULE m, const char *dll, const char *name);   // the slot itself
void pc64_patch(void *slot, void *fn);   // a pointer (an import or a vtable's) pointed at FN

// The pieces, each hooked into the game's DLL (game) or AVS2's core (avs) before the game starts
int pc64_game_is_iidx(void);            // main.c: this run's game is a 64-bit PC IIDX (its DLL bm2dx.dll)
void pc64_keystub_install(HMODULE game); // keystub.c: the license key (kbt.dll, kld.dll), shared by the boards
void pc64_files_install(HMODULE game, const char *game_dir);   // files.c: the data mounted from the game's folder
const char *pc64_data_folder(void);        // files.c: the game's data folder's name (plain_data, or data)
const char *pc64_game_folder(void);        // ... the game's folder ("": the current one)
void pc64_display_install(HMODULE game);   // display.c: two screens on one adapter
void pc64_afp_names_install(void);         // display.c: the layouts' picture search with its answers kept (a 64-bit PC IIDX)
void pc64_board_install(HMODULE game);     // board.c: the BI3A board, the AMI2000 reader, the license key
void pc64_board_iidx_install(HMODULE game); // board_iidx.c: a 64-bit PC IIDX's BI2X TDJ board at its imports
void pc64_clock_install(HMODULE game);       // clock.c: a 64-bit PC IIDX's clock record, first-boot TEST
unsigned pc64_clock_test_held(void);         // clock.c: TEST reported held on a first boot (the board reads it)
void pc64_clock_poll(void);                  // clock.c: the clock-set once on a new backup (the board's poll calls it)
const char *pc64_ami_rom(void);              // board.c: the AMI ROM's id string (the lid; the first boot's shop name)
void pc64_net_install(HMODULE avs);        // net.c: the network card, e-AMUSEMENT's port, the keepalive
void pc64_netlog_install(HMODULE avs, HMODULE game); // net.c: HK_PC64_NET_LOG's lookups, logged and passed on
void pc64_span_start(void);             // link.c: the monitor check's span starts (main.c's avs_log: "boot_status")
void pc64_span_end(void);               // link.c: the span's end (main.c's avs_log: the title's first "CMovieManager")
void pc64_monitor_install(HMODULE game); // link.c: the monitor check's meter clock (a 64-bit PC IIDX)
void pc64_touch_install(HMODULE game);     // touch.c: the touch screen
void pc64_sound_install(HMODULE game);     // sound.c: the game's WASAPI output
void pc64_sound_pos(unsigned *written, unsigned *played);   // sound.c: the stream's counters, for HK_PC64_SYNC_LOG
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
unsigned pc64_link_pad(void);          // the pad as it is, the game's own layout of its bits
unsigned pc64_link_inputs(void);       // the inputs it holds (PC64_IN_*), coins as pulses
unsigned pc64_link_buttons(void);      // the buttons alone held now (PC64_IN_BUTTON), nothing counted
unsigned pc64_link_keypad(void);       // player 1's 10-key held (link.h: bits 0-9 the digits, HK_PC_KEY_*)
extern volatile unsigned pc64_keys_held;   // ... and HK_SCRIPT's
uint32_t *pc64_link_pixels(void);      // where the main screen's picture goes (0x00RRGGBB)
void pc64_link_picture(unsigned w, unsigned h);   // it's there: counted, then wait for the next frame
void pc64_link_pad_read(void);   // the game read its pad: set the link's pad_read_us (first read in a frame)
void pc64_note_readback(uint32_t us);   // display.c: the picture's copy to the link took us (HK_PC64_FRAME_LOG)
int pc64_frame_log_on(void);   // link.c: HK_PC64_FRAME_LOG is on
void pc64_note_present_span(uint32_t us);   // display.c: the real Present call took us (HK_PC64_FRAME_LOG)
void pc64_note_pace(uint32_t us);   // link.c: the rig pacer's own hold (HK_PC64_FRAME_LOG)
void pc64_note_devwait(int which, uint32_t us);   // display.c: WaitForVBlank(0)/GetRasterStatus(1) took us
void pc64_waits_install(HMODULE game);   // waits.c: the game's main-thread waits from its DLL (HK_PC64_FRAME_LOG)
void pc64_waits_window(void);   // waits.c: link.c's per-window top three wait sites

// touch.c: a touch on the sub screen at (x, y) of its 1280x800: down, held 160 ms, up
void pc64_touch(int x, int y);
// touch.c: the same on the MAIN screen's window at (x, y) of its 1280x720 (tests)
void pc64_mtouch(int x, int y);
// touch.c: a drag on the sub screen (a list scrolled), held still at its end before it lets go
void pc64_drag(int x0, int y0, int x1, int y1);
void pc64_touch_while(int x, int y, int (*held)(void), int ms);   // kept there while HELD says so
// touch.c: a mouse click posted to the sub screen's window there (the mouse's way to a touch)
void pc64_click(int x, int y);
// link.c: the card held to the reader (a count that moves on with each) and its UID; HK_SCRIPT's
// "card" adds one
uint32_t pc64_card(uint8_t uid[8]);
extern volatile unsigned pc64_card_presses;
extern volatile unsigned pc64_cards_given;   // board.c: cards given to the game
// log.c: HK_PC64_TRACE_STRUCTS's trace of the game's answer nodes and struct reads (tests)
void pc64_trace_install(HMODULE game);
void pc64_peek_install(HMODULE game);   // HK_PC64_PEEK: the game's memory logged (tests)
// log.c: HK_PC64_TRACE_D3DX's trace of the game's D3DX textures, effects and shaders (tests)
void pc64_trace_d3dx_install(HMODULE game);
// link.c: the picture being drawn won't be shown by hiraku-pc (no need to read it back)
int pc64_link_not_shown(void);
// crt.c: the single-screen mode (HK_PC64_OUTPUT=crt): on; the board's inputs for its touches (those
// the game gets back: none of the buttons while a menu is open; the event's big buttons)
int pc64_crt(void);
unsigned pc64_crt_inputs(unsigned in);
// crt.c: the parts of the touch screen shown over the main screen's picture (a menu, the PIN), how
// many (none: 0): each one's place on the touch screen (1280x800), where it goes on the 640x480, the
// item marked on it (touch screen places; hw 0: none)
#define PC64_PANELS 2
struct pc64_panel { int sx, sy, sw, sh, dx, dy, dw, dh, hx, hy, hw, hh; };
int pc64_crt_panels(struct pc64_panel p[PC64_PANELS]);
// crt.c: a place marked on the main screen's own picture (x, y, w, h on the 640x480), or 0
int pc64_crt_mark(int at[4]);
// crt.c: the touch screen's picture (640x400, 0x00RRGGBB), kept for the menus to find the buttons on
// it (display.c gives it, 10 times a second while wanted)
void pc64_crt_sub_picture(const uint32_t *px, int w, int h);
int pc64_crt_wants_sub(void);
void pc64_crt_install(HMODULE game);
// tables.c: the music and chart tables copied into bigger ones, the code pointed at the copies
int pc64_tables_move(HMODULE game, BYTE **music, unsigned songs, unsigned new_songs, BYTE **charts, unsigned nchart, unsigned new_nchart);
// the same, and the code sized by the counts grown with them (the new songs' slots as songs left out)
int pc64_tables_grow(HMODULE game, BYTE **music, unsigned songs, unsigned new_songs, BYTE **charts, unsigned nchart, unsigned new_nchart);
// the character table made bigger: characters past the game's own (tables.c)
int pc64_charas_grow(HMODULE game, BYTE **charas, unsigned n, unsigned new_n);
// a chart's format as the music data has it (old: 8-byte events), for the game's check of it
int pc64_chart_formats_install(HMODULE game, unsigned nchart);
void pc64_chart_format(unsigned chart, int old);
// patches.c: the mods' memory patches (HK_PC_PATCHES), put in once the game is loaded, then each frame
void pc64_patches_install(HMODULE game);
void pc64_patches_apply(void);
void pc64_songs_install(HMODULE game);   // songs.c: songs the game left out, back from the mods' music data   // crt.c: its play screen's characters, scaled to the 4:3 picture
void pc64_inspect_install(HMODULE game); // inspect.c: the debug switches' logs (HK_PC64_FILE/WIN/DISP/THREAD_LOG, off by default)
// link.c: a fifth of a millisecond's nap
void pc64_nap(void);
void pc64_frame_log_exit(void);   // link.c: HK_PC64_FRAME_LOG's exit line + thread table (main.c calls it)
