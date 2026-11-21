// SPDX-License-Identifier: GPL-3.0-or-later
// The mods' files in a PC game's process (runtime/pc/cabinet.c; docs/MODS.md, "The PC games"), as
// a game meets them: this program loads HIRAKU's board DLL beside it, which hooks its imports, then
// opens, looks at and lists files by the names a game gives (its data folder as "..\data"). A file a
// mod replaces, one it adds and a folder it adds must agree in CreateFileA/W, GetFileAttributesA/ExA,
// FindFirstFileA for a name and the listings (FindFirstFileA/FindNextFileA/FindClose with a
// wildcard), each name once, however the path is written ("\\", "/", ".\", "x\..", a "\" at the
// end, any case). tests/pc/test_modfiles.sh makes the folders and the list (HK_PC_FILES).
//
//   test_modfiles mods        the mods' files as above
//   test_modfiles plain       no mods (no HK_PC_FILES): the game's files only, as before
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int failed;
static void check(int ok, const char *what) {
    printf("%s%s\n", ok ? "ok    " : "FAIL  ", what);
    failed += !ok;
}

// the file's text ("" when it can't be opened)
static const char *text(const char *p) {
    static char b[256];
    b[0] = 0;
    HANDLE h = CreateFileA(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return b;
    DWORD n = 0;
    ReadFile(h, b, sizeof b - 1, &n, NULL);
    b[n] = 0;
    CloseHandle(h);
    return b;
}
static const char *wtext(const WCHAR *p) {
    static char b[256];
    b[0] = 0;
    HANDLE h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return b;
    DWORD n = 0;
    ReadFile(h, b, sizeof b - 1, &n, NULL);
    b[n] = 0;
    CloseHandle(h);
    return b;
}

// a listing's names, sorted by the order given in want ("." and ".." left out), as "a b c"; the
// size of `sized` in *size
static const char *listed(const char *pattern, const char *sized, DWORD *size) {
    static char out[1024];
    out[0] = 0;
    WIN32_FIND_DATAA d;
    HANDLE h = FindFirstFileA(pattern, &d);
    if (h == INVALID_HANDLE_VALUE) return "(none)";
    char names[64][MAX_PATH];
    int n = 0;
    do {
        if (!strcmp(d.cFileName, ".") || !strcmp(d.cFileName, "..")) continue;
        if (sized && !_stricmp(d.cFileName, sized)) *size = d.nFileSizeLow;
        if ((d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && n < 64) snprintf(names[n++], MAX_PATH, "%s/", d.cFileName);
        else if (n < 64) snprintf(names[n++], MAX_PATH, "%s", d.cFileName);
    } while (FindNextFileA(h, &d));
    const DWORD end = GetLastError();
    FindClose(h);
    for (int i = 0; i < n; i++)   // (sorted, any case)
        for (int j = i + 1; j < n; j++)
            if (_stricmp(names[j], names[i]) < 0) {
                char t[MAX_PATH];
                strcpy(t, names[i]), strcpy(names[i], names[j]), strcpy(names[j], t);
            }
    for (int i = 0; i < n; i++) snprintf(out + strlen(out), sizeof out - strlen(out), "%s%s", i ? " " : "", names[i]);
    if (end != ERROR_NO_MORE_FILES) snprintf(out + strlen(out), sizeof out - strlen(out), " (ended with %lu)", end);
    return out;
}

static void same(const char *got, const char *want, const char *what) {
    char line[1024];
    snprintf(line, sizeof line, "%s: \"%s\"%s%s%s", what, got, strcmp(got, want) ? " (want \"" : "", strcmp(got, want) ? want : "",
             strcmp(got, want) ? "\")" : "");
    check(!strcmp(got, want), line);
}

int main(int argc, char **argv) {
    const int mods = argc > 1 && !strcmp(argv[1], "mods");
    if (!LoadLibraryA("ezusb.dll")) {
        printf("FAIL  no ezusb.dll beside the program (%lu)\n", GetLastError());
        return 1;
    }
    DWORD size = 0;
    if (!mods) {
        same(text("..\\data\\a.txt"), "game a", "no mods: the game's file");
        same(listed("..\\data\\sub\\*.*", NULL, NULL), "b.txt c.txt", "no mods: the game's listing");
        check(GetFileAttributesA("..\\data\\newdir") == INVALID_FILE_ATTRIBUTES, "no mods: a mod's folder isn't the game's");
        printf("%s\n", failed ? "FAILED" : "all ok");
        return failed != 0;
    }

    // opened, by any spelling of the path
    same(text("..\\data\\a.txt"), "mod A a", "a replaced file");
    same(text("..\\data\\sub\\new1.txt"), "A new1", "an added file, the first mod's of two");
    same(text("..\\data\\sub\\\\new1.txt"), "A new1", "... with a doubled \\ (as IIDX opens ..\\data\\sd_data\\\\osprey.bin)");
    same(text("..\\DATA/sub/./new2.txt"), "B new2", "... in another case, with / and .\\, the second mod's");
    same(text("..\\data\\sub\\..\\newdir\\x.txt"), "A x", "a file in an added folder, through x\\..");
    same(text("..//data\\newdir\\deep\\y.txt"), "A y", "a folder in an added folder");
    same(text("..\\data\\newdir\\z.txt"), "B z", "the second mod's file in the same added folder");
    same(text("..\\data\\sub\\b.txt"), "game b", "the game's own file beside the added ones");
    same(text("..\\data\\newdir\\none.txt"), "", "a file no mod has in an added folder: not there");
    same(wtext(L"..\\data\\newdir\\x.txt"), "A x", "CreateFileW: an added file");
    WCHAR w[MAX_PATH];
    char back[MAX_PATH] = "";
    if (MultiByteToWideChar(CP_ACP, 0, "..\\data\\movie\\0001.4", -1, w, MAX_PATH) > 0)
        WideCharToMultiByte(CP_ACP, 0, w, -1, back, sizeof back, NULL, NULL);
    check(strstr(back, "modA") && strstr(back, "0001.4"), "a name turned to UTF-16 (for DirectShow's movies): the mod's file's");

    // looked at
    DWORD a = GetFileAttributesA("..\\data\\newdir");
    check(a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY), "GetFileAttributesA: an added folder is a folder");
    a = GetFileAttributesA("..\\data\\newdir\\deep\\");
    check(a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY), "... a folder in it, written with a \\ at the end");
    a = GetFileAttributesA("..\\data\\sub\\new2.txt");
    check(a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY), "... an added file is a file");
    WIN32_FILE_ATTRIBUTE_DATA fa;
    check(GetFileAttributesExA("..\\data\\a.txt", GetFileExInfoStandard, &fa) && fa.nFileSizeLow == 7,
          "GetFileAttributesExA: a replaced file has the mod's file's size (7)");
    WIN32_FIND_DATAA d;
    HANDLE h = FindFirstFileA("..\\data\\sub\\new1.txt", &d);
    check(h != INVALID_HANDLE_VALUE && !_stricmp(d.cFileName, "new1.txt") && d.nFileSizeLow == 6, "FindFirstFileA for a name: an added file");
    if (h != INVALID_HANDLE_VALUE) FindClose(h);
    h = FindFirstFileA("..\\data\\newdir", &d);
    check(h != INVALID_HANDLE_VALUE && (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY), "... an added folder");
    if (h != INVALID_HANDLE_VALUE) FindClose(h);

    // listed: each name once
    same(listed("..\\data\\sub\\*.*", NULL, NULL), "b.txt c.txt new1.txt new2.txt", "a game's folder with added files");
    same(listed("..\\data\\sub\\\\*", NULL, NULL), "b.txt c.txt new1.txt new2.txt", "... by a pattern after a doubled \\");
    same(listed("..\\data\\sub\\new*.txt", NULL, NULL), "new1.txt new2.txt", "... a pattern the added names alone fit");
    same(listed("..\\data\\newdir\\*", NULL, NULL), "deep/ x.txt z.txt", "an added folder, both mods' files and its folder");
    same(listed("..\\data\\newdir\\deep\\*.*", NULL, NULL), "y.txt", "a folder in an added folder");
    same(listed("..\\data\\*", "a.txt", &size), "a.txt movie/ newdir/ sub/", "the data folder: added folders once, the game's once");
    check(size == 7, "... a replaced file there with the mod's file's size (7)");
    same(listed("..\\data\\nothere\\*", NULL, NULL), "(none)", "a folder no one has: no listing");

    printf("%s\n", failed ? "FAILED" : "all ok");
    return failed != 0;
}
