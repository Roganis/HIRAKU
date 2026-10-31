// SPDX-License-Identifier: GPL-3.0-or-later
// The cabinet's sound effects for beatmania IIDX: the effector (the Effect and VEFX buttons, the
// five sliders beside them). GOLD to EMPRESS's here; 10th style to DistorteD set the same through
// RtEffect.dll (rteffect.c), whose environments are numbered as below.
//
// The game doesn't make the effects itself: it reads its buttons and sliders, works out an
// environment (a reverb or an echo, by the VEFX slider), a key shift (PITCH, -4 to 4) and an
// equaliser's 10 bands (the low-EQ, hi-EQ and filter sliders), and sets them in the cabinet PC's
// sound driver, Realtek's, through a COM object of the driver's (its CLSID below), whose effects
// work on everything the PC plays. Without that driver the game's CoCreateInstance fails, the game
// notes it has no effects and sets none: the effector's labels change on the screen and the sound
// doesn't. So with HIRAKU's sound (dsound.c) the game gets this object instead, which says it is
// there, takes what the game sets and puts it in the link, and hiraku-pc applies it to the game's
// sound (runtime/host/effector.h). Its methods are those the game calls (by their place in the
// vtable, stdcall); any other one stops the game with a line in the log, as its arguments are
// unknown.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cabinet.h"
#include "link.h"

// {C6606CA5-2108-4CAF-8E52-1953F2DBF716}, and the interface the game asks for,
// {CF743829-4C33-457D-82CB-A00DE3308CD5}
static const GUID CLSID_driver = {0xc6606ca5, 0x2108, 0x4caf, {0x8e, 0x52, 0x19, 0x53, 0xf2, 0xdb, 0xf7, 0x16}};
static const GUID IID_driver = {0xcf743829, 0x4c33, 0x457d, {0x82, 0xcb, 0xa0, 0x0d, 0xe3, 0x30, 0x8c, 0xd5}};
static const GUID IID_unknown = {0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

// ---------------------------------------------------------------- the programs
// The effector's programs, as runtime/host/effector.h numbers them
enum { FX_NONE, FX_ECHO_1, FX_ECHO_2, FX_ECHO_3, FX_ECHO_4, FX_ECHO_5, FX_REVERB_1, FX_REVERB_2, FX_REVERB_3,
       FX_SMALL_ROOM, FX_ROOM, FX_HALL, FX_OUTDOORS, FX_UNDERWATER };

// The drivers' environments are 0, none, then the 23 places EAX has, from 1 (generic, padded cell,
// room, bathroom, living room, stone room, auditorium, concert hall, cave, arena, hangar, carpeted
// hallway, hallway, stone corridor, alley, forest, city, mountains, quarry, plain, parking lot,
// sewer pipe, underwater: IIDX 10th style has a label for each, in that order). Those a game's VEFX
// slider reaches are what its labels call them, ECHO or REVERB; any other is a kind of place
static int place(int env) {
    static const uint8_t kind[24] = {FX_NONE, FX_ROOM, FX_SMALL_ROOM, FX_SMALL_ROOM, FX_SMALL_ROOM, FX_SMALL_ROOM,
                                     FX_SMALL_ROOM, FX_HALL, FX_HALL, FX_HALL, FX_HALL, FX_HALL, FX_ROOM, FX_ROOM,
                                     FX_ROOM, FX_OUTDOORS, FX_OUTDOORS, FX_OUTDOORS, FX_OUTDOORS, FX_OUTDOORS,
                                     FX_OUTDOORS, FX_ROOM, FX_ROOM, FX_UNDERWATER};
    return env >= 0 && env < 24 ? kind[env] : FX_ROOM;
}

// RtEffect.dll's (IIDX 10th style to DistorteD: rteffect.c). The VEFX slider, bottom to top,
// chooses 17, 16, 20, 5, 2, 3, 13, 15, which RED, HAPPY SKY and DistorteD's labels call REVERB 3,
// 2, 1, ECHO 1-5, as 10th style's own labels for those places do
int hk_fx_program_rt(int env) {
    switch (env) {
    case 5: return FX_ECHO_1;
    case 2: return FX_ECHO_2;
    case 3: return FX_ECHO_3;
    case 13: return FX_ECHO_4;
    case 15: return FX_ECHO_5;
    case 20: return FX_REVERB_1;
    case 16: return FX_REVERB_2;
    case 17: return FX_REVERB_3;
    default: return place(env);
    }
}

// The HD driver's (GOLD to EMPRESS), numbered the same. The game's VEFX slider, bottom to top,
// chooses 16, 20, 18, 2, 15, 5, 3 (and GOLD and DJ TROOPERS 12 at the top), which its labels call
// REVERB 3, 2, 1, ECHO 1, 2, 3, 4 (and 5): other places than 10th style's
static int program_hd(int env) {
    switch (env) {
    case 2: return FX_ECHO_1;
    case 15: return FX_ECHO_2;
    case 5: return FX_ECHO_3;
    case 3: return FX_ECHO_4;
    case 12: return FX_ECHO_5;
    case 18: return FX_REVERB_1;
    case 20: return FX_REVERB_2;
    case 16: return FX_REVERB_3;
    default: return place(env);
    }
}

static int g_driver_made;   // the game made the HD driver's object (GOLD to EMPRESS)
static int g_log = -1;

static void say(const char *fmt, int a, int b) {
    if (g_log < 0) g_log = getenv("HK_PC_EFFECTOR_LOG") != NULL;
    if (g_log) hk_log(fmt, a, b);
}

static void changed(void) {
    struct hk_pc_link *l = hk_link();
    MemoryBarrier();
    l->fx_seq++;
}

// What the game set, to hiraku-pc
void hk_fx_program(int program, int env) {
    hk_link()->fx_program = program;
    changed();
    say("effector: environment %d: program %d", env, program);
}
void hk_fx_band(int band, int db) {
    if (band < 0 || band > 9) return;
    hk_link()->fx_eq_db[band] = (int8_t)(db < -12 ? -12 : db > 12 ? 12 : db);
    changed();
    say("effector: band %d %d dB", band, db);
}
void hk_fx_equaliser(int on) {
    hk_link()->fx_eq_on = on != 0;
    changed();
    say("effector: equaliser %d", on, 0);
}
// The speakers' volume, as the game sets it in Windows' mixer (dsound.c's, 0-65535): GOLD to
// EMPRESS keep it at 52.5 of 127 and, with the effector on, set it by the play volume slider, from
// 38.5 at the bottom to 70.5 at the top (7 is 52.5). Other games' volume is their own, as before:
// only the effector's games' goes to hiraku-pc, as a part of 52.5 (thousandths)
void hk_fx_volume(unsigned value) {
    if (!g_driver_made) return;
    const unsigned milli = (unsigned)(value * 127.0 * 1000 / (52.5 * 65535) + 0.5);
    hk_link()->fx_volume_milli = milli;
    changed();
    say("effector: volume %d (thousandths of the usual)", (int)milli, 0);
}
static void fx_key(int key) {
    hk_link()->fx_key = key;
    changed();
    say("effector: key %d", key, 0);
}

// ---------------------------------------------------------------- the HD driver's COM object
#define SLOTS 154   // the last the game calls is 153 (+0x264)

struct driver {
    void **vt;
    LONG refs;
    int environment, speakers, eq_on, key;
};
static struct driver g_driver;
static void *g_vt[SLOTS];
static HRESULT __stdcall drv_QueryInterface(struct driver *self, REFIID riid, void **out) {
    if (!out) return E_POINTER;
    if (IsEqualGUID(riid, &IID_unknown) || IsEqualGUID(riid, &IID_driver)) {
        *out = self;
        InterlockedIncrement(&self->refs);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG __stdcall drv_AddRef(struct driver *self) { return (ULONG)InterlockedIncrement(&self->refs); }
static ULONG __stdcall drv_Release(struct driver *self) {
    const LONG n = InterlockedDecrement(&self->refs);
    return (ULONG)(n > 0 ? n : 0);
}
// 7: the speakers' arrangement (the sound test menu's SPEAKER SELECT, 0-4): kept
static HRESULT __stdcall drv_SetSpeakers(struct driver *self, int n) {
    self->speakers = n;
    say("effector: speakers %d", n, 0);
    return S_OK;
}
// 8: the driver's there (the game takes no effects otherwise)
static HRESULT __stdcall drv_Present(struct driver *self, int *yes) {
    (void)self;
    if (yes) *yes = 1;
    return S_OK;
}
// 117, 118: the environment, 0-23 (0 none: program_hd)
static HRESULT __stdcall drv_GetEnvironment(struct driver *self, int *n) {
    if (n) *n = self->environment;
    return S_OK;
}
static HRESULT __stdcall drv_SetEnvironment(struct driver *self, int n) {
    if (n < 0 || n > 23) return E_INVALIDARG;
    self->environment = n;
    hk_fx_program(program_hd(n), n);
    return S_OK;
}
// 144: an equaliser band's gain (0-9, 31 Hz to 16 kHz), dB (-12 to 12)
static HRESULT __stdcall drv_SetBand(struct driver *self, int band, int db) {
    (void)self;
    if (band < 0 || band > 9) return E_INVALIDARG;
    hk_fx_band(band, db);
    return S_OK;
}
// 149: the equaliser on or off
static HRESULT __stdcall drv_SetEqualiser(struct driver *self, int on) {
    self->eq_on = on != 0;
    hk_fx_equaliser(on);
    return S_OK;
}
// 150-152: called by none of the game's code (kept for its wrappers' sake)
static HRESULT __stdcall drv_Get150(struct driver *self, int *v) {
    (void)self;
    if (v) *v = 0;
    return S_OK;
}
static HRESULT __stdcall drv_Set151(struct driver *self, int v) {
    (void)self, (void)v;
    return S_OK;
}
static HRESULT __stdcall drv_Set152(struct driver *self, void *v) {
    (void)self, (void)v;
    return S_OK;
}
// 153: the key shift, semitones (-4 to 4)
static HRESULT __stdcall drv_SetKey(struct driver *self, int key) {
    if (key < -4 || key > 4) return E_INVALIDARG;
    self->key = key;
    fx_key(key);
    return S_OK;
}

// Any other: the game stopped, the slot said (its arguments, which stdcall leaves to the callee,
// unknown). Each slot's stub is `mov eax, slot; jmp unknown`
void __cdecl hk_effector_unknown(int slot);
void __cdecl hk_effector_unknown(int slot) {
    hk_log("hiraku-pc: the game called the sound driver's method %d, which HIRAKU doesn't know; stopping", slot);
    ExitProcess(3);
}
static __attribute__((naked)) void unknown_entry(void) {
    __asm__ volatile("push %eax\n\tcall _hk_effector_unknown\n\tint3");
}

static void make_vtable(void) {
    uint8_t *stubs = (uint8_t *)VirtualAlloc(NULL, SLOTS * 10, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    for (int i = 0; i < SLOTS; i++) {
        uint8_t *s = stubs + 10 * i;
        s[0] = 0xb8;   // mov eax, i
        memcpy(s + 1, &i, 4);
        s[5] = 0xe9;   // jmp unknown_entry
        const int32_t rel = (int32_t)((uintptr_t)unknown_entry - (uintptr_t)(s + 10));
        memcpy(s + 6, &rel, 4);
        g_vt[i] = s;
    }
    DWORD old;
    VirtualProtect(stubs, SLOTS * 10, PAGE_EXECUTE_READ, &old);
    g_vt[0] = (void *)drv_QueryInterface;
    g_vt[1] = (void *)drv_AddRef;
    g_vt[2] = (void *)drv_Release;
    g_vt[7] = (void *)drv_SetSpeakers;
    g_vt[8] = (void *)drv_Present;
    g_vt[117] = (void *)drv_GetEnvironment;
    g_vt[118] = (void *)drv_SetEnvironment;
    g_vt[144] = (void *)drv_SetBand;
    g_vt[149] = (void *)drv_SetEqualiser;
    g_vt[150] = (void *)drv_Get150;
    g_vt[151] = (void *)drv_Set151;
    g_vt[152] = (void *)drv_Set152;
    g_vt[153] = (void *)drv_SetKey;
}

typedef HRESULT(WINAPI *CoCreateInstance_t)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID *);

static HRESULT WINAPI my_CoCreateInstance(REFCLSID clsid, LPUNKNOWN outer, DWORD ctx, REFIID riid, LPVOID *out) {
    static CoCreateInstance_t real;
    if (clsid && IsEqualGUID(clsid, &CLSID_driver)) {
        if (!g_driver.vt) {
            g_driver_made = 1;
            make_vtable();
            g_driver.vt = g_vt;
            hk_log("hiraku-pc: the game's sound effects (its effector) are HIRAKU's");
        }
        if (!out) return E_POINTER;
        return drv_QueryInterface(&g_driver, riid, out);
    }
    if (!real) real = (CoCreateInstance_t)(void *)GetProcAddress(LoadLibraryA("ole32.dll"), "CoCreateInstance");
    return real ? real(clsid, outer, ctx, riid, out) : REGDB_E_CLASSNOTREG;
}

void *hk_effector_hook(const char *fn) {
    return !strcmp(fn, "CoCreateInstance") ? (void *)my_CoCreateInstance : NULL;
}
