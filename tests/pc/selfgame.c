// SPDX-License-Identifier: GPL-3.0-or-later
// HIRAKU's own "game" for hiraku-pc (tests/pc/test_selfgame.sh): a small Windows program that does
// what a PC game does to a cabinet, so that the whole way between a game and the
// shell is run with no game's data: it loads the board's DLL (HIRAKU's ezusb.dll, which hooks this
// program's imports as it does a game's), reads the pad through it, draws with Direct3D 9, plays a
// sound with DirectSound, lights the buttons' lamps, and reads and writes files by the cabinet's
// drive letters. Everything it shows says what it saw, so a snapshot can be read back:
//
//   the picture, 160x120 (small: a test keeps one of every frame): the ground's red is 0xff while
//   button 1 is held (else 0x20), its green the frame's number times two, its blue 0x80 if the
//   data file on drive D: read "hello" (else 0); a white square 10 pixels wide moves right a pixel
//   a frame (at x = the frame's number, modulo 150: each picture names its frame); a second,
//   yellow one stands at x = 4 * the coins counted;
//   the sound: a 441 Hz square for a quarter of a second each time button 1 goes down;
//   the lamps: each button's while it is held;
//   drive E: (the backup drive): selfgame.txt, written at the tenth frame.
//
// It ends when the Test switch is pressed, or its window is closed.
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmreg.h>
#include <d3d9.h>
#include <dsound.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int(__cdecl *start_t)(int);
typedef int(__cdecl *pad_read_t)(unsigned long *);
typedef int(__cdecl *lamp_t)(int);
typedef int(__cdecl *coin_get_t)(int);

static LRESULT CALLBACK window_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(w, msg, wp, lp);
}

static int fail(const char *what) {
    printf("selfgame: %s\n", what);
    fflush(stdout);
    return 1;
}

int main(void) {
    // the cabinet's board: Konami's names for its calls (a C++ library's)
    const HMODULE board = LoadLibraryA("ezusb.dll");
    if (!board) return fail("no ezusb.dll beside the program");
    const start_t usb_start = (start_t)(void *)GetProcAddress(board, "?usbStart@@YAHH@Z");
    const pad_read_t usb_pad_read = (pad_read_t)(void *)GetProcAddress(board, "?usbPadRead@@YAHPAK@Z");
    const lamp_t usb_lamp = (lamp_t)(void *)GetProcAddress(board, "?usbLamp@@YAHH@Z");
    const coin_get_t usb_coin_get = (coin_get_t)(void *)GetProcAddress(board, "?usbCoinGet@@YAHH@Z");
    if (!usb_start || !usb_pad_read || !usb_lamp || !usb_coin_get) return fail("the board's DLL lacks a call");
    usb_start(0);

    // the data, by the drive letter a cabinet has it at
    int data_ok = 0;
    HANDLE f = CreateFileA("D:\\selfgame\\data\\hello.txt", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        char text[8] = {0};
        DWORD got = 0;
        ReadFile(f, text, 5, &got, NULL);
        CloseHandle(f);
        data_ok = got == 5 && !memcmp(text, "hello", 5);
    }

    WNDCLASSA wc = {0};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "hiraku-selfgame";
    RegisterClassA(&wc);
    const HWND window = CreateWindowExA(0, wc.lpszClassName, "HIRAKU self-test game", WS_OVERLAPPEDWINDOW, 40, 40, 640, 480, NULL, NULL, wc.hInstance, NULL);
    if (!window) return fail("no window");
    ShowWindow(window, SW_SHOW);

    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) return fail("no Direct3D 9");
    D3DPRESENT_PARAMETERS pp = {0};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth = 160, pp.BackBufferHeight = 120;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.hDeviceWindow = window;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    IDirect3DDevice9 *dev = NULL;
    if (FAILED(IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev)) || !dev)
        return fail("no Direct3D 9 device");

    // the sound: a quarter of a second of a 441 Hz square, 44.1 kHz 16-bit stereo
    IDirectSound8 *ds = NULL;
    IDirectSoundBuffer *tone = NULL;
    if (FAILED(DirectSoundCreate8(NULL, &ds, NULL)) || !ds) return fail("no DirectSound");
    IDirectSound8_SetCooperativeLevel(ds, window, DSSCL_PRIORITY);
    WAVEFORMATEX wf = {0};
    wf.wFormatTag = WAVE_FORMAT_PCM, wf.nChannels = 2, wf.nSamplesPerSec = 44100, wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4, wf.nAvgBytesPerSec = 44100 * 4;
    DSBUFFERDESC bd = {0};
    bd.dwSize = sizeof bd;
    bd.dwFlags = DSBCAPS_GLOBALFOCUS | DSBCAPS_CTRLVOLUME;
    bd.dwBufferBytes = 11025 * 4;
    bd.lpwfxFormat = &wf;
    if (FAILED(IDirectSound8_CreateSoundBuffer(ds, &bd, &tone, NULL)) || !tone) return fail("no sound buffer");
    void *p1 = NULL, *p2 = NULL;
    DWORD n1 = 0, n2 = 0;
    if (FAILED(IDirectSoundBuffer_Lock(tone, 0, bd.dwBufferBytes, &p1, &n1, &p2, &n2, 0)) || n1 != bd.dwBufferBytes) return fail("the sound buffer can't be written");
    for (DWORD k = 0; k < 11025; k++) {
        const int16_t v = (int16_t)(k % 100 < 50 ? 8000 : -8000);
        ((int16_t *)p1)[2 * k] = v, ((int16_t *)p1)[2 * k + 1] = v;
    }
    IDirectSoundBuffer_Unlock(tone, p1, n1, p2, n2);

    printf("selfgame: started (data %s)\n", data_ok ? "read" : "NOT read");
    fflush(stdout);
    unsigned long before = 0;
    unsigned presses = 0;
    for (unsigned frame = 0;; frame++) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto out;
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        unsigned long pad = 0;
        usb_pad_read(&pad);
        const unsigned buttons = (unsigned)(pad >> 8) & 0x1ff;
        if ((pad & ~before) & 0x100) {   // button 1 went down: its sound, from the start
            presses++;
            IDirectSoundBuffer_SetCurrentPosition(tone, 0);
            IDirectSoundBuffer_Play(tone, 0, 0, 0);
        }
        before = pad;
        usb_lamp((int)buttons);
        const int coins = usb_coin_get(0);
        if (frame == 10) {   // the backup drive: what was seen so far
            char line[96];
            const int len = snprintf(line, sizeof line, "selfgame: data %s, frame %u\r\n", data_ok ? "read" : "NOT read", frame);
            HANDLE out = CreateFileA("E:\\selfgame.txt", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (out != INVALID_HANDLE_VALUE) {
                DWORD wrote = 0;
                WriteFile(out, line, (DWORD)len, &wrote, NULL);
                CloseHandle(out);
            }
        }
        IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(buttons & 1 ? 0xff : 0x20, (frame * 2) & 0xff, data_ok ? 0x80 : 0), 1.0f, 0);
        const LONG x = (LONG)(frame % 150);
        const D3DRECT white = {x, 50, x + 10, 60}, yellow = {4 * coins, 80, 4 * coins + 10, 90};
        IDirect3DDevice9_Clear(dev, 1, &white, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0xff, 0xff, 0xff), 1.0f, 0);
        IDirect3DDevice9_Clear(dev, 1, &yellow, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0xff, 0xff, 0), 1.0f, 0);
        IDirect3DDevice9_BeginScene(dev);
        IDirect3DDevice9_EndScene(dev);
        if (pad & 0x80) break;   // the Test switch: the operator turns the game off
        IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
    }
out:
    printf("selfgame: ended, button 1 pressed %u times\n", presses);
    fflush(stdout);
    IDirectSoundBuffer_Release(tone);
    IDirectSound8_Release(ds);
    IDirect3DDevice9_Release(dev);
    IDirect3D9_Release(d3d);
    return 0;
}
