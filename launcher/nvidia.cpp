// SPDX-License-Identifier: GPL-3.0-or-later
// NVAPI's driver settings, read only: nvapi64.dll's nvapi_QueryInterface gives each call by its
// number, and the structures are NVAPI's (NVIDIA's open headers, nvapi.h and
// NvApiDriverSettings.h: MIT), declared here with the sizes the driver checks.
#include "nvidia.h"

#include <QCheckBox>
#include <QDir>
#include <QMessageBox>

#ifdef Q_OS_WIN
#include <windows.h>

#include <cstdint>
#include <cstring>

namespace {

typedef int NvStatus;
typedef void *NvSession;
typedef void *NvProfile;
typedef void *(__cdecl *QueryInterface_t)(unsigned);

const unsigned INITIALIZE = 0x0150e828, UNLOAD = 0xd22bdd7e, CREATE_SESSION = 0x0694d52e, DESTROY_SESSION = 0xdad9cff8,
               LOAD_SETTINGS = 0x375dbd6b, CURRENT_GLOBAL_PROFILE = 0x617bff9f, FIND_APPLICATION = 0xeee566b2,
               GET_SETTING = 0x73bf8338;
const NvStatus OK = 0, SETTING_NOT_FOUND = -160, EXECUTABLE_NOT_FOUND = -166;
const uint32_t PRESENT_METHOD = 0x20d690f8;   // OGL_CPL_PREFER_DXPRESENT_ID
const int PRESENT_DEFAULT = 2;                 // OGL_CPL_PREFER_DXPRESENT_AUTO

#pragma pack(push, 4)
struct Setting {   // NVDRS_SETTING_V1
    uint32_t version;
    uint16_t name[2048];
    uint32_t id, type, location, current_predefined, predefined_valid;
    uint8_t predefined[4100];   // (a union: its NvU32 first)
    union {
        uint32_t u32;
        uint8_t bytes[4100];
    } current;
};
struct Application {   // NVDRS_APPLICATION_V1
    uint32_t version, predefined;
    uint16_t name[2048], friendly[2048], launcher[2048];
};
#pragma pack(pop)
static_assert(sizeof(Setting) == 12320, "NVDRS_SETTING_V1's size");
static_assert(sizeof(Application) == 12296, "NVDRS_APPLICATION_V1's size");
constexpr uint32_t version_of(size_t size, unsigned v) { return (uint32_t)size | (v << 16); }

}  // namespace

nv::PresentMethod nv::presentMethod(const QString &exe_name) {
    PresentMethod r;
    HMODULE dll = LoadLibraryW(sizeof(void *) == 8 ? L"nvapi64.dll" : L"nvapi.dll");
    auto query = dll ? (QueryInterface_t)(void *)GetProcAddress(dll, "nvapi_QueryInterface") : nullptr;
    if (!query) {
        if (dll) FreeLibrary(dll);
        return r;
    }
    auto initialize = (NvStatus(__cdecl *)())query(INITIALIZE);
    auto unload = (NvStatus(__cdecl *)())query(UNLOAD);
    auto create = (NvStatus(__cdecl *)(NvSession *))query(CREATE_SESSION);
    auto destroy = (NvStatus(__cdecl *)(NvSession))query(DESTROY_SESSION);
    auto load = (NvStatus(__cdecl *)(NvSession))query(LOAD_SETTINGS);
    auto global = (NvStatus(__cdecl *)(NvSession, NvProfile *))query(CURRENT_GLOBAL_PROFILE);
    auto find = (NvStatus(__cdecl *)(NvSession, const uint16_t *, NvProfile *, Application *))query(FIND_APPLICATION);
    auto get = (NvStatus(__cdecl *)(NvSession, NvProfile, uint32_t, Setting *))query(GET_SETTING);
    NvSession s = nullptr;
    if (initialize && unload && create && destroy && load && global && find && get && initialize() == OK) {
        if (create(&s) == OK && load(s) == OK) {
            r.nvidia = true;
            uint16_t name[2048] = {};
            const QString n = exe_name.left(2047);
            memcpy(name, n.utf16(), sizeof(uint16_t) * (size_t)n.size());
            auto *app = new Application{};
            app->version = version_of(sizeof(Application), 1);
            NvProfile profile = nullptr;
            NvStatus st = find(s, name, &profile, app);
            r.from = "its own profile";
            if (st == EXECUTABLE_NOT_FOUND) st = global(s, &profile), r.from = "the global profile";
            if (st == OK) {
                auto *set = new Setting{};
                set->version = version_of(sizeof(Setting), 1);
                st = get(s, profile, PRESENT_METHOD, set);
                if (st == OK) {
                    r.value = (int)set->current.u32;
                    if (set->location == 1) r.from = "the global profile";
                    else if (set->location >= 2) r.from = "the driver's default";
                } else if (st == SETTING_NOT_FOUND) {
                    r.value = PRESENT_DEFAULT, r.from = "the driver's default";
                }
                delete set;
            }
            delete app;
        }
        if (s) destroy(s);
        unload();
    }
    FreeLibrary(dll);
    return r;
}
#else
nv::PresentMethod nv::presentMethod(const QString &) { return {}; }
#endif

QString nv::presentMethodName(int value) {
    switch (value) {
    case 0: return "Prefer native";
    case 1: return "Prefer layered on DXGI Swapchain";
    case 2: return "Auto";
    default: return "unknown";
    }
}

QMessageBox *nv::noticeBox(QWidget *parent, const QString &exe_path, const PresentMethod &now) {
    auto *box = new QMessageBox(QMessageBox::Information, "NVIDIA setting for the PC games",
                                "The PC games need one NVIDIA setting to start in full screen.", QMessageBox::Ok, parent);
    box->setInformativeText(
        "With NVIDIA's default, HIRAKU's full-screen window takes the screen to itself, and Windows refuses the game "
        "its Direct3D (\"Direct3D の初期化に失敗しました\"). In a window the game runs either way.\n\n"
        "To set it, once:\n"
        "1. Open the NVIDIA Control Panel (right-click the desktop, or the Start menu).\n"
        "2. 3D Settings > Manage 3D settings > Program Settings.\n"
        "3. Add > Browse..., and choose\n    " + QDir::toNativeSeparators(exe_path) + "\n"
        "4. Set \"Vulkan/OpenGL present method\" to \"Prefer layered on DXGI Swapchain\".\n"
        "5. Apply.\n\n"
        "It's now \"" + presentMethodName(now.value) + "\" (" + now.from + ").");
    box->setTextInteractionFlags(Qt::TextSelectableByMouse);
    box->setCheckBox(new QCheckBox("Don't show this again"));
    return box;
}

bool nv::notice(QWidget *parent, const QString &exe_path, const PresentMethod &now) {
    QMessageBox *box = noticeBox(parent, exe_path, now);
    box->exec();
    const bool never = box->checkBox()->isChecked();
    delete box;
    return never;
}
