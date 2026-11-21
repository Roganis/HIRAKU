// SPDX-License-Identifier: GPL-3.0-or-later
// HIRAKU's ezusb_video.dll for the C33 cabinet (ee'MALL 1st Avenue): no code of its own. Each of
// Konami's video board's calls is forwarded (ezusb_video_c33.def) to HIRAKU's ezusb.dll
// (ezusb_c33.c), which keeps the I/O board and the video board, and the cabinet's link, together.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE h, DWORD why, LPVOID r) {
    (void)h, (void)why, (void)r;
    return TRUE;
}
