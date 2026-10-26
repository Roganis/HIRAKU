// SPDX-License-Identifier: GPL-3.0-or-later
// hiraku-pc64's start: the cabinet's bootstrap, as spice2x does it (avs/core.cpp, avs/ea3.cpp):
// AVS2's boot with prop/avs-config.xml, the defaults copied into /dev/nvram, the game's DLL
// loaded and hooked, dll_entry_init with the soft ID, ea3_boot with prop/ea3-config.xml, then
// dll_entry_main, which runs the game.
//
//   hiraku-pc64.exe CONTENTS [GAME.dll]     CONTENTS: the game's folder (modules/, prop/, data)
//
// HK_PC64_EA: e-AMUSEMENT's services URL (http://a.b.c.d:port/), in place of ea3-config's (or
// HK_PC_EA's a.b.c.d:port, hiraku-pc's server)
// HK_PC64_LOG: the loader's log file (else stderr); AVS2's own log goes to the same place
// HK_PC64_SAVE: the folder the game runs in (its settings, backup data, logs), the game's own
// folder only read; without it the game runs in its own folder
#include "pc64.h"

#include <stdlib.h>
#include <string.h>

// AVS2 2.17 (the game's avs2-core.dll and avs2-ea3.dll), by their exports' names (spice2x's
// IMPORT_AVS21730 table: the same names in 2.17.4)
typedef void *prop_t;
typedef void *node_t;
typedef int (*avs_reader_t)(uint32_t, void *, size_t);
static int (*avs_boot)(node_t config, void *heap, size_t size, void *reserved, void *log, HANDLE file);
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
#define NODE_S32 6
#define NODE_STR 11
#define NODE_BOOL 52

volatile int pc64_scene = -1;

static FILE *g_reading;
static int read_file(uint32_t context, void *to, size_t n) {
    (void)context;
    return (int)fread(to, 1, n, g_reading);
}

// AVS2's log: the loader's, and the game's scenes from it (the tests' script waits for them)
static BOOL avs_log(const char *buf, DWORD size, HANDLE file) {
    (void)file;
    for (DWORD i = 0; i + 20 < size; i++)
        if (!memcmp(buf + i, "CSequence::Attach: ", 19)) pc64_scene = atoi(buf + i + 19);
    FILE *f = pc64_log_file();
    fwrite(buf, 1, size, f);
    fflush(f);
    return TRUE;
}

static void *need(HMODULE m, const char *what, const char *name) {
    void *p = (void *)GetProcAddress(m, name);
    if (!p) {
        pc64_log("%s has no %s: not the AVS2 this loader knows (2.17)", what, name);
        ExitProcess(2);
    }
    return p;
}

static prop_t read_property(FILE *in, const char *what, size_t extra) {
    g_reading = in;
    int size = property_read_query_memsize(read_file, 0, NULL, NULL);
    if (size <= 0) {
        pc64_log("%s: not a property (%d)", what, size);
        ExitProcess(2);
    }
    size += (int)extra;   // room for the nodes the loader adds: AVS2 doesn't grow a property
    rewind(in);
    prop_t p = property_create(23, malloc((size_t)size), (uint32_t)size);
    if (!p || !property_insert_read(p, NULL, read_file, 0)) {
        pc64_log("%s: can't read it", what);
        ExitProcess(2);
    }
    return p;
}

static prop_t read_property_file(const char *path, size_t extra) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        pc64_log("no %s", path);
        ExitProcess(2);
    }
    prop_t p = read_property(f, path, extra);
    fclose(f);
    return p;
}

static void drop(prop_t p, node_t n, const char *path) {
    node_t x = property_search(p, n, path);
    if (x) property_node_remove(x);
}

static void text(prop_t p, const char *path, char *out, size_t n) {
    out[0] = 0;
    property_node_refer(p, NULL, path, NODE_STR, out, (uint32_t)n);
}

// FOLDER (prop/) from the game's folder into the save folder, the files the save folder hasn't yet
static void copy_tree(const char *from_root, const char *folder) {
    char pattern[MAX_PATH * 2];
    snprintf(pattern, sizeof pattern, "%s\\%s\\*", from_root, folder);
    CreateDirectoryA(folder, NULL);
    WIN32_FIND_DATAA f;
    HANDLE h = FindFirstFileA(pattern, &f);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(f.cFileName, ".") || !strcmp(f.cFileName, "..")) continue;
        char rel[MAX_PATH], from[MAX_PATH * 2];
        snprintf(rel, sizeof rel, "%s\\%s", folder, f.cFileName);
        snprintf(from, sizeof from, "%s\\%s", from_root, rel);
        if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) copy_tree(from_root, rel);
        else CopyFileA(from, rel, TRUE);   // TRUE: what's there stays
    } while (FindNextFileA(h, &f));
    FindClose(h);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: hiraku-pc64 CONTENTS [GAME.dll]\n");
        return 2;
    }
    const char *game_dll = argc > 2 ? argv[2] : "popn.dll";
    char contents[MAX_PATH], modules[MAX_PATH + 16];
    GetFullPathNameA(argv[1], sizeof contents, contents, NULL);
    snprintf(modules, sizeof modules, "%s\\modules", contents);
    if (GetFileAttributesA(modules) == INVALID_FILE_ATTRIBUTES) {
        pc64_log("no %s: not the game's folder", modules);
        return 2;
    }
    SetDllDirectoryA(modules);
    // HK_PC64_SAVE: the run in that folder, the game's own only read (files.c)
    const char *save = getenv("HK_PC64_SAVE");
    if (save) {
        CreateDirectoryA(save, NULL);
        if (!SetCurrentDirectoryA(save)) {
            pc64_log("can't use %s as the save folder", save);
            return 2;
        }
        copy_tree(contents, "prop");
    } else if (!SetCurrentDirectoryA(contents)) {
        pc64_log("can't run in %s", contents);
        return 2;
    }
    CreateDirectoryA("dev", NULL);
    CreateDirectoryA("dev\\nvram", NULL);
    CreateDirectoryA("dev\\raw", NULL);
    pc64_faults_install();

    HMODULE core = LoadLibraryA("avs2-core.dll"), ea3 = core ? LoadLibraryA("avs2-ea3.dll") : NULL;
    if (!core || !ea3) {
        pc64_log("AVS2 didn't load from %s (%lu)", modules, GetLastError());
        return 2;
    }
    avs_boot = need(core, "avs2-core", "XCgsqzn0000129");
    property_create = need(core, "avs2-core", "XCgsqzn0000090");
    property_insert_read = need(core, "avs2-core", "XCgsqzn0000094");
    property_read_query_memsize = need(core, "avs2-core", "XCgsqzn00000b0");
    property_search = need(core, "avs2-core", "XCgsqzn00000a1");
    property_node_create = need(core, "avs2-core", "XCgsqzn00000a2");
    property_node_remove = need(core, "avs2-core", "XCgsqzn00000a3");
    property_node_refer = need(core, "avs2-core", "XCgsqzn00000af");
    std_setenv = need(core, "avs2-core", "XCgsqzn00000d4");
    avs_fs_copy = need(core, "avs2-core", "XCgsqzn0000065");
    avs_fs_lstat = need(core, "avs2-core", "XCgsqzn0000063");
    ea3_boot = need(ea3, "avs2-ea3", "XEmdwapa000024");
    pc64_net_install(core);

    // AVS2: Japan's time, the PC's own clock (not e-AMUSEMENT's), its log here
    prop_t config = read_property_file("prop\\avs-config.xml", 0x10000);
    node_t root = property_search(config, NULL, "/config");
    drop(config, root, "/time/gmt_offset");
    property_node_create(config, root, NODE_S32, "/time/gmt_offset", 9 * 3600);
    drop(config, root, "/sntp/ea_on");
    property_node_create(config, root, NODE_BOOL, "/sntp/ea_on", 0);
    drop(config, root, "/log/level");
    property_node_create(config, root, NODE_STR, "/log/level", "misc");
    const size_t heap = 0x20000000;   // spice2x's for this cabinet
    avs_boot(root, malloc(heap), heap, NULL, (void *)avs_log, NULL);

    // The cabinet's settings start as its defaults
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
        pc64_log("%s didn't load (%lu)", game_dll, GetLastError());
        return 2;
    }
    pc64_files_install(game, save ? contents : NULL);
    pc64_display_install(game);
    pc64_board_install(game);
    pc64_touch_install(game);
    pc64_sound_install(game);
    pc64_trace_install(game);
    pc64_peek_install(game);
    pc64_trace_d3dx_install(game);
    pc64_crt_install(game);
    pc64_songs_install(game);
    int (*entry_init)(char *, node_t) = need(game, game_dll, "dll_entry_init");
    int (*entry_main)(void) = need(game, game_dll, "dll_entry_main");

    // ea3: the IDs the cabinet's bootstrap sets in AVS2's environment (spice2x's ea3.cpp)
    prop_t e = read_property_file("prop\\ea3-config.xml", 0x40000);
    char model[8], dest[4], spec[4], rev[4], ext[16], pcbid[32], softid[32], hardid[32], account[32];
    text(e, "/ea3/soft/model", model, sizeof model);
    text(e, "/ea3/soft/dest", dest, sizeof dest);
    text(e, "/ea3/soft/spec", spec, sizeof spec);
    text(e, "/ea3/soft/rev", rev, sizeof rev);
    text(e, "/ea3/soft/ext", ext, sizeof ext);
    text(e, "/ea3/id/pcbid", pcbid, sizeof pcbid);
    text(e, "/ea3/id/softid", softid, sizeof softid);
    text(e, "/ea3/id/hardid", hardid, sizeof hardid);
    text(e, "/ea3/id/accountid", account, sizeof account);
    if (!hardid[0]) property_node_create(e, NULL, NODE_STR, "/ea3/id/hardid", strcpy(hardid, "0100DEADBEEF"));
    if (!account[0]) property_node_create(e, NULL, NODE_STR, "/ea3/id/accountid", strcpy(account, "012018008135"));
    char security[32], soft_id[64], init_code[64];
    snprintf(security, sizeof security, "G*%s%s%s%s", model, dest, spec, rev);
    snprintf(soft_id, sizeof soft_id, "%s:%s:%s:%s:%s", model, dest, spec, rev, ext);
    snprintf(init_code, sizeof init_code, "%s%s%s%s%s", model, dest, spec, rev, ext);
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

    // dll_entry_init(the soft ID, its app-config's /param: an empty one)
    static const char app_config[] = "<?xml version=\"1.0\"?><param></param>";
    FILE *app = tmpfile();
    fwrite(app_config, 1, sizeof app_config - 1, app);
    rewind(app);
    prop_t param = read_property(app, "app-config", 0x1000);
    fclose(app);
    pc64_log("%s: dll_entry_init(%s)", game_dll, init_code);
    if (!entry_init(init_code, property_search(param, NULL, "/param"))) {
        pc64_log("dll_entry_init failed");
        return 2;
    }
    char from_pc[64];
    const char *url = getenv("HK_PC64_EA");
    if (!url && getenv("HK_PC_EA")) {   // hiraku-pc's server, a.b.c.d:port
        snprintf(from_pc, sizeof from_pc, "http://%s/", getenv("HK_PC_EA"));
        url = from_pc;
    }
    if (url) {
        node_t network = property_search(e, NULL, "/ea3/network");
        drop(e, network, "services");
        property_node_create(e, network, NODE_STR, "services", url);
        pc64_log("e-AMUSEMENT at %s", url);
    }
    ea3_boot(property_search(e, NULL, "/ea3"));
    pc64_script_start();
    int r = entry_main();
    pc64_log("dll_entry_main -> %d", r);
    return r;
}
