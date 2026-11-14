// SPDX-License-Identifier: GPL-3.0-or-later
// The launcher's change of Windows' power plan while a game runs (launcher/powerplan.h), its steps
// against a Windows made up here: changed and put back, a plan the player chose left alone, a
// launcher that never put it back, a PC with no High performance plan.
#include <QCoreApplication>
#include <QDir>
#include <QSettings>

#include <cstdio>

#include "powerplan.h"

static int g_failed = 0;
#define CHECK(x) \
    do { \
        if (!(x)) g_failed++, fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
    } while (0)

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    QCoreApplication app(argc, argv);
    const QString file = QString::fromLocal8Bit(argv[1]) + "/powerplan-test.ini";
    QFile::remove(file);
    const QString balanced = "381b4222-f694-41f0-9685-ff5bb260df2e", saver = "a1841308-3541-4fab-bc81-f71556f20b4a";
    QString now = balanced;
    bool has_high = true;
    int sets = 0;
    const auto name_of = [&](const QString &g) { return g == balanced ? QString("Balanced") : g == saver ? QString("Power saver") : g == powerplan::kHigh ? QString("High performance") : QString(); };
    powerplan::Api api;
    api.active = [&](QString &guid, QString &name) { guid = now, name = name_of(now); return true; };
    api.set = [&](const QString &guid) {
        if (guid == powerplan::kHigh && !has_high) return false;
        now = guid, sets++;
        return true;
    };
    api.name = name_of;
    {
        QSettings s(file, QSettings::IniFormat);
        // changed for a game, and back
        CHECK(powerplan::raise(s, api) == "power plan: High performance while a game runs (it was Balanced)");
        CHECK(now == powerplan::kHigh && s.value(powerplan::kBefore).toString() == balanced);
        CHECK(powerplan::raise(s, api).isEmpty() && sets == 1);   // the next game of the same sitting: nothing to do
        CHECK(powerplan::restore(s, api) == "power plan: back to Balanced");
        CHECK(now == balanced && !s.contains(powerplan::kBefore));
        CHECK(powerplan::restore(s, api).isEmpty() && sets == 2);   // nothing changed: nothing to put back

        // the player's own High performance: not HIRAKU's to take away
        now = powerplan::kHigh;
        CHECK(powerplan::raise(s, api).isEmpty() && !s.contains(powerplan::kBefore));
        CHECK(powerplan::restore(s, api).isEmpty() && now == powerplan::kHigh);

        // changed by the player while a game ran: left as they set it
        now = balanced;
        powerplan::raise(s, api);
        now = saver;
        CHECK(powerplan::restore(s, api) == "power plan: changed to Power saver meanwhile; left as it is");
        CHECK(now == saver && !s.contains(powerplan::kBefore));

        // a PC with no such plan
        now = balanced, has_high = false;
        CHECK(powerplan::raise(s, api).contains("no High performance plan") && now == balanced && !s.contains(powerplan::kBefore));
        has_high = true;

        // a launcher that never put it back: the plan before is still in the settings' file
        powerplan::raise(s, api);
        CHECK(now == powerplan::kHigh);
    }
    {
        QSettings s(file, QSettings::IniFormat);   // the next start
        CHECK(s.value(powerplan::kBefore).toString() == balanced);
        CHECK(powerplan::restore(s, api) == "power plan: back to Balanced" && now == balanced);
    }
    // where the plan can't be read (not Windows): nothing is done, nothing kept
    {
        QSettings s(file, QSettings::IniFormat);
        CHECK(powerplan::raise(s, powerplan::Api{}).isEmpty() && !s.contains(powerplan::kBefore));
    }
    // this PC's own plan, read only (never changed by a test): on Windows there is one, and its
    // id reads as an id
    if (powerplan::available()) {
        QString guid, name;
        if (powerplan::windowsApi().active(guid, name)) {
            CHECK(guid.size() == powerplan::kHigh.size());
            printf("this Windows' plan: %s (%s)\n", qPrintable(name), qPrintable(guid));
        } else {
            printf("this Windows tells no power plan (Wine doesn't): nothing would be changed here\n");
        }
    } else {
        CHECK(!powerplan::windowsApi().active);
    }
    QFile::remove(file);
    printf(g_failed ? "%d failed\n" : "ok\n", g_failed);
    return g_failed ? 1 : 0;
}
