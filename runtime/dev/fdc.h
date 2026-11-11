// SPDX-License-Identifier: GPL-3.0-or-later
// SMSC FDC37C665GT Super I/O floppy controller (an 82077-compatible core in PS/2 mode) with one
// 3.5" HD drive: beatmania III's, where a player's disk goes.
//
// It answers in one of two ways:
//
//   A drive that never held a disk (no --floppy): as before this file knew of disks. Only the
//   commands of the games' FDD check, with flat times, matched against a MAME trace of that check.
//   The regress baselines were recorded with it, so it stays exactly as it was.
//
//   Once a disk has been put in (and from then on, in or out): the controller and the drive as
//   MAME has them (upd765.cpp's N82077AA, floppy.cpp's 3.5" HD drive; BSD-3-Clause, Olivier
//   Galibert) - the command, execution and result phases, the 16-byte FIFO with its threshold
//   and its interrupts (the board has no DMA line: the games run it in non-DMA mode), seeks a
//   step at a time, the disk-change line, write protect, and a disk that turns at 300 rpm under
//   the head: a sector's bytes come and go one every 16 us, at the place on the track where
//   MAME's 1.44 MB format puts them. MAME's drive works on flux; this one on the bytes of each
//   field, which is the same to a program.
//
//   In three things the chip's data sheet (SMSC's 82077 core: FDC37C665GT, FDC37C669) says
//   otherwise than MAME does, and the games' interrupt handler leans on each. They are as the
//   data sheet has them unless set_mame_fifo(true) (--mame-fdc: the oracle's answers):
//     - a read asks for service when the FIFO holds 16 - <threshold> bytes (the games set the
//       threshold to 15: at the first byte, with 238 us to be served in). MAME: when it holds
//       <threshold> bytes, which leaves the handler 32 us.
//     - the status register's non-DMA bit is set for the whole execution phase: it is how the
//       handler tells a data byte from a result byte. MAME drops it between two sectors.
//     - the result phase comes once the host has taken a read's last bytes out of the FIFO. MAME:
//       at the end of the last sector, whatever is left in the FIFO.
//   With MAME's, a handler late by some tens of microseconds takes FIFO data for the result, the
//   game's try fails and it tries again (ten times, then DISK ERROR): in MAME itself about every
//   third read fails that way. With the data sheet's the pin goes up at every byte the handler
//   left, so the board passes it on as the level it is (firebeat/board.cpp, the extend board's
//   latch). oracle/mame-patches/firebeat-fdc.patch is the same for MAME: the two then send the
//   same commands and get the same answers, without a failed try.
//
// The disk is a raw sector image: 80 cylinders x 2 sides x 18 sectors x 512 bytes, in that order.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <vector>

namespace hk {

class Timer;

class Fdc37c665 {
public:
    static constexpr int kCylinders = 80, kSides = 2, kSectors = 18, kSectorBytes = 512;
    static constexpr size_t kDiskBytes = (size_t)kCylinders * kSides * kSectors * kSectorBytes;   // 1,474,560

    Fdc37c665();
    ~Fdc37c665();
    Fdc37c665(const Fdc37c665 &) = delete;
    Fdc37c665 &operator=(const Fdc37c665 &) = delete;

    uint8_t read(int port);              // port = ISA address 0x3f0..0x3f7
    void write(int port, uint8_t v);
    std::function<void(bool)> irq;

    // The drive's disk. insert() takes a whole image (kDiskBytes; false and nothing done for
    // another size); a disk already in is taken out first. The drive's disk-change line then
    // tells the game, at its next look, that the disk is another; not for a disk that is in the
    // drive when the machine is turned on (at_power_on).
    bool insert(std::vector<uint8_t> image, bool write_protected, bool at_power_on = false);
    void eject();
    bool has_disk() const { return !disk_.empty(); }
    bool write_protected() const { return write_protected_; }
    const std::vector<uint8_t> &disk() const { return disk_; }
    // Moves each time a write changes a byte of the disk: "is the disk as its file has it"
    uint64_t disk_writes() const { return disk_writes_; }
    // The game is writing a sector at this moment (a disk taken out now loses the rest of it)
    bool writing() const { return x_ == X_WRITE || x_ == X_WRITE_END; }
    bool motor_on() const { return (dor_ & 0x10) != 0; }
    // The FIFO's requests and the non-DMA status bit as MAME has them (above)
    void set_mame_fifo(bool on) { mame_fifo_ = on; }
    // The interrupt pin's level, with a disk (false for the drive that never held one: its
    // interrupts are moments, as before)
    bool irq_line() const { return model_ && cur_irq_; }

    // A blank disk as a PC formats it (FAT12, 2880 sectors, no label), which is what the games
    // want put in: they make their files on it but never format one. `serial`: the volume's number.
    static std::vector<uint8_t> blank_disk(uint32_t serial = 0x48524b55);

private:
    // ---- the drive that never held a disk ---------------------------------------------------
    uint8_t empty_read(int port);
    void empty_write(int port, uint8_t v);
    void command_complete();
    void raise_irq(uint8_t st0, uint8_t pcn);

    uint8_t dor_ = 0;
    std::vector<uint8_t> cmd_;
    std::deque<uint8_t> result_;
    uint8_t pcn_[4]{};
    uint8_t busy_ = 0;               // MSR drive-busy bits: set by seek/recalibrate until sensed
    int sensing_drive_ = -1;         // drive whose SENSE INTERRUPT result is being read
    // pending "seek end" interrupts, reported by SENSE INTERRUPT STATUS
    std::deque<std::pair<uint8_t, uint8_t>> irq_status_;
    bool config_mode_ = false;
    uint8_t config_index_ = 0;
    uint8_t config_regs_[64]{};

    // ---- the controller and drive with a disk (MAME's) ---------------------------------------
    enum Phase { PH_IDLE, PH_CMD, PH_EXEC, PH_RESULT };
    enum Seek { S_IDLE, S_STEP_SIGNAL, S_STEP_TIME, S_STEP_TIME_DONE, S_WAIT_DONE };
    struct Unit {                    // one of the four drive selects (only 0 has a drive)
        enum { NONE, RECALIBRATE, SEEK } moving = NONE;
        Seek sub = S_IDLE;
        uint8_t pcn = 0, target = 0, st0 = 0;
        bool st0_filled = false, ready = false, toward_zero = false;
        int counter = 0;
        std::unique_ptr<Timer> timer;
    };
    // what the data command in execution waits for next
    enum Xfer {
        X_NONE,
        X_STEP_SIGNAL,    // an implied seek's step pulse
        X_STEP_TIME,      // ... and its step time
        X_WAIT_FLUX,      // nothing turns under the head (no disk, or the motor is off): MAME waits too
        X_ID,             // the end of the next sector ID to pass
        X_SCAN_FAIL,      // the second index pulse without the wanted ID
        X_READ,           // the end of the next data byte
        X_READ_END,       // the end of the sector's CRC
        X_WRITE,          // the start of the next data byte
        X_WRITE_END       // the end of the sector's CRC and gap
    };

    uint8_t model_read(int port);
    void model_write(int port, uint8_t v);
    void start_model();
    void soft_reset();
    void end_reset();
    void poll_ready();
    void check_irq();
    uint8_t msr() const;
    uint8_t fifo_read();
    void fifo_write(uint8_t v);
    int check_command() const;
    void execute_command(int cmd);
    void command_end_seek(Unit &u, int id);
    void command_done();
    void seek_continue(int id, bool from_timer);
    void fifo_expect(int size, bool write);
    void fifo_push(uint8_t v, bool internal);
    uint8_t fifo_pop(bool internal);
    void enable_transfer();
    void disable_transfer();
    void set_motor(bool on);
    void step(bool toward_zero);
    bool turning() const { return rev_start_ != UINT64_MAX; }
    bool readable() const;
    uint64_t delay_cycles(int cycles) const;
    void xfer_at(Xfer x, uint64_t when);
    void xfer_event();
    void head_loaded(uint64_t t);
    void scan_begin(uint64_t t);
    void scan_next(uint64_t t);
    bool next_sector();
    size_t sector_offset() const;

    bool model_ = false;             // a disk has been put in: MAME's controller from here on
    Phase phase_ = PH_IDLE;
    Unit unit_[4];
    uint8_t drive_busy_ = 0;
    bool irq_ = false, internal_drq_ = false, cur_irq_ = false, locked_ = false;
    uint8_t dsr_ = 0, fifocfg_ = 0x20, precomp_ = 0, perpmode_ = 0;
    uint16_t spec_ = 0;
    uint8_t command_[16]{}, result_buf_[16]{};
    int command_pos_ = 0, result_pos_ = 0;
    uint8_t fifo_[16]{};
    int fifo_pos_ = 0, fifo_expected_ = 0;
    bool fifo_write_ = false, xfer_in_progress_ = false;
    uint8_t st1_ = 0, st2_ = 0;
    uint64_t poll_base_ = 0;         // when the drive-ready polling began (the end of a reset)
    std::unique_ptr<Timer> poll_timer_, xfer_timer_;
    Xfer x_ = X_NONE;
    uint64_t xt_ = 0;                // the time the data command's current wait ends
    int xunit_ = 0;
    int scan_sector_ = 0;            // the sector (0..17) whose ID passes next
    uint64_t scan_rev_ = 0;          // ... and the start of its turn of the disk
    uint64_t scan_fail_at_ = 0;
    uint64_t sector_at_ = 0;         // the start of the sector in transfer
    int byte_ = 0;
    bool data_lost_ = false;         // the disk went while its sector was read
    bool mame_fifo_ = false;
    bool result_waits_ = false;      // a read is over, its last bytes still in the FIFO: the result phase waits for them

    // the drive (unit 0)
    std::vector<uint8_t> disk_;
    bool write_protected_ = false;
    uint64_t disk_writes_ = 0;
    int cyl_ = 0;                    // where the head is
    int side_ = 0;
    bool dskchg_ = false;            // the disk-change line as MAME's dskchg_r(): true once a step found a disk in
    uint64_t rev_start_ = UINT64_MAX;   // the start of a turn of the disk (never: nothing turns)
    uint64_t stopped_at_ = 0;        // where in its turn the disk stopped
};

}  // namespace hk
