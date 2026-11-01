// SPDX-License-Identifier: GPL-3.0-or-later
// Inputs as the game will see them: SDL joysticks (polled on a timer) and key
// names in SDL's spelling, so that a captured binding is exactly what the
// runtime's bindings parser (runtime/host/input.cpp) reads back.
#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVector>

#include "shell/cabinet.h"

class QKeyEvent;

// "key:Z", or "" when the key cannot be bound (unknown, or '#' which starts a comment)
QString bindingForKey(const QKeyEvent *e);
// "Z", "DAO FPS: button 3", "Pad 0: axis 1+", "Pad 1: hat 0 up", "DAO FPS: axis 0 (reversed, ×1.5)"
QString describeBinding(const QString &b);
// One spelling per source, so bindings compare as strings ("key:," -> "key:Comma",
// "key:z" -> "key:Z"); unparsable keys are returned unchanged
QString normalizeBinding(const QString &b);

// A binding's parts: its source device ("key", "joy0", "joy[DAO FPS]") and the rest ("Z", "button3",
// "axis0*-1.5"); bindingWith() puts them together again
QString bindingPad(const QString &b);
QString bindingRest(const QString &b);
QString bindingWith(const QString &pad, const QString &rest);
// The input itself, whichever way the binding names it: a controller plugged in as joyN, without
// an axis's speed ("joy[DAO FPS]:axis0*2" -> "joy0:axis0"), so two bindings of one input compare equal
QString canonicalBinding(const QString &b);
// An analog axis binding's speed (1: as is; negative: reversed), and the binding with another one
double axisScale(const QString &b);
QString withAxisScale(const QString &b, double scale);

// One SDL joystick per index, in the order a game started now would number them (joy0, joy1, ...)
class Joysticks : public QObject {
    Q_OBJECT
public:
    static Joysticks &instance();
    QStringList names() const;          // "joy0: DAO FPS (14 buttons, 2 axes)"
    QString pad(int joy) const;         // as bindings name it: "joy[DAO FPS]", "joy[DAO FPS@2]"
    QString label(int joy) const;       // for people: "DAO FPS", "DAO FPS (2)"
    int indexOf(const QString &pad) const;   // "joy0" or "joy[DAO FPS]" -> 0, or -1 (not plugged in)
    int count() const { return (int)m_joys.size(); }
    int axisValue(int joy, int axis) const;
    // the axis has reported a value since it was opened: until then SDL says 0, whatever it really
    // reads (some USB controllers report only once something changes)
    bool axisKnown(int joy, int axis) const { return m_known.contains(QString("%1:%2").arg(joy).arg(axis)); }
    int axisCount(int joy) const;
signals:
    void devicesChanged();
    void button(int joy, int button, bool down);
    void axis(int joy, int axis, int value);
    void hat(int joy, int hat, int value);   // SDL_HAT_* bits
private:
    Joysticks();
    void poll();
    void reopen();
    QTimer m_timer;
    QVector<void *> m_joys;             // SDL_Joystick *
    QStringList m_pads;                 // pad() of each
    QSet<QString> m_known;              // "joy:axis" that have reported (axisKnown)
    // as the game takes them (runtime/host/input.cpp first_report): two or more buttons or hat
    // directions held at a controller's first report rest (a PS2 BEMANI controller holds some for
    // good) and aren't pressed until released once
    QSet<int> m_seen;                   // joysticks that have reported
    QSet<QString> m_buttonRest;         // "joy:button"
    QHash<QString, int> m_hatRest;      // "joy:hat" -> SDL hat bits
    void firstReport(int joy);
};
