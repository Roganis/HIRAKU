// SPDX-License-Identifier: GPL-3.0-or-later
// A stand-in for the one third-party DLL a 64-bit PC game hard-imports but a PC without the
// vendor's card doesn't have: the video-encoder API. The game imports exactly two of its
// functions (shown by the program's import table) and calls them in one place, its encoder's
// start-up; on an error return it logs the code and carries on without the recorder, so the
// stand-in answers "no encoder" to both. Built as pc/nvEncodeAPI64.dll beside hiraku-pc64.exe,
// which is first in the DLL search path (CMakeLists.txt, next to the loader's own rule).
#include <stdint.h>

#define WINAPI __stdcall

// 1 is the API's own "no encode device" status (NV_ENC_ERR_NO_ENCODE_DEVICE): the game tests for
// nonzero and logs the value, then carries on without the recorder
#define NO_ENCODER 1

__declspec(dllexport) uint32_t WINAPI NvEncodeAPIGetMaxSupportedVersion(uint32_t *version) {
    (void)version;   // not written: the caller's out-parameter stays its own
    return NO_ENCODER;
}

__declspec(dllexport) uint32_t WINAPI NvEncodeAPICreateInstance(void *create_params, void *encoder) {
    (void)create_params;
    (void)encoder;   // not written either
    return NO_ENCODER;
}
