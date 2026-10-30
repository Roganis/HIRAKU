// SPDX-License-Identifier: GPL-3.0-or-later
// HIRAKU's RtEffect.dll for the C02 cabinet's later games (beatmania IIDX RED, HAPPY SKY and
// DistorteD, PLAN.md section 36).
// Konami's is Realtek's sound effects library for the cabinet PC's sound chip (an equaliser, the
// speakers' mode); on anything else it stops the game, and bemanitools stubs it out. From RED on the
// game has no DLL of the board's for HIRAKU to take the place of (the board's library is built into
// the program: ezusb_dev.c answers under it), so this one is HIRAKU's way in: it starts the cabinet
// (cabinet.c) and the board, sets up a new cabinet, and the five calls the game makes (cdecl, as
// bm2dx.exe calls them) do nothing and say they did. Every call Konami's DLL exports is here
// (rteffect.def).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "cabinet.h"
#include "c02_setup.h"
#include "ezusb_dev.h"

#define CALL __cdecl
#define FINE(name) int CALL name(int a, int b, int c, int d) { (void)a, (void)b, (void)c, (void)d; return TRUE; }

FINE(EnableEqualizer)
FINE(GetEqualizerGain)
FINE(GetEqualizerStatus)
FINE(GetGlobalEnvironment)
FINE(GetMaximumChannels)
FINE(GetRTSEVersion)
FINE(GetSPDIFOut44KMode)
FINE(GetSpdifOutCaps)
FINE(GetSpdifOutStatus)
FINE(GetSpeakerMode)
FINE(Is_Realtek_Audio_Driver_Exist)
FINE(SetEqualizerGain)
FINE(SetGlobalEnvironment)
FINE(SetSPDIFOut44KMode)
FINE(SetSpdifOutStatus)
FINE(SetSpeakerMode)

BOOL WINAPI DllMain(HINSTANCE h, DWORD why, LPVOID r) {
    (void)h, (void)r;
    if (why == DLL_PROCESS_ATTACH) {
        hk_ezusb_dev_start();
        hk_cabinet_start();
        hk_c02_set_up_cabinet();
        hk_c02_movies();
        hk_c02_debug_lines();
    }
    if (why == DLL_THREAD_ATTACH) hk_watch_thread();   // (debug: HK_PC_WATCH)
    return TRUE;
}
