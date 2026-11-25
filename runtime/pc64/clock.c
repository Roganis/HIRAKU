// SPDX-License-Identifier: GPL-3.0-or-later
// A 64-bit PC IIDX's clock record, and a first boot that needs no operator (chosen by the game
// DLL's name in main.c, so the first pc64 game is untouched).
//
// What the game's own code does (read from the game module's code, confirmed at run time only
// where the log says so): the start-up check reads one byte of its clock settings object through
// the getter `movzx eax,[rcx+8]; ret`, reached from the start-up screen flow as
// `add rcx,<imm32>; call <getter>` on the I/O singleton; 0 -> CLOCK ERROR 5-1506-0001.
// The operator's CLOCK SETTING writes 1 through the setter `mov byte [rcx+8], dl; ret` (the
// class vtable's first slot is `xor edx,edx; jmp <setter>`, so the slot clears the flag) and then
// saves the backup through the game's own saver. The flag is loaded from the backup
// (/dev/nvram/cabinet.xml) at boot, so a backup the game has just created has it 0.
//
// So, once per run, on a backup the game has just created: this file calls the game's OWN
// setter and saver with the flag 1 — found by byte shape in the game's module, and refused
// (logged, not called) unless every shape has exactly the expected number of matches and the
// bytes at each found address are the expected ones:
//   - the vtable slot + setter: `33 d2 e9 <rel32>` whose target is `88 51 08 c3`
//   - the start-up read site: `48 81 c1 <imm32> e8 <rel32>` whose target is `0f b6 41 08 c3`
//     (the imm32 is the clock object's offset in the I/O object, the 08 the flag's offset in it)
//   - the backup saver: its 65-byte prologue, the stack-cookie pointer's displacement masked out
//   - the I/O object: the one singleton-getter candidate (init-once prologue
//     `push rbx; sub rsp,0x30; mov [rsp+0x20],-2; ...`) whose RIP-relative lea points at a static
//     holding an object whose +<imm32> qword is that vtable
// The PC's clock itself reaches the AVS2 library through its own time module, so no time is
// written here; the operator's explicit time entry in the menu (an avs2-core call) is not
// replicated.
//
// First boot without an operator: when dev\nvram\cabinet.xml did not exist when the run began and
// the game's failed open of it is seen (the file hooks chain through here), TEST is reported held
// from then until the game's write-open of cabinet.xml is seen, then never again in that run —
// the board stand-in (board_iidx.c) reads it via pc64_clock_test_held(). No frame-number timing:
// the file events drive it. (A guard timeout releases the hold if no write ever comes.)
#include "pc64.h"

#include <stdlib.h>
#include <string.h>

// --- scanning the game module's executable sections: fn(base, code, size) per exec section
typedef void (*scan_fn)(char *base, uint8_t *code, size_t n);
static void scan_exec(HMODULE m, scan_fn fn) {
    char *base = (char *)m;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    for (WORD s = 0; s < nt->FileHeader.NumberOfSections; s++, sec++)
        if (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)
            fn(base, (uint8_t *)base + sec->VirtualAddress, sec->Misc.VirtualSize);
}

static void *g_game;
static void *g_vt0;       // the clock class's vtable (first slot = the clear-the-flag thunk)
static void *g_setter;    // mov byte [rcx+<flag off>], dl; ret
static void *g_saver;     // the backup saver (takes the I/O object)
static void *g_io;        // the I/O singleton
static uint8_t *g_clock;  // the clock settings object (I/O + <clock off>)
static unsigned g_flag_off;
static int g_found, g_ok;
static size_t g_image_size;

// --- the shop name, on the same first-boot path: the start-up's SHOP NAME ERROR (5-1506-0001)
// tests the string at (I/O + <shop off>)+<str off> through a decider the boot box calls once;
// the game's own setter — the one the getname answer's handler and the TEST menu's SHOP NAME
// SETTING call — copies a name there, then the backup saver persists it. Found by shape like
// the clock routines (each pattern must match exactly the expected number of times):
//   - the setter's body: test rdx,rdx; je; mov r8,rdx; add rcx,<str off>; mov edx,<max>; jmp <copy>
//   - the object's offset: every `lea rcx,[rax+imm32]; call <setter>` must carry the same imm32
//   - the decider: the one `lea rcx,[rbx+shop_off]; call X` whose body has the byte-compare
//     `movzx ecx,[rax]; cmp cl,[rip]` (the empty-name test) in its first 0x40 bytes
// The name used is the one the board's AMI ROM reports (the lid the game itself sends); the
// server's getname answer replaces it on the next start if it differs. Only the game's own
// routines are called; nothing is written into its memory by hand.
static int rd32(const uint8_t *p);
static void *g_shop_setter, *g_shop_decider;
static unsigned g_shop_off, g_shop_str, g_shop_max;
static int g_shop_ok;
static const uint8_t SHOP_SETTER_PAT[] = {
    0x48, 0x85, 0xd2, 0x74, 0x11, 0x4c, 0x8b, 0xc2, 0x48, 0x83, 0xc1, 0x0c, 0xba, 0x19, 0x00, 0x00, 0x00, 0xe9 };
static unsigned sh_hits, sh_off;
static void *sh_a, *sh_dec;
static void scan_shop_setter(char *base, uint8_t *code, size_t n) {
    (void)base;
    for (size_t i = 0; i + sizeof SHOP_SETTER_PAT <= n; i++)
        if (!memcmp(code + i, SHOP_SETTER_PAT, sizeof SHOP_SETTER_PAT)) { sh_hits++; sh_a = code + i; }
}
static void scan_shop_off(char *base, uint8_t *code, size_t n) {   // lea rcx,[rax+imm32]; call <setter>
    (void)base;
    for (size_t i = 0; i + 12 <= n; i++)
        if (code[i] == 0x48 && code[i + 1] == 0x8d && code[i + 2] == 0x88 && code[i + 7] == 0xe8) {
            long t = (long)i + 12 + rd32(code + i + 8);
            if (t >= 0 && (size_t)t < n && code + t == g_shop_setter) {
                unsigned imm = (unsigned)(uint32_t)rd32(code + i + 3);
                sh_hits++;
                if (!sh_off) sh_off = imm;
                else if (sh_off != imm) sh_off = 0xffffffffu;
            }
        }
}
static void scan_shop_decider(char *base, uint8_t *code, size_t n) {   // lea rcx,[rbx+shop_off]; call <decider>
    (void)base;
    for (size_t i = 0; i + 12 <= n; i++)
        if (code[i] == 0x48 && code[i + 1] == 0x8d && code[i + 2] == 0x8b && code[i + 7] == 0xe8 &&
            (unsigned)(uint32_t)rd32(code + i + 3) == g_shop_off) {
            long t = (long)i + 12 + rd32(code + i + 8);
            if (t < 0 || (size_t)t + 0x40 > n) continue;
            for (int k = 0; k + 5 <= 0x40; k++)
                if (!memcmp(code + t + k, "\x0f\xb6\x08\x3a\x0d", 5)) { sh_hits++; sh_dec = code + t; break; }
        }
}

static int rd32(const uint8_t *p) { int v; memcpy(&v, p, 4); return v; }

// scan state (find_routines runs once, single-threaded from the board's poll)
static unsigned sc_hits;
static void *sc_a, *sc_b;
static unsigned sc_off;

// the vtable slot + setter: 33 d2 e9 <rel32> -> 88 51 08 c3
static void scan_vt0(char *base, uint8_t *code, size_t n) {
    (void)base;
    for (size_t i = 0; i + 8 <= n; i++) {
        if (code[i] == 0x33 && code[i + 1] == 0xd2 && code[i + 2] == 0xe9) {
            long t = (long)i + 7 + rd32(code + i + 3);
            if (t >= 0 && (size_t)t + 4 <= n && !memcmp(code + t, "\x88\x51\x08\xc3", 4)) {
                sc_hits++;
                sc_a = code + i;      // the vtable's first slot
                sc_b = code + t;      // the setter
            }
        }
    }
}
// the start-up read site: 48 81 c1 <imm32> e8 <rel32> -> 0f b6 41 08 c3
static void scan_site(char *base, uint8_t *code, size_t n) {
    (void)base;
    for (size_t i = 0; i + 12 <= n; i++) {
        if (code[i] == 0x48 && code[i + 1] == 0x81 && code[i + 2] == 0xc1 && code[i + 7] == 0xe8) {
            long t = (long)i + 12 + rd32(code + i + 8);
            if (t >= 0 && (size_t)t + 5 <= n && !memcmp(code + t, "\x0f\xb6\x41\x08\xc3", 5)) {
                sc_hits++;
                sc_off = (unsigned)(uint32_t)rd32(code + i + 3);   // the clock object's offset in the I/O object
                sc_b = code + t;                                    // the getter
            }
        }
    }
}
// the backup saver: 65-byte prologue, the cookie pointer's displacement (offset 24..27) masked
static const uint8_t SAVER_PAT[] = {
    0x48, 0x89, 0x5c, 0x24, 0x10, 0x55, 0x48, 0x8d, 0xac, 0x24, 0xf0, 0xfd, 0xff, 0xff,
    0x48, 0x81, 0xec, 0x10, 0x03, 0x00, 0x00, 0x48, 0x8b, 0x05, 0, 0, 0, 0,
    0x48, 0x33, 0xc4, 0x48, 0x89, 0x85, 0x00, 0x02, 0x00, 0x00, 0x33, 0xc0, 0x48, 0x8b,
    0xd9, 0x48, 0x8d, 0x8d, 0x08, 0x01, 0x00, 0x00, 0x66, 0x89, 0x45, 0x75, 0x33, 0xd2,
    0x88, 0x45, 0x77, 0x41, 0xb8, 0xb0, 0x00, 0x00, 0x00 };
static void scan_saver(char *base, uint8_t *code, size_t n) {
    (void)base;
    for (size_t i = 0; i + sizeof SAVER_PAT <= n; i++) {
        size_t k = 0;
        while (k < sizeof SAVER_PAT) {
            if (k >= 24 && k < 28) { k++; continue; }
            if (code[i + k] != SAVER_PAT[k]) break;
            k++;
        }
        if (k == sizeof SAVER_PAT) { sc_hits++; sc_a = code + i; }
    }
}
// the singleton-getter candidates: init-once prologue, the guard's displacement (offset 17..20) masked
static const uint8_t PRO_PAT[] = {
    0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x48, 0xc7, 0x44, 0x24, 0x20, 0xfe, 0xff, 0xff,
    0xff, 0x8b, 0x05, 0, 0, 0, 0, 0xa8, 0x01, 0x75 };
static unsigned sc_cand, sc_io_hits;
// the clock object's vtable pointer's first entry must be the found slot code (33 d2 e9 -> 88 51 08 c3)
static int clock_vt_matches(uint8_t *clock) {
    if (IsBadReadPtr(clock, 8)) return 0;
    uint64_t v;
    memcpy(&v, clock, 8);                       // the object's vtable
    if (v < 0x10000 || (v & 7)) return 0;
    if (IsBadReadPtr((void *)(uintptr_t)v, 8)) return 0;
    uint64_t e0;
    memcpy(&e0, (void *)(uintptr_t)v, 8);       // vtable[0], the clear-the-flag thunk's address
    return e0 == (uint64_t)(uintptr_t)g_vt0;
}
static void scan_singleton(char *base, uint8_t *code, size_t n) {
    (void)base;
    for (size_t i = 0; i + sizeof PRO_PAT <= n; i++) {
        size_t k = 0;
        while (k < sizeof PRO_PAT) {
            if (k >= 17 && k < 21) { k++; continue; }
            if (code[i + k] != PRO_PAT[k]) break;
            k++;
        }
        if (k != sizeof PRO_PAT) continue;
        sc_cand++;
        uint8_t *fn = code + i;
        size_t lim = n - i < 0x60 ? n - i : 0x60;
        for (size_t j = 0; j + 7 <= lim; j++) {
            if (fn[j] != 0x48 || fn[j + 1] != 0x8d) continue;
            uint8_t modrm = fn[j + 2];
            if ((modrm >> 6) != 0 || (modrm & 7) != 5) continue;
            uint8_t *slot = fn + j + 7 + rd32(fn + j + 3);   // lea reg,[rip+disp32] target
            // (a) the target is the static object itself
            if (slot >= (uint8_t *)g_game && slot < (uint8_t *)g_game + g_image_size && ((slot - (uint8_t *)g_game) & 7) == 0) {
                uint8_t *clock = slot + sc_off;   // in the image, readable
                if (clock_vt_matches(clock)) {
                    if (g_io && slot == (uint8_t *)g_io) continue;   // same static seen twice in one getter
                    sc_io_hits++;
                    if (!g_io) { g_io = slot; g_clock = clock; }
                }
            }
            // (b) the target holds a pointer to the object (heap singleton)
            if (IsBadReadPtr(slot, 8)) continue;
            uint64_t ioq;
            memcpy(&ioq, slot, 8);
            if (ioq < 0x10000 || (ioq & 7)) continue;   // not a plausible object pointer
            uint8_t *clock = (uint8_t *)(uintptr_t)(ioq + sc_off);
            if (clock_vt_matches(clock)) {
                if (g_io && ioq == (uint64_t)(uintptr_t)g_io) continue;
                sc_io_hits++;
                if (!g_io) { g_io = (void *)(uintptr_t)ioq; g_clock = clock; }
            }
        }
    }
}

static void find_routines(HMODULE game) {
    sc_hits = 0; sc_a = sc_b = NULL; sc_off = 0;
    scan_exec(game, scan_vt0);
    pc64_log("clock: the clock vtable slot + setter: %u matches", sc_hits);
    if (sc_hits != 1) return;
    g_vt0 = sc_a; g_setter = sc_b;
    sc_hits = 0; sc_b = NULL;
    scan_exec(game, scan_site);
    pc64_log("clock: the start-up read site (clock object at +0x%x in the I/O object): %u matches", sc_off, sc_hits);
    if (sc_hits != 1 || !sc_b) return;
    g_flag_off = ((uint8_t *)sc_b)[3];
    sc_hits = 0; sc_a = NULL;
    scan_exec(game, scan_saver);
    pc64_log("clock: the backup saver: %u matches", sc_hits);
    if (sc_hits != 1) return;
    g_saver = sc_a;
    sc_cand = 0; sc_io_hits = 0; g_io = NULL; g_clock = NULL;
    scan_exec(game, scan_singleton);
    pc64_log("clock: the singleton-getter candidates: %u, the I/O object matches: %u", sc_cand, sc_io_hits);
    if (sc_io_hits != 1 || !g_io) { g_io = NULL; return; }
    g_ok = 1;
    pc64_log("clock: the game's own routines found (vtable %p, setter %p, saver %p, I/O %p, clock %p, flag +0x%x)",
             g_vt0, g_setter, g_saver, g_io, (void *)g_clock, g_flag_off);
    // the shop name's routines, same shapes idea
    sh_hits = 0; sh_a = NULL;
    scan_exec(game, scan_shop_setter);
    pc64_log("shop name: the game's setter: %u matches", sh_hits);
    if (sh_hits != 1) return;
    g_shop_setter = sh_a;
    g_shop_str = ((const uint8_t *)sh_a)[11];    // the add rcx,<str off> immediate
    g_shop_max = ((const uint8_t *)sh_a)[13];    // the mov edx,<max> immediate
    sh_hits = 0; sh_off = 0;
    scan_exec(game, scan_shop_off);
    pc64_log("shop name: the setter's call sites: %u, the object offset 0x%x", sh_hits, sh_off);
    if (sh_hits < 1 || sh_off == 0xffffffffu) return;
    g_shop_off = sh_off;
    sh_hits = 0; sh_dec = NULL;
    scan_exec(game, scan_shop_decider);
    pc64_log("shop name: the boot box's decider site: %u matches", sh_hits);
    if (sh_hits != 1) return;
    g_shop_decider = sh_dec;
    g_shop_ok = 1;
    pc64_log("shop name: the game's own routines found (setter %p, decider %p, shop at I/O+0x%x, string +0x%x, max %u)",
             g_shop_setter, g_shop_decider, g_shop_off, g_shop_str, g_shop_max);
}

// --- the file events: chained onto the file hooks' imports, after them
static int is_cabinet(const char *path) {
    if (!path) return 0;
    size_t n = strlen(path);
    static const char tail[] = "nvram\\cabinet.xml";
    static const char tail2[] = "nvram/cabinet.xml";
    if (n < sizeof tail - 1) return 0;
    if (!_stricmp(path + n - (sizeof tail - 1), tail)) return 1;
    return n >= sizeof tail2 - 1 && !_stricmp(path + n - (sizeof tail2 - 1), tail2);
}

static int g_armed;          // no backup file when the run began
static int g_holding, g_never, g_saw_write, g_done, g_shop_done, g_in;
static DWORD g_hold_since;
#define HOLD_GUARD_MS 120000u   // guard only: release if no write ever comes (the write is the event)

static HANDLE(WINAPI *r_CFA)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static HANDLE(WINAPI *r_CFW)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);

static void note(const char *path, DWORD acc, HANDLE h) {
    if (g_in || !is_cabinet(path)) return;
    if (h == INVALID_HANDLE_VALUE) {
        if (g_armed && !g_holding && !g_never) {
            g_holding = 1;
            g_hold_since = GetTickCount();
            pc64_log("clock: no backup yet (the game's open of cabinet.xml failed): TEST reported held");
        }
    } else if (acc & (GENERIC_WRITE | GENERIC_ALL)) {
        if (g_holding) {
            g_holding = 0;
            pc64_log("clock: the game's write of cabinet.xml seen (thread %lu): TEST released", GetCurrentThreadId());
        }
        g_never = 1;   // never again in this run
        g_saw_write = 1;
    }
}
static HANDLE WINAPI CFA_(LPCSTR path, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE t) {
    HANDLE h = r_CFA(path, acc, share, sa, disp, flags, t);
    note(path, acc, h);
    return h;
}
static HANDLE WINAPI CFW_(LPCWSTR path, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE t) {
    char a[MAX_PATH * 2];
    int known = WideCharToMultiByte(CP_UTF8, 0, path, -1, a, sizeof a, NULL, NULL) > 0;
    HANDLE h = r_CFW(path, acc, share, sa, disp, flags, t);
    note(known ? a : NULL, acc, h);
    return h;
}

// The board stand-in's poll asks: TEST held for a first boot (until the backup is written)
unsigned pc64_clock_test_held(void) {
    if (g_holding && GetTickCount() - g_hold_since > HOLD_GUARD_MS) {
        g_holding = 0;
        g_never = 1;
        pc64_log("clock: the guard timeout without the backup's write: TEST released");
    }
    return g_holding ? 1u : 0u;
}

// Called from the board stand-in's status poll: once, on a backup the game has just created, set
// the clock flag and save through the game's own routines (nothing when the flag is already set)
void pc64_clock_poll(void) {
    if ((g_done && g_shop_done) || !g_saw_write || !g_armed || g_in) return;
    if (!g_found) {
        g_found = 1;
        find_routines(g_game);
    }
    if (!g_ok) return;
    if (!g_done) {
        g_done = 1;
        uint8_t flag;
        memcpy(&flag, g_clock + g_flag_off, 1);
        if (flag) {
            pc64_log("clock: the clock record already set (flag +0x%x = %u): nothing done", g_flag_off, flag);
        } else {
            g_in = 1;
            ((void (*)(void *, uint8_t))g_setter)(g_clock, 1);
            memcpy(&flag, g_clock + g_flag_off, 1);
            const int sr = ((int (*)(void *))g_saver)(g_io);
            g_in = 0;
            pc64_log("clock: the game's own clock-set done once on the new backup (thread %lu; setter %p, saver %p, I/O %p, clock %p): the flag now %u, the saver returned %d",
                     GetCurrentThreadId(), g_setter, g_saver, g_io, (void *)g_clock, flag, sr);
        }
    }
    if (!g_shop_done && g_shop_ok) {   // the same moment: the shop name the operator would set
        g_shop_done = 1;
        uint8_t *shop = (uint8_t *)g_io + g_shop_off;
        const int r = ((int (*)(void *))g_shop_decider)(shop);
        if (r == 0) {
            pc64_log("shop name: the game's decider already passes: nothing done");
        } else {
            uint8_t before[9] = {0};
            memcpy(before, shop + g_shop_str, 8);
            g_in = 1;
            ((void (*)(void *, const char *))g_shop_setter)(shop, pc64_ami_rom());
            const int sr = ((int (*)(void *))g_saver)(g_io);
            g_in = 0;
            const int r2 = ((int (*)(void *))g_shop_decider)(shop);
            uint8_t after[9] = {0};
            memcpy(after, shop + g_shop_str, 8);
            pc64_log("shop name: the game's own setter+saver done once on the new backup (thread %lu; setter %p, decider %p, saver %p, shop %p, string +0x%x): the decider was %d, the saver returned %d, now %d; the name was %02x%02x%02x%02x.. now '%s'",
                     GetCurrentThreadId(), g_shop_setter, g_shop_decider, g_saver, (void *)shop, g_shop_str,
                     r, sr, r2, before[0], before[1], before[2], before[3], (const char *)(shop + g_shop_str));
        }
    }
}

void pc64_clock_install(HMODULE game) {
    if (!game) return;
    g_game = game;
    g_image_size = ((IMAGE_NT_HEADERS *)((char *)game + ((IMAGE_DOS_HEADER *)game)->e_lfanew))->OptionalHeader.SizeOfImage;
    g_armed = GetFileAttributesA("dev\\nvram\\cabinet.xml") == INVALID_FILE_ATTRIBUTES;
    pc64_log("clock: %s at the run's start", g_armed ? "no dev\\nvram\\cabinet.xml" : "a backup already there");
    void *o;
    HMODULE avs = GetModuleHandleA("avs2-core.dll");
    if ((o = pc64_import(game, "kernel32.dll", "CreateFileA", (void *)CFA_))) r_CFA = o;
    if ((o = pc64_import(game, "kernel32.dll", "CreateFileW", (void *)CFW_))) r_CFW = o;
    if (avs) {
        if ((o = pc64_import(avs, "kernel32.dll", "CreateFileA", (void *)CFA_)) && !r_CFA) r_CFA = o;
        if ((o = pc64_import(avs, "kernel32.dll", "CreateFileW", (void *)CFW_)) && !r_CFW) r_CFW = o;
    }
    if (!r_CFA || !r_CFW) pc64_log("clock: the file hooks not chained (CreateFileA %p W %p)", (void *)r_CFA, (void *)r_CFW);
}
