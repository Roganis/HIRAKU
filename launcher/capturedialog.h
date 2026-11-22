// SPDX-License-Identifier: GPL-3.0-or-later
// Waits for one key press or controller input and returns it as a binding (a controller's by its
// name: "joy[DAO FPS]:button3"). A walk through several (the Controls dialog's "Bind all") shows
// where it is, adds Back and Skip, and waits as long as it takes.
#pragma once

#include <QCoreApplication>
#include <QDialog>
#include <QHash>
#include <QTimer>

class QLabel;

class CaptureDialog : public QDialog {
    Q_DECLARE_TR_FUNCTIONS(CaptureDialog)   // its texts' context in the catalogues (launcher/language.h)
public:
    enum Outcome { Stopped = QDialog::Rejected, Captured = QDialog::Accepted, Skipped = 2, Back = 3 };
    // step: "" for one binding (6 s to answer), or the walk's place ("3 of 12")
    CaptureDialog(const QString &what, bool analog, QWidget *parent, const QString &step = {}, bool canGoBack = false);
    QString result() const { return m_result; }
protected:
    void showEvent(QShowEvent *e) override;
    void hideEvent(QHideEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
private:
    void showText();
    void onAxis(int j, int a, int v);
    void finish(const QString &b);
    bool m_analog, m_walk;
    QLabel *m_text;
    QTimer m_tick;
    int m_left = 6;
    QHash<QString, int> m_base;
    QString m_result;
};
