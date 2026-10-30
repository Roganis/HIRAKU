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
#include <setupapi.h>
#include <stddef.h>
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
static int g_v2;                // GOLD on: the firmware's later answers (other status codes, the FPGA on
                                // node 4, no H8) and the later plugs: two (the game's black one and
                                // e-AMUSEMENT's white one), each signed with an MD5 (ezusb_dev_sign2)
static int g_plug_white;        // v2: the plug the game picked
static int g_fx2;               // the later board (IO2, Cypress FX2, CyUSB's driver; DJ TROOPERS on): HK_PC_BOARD=fx2
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

// The later plugs' data (GOLD on, bemanitools' security_rp2): a key of 8 characters, the game's
// own for the black plug and e-AMUSEMENT's for the white one, both found in the game's program by
// their CRC-32s (HIRAKU has no copy). The key (each byte ^ 0x40, the black plug's ^ the model code
// too) packed 6 bits a character after the plug's id (its check byte, id bytes 2-7, id byte 1); their
// MD5, its bytes reordered and folded three times into 6: the signature, then the model code packed
static const uint8_t *g_keys2[2];   // black, white
static int find_keys2(void) {
    static const uint32_t crc[2] = {0xb8667837u, 0x6ea5bba2u};
    if (g_keys2[0] && g_keys2[1]) return 1;
    char *base = (char *)hk_program();
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    for (WORD w = 0; w < nt->FileHeader.NumberOfSections; w++, sec++) {   // (every section: some programs mark all executable)
        const uint8_t *p = (const uint8_t *)base + sec->VirtualAddress;
        const size_t n = sec->Misc.VirtualSize;
        for (size_t i = 0; i + 9 <= n; i++) {
            if (p[i] < 0x20 || p[i] > 0x5f || p[i + 8] != 0) continue;
            const uint32_t c = crc32(p + i, 8);
            for (int k = 0; k < 2; k++)
                if (!g_keys2[k] && c == crc[k]) g_keys2[k] = p + i;
        }
    }
    if (!g_keys2[0] || !g_keys2[1]) {
        hk_log("hiraku-pc: board: the security plugs' keys not found in the game: its plugs can't be answered");
        return 0;
    }
    return 1;
}

static void pack6(const uint8_t *in, uint8_t *out) {   // 8 characters, 6 bits each, first first
    uint8_t t[8];
    for (int i = 0; i < 8; i++) t[i] = (uint8_t)((in[i] - 0x20) & 0x3f);
    out[0] = (uint8_t)(t[0] | t[1] << 6), out[1] = (uint8_t)(t[1] >> 2 | t[2] << 4), out[2] = (uint8_t)(t[2] >> 4 | t[3] << 2);
    out[3] = (uint8_t)(t[4] | t[5] << 6), out[4] = (uint8_t)(t[5] >> 2 | t[6] << 4), out[5] = (uint8_t)(t[6] >> 4 | t[7] << 2);
}

typedef struct {
    ULONG i[2], buf[4];
    unsigned char in[64], digest[16];
} md5_ctx;
static int md5(const uint8_t *p, unsigned n, uint8_t out[16]) {   // Windows' own (advapi32)
    static void(WINAPI * init)(md5_ctx *), (WINAPI * update)(md5_ctx *, const unsigned char *, unsigned), (WINAPI * final)(md5_ctx *);
    if (!init) {
        HMODULE a = LoadLibraryA("advapi32.dll");
        init = (void(WINAPI *)(md5_ctx *))(void *)GetProcAddress(a, "MD5Init");
        update = (void(WINAPI *)(md5_ctx *, const unsigned char *, unsigned))(void *)GetProcAddress(a, "MD5Update");
        final = (void(WINAPI *)(md5_ctx *))(void *)GetProcAddress(a, "MD5Final");
        if (!init || !update || !final) return init = NULL, 0;
    }
    md5_ctx c;
    init(&c), update(&c, p, n), final(&c);
    memcpy(out, c.digest, 16);
    return 1;
}

static int ezusb_dev_sign2(int white, uint8_t out[12]) {
    if (!find_keys2()) return 0;
    static const char eamuse_mcode[8] = {'@', '@', '@', '@', '@', '@', '@', '@'};
    const uint8_t *mcode = white ? (const uint8_t *)eamuse_mcode : (const uint8_t *)g_mcode;
    uint8_t key[8], data[14], h[16], b[18];
    for (int i = 0; i < 8; i++) key[i] = (uint8_t)(g_keys2[white][i] ^ 0x40 ^ (white ? 0 : mcode[i]));
    data[0] = g_pcbid[9];                  // the id's check byte, its bytes 2-7, its byte 1
    memcpy(data + 1, g_pcbid + 3, 6);
    data[7] = g_pcbid[2];
    pack6(key, data + 8);
    if (!md5(data, 14, h)) return 0;
    static const uint8_t order[16] = {12, 2, 15, 1, 7, 9, 4, 10, 0, 14, 3, 13, 11, 5, 8, 6};
    for (int i = 0; i < 16; i++) b[i] = h[order[i]];
    b[16] = 0xde, b[17] = 0xad;
    for (int i = 0; i < 6; i++) out[i] = b[i] ^ b[i + 6] ^ b[i + 12];
    pack6(mcode, out + 6);
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

// The 10-keys: a key's PS/2 set 2 scan code as it goes down, queued as it comes (status_read) and
// handed one a read. Nothing as it comes up: 9th style's library takes f0 and the code up as a
// key's release, RED reads every byte as a key (the PIN's two entries came out different)
static uint8_t g_keys[2][64];
static int g_nkeys[2];
static uint32_t g_keypad_was[2];
static void keypad_poll(void) {
    static const uint8_t scan[13] = {0x70, 0x69, 0x72, 0x7a, 0x6b, 0x73, 0x74, 0x6c, 0x75, 0x7d, 0x5a, 0x71, 0x66};
    for (int u = 0; u < 2; u++) {
        const uint32_t now = u == 0 ? hk_keypad_now() : hk_keypad2_now(), down = now & ~g_keypad_was[u];
        g_keypad_was[u] = now;
        for (int k = 0; k < 13; k++)
            if (down >> k & 1 && g_nkeys[u] < (int)sizeof g_keys[u]) g_keys[u][g_nkeys[u]++] = scan[k];
    }
}

// The magnetic cards (as 9th style's, ezusb_c02.c: 128 bytes, a file per player, HK_PC_CARD1 and
// HK_PC_CARD2). A card nobody has written yet is a new e-AMUSEMENT common card (the header 08 20
// 82 01 7c: flags, "@@@" six bits a letter, its CRC-8), which the game takes as new and writes its
// own. The reader's slot (bemanitools' ezusb-iidx-emu node-serial): the game opens it to take a
// card (one held to it goes in), closes it, ejects the card, and asks for a read or a write
static const uint8_t new_card[5] = {0x08, 0x20, 0x82, 0x01, 0x7c};
enum { SLOT_CLOSE, SLOT_OPEN, SLOT_EJECT, SLOT_FORMAT, SLOT_READ, SLOT_WRITE };
static struct {
    int open, inside, offered;
    uint32_t seq_seen;
    uint8_t data[128];
} g_cards[2];

static const char *card_file(int u) { return getenv(u == 0 ? "HK_PC_CARD1" : "HK_PC_CARD2"); }

static void card_load(int u) {
    memset(g_cards[u].data, 0, 128);
    memcpy(g_cards[u].data, new_card, sizeof new_card);
    const char *f = card_file(u);
    FILE *fh = f && *f ? fopen(f, "rb") : NULL;
    if (!fh) return;
    if (fread(g_cards[u].data, 1, 128, fh) != 128) {
        memset(g_cards[u].data, 0, 128);
        memcpy(g_cards[u].data, new_card, sizeof new_card);
    }
    fclose(fh);
}

static void card_save(int u) {
    const char *f = card_file(u);
    FILE *fh = f && *f ? fopen(f, "wb") : NULL;
    if (!fh) return;
    fwrite(g_cards[u].data, 1, 128, fh);
    fclose(fh);
}

// a card offered since the last look goes in while the slot is open
static void card_poll(int u) {
    const uint32_t seq = u == 0 ? hk_card_seq() : hk_card_seq2();
    if (seq != g_cards[u].seq_seen) {
        g_cards[u].seq_seen = seq;
        if (!g_cards[u].inside) g_cards[u].offered = 1, hk_log("hiraku-pc: board: player %d's card held to the slot", u + 1);
    }
    if (g_cards[u].open && g_cards[u].offered && !g_cards[u].inside) {
        card_load(u);
        g_cards[u].inside = 1, g_cards[u].offered = 0;
        hk_log("hiraku-pc: board: player %d's card taken in", u + 1);
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
        case 0x00: case 0x10: case 0x1e: case 0x20: case 0x24:
            return 5;   // the card's and the keys' init, a format done, statuses
        case 0x12:      // the slot's sensors: a card in sets the back (0x80) and front (0x40), and 2 with either
            card_poll(u);
            if (g_cards[u].inside) a[4] = 0xc2;
            return 5;
        case 0x14:      // the slot's state
            switch (n > 4 ? q[4] : -1) {
            case SLOT_OPEN: g_cards[u].open = 1, card_poll(u); break;
            case SLOT_CLOSE: g_cards[u].open = 0; break;
            case SLOT_EJECT:
                if (g_cards[u].inside) hk_log("hiraku-pc: board: player %d's card given back", u + 1);
                g_cards[u].inside = g_cards[u].open = 0;
                break;
            }
            return 5;
        case 0x16:      // a card written: its 128 bytes
            if (g_cards[u].inside && n >= 4 + 128) {
                memcpy(g_cards[u].data, q + 4, 128);
                card_save(u);
                hk_log("hiraku-pc: board: player %d's card written", u + 1);
            }
            return 5;
        case 0x18:      // a card read: 0x48, then its 128 bytes (the length says 24); none: 0xff
            if (!g_cards[u].inside) {
                a[3] = 1, a[4] = 0xff;
                return 5;
            }
            a[3] = 24, a[4] = 0x48;
            memcpy(a + 5, g_cards[u].data, 128);
            return 5 + 128;
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
            if (usb_log()) hk_log("usb: key read (%d asked): %02x, %d left", a[3], a[4], g_nkeys[u]);
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
        if (g_v2) {   // 1 a search and 6 the ROM (a0 as above), 2 the data, 3 a write, 7-11 a slot
            switch (cmd) {
            case 0x01: g_plug_data = 0, g_plug_seed = a0; return 0x12;
            case 0x06: g_plug_data = 0, g_plug_seed = a0; return 0x15;
            case 0x02: g_plug_data = 1; return 0x13;
            case 0x03: return 0x14;
            case 0x07: case 0x08: case 0x0b: g_plug_white = 1; return 0x16;   // the white plug's slots
            case 0x09: case 0x0a: g_plug_white = 0; return 0x16;              // the black plug's
            }
            return 0xfe;
        }
        if (cmd == 1) g_plug_data = 0, g_plug_seed = a0;
        else if (cmd == 2) g_plug_data = 1;
        else if (cmd < 1 || cmd > 5) return 0xfe;
        return 0;
    case 0x02:   // the EEPROM: 2 read, 3 write
        if (cmd == 2) return g_eeprom_page = 0, g_v2 ? 0x21 : 0x01;
        return cmd == 3 ? (g_v2 ? 0x22 : 0x02) : 0xfe;
    case 0x04:   // the serial port to the H8: 2 and 4 the answer to be read, 3 and 5 a question to be written
        if (g_v2) {   // GOLD on, the FPGA: 1 reset, 2 check, 3 its program follows, 4 done
            static const uint8_t ok[5] = {0, 0x41, 0x42, 0x43, 0x43};
            return cmd >= 1 && cmd <= 4 ? ok[cmd] : 0xfe;
        }
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
    case 0xfe: return cmd == 0 ? (g_v2 ? 0x11 : 0x60) : 0xfe;   // the security memory's init
    }
    if (usb_log()) hk_log("usb: a command for a node not known: %02x %02x", node, cmd);
    return 0xfe;
}

static uint8_t g_sram[PAGE * 12];
static int g_sram_page;

// A page written to the last command's node (or the node the page names)
static void bulk_write(const struct bulk *b) {
    if (usb_log()) hk_log("usb: a page written to node %02x, page %02x", b->node, b->page);
    switch (b->node) {
    case 0x02:
        if (b->page < 3) memcpy(g_eeprom + b->page * PAGE, b->data, PAGE);
        break;
    case 0x04: {   // the serial stream: full pages, then the last (page 0x42 + its length)
        if (g_v2) break;   // (GOLD on: the FPGA's program)
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
    if (usb_log()) hk_log("usb: a page read from node %02x", g_node);
    memset(b, 0, sizeof *b);
    switch (g_node) {
    case 0x01:
        if (!g_plug_data) {   // the ROM: the PCB id, mixed with the security memory
            b->node = 0x11;
            for (int i = 0; i < 10; i++) b->data[i] = g_pcbid[i] ^ g_secmem[(g_plug_seed + i) & 0xff];
        } else if (g_v2) {   // the data of the plug picked, signed
            b->node = 0x12;
            ezusb_dev_sign2(g_plug_white, b->data);
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
    // the 10-keys looked at each read (the game asks the H8 for keys only every few frames: a short
    // press between two asks was lost)
    keypad_poll();
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
    if (g_d01) pad |= 1u << 4;   // a D01 board's pin (active low: read as 0; 10th style takes the D01 plug's path by it)
    s->pad = ~pad;
    s->status = g_status, g_status = 0;
    const uint32_t coins = hk_coins_now();
    s->coins_hi = (uint8_t)(coins >> 8), s->coins_lo = (uint8_t)coins;
    s->turntable1 = l->turntable[0], s->turntable2 = l->turntable[1];
    hk_turntable_seen(0, l->turntable[0]), hk_turntable_seen(1, l->turntable[1]);
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
    if (usb_log() && code != IOCTL_BULK_READ && code != IOCTL_BULK_WRITE) {
        char hex[64] = "";
        for (DWORD i = 0; i < in_n && i < 16; i++) snprintf(hex + 2 * i, 3, "%02x", ((const uint8_t *)in)[i]);
        hk_log("usb: driver call %08lx, %lu bytes in (%s), %lu out", (unsigned long)code, (unsigned long)in_n, hex, (unsigned long)out_n);
    }
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

// --- the later board (IO2: a Cypress FX2 under CyUSB's driver, bemanitools' ezusb2-emu and
// ezusb2-iidx-emu read again): the same nodes and their later answers (g_v2), other packets. The
// driver's calls carry a SINGLE_TRANSFER (38 bytes) before their data, and the answer starts with
// it again: control transfers on endpoint 0 (the descriptors; the 8051 held in reset while its
// firmware is loaded), the rest on endpoints 0x01 (the interrupt out: a node's command, lamps,
// the ticker's 9 characters), 0x02 (bulk out: a page), 0x81 (interrupt in: the pad, the
// turntables, the sliders, the last command's answer) and 0x86 (bulk in). 64 bytes each way, the
// bulk ones a node, a page and 62 bytes as before
#define CY_EP0 0x220020u    // IOCTL_ADAPT_SEND_EP0_CONTROL_TRANSFER
#define CY_EPX 0x220024u    // IOCTL_ADAPT_SEND_NON_EP0_TRANSFER
#define CY_HEAD 38
static uint8_t g_fw[0x10000];   // the firmware the game loads: the security memory's first contents are in it
static int g_fw_secmem;

// The board's security memory comes filled by its firmware (the game writes only some of it): the
// 256 bytes found in the firmware the game loaded, by their CRC-32 (HIRAKU has no copy)
static void fx2_secmem_from_firmware(void) {
    if (g_fw_secmem) return;
    g_fw_secmem = 1;
    const char *dump = getenv("HK_PC_FW_DUMP");   // (bring-up: the firmware as loaded, to a file)
    if (dump && *dump) {
        FILE *f = fopen(dump, "wb");
        if (f) fwrite(g_fw, 1, sizeof g_fw, f), fclose(f);
    }
    for (size_t i = 0; i + 256 <= sizeof g_fw; i++)
        if (crc32(g_fw + i, 64) == 0x7ab1cbafu && crc32(g_fw + i, 256) == 0x819b83bcu) {
            memcpy(g_secmem, g_fw + i, 256);
            hk_log("hiraku-pc: board: the security memory's contents found in the board's firmware");
            return;
        }
    hk_log("hiraku-pc: board: the security memory's contents not found in the board's firmware");
}

static void fx2_status(uint8_t *p) {   // the interrupt pipe in, 64 bytes
    memset(p, 0, 64);
    keypad_poll();
    struct hk_pc_link *l = hk_link();
    const uint32_t pad = hk_pad_now();
    const uint32_t coins = hk_coins_now();   // the coins put in so far, as the C02 board counts them
    uint32_t bits = (pad >> 8 & 0x3fff) << 16;   // the keys: player 1's 1-7, then player 2's
    if (pad & HK_PC_C02_START(1)) bits |= 1u << 0;
    if (pad & HK_PC_C02_START(2)) bits |= 1u << 1;
    if (pad & HK_PC_C02_VEFX) bits |= 1u << 2;
    if (pad & HK_PC_C02_EFFECT) bits |= 1u << 3;
    if (pad & HK_PC_C02_TEST) bits |= 1u << 4;
    if (pad & HK_PC_C02_SERVICE) bits |= 1u << 5;
    const uint32_t inverted = ~bits;
    p[3] = g_seq++;
    p[4] = g_status, g_status = 0;
    p[5] = (uint8_t)(coins >> 8), p[6] = (uint8_t)coins;   // (DJ TROOPERS counts these, not the coin switch's bit 30)
    memcpy(p + 8, &inverted, 4);
    p[13] = l->turntable[1], p[14] = l->turntable[0];
    hk_turntable_seen(0, l->turntable[0]), hk_turntable_seen(1, l->turntable[1]);
    p[15] = (uint8_t)((l->slider[0] & 15) | (l->slider[1] & 15) << 4);
    p[16] = (uint8_t)((l->slider[2] & 15) | (l->slider[3] & 15) << 4);
    p[17] = (uint8_t)(l->slider[4] & 15);
}

static void fx2_command(const uint8_t *p) {   // the interrupt pipe out, 64 bytes
    struct hk_pc_link *l = hk_link();
    l->led = (uint8_t)(p[8] & 15);
    l->lamps = (uint16_t)(p[11] | p[12] << 8);
    l->top_lamp = p[14];
    l->top_neon = (uint8_t)(p[15] & 1);
    memcpy((void *)l->ticker, p + 16, 9);
    g_node = p[2];
    g_status = command(p[2], p[3], p[4]);
    // (debug HK_PC_FX2_LOG: the packet's other bytes, each change)
    static int fx2_log = -1;
    static uint8_t was[64];
    if (fx2_log < 0) fx2_log = getenv("HK_PC_FX2_LOG") != NULL;
    if (fx2_log) {
        char b[256];
        int k = 0;
        for (int i = 0; i < 64; i++) {
            if ((i >= 16 && i < 25) || p[i] == was[i]) continue;   // (all but the ticker's text)
            k += snprintf(b + k, sizeof b - k, " %d:%02x", i, p[i]);
            if (k > 200) break;
        }
        if (k) hk_log("fx2 out:%s", b);
        memcpy(was, p, 64);
    }
    static int n;
    if (usb_log() || (p[2] != 0 && n++ < 40)) hk_log("usb: node %02x command %02x (%02x %02x) -> %02x", p[2], p[3], p[4], p[5], g_status);
}

static DWORD fx2_ioctl(DWORD code, const uint8_t *in, DWORD in_n, uint8_t *out, DWORD out_n) {
    if (usb_log() && (code == CY_EP0 || (in_n > 13 && in[13] != 0x81 && in[13] != 0x01)))
        hk_log("usb: CyUSB call %08lx, endpoint %02x, %lu bytes in, %lu out", (unsigned long)code, in_n > 13 ? in[13] : 0,
               (unsigned long)in_n, (unsigned long)out_n);
    if (in_n < CY_HEAD || out_n < CY_HEAD) return 0;
    memcpy(out, in, CY_HEAD);
    const uint8_t *data = in + CY_HEAD;
    const DWORD data_n = in_n - CY_HEAD;
    uint8_t *back = out + CY_HEAD;
    const DWORD room = out_n - CY_HEAD;
    if (code == CY_EP0) {
        const uint8_t type = in[0], req = in[1];
        const uint16_t value = (uint16_t)(in[2] | in[3] << 8);
        if (type == 0x80 && req == 6 && value == 0x0100 && room >= 18) {   // the device's descriptor: vendor and product
            memset(back, 0, 18);
            back[0] = 18, back[1] = 1;
            back[8] = 0xb4, back[9] = 0x04, back[10] = 0x13, back[11] = 0x86;   // 04b4:8613, the FX2
            return CY_HEAD + 18;
        }
        if (type == 0x80 && req == 6 && value == 0x0301 && room >= 20) {   // its maker
            static const char maker[] = "KONAMI";
            memset(back, 0, 20);
            back[0] = 20, back[1] = 3;
            for (int i = 0; i < 6; i++) back[2 + 2 * i] = (uint8_t)maker[i];
            return CY_HEAD + 20;
        }
        if (type == 0x40 && req == 0xa0) {   // the 8051 held (CPUCS 0xe600) and let go, its firmware between
            if (value == 0xe600) {
                if (data_n && data[0] == 0) fx2_secmem_from_firmware();
            } else if ((size_t)value + data_n <= sizeof g_fw) {
                memcpy(g_fw + value, data, data_n);
            }
            return CY_HEAD;
        }
        hk_log("hiraku-pc: board: a control transfer not known: %02x %02x %04x", type, req, value);
        return CY_HEAD;
    }
    switch (in[13]) {   // the endpoint
    case 0x01:
        if (data_n >= 24) fx2_command(data);
        return CY_HEAD;
    case 0x02:   // a node and a page, then up to 510 bytes: the security memory's whole in one
        if (data_n >= 2 && data[0] == 0xfe) {
            const DWORD n = data_n - 2 < sizeof g_secmem ? data_n - 2 : (DWORD)sizeof g_secmem;
            memcpy(g_secmem, data + 2, n);
            if (usb_log()) hk_log("usb: the security memory written, %lu bytes", (unsigned long)n);
        } else if (data_n >= sizeof(struct bulk)) {
            bulk_write((const struct bulk *)data);
        }
        return CY_HEAD;
    case 0x81:
        if (room < 64) return CY_HEAD;
        fx2_status(back);
        return CY_HEAD + 64;
    case 0x86:
        if (room < sizeof(struct bulk)) return CY_HEAD;
        bulk_read((struct bulk *)back);
        return CY_HEAD + sizeof(struct bulk);
    }
    hk_log("hiraku-pc: board: a transfer on endpoint %02x not known", in[13]);
    return CY_HEAD;
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
    const int fx2 = g_fx2 && (code == CY_EP0 || code == CY_EPX);
    const DWORD n = fx2 ? fx2_ioctl(code, in_copy, in_n, out_copy, out_n) : ioctl(code, in_copy, in_n, out_copy, out_n);
    if (fx2 || code == IOCTL_BULK_READ || code == IOCTL_GET_DEVICE_DESCRIPTOR) memcpy(out, out_copy, n);
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

// The board among the machine's USB devices (IIDX GOLD on): the program lists them (SetupAPI, the
// "USB" enumerator) and takes the C02 cabinet's board by its description ("Cypress EZ-USB (2235)",
// then the driver as before) or the later FX2 board's ("... FX2LP"). HIRAKU's is the first: one
// device, its description, and the driver's name for it
static int g_devs;   // the list's handle
#define DEV_DESC_C02 "Cypress EZ-USB (2235) - EEPROM missing"
#define DEV_DESC_FX2 "Cypress EZ-USB FX2LP - EEPROM missing"
#define DEV_DESC (g_fx2 ? DEV_DESC_FX2 : DEV_DESC_C02)

// CyUSB's device interface, by which the FX2 board's driver is then found
static const GUID g_cyusb = {0xae18aa60, 0x7f6a, 0x11d4, {0x97, 0xdd, 0x00, 0x01, 0x02, 0x29, 0xb9, 0x59}};

static HDEVINFO WINAPI dev_SetupDiGetClassDevsA(const GUID *cls, PCSTR enumerator, HWND w, DWORD flags) {
    if (g_fx2 && cls && IsEqualGUID(cls, &g_cyusb)) {
        hk_log("hiraku-pc: board: the game asks for CyUSB's devices: HIRAKU's board");
        SetLastError(ERROR_SUCCESS);
        return (HDEVINFO)&g_devs;
    }
    if (enumerator && !_stricmp(enumerator, "USB")) {
        hk_log("hiraku-pc: board: the game lists the USB devices: HIRAKU's board, \"%s\"", DEV_DESC);
        SetLastError(ERROR_SUCCESS);
        return (HDEVINFO)&g_devs;
    }
    return SetupDiGetClassDevsA(cls, enumerator, w, flags);
}

static BOOL WINAPI dev_SetupDiEnumDeviceInfo(HDEVINFO l, DWORD i, PSP_DEVINFO_DATA d) {
    if (l != (HDEVINFO)&g_devs) return SetupDiEnumDeviceInfo(l, i, d);
    SetLastError(i == 0 ? ERROR_SUCCESS : ERROR_NO_MORE_ITEMS);
    return i == 0;
}

static BOOL WINAPI dev_SetupDiEnumDeviceInterfaces(HDEVINFO l, PSP_DEVINFO_DATA d, const GUID *g, DWORD i,
                                                   PSP_DEVICE_INTERFACE_DATA f) {
    if (l != (HDEVINFO)&g_devs) return SetupDiEnumDeviceInterfaces(l, d, g, i, f);
    SetLastError(i == 0 ? ERROR_SUCCESS : ERROR_NO_MORE_ITEMS);
    return i == 0;
}

static BOOL WINAPI dev_SetupDiGetDeviceRegistryPropertyA(HDEVINFO l, PSP_DEVINFO_DATA d, DWORD prop, PDWORD type, PBYTE out,
                                                         DWORD n, PDWORD need) {
    if (l != (HDEVINFO)&g_devs) return SetupDiGetDeviceRegistryPropertyA(l, d, prop, type, out, n, need);
    if (prop != SPDRP_DEVICEDESC) {
        SetLastError(ERROR_INVALID_DATA);
        return FALSE;
    }
    const DWORD size = (DWORD)strlen(DEV_DESC) + 1;
    if (need) *need = size;
    if (!out || n < size) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    if (type) *type = REG_SZ;
    memcpy(out, DEV_DESC, size);
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

static BOOL WINAPI dev_SetupDiGetDeviceInterfaceDetailA(HDEVINFO l, PSP_DEVICE_INTERFACE_DATA f,
                                                        PSP_DEVICE_INTERFACE_DETAIL_DATA_A out, DWORD n, PDWORD need,
                                                        PSP_DEVINFO_DATA d) {
    if (l != (HDEVINFO)&g_devs) return SetupDiGetDeviceInterfaceDetailA(l, f, out, n, need, d);
    static const char path[] = "\\\\.\\Ezusb-0";
    const DWORD size = (DWORD)(offsetof(SP_DEVICE_INTERFACE_DETAIL_DATA_A, DevicePath) + sizeof path);
    if (need) *need = size;
    if (!out || n < size) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    memcpy(out->DevicePath, path, sizeof path);
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

static BOOL WINAPI dev_SetupDiDestroyDeviceInfoList(HDEVINFO l) {
    return l == (HDEVINFO)&g_devs ? TRUE : SetupDiDestroyDeviceInfoList(l);
}

static void *dev_hook(const char *dll, const char *fn, void *next) {
    if (!_stricmp(dll, "setupapi.dll")) {
        static const struct { const char *fn; void *to; } setupapi[] = {
            {"SetupDiGetClassDevsA", (void *)dev_SetupDiGetClassDevsA},
            {"SetupDiEnumDeviceInfo", (void *)dev_SetupDiEnumDeviceInfo},
            {"SetupDiEnumDeviceInterfaces", (void *)dev_SetupDiEnumDeviceInterfaces},
            {"SetupDiGetDeviceRegistryPropertyA", (void *)dev_SetupDiGetDeviceRegistryPropertyA},
            {"SetupDiGetDeviceInterfaceDetailA", (void *)dev_SetupDiGetDeviceInterfaceDetailA},
            {"SetupDiDestroyDeviceInfoList", (void *)dev_SetupDiDestroyDeviceInfoList},
        };
        for (size_t i = 0; i < sizeof setupapi / sizeof *setupapi; i++)
            if (!strcmp(fn, setupapi[i].fn)) return setupapi[i].to;
        return NULL;
    }
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
        const int got = sscanf(sec, "%15s %u:%u:%u", code, &a, &b, &c);
        if (got == 4 && strlen(code) <= 8 && a <= 16 && b <= 16 && c >= b && c <= 16) {
            memset(g_mcode, ' ', 8);
            memcpy(g_mcode, code, strlen(code));
            g_seeds[0] = a, g_seeds[1] = b, g_seeds[2] = c;
        } else if (got == 1 && strlen(code) == 8) {   // GOLD on: the later plugs, no seeds
            memcpy(g_mcode, code, 8);
            g_v2 = 1;
        } else {
            hk_log("hiraku-pc: board: HK_PC_SECURITY \"%s\" isn't MODEL A:B:C", sec);
        }
    }
    const char *board = getenv("HK_PC_BOARD");
    if (board && !_stricmp(board, "c02")) g_d01 = 0;
    if (board && !_stricmp(board, "fx2")) g_fx2 = 1, g_v2 = 1;
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
    if (g_v2)
        hk_log("hiraku-pc: board: HIRAKU's %s board under the %s driver, plugs %.8s and e-AMUSEMENT's", g_fx2 ? "FX2" : g_d01 ? "D01" : "C02",
               g_fx2 ? "CyUSB" : "EZ-USB", g_mcode);
    else
        hk_log("hiraku-pc: board: HIRAKU's %s board under the EZ-USB driver, plug %.8s %u:%u:%u", g_d01 ? "D01" : "C02", g_mcode,
               g_seeds[0], g_seeds[1], g_seeds[2]);
}
