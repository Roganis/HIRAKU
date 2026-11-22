// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Where HIRAKU keeps what is the user's: the saves (NVRAM, e-AMUSEMENT data, a Python game's
// run files) and the launcher's settings (controls, lights, picture). A HIRAKU folder holding a
// folder named `user` is a portable install: all of it lives in there, and the whole folder
// moves (or goes on a USB stick) as one. Otherwise the XDG folders.
//
//   portable           not portable
//   <root>/user/data   $XDG_DATA_HOME/hiraku   (~/.local/share/hiraku; on Windows %LOCALAPPDATA%\hiraku,
//                                                as the launcher's Qt has it)
//   <root>/user/config $XDG_CONFIG_HOME/hiraku (~/.config/hiraku; the launcher's)
//
// The launcher (launcher/config.cpp) and tools/unlock.py follow the same rule.

#include <sys/stat.h>

#include <cstdlib>
#include <string>

namespace hk {

inline bool portable(const std::string &root) {
    struct stat st;
#ifdef _MSC_VER   // no S_ISDIR in MSVC's C library
    return !root.empty() && stat((root + "/user").c_str(), &st) == 0 && (st.st_mode & _S_IFMT) == _S_IFDIR;
#else
    return !root.empty() && stat((root + "/user").c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

// the saves' folder: <it>/<game> is a game's
inline std::string user_data_dir(const std::string &root) {
    if (portable(root)) return root + "/user/data";
#ifdef _WIN32
    if (const char *appdata = getenv("LOCALAPPDATA"); appdata && *appdata) return std::string(appdata) + "/hiraku";
#endif
    const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    return (xdg && *xdg ? std::string(xdg) : std::string(home ? home : ".") + "/.local/share") + "/hiraku";
}

// the launcher's settings' folder (its launcher.ini: QSettings' GenericConfigLocation + /hiraku)
inline std::string user_config_dir(const std::string &root) {
    if (portable(root)) return root + "/user/config";
#ifdef _WIN32
    if (const char *a = getenv("LOCALAPPDATA"); a && *a) return std::string(a) + "/hiraku";
#endif
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    return (xdg && *xdg ? std::string(xdg) : std::string(home ? home : ".") + "/.config") + "/hiraku";
}

}  // namespace hk
