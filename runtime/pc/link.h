// SPDX-License-Identifier: GPL-3.0-or-later
// What hiraku-pc and its DLLs in a PC game's process share (PLAN.md section 30): the cabinet's
// controls, lamps, coins and card, a few words each way, and the game's pictures, which HIRAKU's
// window shows. hiraku-pc makes it a file and the DLLs map that file (runtime/pc/cabinet.c), which
// works the same under Wine on Linux, where the file is in /dev/shm, and on Windows. Plain C: the
// DLLs are C, built 32-bit (and hiraku-pc64.exe, runtime/pc64/, 64-bit: the same layout).
//
// The pictures go in step: the game draws one, the DLL copies it to the pixels, counts it in
// frame_seq and waits for frame_go = frame_seq. hiraku-pc shows the picture, and gives that when
// its pacing (runtime/host/pacing.h) starts the next frame, as an emulated machine's frame starts:
// the game reads its controls then and draws at once, its picture shown at the next refresh. With
// --game-ahead, hiraku-pc gives it as soon as it has its copy: the game draws the next while this
// one is shown (a frame ahead of the screen, as on its own monitor, never further), a frame later
// from the controls to the screen but with a whole frame for its drawing.
#pragma once
#include <stdint.h>

#define HK_PC_LINK_MAGIC 0x4b50484bu   // "HKPK"
#define HK_PC_LINK_VERSION 9u
#define HK_PC_WATCHES 8u                           // [[rate]]'s words of the game's memory

// The pad's bits, as Konami's ezusb.dll gives them to pop'n music (usbPadRead)
#define HK_PC_PAD_SERVICE (1u << 6)
#define HK_PC_PAD_TEST (1u << 7)
#define HK_PC_PAD_BUTTON(n) (1u << (7 + (n)))   // buttons 1-9: bits 8-16
// the 64-bit PC pop'n's cabinet (runtime/pc64/): its big red and blue buttons, its headphones
#define HK_PC_PAD_RED (1u << 17)
#define HK_PC_PAD_BLUE (1u << 18)
#define HK_PC_PAD_HEADPHONES (1u << 19)

// The C02 cabinet's pad (beatmania IIDX 9th style, runtime/pc/ezusb_c02.c), as its usbPadRead gives
// it: the keys 1-7 of player 1 (p 1) and 2, the starts, VEFX, Effect, test and service
#define HK_PC_C02_KEY(p, k) (1u << (7 + 7 * ((p) - 1) + (k)))   // player 1: bits 8-14, player 2: 15-21
#define HK_PC_C02_START(p) (1u << (23 + (p)))                    // 24, 25
#define HK_PC_C02_VEFX (1u << 26)
#define HK_PC_C02_EFFECT (1u << 27)
#define HK_PC_C02_TEST (1u << 28)
#define HK_PC_C02_SERVICE (1u << 29)

// The reader's 10-key: bits 0-9 its digits, then these
#define HK_PC_KEY_00 (1u << 10)
#define HK_PC_KEY_DECIMAL (1u << 11)
#define HK_PC_KEY_CLEAR (1u << 12)     // the C02 cabinet's 10-key

#define HK_PC_PAD_EVENTS 64u   // a power of two

struct hk_pc_link {
    uint32_t magic, version;   // set by hiraku-pc once the rest is ready
    // hiraku-pc to the game
    volatile uint32_t pad;          // HK_PC_PAD_* held
    volatile uint32_t coins;        // coins put in since the start
    volatile uint32_t keypad;       // HK_PC_KEY_* and digits held, player 1's reader
    volatile uint32_t card_seq;     // moves on when a card is held to the reader: card_uid is it
    uint8_t card_uid[8];
    // The game's window has the keyboard, so the DLL reads the keys bound to pad bits itself: for
    // each pad bit, up to two Windows virtual-key codes (0: none)
    uint8_t keys[32][2];
    uint8_t coin_keys[2];           // a coin each time one goes down
    uint8_t card_keys[2];           // card_uid held to the reader each time one goes down
    uint8_t keypad_keys[12][2];     // the 10-key: digits 0-9, then 00 and the decimal point
    // the game to hiraku-pc
    volatile uint32_t lamps;        // usbLamp's last bits
    volatile uint32_t ext_io;       // usbSetExtIo's last bits
    volatile uint32_t frames;       // pictures shown
    volatile uint32_t game_alive;   // moves on while the game polls its board
    // the pictures: the game's last, 0x00RRGGBB, frame_w x frame_h, at HK_PC_PIXELS from the start
    volatile uint32_t frame_w, frame_h;
    volatile uint32_t frame_seq;    // the DLL: pictures copied in
    volatile uint32_t frame_go;     // hiraku-pc: the last picture shown, the game may draw the next
    volatile uint32_t host_shows;   // hiraku-pc shows the pictures (else the game's own window does)
    // The sound: HIRAKU's DirectSound in the DLL (runtime/pc/dsound.c) mixes the game's buffers, as
    // far as hiraku-pc asks (735 frames a frame shown: 44.1 kHz at 60), into a ring of
    // HK_PC_AUDIO_FRAMES stereo float frames at HK_PC_AUDIO, which hiraku-pc plays
    volatile uint32_t audio_want;   // hiraku-pc: frames of sound asked for since the start
    volatile uint32_t audio_made;   // the DLL: frames mixed into the ring since the start
    volatile uint32_t audio_taken;  // hiraku-pc: frames taken out of the ring
    // A picture's sound comes with it: hiraku-pc sets audio_frame_end to the sound's end at the
    // frame that picture will be shown in, and the DLL, as the game counts the picture, moves
    // audio_want there and mixes, so what the game started while making it (a keysound, with the
    // controls read) comes with it. 0: not so. The DLL sets sound_on once the game has its sound
    volatile uint32_t audio_frame_end;
    volatile uint32_t sound_on;
    // The DLL, for each picture: microseconds from frame_go seen to the game's first read of its
    // pad, and to the picture counted (hiraku-pc's exit report)
    volatile uint32_t pad_read_us, drawn_us;
    // The DLL: where in the ring (audio_made then) the first sound the game started after a button
    // went down begins, counted in key_sound_seq (hiraku-pc's exit report: a button to its sound)
    volatile uint32_t key_sound_at, key_sound_seq;
    // Between the DLLs' own state (each of HIRAKU's DLLs has its own copy of the cabinet's code:
    // the pad is read in ezusb.dll's, the hooks are the first loaded's): frame_go seen (counted,
    // and when, the low 32 bits of QueryPerformanceCounter), and buttons gone down
    volatile uint32_t go_count, go_qpc, key_edges;
    // Mods (HK_PC_PATCHES): the DLL sets patch_done[i] once the i-th memory patch is in place
    volatile uint8_t patch_done[256];
    // When the pad changed, for the board's samples (pop'n 16-18) and for judging presses by when
    // they happened (--press-time). Times are hiraku-pc's, in microseconds (its steady clock, the
    // low 32 bits): go_host_us is when it gave the latest frame_go, and the DLL that sees that go
    // sets go_seen_host_us to it with go_qpc, so either DLL can tell hiraku-pc's time from its own.
    // Each change of the pad's buttons goes in pad_ev at pad_ev_seq (then counted), with its time:
    // the controller's own, a little before hiraku-pc hands it on
    // The game's last read of its pad: when (hiraku-pc's time) and pad_ev_seq then, the changes it
    // could have seen
    volatile uint32_t go_host_us, go_seen_host_us;
    volatile uint32_t pad_read_host_us, pad_ev_seen;
    volatile uint32_t pad_ev_seq;
    struct { volatile uint32_t us, pad; } pad_ev[HK_PC_PAD_EVENTS];
    // hiraku-pc, with frame_go: the picture the game draws next won't be shown (a 120 Hz game's
    // first of two on a slower display): the 64-bit loader doesn't read it back
    volatile uint32_t not_shown;
    // The C02 cabinet's (v7): hiraku-pc's turntables (0-255, players 1 and 2) and effector sliders
    // (0-15); the game's 16-segment ticker (9 characters), its top unit's lamps (spotlights 1-8) and
    // neon, and the LEDs (bits 0-3: the starts, VEFX, Effect; usbLamp's keys are in lamps)
    volatile uint8_t turntable[2], slider[5];
    volatile char ticker[9];
    volatile uint8_t top_lamp, top_neon, led;
    // (v8) the C02 cabinet's player 2: the 10-key (as keypad) and the card (as card_seq)
    volatile uint32_t keypad2, card_seq2;
    // (v9) Mods' high frame rates ([[rate]], docs/MODS.md): the words of the game's memory a rate
    // watches (hiraku-pc sets watch_addr; the DLL copies each word into watch_val before each
    // picture, 0 where it can't be read), and the game's pictures a second now (thousandths:
    // 60000 the cabinet's, more while a rate is on; the DLL counts the cabinet's ticks from it)
    volatile uint32_t watch_addr[HK_PC_WATCHES], watch_val[HK_PC_WATCHES];
    volatile uint32_t game_hz_milli;
};

#define HK_PC_PIXELS 4096u                         // where the pixels start in the file
#define HK_PC_MAX_W 1920u
#define HK_PC_MAX_H 1200u
#define HK_PC_AUDIO (HK_PC_PIXELS + HK_PC_MAX_W * HK_PC_MAX_H * 4u)
#define HK_PC_AUDIO_RATE 44100u
#define HK_PC_AUDIO_FRAMES 16384u                  // a power of two
#define HK_PC_LINK_SIZE (HK_PC_AUDIO + HK_PC_AUDIO_FRAMES * 8u)
