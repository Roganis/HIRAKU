// SPDX-License-Identifier: GPL-3.0-or-later
// HIRAKU: MAME's imagedev/memcard.h (memory card images): see image.h.
// A card is in while the board has given it bytes (hk_insert: the System 573's memory card reader,
// runtime/sys573/memcard.h): its device reads and writes them with fseek/fread/fwrite, as it would
// MAME's open image file; the bytes stay the board's, which writes them to the card's file.
#pragma once
#include "imagedev/image.h"
#include <vector>
class device_memcard_image_interface : public device_image_interface {
public:
    device_memcard_image_interface(const machine_config &mconfig, device_t &device) : device_image_interface(mconfig, device) {}
    bool is_readable() const noexcept override { return true; }
    bool is_writeable() const noexcept override { return true; }
    bool is_creatable() const noexcept override { return true; }
    bool support_command_line_image_creation() const noexcept override { return true; }
    const char *image_type_name() const noexcept override { return "memcard"; }
    const char *image_brief_type_name() const noexcept override { return "memc"; }

    void hk_insert(std::vector<u8> *bytes) { m_hk_bytes = bytes, m_hk_pos = 0; }   // nullptr: taken out
    u64 hk_writes() const { return m_hk_writes; }   // fwrite()s so far (the board: has the card changed?)
    // true: writes are taken and go nowhere (the board's departure from MAME: the reader's first
    // write after a card goes in, which the real reader never puts on the card; memcard.h)
    void hk_ignore_writes(bool on) { m_hk_ignore = on; }

    bool is_loaded() const noexcept { return m_hk_bytes != nullptr; }
    bool exists() const noexcept { return is_loaded(); }
    bool is_open() const noexcept { return is_loaded(); }
    bool is_readonly() const noexcept { return false; }
    void unload() { m_hk_bytes = nullptr; }
    u64 length() { return m_hk_bytes ? m_hk_bytes->size() : 0; }
    u64 ftell() { return m_hk_pos; }
    std::error_condition fseek(s64 offset, int whence) {
        const s64 base = whence == SEEK_CUR ? (s64)m_hk_pos : whence == SEEK_END ? (s64)length() : 0;
        m_hk_pos = (u64)std::max<s64>(0, base + offset);
        return {};
    }
    u32 fread(void *buffer, u32 length) {
        const u64 size = this->length(), n = m_hk_pos < size ? std::min<u64>(length, size - m_hk_pos) : 0;
        if (n) memcpy(buffer, m_hk_bytes->data() + m_hk_pos, n);
        m_hk_pos += n;
        return (u32)n;
    }
    u32 fwrite(const void *buffer, u32 length) {
        const u64 size = this->length(), n = m_hk_pos < size ? std::min<u64>(length, size - m_hk_pos) : 0;   // (a card doesn't grow)
        if (n && !m_hk_ignore) memcpy(m_hk_bytes->data() + m_hk_pos, buffer, n), m_hk_writes++;
        m_hk_pos += n;
        return (u32)n;
    }

private:
    std::vector<u8> *m_hk_bytes = nullptr;
    u64 m_hk_pos = 0;
    u64 m_hk_writes = 0;
    bool m_hk_ignore = false;
};
