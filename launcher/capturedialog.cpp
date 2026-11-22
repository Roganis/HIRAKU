// SPDX-License-Identifier: GPL-3.0-or-later
#include "capturedialog.h"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include <SDL3/SDL.h>

#include "inputs.h"

namespace {

QString hatDir(int bits) {
    if (bits & SDL_HAT_UP) return "up";
    if (bits & SDL_HAT_DOWN) return "down";
    if (bits & SDL_HAT_LEFT) return "left";
    if (bits & SDL_HAT_RIGHT) return "right";
    return {};
}

QPushButton *quietButton(const QString &text) {
    auto *b = new QPushButton(text);
    b->setAutoDefault(false);
    b->setFocusPolicy(Qt::NoFocus);   // the keys pressed are bindings, not clicks
    return b;
}

}  // namespace

CaptureDialog::CaptureDialog(const QString &what, bool analog, QWidget *parent, const QString &step, bool canGoBack)
    : QDialog(parent), m_analog(analog), m_walk(!step.isEmpty()) {
    setWindowTitle(m_walk ? tr("Bind all") : tr("Set binding"));
    auto *v = new QVBoxLayout(this);
    if (m_walk) {
        auto *where = new QLabel(step);
        where->setAlignment(Qt::AlignCenter);
        v->addWidget(where);
    }
    auto *title = new QLabel("<b>" + what.toHtmlEscaped() + "</b>");
    title->setAlignment(Qt::AlignCenter);
    v->addWidget(title);
    m_text = new QLabel;
    m_text->setAlignment(Qt::AlignCenter);
    m_text->setMinimumWidth(360);
    v->addWidget(m_text);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch(1);
    if (m_walk) {
        auto *back = quietButton(tr("Back"));
        back->setEnabled(canGoBack);
        connect(back, &QPushButton::clicked, this, [this] { done(Back); });
        buttons->addWidget(back);
        auto *skip = quietButton(tr("Skip"));
        skip->setToolTip(tr("Leave this control as it is"));
        connect(skip, &QPushButton::clicked, this, [this] { done(Skipped); });
        buttons->addWidget(skip);
    }
    auto *stop = quietButton(m_walk ? tr("Stop") : tr("Cancel"));
    connect(stop, &QPushButton::clicked, this, &QDialog::reject);
    buttons->addWidget(stop);
    buttons->addStretch(1);
    v->addLayout(buttons);

    auto &joys = Joysticks::instance();
    for (int j = 0; j < joys.count(); j++)
        for (int a = 0; a < joys.axisCount(j); a++)
            if (joys.axisKnown(j, a)) m_base[QString("%1:%2").arg(j).arg(a)] = joys.axisValue(j, a);
    connect(&joys, &Joysticks::button, this, [this](int j, int b, bool down) {
        if (down && !m_analog) finish(QString("%1:button%2").arg(Joysticks::instance().pad(j)).arg(b));
    });
    connect(&joys, &Joysticks::hat, this, [this](int j, int h, int v) {
        if (!m_analog && !hatDir(v).isEmpty()) finish(QString("%1:hat%2:%3").arg(Joysticks::instance().pad(j)).arg(h).arg(hatDir(v)));
    });
    connect(&joys, &Joysticks::axis, this, [this](int j, int a, int v) { onAxis(j, a, v); });
    if (!m_walk) {
        connect(&m_tick, &QTimer::timeout, this, [this] {
            if (--m_left <= 0) reject();
            else showText();
        });
        m_tick.start(1000);
    }
    showText();
}

void CaptureDialog::showEvent(QShowEvent *e) {
    QDialog::showEvent(e);
    grabKeyboard();
}

void CaptureDialog::hideEvent(QHideEvent *e) {
    releaseKeyboard();
    QDialog::hideEvent(e);
}

void CaptureDialog::keyPressEvent(QKeyEvent *e) {
    if (e->isAutoRepeat()) return;
    if (e->key() == Qt::Key_Escape) {
        reject();
        return;
    }
    if (m_analog) return;
    QString b = bindingForKey(e);
    if (b.isEmpty()) m_text->setText(tr("That key cannot be bound. Try another one."));
    else finish(b);
}

void CaptureDialog::showText() {
    const QString ask = m_analog ? tr("Spin the turntable (move an analog axis)…") : tr("Press a key or a controller button…");
    // TR: %1 is the seconds left before the dialog closes by itself
    m_text->setText(ask + "\n\n" + (m_walk ? tr("Esc stops.") : tr("Esc cancels (%1 s)").arg(m_left)));
}

void CaptureDialog::onAxis(int j, int a, int v) {
    // an axis that hadn't reported yet: this is where it rests (the report a key's press brought
    // with it), not the axis moving; it's bound once it moves from there
    const QString key = QString("%1:%2").arg(j).arg(a);
    if (!m_base.contains(key)) {
        m_base[key] = v;
        return;
    }
    int base = m_base.value(key);
    int d = v - base;
    if (d > 32768) d -= 65536;   // turntable encoders wrap
    if (d < -32768) d += 65536;
    QString pad = Joysticks::instance().pad(j);
    if (m_analog) {
        if (std::abs(d) > 3000) finish(QString("%1:axis%2").arg(pad).arg(a));
        return;
    }
    // As a button: the runtime's thresholds; an axis that rests past one (a trigger) must move first
    if (v > 16384 && base <= 16384) finish(QString("%1:axis%2+").arg(pad).arg(a));
    else if (v < -16384 && base >= -16384) finish(QString("%1:axis%2-").arg(pad).arg(a));
}

void CaptureDialog::finish(const QString &b) {
    if (!result().isEmpty()) return;   // one input, even if another arrives before the dialog closes
    m_result = b;
    accept();
}
