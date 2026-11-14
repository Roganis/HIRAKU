// SPDX-License-Identifier: GPL-3.0-or-later
// HIRAKU.exe, at the top of the Windows package (tools/package-win.sh): starts the launcher,
// bin\hiraku.exe, so that it is the first thing seen in the folder, with HIRAKU's icon. A
// shortcut (.lnk) can't do this: it names the program by a whole path, which moving or unpacking
// the folder anywhere else breaks. What it is given goes on to the launcher; it ends at once.
#include <windows.h>
#include <wchar.h>

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show) {
    (void)inst, (void)prev, (void)cmd, (void)show;
    wchar_t dir[32768];
    DWORD n = GetModuleFileNameW(NULL, dir, 32768 - 32);
    if (n == 0 || n >= 32768 - 32) return 1;
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (!slash) return 1;
    wcscpy(slash, L"\\bin");
    static wchar_t exe[32768], line[32768];
    _snwprintf(exe, 32768, L"%ls\\hiraku.exe", dir);
    // the arguments after this program's own name (quoted or not)
    const wchar_t *args = GetCommandLineW();
    if (*args == L'"') {
        args++;
        while (*args && *args != L'"') args++;
        if (*args) args++;
    } else
        while (*args && *args != L' ' && *args != L'\t') args++;
    while (*args == L' ' || *args == L'\t') args++;
    _snwprintf(line, 32768, L"\"%ls\"%ls%ls", exe, *args ? L" " : L"", args);
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    // (in bin\, as when bin\hiraku.exe is opened itself)
    if (!CreateProcessW(exe, line, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
        wchar_t msg[33000];
        _snwprintf(msg, 33000, L"The launcher isn't where it should be:\n%ls\n\nThis folder may be incomplete: "
                               L"unpack the whole HIRAKU package again.", exe);
        MessageBoxW(NULL, msg, L"HIRAKU", MB_OK | MB_ICONERROR);
        return 1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
