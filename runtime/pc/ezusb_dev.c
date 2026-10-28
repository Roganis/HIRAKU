// SPDX-License-Identifier: GPL-3.0-or-later
// The C02 and D01 cabinets' EZ-USB board as the device under its driver (PLAN.md section 36): from
// beatmania IIDX RED on, the board's library is built into the game's program, which opens the
// driver itself (\\.\Ezusb-0) and talks to the board's firmware with DeviceIoControl, so HIRAKU
// answers there instead of in an ezusb.dll (9th style's, ezusb_c02.c). What the firmware answers
// is bemanitools' (public domain: ezusb-emu, ezusb-iidx-emu, security), read and written again
// here for HIRAKU's cabinet (runtime/pc/link.h).
//
// The driver's calls: the device's descriptor, vendor requests (the 8051 held in reset while its
// firmware is loaded, then let go), the firmware's bytes, and reads and writes on its pipes. The
// interrupt pipes carry a 16-byte packet each way, each 1/1000 s or so: in, the pad (active low),
// the coins counted, the turntables, the effect sliders and the answer to the last command; out, the lamps and a
// command for one of the board's nodes. The bulk pipes carry 64-byte pages to and from the node
// the last command named:
//   0x01 the security plug (the black round plug: its ROM, the PCB id, and its data, signed for the
//        game; ezusb_dev_sign below)   0x02 the board's EEPROM   0x04 the H8 behind the FPGA: the
//   magnetic card readers and the 10-keys (a serial stream of messages, h8_answer)   0x05 the
//   16-segment ticker   0x09 the coin mode   0x0c the watchdog   0x10 the FPGA (its program,
//   data\hex\*.hex, written at start-up)   0x40 the SRAM   0xfe the security memory (five pages
//   the game writes; the plug's ROM is read back through them)
//
// Step 1: the game boots to its attract loop with the controls, lamps and ticker; the card readers
// and 10-keys answer as present and empty (cards come in a later step).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cabinet.h"
#include "ezusb_dev.h"

static int usb_log(void) {
    static int on = -1;
    if (on < 0) on = getenv("HK_PC_USB_LOG") != NULL;
    return on;
}

// --- the driver's calls (Cypress' ezusbsys.h: FILE_DEVICE_UNKNOWN, index 0x800 + n)
#define EZ_IOCTL(n, method) ((0x22u << 16) | ((0x800u + (n)) << 2) | (method))
#define IOCTL_GET_DEVICE_DESCRIPTOR EZ_IOCTL(1, 0)
#define IOCTL_VENDOR_REQUEST EZ_IOCTL(5, 0)
#define IOCTL_BULK_READ EZ_IOCTL(19, 2)        // a read on any pipe, interrupt or bulk
#define IOCTL_BULK_WRITE EZ_IOCTL(20, 1)
#define IOCTL_ANCHOR_DOWNLOAD EZ_IOCTL(27, 1)  // the firmware's bytes, to the 8051's memory

enum { PIPE_INT_OUT = 0, PIPE_INT_IN = 1, PIPE_BULK_OUT = 2, PIPE_BULK_IN = 3 };

#define PAGE 62
struct bulk {
    uint8_t node, page, data[PAGE];
};

#pragma pack(push, 1)
struct status_in {         // the interrupt pipe in: what the board reads
    uint32_t pad;          // active low: 9th style's bits (link.h HK_PC_C02_*), 31 the coin node's mode 2,
                           // 4 low on a D01 board, 22 the coin switch (not used: the count below is)
    uint8_t status;        // the last command's answer, given once
    uint8_t coins_hi, coins_lo;   // the coins put in so far (the game's usbCoinGet(0) reads them)
    uint8_t turntable2, turntable1;
    uint8_t seq;
    uint8_t fpga2_check;   // 2, or the FPGA's check fails
    uint8_t fpga_write_ready;
    uint8_t serial_busy;   // bit 0 the H8's answer not read yet, bit 1 its question not sent yet
    uint8_t sliders[3];    // nibbles: 1 and 2, 3 and 4, 5
};
struct command_out {       // the interrupt pipe out: the lamps and a node's command
    uint16_t deck_lamps;   // the keys' (bits 0-13, player 1's first)
    uint8_t node, cmd, arg[2];
    uint8_t panel_lamps;   // the starts, VEFX, Effect
    uint8_t unk0;
    uint8_t top_lamps, top_neon;
    uint8_t fpga_run, unk[5];
};
#pragma pack(pop)

static CRITICAL_SECTION g_lock;
static int g_d01 = 1;           // the board reports a D01 cabinet's FPGA (10th style on); 0: C02's
static HANDLE g_handle;         // the device's handle, one for every open (the library opens and
                                // closes it all the time)
static uint8_t g_status, g_seq, g_node;
static int g_coin_mode2;        // the coin node's mode 2 (the test menu's)

// --- the security plug and the memory its ROM is read back through
static uint8_t g_secmem[PAGE * 5];
static uint8_t g_eeprom[PAGE * 3];
static int g_eeprom_page;
static uint8_t g_plug_seed;
static int g_plug_data;         // 0: the ROM (the PCB id), 1: the data (the signature)
static uint8_t g_pcbid[10];     // header 1, the id's 8 bytes, a check byte
static char g_mcode[9] = "GQE11JAA", g_boot[9] = "GEC02   ";
static unsigned g_seeds[3] = {0, 2, 2};

// The PCB id's check byte (bemanitools' security_id: a CRC-8, 0x8c, of the id's bytes 1-7 in the
// plug's order)
static uint8_t id_check(const uint8_t *id8) {
    const uint8_t b[7] = {id8[1], id8[7], id8[6], id8[5], id8[4], id8[3], id8[2]};
    uint8_t c = 0;
    for (int i = 0; i < 7; i++)
        for (int k = 0; k < 8; k++) {
            const int bit = (c ^ (b[i] >> k)) & 1;
            c >>= 1;
            if (bit) c ^= 0x8c;
        }
    return c;
}

// The plug's data, signed as the game checks it: Konami's Blowfish (its own starting tables, and a
// key from a table XORed with the boot version), over two key bytes and the PCB id; the game's model
// code packed 6 bits a character. The three tables are the game's own, found in its memory by their
// CRC-32s (in the program from RED on, in Konami's ezusb.dll in 10th style): HIRAKU has no copy
struct table {
    const char *name;
    uint32_t crc64, crc, size;   // the first 64 bytes', the whole table's
    const uint8_t *at;
};
static struct table g_tables[3] = {
    {"key", 0x3a76101fu, 0x5628bae3u, 20480, NULL},
    {"P-box", 0x1514fef9u, 0x838a80bcu, 72, NULL},
    {"S-boxes", 0x208e8e0bu, 0x39790c83u, 4096, NULL},
};

static uint32_t crc32(const uint8_t *p, size_t n) {
    static uint32_t t[256];
    if (!t[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
    uint32_t c = ~0u;
    while (n--) c = t[(c ^ *p++) & 0xff] ^ (c >> 8);
    return ~c;
}

static void find_tables_in(HMODULE m) {
    char *base = (char *)m;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    for (WORD s = 0; s < nt->FileHeader.NumberOfSections; s++, sec++) {
        const uint8_t *p = (const uint8_t *)base + sec->VirtualAddress;
        const size_t n = sec->Misc.VirtualSize;
        for (size_t i = 0; i + 64 <= n; i += 4)
            for (int t = 0; t < 3; t++)
                if (!g_tables[t].at && i + g_tables[t].size <= n && crc32(p + i, 64) == g_tables[t].crc64 &&
                    crc32(p + i, g_tables[t].size) == g_tables[t].crc)
                    g_tables[t].at = p + i;
    }
}

static int find_tables(void) {
    if (g_tables[0].at && g_tables[1].at && g_tables[2].at) return 1;
    find_tables_in((HMODULE)hk_program());
    HMODULE ez = GetModuleHandleA("ezusb.dll");   // 10th style's (Konami's)
    if (ez) find_tables_in(ez);
    for (int t = 0; t < 3; t++)
        if (!g_tables[t].at) {
            hk_log("hiraku-pc: board: the security plug's %s table not found in the game: its plug can't be answered", g_tables[t].name);
            return 0;
        }
    return 1;
}

struct bf {
    uint32_t p[18], s[4][256];
};
static uint32_t bf_f(const struct bf *b, uint32_t x) {
    return ((b->s[0][x >> 24] + b->s[1][(x >> 16) & 0xff]) ^ b->s[2][(x >> 8) & 0xff]) + b->s[3][x & 0xff];
}
static void bf_encrypt(const struct bf *b, uint32_t *l, uint32_t *r) {
    uint32_t xl = *l, xr = *r, t;
    for (int i = 0; i < 16; i++) xl ^= b->p[i], xr ^= bf_f(b, xl), t = xl, xl = xr, xr = t;
    t = xl, xl = xr, xr = t;
    *r = xr ^ b->p[16], *l = xl ^ b->p[17];
}
static void bf_init(struct bf *b, const uint8_t *key, int n) {
    memcpy(b->p, g_tables[1].at, sizeof b->p);
    memcpy(b->s, g_tables[2].at, sizeof b->s);
    for (int i = 0, j = 0; i < 18; i++) {
        uint32_t d = 0;
        for (int k = 0; k < 4; k++, j = (j + 1) % n) d = d << 8 | key[j];
        b->p[i] ^= d;
    }
    uint32_t l = 0, r = 0;
    for (int i = 0; i < 18; i += 2) bf_encrypt(b, &l, &r), b->p[i] = l, b->p[i + 1] = r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 256; j += 2) bf_encrypt(b, &l, &r), b->s[i][j] = l, b->s[i][j + 1] = r;
}

// The plug's 12 data bytes: the signature (6) and the model code (6)
static int ezusb_dev_sign(uint8_t out[12]) {
    if (!find_tables()) return 0;
    static uint8_t key[20480];
    size_t boot_len = 0;
    while (boot_len < 8 && g_boot[boot_len] != ' ' && g_boot[boot_len]) boot_len++;
    for (size_t i = 0; i < sizeof key; i++) key[i] = g_tables[0].at[i] ^ (uint8_t)g_boot[i % boot_len];
    const unsigned seed = 16 * (g_seeds[2] + 16 * (g_seeds[1] + 16 * g_seeds[0]));
    if (seed + 16 > sizeof key) return 0;
    uint8_t d[8] = {key[14 + seed], key[15 + seed], g_pcbid[8], g_pcbid[7], g_pcbid[6], g_pcbid[5], g_pcbid[4], g_pcbid[3]};
    static struct bf b;
    bf_init(&b, key + seed, 14);
    uint32_t l, r;
    memcpy(&l, d, 4), memcpy(&r, d + 4, 4);
    bf_encrypt(&b, &l, &r);
    uint8_t e[8];
    memcpy(e, &l, 4), memcpy(e + 4, &r, 4);
    out[0] = e[0] ^ e[6], out[1] = e[1] ^ e[7];
    memcpy(out + 2, e + 2, 4);
    const uint8_t *m = (const uint8_t *)g_mcode;   // 6 bits a character, from the last
#define C(i) ((uint8_t)(m[i] - 0x20))
    out[6] = (uint8_t)(C(7) << 2 | ((C(6) >> 4) & 3));
    out[7] = (uint8_t)(C(6) << 4 | ((C(5) >> 2) & 15));
    out[8] = (uint8_t)(C(5) << 6 | (C(4) & 63));
    out[9] = (uint8_t)(C(3) << 2 | ((C(2) >> 4) & 3));
    out[10] = (uint8_t)(C(2) << 4 | ((C(1) >> 2) & 15));
    out[11] = (uint8_t)(C(1) << 6 | (C(0) & 63));
#undef C
    return 1;
}

// --- the H8: the magnetic card readers (nodes 1 and 2, players 1 and 2) and their 10-keys, behind
// the FPGA's serial port. The game writes a message (0xaa, the message, a sum of its bytes) a page
// at a time and reads the answer back the same way; the answer starts with the question
static uint8_t g_ser_out[512], g_ser_in[512];
static int g_ser_out_n, g_ser_in_n, g_ser_in_page;

static uint8_t sum(const uint8_t *p, int n) {
    uint8_t s = 0;
    while (n-- > 0) s += *p++;
    return s;
}

// The 10-keys: PS/2 set 2 scan codes (the code down, f0 and the code up), as 9th style's
static uint8_t g_keys[2][64];
static int g_nkeys[2];
static uint32_t g_keypad_was[2];
static void keypad_poll(void) {
    static const uint8_t scan[13] = {0x70, 0x69, 0x72, 0x7a, 0x6b, 0x73, 0x74, 0x6c, 0x75, 0x7d, 0x5a, 0x71, 0x66};
    for (int u = 0; u < 2; u++) {
        const uint32_t now = u == 0 ? hk_keypad_now() : hk_keypad2_now(), changed = now ^ g_keypad_was[u];
        g_keypad_was[u] = now;
        for (int k = 0; k < 13; k++) {
            if (!(changed >> k & 1) || g_nkeys[u] + 2 > (int)sizeof g_keys[u]) continue;
            if (!(now >> k & 1)) g_keys[u][g_nkeys[u]++] = 0xf0;
            g_keys[u][g_nkeys[u]++] = scan[k];
        }
    }
}

// An H8 message's answer (its 4-byte head and what follows), its length; -1: none
static int h8_answer(const uint8_t *q, int n, uint8_t *a) {
    if (n < 4) return -1;
    const uint8_t kind = q[0], node = q[1], cmd = q[2];
    const int u = node >= 1 && node <= 2 ? node - 1 : 0;
    a[1] = node, a[2] = cmd, a[3] = 1, a[4] = 0;   // most: a status, 0
    if (kind == 0xaa) {   // to the H8 itself
        a[0] = 0xa5;
        switch (cmd) {
        case 0x01: a[1] = 0, a[4] = 2; return 5;   // the nodes behind it: two readers
        case 0x02:                                  // a node's version (the length says 5; 13 follow)
            a[3] = 5;
            memset(a + 4, 0, 13);
            a[4] = 3, a[9] = 1, a[10] = 6, a[11] = 0;
            memcpy(a + 12, "ICCA", 5);
            return 17;
        case 0x03: return 5;                        // the node's program started
        }
    } else if (kind == 0x00) {   // to a reader
        a[0] = 0x01;
        switch (cmd) {
        case 0x00: case 0x10: case 0x14: case 0x16: case 0x1e: case 0x20: case 0x24:
            return 5;   // the card's and the keys' init, the slot's state set, a card written, statuses
        case 0x12: return 5;   // the slot's sensors: none (no card yet)
        case 0x18:             // a card read: none in the slot
            a[3] = 1, a[4] = 0xff;
            return 5;
        case 0x27:             // the keys waiting: 1 when any (one a read, as bemanitools gives them)
            keypad_poll();
            a[3] = 2, a[4] = g_nkeys[u] ? 1 : 0, a[5] = 0;
            return 6;
        case 0x26:             // a key
            a[3] = n > 4 ? q[4] : 0, a[4] = 0, a[5] = 0;
            if (g_nkeys[u] && a[3]) {
                a[4] = g_keys[u][0];
                memmove(g_keys[u], g_keys[u] + 1, (size_t)--g_nkeys[u]);
            }
            return 6;
        }
    }
    hk_log("hiraku-pc: board: an H8 message not known: %02x %02x %02x", kind, node, cmd);
    return -1;
}

static void serial_exec(void) {
    const uint8_t *q = g_ser_out;
    const int n = g_ser_out_n;
    uint8_t *a = g_ser_in;
    g_ser_in_n = 0, g_ser_in_page = 0;
    // the port's reset: given back as it came
    if ((n > 0 && q[0] == 0x00) || (n == 4 && q[0] == 0xaa)) {
        memcpy(a, q, (size_t)n), g_ser_in_n = n;
        return;
    }
    if (n < 3 || q[0] != 0xaa || q[n - 1] != sum(q + 1, n - 2)) {
        hk_log("hiraku-pc: board: an H8 message not understood (%d bytes)", n);
        return;
    }
    const uint8_t *m = q + 1;
    const int mn = n - 2;
    if (mn == 2 && m[0] == 0xaa && m[1] == 0xaa) {   // the port's reset, again
        a[0] = a[1] = a[2] = 0xaa, a[3] = sum(a + 1, 2), g_ser_in_n = 4;
        return;
    }
    uint8_t r[300];
    const int rn = h8_answer(m, mn, r);
    if (rn < 0) return;
    if (usb_log()) hk_log("usb: H8 %02x %02x %02x -> %d bytes", m[0], m[1], m[2], rn);
    int at = 0;
    if (!(m[0] == 0xaa && m[2] == 0x01)) memcpy(a, q, (size_t)n), at = n;   // the question first (not for the count of nodes)
    a[at] = 0xaa;
    memcpy(a + at + 1, r, (size_t)rn);
    a[at + 1 + rn] = sum(r, rn);
    g_ser_in_n = at + rn + 2;
}

// --- the nodes' commands: what the board answers (the status packet's byte, once)
static uint8_t command(uint8_t node, uint8_t cmd, uint8_t a0) {
    switch (node) {
    case 0x00: return 0;
    case 0x01:   // the security plug: 1 its ROM (a0: where in the security memory its bytes are mixed
                 // with), 2 its data, 3 a write, 4 and 5 the plug's slot
        if (cmd == 1) g_plug_data = 0, g_plug_seed = a0;
        else if (cmd == 2) g_plug_data = 1;
        else if (cmd < 1 || cmd > 5) return 0xfe;
        return 0;
    case 0x02:   // the EEPROM: 2 read, 3 write
        if (cmd == 2) return g_eeprom_page = 0, 0x01;
        return cmd == 3 ? 0x02 : 0xfe;
    case 0x04:   // the serial port to the H8: 2 and 4 the answer to be read, 3 and 5 a question to be written
        if (cmd == 2 || cmd == 4) return g_ser_in_page = 0, 0;
        if (cmd == 3 || cmd == 5) return 0;
        return 0xfe;
    case 0x05: return cmd == 3 ? 0 : 0xfe;   // the ticker's text follows
    case 0x09:   // the coin mode: 1 in play, 2 in the test menu
        if (cmd != 1 && cmd != 2) return 0xfe;
        g_coin_mode2 = cmd == 2;
        return 0;
    case 0x0c: return cmd == 0x3c ? 0 : 0xfe;   // the watchdog
    case 0x10:   // the FPGA: 1 reset, 0xff and 2 checks, 3 its program follows (the arguments its size), 4 done
        switch (cmd) {
        case 0x01: case 0x02: case 0x04: return 0xfe;   // (the firmware's "fine" for these)
        case 0xff: case 0x03: return 0;
        }
        return 0xfe;
    case 0x40: return 0;   // the SRAM: 2 read, 3 write, 4 done
    case 0xfe: return cmd == 0 ? 0x60 : 0xfe;   // the security memory's init
    }
    if (usb_log()) hk_log("usb: a command for a node not known: %02x %02x", node, cmd);
    return 0xfe;
}

static uint8_t g_sram[PAGE * 12];
static int g_sram_page;

// A page written to the last command's node (or the node the page names)
static void bulk_write(const struct bulk *b) {
    switch (b->node) {
    case 0x02:
        if (b->page < 3) memcpy(g_eeprom + b->page * PAGE, b->data, PAGE);
        break;
    case 0x04: {   // the serial stream: full pages, then the last (page 0x42 + its length)
        const int len = b->page >= 0x42 ? b->page - 0x42 : PAGE;
        if (g_ser_out_n + len <= (int)sizeof g_ser_out) memcpy(g_ser_out + g_ser_out_n, b->data, (size_t)len), g_ser_out_n += len;
        if (b->page >= 0x42) serial_exec(), g_ser_out_n = 0;
        break;
    }
    case 0x05: {
        memcpy((void *)hk_link()->ticker, b->data, 9);
        static int n;
        if (n++ < 3 || usb_log()) {
            char text[10];
            memcpy(text, b->data, 9);
            text[9] = 0;
            hk_log("usb: the ticker \"%s\"", text);
        }
        break;
    }
    case 0x40:
        if (b->page < 12) memcpy(g_sram + b->page * PAGE, b->data, PAGE);
        break;
    case 0xfe:
        if (b->page < 5) memcpy(g_secmem + b->page * PAGE, b->data, PAGE);
        break;
    }
}

// A page read from the last command's node
static void bulk_read(struct bulk *b) {
    memset(b, 0, sizeof *b);
    switch (g_node) {
    case 0x01:
        if (!g_plug_data) {   // the ROM: the PCB id, mixed with the security memory
            b->node = 0x11;
            for (int i = 0; i < 10; i++) b->data[i] = g_pcbid[i] ^ g_secmem[(g_plug_seed + i) & 0xff];
        } else {              // the data: signed; the EEPROM keeps the signature, which the game compares
            b->node = 0x12;
            if (ezusb_dev_sign(b->data)) memcpy(g_eeprom + 6, b->data, 6);
        }
        break;
    case 0x02:
        b->node = 0x22, b->page = (uint8_t)g_eeprom_page;
        if (g_eeprom_page < 3) memcpy(b->data, g_eeprom + g_eeprom_page++ * PAGE, PAGE);
        break;
    case 0x04: {   // the H8's answer: full pages, then the last (page 0x40 + its length)
        b->node = 0x42;
        memset(b->data, 0xff, PAGE);
        const int off = g_ser_in_page * PAGE, left = g_ser_in_n - off;
        if (left >= PAGE) {
            memcpy(b->data, g_ser_in + off, PAGE);
            b->page = (uint8_t)g_ser_in_page++;
            if (left == PAGE) g_ser_in_n = off + PAGE;
        } else {
            if (left > 0) memcpy(b->data, g_ser_in + off, (size_t)left);
            b->page = (uint8_t)(0x40 + (left > 0 ? left : 0));
            g_ser_in_n = 0, g_ser_in_page = 0;
        }
        break;
    }
    case 0x40:
        b->node = 0x40, b->page = (uint8_t)g_sram_page;
        if (g_sram_page < 12) memcpy(b->data, g_sram + g_sram_page++ * PAGE, PAGE);
        break;
    }
}

// --- the interrupt pipes
static void status_read(struct status_in *s) {
    memset(s, 0, sizeof *s);
    struct hk_pc_link *l = hk_link();
    uint32_t pad = hk_pad_now() & ~(1u << 31 | 1u << 4);
    // Test held at least 300 ms: RED takes it only once it has been down a while (8 frames weren't
    // enough, 15 were), longer than a quick tap on a key
    static DWORD test_from;
    const DWORD now = GetTickCount();
    if (pad & HK_PC_C02_TEST) {
        if (!test_from) test_from = now ? now : 1;
    } else if (test_from && now - test_from < 300) {
        pad |= HK_PC_C02_TEST;
    } else {
        test_from = 0;
    }
    if (g_coin_mode2) pad |= 1u << 31;
    if (!g_d01) pad |= 1u << 4;   // a C02 cabinet's FPGA (active low: the bit read as 0)
    s->pad = ~pad;
    s->status = g_status, g_status = 0;
    const uint32_t coins = hk_coins_now();
    s->coins_hi = (uint8_t)(coins >> 8), s->coins_lo = (uint8_t)coins;
    s->turntable1 = l->turntable[0], s->turntable2 = l->turntable[1];
    s->sliders[0] = (uint8_t)((l->slider[0] & 15) | (l->slider[1] & 15) << 4);
    s->sliders[1] = (uint8_t)((l->slider[2] & 15) | (l->slider[3] & 15) << 4);
    s->sliders[2] = (uint8_t)(l->slider[4] & 15);
    s->seq = g_seq++;
    s->fpga2_check = 2, s->fpga_write_ready = 1;
}

static void command_write(const struct command_out *c) {
    struct hk_pc_link *l = hk_link();
    l->lamps = c->deck_lamps;
    l->led = (uint8_t)(c->panel_lamps & 15);
    l->top_lamp = c->top_lamps;
    l->top_neon = (uint8_t)(c->top_neon & 1);
    g_node = c->node;
    g_status = command(c->node, c->cmd, c->arg[0]);
    static int n;
    if (usb_log() || (c->node != 0 && n++ < 40)) hk_log("usb: node %02x command %02x (%02x %02x) -> %02x", c->node, c->cmd, c->arg[0], c->arg[1], g_status);
}

// --- the driver's calls, in the game's process
static DWORD ioctl(DWORD code, const void *in, DWORD in_n, void *out, DWORD out_n) {
    switch (code) {
    case IOCTL_GET_DEVICE_DESCRIPTOR:
        if (out_n < 18) return 0;
        memset(out, 0, 18);
        ((uint8_t *)out)[0] = 18, ((uint8_t *)out)[1] = 1;
        ((uint16_t *)out)[4] = 0x0547, ((uint16_t *)out)[5] = 0x2235;   // Cypress' EZ-USB, no EEPROM
        return 18;
    case IOCTL_VENDOR_REQUEST:   // the 8051 held and let go around its firmware: nothing to do
    case IOCTL_ANCHOR_DOWNLOAD:
        return 0;
    case IOCTL_BULK_READ:
    case IOCTL_BULK_WRITE: {
        if (in_n < 4) return 0;
        const uint32_t pipe = *(const uint32_t *)in;
        if (code == IOCTL_BULK_READ && pipe == PIPE_INT_IN && out_n >= sizeof(struct status_in)) {
            status_read((struct status_in *)out);
            return sizeof(struct status_in);
        }
        if (code == IOCTL_BULK_READ && pipe == PIPE_BULK_IN && out_n >= sizeof(struct bulk)) {
            bulk_read((struct bulk *)out);
            return sizeof(struct bulk);
        }
        if (code == IOCTL_BULK_WRITE && pipe == PIPE_INT_OUT && out_n >= sizeof(struct command_out)) {
            command_write((const struct command_out *)out);
            return out_n;
        }
        if (code == IOCTL_BULK_WRITE && pipe == PIPE_BULK_OUT && out_n >= sizeof(struct bulk)) {
            bulk_write((const struct bulk *)out);
            return out_n;
        }
        hk_log("hiraku-pc: board: a transfer not known (pipe %u, %lu bytes)", pipe, (unsigned long)out_n);
        return 0;
    }
    }
    hk_log("hiraku-pc: board: a driver call not known: %08lx", (unsigned long)code);
    return 0;
}

static HANDLE(WINAPI *next_CreateFileA)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);

static HANDLE WINAPI dev_CreateFileA(LPCSTR p, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD cd, DWORD fl, HANDLE t) {
    if (p && !_strnicmp(p, "\\\\.\\Ezusb-", 10)) {
        if (strcmp(p + 10, "0")) {   // one board
            SetLastError(ERROR_FILE_NOT_FOUND);
            return INVALID_HANDLE_VALUE;
        }
        static int n;
        if (n++ < 2 || usb_log()) hk_log("hiraku-pc: board: the game opens %s (HIRAKU's)", p);
        return g_handle;
    }
    return next_CreateFileA(p, acc, share, sa, cd, fl, t);
}

static BOOL WINAPI dev_CloseHandle(HANDLE h) {
    return h == g_handle ? TRUE : CloseHandle(h);
}

static BOOL WINAPI dev_DeviceIoControl(HANDLE h, DWORD code, LPVOID in, DWORD in_n, LPVOID out, DWORD out_n, LPDWORD got,
                                       LPOVERLAPPED ov) {
    if (h != g_handle) return DeviceIoControl(h, code, in, in_n, out, out_n, got, ov);
    // The game's main thread reads the packet the library reads into while the library's thread
    // reads the next (bemanitools' finding): an answer is made apart and copied in whole, or a read
    // between clearing and filling it sees every button held (RED went to its test menu at start-up)
    static uint8_t in_copy[4096], out_copy[4096];
    if (in_n > sizeof in_copy || out_n > sizeof out_copy) {
        hk_log("hiraku-pc: board: a driver call too big (%lu, %lu bytes)", (unsigned long)in_n, (unsigned long)out_n);
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    EnterCriticalSection(&g_lock);
    if (in_n) memcpy(in_copy, in, in_n);
    if (out_n) memcpy(out_copy, out, out_n);
    const DWORD n = ioctl(code, in_copy, in_n, out_copy, out_n);
    if (code == IOCTL_BULK_READ || code == IOCTL_GET_DEVICE_DESCRIPTOR) memcpy(out, out_copy, n);
    LeaveCriticalSection(&g_lock);
    if (got) *got = n;
    if (ov) {   // done at once
        ov->Internal = 0, ov->InternalHigh = n;
        if (ov->hEvent) SetEvent(ov->hEvent);
    }
    return TRUE;
}

static BOOL WINAPI dev_GetOverlappedResult(HANDLE h, LPOVERLAPPED ov, LPDWORD got, BOOL wait) {
    if (h != g_handle) return GetOverlappedResult(h, ov, got, wait);
    if (got) *got = (DWORD)ov->InternalHigh;
    return TRUE;
}

static BOOL WINAPI dev_CancelIo(HANDLE h) {
    return h == g_handle ? TRUE : CancelIo(h);
}

static void *dev_hook(const char *dll, const char *fn, void *next) {
    if (_stricmp(dll, "kernel32.dll")) return NULL;
    if (!strcmp(fn, "CreateFileA")) {
        if (!next_CreateFileA) next_CreateFileA = next ? next : (void *)CreateFileA;
        return (void *)dev_CreateFileA;
    }
    if (!strcmp(fn, "CloseHandle")) return (void *)dev_CloseHandle;
    if (!strcmp(fn, "DeviceIoControl")) return (void *)dev_DeviceIoControl;
    if (!strcmp(fn, "GetOverlappedResult")) return (void *)dev_GetOverlappedResult;
    if (!strcmp(fn, "CancelIo")) return (void *)dev_CancelIo;
    return NULL;
}

void hk_ezusb_dev_start(void) {
    InitializeCriticalSection(&g_lock);
    g_handle = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    memset(g_eeprom, 0xff, sizeof g_eeprom);
    // The game's plug (HK_PC_SECURITY: its model code, the boot's seeds, "GQE11JAA 0:2:2"), the
    // board (HK_PC_BOARD: d01 or c02) and the PCB id (HK_PC_PCBID, 20 hex digits; its check byte
    // made right)
    const char *sec = getenv("HK_PC_SECURITY");
    if (sec && *sec) {
        char code[16] = "";
        unsigned a, b, c;
        if (sscanf(sec, "%15s %u:%u:%u", code, &a, &b, &c) == 4 && strlen(code) <= 8 && a <= 16 && b <= 16 && c >= b && c <= 16) {
            memset(g_mcode, ' ', 8);
            memcpy(g_mcode, code, strlen(code));
            g_seeds[0] = a, g_seeds[1] = b, g_seeds[2] = c;
        } else {
            hk_log("hiraku-pc: board: HK_PC_SECURITY \"%s\" isn't MODEL A:B:C", sec);
        }
    }
    const char *board = getenv("HK_PC_BOARD");
    if (board && !_stricmp(board, "c02")) g_d01 = 0;
    const char *id = getenv("HK_PC_PCBID");
    g_pcbid[0] = 1;
    for (int i = 1; i < 9; i++) g_pcbid[i] = (uint8_t)i;   // bemanitools' 0102030405060708
    if (id && strlen(id) == 20)
        for (int i = 0; i < 10; i++) {
            unsigned v;
            if (sscanf(id + 2 * i, "%2x", &v) == 1) g_pcbid[i] = (uint8_t)v;
        }
    g_pcbid[0] = 1;
    const uint8_t check = id_check(g_pcbid + 1);
    if (g_pcbid[9] != check) {
        if (id && *id) hk_log("hiraku-pc: board: the PCB id's check byte made %02x (was %02x)", check, g_pcbid[9]);
        g_pcbid[9] = check;
    }
    hk_set_device_hook(dev_hook);
    hk_log("hiraku-pc: board: HIRAKU's %s board under the EZ-USB driver, plug %.8s %u:%u:%u", g_d01 ? "D01" : "C02", g_mcode,
           g_seeds[0], g_seeds[1], g_seeds[2]);
}
