// SPDX-License-Identifier: GPL-3.0-or-later
// The cabinet's devices at the game's imports (PLAN.md section 32), after spice2x's
// games/popn/bi3a_hook.cpp, misc/ami2000.cpp and stubs/stubs.cpp (GPL-3.0):
//   - The BI3A board (libaio, libaio-iob5): the game opens it on a USB serial port it finds by
//     SetupDi (1CCF:8058), then reads its status 60 times a second. Its calls answer for a board
//     that isn't there; the status is the inputs held (pc64_held).
//   - The AMI2000 card reader (iccr-ami2000-api): a card held to it (the link's card, or HK_SCRIPT's)
//     is its identifier once, and its type (1 an e-AMUSEMENT pass, 2 FeliCa), as spice2x's.
//   - The license key (kbt.dll, kld.dll: on the cabinet, not in the game's folder): keystub.c,
//     shared with the other board (board_iidx.c).
#include "pc64.h"

#include <setupapi.h>
#include <stdlib.h>
#include <string.h>

volatile unsigned pc64_held;
volatile DWORD pc64_board_t0;

// --- the board: libaio's objects, the game's own sizes (spice2x's, this build's too)
static uint8_t g_sci[0x108], g_nmgr[0xb10], g_bi3a[0x4ff0];
typedef struct { uint64_t zero; void *sci; } eresult;
static void (*r_GetCommStatus)(void *, void *);
static void (*r_SciDestroy)(void *), (*r_NMDestroy)(void *), (*r_NCDestroy)(void *);
static int (*r_NMGetState)(void *), (*r_NCGetState)(void *);
static int (*r_NMIsReady)(void *, int), (*r_NMIsError)(void *, int), (*r_NCIsReady)(void *, int), (*r_NCIsError)(void *, int);

static eresult *OpenSciUsbCdc_(eresult *out, uint64_t port) {
    pc64_log("the BI3A board opened (port %llu)", (unsigned long long)port);
    out->zero = 0, out->sci = g_sci;
    return out;
}
static void GetCommStatus_(void *s, void *status) {   // no errors counted
    if (s == g_sci) memset(status, 0, 40);
    else r_GetCommStatus(s, status);
}
static void SciDestroy_(void *s) { if (s != g_sci) r_SciDestroy(s); }
static void NMDestroy_(void *m) { if (m != g_nmgr) r_NMDestroy(m); }
static int NMGetState_(void *m) { return m == g_nmgr ? 1 : r_NMGetState(m); }
static int NMIsReady_(void *m, int s) { return m == g_nmgr ? 1 : r_NMIsReady(m, s); }
static int NMIsError_(void *m, int s) { return m == g_nmgr ? 0 : r_NMIsError(m, s); }
static void NCDestroy_(void *c) { if (c != g_bi3a) r_NCDestroy(c); }
static int NCGetState_(void *c) { return c == g_bi3a ? 1 : r_NCGetState(c); }
static int NCIsReady_(void *c, int s) { return c == g_bi3a ? 1 : r_NCIsReady(c, s); }
static int NCIsError_(void *c, int s) { return c == g_bi3a ? 0 : r_NCIsError(c, s); }
static void UpdateDevicesStatus_(void) {}
static void *NMgrIob5_Create_(void *sci, uint32_t mode) {
    (void)sci, (void)mode;
    return g_nmgr;
}
static void *Bi3a_Create_(void *nmgr, uint8_t i, void *p) {
    (void)nmgr, (void)i, (void)p;
    return g_bi3a;
}

// The board's status, 0x288 bytes, as the game's Bi3aInput reads it (its update, 0x18032b020):
//   27 coins counted, 44 TEST, 45 SERVICE, 46 the coin switch, 66 and 71 the big red and blue
//   buttons, 72 headphones, 369 buttons 1-8 (bit 0 button 1), 370 button 9;
//   62, 63, 67, 68 read held when 0 (four switches active low): 1 here
static void Bi3a_GetDeviceStatus_(void *c, uint8_t *st) {
    (void)c;
    if (!pc64_board_t0) pc64_board_t0 = GetTickCount();
    memset(st, 0, 0x288);
    st[62] = st[63] = st[67] = st[68] = 1;
    // the single-screen mode (crt.c): button 5 for the touches, the touch screen's view takes the buttons
    const unsigned h = pc64_crt_inputs(pc64_held | pc64_link_inputs());
    st[44] = (h & PC64_IN_TEST) != 0;
    st[45] = (h & PC64_IN_SERVICE) != 0;
    st[46] = (h & PC64_IN_COIN) != 0;
    st[66] = (h & PC64_IN_RED) != 0;
    st[71] = (h & PC64_IN_BLUE) != 0;
    st[72] = (h & PC64_IN_HEADPHONES) != 0;
    for (int b = 0; b < 8; b++)
        if (h & PC64_IN_BUTTON(b + 1)) st[369] |= (uint8_t)(1 << b);
    st[370] = (h & PC64_IN_BUTTON(9)) != 0;
}
// Lamps, tape LEDs, counters, watchdog: taken (the lamps go to HIRAKU later)
static void Bi3a_SetOutputData_(void *c, unsigned pin, uint8_t v) { (void)c, (void)pin, (void)v; }
static void Bi3a_SetTapeLedDataPart_(void *c, unsigned pin, char kind, const void *d, unsigned n, int last) {
    (void)c, (void)pin, (void)kind, (void)d, (void)n, (void)last;
}
static int64_t Bi3a_SetTapeLedDataLimit_(void *c, uint32_t ch, uint8_t scale, uint8_t limit) {
    (void)c, (void)ch, (void)scale, (void)limit;
    return 0;
}
static void Bi3a_AddCounter_(void *c, unsigned n, unsigned v) { (void)c, (void)n, (void)v; }
static void Bi3a_SetWatchDogTimer_(void *c, uint8_t v) { (void)c, (void)v; }

// --- SetupDi: one USB serial device, 1CCF:8058 (the BI3A)
static int g_devices;
#define DEVICES ((HDEVINFO)&g_devices)
static const char DESC[] = "1CCF(8058)_000", HWID[] = "USB\\VID_1CCF&PID_8058&MI_00\\000";
static HDEVINFO(WINAPI *r_GetClassDevsA)(const GUID *, LPCSTR, HWND, DWORD);
static BOOL(WINAPI *r_EnumDeviceInfo)(HDEVINFO, DWORD, PSP_DEVINFO_DATA);
static BOOL(WINAPI *r_GetInstanceIdA)(HDEVINFO, PSP_DEVINFO_DATA, PSTR, DWORD, PDWORD);
static BOOL(WINAPI *r_GetRegPropertyA)(HDEVINFO, PSP_DEVINFO_DATA, DWORD, PDWORD, PBYTE, DWORD, PDWORD);
static BOOL(WINAPI *r_DestroyList)(HDEVINFO);

static HDEVINFO WINAPI GetClassDevsA_(const GUID *g, LPCSTR e, HWND w, DWORD f) {
    (void)g, (void)e, (void)w, (void)f;
    return DEVICES;
}
static BOOL WINAPI EnumDeviceInfo_(HDEVINFO s, DWORD i, PSP_DEVINFO_DATA d) {
    if (s != DEVICES) return r_EnumDeviceInfo(s, i, d);
    if (i) {
        SetLastError(ERROR_NO_MORE_ITEMS);
        return FALSE;
    }
    return TRUE;
}
static BOOL WINAPI GetInstanceIdA_(HDEVINFO s, PSP_DEVINFO_DATA d, PSTR out, DWORD n, PDWORD need) {
    if (s != DEVICES) return r_GetInstanceIdA(s, d, out, n, need);
    if (need) *need = sizeof HWID;
    if (n < sizeof HWID) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    memcpy(out, HWID, sizeof HWID);
    return TRUE;
}
static BOOL WINAPI GetRegPropertyA_(HDEVINFO s, PSP_DEVINFO_DATA d, DWORD p, PDWORD type, PBYTE out, DWORD n, PDWORD need) {
    if (s != DEVICES) return r_GetRegPropertyA(s, d, p, type, out, n, need);
    const char *v = p == SPDRP_DEVICEDESC ? DESC : p == SPDRP_HARDWAREID ? HWID : p == SPDRP_FRIENDLYNAME ? "USB Serial Device (COM3)" : NULL;
    if (!v) {
        SetLastError(ERROR_INVALID_DATA);
        return FALSE;
    }
    DWORD len = (DWORD)strlen(v) + 2;   // a multi-string's two ends
    if (need) *need = len;
    if (n < len) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    if (type) *type = p == SPDRP_HARDWAREID ? REG_MULTI_SZ : REG_SZ;
    memcpy(out, v, len - 1);
    out[len - 1] = 0;
    return TRUE;
}
static BOOL WINAPI DestroyList_(HDEVINFO s) { return s == DEVICES ? TRUE : r_DestroyList(s); }

// --- the AMI2000 card reader: the game asks for a card's identifier (its UID, the first byte the
// highest) as it polls; a card held to the reader is given once
static void Ami_void(void) {}
static int Ami_zero(void) { return 0; }
static const char *Ami_rom(void) { return "HIRAKU"; }
const char *pc64_ami_rom(void) { return Ami_rom(); }   // the lid the game itself reports; used as the shop name on a first boot — the server's getname answer replaces it on the next start if it differs
static int g_card_type;
static uint32_t g_card_seen;
volatile unsigned pc64_cards_given;
static int Ami_type(void) { return g_card_type; }
static uint64_t Ami_card(void) {
    uint8_t uid[8];
    const uint32_t seq = pc64_card(uid);
    if (seq == g_card_seen) {
        g_card_type = 0;
        return 0;
    }
    g_card_seen = seq;
    pc64_cards_given++;
    g_card_type = uid[0] == 0xe0 && uid[1] == 0x04 ? 1 : 2;
    uint64_t id = 0;
    for (int i = 0; i < 8; i++) id = id << 8 | uid[i];
    pc64_log("card: %016llx (%s)", (unsigned long long)id, g_card_type == 1 ? "e-AMUSEMENT pass" : "FeliCa");
    return id;
}

#define HOOK(dll, name, fn, keep) do { void *o = pc64_import(game, dll, name, (void *)(fn)); \
    if (keep) *(void **)(keep) = o; if (!o) pc64_log("the game doesn't import %s!%s", dll, name); } while (0)

void pc64_board_install(HMODULE game) {
    HOOK("libaio-iob5.dll", "?OpenSciUsbCdc@AIO_IOB5_BI3A@@SA?AU?$AC_ERESULT_T@PEAVAIO_SCI_COMM@@@@_K@Z", OpenSciUsbCdc_, NULL);
    HOOK("libaio.dll", "aioSci_GetCommStatus", GetCommStatus_, &r_GetCommStatus);
    HOOK("libaio.dll", "aioSci_Destroy", SciDestroy_, &r_SciDestroy);
    HOOK("libaio.dll", "aioNodeMgr_Destroy", NMDestroy_, &r_NMDestroy);
    HOOK("libaio.dll", "aioNodeMgr_GetState", NMGetState_, &r_NMGetState);
    HOOK("libaio.dll", "aioNodeMgr_IsReady", NMIsReady_, &r_NMIsReady);
    HOOK("libaio.dll", "aioNodeMgr_IsError", NMIsError_, &r_NMIsError);
    HOOK("libaio.dll", "aioNodeCtl_Destroy", NCDestroy_, &r_NCDestroy);
    HOOK("libaio.dll", "aioNodeCtl_GetState", NCGetState_, &r_NCGetState);
    HOOK("libaio.dll", "aioNodeCtl_IsReady", NCIsReady_, &r_NCIsReady);
    HOOK("libaio.dll", "aioNodeCtl_IsError", NCIsError_, &r_NCIsError);
    HOOK("libaio.dll", "aioNodeCtl_UpdateDevicesStatus", UpdateDevicesStatus_, NULL);
    HOOK("libaio-iob5.dll", "aioNMgrIob5_Create", NMgrIob5_Create_, NULL);
    HOOK("libaio-iob5.dll", "aioIob5Bi3a_Create", Bi3a_Create_, NULL);
    HOOK("libaio-iob5.dll", "aioIob5Bi3a_GetDeviceStatus", Bi3a_GetDeviceStatus_, NULL);
    HOOK("libaio-iob5.dll", "aioIob5Bi3a_SetOutputData", Bi3a_SetOutputData_, NULL);
    HOOK("libaio-iob5.dll", "aioIob5Bi3a_SetTapeLedDataPart", Bi3a_SetTapeLedDataPart_, NULL);
    HOOK("libaio-iob5.dll", "aioIob5Bi3a_SetTapeLedDataLimit", Bi3a_SetTapeLedDataLimit_, NULL);
    HOOK("libaio-iob5.dll", "aioIob5Bi3a_AddCounter", Bi3a_AddCounter_, NULL);
    HOOK("libaio-iob5.dll", "aioIob5Bi3a_SetWatchDogTimer", Bi3a_SetWatchDogTimer_, NULL);
    HOOK("setupapi.dll", "SetupDiGetClassDevsA", GetClassDevsA_, &r_GetClassDevsA);
    HOOK("setupapi.dll", "SetupDiEnumDeviceInfo", EnumDeviceInfo_, &r_EnumDeviceInfo);
    HOOK("setupapi.dll", "SetupDiGetDeviceInstanceIdA", GetInstanceIdA_, &r_GetInstanceIdA);
    HOOK("setupapi.dll", "SetupDiGetDeviceRegistryPropertyA", GetRegPropertyA_, &r_GetRegPropertyA);
    HOOK("setupapi.dll", "SetupDiDestroyDeviceInfoList", DestroyList_, &r_DestroyList);
    HOOK("iccr-ami2000-api.dll", "AMI2000_Initialize", Ami_void, NULL);
    HOOK("iccr-ami2000-api.dll", "AMI2000_Cleanup", Ami_void, NULL);
    HOOK("iccr-ami2000-api.dll", "AMI2000_Startup", Ami_void, NULL);
    HOOK("iccr-ami2000-api.dll", "AMI2000_GetRomVersion", Ami_rom, NULL);
    HOOK("iccr-ami2000-api.dll", "AMI2000_IsDeviceError", Ami_zero, NULL);
    HOOK("iccr-ami2000-api.dll", "AMI2000_ReadCtrl", Ami_zero, NULL);
    HOOK("iccr-ami2000-api.dll", "AMI2000_GetCardType", Ami_type, NULL);
    HOOK("iccr-ami2000-api.dll", "AMI2000_GetCardIdentifier", Ami_card, NULL);
    pc64_keystub_install(game);
}
