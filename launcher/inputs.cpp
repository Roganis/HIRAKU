// SPDX-License-Identifier: GPL-3.0-or-later
#include "inputs.h"

#include <QKeyEvent>
#include <QRegularExpression>

#include <SDL3/SDL.h>

#include <cmath>

namespace {

// The runtime spells these by name: ',' separates bindings (runtime/host/input.cpp)
const struct { const char *alias, *key; } kAliases[] = {
    {"Comma", ","}, {"Period", "."}, {"Slash", "/"}, {"Semicolon", ";"}, {"Minus", "-"}, {"Equals", "="}};

SDL_Keycode keycodeOf(QString name) {
    for (auto &a : kAliases)
        if (name == a.alias) name = a.key;
    return SDL_GetKeyFromName(name.toUtf8().constData());
}

QString nameOf(SDL_Keycode k) {
    QString n = QString::fromUtf8(SDL_GetKeyName(k));
    for (auto &a : kAliases)
        if (n == a.key) return a.alias;
    return n;
}

// X11 / Wayland keycodes (evdev + 8) for keys Qt does not tell apart by Qt::Key
QString byScanCode(quint32 sc) {
    switch (sc) {
    case 50: return "Left Shift";
    case 62: return "Right Shift";
    case 37: return "Left Ctrl";
    case 105: return "Right Ctrl";
    case 64: return "Left Alt";
    case 108: return "Right Alt";
    case 133: return "Left GUI";
    case 134: return "Right GUI";
    case 79: return "Keypad 7";
    case 80: return "Keypad 8";
    case 81: return "Keypad 9";
    case 82: return "Keypad -";
    case 83: return "Keypad 4";
    case 84: return "Keypad 5";
    case 85: return "Keypad 6";
    case 86: return "Keypad +";
    case 87: return "Keypad 1";
    case 88: return "Keypad 2";
    case 89: return "Keypad 3";
    case 90: return "Keypad 0";
    case 91: return "Keypad .";
    case 104: return "Keypad Enter";
    case 106: return "Keypad /";
    case 63: return "Keypad *";
    default: return {};
    }
}

QString byQtKey(int k) {
    if (k >= Qt::Key_A && k <= Qt::Key_Z) return QString(QChar(k));
    if (k >= Qt::Key_0 && k <= Qt::Key_9) return QString(QChar(k));
    if (k >= Qt::Key_F1 && k <= Qt::Key_F24) return QString("F%1").arg(k - Qt::Key_F1 + 1);
    switch (k) {
    case Qt::Key_Space: return "Space";
    case Qt::Key_Return: case Qt::Key_Enter: return "Return";
    case Qt::Key_Tab: return "Tab";
    case Qt::Key_Backspace: return "Backspace";
    case Qt::Key_Insert: return "Insert";
    case Qt::Key_Delete: return "Delete";
    case Qt::Key_Home: return "Home";
    case Qt::Key_End: return "End";
    case Qt::Key_PageUp: return "PageUp";
    case Qt::Key_PageDown: return "PageDown";
    case Qt::Key_Left: return "Left";
    case Qt::Key_Right: return "Right";
    case Qt::Key_Up: return "Up";
    case Qt::Key_Down: return "Down";
    case Qt::Key_CapsLock: return "CapsLock";
    case Qt::Key_Menu: return "Menu";
    case Qt::Key_Pause: return "Pause";
    case Qt::Key_Print: return "PrintScreen";
    case Qt::Key_ScrollLock: return "ScrollLock";
    case Qt::Key_Shift: return "Left Shift";
    case Qt::Key_Control: return "Left Ctrl";
    case Qt::Key_Alt: return "Left Alt";
    case Qt::Key_AltGr: return "Right Alt";
    case Qt::Key_Meta: return "Left GUI";
    default: return {};
    }
}

}  // namespace

QString bindingForKey(const QKeyEvent *e) {
    QString n = byScanCode(e->nativeScanCode());
    if (n.isEmpty()) n = byQtKey(e->key());
    if (n.isEmpty() && e->text().size() == 1 && e->text()[0].isPrint()) n = e->text();
    if (n.isEmpty() || n == "#") return {};
    SDL_Keycode k = keycodeOf(n);
    if (k == SDLK_UNKNOWN) return {};
    return "key:" + nameOf(k);   // SDL's own spelling
}

QString describeBinding(const QString &b) {
    if (b.startsWith("key:")) {
        QString k = b.mid(4);
        for (auto &a : kAliases)
            if (k == a.alias) return QString(a.key);
        return k;
    }
    static const QRegularExpression re(R"(^(button|axis|hat)(\d+)(.*)$)");
    QString pad = bindingPad(b);
    auto m = re.match(bindingRest(b));
    if (pad.isEmpty() || !m.hasMatch()) return b;
    QString who = pad.startsWith("joy[") ? pad.mid(4, pad.size() - 5).replace('@', " (") + (pad.contains('@') ? ")" : "")
                                         : "Pad " + pad.mid(3);
    QString rest = m.captured(3);
    if (rest.startsWith(':')) rest = " " + rest.mid(1);
    if (rest.startsWith('*')) {
        double s = axisScale(b);
        QStringList how;
        if (s < 0) how << "reversed";
        if (std::abs(s) != 1) how << QString("×%1").arg(std::abs(s));
        rest = how.isEmpty() ? QString() : " (" + how.join(", ") + ")";
    }
    return QString("%1: %2 %3%4").arg(who, m.captured(1), m.captured(2), rest);
}

QString bindingPad(const QString &b) {
    if (b.startsWith("key:")) return "key";
    if (b.startsWith("joy[")) {
        int e = b.indexOf("]:");
        return e < 0 ? QString() : b.left(e + 1);
    }
    static const QRegularExpression re(R"(^joy\d+(?=:))");
    return re.match(b).captured(0);
}

QString bindingRest(const QString &b) {
    QString pad = bindingPad(b);
    return pad.isEmpty() ? b : b.mid(pad.size() + 1);
}

QString bindingWith(const QString &pad, const QString &rest) { return pad + ":" + rest; }

QString canonicalBinding(const QString &b) {
    QString pad = bindingPad(b), rest = bindingRest(b);
    if (pad.isEmpty() || pad == "key") return b;
    if (int j = Joysticks::instance().indexOf(pad); j >= 0) pad = QString("joy%1").arg(j);
    if (int star = rest.indexOf('*'); star >= 0) rest.truncate(star);
    return bindingWith(pad, rest);
}

double axisScale(const QString &b) {
    QString rest = bindingRest(b);
    int star = rest.indexOf('*');
    bool ok = false;
    double s = star < 0 ? 1.0 : rest.mid(star + 1).toDouble(&ok);
    return star < 0 || (ok && s != 0) ? s : 1.0;
}

QString withAxisScale(const QString &b, double scale) {
    QString rest = bindingRest(b);
    if (int star = rest.indexOf('*'); star >= 0) rest.truncate(star);
    if (scale != 1.0) rest += "*" + QString::number(scale, 'g', 4);
    return bindingWith(bindingPad(b), rest);
}

QString normalizeBinding(const QString &b) {
    if (!b.startsWith("key:")) return b.trimmed();
    SDL_Keycode k = keycodeOf(b.mid(4));
    return k == SDLK_UNKNOWN ? b : "key:" + nameOf(k);
}

// ---- joysticks ----

Joysticks &Joysticks::instance() {
    static Joysticks j;
    return j;
}

Joysticks::Joysticks() {
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");   // Ctrl-C stays Qt's
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_InitSubSystem(SDL_INIT_JOYSTICK);
    reopen();
    connect(&m_timer, &QTimer::timeout, this, &Joysticks::poll);
    m_timer.start(8);
}

void Joysticks::reopen() {
    for (void *j : m_joys) SDL_CloseJoystick((SDL_Joystick *)j);
    m_joys.clear();
    m_pads.clear();
    m_known.clear();
    m_seen.clear();
    m_buttonRest.clear();
    m_hatRest.clear();
    int n = 0;
    SDL_JoystickID *ids = SDL_GetJoysticks(&n);
    for (int k = 0; k < n; k++)
        if (SDL_Joystick *j = SDL_OpenJoystick(ids[k])) {
            // as the runtime names them (runtime/host/input.cpp add_joystick): a second of a name is "<name>@2"
            const char *raw = SDL_GetJoystickName(j);
            QString name = QString::fromStdString(hk::cab::pad_name(raw ? raw : ""));
            int same = 1;
            for (const QString &p : m_pads)
                if (p == "joy[" + name + "]" || p.startsWith("joy[" + name + "@")) same++;
            m_joys.push_back(j);
            m_pads << "joy[" + name + (same > 1 ? QString("@%1").arg(same) : QString()) + "]";
        }
    SDL_free(ids);
}

QString Joysticks::pad(int joy) const { return joy >= 0 && joy < m_pads.size() ? m_pads[joy] : QString(); }

QString Joysticks::label(int joy) const {
    QString p = pad(joy);
    if (p.isEmpty()) return {};
    p = p.mid(4, p.size() - 5);
    int at = p.indexOf('@');
    return at < 0 ? p : p.left(at) + " (" + p.mid(at + 1) + ")";
}

int Joysticks::indexOf(const QString &pad) const {
    if (pad.startsWith("joy[")) return (int)m_pads.indexOf(pad);
    bool ok = false;
    int j = pad.mid(3).toInt(&ok);
    return pad.startsWith("joy") && ok && j >= 0 && j < m_joys.size() ? j : -1;
}

QStringList Joysticks::names() const {
    QStringList out;
    for (int k = 0; k < m_joys.size(); k++) {
        auto *j = (SDL_Joystick *)m_joys[k];
        out << QString("joy%1: %2 (%3 buttons, %4 axes)")
                   .arg(k)
                   .arg(QString::fromUtf8(SDL_GetJoystickName(j)))
                   .arg(SDL_GetNumJoystickButtons(j))
                   .arg(SDL_GetNumJoystickAxes(j));
    }
    return out;
}

int Joysticks::axisValue(int joy, int axis) const {
    if (joy < 0 || joy >= m_joys.size()) return 0;
    return SDL_GetJoystickAxis((SDL_Joystick *)m_joys[joy], axis);
}

int Joysticks::axisCount(int joy) const {
    if (joy < 0 || joy >= m_joys.size()) return 0;
    return SDL_GetNumJoystickAxes((SDL_Joystick *)m_joys[joy]);
}

void Joysticks::firstReport(int joy) {
    if (m_seen.contains(joy)) return;
    m_seen.insert(joy);
    auto *js = (SDL_Joystick *)m_joys[joy];
    int held = 0;
    for (int b = 0; b < SDL_GetNumJoystickButtons(js); b++) held += SDL_GetJoystickButton(js, b);
    for (int h = 0; h < SDL_GetNumJoystickHats(js); h++)
        for (int bit = 1; bit < 16; bit <<= 1) held += (SDL_GetJoystickHat(js, h) & bit) != 0;
    if (held < 2) return;
    for (int b = 0; b < SDL_GetNumJoystickButtons(js); b++)
        if (SDL_GetJoystickButton(js, b)) m_buttonRest.insert(QString("%1:%2").arg(joy).arg(b));
    for (int h = 0; h < SDL_GetNumJoystickHats(js); h++)
        if (int v = SDL_GetJoystickHat(js, h) & 0x0f) m_hatRest[QString("%1:%2").arg(joy).arg(h)] = v;
}

void Joysticks::poll() {
    auto index = [&](SDL_JoystickID id) {
        for (int k = 0; k < m_joys.size(); k++)
            if (SDL_GetJoystickID((SDL_Joystick *)m_joys[k]) == id) return k;
        return -1;
    };
    SDL_Event e;
    bool changed = false;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_JOYSTICK_ADDED:
        case SDL_EVENT_JOYSTICK_REMOVED: changed = true; break;
        case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
        case SDL_EVENT_JOYSTICK_BUTTON_UP:
            if (int j = index(e.jbutton.which); j >= 0) {
                firstReport(j);
                const QString key = QString("%1:%2").arg(j).arg(e.jbutton.button);
                if (m_buttonRest.contains(key)) {   // resting: let go, it's pressed from now on
                    if (e.type == SDL_EVENT_JOYSTICK_BUTTON_UP) m_buttonRest.remove(key);
                    break;
                }
                emit button(j, e.jbutton.button, e.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN);
            }
            break;
        case SDL_EVENT_JOYSTICK_AXIS_MOTION:
            if (int j = index(e.jaxis.which); j >= 0) {
                firstReport(j);
                emit axis(j, e.jaxis.axis, e.jaxis.value);
                m_known.insert(QString("%1:%2").arg(j).arg(e.jaxis.axis));
            }
            break;
        case SDL_EVENT_JOYSTICK_HAT_MOTION:
            if (int j = index(e.jhat.which); j >= 0) {
                firstReport(j);
                int &rest = m_hatRest[QString("%1:%2").arg(j).arg(e.jhat.hat)];
                rest &= e.jhat.value;
                emit hat(j, e.jhat.hat, e.jhat.value & ~rest);
            }
            break;
        default: break;
        }
    }
    if (changed) {
        reopen();
        emit devicesChanged();
    }
}
