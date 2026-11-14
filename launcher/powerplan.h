// SPDX-License-Identifier: GPL-3.0-or-later
// Windows' power plan while a game runs (Settings > Audio, off by default). The Balanced plan lets
// the processors slow down while a game waits for its next frame, so each frame's short burst of
// work starts slow and some come late (the run log says so: runtime/host/window.cpp,
// cpu_speed_describe). With the setting on, the launcher changes to the High performance plan when
// a game starts and back to the player's own when the launcher's window is back.
//
// The plan before is kept in the settings while it is changed, so a launcher that never got to put
// it back (a crash, a power cut) does at its next start. A plan the player changed in between is
// left as they set it.
//
// The steps are apart from Windows' calls (Api), so they are tested anywhere
// (tests/launcher/test_powerplan.cpp).
#pragma once

#include <QSettings>
#include <QString>

#include <functional>

namespace powerplan {

// Windows' own name for High performance (GUID_MIN_POWER_SAVINGS)
inline const QString kHigh = QStringLiteral("8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c");
inline const char *const kBefore = "system/planBefore";   // the plan to go back to, while changed

struct Api {
    std::function<bool(QString &guid, QString &name)> active;   // the plan in use now
    std::function<bool(const QString &guid)> set;               // false: Windows refused (no such plan)
    std::function<QString(const QString &guid)> name;           // a plan's name ("" if unknown)
};

// To High performance; what to say in the log ("" if there was nothing to do)
inline QString raise(QSettings &s, const Api &api) {
    QString now, name;
    if (!api.active || !api.active(now, name)) return {};
    if (now.compare(kHigh, Qt::CaseInsensitive) == 0) return {};   // the player's own choice, or already changed
    if (!s.contains(kBefore)) s.setValue(kBefore, now), s.sync();  // (before the change: a crash right after still finds it)
    if (!api.set(kHigh)) {
        s.remove(kBefore);
        return "power plan: Windows has no High performance plan to change to on this PC; it stays " + name;
    }
    return "power plan: High performance while a game runs (it was " + name + ")";
}

// Back to the plan before; what to say in the log ("" if nothing was changed)
inline QString restore(QSettings &s, const Api &api) {
    const QString before = s.value(kBefore).toString();
    if (before.isEmpty()) return {};
    s.remove(kBefore);
    s.sync();
    QString now, name;
    if (!api.active || !api.active(now, name)) return {};
    if (now.compare(kHigh, Qt::CaseInsensitive) != 0) return "power plan: changed to " + name + " meanwhile; left as it is";
    if (!api.set(before)) return "power plan: couldn't go back to the plan before (" + before + ")";
    const QString was = api.name ? api.name(before) : QString();
    return "power plan: back to " + (was.isEmpty() ? before : was);
}

// Windows' own calls (powerplan.cpp); elsewhere there is no plan to read, and nothing is done
Api windowsApi();
bool available();   // Windows

}  // namespace powerplan
