// SPDX-License-Identifier: GPL-3.0-or-later
// The C33 cabinet (Konami's ee'MALL 1st Avenue station): its I/O board's pad bits, as usbPadRead
// gives them (active high) and as hiraku-pc puts them in the link's pad (runtime/pc/ezusb_c33.c)
#pragma once
#define HK_PC_C33_A (1u << 0)
#define HK_PC_C33_B (1u << 1)
#define HK_PC_C33_COIN (1u << 16)      // the coin mech's switch
#define HK_PC_C33_TEST (1u << 18)
#define HK_PC_C33_SERVICE (1u << 19)

// The cabinet's frame in HIRAKU (ezusb_c33.c, core.cpp): its two screens side by side, each 640
// wide. Left: the main monitor's 640x480, centred in height. Right: the touch panel as the player
// sees it, upright, in TouchPanel.dll's own space (228 across, 300 down, as its test screen draws
// it), drawn HK_PC_C33_PX / HK_PC_C33_CTR pixels to a unit: the small display (320x240, at 1:1) in
// its top part (from HK_PC_C33_SCREEN_X, _Y, 154 units across), the keys under it
#define HK_PC_C33_FRAME_W 1280
#define HK_PC_C33_FRAME_H 624
#define HK_PC_C33_MAIN_Y 72              // the main picture's top line in the frame
#define HK_PC_C33_PANEL_X 723            // the panel's left column in the frame (centred in 640..1279)
#define HK_PC_C33_CTR_W 228              // TouchPanel.dll's space
#define HK_PC_C33_CTR_H 300
#define HK_PC_C33_PX 320                 // pixels...
#define HK_PC_C33_CTR 154                // ... to these many units
#define HK_PC_C33_SCREEN_X 37            // the small display's top-left corner on the panel, in units
#define HK_PC_C33_SCREEN_Y 24
