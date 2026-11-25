// SPDX-License-Identifier: GPL-3.0-or-later
// hiraku-pc's link (runtime/pc/link.h), as pop'n 15-18's DLLs share it (runtime/pc/cabinet.c):
// HK_PC_LINK names the file hiraku-pc maps. The controls come in (the pad's bits, the coins, the
// keys bound to them: the game's window may have the keyboard), the main screen's pictures go out
// in step with hiraku-pc's pacing. Without it the loader runs alone (the game's own windows).
#include "pc64.h"
#include "../pc/link.h"
#include "../shell/wintimer.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tlhelp32.h>

static struct hk_pc_link *g_link;
static int g_looked;

struct hk_pc_link *pc64_link(void) {
    if (g_looked) return g_link;
    g_looked = 1;
    const char *f = getenv("HK_PC_LINK");
    if (!f || !*f) return NULL;
    HANDLE h = CreateFileA(f, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    HANDLE m = h != INVALID_HANDLE_VALUE ? CreateFileMappingA(h, NULL, PAGE_READWRITE, 0, HK_PC_LINK_SIZE, NULL) : NULL;
    struct hk_pc_link *l = m ? (struct hk_pc_link *)MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, HK_PC_LINK_SIZE) : NULL;
    if (m) CloseHandle(m);   // the view keeps the mapping
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (l && l->magic == HK_PC_LINK_MAGIC && l->version == HK_PC_LINK_VERSION) {
        g_link = l;
        pc64_log("linked to hiraku-pc (%s)", f);
    } else {
        pc64_log("HK_PC_LINK=%s: not hiraku-pc's link (version %u)", f, l ? l->version : 0);
    }
    return g_link;
}

int pc64_host_shows(void) {
    struct hk_pc_link *l = pc64_link();
    return l && l->host_shows;
}

static int key_down(int vk) { return vk && (GetAsyncKeyState(vk) & 0x8000) != 0; }

// Escape in one of the game's windows ends it (HIRAKU's window ends with it)
static void escape_quits(void) {
    if (!key_down(VK_ESCAPE)) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    if (pid != GetCurrentProcessId()) return;
    pc64_log("Escape: the game ends");
    TerminateProcess(GetCurrentProcess(), 0);
}

// The inputs held now (PC64_IN_*): the link's pad and the keys bound to its bits, read at most
// once a millisecond (each key a call Wine may take to its server), and the coins as the coin
// switch's pulses (four status reads down, four up: a coin each)
// The link's pad as it is, with the keys bound to its bits held: which bits mean what is the game's
// (link.h's HK_PC_PAD_* for the other 64-bit game, the HK_PC_C02_* ones for a 64-bit game of the
// family "iidx")
unsigned pc64_link_pad(void) {
    struct hk_pc_link *l = pc64_link();
    if (!l) return 0;
    static uint32_t keys;
    static DWORD keys_at;
    const DWORD now = GetTickCount();
    if (now != keys_at) {
        keys_at = now;
        escape_quits();
        keys = 0;
        for (int i = 0; i < 32; i++)
            if (key_down(l->keys[i][0]) || key_down(l->keys[i][1])) keys |= 1u << i;
    }
    return l->pad | keys;
}

unsigned pc64_link_inputs(void) {
    struct hk_pc_link *l = pc64_link();
    if (!l) return 0;
    const uint32_t pad = pc64_link_pad();
    l->game_alive++;
    unsigned in = 0;
    if (pad & HK_PC_PAD_TEST) in |= PC64_IN_TEST;
    if (pad & HK_PC_PAD_SERVICE) in |= PC64_IN_SERVICE;
    for (int b = 1; b <= 9; b++)
        if (pad & HK_PC_PAD_BUTTON(b)) in |= PC64_IN_BUTTON(b);
    if (pad & HK_PC_PAD_RED) in |= PC64_IN_RED;
    if (pad & HK_PC_PAD_BLUE) in |= PC64_IN_BLUE;
    if (pad & HK_PC_PAD_HEADPHONES) in |= PC64_IN_HEADPHONES;
    static uint32_t coins_seen, phase;
    static int coin_was;
    const int coin_key = key_down(l->coin_keys[0]) || key_down(l->coin_keys[1]);
    if (coin_key && !coin_was) coins_seen--;   // a key's coin: one more to pulse
    coin_was = coin_key;
    if (phase) {
        if (++phase <= 4) in |= PC64_IN_COIN;
        if (phase == 8) phase = 0;
    } else if (coins_seen != l->coins) {
        coins_seen++;
        phase = 1;
        in |= PC64_IN_COIN;
    }
    return in;
}

// The buttons held now (PC64_IN_BUTTON), the link's and the keys bound to them, with nothing else
// done (pc64_link_inputs counts coins and the game's polls: the game's board reads only)
unsigned pc64_link_buttons(void) {
    struct hk_pc_link *l = pc64_link();
    if (!l) return 0;
    uint32_t pad = l->pad;
    for (int b = 1; b <= 9; b++) {
        const int i = __builtin_ctz(HK_PC_PAD_BUTTON(b));
        if (key_down(l->keys[i][0]) || key_down(l->keys[i][1])) pad |= HK_PC_PAD_BUTTON(b);
    }
    unsigned in = 0;
    for (int b = 1; b <= 9; b++)
        if (pad & HK_PC_PAD_BUTTON(b)) in |= PC64_IN_BUTTON(b);
    return in;
}

// Player 1's 10-key: the link's, and the keys bound to it (the game's window may have the keyboard)
volatile unsigned pc64_keys_held;
unsigned pc64_link_keypad(void) {
    struct hk_pc_link *l = pc64_link();
    if (!l) return pc64_keys_held;
    unsigned k = l->keypad | pc64_keys_held;
    for (int i = 0; i < 12; i++)
        if (key_down(l->keypad_keys[i][0]) || key_down(l->keypad_keys[i][1])) k |= 1u << i;
    return k;
}

// The card held to the reader: a count that moves on with each (the link's, a key bound to it,
// HK_SCRIPT's "card"), and its UID (the link's, else HK_PC64_CARD's 16 hex digits)
volatile unsigned pc64_card_presses;
uint32_t pc64_card(uint8_t uid[8]) {
    static int was;
    static uint32_t keys;
    struct hk_pc_link *l = pc64_link();
    const int now = l && (key_down(l->card_keys[0]) || key_down(l->card_keys[1]));
    if (now && !was) keys++;
    was = now;
    memset(uid, 0, 8);
    if (l) {
        memcpy(uid, (const void *)l->card_uid, 8);
    } else {
        const char *hex = getenv("HK_PC64_CARD");
        for (int i = 0; hex && i < 8 && hex[2 * i] && hex[2 * i + 1]; i++) {
            unsigned b;
            if (sscanf(hex + 2 * i, "%2x", &b) == 1) uid[i] = (uint8_t)b;
        }
    }
    return (l ? l->card_seq : 0) + keys + pc64_card_presses;
}

// A fifth of a millisecond's nap, as cabinet.c's: to the timer's next tick on Windows, half a
// millisecond (main asks for that)
void pc64_nap(void) { hk_win_nap(2000); }

// The main screen's picture is in the link's pixels (pc64_link_pixels): counted, then the game
// waits until hiraku-pc has shown it and its pacing starts the next frame (a quarter of a second
// at most, if it has stopped answering)
// The CPU time this process has used, for hiraku-pc's performance overlay (link.h cpu_ms): once a second
static void cpu_time(struct hk_pc_link *l) {
    static DWORD last;
    const DWORD now = GetTickCount();
    FILETIME c, e, k, u;
    if (now - last < 1000) return;
    last = now;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return;
    l->cpu_ms = (uint32_t)((((uint64_t)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((uint64_t)u.dwHighDateTime << 32 | u.dwLowDateTime)) / 10000);
    l->cpu_at_ms = now ? now : 1;
}

// A test-rig aid only, and OFF unless HK_PC64_PACE_120=1 is in the environment: this game's frames
// on the PC's own clock at 120 Hz, its cabinet's rate. Its start-up monitor check measures the frame
// rate and fails it outside 119.0-121.0 Hz, and the test rig's pacing (the shell's sound clock) has
// measured a little fast there, so the rig sets the switch to pass the check. It must NOT be on for
// a player: the shell (hiraku-pc) is the one pacer — frame_go follows the display's refresh or the
// sound device — and a second clock here fights it: when frame_go runs a little faster than this
// grid the game is held later and later (up to half a frame), then the grid snaps — a moving delay
// and a hitch every few seconds, in a rhythm game. A late frame never starts a catch-up burst here:
// the next slice begins one tick after NOW, never earlier, so no two frames come closer than the
// grid and the game's own meter never reads above 120.
static int frame_log_on(void);   // below, the HK_PC64_FRAME_LOG switch
void pc64_waits_window(void);   // waits.c: the per-window wait sites

// HK_PC64_PACE_120: 1 = the whole run, N > 1 = the first N frame_go's only (the rig turns it off
// once the game's start-up monitor check has passed, so the rest of the run is measured without it)
static int pace_120_limit(void) {
    static int lim = -2;
    if (lim == -2) {
        lim = 0;
        const char *v = getenv("HK_PC64_PACE_120");
        if (v && *v) {
            if (!strcmp(v, "1")) {
                lim = -1;
                pc64_log("HK_PC64_PACE_120=1: the 120 Hz test pacer is ON for the whole run (a test-rig aid only, not for players)");
            } else {
                const long n = atol(v);
                if (n > 1) {
                    lim = (int)n;
                    pc64_log("HK_PC64_PACE_120=%ld: the 120 Hz test pacer is ON for the first %ld frame_go's only (a test-rig aid, not for players)", n, n);
                }
            }
        }
    }
    return lim;
}
static int pace_120_expired;
static int pace_120_on(void) {
    return pace_120_limit() && !pace_120_expired;
}

static void pace_frame_120(void) {
    static int64_t fhz;
    static int64_t next;
    static unsigned count;
    LARGE_INTEGER q;
    if (!fhz) {
        if (!QueryPerformanceFrequency(&q)) return;
        fhz = q.QuadPart;
    }
    if (!QueryPerformanceCounter(&q)) return;
    const int64_t now = q.QuadPart;
    const int64_t tick = (fhz * 1000 + 60000) / 120000;   // 1/120 s in QPC ticks, rounded
    if (!next) next = now;
    for (int64_t t = now; t < next;) {
        const int64_t left = next - t;
        if (left > tick / 4) pc64_nap();
        else if (left > tick / 40) SwitchToThread();
        else YieldProcessor();
        if (!QueryPerformanceCounter(&q)) return;
        t = q.QuadPart;
    }
    // a late frame must not start a catch-up burst (two frames close together read above 120 on
    // the game's own meter): the next slice starts one tick after now, never earlier
    if (!QueryPerformanceCounter(&q)) return;
    next = (next > q.QuadPart ? next : q.QuadPart) + tick;
    count++;
    const int lim = pace_120_limit();
    if (lim > 0 && (int)count >= lim && !pace_120_expired) {
        pace_120_expired = 1;
        pc64_log("HK_PC64_PACE_120=%d: the pacer is OFF now, after %u frame_go's", lim, count);
    }
    if (frame_log_on()) {   // HK_PC64_FRAME_LOG: the pacer's own hold, a column of its own
        LARGE_INTEGER q2;
        if (QueryPerformanceCounter(&q2))
            pc64_note_pace((uint32_t)((q2.QuadPart - q.QuadPart) * 1000000 / fhz));
    }
}

// HK_PC64_FRAME_LOG (tests, off by default): per 1200 frames, the mean and worst of each part of
// a frame's span, split: from the frame_go to the game's Present/PresentEx call (pc64_note_present_
// enter, from display.c) minus the pacer's own hold (pc64_note_pace) and the Direct3D layer's time
// inside the real Present (pc64_note_present_span) = the game's own work; then the picture's
// read-back into the link (pc64_note_readback, from display.c); the wait for the next frame_go;
// the device's WaitForVBlank/GetRasterStatus if the game calls them (pc64_note_devwait); how many
// frame_go's found the game still busy (late), and how many pictures were not shown; and, for the
// game's main thread, the waits called from the game's DLL (waits.c, top three sites per window).
// With every window and at exit, the ten threads with the most CPU time, each with the module its
// start address lies in (the rig's runner kills the loader at the end of a run, so a table printed
// only at exit would never be flushed).
void frame_log_exit(void);
static int frame_log_on(void) {
    static int on = -1;
    if (on < 0) {
        on = getenv("HK_PC64_FRAME_LOG") && *getenv("HK_PC64_FRAME_LOG") == '1';
        if (on) atexit(frame_log_exit);
    }
    return on;
}
int pc64_frame_log_on(void) { return frame_log_on(); }
static int sync_log_on(void) {   // HK_PC64_SYNC_LOG: the song-sync line every 600 frames
    static int on = -1;
    if (on < 0) on = getenv("HK_PC64_SYNC_LOG") && *getenv("HK_PC64_SYNC_LOG") == '1';
    return on;
}
static int64_t fl_fhz(void) {
    static int64_t fhz;
    LARGE_INTEGER q;
    if (!fhz && QueryPerformanceFrequency(&q)) fhz = q.QuadPart;
    return fhz;
}
static uint32_t fl_us_since(int64_t t0) {   // microseconds since t0 (QPC)
    LARGE_INTEGER q;
    if (!QueryPerformanceCounter(&q)) return 0;
    return (uint32_t)((q.QuadPart - t0) * 1000000 / fl_fhz());
}
static double fl_s_draw, fl_s_wait, fl_s_rb, fl_s_pre, fl_s_pres, fl_s_pace;
static uint32_t fl_w_draw, fl_w_wait, fl_w_rb, fl_w_pre, fl_w_pres, fl_w_pace;
static uint32_t fl_late, fl_again, fl_n, fl_win0, fl_paced;
static uint32_t fl_vb_n, fl_vb_us, fl_ras_n, fl_ras_us;   // WaitForVBlank / GetRasterStatus
void pc64_note_readback(uint32_t us) {   // display.c: the picture's copy to the link took this
    if (!frame_log_on()) return;
    fl_s_rb += us;
    if (us > fl_w_rb) fl_w_rb = us;
}
void pc64_note_present_span(uint32_t us) {   // display.c: the real Present call took this
    if (!frame_log_on()) return;
    fl_s_pres += us;
    if (us > fl_w_pres) fl_w_pres = us;
}
// The start-up monitor check's span, ON BY DEFAULT for a 64-bit PC IIDX (the product's behaviour,
// not a test switch): from the game's "boot_status" log line to the title's first movie load
// ("CMovieManager"; both seen by main.c's avs_log, and the movie load is always AFTER the check on
// any boot, so first-boot writes of dev\nvram\cabinet.xml cannot end the span early). In the span
// the check's meter is shown a clock advancing one frame period of the rate HIRAKU keeps per picture
// handed (link->game_hz_milli), so the check measures the frame grid, not the PC's clock; frames are
// released with no grid of ours (on a player's PC the shell is the one pacer). The check's immediate
// accept window is widened in memory to half a hertz either side of that rate (widen_window below —
// the only patch of the game here), so the true rate passes in the FIRST round. Fail-safe: the span
// also ends at the cap. Switches: HK_PC64_MONITOR_CHECK=window-off (the link's rate, the game's own
// window: four rounds), =seed (119.985 shown, one round, no patch), =real (the game's real-time
// check + the span's 1/120 s grid, no meter hook). HK_PC64_PACE_120 stays for tests.
#define SPAN_CAP_FRAMES 90000u   // fail-safe only: the span ends at the title (observed 10788; a first boot boots longer —
                             // 60000+ shell frames) — 90000 frames (750 s) covers it; =window-off worst case was 17988
// The seed rate for =seed: the middle of the game's own immediate accept window [119.975, 119.995].
#define HK_PC64_MONITOR_RATE_MILLI 119985u
static int g_span;
static unsigned g_span_n, g_span_at;
static int64_t g_span_qpc;
static int mon_mode(void) {   // HK_PC64_MONITOR_CHECK: "" window (default), window-off, seed, real
    static int m = -1;
    if (m < 0) {
        const char *v = getenv("HK_PC64_MONITOR_CHECK");
        m = v && !strcmp(v, "window-off") ? 1 : v && !strcmp(v, "real") ? 2
          : v && !strcmp(v, "seed") ? 3 : 0;
    }
    return m;
}
// The meter is a ring of 360 QPC stamps, one per presented frame, read via the game's own import of
// kernel32!QueryPerformanceCounter. Only that one read is substituted — bounded to the span, the
// game's main thread and the meter's call site (found by shape: the QPC call followed by the ring's
// ÷360 magic). Every other clock read (the game's pacer, the check's timeGetTime deadlines, all
// other modules' imports) gets the real time.
static BOOL(WINAPI *r_QPC)(LARGE_INTEGER *);
static HMODULE g_game;
static void *g_meter_ret;
static DWORD g_main;
static int g_von;
static uint64_t v_base, v_qpc, v_acc, v_freq;
static uint32_t v_hz;          // the rate the meter is shown, milli-Hz
static unsigned v_moves;       // times the link's rate moved during the span
static void window_follow(uint32_t milli);   // below
static void meter_advance(void) {   // one frame period of the shown rate, per picture handed
    // the rate followed as the link gives it now: the shell writes game_hz_milli with each frame
    // (with display sync on, the display's measured rate, known only once frames reach it - after
    // the span has started at the game's first log line); the meter's clock goes on from where it
    // is at the new rate, so what the check measures is the rate kept through its round
    if (mon_mode() != 3) {
        struct hk_pc_link *l = pc64_link();
        const uint32_t now = l && l->game_hz_milli ? l->game_hz_milli : v_hz;
        if (now != v_hz) {
            if (!v_moves++ || v_moves % 1000 == 0)
                pc64_log("monitor check: the link's rate %.3f -> %.3f Hz (frame %u, %u moves): the meter follows",
                         v_hz / 1000.0, now / 1000.0, pc64_frames, v_moves);
            v_base = v_qpc, v_acc = 0, v_hz = now;
            if (mon_mode() == 0) window_follow(now);
        }
    }
    v_acc += v_freq * 1000ull;
    v_qpc = v_base + v_acc / v_hz;
}
static BOOL WINAPI QPC_(LARGE_INTEGER *out) {
    if (out && g_von && GetCurrentThreadId() == g_main && __builtin_return_address(0) == g_meter_ret) {
        out->QuadPart = (LONGLONG)v_qpc;   // the meter's read: the frame grid's clock
        return TRUE;
    }
    return r_QPC(out);
}
// The check's immediate accept window, widened in memory (the only patch of the game allowed here).
// Its round code ends a round with the compare pair: comisd xmm1,[rsp+0x58] (the lower bound, stored
// to the stack just before) ; jb re-measure ; movsd xmm0,[rip+..] (the upper bound, a .rdata double)
// ; comisd xmm0,xmm1 ; jae pass. The bounds are found by that byte shape, never by fixed address;
// in the dump each bound constant is read at exactly one site, only by this round code (the final
// range 119.0..121.0 and the 119.982 accumulation centre are separate constants, not touched).
// Both bounds are replaced by half a hertz either side of the rate the meter is shown, so a
// 119.88 Hz pace would pass too. Rounds, final range, error and store stay the game's own.
static double *g_win_up, *g_win_lo[4];   // the bounds found (widen_window), for window_follow
static int g_win_nlo;
static uint32_t g_win_mid;                // the rate they were last centred on, milli-Hz
static void set_bound(double *at, double v) {
    DWORD old;
    VirtualProtect(at, 8, PAGE_EXECUTE_READWRITE, &old);
    *at = v;
    VirtualProtect(at, 8, old, &old);
}
// The link's rate moved (meter_advance): the window centred on it again once it is 0.1 Hz off the
// last centre (the measured refresh's small changes leave it be: half a hertz either side covers them)
static void window_follow(uint32_t milli) {
    if (!g_win_up || (milli + 100 > g_win_mid && milli < g_win_mid + 100)) return;
    const double lo = milli / 1000.0 - 0.5, hi = milli / 1000.0 + 0.5;
    set_bound(g_win_up, hi);
    for (int k = 0; k < g_win_nlo; k++) set_bound(g_win_lo[k], lo);
    g_win_mid = milli;
    pc64_log("monitor check: the immediate window centred again on the link's rate: [%.3f, %.3f] Hz (frame %u)", lo, hi, pc64_frames);
}
static void widen_window(BYTE *game, uint32_t shown_milli) {
    const double lo = shown_milli / 1000.0 - 0.5, hi = shown_milli / 1000.0 + 0.5;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(game + ((IMAGE_DOS_HEADER *)game)->e_lfanew);
    IMAGE_SECTION_HEADER *first = IMAGE_FIRST_SECTION(nt);
    for (int s = 0; s < nt->FileHeader.NumberOfSections; s++) {
        IMAGE_SECTION_HEADER *sh = &first[s];
        if (!(sh->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        for (BYTE *p = game + sh->VirtualAddress, *end = p + sh->Misc.VirtualSize; p + 28 < end; p++) {
            if (p[0] != 0x66 || p[1] != 0x0f || p[2] != 0x2f || p[3] != 0x4c || p[4] != 0x24 || p[5] != 0x58)
                continue;                                             // comisd xmm1,[rsp+0x58]
            if (p[6] != 0x72) continue;                               // jb rel8 (re-measure)
            if (p[8] != 0xf2 || p[9] != 0x0f || p[10] != 0x10 || p[11] != 0x05) continue;  // movsd xmm0,[rip+rel32]
            BYTE *up = p + 16 + *(int32_t *)(p + 12);                 // the upper-bound constant
            if (p[16] != 0x66 || p[17] != 0x0f || p[18] != 0x2f || p[19] != 0xc1) continue; // comisd xmm0,xmm1
            if (p[20] != 0x0f || p[21] != 0x83) continue;             // jae (pass)
            double upv = *(double *)up;
            if (upv < 119.9 || upv > 120.1) continue;                 // sanity: the window's upper bound
            BYTE *lowers[4]; int nlow = 0;
            for (BYTE *q = p; q + 8 > p - 0x60 && q >= game + sh->VirtualAddress; q--) {
                if (q[0] == 0x0f && q[1] == 0x11 && q[2] == 0x4c && q[3] == 0x24 && q[4] == 0x58) {  // movups [rsp+0x58],xmm1
                    for (BYTE *r = q; r + 8 > q - 0x20 && r >= game + sh->VirtualAddress; r--)
                        if (r[0] == 0xf2 && r[1] == 0x0f && r[2] == 0x10 && r[3] == 0x05) {          // movsd xmm0,[rip+rel32]
                            BYTE *lo = r + 8 + *(int32_t *)(r + 4);
                            double lov = *(double *)lo;
                            if (lov >= 119.9 && lov < upv) {
                                int seen = 0;
                                for (int k = 0; k < nlow; k++) if (lowers[k] == lo) seen = 1;
                                if (!seen && nlow < 4) lowers[nlow++] = lo;
                            }
                            break;
                        }
                }
            }
            if (!nlow) { pc64_log("monitor check: window upper found at game+%lx (%.3f) but no lower: not patched", (unsigned long)(up - game), upv); return; }
            const double oldup = *(double *)up;
            set_bound((double *)up, hi);
            double oldlo0 = 0;
            for (int k = 0; k < nlow; k++) {
                const double oldlo = *(double *)lowers[k];
                set_bound((double *)lowers[k], lo);
                if (!k) oldlo0 = oldlo;
                pc64_log("monitor check: window bound game+%lx %.3f -> %.3f", (unsigned long)(lowers[k] - game), oldlo, lo);
                g_win_lo[k] = (double *)lowers[k];
            }
            g_win_up = (double *)up, g_win_nlo = nlow, g_win_mid = shown_milli;
            pc64_log("monitor check: the immediate window widened to [%.3f, %.3f] Hz (was [%.3f, %.3f]) so the shown rate passes in one round",
                     lo, hi, oldlo0, oldup);
            return;
        }
    }
    pc64_log("monitor check: the window's compare pair not found by shape: the game's own window applies (four rounds at the shown rate)");
}
void pc64_monitor_install(HMODULE game) {   // main.c: once the game's DLL is loaded
    if (!pc64_game_is_iidx() || mon_mode() == 2) return;
    g_game = game;
    g_main = GetCurrentThreadId();
    void **slot = pc64_import_slot(game, "kernel32.dll", "QueryPerformanceCounter");
    if (!slot) { pc64_log("monitor check: no QueryPerformanceCounter import"); return; }
    BYTE *base = (BYTE *)game;   // the meter's call site, by shape
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *first = IMAGE_FIRST_SECTION(nt);
    for (int s = 0; s < nt->FileHeader.NumberOfSections && !g_meter_ret; s++) {
        IMAGE_SECTION_HEADER *sh = &first[s];
        if (!(sh->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        for (BYTE *p = base + sh->VirtualAddress, *end = p + sh->Misc.VirtualSize; p + 32 < end; p++) {
            if (p[0] != 0xff || p[1] != 0x15) continue;   // call [rip+disp32]
            if ((BYTE *)(p + 6 + *(int32_t *)(p + 2)) != (BYTE *)slot) continue;
            for (int k = 6; k < 6 + 0x20 && p + k + 5 <= end; k++)
                if (p[k] == 0xb8 && !memcmp(p + k + 1, "\xb7\x60\x0b\xb6", 4)) {   // mov eax,0xb60b60b7
                    g_meter_ret = p + 6;
                    pc64_log("monitor check: the meter's QPC call at game+%lx hooked", (unsigned long)(p - base));
                }
        }
    }
    if (!g_meter_ret) { pc64_log("monitor check: no meter call site found by shape"); return; }
    r_QPC = pc64_import(game, "kernel32.dll", "QueryPerformanceCounter", (void *)QPC_);
    LARGE_INTEGER f;
    if (r_QPC && QueryPerformanceFrequency(&f) && f.QuadPart > 0) v_freq = (uint64_t)f.QuadPart;
    if (r_QPC) {
        pc64_log("monitor check: during the span the meter is shown %s (window-off/seed: the game's own window or the seed's; =real: not hooked)",
                 mon_mode() == 3 ? "119.985 Hz (the seed)" : "the link's exact rate");
    }
}
static void span_end_line(const char *how) {
    const int von = g_von;
    g_von = 0;
    LARGE_INTEGER q;
    const double secs = von && v_freq && QueryPerformanceCounter(&q) ? (double)(q.QuadPart - g_span_qpc) / (double)v_freq : 0.0;
    if (von)
        pc64_log("monitor check: the span ends %s after %u frames (from frame %u, %.1f s): the check was shown %.3f Hz%s and stored that figure (%s)",
                 how, g_span_n, g_span_at, secs, v_hz / 1000.0, v_moves ? " (the link's rate at the end; it moved during the span)" : "",
                 mon_mode() == 1 ? "four rounds: outside the game's own immediate window, inside the wide one"
                 : mon_mode() == 3 ? "one round: the seed is inside the game's immediate window"
                 : "one round: inside the widened window");
    else
        pc64_log("monitor check: the span ends %s after %u frames (from frame %u)", how, g_span_n, g_span_at);
}
void pc64_span_start(void) {
    if (g_span || !pc64_game_is_iidx()) return;
    g_span = 1;
    g_span_n = 0;
    g_span_at = pc64_frames;
    LARGE_INTEGER q;
    if (QueryPerformanceCounter(&q)) g_span_qpc = q.QuadPart;
    if (mon_mode() != 2 && r_QPC) {
        g_von = 1;
        struct hk_pc_link *l = pc64_link();
        v_hz = mon_mode() == 3 ? HK_PC64_MONITOR_RATE_MILLI
             : l && l->game_hz_milli ? l->game_hz_milli : 120000;
        // the default: the check's immediate window widened around the rate read now, at the span's
        // start (not when the DLL loaded), and centred again if the link's rate moves later in the
        // span (meter_advance: display sync's measured rate comes once frames reach the display)
        v_moves = 0;
        if (mon_mode() == 0) widen_window((BYTE *)g_game, v_hz);
        if (r_QPC(&q)) { v_base = v_qpc = (uint64_t)q.QuadPart; v_acc = 0; }
        pc64_log("monitor check: the span starts (frame %u): the meter is shown %.3f Hz, one frame period per picture, no grid",
                 pc64_frames, v_hz / 1000.0);
    } else
        pc64_log("monitor check: the span starts (frame %u): frames no faster than the 1/120 s grid (=real)", pc64_frames);
}
void pc64_span_end(void) {   // main.c's avs_log: the title's first CMovieManager line
    if (!g_span) return;
    g_span = 0;
    span_end_line("at the title's first movie load");
}
static void span_tick(void) {   // per frame while the span is on: the fail-safe cap
    if (!g_span) return;
    g_span_n++;
    if (pc64_frames - g_span_at >= SPAN_CAP_FRAMES) {
        g_span = 0;
        span_end_line("by the frame cap");
    }
}
void pc64_note_pace(uint32_t us) {   // the pacer's own hold (pace_frame_120)
    if (!frame_log_on()) return;
    fl_s_pace += us;
    if (us > fl_w_pace) fl_w_pace = us;
    fl_paced++;
}
void pc64_note_devwait(int which, uint32_t us) {   // display.c: WaitForVBlank(0)/GetRasterStatus(1)
    if (!frame_log_on()) return;
    if (which) fl_ras_n++, fl_ras_us += us;
    else fl_vb_n++, fl_vb_us += us;
}
void pc64_link_pad_read(void) {   // the game's first read of its pad in a frame: when (pad_read_us)
    struct hk_pc_link *l = pc64_link();
    static uint32_t read_at_go;
    if (l && read_at_go != l->go_count) {
        read_at_go = l->go_count;
        LARGE_INTEGER q;
        if (fl_fhz() && QueryPerformanceCounter(&q))
            l->pad_read_us = (uint32_t)((uint64_t)((uint32_t)q.QuadPart - l->go_qpc) * 1000000 / (uint64_t)fl_fhz());
    }
}
static void frame_log_threads(void) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    struct ent { uint64_t cpu; DWORD id; char mod[64]; } ents[512];
    int n = 0;
    THREADENTRY32 te;
    te.dwSize = sizeof te;
    if (Thread32First(snap, &te))
        do {
            if (te.th32OwnerProcessID != GetCurrentProcessId() || n >= 512) continue;
            HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (!h) continue;
            FILETIME c, e, k, u;
            if (GetThreadTimes(h, &c, &e, &k, &u)) {
                ents[n].cpu = ((uint64_t)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((uint64_t)u.dwHighDateTime << 32 | u.dwLowDateTime);
                ents[n].id = te.th32ThreadID;
                void *start = NULL;
                typedef LONG(NTAPI *tNtQIT)(HANDLE, LONG, void *, ULONG, ULONG *);
                static tNtQIT p;
                if (!p) p = (tNtQIT)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
                if (p) p(h, 9, &start, sizeof start, NULL);   // ThreadQuerySetWin32StartAddress
                ents[n].mod[0] = '?', ents[n].mod[1] = 0;
                HMODULE m;
                if (start && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                                (const char *)start, &m)) {
                    char path[MAX_PATH];
                    if (GetModuleFileNameA(m, path, sizeof path)) {
                        const char *b = strrchr(path, '\\');
                        snprintf(ents[n].mod, sizeof ents[n].mod, "%s", b ? b + 1 : path);
                    }
                }
                n++;
            }
            CloseHandle(h);
        } while (Thread32Next(snap, &te));
    CloseHandle(snap);
    for (int i = 1; i < n; i++)   // insertion sort, descending by cpu
        for (int j = i; j > 0 && ents[j - 1].cpu < ents[j].cpu; j--) {
            struct ent t = ents[j - 1]; ents[j - 1] = ents[j]; ents[j] = t;
        }
    for (int i = 0; i < n && i < 10; i++)
        pc64_log("frame log: thread %lu cpu %.1f s: %s", ents[i].id, ents[i].cpu / 10000000.0, ents[i].mod);
}
void frame_log_exit(void) {
    pc64_log("frame log: at exit: main frames %u, sub frames %u, late %u, not shown %u",
             pc64_frames, pc64_sub_frames, fl_late, fl_again);
    frame_log_threads();
}
void pc64_frame_log_exit(void) { if (frame_log_on()) frame_log_exit(); }

void pc64_link_picture(unsigned w, unsigned h) {
    pc64_patches_apply();   // (the mods' patches: each frame, as the 32-bit games' DLL)
    struct hk_pc_link *l = pc64_link();
    cpu_time(l);
    LARGE_INTEGER q0;
    const int64_t t0 = (frame_log_on() && QueryPerformanceCounter(&q0)) ? q0.QuadPart : 0;
    if (l->go_count && fl_fhz()) {   // the picture is ready: the game's own work since the last go
        LARGE_INTEGER q;
        if (QueryPerformanceCounter(&q))
            l->drawn_us = (uint32_t)((uint64_t)((uint32_t)q.QuadPart - l->go_qpc) * 1000000 / (uint64_t)fl_fhz());
    }
    l->frame_w = w, l->frame_h = h;
    MemoryBarrier();
    l->frame_seq = l->frame_seq + 1;
    l->frames++;
    if (g_von) meter_advance();   // one frame period of the link's rate, per picture handed
    // HK_PC64_SYNC_LOG=1 (a test switch): every 600 frames one line, for a song's sync against the
    // sound — "sync f<shell frame>: game_frames <the game's song frame counter [game+0xaa94798]>,
    // song_ms <game_frames*1000/cfg+0x1c, the game's song clock>, rate <cfg+0x1c>, drift <the game's
    // drift word [game+0xaa947a8]>, snd_written/snd_played <the stream's counters in sound.c>,
    // snd_ms <snd_played*1000/HK_PC_AUDIO_RATE, the sound's position>".
    if (sync_log_on() && pc64_frames && pc64_frames % 600 == 0) {
        unsigned sw = 0, sp = 0;
        pc64_sound_pos(&sw, &sp);
        const BYTE *g = (const BYTE *)g_game;
        const uint32_t gf = g ? *(const volatile uint32_t *)(g + 0xaa94798) : 0;   // the game's frame counter
        const uint32_t dr = g ? *(const volatile uint32_t *)(g + 0xaa947a8) : 0;   // its drift word
        float rate = 0.0f;
        const void *cfgp = g ? *(void *const *)(g + 0xabaf550) : 0;
        if (cfgp) rate = *(const float *)((const BYTE *)cfgp + 0x258 + 0x1c);    // cfg+0x1c
        pc64_log("sync f%u: game_frames %u, song_ms %.1f, rate %.3f, drift %u, snd_written %u, snd_played %u, snd_ms %llu",
                 pc64_frames, gf, gf && rate > 0 ? gf * 1000.0 / (double)rate : 0.0, (double)rate, dr, sw, sp,
                 HK_PC_AUDIO_RATE ? (unsigned long long)sp * 1000ull / HK_PC_AUDIO_RATE : 0ull);
    }
    const uint32_t seq = l->frame_seq;
    const DWORD t0g = GetTickCount();
    for (int spins = 0; l->frame_go != seq; spins++) {
        if (GetTickCount() - t0g > 250) break;
        if (spins < 200) YieldProcessor();
        else if (spins < 400) SwitchToThread();
        else pc64_nap();
    }
    l->go_count = l->go_count + 1;
    int late = 0;
    if (l->frame_go == seq) {
        LARGE_INTEGER q;
        if (fl_fhz() && QueryPerformanceCounter(&q)) {
            l->go_qpc = (uint32_t)q.QuadPart;   // the go seen now
            if (pc64_game_is_iidx() && (pace_120_on() || (g_span && mon_mode() == 2))) {
                pace_frame_120();   // the rig switch, or the span's grid in the =real mode only
            }
            if (g_span) span_tick();
        }
    } else late = 1;
    if (frame_log_on()) {
        const uint32_t wait_us = t0 ? fl_us_since(t0) : 0;
        fl_s_draw += l->drawn_us, fl_s_wait += wait_us;
        if (l->drawn_us > fl_w_draw) fl_w_draw = l->drawn_us;
        if (wait_us > fl_w_wait) fl_w_wait = wait_us;
        const uint32_t pre = l->drawn_us;   // go -> this Present call (the picture is read inside it)
        fl_s_pre += pre;
        if (pre > fl_w_pre) fl_w_pre = pre;
        fl_late += late, fl_again += l->not_shown ? 1 : 0, fl_n++;
        if (l->frames - fl_win0 >= 1200) {
            pc64_log("frame log: frames %u-%u [pace on %u]: go->Present mean %.3f worst %.3f ms, of it the game's own %.3f, "
                     "inside Present mean %.3f worst %.3f ms, read-back mean %.3f worst %.3f ms, wait mean %.3f worst %.3f ms, "
                     "pacer mean %.3f worst %.3f ms, vblank %u/%.1f us raster %u/%.1f us, late %u, not shown %u",
                     fl_win0, l->frames, fl_paced,
                     fl_s_pre / fl_n / 1000.0, fl_w_pre / 1000.0,
                     (fl_s_pre - fl_s_pace - fl_s_pres) / fl_n / 1000.0,
                     fl_s_pres / fl_n / 1000.0, fl_w_pres / 1000.0,
                     fl_s_rb / fl_n / 1000.0, fl_w_rb / 1000.0,
                     fl_s_wait / fl_n / 1000.0, fl_w_wait / 1000.0,
                     fl_s_pace / fl_n / 1000.0, fl_w_pace / 1000.0,
                     fl_vb_n, fl_vb_us / 1000.0, fl_ras_n, fl_ras_us / 1000.0, fl_late, fl_again);
            pc64_waits_window();   // waits.c: the game's main thread's waits from the game's DLL, top 3 sites
            fl_win0 = l->frames, fl_s_draw = fl_s_wait = fl_s_rb = fl_s_pre = fl_s_pres = fl_s_pace = 0;
            fl_w_draw = fl_w_wait = fl_w_rb = fl_w_pre = fl_w_pres = fl_w_pace = 0;
            fl_late = fl_again = fl_n = fl_paced = 0;
            fl_vb_n = fl_vb_us = fl_ras_n = fl_ras_us = 0;
            frame_log_threads();   // the exit table too, but every window: the runner's kill can cut the tail
        }
    }
}

int pc64_link_not_shown(void) {
    struct hk_pc_link *l = pc64_link();
    return l && l->not_shown;
}

uint32_t *pc64_link_pixels(void) {
    struct hk_pc_link *l = pc64_link();
    return l ? (uint32_t *)((char *)l + HK_PC_PIXELS) : NULL;
}
