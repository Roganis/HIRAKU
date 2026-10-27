// SPDX-License-Identifier: GPL-3.0-or-later
// The mods' memory patches (docs/MODS.md, [[patch]]): hiraku-pc's list (HK_PC_PATCHES), a line a
// patch: "WHERE ONCE ORIGINAL REPLACE" in hex, WHERE an address or MODULE+OFFSET (a module of the
// game's, wherever Windows put it: "popn.dll+765bc"), ORIGINAL "-" for none (a cheat: written
// each time). Each goes in whenever its original bytes are there, once if asked, as the 32-bit
// games' DLL does (runtime/pc/cabinet.c): when the game's program is loaded, then each frame.
#include "pc64.h"
#include "../pc/link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct patch { BYTE *at, *original, *replace; size_t n; int once, done, line; } *g_patches;   // line: hiraku-pc's number
static int g_npatches;

static int unhex(const char *h, BYTE **out) {
    const size_t n = strlen(h) / 2;
    *out = malloc(n ? n : 1);
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        if (!*out || sscanf(h + 2 * i, "%2x", &v) != 1) return -1;
        (*out)[i] = (BYTE)v;
    }
    return (int)n;
}

// Whether [p, p + n) is all there to read
static int present(const BYTE *p, size_t n) {
    for (const BYTE *end = p + n; p < end;) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(p, &mi, sizeof mi) || mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
        p = (const BYTE *)mi.BaseAddress + mi.RegionSize;
    }
    return 1;
}

void pc64_patches_apply(void) {
    for (int i = 0; i < g_npatches; i++) {
        struct patch *p = &g_patches[i];
        if (p->done || !present(p->at, p->n) || !memcmp(p->at, p->replace, p->n)) continue;   // gone, not there, in place
        if (p->original && memcmp(p->at, p->original, p->n)) continue;   // not (yet) what the mod was made for
        DWORD old;
        VirtualProtect(p->at, p->n, PAGE_EXECUTE_READWRITE, &old);
        memcpy(p->at, p->replace, p->n);
        VirtualProtect(p->at, p->n, old, &old);
        FlushInstructionCache(GetCurrentProcess(), p->at, p->n);
        struct hk_pc_link *l = pc64_link();
        if (!l || !l->patch_done[p->line]) pc64_log("mods: patch at %p (%u bytes) in place", (void *)p->at, (unsigned)p->n);
        if (l) l->patch_done[p->line] = 1;
        if (p->once) p->done = 1;
    }
}

void pc64_patches_install(void) {
    const char *list = getenv("HK_PC_PATCHES");
    FILE *f = list && *list ? fopen(list, "rb") : NULL;
    if (!f) return;
    char line[8192];
    int bad = 0, line_no = -1;
    while (fgets(line, sizeof line, f) && line_no < 255) {
        line_no++;
        char where[300], orig[4096], repl[4096];
        int once;
        if (sscanf(line, "%299s %d %4095s %4095s", where, &once, orig, repl) != 4) continue;
        BYTE *at = NULL;
        char *plus = strrchr(where, '+');
        if (plus) {   // MODULE+OFFSET
            *plus = 0;
            HMODULE m = GetModuleHandleA(where);
            if (m) at = (BYTE *)m + strtoull(plus + 1, NULL, 16);
            else if (bad++ < 4) pc64_log("mods: a patch in %s: the game has no such module", where);
        } else {
            at = (BYTE *)(uintptr_t)strtoull(where, NULL, 16);
        }
        struct patch p = {at, NULL, NULL, 0, once, 0, line_no};
        const int n = unhex(repl, &p.replace);
        if (!at || n <= 0 || (strcmp(orig, "-") && unhex(orig, &p.original) != n)) continue;
        p.n = (size_t)n;
        struct patch *more = realloc(g_patches, sizeof *g_patches * (size_t)(g_npatches + 1));
        if (!more) break;
        g_patches = more;
        g_patches[g_npatches++] = p;
    }
    fclose(f);
    if (g_npatches) pc64_log("mods: %d memory patches", g_npatches);
    pc64_patches_apply();
}
