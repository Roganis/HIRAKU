// SPDX-License-Identifier: GPL-3.0-or-later
// hiraku-pc32 (PLAN.md section 35): the 32-bit AVS2 PC games (popn22.dll) started as the
// cabinet's bootstrap.exe starts them, with HIRAKU's cabinet DLLs (runtime/pc: ezusb.dll,
// libacio.dll) in place of Konami's. In the order spice2x (GPL-3.0) uses (avs/core.cpp,
// avs/ea3.cpp), as runtime/pc64/main.c does for the 64-bit games: AVS2's boot with
// prop/avs-config.xml, the defaults copied into /dev/nvram, the game's DLL loaded,
// dll_entry_init with the soft ID, ea3_boot with /dev/nvram/ea3-config.xml, then dll_entry_main,
// which runs the game.
//
//   hiraku-pc32.exe CONTENTS [GAME.dll]     CONTENTS: the game's folder (modules/, prop/, data/)
//
// The loader runs in the folder it's started in (AVS2's root: data/ and prop/ there, dev/ made
// there), and loads the game's DLLs from its own folder first (HIRAKU's ezusb.dll and libacio.dll
// beside it), then CONTENTS\modules. The cabinet's IDs come from Konami's prop/bootstrap.xml (its
// security code and release code) and the game's DLL's parameters from its <component>.
//
// HK_PC32_EA: e-AMUSEMENT's services URL (http://a.b.c.d:port/), in place of ea3-config's (or
// HK_PC_EA's a.b.c.d:port, hiraku-pc's server)
// HK_PC32_LOG: the loader's log file (else stderr); AVS2's own log goes to the same place
// HK_PC32_PCBID: the cabinet's PCB ID (20 hex digits)
// HK_PC32_DATA: the mods' folders (DIR;DIR...), each laid over the game's data folder (files.c)
// HK_PC32_TRACE: the game's reads of its answers (each struct read through a descriptor: its first
// field's name and the result; each node looked up by path), to the log, for finding what an answer lacks
#include "pc32.h"
#include "../pc64/pc64.h"   // pc64_import, pc64_patch: a module's import slot pointed elsewhere (runtime/pc64/imports.c)

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *log_file(void) {
    static FILE *f;
    if (!f) {
        const char *path = getenv("HK_PC32_LOG");
        f = path ? fopen(path, "a") : NULL;
        if (!f) f = stderr;
    }
    return f;
}

#define say pc32_log
int pc32_trace(void) {
    static int on = -1;
    if (on < 0) on = getenv("HK_PC32_TRACE") != NULL;
    return on;
}
void pc32_log(const char *fmt, ...) {
    FILE *f = log_file();
    va_list a;
    va_start(a, fmt);
    fputs("hiraku-pc32: ", f);
    vfprintf(f, fmt, a);
    fputc('\n', f);
    va_end(a);
    fflush(f);
}

// AVS2's exports by version: each has its own scrambled names (spice2x's IMPORT_AVS21580)
struct avs_names {
    const char *core, *ea3, *version;
    const char *boot, *property_create, *property_insert_read, *property_read_query_memsize,
        *property_search, *property_node_create, *property_node_remove, *property_node_refer, *std_setenv,
        *avs_fs_copy, *avs_fs_lstat, *avs_fs_mount, *psmap_import, *avs_fs_open, *ea3_boot;
};
static const struct avs_names AVS[] = {
    {"libavs-win32.dll", "libavs-win32-ea3.dll", "2.15.8",
     "XCd229cc0000aa", "XCd229cc000126", "XCd229cc00009a", "XCd229cc0000ff",
     "XCd229cc00012e", "XCd229cc00002c", "XCd229cc000028", "XCd229cc000009", "XCd229cc000094",
     "XCd229cc0000eb", "XCd229cc0000c0", "XCd229cc0000ce", "XCd229cc000005", "XCd229cc000090", "XE592acd00008c"},
};

typedef void *prop_t;
typedef void *node_t;
typedef int (*avs_reader_t)(uint32_t, void *, size_t);
// 2.15's boot: two heaps (AVS2's own and the standard one), the log's writer
static int (*avs_boot)(node_t config, void *heap1, size_t size1, void *heap2, size_t size2, void *log, HANDLE file);
static prop_t (*property_create)(int flags, void *buffer, uint32_t size);
static int (*property_insert_read)(prop_t, node_t, avs_reader_t, uint32_t);
static int (*property_read_query_memsize)(avs_reader_t, uint32_t, DWORD *, DWORD *);
static node_t (*property_search)(prop_t, node_t, const char *path);
static node_t (*property_node_create)(prop_t, node_t, int type, const char *path, ...);
static int (*property_node_remove)(node_t);
static int (*property_node_refer)(prop_t, node_t, const char *path, int type, void *out, uint32_t size);
static int (*std_setenv)(const char *, const char *);
static int (*avs_fs_copy)(const char *, const char *);
static int (*avs_fs_lstat)(const char *, void *);
static int (*ea3_boot)(node_t);
static int (*avs_fs_mount)(const char *mountpoint, const char *fsroot, const char *fstype, void *data);
#define NODE_S32 6
#define NODE_STR 11
#define NODE_BOOL 52

static void *need(HMODULE m, const char *what, const char *name) {
    void *p = (void *)GetProcAddress(m, name);
    if (!p) {
        say("%s has no %s: not an AVS2 this loader knows", what, name);
        ExitProcess(2);
    }
    return p;
}

// AVS2's log, into the loader's
static BOOL avs_log(const char *buf, DWORD size, HANDLE file) {
    (void)file;
    FILE *f = log_file();
    fwrite(buf, 1, size, f);
    fflush(f);
    return TRUE;
}

static FILE *g_reading;
static int read_file(uint32_t context, void *to, size_t n) {
    (void)context;
    return (int)fread(to, 1, n, g_reading);
}

static prop_t read_property(FILE *in, const char *what, size_t extra) {
    g_reading = in;
    int size = property_read_query_memsize(read_file, 0, NULL, NULL);
    if (size <= 0) {
        say("%s: not a property (%d)", what, size);
        ExitProcess(2);
    }
    size += (int)extra;   // room for the nodes the loader adds: AVS2 doesn't grow a property
    rewind(in);
    prop_t p = property_create(23, malloc((size_t)size), (uint32_t)size);
    if (!p || !property_insert_read(p, NULL, read_file, 0)) {
        say("%s: can't read it", what);
        ExitProcess(2);
    }
    return p;
}

static prop_t read_property_file(const char *path, size_t extra) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        say("no %s", path);
        ExitProcess(2);
    }
    prop_t p = read_property(f, path, extra);
    fclose(f);
    return p;
}

static prop_t read_property_text(const char *text, const char *what) {
    FILE *f = tmpfile();
    fwrite(text, 1, strlen(text), f);
    rewind(f);
    prop_t p = read_property(f, what, 0x1000);
    fclose(f);
    return p;
}

static void drop(prop_t p, node_t n, const char *path) {
    node_t x = property_search(p, n, path);
    if (x) property_node_remove(x);
}

static void set_text(prop_t p, node_t n, const char *path, const char *value) {
    drop(p, n, path);
    property_node_create(p, n, NODE_STR, path, value);
}

static void text(prop_t p, const char *path, char *out, size_t n) {
    out[0] = 0;
    property_node_refer(p, NULL, path, NODE_STR, out, (uint32_t)n);
}

// The text of the first <TAG>...</TAG> in a file's text (bootstrap.xml's few values: it's Shift-JIS
// with comments, and AVS2 isn't booted yet when they're needed)
static int xml_value(const char *xml, const char *tag, char *out, size_t n) {
    char open[64];
    snprintf(open, sizeof open, "<%s", tag);
    for (const char *p = strstr(xml, open); p; p = strstr(p + 1, open)) {
        const char *q = p + strlen(open);
        if (*q != '>' && *q != ' ') continue;
        q = strchr(q, '>');
        if (!q) return 0;
        q++;
        const char *end = strchr(q, '<');
        if (!end) return 0;
        while (q < end && (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')) q++;
        size_t len = (size_t)(end - q);
        while (len && (q[len - 1] == ' ' || q[len - 1] == '\r' || q[len - 1] == '\n' || q[len - 1] == '\t')) len--;
        if (len >= n) len = n - 1;
        memcpy(out, q, len);
        out[len] = 0;
        return 1;
    }
    return 0;
}

static char *read_all(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    char *s = malloc((size_t)n + 1);
    s[fread(s, 1, (size_t)n, f)] = 0;
    fclose(f);
    return s;
}

// A fault's place (the first few), for bringing the game up: under Wine an unhandled one waits in its
// debugger with the game silent
static LONG CALLBACK on_fault(EXCEPTION_POINTERS *e) {
    const DWORD code = e->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != EXCEPTION_STACK_OVERFLOW)
        return EXCEPTION_CONTINUE_SEARCH;
    static volatile LONG said;
    if (InterlockedIncrement(&said) > 8) return EXCEPTION_CONTINUE_SEARCH;
    HMODULE m = NULL;
    char path[MAX_PATH] = "?";
    void *at = e->ExceptionRecord->ExceptionAddress;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, at, &m);
    if (m) GetModuleFileNameA(m, path, sizeof path);
    const char *b = strrchr(path, '\\');
    say("the game faulted: %08lx at %s+%lx (thread %lu), address %08lx", (unsigned long)code, b ? b + 1 : path,
        (unsigned long)((char *)at - (char *)m), GetCurrentThreadId(),
        e->ExceptionRecord->NumberParameters > 1 ? (unsigned long)e->ExceptionRecord->ExceptionInformation[1] : 0ul);
    return EXCEPTION_CONTINUE_SEARCH;
}

// The licence key (kbt.dll, kld.dll: Konami's talk to the cabinet's dongle): the game finds them with
// GetModuleHandleA and their calls with GetProcAddress; two keys present, every file check passed
// (spice2x's stubs, as runtime/pc64/board.c answers the 64-bit game)
typedef struct { uint32_t v1; uint8_t v2, v3, v4, v5; uint32_t v6, v7; } key_status;
static int __cdecl key_zero(void) { return 0; }
static int __cdecl key_status_(key_status *st) {
    memset(st, 0, 2 * sizeof *st);
    for (int i = 0; i < 2; i++) st[i].v2 = 1, st[i].v4 = 1, st[i].v7 = ~0u;
    return 0;
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
    if (!strcmp(name, "bt_get_ikey_status")) return (FARPROC)(void *)key_status_;
    if (!strncmp(name, "bt_fcheck_", 10)) return (FARPROC)(void *)key_zero;
    say("the game asked the licence key for %s", name);
    return NULL;
}

// The game's import slot (from any DLL) that holds TARGET, pointed at FN: AVS2's calls are imported by
// ordinal, so they're found by where they lead. The old target, or NULL when no slot has it
void *pc32_hook_target(HMODULE m, void *target, void *fn) {
    BYTE *base = (BYTE *)m;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || !target) return NULL;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); d->Name; d++)
        for (IMAGE_THUNK_DATA *t = (IMAGE_THUNK_DATA *)(base + d->FirstThunk); t->u1.Function; t++)
            if ((void *)t->u1.Function == target) {
                pc64_patch(&t->u1.Function, fn);
                return target;
            }
    return NULL;
}

// HK_PC32_TRACE: the struct reads (a descriptor: 16-byte entries, the node's name at +8) and searches
static int (*r_psmap_import)(prop_t, node_t, void *, const uint8_t *);
static node_t (*r_search)(prop_t, node_t, const char *);
static int psmap_import_(prop_t p, node_t n, void *to, const uint8_t *desc) {
    const int r = r_psmap_import(p, n, to, desc);
    const char *first = desc ? *(const char *const *)(desc + 8) : "?";
    say("trace: struct read from %s... -> %d", first ? first : "?", r);
    return r;
}
static node_t search_(prop_t p, node_t n, const char *path) {
    node_t r = r_search(p, n, path);
    say("trace: search %s -> %s", path ? path : "?", r ? "found" : "none");
    return r;
}

// The cabinet's drives the game mounts in AVS2 (/e_drive from e:/: its downloads, up/, and tmp/):
// folders of the save folder's dev/vfs/drive_<letter> instead, as spice2x makes them
static void make_dirs(char *path) {
    for (char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') {
            const char c = *p;
            *p = 0;
            CreateDirectoryA(path, NULL);
            *p = c;
        }
    CreateDirectoryA(path, NULL);
}
static int avs_fs_mount_(const char *mountpoint, const char *fsroot, const char *fstype, void *data) {
    if (mountpoint && fsroot && fstype && !_stricmp(fstype, "fs") && fsroot[0] && fsroot[1] == ':' &&
        strchr("dDeEfF", fsroot[0])) {
        char to[MAX_PATH];
        const char *sub = fsroot + 2;
        snprintf(to, sizeof to, "dev/vfs/drive_%c%s%s", fsroot[0] | 0x20, *sub == '/' || *sub == '\\' ? "" : "/", sub);
        make_dirs(to);
        if ((fsroot[0] | 0x20) == 'e' && (!sub[0] || ((sub[0] == '/' || sub[0] == '\\') && !sub[1]))) {
            char d[MAX_PATH + 8];
            snprintf(d, sizeof d, "dev/vfs/drive_e/tmp");
            CreateDirectoryA(d, NULL);
            snprintf(d, sizeof d, "dev/vfs/drive_e/up");
            CreateDirectoryA(d, NULL);
        }
        say("%s: the cabinet's %s, here %s", mountpoint, fsroot, to);
        return avs_fs_mount(mountpoint, to, fstype, data);
    }
    char moved[MAX_PATH * 2];
    const char *from = pc32_files_mount(mountpoint, fsroot, fstype, moved, sizeof moved);   // a mod's pack (files.c)
    const int r = avs_fs_mount(mountpoint, from, fstype, data);
    if (pc32_trace()) say("trace: mount %s %s %s -> %d", mountpoint ? mountpoint : "?", from ? from : "?", fstype ? fstype : "?", r);
    return r;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: hiraku-pc32 CONTENTS [GAME.dll]\n");
        return 2;
    }
    char contents[MAX_PATH], modules[MAX_PATH + 16], path[MAX_PATH + 32];
    GetFullPathNameA(argv[1], sizeof contents, contents, NULL);
    snprintf(modules, sizeof modules, "%s\\modules", contents);
    if (GetFileAttributesA(modules) == INVALID_FILE_ATTRIBUTES) {
        say("no %s: not the game's folder", modules);
        return 2;
    }
    SetDllDirectoryA(modules);
    AddVectoredExceptionHandler(1, on_fault);
    CreateDirectoryA("dev", NULL);
    CreateDirectoryA("dev\\nvram", NULL);
    CreateDirectoryA("dev\\raw", NULL);

    // Konami's bootstrap.xml: the game's DLL, its parameters, the cabinet's codes
    snprintf(path, sizeof path, "%s\\prop\\bootstrap.xml", contents);
    char *bootstrap = read_all(path);
    if (!bootstrap) {
        say("no %s", path);
        return 2;
    }
    char game_dll[64] = "popn22.dll", security[16] = "", release[16] = "";
    const char *component = strstr(bootstrap, "<component>");
    if (component) xml_value(component, "file", game_dll, sizeof game_dll);
    if (argc > 2) snprintf(game_dll, sizeof game_dll, "%s", argv[2]);
    xml_value(bootstrap, "security_code", security, sizeof security);
    xml_value(bootstrap, "release_code", release, sizeof release);
    // the security code: G, a letter, the model (3), dest, spec, rev
    if (strlen(security) != 8 || strlen(release) != 10) {
        say("bootstrap.xml: no security code (%s) or release code (%s) as expected", security, release);
        return 2;
    }
    char model[4] = {security[2], security[3], security[4], 0}, dest[2] = {security[5], 0}, spec[2] = {security[6], 0},
         rev[2] = {security[7], 0};
    // <component><param>...</param>: what the DLL's dll_entry_init gets
    char param[1024] = "<param></param>";
    const char *p1 = component ? strstr(component, "<param>") : NULL,
               *p2 = p1 ? strstr(p1, "</param>") : NULL;
    if (p2 && (size_t)(p2 + 8 - p1) < sizeof param) snprintf(param, sizeof param, "%.*s", (int)(p2 + 8 - p1), p1);

    // AVS2, by the version the game's folder has
    const struct avs_names *v = NULL;
    HMODULE core = NULL, ea3 = NULL;
    for (size_t i = 0; i < sizeof AVS / sizeof *AVS && !core; i++)
        if ((core = LoadLibraryA(AVS[i].core))) v = &AVS[i];
    if (!core || !(ea3 = LoadLibraryA(v->ea3))) {
        say("AVS2 didn't load from %s (%lu)", modules, GetLastError());
        return 2;
    }
    avs_boot = need(core, v->core, v->boot);
    property_create = need(core, v->core, v->property_create);
    property_insert_read = need(core, v->core, v->property_insert_read);
    property_read_query_memsize = need(core, v->core, v->property_read_query_memsize);
    property_search = need(core, v->core, v->property_search);
    property_node_create = need(core, v->core, v->property_node_create);
    property_node_remove = need(core, v->core, v->property_node_remove);
    property_node_refer = need(core, v->core, v->property_node_refer);
    std_setenv = need(core, v->core, v->std_setenv);
    avs_fs_copy = need(core, v->core, v->avs_fs_copy);
    avs_fs_lstat = need(core, v->core, v->avs_fs_lstat);
    avs_fs_mount = need(core, v->core, v->avs_fs_mount);
    ea3_boot = need(ea3, v->ea3, v->ea3_boot);
    say("AVS2 %s, %s, %s", v->version, game_dll, security);

    // AVS2's file system: the root here, NVRAM and RAW as plain folders in dev/ (the cabinet's
    // are D:\m39\contents\dev\...); Japan's time; the PC's clock, not e-AMUSEMENT's; the log here
    snprintf(path, sizeof path, "%s\\prop\\avs-config.xml", contents);
    prop_t config = read_property_file(path, 0x10000);
    node_t root = property_search(config, NULL, "/config");
    set_text(config, root, "/fs/root/device", ".");
    set_text(config, root, "/fs/nvram/device", "dev/nvram");
    set_text(config, root, "/fs/nvram/fstype", "fs");
    drop(config, root, "/fs/nvram/option");
    set_text(config, root, "/fs/raw/device", "dev/raw");
    set_text(config, root, "/fs/raw/fstype", "fs");
    drop(config, root, "/time/gmt_offset");
    property_node_create(config, root, NODE_S32, "/time/gmt_offset", 9 * 3600);
    drop(config, root, "/sntp/ea_on");
    property_node_create(config, root, NODE_BOOL, "/sntp/ea_on", 0);
    set_text(config, root, "/log/level", "misc");
    drop(config, root, "/net/enable_raw");   // a u8 in the cabinet's file; 2.15.8 reads a bool
    property_node_create(config, root, NODE_BOOL, "/net/enable_raw", 1);
    const size_t heap = 0x3000000;   // bootstrap.xml's: 48 MB AVS2's, 32 MB the standard one's
    avs_boot(root, malloc(heap), heap, malloc(heap), heap, (void *)avs_log, NULL);

    // The cabinet's settings start as its defaults (bootstrap.xml's <default>)
    char st[512];
    static const char *defaults[] = {"ea3-config.xml", "coin.xml", "eacoin.xml"};
    for (int i = 0; i < 3; i++) {
        char from[64], to[64];
        snprintf(from, sizeof from, "/prop/defaults/%s", defaults[i]);
        snprintf(to, sizeof to, "/dev/nvram/%s", defaults[i]);
        if (avs_fs_lstat(to, st) <= 0) avs_fs_copy(from, to);
    }

    HMODULE game = LoadLibraryA(game_dll);
    if (!game) {
        say("%s didn't load (%lu)", game_dll, GetLastError());
        return 2;
    }
    r_GetModuleHandleA = pc64_import(game, "kernel32.dll", "GetModuleHandleA", (void *)GetModuleHandleA_);
    r_GetProcAddress = pc64_import(game, "kernel32.dll", "GetProcAddress", (void *)GetProcAddress_);
    if (!r_GetModuleHandleA || !r_GetProcAddress) say("%s doesn't find the licence key as expected", game_dll);
    if (!pc32_hook_target(game, (void *)avs_fs_mount, (void *)avs_fs_mount_)) say("%s doesn't mount drives as expected", game_dll);
    if (GetModuleHandleA("_ibavs-win32.dll"))   // the texture library's AVS2 (its packs' mounts)
        pc32_hook_target(GetModuleHandleA("_ibavs-win32.dll"), (void *)avs_fs_mount, (void *)avs_fs_mount_);
    pc32_files_install(core, game, (pc32_mount_fn)avs_fs_mount, need(core, v->core, v->avs_fs_open), (pc32_stat_fn)avs_fs_lstat);
    if (pc32_trace()) {
        r_psmap_import = need(core, v->core, v->psmap_import);
        r_search = (void *)property_search;
        pc32_hook_target(game, (void *)r_psmap_import, (void *)psmap_import_);
        pc32_hook_target(game, (void *)r_search, (void *)search_);
    }
    int (*entry_init)(char *, node_t) = need(game, game_dll, "dll_entry_init");
    int (*entry_main)(void) = need(game, game_dll, "dll_entry_main");

    // ea3: the cabinet's IDs, its soft ID from bootstrap.xml (the cabinet's ea3-config has none)
    prop_t e = read_property_file("dev\\nvram\\ea3-config.xml", 0x40000);
    char pcbid[32], softid[32], hardid[32], account[32];
    text(e, "/ea3/id/pcbid", pcbid, sizeof pcbid);
    text(e, "/ea3/id/softid", softid, sizeof softid);
    text(e, "/ea3/id/hardid", hardid, sizeof hardid);
    text(e, "/ea3/id/accountid", account, sizeof account);
    if (getenv("HK_PC32_PCBID")) snprintf(pcbid, sizeof pcbid, "%s", getenv("HK_PC32_PCBID"));
    if (!pcbid[0]) strcpy(pcbid, "01201000000000000000");
    if (!softid[0]) strcpy(softid, "012199999999");
    if (!hardid[0]) strcpy(hardid, "0100DEADBEEF");
    if (!account[0]) strcpy(account, "012018008135");
    node_t ea3_root = property_search(e, NULL, "/ea3");
    set_text(e, ea3_root, "/id/pcbid", pcbid);
    set_text(e, ea3_root, "/id/softid", softid);
    set_text(e, ea3_root, "/id/hardid", hardid);
    set_text(e, ea3_root, "/id/accountid", account);
    set_text(e, ea3_root, "/soft/model", model);
    set_text(e, ea3_root, "/soft/dest", dest);
    set_text(e, ea3_root, "/soft/spec", spec);
    set_text(e, ea3_root, "/soft/rev", rev);
    set_text(e, ea3_root, "/soft/ext", release);
    char soft_id[64], init_code[64];
    snprintf(soft_id, sizeof soft_id, "%s:%s:%s:%s:%s", model, dest, spec, rev, release);
    snprintf(init_code, sizeof init_code, "%s%s%s%s%s", model, dest, spec, rev, release);
    std_setenv("/env/boot/build", "HIRAKU");
    std_setenv("/env/boot/version", "HIRAKU");
    std_setenv("/env/boot/tag", "HIRAKU");
    std_setenv("/env/profile/security_code", security);
    std_setenv("/env/profile/secplug_b_security_code", security);
    std_setenv("/env/profile/system_id", pcbid);
    std_setenv("/env/profile/hardware_id", hardid);
    std_setenv("/env/profile/license_id", softid);
    std_setenv("/env/profile/software_id", softid);
    std_setenv("/env/profile/account_id", account);
    std_setenv("/env/profile/soft_id_code", soft_id);

    prop_t params = read_property_text(param, "bootstrap.xml's <param>");
    say("%s: dll_entry_init(%s)", game_dll, init_code);
    if (!entry_init(init_code, property_search(params, NULL, "/param"))) {
        say("dll_entry_init failed");
        return 2;
    }
    char from_pc[64];
    const char *url = getenv("HK_PC32_EA");
    if (!url && getenv("HK_PC_EA")) {   // hiraku-pc's server, a.b.c.d:port
        snprintf(from_pc, sizeof from_pc, "http://%s/", getenv("HK_PC_EA"));
        url = from_pc;
    }
    if (url) {
        node_t network = property_search(e, NULL, "/ea3/network");
        set_text(e, network, "services", url);
        drop(e, network, "ssl");
        property_node_create(e, network, NODE_BOOL, "ssl", 0);
        say("e-AMUSEMENT at %s", url);
    }
    ea3_boot(ea3_root);
    int r = entry_main();
    say("dll_entry_main -> %d", r);
    return r;
}
