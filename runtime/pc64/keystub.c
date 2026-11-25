// The license key (kbt.dll, kld.dll: on the cabinet, not in the game's folder): the game finds
// them with GetModuleHandleA and their calls with GetProcAddress; two keys present (the stubs
// spice2x uses: the key status, 16 bytes a key). Shared by the boards (board.c, board_iidx.c).
#include "pc64.h"

#include <string.h>

typedef struct { uint32_t v1; uint8_t v2, v3, v4, v5; uint32_t v6, v7; } key_status;
static int __cdecl key_zero(void) { return 0; }
static int __cdecl key_status_(key_status *st) {
    memset(st, 0, 2 * sizeof *st);
    for (int i = 0; i < 2; i++) st[i].v2 = 1, st[i].v4 = 1, st[i].v7 = ~0u;
    return 0;
}
static void __cdecl key_run(void (__cdecl *f)(void *), void *arg) {
    if (f) f(arg);
}
static char g_kbt, g_kld;
#define KBT ((HMODULE)&g_kbt)
#define KLD ((HMODULE)&g_kld)
static HMODULE(WINAPI *r_GetModuleHandleA)(LPCSTR);
static FARPROC(WINAPI *r_GetProcAddress)(HMODULE, LPCSTR);
static HMODULE WINAPI GetModuleHandleA_(LPCSTR name) {
    if (name && !_stricmp(name, "kbt.dll")) return KBT;
    if (name && !_stricmp(name, "kld.dll")) return KLD;
    return r_GetModuleHandleA(name);
}
static FARPROC WINAPI GetProcAddress_(HMODULE m, LPCSTR name) {
    if (m != KBT && m != KLD) return r_GetProcAddress(m, name);
    if (!HIWORD((ULONG_PTR)name)) return NULL;
    if (!strcmp(name, "bt_get_ikey_status") || !strcmp(name, "k_bt0001")) return (FARPROC)(void *)key_status_;
    if (!strcmp(name, "k_bt0002")) return (FARPROC)(void *)key_run;
    if (!strncmp(name, "bt_fcheck_", 10)) return (FARPROC)(void *)key_zero;
    return NULL;
}

void pc64_keystub_install(HMODULE game) {
    r_GetModuleHandleA = pc64_import(game, "kernel32.dll", "GetModuleHandleA", (void *)GetModuleHandleA_);
    if (!r_GetModuleHandleA) pc64_log("the game doesn't import kernel32.dll!GetModuleHandleA");
    r_GetProcAddress = pc64_import(game, "kernel32.dll", "GetProcAddress", (void *)GetProcAddress_);
    if (!r_GetProcAddress) pc64_log("the game doesn't import kernel32.dll!GetProcAddress");
}
