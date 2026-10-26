// SPDX-License-Identifier: GPL-3.0-or-later
// hiraku-pc32 (PLAN.md section 35): what the loader's pieces share. main.c starts the game,
// files.c lays the mods' folders over its data.
#pragma once
#include <windows.h>

// The loader's log (HK_PC32_LOG's file, or standard error)
void pc32_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
// HK_PC32_TRACE: the game's reads logged (main.c's struct reads and searches, files.c's opens)
int pc32_trace(void);
// The module's import slot (from any DLL) holding TARGET pointed at FN: AVS2's calls are imported
// by ordinal, so found by where they lead. The old target, or NULL when no slot holds it
void *pc32_hook_target(HMODULE m, void *target, void *fn);

// files.c: the mods (HK_PC32_DATA=DIR;DIR...: each a folder laid over the game's data folder, the
// first winning). AVS2's mount, open and stat as libavs-win32 exports them; the game's modules
// that call them (popn22.dll and the texture library's AVS2 shim) are hooked
typedef int (*pc32_mount_fn)(const char *mountpoint, const char *source, const char *fstype, void *data);
typedef int (*pc32_open_fn)(const char *path, int mode, int perm);
typedef int (*pc32_stat_fn)(const char *path, void *st);
void pc32_files_install(HMODULE core, HMODULE game, pc32_mount_fn mount, pc32_open_fn open, pc32_stat_fn stat);
// A mount the game makes (main.c's hook sees each): its source as it is, or the mod's (written in
// buf) when it's a pack inside a pack the mod has; the mount noted for its members
const char *pc32_files_mount(const char *mountpoint, const char *source, const char *fstype, char *buf, size_t n);
