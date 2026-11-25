// SPDX-License-Identifier: GPL-3.0-or-later
// A module's imports, pointed elsewhere: the loader's way into the game (pc64.h)
#include "pc64.h"

#include <string.h>

void pc64_patch(void *slot, void *fn) {
    DWORD old;
    VirtualProtect(slot, sizeof(void *), PAGE_EXECUTE_READWRITE, &old);
    *(void **)slot = fn;
    VirtualProtect(slot, sizeof(void *), old, &old);
}

// The slot of DLL's import NAME (NULL: by ORDINAL) in module M
static void **import_slot(HMODULE m, const char *dll, const char *name, WORD ordinal) {
    BYTE *base = (BYTE *)m;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return NULL;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); d->Name; d++) {
        if (_stricmp((char *)base + d->Name, dll)) continue;
        IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        IMAGE_THUNK_DATA *slots = (IMAGE_THUNK_DATA *)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, slots++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
                if (!name && IMAGE_ORDINAL(names->u1.Ordinal) == ordinal) return (void **)&slots->u1.Function;
            } else if (name && !strcmp((char *)((IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData))->Name, name)) {
                return (void **)&slots->u1.Function;
            }
        }
    }
    return NULL;
}

void *pc64_import(HMODULE m, const char *dll, const char *name, void *fn) {
    void **slot = import_slot(m, dll, name, 0);
    if (!slot) return NULL;
    void *old = *slot;
    pc64_patch(slot, fn);
    return old;
}

void *pc64_import_ordinal(HMODULE m, const char *dll, WORD ordinal, void *fn) {
    void **slot = import_slot(m, dll, NULL, ordinal);
    if (!slot) return NULL;
    void *old = *slot;
    pc64_patch(slot, fn);
    return old;
}

// The slot of DLL's import NAME in module M, without pointing it anywhere (to find the game's
// own call sites of an import by their code's shape)
void **pc64_import_slot(HMODULE m, const char *dll, const char *name) {
    return import_slot(m, dll, name, 0);
}
