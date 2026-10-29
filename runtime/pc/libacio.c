// SPDX-License-Identifier: GPL-3.0-or-later
// HIRAKU's libacio.dll for the PC pop'n music games (PLAN.md section 30) and beatmania IIDX 13:
// Konami's library for the e-AMUSEMENT card readers on the cabinet's serial port, answered from
// hiraku-pc's cards and 10-keys (runtime/pc/link.h) instead. The reader's states and status block are spice2x's (acio/icca,
// acio/core). With a real reader on a serial port, hiraku-pc keeps Konami's DLL instead of this one.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cabinet.h"
#include "c02_setup.h"
#include "ezusb_dev.h"

#define API __declspec(dllexport) __cdecl

struct icca_status {   // 24 bytes, as ac_io_icca_get_status fills it
    uint8_t state, solenoid, front_sensor, rear_sensor;
    uint8_t uid[8];
    int32_t error;
    uint32_t key_edge, key_level;
};

enum { STEP = 0, SLEEP = 1, START = 2, INIT = 3, READY = 4, GET_USERID = 5, ACTIVE = 6, EJECT = 7,
       EJECT_CHECK = 8, END = 9, CLOSE_EJECT = 10, CLOSE_E_CHK = 11, CLOSE_END = 12, ERR_GETUID = -2 };

// A reader each player: beatmania IIDX 13 (bm2dx.exe) has two, units 0 and 1 (its poll at JAG
// 0x407920), player 2's from the link's card_seq2, card_uid2 and keypad2; pop'n has one, whatever
// unit it names (pop'n 16 asks for unit 1), player 1's
static int two_readers(void);
struct reader {
    struct icca_status st;
    int state;
    uint32_t seen_seq;   // the link's card_seq last taken
    DWORD card_at;       // when a card was held there (GetTickCount), 0: none
    DWORD ejected_at;    // the slot reader: when the game pushed it out (EJECT), 0: not
    uint8_t key_serial;
    uint32_t keys_last;
    int ended;           // IIDX: the game ended the card's reading (workflow END): END, not ACTIVE, until its next step
};
static struct reader g_readers[2];
static struct reader *reader(int unit) { return &g_readers[unit == 1 && two_readers() ? 1 : 0]; }

// pop'n 15's reader is read with a card held to it: it stays there 2 s (HK_PC_CARD_MS another
// time). From pop'n 16 on the cabinet's reader is a slot: the card stays in until the game pushes
// it out (workflow EJECT, at the credit's end), then a moment at the slot's mouth (the front sensor
// only: "take your card"), then it's taken. popn22.dll's cabinets touch the card again (PLAN.md
// section 35: its "take your card" waits for the card to leave)
static int slot_reader(void) {
    static int slot = -1;
    if (slot < 0) {
        char path[MAX_PATH];
        GetModuleFileNameA((HMODULE)hk_program(), path, MAX_PATH);
        const char *base = strrchr(path, '\\');
        base = base ? base + 1 : path;
        slot = _stricmp(base, "popn15.exe") != 0 && _stricmp(base, "popn22.dll") != 0;
    }
    return slot;
}
static int two_readers(void) {
    static int two = -1;
    if (two < 0) {
        char path[MAX_PATH];
        GetModuleFileNameA((HMODULE)hk_program(), path, MAX_PATH);
        const char *base = strrchr(path, '\\');
        two = _stricmp(base ? base + 1 : path, "bm2dx.exe") == 0;
    }
    return two;
}
static DWORD card_held_ms(void) {
    static DWORD ms;
    if (!ms) ms = getenv("HK_PC_CARD_MS") ? (DWORD)atoi(getenv("HK_PC_CARD_MS")) : slot_reader() ? 0xffffffffu : 2000;
    return ms;
}
#define MOUTH_MS 1500

// HK_PC_USB_LOG: the reader's calls as the game makes them (each change)
static void trace(const char *what, int a, int b) {
    static int on = -1;
    static char last[64];
    char now[64];
    if (on < 0) on = getenv("HK_PC_USB_LOG") != NULL;
    if (!on) return;
    snprintf(now, sizeof now, "%s %d %d", what, a, b);
    if (strcmp(now, last)) hk_log("acio: %s", now), strcpy(last, now);
}

static void update(int unit) {
    struct hk_pc_link *l = hk_link();
    struct reader *r = reader(unit);
    const int two = r == &g_readers[1];
    // the 10-key: the reader's key codes in its order (0-9, decimal, 00), level and edge; the edge
    // names a key by its number on the reader's grid, not its digit (spice2x's KEYPAD_KEY_CODE_NUMS)
    static const uint32_t code[12] = {0x100, 0x200, 0x2000, 2, 0x400, 0x4000, 4, 0x800, 0x8000, 8, 1, 0x1000};
    static const uint32_t num[12] = {0, 1, 5, 9, 2, 6, 10, 3, 7, 11, 8, 4};
    const uint32_t held = two ? hk_keypad2_now() : hk_keypad_now();
    r->st.key_level = 0;
    for (int i = 0; i < 12; i++) {
        const uint32_t bit = i < 10 ? 1u << i : i == 10 ? HK_PC_KEY_DECIMAL : HK_PC_KEY_00;
        if (held & bit) {
            r->st.key_level |= code[i];
            if (!(r->keys_last & bit)) {
                r->st.key_edge = code[i] << 16 | 0x80 | (uint32_t)r->key_serial << 4 | num[i];
                r->key_serial = (r->key_serial + 1) & 7;
                trace("key", (int)r->st.key_edge, two);
            }
        } else if (r->st.key_edge >> 16 == code[i] && two_readers()) {
            r->st.key_edge = 0;   // IIDX 13: an event left after its key comes up keeps the next page from reading keys
        } else {
            r->st.key_edge &= ~(code[i] << 16);
        }
    }
    r->keys_last = held;
    // a card held to the reader: there (active, its number read) for as long as it is held, as
    // spice2x's while its key is down, whatever the game's steps say meanwhile
    const uint32_t seq = two ? hk_card_seq2() : hk_card_seq();
    if (seq != r->seen_seq) {
        r->seen_seq = seq;
        memcpy(r->st.uid, two ? (const void *)l->card_uid2 : (const void *)l->card_uid, 8);
        r->card_at = GetTickCount();
        r->ejected_at = 0;
        r->ended = 0;
        hk_log("hiraku-pc: a card at player %d's reader", two ? 2 : 1);
    }
    const DWORD in_for = r->card_at ? GetTickCount() - r->card_at : 0;
    if (r->card_at && r->ejected_at && GetTickCount() - r->ejected_at <= MOUTH_MS) {   // out, at the mouth
        r->state = EJECT_CHECK;
        r->st.front_sensor = 1, r->st.rear_sensor = 0;
    } else if (r->card_at && r->ejected_at && GetTickCount() - r->ejected_at <= MOUTH_MS + 1000) {
        // taken: the reader starts again, empty (pop'n 18's "take your card" waits for START or
        // INIT with the front sensor clear: 0x49aa06, 0x49a6e0, 0x450ec0)
        r->state = START;
        r->st.front_sensor = r->st.rear_sensor = 0;
        memset(r->st.uid, 0, 8);
    } else if (r->card_at && r->ejected_at && GetTickCount() - r->ejected_at <= MOUTH_MS + 2000) {
        // then asleep: pop'n 18's ENTRY, its card pushed out before a credit (the player backed
        // out), waits for SLEEP or READY instead (0x46885d, 0x468967)
        r->state = SLEEP;
        r->st.front_sensor = r->st.rear_sensor = 0;
    } else if (r->card_at && !r->ejected_at && in_for <= card_held_ms()) {
        r->state = r->ended ? END : ACTIVE;
        r->st.front_sensor = r->st.rear_sensor = 1;
    } else {   // no card (or taken away): the reader closed and empty, as spice2x reports it; a
        // stepping reader (0) holds the game's card screen until its time runs out
        if (r->card_at) {
            r->card_at = 0;
            memset(r->st.uid, 0, 8);
            r->st.front_sensor = r->st.rear_sensor = 0;
        }
        if (r->state == STEP || r->state == ACTIVE) r->state = CLOSE_EJECT;
    }
}

API char ac_io_begin(size_t dev, const char *ver, unsigned *val, size_t flags, void *p, size_t baud) {
    (void)dev, (void)ver, (void)val, (void)flags, (void)p, (void)baud;
    hk_log("hiraku-pc: the game opened its card reader");
    return 1;
}
API char ac_io_begin_get_status(void) { return 1; }
API int ac_io_end(int a) { (void)a; return 1; }
API int ac_io_end_get_status(int a) { (void)a; return 1; }
API void *ac_io_get_rs232c_status(char *p, int a) { (void)a; return memset(p, 0, 88); }
// pop'n music wants a version above 1.x.x (spice2x)
API char ac_io_get_version(uint8_t *p, int a) {
    (void)a;
    p[5] = 2, p[6] = 0, p[7] = 0;
    return 1;
}
API const char *ac_io_get_version_string(void) { return "1.25.0"; }
// The reader comes up after a while, as a real one does, or at once once is_active2 asked
static int g_polls;
API char ac_io_is_active(int a, int b) {
    (void)a, (void)b;
    return ++g_polls > 601;
}
API int ac_io_is_active2(int a, int *b, int c) {
    (void)a, (void)c;
    g_polls = 601;
    *b = 6;
    return 1;
}
// Device 5 is the card dispenser (popn22.dll asks for it, then waits for it to start): none, as
// spice2x answers
API char ac_io_is_active_device(int index, int a) {
    trace("is_active_device", index, a);
    return index != 5;
}
API int ac_io_get_node_no(void) { return 1; }
API int ac_io_get_firmware_update_device_index(void) { return 0xff; }
API int ac_io_go_firmware_update(void) { return 1; }
API int ac_io_get_recv_log(void) { return 0; }
API int ac_io_get_send_log(void) { return 0; }
API int ac_io_reset(void) { return 1; }
API int ac_io_update(void) { return 1; }
API int ac_io_version(void) { return 1; }
API int ac_io_set_soft_watch_dog(int a, int b) { (void)a, (void)b; return 1; }
API int ac_io_soft_watch_dog_on(int a) { (void)a; return 1; }
API int ac_io_soft_watch_dog_off(void) { return 1; }

API char ac_io_icca_cardunit_init(int unit) { (void)unit; return 1; }
API char ac_io_icca_cardunit_init_isfinished(int unit, DWORD *st) {
    (void)unit;
    *st = READY;
    return 1;
}
API char ac_io_icca_device_control_iccard_power_supply_off(int unit) { (void)unit; return 1; }
API char ac_io_icca_device_control_iccard_power_supply_on(int unit) { (void)unit; return 1; }
API char ac_io_icca_device_control_isfinished(int unit, DWORD *a) { (void)unit, (void)a; return 1; }
API char ac_io_icca_get_keep_alive_error(int unit, DWORD *a) {
    (void)unit;
    *a = 0;
    return 0;
}
API int ac_io_icca_send_keep_alive_packet(int a, int b, int c) { (void)a, (void)b, (void)c; return 0; }
API char ac_io_icca_get_status(void *out, int unit) {
    struct reader *r = reader(unit);
    trace("get_status", unit, r->state);
    update(unit);
    r->st.state = (uint8_t)r->state;
    memcpy(out, &r->st, sizeof r->st);
    return 1;
}
API char ac_io_icca_get_uid(int unit, char *card) {
    struct reader *r = reader(unit);
    trace("get_uid", unit, r->state);
    memcpy(card, r->st.uid, 8);
    return r->state != ERR_GETUID;
}
API char ac_io_icca_req_uid(int unit) {
    struct reader *r = reader(unit);
    trace("req_uid", unit, r->state);
    r->state = GET_USERID;
    update(unit);
    return 1;
}
API int ac_io_icca_req_uid_isfinished(int unit, DWORD *st) {
    struct reader *r = reader(unit);
    trace("req_uid_isfinished", unit, r->state);
    update(unit);
    if (r->state == ACTIVE) r->state = END;
    *st = (DWORD)r->state;
    return 1;
}
API int ac_io_icca_workflow(int w, int unit) {
    struct reader *r = reader(unit);
    trace(unit == 1 && two_readers() ? "workflow (reader 2)" : "workflow", w, r->state);
    if (w != STEP) r->ended = 0;
    switch (w) {
    // IIDX GOLD reads a pass, then ends the reading and waits for the reader to say END (with the pass
    // still in: ACTIVE for ever held it at "please wait")
    case END:
        if (two_readers()) r->state = END, r->ended = 1;
        break;
    // IIDX 13 steps its empty readers all the time: an empty one stays as it is (ready while the entry
    // is open, asleep once closed). Taken for a card on its way out instead, the entry page said CARD
    // OUT on a side with no card; set back to ready after a close, the game closed it again for ever
    case STEP:
        if (!(two_readers() && !r->card_at)) r->state = STEP;
        break;
    case SLEEP: r->state = SLEEP; break;
    case INIT: r->state = READY; break;
    case START: r->state = r->card_at ? ACTIVE : READY; break;
    case EJECT:
        if (slot_reader() && r->card_at) {   // pushed out: the player takes it in a moment
            if (!r->ejected_at) r->ejected_at = GetTickCount();
        } else {
            r->card_at = 0;
        }
        break;
    case CLOSE_EJECT:   // IIDX GOLD: the pass pushed out as by EJECT (it waited with the pass held in)
        if (two_readers() && r->card_at && !r->ejected_at) r->ejected_at = GetTickCount();
        r->state = r->card_at ? EJECT_CHECK : SLEEP;
        break;
    case CLOSE_END: r->state = SLEEP; break;
    case GET_USERID: r->state = GET_USERID; break;
    default: break;
    }
    update(unit);
    return r->state;
}

// The status buffer's C++ pair, and the other boards' calls the library also has (pjec, pjei:
// other cabinets'): there, answering yes
void __cdecl icca_get_status_buffer(void *buf, int unit) {
    struct reader *r = reader(unit);
    update(unit);
    r->st.state = (uint8_t)r->state;
    memcpy(buf, &r->st, sizeof r->st);
}
int __cdecl icca_update_status_buffer(int a) { (void)a; return 1; }

#define YES(name) API int name(int a, int b, int c, int d) { (void)a, (void)b, (void)c, (void)d; return 1; }
YES(ac_io_pjec_create_get_status_thread)
YES(ac_io_pjec_destroy_get_status_thread)
YES(ac_io_pjec_get_control_status_buffer)
YES(ac_io_pjec_get_ps2)
YES(ac_io_pjec_set_motor)
YES(ac_io_pjec_update_control_status_buffer)
YES(ac_io_pjei_add_coin)
YES(ac_io_pjei_consume_coinstock)
YES(ac_io_pjei_control_coin_blocker_off)
YES(ac_io_pjei_control_coin_blocker_on)
YES(ac_io_pjei_control_lamp_bright)
YES(ac_io_pjei_control_lamp_mode)
YES(ac_io_pjei_control_lamp_off)
YES(ac_io_pjei_control_lamp_on)
YES(ac_io_pjei_control_parallel_off)
YES(ac_io_pjei_control_parallel_on)
YES(ac_io_pjei_control_reset)
YES(ac_io_pjei_create_get_status_thread)
YES(ac_io_pjei_current_coinstock)
YES(ac_io_pjei_destroy_get_status_thread)
YES(ac_io_pjei_get_coin_input_wave_buffer)
YES(ac_io_pjei_get_control_status_buffer)
YES(ac_io_pjei_get_softwareid)
YES(ac_io_pjei_get_systemid)
YES(ac_io_pjei_get_watchdog_status)
YES(ac_io_pjei_get_watchdog_time_min)
YES(ac_io_pjei_get_watchdog_time_now)
YES(ac_io_pjei_lock_coincounter)
YES(ac_io_pjei_req_coin_input_wave)
YES(ac_io_pjei_req_get_control_status)
YES(ac_io_pjei_req_secplug_check)
YES(ac_io_pjei_req_secplug_check_isfinished)
YES(ac_io_pjei_req_secplug_check_softwareplug)
YES(ac_io_pjei_req_secplug_check_systemplug)
YES(ac_io_pjei_req_secplug_missing_check)
YES(ac_io_pjei_req_secplug_missing_check_isfinished)
YES(ac_io_pjei_set_framing_err_packet_send_interval)
YES(ac_io_pjei_set_watchdog_time)
YES(ac_io_pjei_unlock_coincounter)
YES(ac_io_pjei_update_control_status_buffer)

// popn22.dll's (PLAN.md section 35): a FeliCa card is any but an ISO 15693 one (E0 04 ...); its number
// as the plain one's, then a byte saying it's FeliCa (spice2x)
static int felica(int unit) { return !(reader(unit)->st.uid[0] == 0xe0 && reader(unit)->st.uid[1] == 0x04); }
API char ac_io_icca_is_felica(void) { return (char)felica(0); }
API char ac_io_icca_get_uid_felica(int unit, char *card) {
    struct reader *r = reader(unit);
    trace("get_uid_felica", unit, r->state);
    memcpy(card, r->st.uid, 8);
    card[8] = (char)felica(unit);
    return r->state != ERR_GETUID;
}
API char ac_io_icca_crypt_init(int unit) { (void)unit; return 1; }
// ... and the card dispenser (none: every request done at once) and the LED strip (lit by nothing)
API char ac_io_hbhi_req_carddispenser_init(void) { trace("dispenser_init", 0, 0); return 1; }
API char ac_io_hbhi_req_carddispenser_init_isfinished(int *n) { trace("dispenser_init_isfinished", *n, 0); ++*n; return 1; }
API char ac_io_hbhi_req_carddispenser_get_status(void) { trace("dispenser_get_status", 0, 0); return 1; }
API int ac_io_hbhi_req_carddispenser_get_status_isfinished(int *n) { trace("dispenser_status_isfinished", *n, 0); ++*n; return 2; }
API char ac_io_hbhi_req_carddispenser_disburse(void) { trace("dispenser_disburse", 0, 0); return 1; }
API char ac_io_hbhi_req_carddispenser_disburse_isfinished(int *n) { trace("dispenser_disburse_isfinished", *n, 0); ++*n; return 1; }
API int ac_io_hdxs_get_control_status_buffer(int a, void *buf) { (void)a; memset(buf, 0, 32); return 1; }
API char ac_io_hdxs_update_control_status_buffer(int a) { (void)a; return 1; }
API int ac_io_hdxs_led_scroll(int a, char b, char c, char d, char e, char f, char g_, char h, char i, char j, char k, char l,
                              char m) {
    (void)a, (void)b, (void)c, (void)d, (void)e, (void)f, (void)g_, (void)h, (void)i, (void)j, (void)k, (void)l, (void)m;
    return 1;
}
API int ac_io_hdxs_led_set_pattern(int index, char r, char g_, char b, uint64_t bits) {
    (void)index, (void)r, (void)g_, (void)b, (void)bits;
    return 1;
}
API int ac_io_hdxs_led_set_rgb_mask(int a, char b, char c, long d) { (void)a, (void)b, (void)c, (void)d; return 1; }

// Whichever of HIRAKU's DLLs loads first sets the game up (runtime/pc/cabinet.c: once)
BOOL WINAPI DllMain(HINSTANCE h, DWORD why, LPVOID r) {
    (void)h, (void)r;
    if (why == DLL_PROCESS_ATTACH) {
        // IIDX GOLD on: no RtEffect.dll to start the C02 board and set up the cabinet from (rteffect.c), so here
        const char *board = getenv("HK_PC_BOARD_IN_ACIO");
        if (board && *board) hk_ezusb_dev_start();
        hk_cabinet_start();
        if (board && *board) hk_c02_set_up_cabinet(), hk_c02_movies(), hk_c02_debug_lines();
    }
    return TRUE;
}
