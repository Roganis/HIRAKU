// SPDX-License-Identifier: GPL-3.0-or-later
// Windows' side of launcher/powerplan.h (apart from the header: windows.h's names, `interface`
// among them, aren't for every file that needs the steps)
#include "powerplan.h"

#ifdef _WIN32
#include <windows.h>
#include <powrprof.h>

#include <cstdio>

namespace powerplan {

namespace {

QString guidText(const GUID &g) {
    return QString::asprintf("%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", (unsigned long)g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1],
                             g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
}
bool guidFrom(const QString &text, GUID &g) {
    unsigned long a = 0;
    unsigned b = 0, c = 0, d[8] = {};
    if (sscanf(text.toLatin1().constData(), "%8lx-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x", &a, &b, &c, &d[0], &d[1], &d[2], &d[3], &d[4], &d[5], &d[6], &d[7]) != 11)
        return false;
    g.Data1 = a, g.Data2 = (unsigned short)b, g.Data3 = (unsigned short)c;
    for (int k = 0; k < 8; k++) g.Data4[k] = (unsigned char)d[k];
    return true;
}
QString planName(const QString &guid) {
    GUID g;
    wchar_t name[256] = {};
    DWORD size = sizeof name - sizeof name[0];
    if (!guidFrom(guid, g) || PowerReadFriendlyName(nullptr, &g, nullptr, nullptr, (UCHAR *)name, &size) != ERROR_SUCCESS) return {};
    return QString::fromWCharArray(name);
}
}  // namespace

Api windowsApi() {
    Api api;
    api.active = [](QString &guid, QString &name) {
        GUID *scheme = nullptr;
        if (PowerGetActiveScheme(nullptr, &scheme) != ERROR_SUCCESS || !scheme) return false;
        guid = guidText(*scheme);
        LocalFree(scheme);
        name = planName(guid);
        if (name.isEmpty()) name = guid;
        return true;
    };
    api.set = [](const QString &guid) {
        GUID g;
        return guidFrom(guid, g) && PowerSetActiveScheme(nullptr, &g) == ERROR_SUCCESS;
    };
    api.name = planName;
    return api;
}
bool available() { return true; }

}  // namespace powerplan

#else

namespace powerplan {
Api windowsApi() { return {}; }
bool available() { return false; }
}  // namespace powerplan

#endif
