// SPDX-License-Identifier: GPL-3.0-or-later
// A mod's settings, the launcher's side (docs/MODS.md, "Hooks, settings and actions"): its
// [[setting]]s and [[action]]s read from its mod.toml by the loader's own reader
// (runtime/host/modsettings.h), their values from the file the game keeps them in
// (cfg::modSettingsFile), each shown as a tick box (0/1), a list (named values) or a number, and
// written back with the file's other lines (the actions' bindings, a setting the mod no longer has)
// kept. No widgets here: the panel (modsettingsdialog.h) shows it, the tests check it.
#pragma once

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QSaveFile>
#include <QString>
#include <QStringList>

#include <stdexcept>
#include <vector>

#include "host/modsettings.h"

class ModSettings {
public:
    enum class Kind { tick, list, number };
    struct Row {
        hk::modsettings::Setting setting;
        Kind kind = Kind::number;
        int64_t value = 0;   // as it is now (kept, else the mod's default)
    };

    // The mod in `modDir` and its settings file. false (why in err): the mod.toml can't be read as
    // the game would read it; a mod without one has no settings (true, no rows)
    bool load(const QString &modDir, const QString &settingsFile, QString *err = nullptr) {
        m_rows.clear();
        m_actions.clear();
        m_file = settingsFile;
        m_text.clear();
        QFile toml(modDir + "/mod.toml");
        if (toml.open(QIODevice::ReadOnly)) {
            try {
                const hk::modsettings::Defs d =
                    hk::modsettings::read(toml.readAll().toStdString(), (QFileInfo(modDir).fileName() + "/mod.toml").toStdString());
                for (const hk::modsettings::Setting &s : d.settings) {
                    Row r;
                    r.setting = s;
                    r.kind = !s.choices.empty() ? Kind::list : s.tick() ? Kind::tick : Kind::number;
                    r.value = s.dflt;
                    m_rows.push_back(std::move(r));
                }
                m_actions = d.actions;
            } catch (const std::exception &e) {
                if (err) *err = QString::fromStdString(e.what());
                return false;
            }
        }
        QFile kept(settingsFile);
        if (kept.open(QIODevice::ReadOnly)) m_text = kept.readAll().toStdString();
        const auto kv = hk::modsettings::values(m_text);
        for (Row &r : m_rows) {
            int64_t v;
            if (auto it = kv.find(r.setting.name); it != kv.end() && hk::modsettings::value_of(r.setting, it->second, v))
                r.value = std::clamp(v, r.setting.min, r.setting.max);   // (the game takes it so)
        }
        for (hk::modsettings::Action &a : m_actions)
            if (auto it = kv.find("action." + a.name); it != kv.end() && !it->second.empty()) a.binding = it->second;
        return true;
    }

    const std::vector<Row> &rows() const { return m_rows; }
    bool empty() const { return m_rows.empty(); }
    // a value set (clamped to the setting's ends)
    void set(size_t i, int64_t v) {
        if (i < m_rows.size()) m_rows[i].value = std::clamp(v, m_rows[i].setting.min, m_rows[i].setting.max);
    }
    void defaults() {
        for (Row &r : m_rows) r.value = r.setting.dflt;
    }
    static QString shown(const Row &r, int64_t v) {
        if (r.kind == Kind::tick) return v ? "on" : "off";
        return QString::fromStdString(hk::modsettings::shown(r.setting, v));
    }
    // "Backing track timing: PS2, Lane cover: 120" (for the mod's row)
    QString summary() const {
        QStringList parts;
        for (const Row &r : m_rows) parts << QString::fromStdString(r.setting.label) + ": " + shown(r, r.value);
        return parts.join(", ");
    }
    // the actions' keys, read-only: "Keypad 8: later (Judging (ms) up by 5)"
    QStringList bindings() const {
        QStringList out;
        for (const hk::modsettings::Action &a : m_actions) {
            QString key = QString::fromStdString(a.binding);
            if (key.startsWith("key:")) key = key.mid(4);
            QString does = QString::fromStdString(a.what);
            for (const Row &r : m_rows)
                if (r.setting.name == a.setting) {
                    const QString label = QString::fromStdString(r.setting.label);
                    does = a.what == "toggle" ? label + ", " + shown(r, r.setting.min) + " / " + shown(r, r.setting.max)
                                              : label + " " + does + (r.setting.step != 1 ? " by " + QString::number(r.setting.step) : "");
                }
            out << key + ": " + QString::fromStdString(a.label) + " (" + does + ")";
        }
        return out;
    }
    // the settings file's text with the values as they are now
    std::string written() const {
        std::vector<std::pair<std::string, std::string>> set;
        for (const Row &r : m_rows) set.emplace_back(r.setting.name, std::to_string(r.value));
        return hk::modsettings::written(m_text, set);
    }
    bool save(QString *err = nullptr) {
        QDir().mkpath(QFileInfo(m_file).path());
        QSaveFile f(m_file);
        const std::string text = written();
        if (!f.open(QIODevice::WriteOnly) || f.write(text.data(), (qint64)text.size()) != (qint64)text.size() || !f.commit()) {
            if (err) *err = m_file + ": " + f.errorString();
            return false;
        }
        m_text = text;
        return true;
    }

private:
    std::vector<Row> m_rows;
    std::vector<hk::modsettings::Action> m_actions;
    QString m_file;
    std::string m_text;   // the file as read (its other lines are kept)
};
