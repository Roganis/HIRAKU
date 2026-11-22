// SPDX-License-Identifier: GPL-3.0-or-later
// A game's program where Windows put it (docs/MODS.md, "The PC games"): a mod's addresses are the
// program's own, as a disassembler shows them at its preferred base (ImageBase); a DLL loaded
// elsewhere (Wine 11.22 moves every DLL that has a relocation table when the EXE starting it is
// DYNAMIC_BASE, as hiraku-pc32.exe and hiraku-pc64.exe are) has each such address moved by its
// load address less its base, and a patch's bytes at the fields its base relocation table names
// (an absolute operand: `add [0x10de78dc], ebx`) read and written as the loader relocated them.
// Pure functions on the image's mapped bytes (the PE headers and the relocation table there), for
// the 32-bit DLLs (runtime/pc/cabinet.c) and the 64-bit loader (runtime/pc64/patches.c); an EXE
// at its own base (pop'n 15-18, the PC IIDX games) changes nothing.
#pragma once
#include <stddef.h>
#include <stdint.h>

struct hk_image {
    const uint8_t *map;      // the image as mapped (its first byte at `real`)
    uint64_t preferred;      // ImageBase
    uint64_t real;           // where it is
    uint32_t size;           // SizeOfImage
    uint32_t relocs, relocs_size;   // the base relocation table (RVA, bytes); 0: none
};

// The headers of the image mapped at `map` (whose address is `real`): 0 when it isn't a PE image
// (then *im moves nothing)
int hk_image_open(struct hk_image *im, const void *map, uint64_t real);
// An address as the mod writes it (at the preferred base): where it is now. Outside the image's
// preferred range: as it is
uint64_t hk_image_move(const struct hk_image *im, uint64_t addr);
// ... and back: a real address as the mod would write it (for the logs); outside the image: as it is
uint64_t hk_image_unmove(const struct hk_image *im, uint64_t addr);
// The patch's bytes at [rva, rva + n) of the image, made what the loader made them: `original`
// (may be NULL) the bytes the mod expects there at the preferred base, each relocated field it
// covers moved as the loader moved it; `replace` (may be NULL) moved at a relocated field only
// where it still holds an address in the image (a replace that keeps an absolute operand; a nop
// written over one stays as it is). A field partly outside the range takes its other bytes from the
// mapped image. Returns how many relocated fields the range covers (0: the bytes are as they were)
int hk_image_fix_patch(const struct hk_image *im, uint32_t rva, size_t n, uint8_t *original, uint8_t *replace);
