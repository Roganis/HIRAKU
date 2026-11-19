// SPDX-License-Identifier: GPL-3.0-or-later
// The USB_2_573 board's raw HID interface through SDL's hidapi (host/usb573.h's Link).
#include <SDL3/SDL.h>

#include <cstring>

#include "usb573.h"

namespace hk::host::usb573 {

namespace {

class HidLink final : public Link {
public:
    ~HidLink() override { close(); }

    bool open(const std::string &where, std::string &why) override {
        close();
        if (SDL_hid_init() != 0) {
            why = "SDL's HID support didn't start";
            return false;
        }
        std::string path = where;
        if (where == "auto") {
            path.clear();
            SDL_hid_device_info *list = SDL_hid_enumerate(VID, PID);
            for (SDL_hid_device_info *e = list; e && path.empty(); e = e->next) {
                // the IDs are pid.codes' test ones, anybody's: the board says its name
                const size_t len = strlen(PRODUCT);
                bool named = e->product_string != nullptr;
                for (size_t k = 0; named && k < len; k++) named = e->product_string[k] == (wchar_t)PRODUCT[k];
                if (!named) continue;
                // its raw interface: by its usage page where the system tells one, else the first
                if (e->usage_page ? e->usage_page != USAGE_PAGE : e->interface_number != 0) continue;
                path = e->path;
            }
            SDL_hid_free_enumeration(list);
            if (path.empty()) return false;
        }
        m_dev = SDL_hid_open_path(path.c_str());
        if (!m_dev) {
            why = "the USB_2_573 board at " + path + " can't be opened";
#ifdef __linux__
            why += ": its /dev/hidraw node must be the user's (docs/USB573.md has the udev rule)";
#endif
            return false;
        }
        return true;
    }

    void close() override {
        if (m_dev) SDL_hid_close(m_dev);
        m_dev = nullptr;
    }

    int read(uint8_t *report, int max, int timeout_ms) override {
        return m_dev ? SDL_hid_read_timeout(m_dev, report, (size_t)max, timeout_ms) : -1;
    }

    bool write(const uint8_t *report, int n) override {
        if (!m_dev || n > REPORT) return false;
        uint8_t b[1 + REPORT] = {};   // (the report ID first: none, 0)
        memcpy(b + 1, report, (size_t)n);
        return SDL_hid_write(m_dev, b, (size_t)n + 1) >= 0;
    }

private:
    SDL_hid_device *m_dev = nullptr;
};

}  // namespace

std::unique_ptr<Link> system_link() { return std::make_unique<HidLink>(); }

}  // namespace hk::host::usb573
