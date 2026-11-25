// SPDX-License-Identifier: GPL-3.0-or-later
// A 64-bit PC IIDX's board, at the game's import table only: the functions and class methods the
// game's DLL imports from libaio.dll, libaio-iob.dll and libaio-iob2_video.dll are replaced with
// answers for a board that is present and ready — its status carrying HIRAKU's controls in this
// cabinet's layout (the switches, both starts, VEFX/Effect, both players' keys, both
// turntables' positions, the coin count; the rest released), its lamps accepted and dropped.
// No USB device and no serial protocol is emulated under libaio, and
// the in-band firmware
// path (the aioIob2Bi2x_WriteFirm* imports) is left alone. The license key stubs (kbt.dll, kld.dll
// via GetModuleHandleA/GetProcAddress) come from keystub.c, shared with board.c. Chosen by the
// game DLL's name in main.c, so the first pc64 game's board (board.c, the BI3A) is untouched.
//
// What the game's own code is read as doing with them (read from the game's DLL — not yet seen at
// run time: the boot stops at the camera gate before the board is made): its I/O boot opens the
// board's USB serial (aioIob2Bi2x_OpenSciUsbCdc), makes a node manager on it (aioNMgrIob2_Create),
// makes the TDJ node controller (aioIob2Bi2xTDJ_Create), then calls the manager's vtable slot 0x50
// (BeginManage, AIO_NMGR_IOB2_COMM's last slot) and reads two words of each returned object (an
// error word at +8, a state word at +0x3a on the manager and +0x7c on the controller: zero is fine
// for both) and tail-calls the controller's GetDeviceStatus into the status structure the game
// keeps in its own I/O object — the structure's size is the one the game's own code writes (0xca).
// The card reader is the same shape on a second manager (aioNMgrIob_Create, aioIobIcca_Create for
// units 1 and 2, ICCA GetDeviceStatus/Begin/EndGetCardId) and its own serial open
// (aioSciComm_OpenB8PNS1, imported from libaio.dll). Every lamp and output method is accepted and
// dropped, and the Destroy/UpdateDevicesStatus calls are answered without ever handing a stand-in
// object to the real libaio.
//
// HK_PC64_BOARD_SLOT_LOG=1: every vtable call the game makes on a stand-in object is logged (the
// object, the slot, the arguments) and counted, with a summary at exit — log only, the slots answer
// 0 while it is on, to see which slots the boot really calls.
#include "pc64.h"
#include "../pc/link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The status structure's size, as the game's own code writes it
#define DEVSTATUS_SIZE 0xca
// The reader unit's status structure is smaller: the game keeps two of them in 0x48-byte
// unit slots, and the library fills only 0x34 bytes of it, so a wider fill would run into the
// next unit and the reader's own state word (seen at run time: the boot state was reset to 0
// by every poll)
#define ICCA_DEVSTATUS_SIZE 0x34

static int g_said;
#define SAID(what) do { if (g_said++ < 60) pc64_log("board: %s", what); } while (0)

// --- the stand-in objects: zeroed (so the error and state words the game reads are "no error,
// state 0"), each with a vtable whose every slot is its own stub
#define NSLOTS 0x20
#define NKINDS 7
// the stand-ins are as big as the real objects' fields the game reads (BeginManage in the real
// manager classes sets a byte at +0x588 — libaio-iob.dll, verified — so +0x588 must be inside)
static uint8_t g_sci[0x600], g_sci_icca[0x600], g_nmgr_iob2[0x600], g_nmgr_iob[0x600], g_tdj[0x600], g_icca[2][0x600];

static int g_slot_log;
static unsigned g_slot_count[NKINDS][NSLOTS];
static const char *kind_of(void *self) {
    if (self == g_sci) return "sci";
    if (self == g_sci_icca) return "sci-icca";
    if (self == g_nmgr_iob2) return "nmgr-iob2";
    if (self == g_nmgr_iob) return "nmgr-iob";
    if (self == g_tdj) return "tdj";
    if (self == g_icca[0]) return "icca1";
    if (self == g_icca[1]) return "icca2";
    return "?";
}
static int kind_i(void *self) {
    if (self == g_sci) return 0;
    if (self == g_sci_icca) return 1;
    if (self == g_nmgr_iob2) return 2;
    if (self == g_nmgr_iob) return 3;
    if (self == g_tdj) return 4;
    if (self == g_icca[0]) return 5;
    if (self == g_icca[1]) return 6;
    return -1;
}
static void *slot_hit(void *self, int slot, void *a2, void *a3, void *a4) {
    int k = kind_i(self);
    // slot 0x50 on the managers is BeginManage: the real classes' body is `mov byte [this+0x588],1;
    // ret` (AIO_NMGR_IOB2::BeginManage, libaio-iob.dll — verified; the reader manager's export has
    // the same name and shape — assumed), so answer it the same way: the managing flag set, void
    if (slot == 0x50 / 8 && (k == 2 || k == 3)) ((uint8_t *)self)[0x588] = 1;
    if (k >= 0) {
        unsigned n = ++g_slot_count[k][slot];
        if (g_slot_log && n <= 6) {
            // the call site, as module+offset (skip this stub and its wrapper)
            void *ret = NULL;
            CaptureStackBackTrace(2, 1, &ret, NULL);
            HMODULE m = NULL;
            char path[MAX_PATH] = "?";
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, ret, &m);
            if (m) GetModuleFileNameA(m, path, sizeof path);
            const char *b = strrchr(path, '\\');
            pc64_log("board slot: %s this=%p slot +0x%02x args %p %p %p (call %u from %s+%llx)", kind_of(self), self, slot * 8, a2, a3, a4, n,
                     b ? b + 1 : path, (unsigned long long)((const char *)ret - (const char *)m));
        }
    } else if (g_slot_log) {
        pc64_log("board slot: unknown object %p slot +0x%02x args %p %p %p", self, slot * 8, a2, a3, a4);
    }
    return 0;   // log only
}
static void slot_summary(void) {
    if (!g_slot_log) return;
    static const char *kinds[NKINDS] = {"sci", "sci-icca", "nmgr-iob2", "nmgr-iob", "tdj", "icca1", "icca2"};
    for (int k = 0; k < NKINDS; k++)
        for (int s = 0; s < NSLOTS; s++)
            if (g_slot_count[k][s])
                pc64_log("board slot summary: %s slot +0x%02x called %u times", kinds[k], s * 8, g_slot_count[k][s]);
}
#define X(n) static void *slot_##n(void *self, void *a2, void *a3, void *a4) { return slot_hit(self, n, a2, a3, a4); }
#define SLOTS X(0)X(1)X(2)X(3)X(4)X(5)X(6)X(7)X(8)X(9)X(10)X(11)X(12)X(13)X(14)X(15) \
              X(16)X(17)X(18)X(19)X(20)X(21)X(22)X(23)X(24)X(25)X(26)X(27)X(28)X(29)X(30)X(31)
SLOTS
#undef X
static void *g_vt[NSLOTS];
#define X(n) slot_##n,
static void *const g_vt_init[NSLOTS] = { SLOTS };
#undef X

static void *obj(void *storage) {
    static int vt_ready;
    if (!vt_ready) {
        memcpy(g_vt, g_vt_init, sizeof g_vt);
        vt_ready = 1;
    }
    *(void **)storage = g_vt;
    return storage;
}

// --- the opens and creates: a board that is there
static void *OpenSciUsbCdc_(uint64_t port) {
    (void)port;
    SAID("OpenSciUsbCdc (the board's USB serial)");
    return obj(g_sci);
}
// the card reader's own serial open, imported from libaio.dll: its own stand-in storage, not the
// unit 1 ICCA object (answered at the game's import table like the rest; nothing of the serial
// protocol itself is emulated; read from the game's code, not yet seen at run time, that the boot
// needs this one non-null too)
static void *SciCommOpen_(uint32_t port, uint32_t baud) {
    (void)port, (void)baud;
    SAID("aioSciComm_OpenB8PNS1 (the reader's serial)");
    return obj(g_sci_icca);
}
static void *NMgrIob2_Create_(void *sci, uint32_t mode) {
    (void)sci, (void)mode;
    SAID("NMgrIob2_Create");
    return obj(g_nmgr_iob2);
}
static void *NMgrIob_Create_(void *sci, uint32_t mode) {
    (void)sci, (void)mode;
    SAID("NMgrIob_Create (reader manager)");
    return obj(g_nmgr_iob);
}
static void *TDJ_Create_(void *nmgr, uint32_t idx, void *p) {
    (void)nmgr, (void)idx, (void)p;
    SAID("TDJ_Create");
    return obj(g_tdj);
}
static void *Icca_Create_(void *nmgr, uint32_t unit, void *p) {
    (void)nmgr, (void)p;
    SAID("Icca_Create");
    return obj(g_icca[unit >= 1 && unit <= 2 ? unit - 1 : 0]);
}

// --- the Destroy and UpdateDevicesStatus calls: the stand-in objects are ours, never the real
// libaio's; anything else goes through
static void (*r_SciDestroy)(void *), (*r_NMDestroy)(void *), (*r_NCDestroy)(void *);
static void (*r_NCUpdate)(void *);
static int is_ours(void *o) {
    return o == g_sci || o == g_sci_icca || o == g_nmgr_iob2 || o == g_nmgr_iob || o == g_tdj || o == g_icca[0] || o == g_icca[1];
}
static void SciDestroy_(void *s) { if (!is_ours(s) && r_SciDestroy) r_SciDestroy(s); }
static void NMDestroy_(void *m) { if (!is_ours(m) && r_NMDestroy) r_NMDestroy(m); }
static void NCDestroy_(void *c) { if (!is_ours(c) && r_NCDestroy) r_NCDestroy(c); }
static void NCUpdateDevicesStatus_(void *c) { if (!is_ours(c) && r_NCUpdate) r_NCUpdate(c); }
static void NCtlUpdateDevicesStatus_(void) {}   // AIO_NCTL::UpdateDevicesStatus(): no device behind the stand-ins

// --- the status polls: no card; the cabinet's switches, buttons, keys and turntables from
// HIRAKU's controls. The layout is the one the real TDJ class builds in the status structure
// (libaio-iob2_video.dll, its status-build code, read there): +4..+7 from the poll answer's byte
// 4 bits 0-3 (TEST, SERVICE, coin mech, one unknown), +8..+15 from byte 5's bits 0-7, +16..+19
// from byte 6's bits 0-3, +20/+21 the two turntables' 8-bit positions, +22 the coin count (taken
// when two polls in a row agree), +23..+26 from byte 7, +27..+33 the first player's keys 1-7,
// +34..+40 the second's (the names are docs/BIO2-BI2X.md's table, from the other layer's hook;
// the TEST bit is confirmed at run time, the start-up screen answers the press). HIRAKU's link
// carries this cabinet's bits in the series' layout (link.h's HK_PC_C02_*, the core gives a
// 64-bit game of the family "iidx" the series' control names): +8/+9 the two starts, +10/+11
// VEFX/Effect, +27..+33/+34..+40 the keys, +20/+21 the turntables (0-255, one full turn is the
// whole range — the game folds the change into -128..128, so the wrap gives the direction),
// +22 the link's coin count; the coin switch +6 pulses as link.c counts the coins. No headphone
// bit exists in this layout (+12/+13 stay 0), and the sliders (the link's slider[], set by the
// core's "Volume N") are not wired: this cabinet's panel has none.
static void TDJ_GetDeviceStatus_(void *self, void *st) {
    (void)self;
    if (!pc64_board_t0) pc64_board_t0 = GetTickCount();   // the game first read its board (HK_SCRIPT's clock)
    pc64_link_pad_read();   // the game read its pad here: the link's pad_read_us
    if (g_said < 40) SAID("TDJ GetDeviceStatus (poll)");
    memset(st, 0, DEVSTATUS_SIZE);
    const unsigned h = pc64_crt_inputs(pc64_held | pc64_link_inputs()) | (pc64_clock_test_held() ? PC64_IN_TEST : 0);
    pc64_clock_poll();
    uint8_t *s = (uint8_t *)st;
    const uint32_t pad = pc64_link_pad();   // this cabinet's bits (HK_PC_C02_*)
    struct hk_pc_link *l = pc64_link();
    static unsigned s_polls;                // +0 is the answered-poll count the real board keeps
    s[0] = (uint8_t)(++s_polls);
    s[4] = ((pad & HK_PC_C02_TEST) || (h & PC64_IN_TEST)) != 0;   // the TEST switch, nothing else
    s[5] = ((pad & HK_PC_C02_SERVICE) || (h & PC64_IN_SERVICE)) != 0;
    s[6] = (h & PC64_IN_COIN) != 0;                 // the coin switch's pulse
    s[22] = (uint8_t)(l ? l->coins : 0);           // the coin count the board keeps
    s[8] = (pad & HK_PC_C02_START(1)) != 0;        // 1P start
    s[9] = (pad & HK_PC_C02_START(2)) != 0;        // 2P start
    s[10] = (pad & HK_PC_C02_VEFX) != 0;           // VEFX
    s[11] = (pad & HK_PC_C02_EFFECT) != 0;         // EFFECT
    for (int p = 1; p <= 2; p++)
        for (int k = 1; k <= 7; k++)
            s[26 + 7 * (p - 1) + k] = (pad & HK_PC_C02_KEY(p, k)) != 0;   // keys 1-7
    if (l) { s[20] = l->turntable[0]; s[21] = l->turntable[1]; }       // the turntables' positions
    static uint8_t last_sw, last_in;
    const uint8_t now_sw = (uint8_t)(s[4] | s[5] << 1 | s[6] << 2);
    if (now_sw != last_sw) {
        pc64_log("board: TDJ status switches test/service/coin %u%u%u -> %u%u%u (poll)",
                 last_sw & 1, (last_sw >> 1) & 1, (last_sw >> 2) & 1,
                 now_sw & 1, (now_sw >> 1) & 1, (now_sw >> 2) & 1);
        last_sw = now_sw;
    }
    uint8_t btn = 0, k1 = 0, k2 = 0;
    for (int k = 0; k < 8; k++) btn |= (uint8_t)(s[8 + k] << k);
    for (int k = 0; k < 7; k++) k1 |= (uint8_t)(s[27 + k] << k), k2 |= (uint8_t)(s[34 + k] << k);
    const uint8_t now_in = (uint8_t)(btn ^ k1 ^ k2 ^ s[20] ^ s[21] ^ s[22] ^ (s[12] << 1));
    if (now_in != last_in) {
        pc64_log("board: TDJ status in %02x -> %02x (start/vefx/effect %02x, tt %u/%u, coin %u, keys1 %02x keys2 %02x)",
                 last_in, now_in, btn, s[20], s[21], s[22], k1, k2);
        last_in = now_in;
    }
}
// --- the card reader (ICCA): the DEVSTATUS fields the game reads (read from the game's code:
// +3 the status byte — 0 no card, 1/2 a card's id was read, mapped 0->state 5, 1->(0,1),
// 2->(1,2); +4..+0xc the 9-byte id, of which the game formats the first 8 as "%02X.." into the
// cardid it sends to cardmng.inquire; +0xd the error byte, the game proceeds when <= 3). A card
// held to the reader (the link's card, HK_SCRIPT's "card", HK_PC64_CARD's) is reported from the
// poll that follows it; EndGetCardId takes it back, as the real reader's does.
static uint32_t g_card_seq;
static int g_card_active;
static void ICCA_GetDeviceStatus_(void *self, void *st) {
    (void)self;
    SAID("ICCA GetDeviceStatus (poll)");
    memset(st, 0, ICCA_DEVSTATUS_SIZE);
    uint8_t uid[8];
    const uint32_t seq = pc64_card(uid);   // the count that moves on with each card held
    if (seq != g_card_seq) {
        g_card_seq = seq;
        g_card_active = 1;
        uint64_t id = 0;
        for (int i = 0; i < 8; i++) id = id << 8 | uid[i];
        pc64_log("board: ICCA: card %016llx held to the reader (seq %u)", (unsigned long long)id, seq);
    }
    if (g_card_active) {
        uint8_t *s = (uint8_t *)st;
        s[3] = 1;              // status: a card's id was read
        memcpy(s + 4, uid, 8); // the card's uid
        s[0xd] = 0;            // no error
    }
}
static void ICCA_BeginGetCardId_(void *self) { SAID("ICCA BeginGetCardId"); (void)self; }
static void ICCA_EndGetCardId_(void *self) {
    SAID("ICCA EndGetCardId");
    (void)self;
    if (g_card_active) pc64_log("board: ICCA: the card is taken back (EndGetCardId)");
    g_card_active = 0;
}

// --- the reader's 10-key (finding, kept for the older cabinet model that has one): the game
// reads it NOT from the ICCA DEVSTATUS but via libacio.dll's ac_io_icca_get_status (IAT
// 0x180c92478): key dword at out+0x10 — byte 0 the key event (bit 7 new key, bits 6-4 count,
// bits 3-0 pad position), bytes 2-3 the held mask (position n <-> bit (n+8)&0xf); pad position n
// is key table 0x180daa490[n] = digits 0-9, 00 (0xa), correction (0xb). This cabinet model has no
// 10-key unit on its readers (its PIN pad is the second screen's touch pad), and the game never
// called the import in our runs, so no stand-in code for it stays here.

// --- the lamps and outputs. The keys, starts, VEFX and Effect go onto the link's lamp words (the
// family's lamp names, shell/cabinet.cpp make_iidx_lamps: key lamps in lamps bits 0-13, player 1
// first; starts, VEFX, Effect in led bits 0-3). The rest (woofer, turntable and reader LEDs, the
// tape-LED stream, the coin blocker) has no link field: logged a few times, then dropped.
static uint32_t g_lamps, g_led;
static int g_lamp_logs, g_led_logs, g_other_logs;
static void lamp_set(uint32_t *mirror, uint32_t bit, int on, int is_led, const char *what) {
    const uint32_t old = *mirror;
    if (on) *mirror |= bit; else *mirror &= ~bit;
    struct hk_pc_link *l = pc64_link();
    if (l) { if (is_led) l->led = (uint8_t)*mirror; else l->lamps = *mirror; }
    if (*mirror != old && (is_led ? g_led_logs++ < 40 : g_lamp_logs++ < 40)) {   // the first 40 of each
        pc64_log("lamp: %s %s (lamps %08lx, led %02x)", what, on ? "on" : "off",
                 (unsigned long)g_lamps, (unsigned)(g_led & 0xff));
    }
}
static void SetPlayerButtonLamp_(void *s, uint32_t player, uint32_t key, uint8_t on) {
    (void)s;
    // key 0-6 (the game's thunks pass 0..6, read from its code; a 1..7 reading lost key 1's lamp and
    // put keys 2-7 one lamp to the left)
    if (player < 2 && key <= 6)
        lamp_set(&g_lamps, 1u << (player * 7 + key), on & 1, 0, "key lamp");
}
static void SetStartLamp_(void *s, uint32_t player, uint8_t on) {
    (void)s;
    if (player < 2) lamp_set(&g_led, 1u << player, on & 1, 1, "start lamp");
}
static void SetVefxButtonLamp_(void *s, uint8_t on) { (void)s; lamp_set(&g_led, 4u, on & 1, 1, "VEFX lamp"); }
static void SetEffectButtonLamp_(void *s, uint8_t on) { (void)s; lamp_set(&g_led, 8u, on & 1, 1, "Effect lamp"); }
static void other_log(const char *what, unsigned long a, unsigned long b) {
    if (g_other_logs < 12) { g_other_logs++; pc64_log("output (dropped): %s %lx %lx", what, a, b); }
}
static void SetWooferLed_(void *s, uint8_t on) { (void)s; other_log("woofer led", on, 0); }
static void SetTurnTableLed_(void *s, uint32_t p, uint32_t v, uint8_t on) {
    (void)s; other_log("turntable led", p, v | (unsigned long)(on & 1) << 8);
}
static void SetIccrLed_(void *s, uint32_t p, uint32_t v, uint8_t on) {
    (void)s; other_log("reader led", p, v | (unsigned long)(on & 1) << 8);
}
static void SetTapeLedData_(void *s, int a, const uint8_t *data, uint32_t len) {
    (void)s; (void)len;
    if (g_other_logs < 12) {
        g_other_logs++;
        char hex[25] = "";
        if (data) for (int i = 0; i < 12; i++) sprintf(hex + i * 2, "%02x", data[i]);
        pc64_log("output (dropped): tape led data index %x buffer %p first %s", a, (void *)data, hex);
    }
}
static void ControlCoinBlocker_(void *s, uint8_t on) { (void)s; other_log("coin blocker", on, 0); }
static void drop_(void) {}

#define HOOK(dll, name, fn) do { if (!pc64_import(game, dll, name, (void *)(fn))) \
    pc64_log("the game doesn't import %s!%s", dll, name); } while (0)
#define HOOKT(dll, name, fn, keep) do { void *o = pc64_import(game, dll, name, (void *)(fn)); \
    if (keep) *(void **)(keep) = o; if (!o) pc64_log("the game doesn't import %s!%s", dll, name); } while (0)

// (A one-version-only read-only inspector of the game's IO boot objects once lived here behind
// HK_PC64_BOARD_INSPECT; it read fixed offsets inside the game module, so it is kept out of the
// tracked source.)

void pc64_board_iidx_install(HMODULE game) {
    const char *t = getenv("HK_PC64_BOARD_SLOT_LOG");
    g_slot_log = t && *t && *t != '0';
    if (g_slot_log) atexit(slot_summary);
    pc64_keystub_install(game);
    HOOK("libaio-iob.dll", "aioIob2Bi2x_OpenSciUsbCdc", OpenSciUsbCdc_);
    HOOK("libaio.dll", "aioSciComm_OpenB8PNS1", SciCommOpen_);
    HOOK("libaio-iob.dll", "aioNMgrIob2_Create", NMgrIob2_Create_);
    HOOK("libaio-iob.dll", "aioNMgrIob_Create", NMgrIob_Create_);
    HOOK("libaio-iob.dll", "aioIobIcca_Create", Icca_Create_);
    HOOK("libaio-iob.dll", "?GetDeviceStatus@AIO_IOB_ICCA@@QEBAXAEAUDEVSTATUS@1@@Z", ICCA_GetDeviceStatus_);
    HOOK("libaio-iob.dll", "?BeginGetCardId@AIO_IOB_ICCA@@QEAAXXZ", ICCA_BeginGetCardId_);
    HOOK("libaio-iob.dll", "?EndGetCardId@AIO_IOB_ICCA@@QEAAXXZ", ICCA_EndGetCardId_);
    // the firmware path (aioIob2Bi2x_WriteFirm*/Create/DestroyWriteFirmContext) is not hooked
    HOOK("libaio-iob2_video.dll", "aioIob2Bi2xTDJ_Create", TDJ_Create_);
    HOOK("libaio-iob2_video.dll", "?GetDeviceStatus@AIO_IOB2_BI2X_TDJ@@QEBAXAEAUDEVSTATUS@1@@Z", TDJ_GetDeviceStatus_);
    HOOK("libaio-iob2_video.dll", "?SetEffectButtonLamp@AIO_IOB2_BI2X_TDJ@@QEAAX_N@Z", SetEffectButtonLamp_);
    HOOK("libaio-iob2_video.dll", "?SetVefxButtonLamp@AIO_IOB2_BI2X_TDJ@@QEAAX_N@Z", SetVefxButtonLamp_);
    HOOK("libaio-iob2_video.dll", "?SetPlayerButtonLamp@AIO_IOB2_BI2X_TDJ@@QEAAXII_N@Z", SetPlayerButtonLamp_);
    HOOK("libaio-iob2_video.dll", "?SetStartLamp@AIO_IOB2_BI2X_TDJ@@QEAAXI_N@Z", SetStartLamp_);
    HOOK("libaio-iob2_video.dll", "?SetWooferLed@AIO_IOB2_BI2X_TDJ@@QEAAXI@Z", SetWooferLed_);
    HOOK("libaio-iob2_video.dll", "?SetTurnTableLed@AIO_IOB2_BI2X_TDJ@@QEAAXII@Z", SetTurnTableLed_);
    HOOK("libaio-iob2_video.dll", "?SetTurnTableResist@AIO_IOB2_BI2X_TDJ@@QEAAXIE@Z", drop_);
    HOOK("libaio-iob2_video.dll", "?SetIccrLed@AIO_IOB2_BI2X_TDJ@@QEAAXII@Z", SetIccrLed_);
    HOOK("libaio-iob2_video.dll", "?ControlCoinBlocker@AIO_IOB2_BI2X_TDJ@@QEAAXI_N@Z", ControlCoinBlocker_);
    HOOK("libaio-iob2_video.dll", "?IoReset@AIO_IOB2_BI2X_TDJ@@QEAAXI@Z", drop_);
    HOOK("libaio-iob2_video.dll", "?SetWatchDogTimer@AIO_IOB2_BI2X_TDJ@@QEAAXE@Z", drop_);
    HOOK("libaio-iob2_video.dll", "?AddCounter@AIO_IOB2_BI2X_TDJ@@QEAAXII@Z", drop_);
    HOOK("libaio-iob2_video.dll", "?SetTapeLedData@AIO_IOB2_BI2X_TDJ@@QEAAXIPEBX@Z", SetTapeLedData_);
    HOOKT("libaio.dll", "aioSci_Destroy", SciDestroy_, &r_SciDestroy);
    HOOKT("libaio.dll", "aioNodeMgr_Destroy", NMDestroy_, &r_NMDestroy);
    HOOKT("libaio.dll", "aioNodeCtl_Destroy", NCDestroy_, &r_NCDestroy);
    HOOKT("libaio.dll", "aioNodeCtl_UpdateDevicesStatus", NCUpdateDevicesStatus_, &r_NCUpdate);
    HOOK("libaio.dll", "?UpdateDevicesStatus@AIO_NCTL@@SAXXZ", NCtlUpdateDevicesStatus_);
    pc64_log("a 64-bit PC IIDX's board: the IOB2 BI2X TDJ imports answered (HIRAKU's controls in the status, lamps on the link for the keys, starts, VEFX, Effect)%s",
             g_slot_log ? " [slot log on]" : "");
}
