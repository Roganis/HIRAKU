// SPDX-License-Identifier: GPL-3.0-or-later
// The mods' memory patches (docs/MODS.md, [[patch]]): hiraku-pc's list (HK_PC_PATCHES), a line a
// patch: "WHERE ONCE ORIGINAL REPLACE" in hex, WHERE an address or MODULE+OFFSET (a module of the
// game's, wherever Windows put it: "popn.dll+765bc"), ORIGINAL "-" for none (a cheat: written
// each time). Each goes in whenever its original bytes are there, once if asked, as the 32-bit
// games' DLL does (runtime/pc/cabinet.c): when the game's program is loaded, then each frame.
// The game's DLL is wherever Windows (or Wine) put it: an address in it at its preferred base is
// moved with it, and a patch's bytes at the fields its relocation table names are compared and
// written as relocated (runtime/pc/imagebase.h), in a module's patch too.
#include "pc64.h"
#include "../pc/imagebase.h"
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

void pc64_patches_install(HMODULE game) {
    // the game's DLL where it is, once in the log
    struct hk_image gi;
    if (hk_image_open(&gi, game, (uintptr_t)game))
        pc64_log("mods: the game's DLL at %llx (its own base %llx)", (unsigned long long)gi.real, (unsigned long long)gi.preferred);
    const char *list = getenv("HK_PC_PATCHES");
    FILE *f = list && *list ? fopen(list, "rb") : NULL;
    if (!f) return;
    // the whole list, cut into lines and words in place (a patch's bytes are as long as the mod
    // made them: no buffer of a fixed size to fit)
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = malloc((size_t)(size > 0 ? size : 0) + 1);
    if (!text) { fclose(f); return; }
    text[fread(text, 1, (size_t)(size > 0 ? size : 0), f)] = 0;
    fclose(f);
    int bad = 0, unread = 0, line_no = -1, moves = 0, fixed = 0;
    for (char *line = text, *next; *line && line_no < 255; line = next) {
        next = line + strcspn(line, "\n");
        if (*next) *next++ = 0;
        line[strcspn(line, "\r")] = 0;
        line_no++;   // (the line's place is the patch's in hiraku-pc's list, read or not)
        char *words[4] = {NULL, NULL, NULL, NULL};
        char *w = line;
        for (int k = 0; k < 4; k++) {
            w += strspn(w, " \t");
            if (!*w) break;
            words[k] = w;
            w += strcspn(w, " \t");
            if (*w) *w++ = 0;
        }
        if (!words[3]) { unread += words[0] != NULL; continue; }
        char *where = words[0];
        const char *orig = words[2], *repl = words[3];
        const int once = atoi(words[1]);
        BYTE *at = NULL;
        struct hk_image im = gi;   // the image the patch is in
        char *plus = strrchr(where, '+');
        if (plus) {   // MODULE+OFFSET: from where the module is (an address in it at its base: moved)
            *plus = 0;
            HMODULE m = GetModuleHandleA(where);
            const unsigned long long off = strtoull(plus + 1, NULL, 16);
            if (m && hk_image_open(&im, m, (uintptr_t)m)) at = off < im.size ? (BYTE *)m + off : (BYTE *)(uintptr_t)hk_image_move(&im, off);
            else if (m) at = (BYTE *)m + off;
            else if (bad++ < 4) pc64_log("mods: a patch in %s: the game has no such module", where);
        } else {   // an address: the game's DLL's at its base, moved with it
            const unsigned long long a = strtoull(where, NULL, 16);
            at = (BYTE *)(uintptr_t)hk_image_move(&gi, a);
            moves += (uintptr_t)at != a;
        }
        struct patch p = {at, NULL, NULL, 0, once, 0, line_no};
        const int n = unhex(repl, &p.replace);
        if (!at) continue;   // (said above)
        if (n <= 0 || (strcmp(orig, "-") && unhex(orig, &p.original) != n)) { unread++; continue; }
        p.n = (size_t)n;
        if (im.size && (uintptr_t)at >= im.real && (uintptr_t)at - im.real < im.size)   // its bytes as the loader relocated them
            fixed += hk_image_fix_patch(&im, (uint32_t)((uintptr_t)at - im.real), p.n, p.original, p.replace) > 0;
        struct patch *more = realloc(g_patches, sizeof *g_patches * (size_t)(g_npatches + 1));
        if (!more) break;
        g_patches = more;
        g_patches[g_npatches++] = p;
    }
    free(text);
    if (g_npatches) pc64_log("mods: %d memory patches", g_npatches);
    if (moves || fixed) pc64_log("mods: %d of them at an address moved with the game's DLL, %d with relocated bytes", moves, fixed);
    if (unread) pc64_log("mods: %d of the patches' lines couldn't be read: left out", unread);
    pc64_patches_apply();
}
