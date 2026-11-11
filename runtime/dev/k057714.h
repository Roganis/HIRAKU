// license:BSD-3-Clause
// copyright-holders:Ville Linde
//
// Konami 0000057714 "GCU" 2D graphics chip.
// Ported to HIRAKU from MAME's k057714.cpp (BSD-3-Clause); the MAME device
// framework is replaced by plain C++, the drawing logic is unchanged.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "hiraku/scroll_guess.h"

namespace hk {

class K057714 {
public:
    static constexpr uint32_t VRAM_SIZE = 0x2000000;        // bytes
    static constexpr uint32_t VRAM_SIZE_HALF = 0x1000000;   // 16-bit pixels

    K057714();
    void reset();

    uint32_t read(uint32_t reg);                            // reg = byte offset, 32-bit aligned
    void write(uint32_t reg, uint32_t data, uint32_t mem_mask);

    void vblank(bool state);                                // start of vertical blank
    std::function<void(bool)> irq;                          // to the CPU's EXT0

    // Compose the visible frame into `out` (xRGB8888, width*height).
    void draw(uint32_t *out, int width, int height);

    // CRTC timing (for the scheduler)
    int htotal() const;
    int vtotal() const;
    int hvisible() const { return m_display_h_visarea; }
    int vvisible() const { return m_display_v_visarea; }
    double pixclock() const { return m_pixclock; }
    void set_pixclock(double hz) { m_pixclock = hz; }

    const uint32_t *vram() const { return m_vram.get(); }

    // HIRAKU: in-between pictures (Core::render_between, --smooth-motion). Once first asked, the GCU
    // keeps each frame's drawing commands and what the rows they drew over held before them; this
    // then draws the frame on show again with its objects carried on `t` (0 < t < 1) of their last
    // move (matched with the frame before's), composes it into `out` like draw(), and puts VRAM
    // back as it was. false: nothing to draw from yet.
    bool draw_between(uint32_t *out, int width, int height, double t);

private:
    struct Frame {
        uint32_t base = 0;
        uint16_t width = 0, height = 0, x = 0, y = 0, alpha = 0;
    };

    uint16_t pix(uint32_t index) const;
    void set_pix(uint32_t index, uint16_t v);
    void draw_frame(int frame, uint16_t *bitmap, int bw, int bh, bool inverse_trans);
    void draw_object(const uint32_t *cmd);
    void fill_rect(const uint32_t *cmd);
    void draw_character(const uint32_t *cmd);
    void fb_config(const uint32_t *cmd);
    void execute_display_list(uint32_t addr, int depth = 0);
    void execute_command(const uint32_t *cmd);
    void run(const uint32_t *cmd);                          // fb config, fill, object, character
    void touch(int y0, int y1, int x0, int x1);             // rows y0..y1, columns x0..x1, about to be drawn on

    using Cmd = std::array<uint32_t, 4>;
    struct Drawn {                                          // a frame's drawing
        std::vector<Cmd> cmds;
        int origin_x = 0, origin_y = 0;                     // fb_config as it began
        std::vector<uint32_t> rows;                         // the rows it drew on, and what they
        std::vector<uint32_t> before;                       // held before (512 words each)
        int x0 = 1024, x1 = -1;                             // the columns it drew on
        std::vector<std::pair<int, int>> cmd_rows;          // each command's rows (first > last: none)
    };
    bool m_keep = false, m_replaying = false;
    Drawn m_drawing, m_shown;
    Drawn m_shown_before;                                   // the frame before m_shown (a game drawing a frame
                                                            // ahead shows it while m_shown waits for its flip)
    hk::ScrollGuess m_scroll;                               // in-between pictures: where what scrolls goes
    std::vector<uint32_t> m_row_mark, m_restore_rows, m_restore;
    uint32_t m_mark = 0, m_replay_mark = 0;
    int m_span0 = 0, m_span1 = 512;                         // a replay: the words of each row it keeps
    bool m_clip = false;                                    // a replay draws only on the rows marked here
    std::vector<uint8_t> m_clip_row;
    int m_cmd_y0 = 0, m_cmd_y1 = 0;                         // the command running: the rows it drew on
    bool row_out(int y) const { return m_clip && !m_clip_row[(uint32_t)y & (m_clip_row.size() - 1)]; }

    std::unique_ptr<uint32_t[]> m_vram;
    uint32_t m_vram_read_addr = 0;
    uint32_t m_vram_fifo0_addr = 0, m_vram_fifo1_addr = 0;
    uint32_t m_vram_fifo0_mode = 0, m_vram_fifo1_mode = 0;
    uint32_t m_command_fifo0[4]{}, m_command_fifo1[4]{};
    uint32_t m_command_fifo0_ptr = 0, m_command_fifo1_ptr = 0;
    uint32_t m_ext_fifo_addr = 0, m_ext_fifo_count = 0, m_ext_fifo_line = 0, m_ext_fifo_num_lines = 0, m_ext_fifo_width = 0;
    Frame m_frame[4];
    int m_fb_origin_x = 0, m_fb_origin_y = 0;
    uint16_t m_layer_select = 0;
    uint16_t m_reg_6c = 0;
    uint16_t m_irqctrl = 0;
    int m_display_h_visarea = 640, m_display_h_frontporch = 16, m_display_h_backporch = 48, m_display_h_syncpulse = 96;
    int m_display_v_visarea = 480, m_display_v_frontporch = 10, m_display_v_backporch = 33, m_display_v_syncpulse = 2;
    double m_pixclock = 25'175'000.0;
};

}  // namespace hk
