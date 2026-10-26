// SPDX-License-Identifier: GPL-3.0-or-later
// NVIDIA's "Vulkan/OpenGL present method" for a program, read from the driver's settings (NVAPI's
// DRS, read only). The PC games need "Prefer layered on DXGI Swapchain" for HIRAKU's window in
// full screen on Windows: with the driver's default its OpenGL full screen holds the screen, and
// the game's Direct3D 9 device is refused as lost (GitHub issue 4).
#pragma once

#include <QString>

class QMessageBox;
class QWidget;

namespace nv {

struct PresentMethod {
    bool nvidia = false;   // an NVIDIA driver answered
    int value = -1;        // 0 prefer native, 1 prefer layered on DXGI (what the PC games need), 2 auto
    QString from;          // where the value comes from ("its own profile", "the global profile", "the driver's default")
};

PresentMethod presentMethod(const QString &exe_name);   // Windows only (elsewhere: nvidia false)
QString presentMethodName(int value);
// The steps to set it, for the program at exe_path, in a box; true: "Don't show this again" ticked
bool notice(QWidget *parent, const QString &exe_path, const PresentMethod &now);
QMessageBox *noticeBox(QWidget *parent, const QString &exe_path, const PresentMethod &now);   // (not shown yet)

}  // namespace nv
