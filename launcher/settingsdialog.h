// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDialog>
#include <QMap>

class QVBoxLayout;

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLineEdit;
class QSpinBox;

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget *parent = nullptr);
    void accept() override;
private:
    // its tabs, each made into the layout it gets
    void displayPage(QVBoxLayout *v);
    void audioPage(QVBoxLayout *v);
    void eamusePage(QVBoxLayout *v);
    void cabinetPage(QVBoxLayout *v);
    void generalPage(QVBoxLayout *v);
    QSpinBox *m_scale, *m_audioMs, *m_volume;
    QCheckBox *m_fullscreen, *m_splitScreens, *m_displaySync, *m_fineSound, *m_audioFixed, *m_priority, *m_audioExclusive = nullptr, *m_socket, *m_lightsOverlay, *m_fastBoot, *m_hideLauncher, *m_checkUpdates;
    QComboBox *m_picture, *m_audioDevice, *m_audioBuffer;
    QMap<QString, QString> m_shaderKeys;    // the in-game shader settings' controls, by action
    QComboBox *m_pyRenderer;                // the Konami Python games' GS: Vulkan, OpenGL, software
    QComboBox *m_motion;                    // on a fast display: frames held, smooth motion, the CRT beam
    QDoubleSpinBox *m_beamGamma, *m_beamGain;
    QLineEdit *m_soundfont, *m_root, *m_bin, *m_nvram, *m_extra;
    QComboBox *m_place;                     // games' files when added: ask, move into the HIRAKU folder, leave
    // e-AMUSEMENT, as spicecfg's Network page (runtime/shell/eaoptions.h)
    QCheckBox *m_smartea, *m_autopin;
    QComboBox *m_autocard;
    QCheckBox *m_pressTime;                 // pop'n music 15-18: presses judged when they happened
    QSpinBox *m_judgeOffset;                // ... and moved this many milliseconds
    QLineEdit *m_eaUrl, *m_pcbid, *m_card[2];
    QComboBox *m_reader[2];                 // serial card readers (runtime/host/readers.h)
    QCheckBox *m_scard, *m_cardio;
    QComboBox *m_bio2;                      // beatmania IIDX's BIO2 board (runtime/host/bio2.h)
    QComboBox *m_popnio;                    // pop'n music's USB I/O board (runtime/host/popnio.h)
    QLineEdit *m_popnioDll;                 // its ezusb.dll
};
