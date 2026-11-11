// SPDX-License-Identifier: GPL-3.0-or-later
#include "fdc.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "../core/sched.h"

namespace hk {

namespace {
// parameter bytes following each command byte (low 5 bits of the opcode)
int params_for(uint8_t c) {
    switch (c & 0x1f) {
    case 0x03: return 2;   // SPECIFY
    case 0x04: return 1;   // SENSE DRIVE STATUS
    case 0x07: return 1;   // RECALIBRATE
    case 0x08: return 0;   // SENSE INTERRUPT STATUS
    case 0x0f: return 2;   // SEEK
    case 0x10: return 0;   // VERSION
    case 0x12: return 1;   // PERPENDICULAR MODE
    case 0x13: return 3;   // CONFIGURE
    case 0x14: return 0;   // LOCK
    default: return -1;
    }
}

Fdc37c665 *g_fdc = nullptr;
std::function<void()> g_fdc_done;
Timer g_fdc_timer("fdc", [] { if (g_fdc_done) g_fdc_done(); });

// ---- with a disk: MAME's names for the bits ------------------------------------------------
enum : uint8_t { MSR_CB = 0x10, MSR_EXM = 0x20, MSR_DIO = 0x40, MSR_RQM = 0x80 };
enum : uint8_t { ST0_EC = 0x10, ST0_SE = 0x20, ST0_FAIL = 0x40, ST0_UNK = 0x80, ST0_ABRT = 0xc0 };
enum : uint8_t { ST1_MA = 0x01, ST1_NW = 0x02, ST1_ND = 0x04, ST1_OR = 0x10, ST1_DE = 0x20, ST1_EN = 0x80 };
enum : uint8_t { ST2_WC = 0x10, ST2_DD = 0x20 };
enum : uint8_t { ST3_TS = 0x08, ST3_T0 = 0x10, ST3_RY = 0x20, ST3_WP = 0x40 };
enum : uint8_t { FIF_THR = 0x0f, FIF_POLL = 0x10, FIF_DIS = 0x20, FIF_EIS = 0x40 };
enum {
    C_INCOMPLETE = -1, C_INVALID = -2,
    C_SPECIFY = 0, C_SENSE_DRIVE_STATUS, C_RECALIBRATE, C_SENSE_INTERRUPT_STATUS, C_SEEK, C_READ_DATA, C_WRITE_DATA,
    C_READ_ID, C_DUMP_REG, C_VERSION, C_PERPENDICULAR, C_CONFIGURE, C_LOCK
};

// The disk under the head. 300 rpm, 500 kbit/s MFM: 12,500 bytes a turn, 16 us each (1056 ticks:
// whole numbers all the way). A track as MAME's 1.44 MB format lays it out
// (lib/formats/pc_dsk.cpp, upd765_dsk.cpp): gap 4a 80, sync 12, index mark 4, gap 1 50; then
// for each sector: sync 12, ID mark 4, C H R N, CRC 2, gap 2 22, sync 12, data mark 4, 512
// bytes, CRC 2, gap 3 108; gap 4b (78 bytes) to the index.
constexpr uint64_t kRevTicks = CPU_HZ / 5;
constexpr int kTrackBytes = 12500;
constexpr uint64_t kByteTicks = kRevTicks / kTrackBytes;
constexpr int kFirstSector = 80 + 12 + 4 + 50;                     // the first sector's sync
constexpr int kSectorSpan = 12 + 4 + 4 + 2 + 22 + 12 + 4 + 512 + 2 + 108;
static_assert(kFirstSector + 18 * kSectorSpan <= kTrackBytes, "a track's sectors fit in a turn");
constexpr int kIdMark = 12;                                        // from a sector's start to its ID mark
constexpr int kIdEnd = kIdMark + 4 + 4 + 2;                        // ... to the end of the ID's CRC
constexpr int kData = kIdEnd + 22 + 12 + 4;                        // ... to its first data byte
constexpr int kDriveTracks = 84;                                   // how far the head goes (MAME's 3.5" HD drive)
constexpr int kRecalibrateSteps = 80;                              // MAME's n82077aa
const int kRates[4] = {500000, 300000, 250000, 1000000};
}  // namespace

Fdc37c665::Fdc37c665() {
    for (int id = 0; id < 4; id++)
        unit_[id].timer = std::make_unique<Timer>("fdc seek " + std::to_string(id), [this, id] {
            Unit &u = unit_[id];
            if (u.sub == S_STEP_TIME) u.sub = S_STEP_TIME_DONE;
            seek_continue(id, true);
        });
    poll_timer_ = std::make_unique<Timer>("fdc poll", [this] { poll_ready(); });
    xfer_timer_ = std::make_unique<Timer>("fdc data", [this] { xfer_event(); });
}

Fdc37c665::~Fdc37c665() {
    if (g_fdc == this) g_fdc = nullptr, g_fdc_done = nullptr, g_fdc_timer.cancel();
}

uint8_t Fdc37c665::read(int port) { return model_ ? model_read(port) : empty_read(port); }
void Fdc37c665::write(int port, uint8_t v) { model_ ? model_write(port, v) : empty_write(port, v); }

// ============================================================================================
// The drive that never held a disk
// ============================================================================================

void Fdc37c665::raise_irq(uint8_t st0, uint8_t pcn) {
    irq_status_.emplace_back(st0, pcn);
    if (irq) irq(true);
}

uint8_t Fdc37c665::empty_read(int port) {
    switch (port & 7) {
    case 0:
    case 1:
        if (config_mode_ && (port & 7) == 1) return config_regs_[config_index_ & 63];
        return 0xff;
    case 2: return dor_;
    case 4: {                                            // main status register
        uint8_t msr = 0x80 | busy_;                      // RQM + drives still seeking/unsensed
        if (!result_.empty()) msr |= 0x40 | 0x10;        // DIO + busy: result to read
        else if (!cmd_.empty()) msr |= 0x10;
        return msr;
    }
    case 5: {                                            // FIFO
        if (result_.empty()) return 0xff;
        uint8_t v = result_.front();
        result_.pop_front();
        if (sensing_drive_ >= 0 && result_.size() == 1) {   // ST0 of a seek-end status: drive no longer busy
            busy_ &= ~(1u << sensing_drive_);
            sensing_drive_ = -1;
        }
        return v;
    }
    case 7: return 0x80;                                 // DIR: disk changed / no disk
    default: return 0xff;
    }
}

void Fdc37c665::empty_write(int port, uint8_t v) {
    switch (port & 7) {
    case 0:                                              // config port: 0x55 0x55 enters, 0xaa leaves
        if (v == 0x55) config_mode_ = true;
        else if (v == 0xaa) config_mode_ = false;
        else if (config_mode_) config_index_ = v;
        break;
    case 1:
        if (config_mode_) config_regs_[config_index_ & 63] = v;
        break;
    case 2: {                                            // digital output register
        uint8_t old = dor_;
        dor_ = v;
        if (!(old & 0x04) && (v & 0x04)) {               // leaving reset: polling interrupt x4
            cmd_.clear();
            result_.clear();
            irq_status_.clear();
            for (int d = 0; d < 4; d++) irq_status_.emplace_back(0xc0 | d, pcn_[d]);
            if (irq) irq(true);
        }
        break;
    }
    case 4: break;                                       // data rate select
    case 5:                                              // FIFO: command and parameters
        cmd_.push_back(v);
        if (params_for(cmd_[0]) < 0) {
            fprintf(stderr, "fdc: unsupported command %02x\n", cmd_[0]);
            cmd_.clear();
            result_ = {0x80};                            // invalid command
        } else if ((int)cmd_.size() == params_for(cmd_[0]) + 1) {
            command_complete();
        }
        break;
    case 7: break;                                       // configuration control
    default: break;
    }
}

void Fdc37c665::command_complete() {
    std::vector<uint8_t> c = cmd_;
    cmd_.clear();
    switch (c[0] & 0x1f) {
    case 0x03: case 0x12: break;                         // SPECIFY, PERPENDICULAR: no result
    case 0x13:                                           // CONFIGURE: POLL=1 cancels reset polling interrupts
        if (c[2] & 0x10) {
            while (!irq_status_.empty() && (irq_status_.front().first & 0xc0) == 0xc0) irq_status_.pop_front();
            if (irq_status_.empty() && irq) irq(false);
        }
        break;
    case 0x14: result_ = {(uint8_t)((c[0] & 0x80) ? 0x10 : 0)}; break;
    case 0x10: result_ = {0x90}; break;                  // VERSION: enhanced controller
    case 0x04: result_ = {(uint8_t)(0x28 | (c[1] & 3) | (pcn_[c[1] & 3] == 0 ? 0x10 : 0))}; break;
    case 0x08:                                           // SENSE INTERRUPT STATUS
        if (irq_status_.empty()) {
            result_ = {0x80};
        } else {
            auto [st0, pcn] = irq_status_.front();
            irq_status_.pop_front();
            result_ = {st0, pcn};
            if ((st0 & 0xe0) == 0x20) sensing_drive_ = st0 & 3;
        }
        if (irq_status_.empty() && irq) irq(false);
        break;
    case 0x07:                                           // RECALIBRATE: seek end after the steps
    case 0x0f: {                                         // SEEK
        int d = c[1] & 3;
        uint8_t target = (c[0] & 0x1f) == 0x07 ? 0 : c[2];
        busy_ |= 1u << d;
        g_fdc = this;
        g_fdc_done = [this, d, target] {
            pcn_[d] = target;
            raise_irq((uint8_t)(0x20 | d), target);
        };
        g_fdc_timer.in(ticks_from_us(3000));             // a few step times
        break;
    }
    }
}

// ============================================================================================
// The disk
// ============================================================================================

std::vector<uint8_t> Fdc37c665::blank_disk(uint32_t serial) {
    std::vector<uint8_t> d(kDiskBytes, 0);
    static const uint8_t boot[] = {
        0xeb, 0x3c, 0x90, 'M', 'S', 'D', 'O', 'S', '5', '.', '0',   // jump, OEM name
        0x00, 0x02,      // bytes a sector: 512
        0x01,            // sectors a cluster
        0x01, 0x00,      // reserved sectors
        0x02,            // FATs
        0xe0, 0x00,      // root directory entries: 224
        0x40, 0x0b,      // sectors: 2880
        0xf0,            // media: 3.5" HD
        0x09, 0x00,      // sectors a FAT
        0x12, 0x00,      // sectors a track
        0x02, 0x00,      // heads
        0, 0, 0, 0,      // hidden sectors
        0, 0, 0, 0,      // (sectors, 32 bits)
        0x00, 0x00,      // drive number, reserved
        0x29,            // extended boot signature: the three fields that follow
        0, 0, 0, 0,      // volume serial number (below)
        'N', 'O', ' ', 'N', 'A', 'M', 'E', ' ', ' ', ' ', ' ', 'F', 'A', 'T', '1', '2', ' ', ' ', ' ',
        0xeb, 0xfe};     // not a system disk: stop
    memcpy(d.data(), boot, sizeof boot);
    for (int k = 0; k < 4; k++) d[39 + k] = (uint8_t)(serial >> (8 * k));
    d[510] = 0x55, d[511] = 0xaa;
    for (int fat = 0; fat < 2; fat++) {                  // the media byte and the end mark, in each FAT
        uint8_t *f = d.data() + (size_t)(1 + 9 * fat) * kSectorBytes;
        f[0] = 0xf0, f[1] = 0xff, f[2] = 0xff;
    }
    return d;
}

bool Fdc37c665::insert(std::vector<uint8_t> image, bool write_protected, bool at_power_on) {
    if (image.size() != kDiskBytes) return false;
    if (!model_) start_model();
    if (has_disk()) eject();
    disk_ = std::move(image);
    write_protected_ = write_protected;
    // MAME: a disk there at power-on has its change line quiet; one put in later leaves the line
    // saying "changed" (as taking the last one out set it) until the head is stepped
    if (at_power_on) dskchg_ = true;
    stopped_at_ = 0;
    rev_start_ = motor_on() ? now() : UINT64_MAX;
    if (x_ == X_WAIT_FLUX && turning()) scan_begin(now());
    return true;
}

void Fdc37c665::eject() {
    if (!has_disk()) return;
    disk_.clear();
    write_protected_ = false;
    dskchg_ = false;
    if (x_ == X_READ) data_lost_ = true;
    // (MAME: the index pulses of a motor left on go on without a disk; a command looking for a
    // sector then ends at the second one)
}

// The first disk: from the flat answers to MAME's controller, keeping what the game has set
void Fdc37c665::start_model() {
    model_ = true;
    g_fdc_timer.cancel();
    if (g_fdc == this) g_fdc = nullptr, g_fdc_done = nullptr;
    cmd_.clear();
    result_.clear();
    irq_status_.clear();
    busy_ = 0, sensing_drive_ = -1;
    for (int id = 0; id < 4; id++) unit_[id].pcn = pcn_[id];
    cyl_ = std::min<int>(pcn_[0], kDriveTracks - 1);
    soft_reset();
}

bool Fdc37c665::readable() const {
    return xunit_ == 0 && has_disk() && turning() && (dsr_ & 3) == 0 && cyl_ < kCylinders;
}

size_t Fdc37c665::sector_offset() const {
    return ((size_t)(cyl_ * kSides + side_) * kSectors + scan_sector_) * kSectorBytes;
}

void Fdc37c665::set_motor(bool on) {
    if (on && has_disk()) {
        if (turning()) return;
        rev_start_ = now() - stopped_at_;                // from where it stopped
        if (x_ == X_WAIT_FLUX) scan_begin(now());
        return;
    }
    if (!turning()) return;
    stopped_at_ = (now() - rev_start_) % kRevTicks;
    rev_start_ = UINT64_MAX;
    if (x_ == X_ID || x_ == X_SCAN_FAIL) x_ = X_WAIT_FLUX, xfer_timer_->cancel();   // no more index pulses: it waits
}

void Fdc37c665::step(bool toward_zero) {
    cyl_ = std::clamp(cyl_ + (toward_zero ? -1 : 1), 0, kDriveTracks - 1);
    if (has_disk()) dskchg_ = true;                      // a step with a disk in: the change line lets go
}

// ============================================================================================
// With a disk: the controller
// ============================================================================================

uint64_t Fdc37c665::delay_cycles(int cycles) const {
    return (uint64_t)cycles * CPU_HZ / (uint64_t)kRates[dsr_ & 3];
}

void Fdc37c665::check_irq() {
    const bool was = cur_irq_;
    cur_irq_ = (irq_ || internal_drq_) && (dor_ & 0x04);
    if (cur_irq_ != was && irq) irq(cur_irq_);
}

void Fdc37c665::soft_reset() {
    phase_ = PH_IDLE;
    for (int id = 0; id < 4; id++) {
        Unit &u = unit_[id];
        u.moving = Unit::NONE, u.sub = S_IDLE;
        u.ready = false;
        u.st0 = (uint8_t)id, u.st0_filled = false;
        u.timer->cancel();
    }
    drive_busy_ = 0;
    irq_ = false, internal_drq_ = false;
    fifo_pos_ = 0, command_pos_ = 0, result_pos_ = 0;
    if (!locked_) fifocfg_ = FIF_DIS;
    x_ = X_NONE;
    xfer_timer_->cancel();
    st1_ = st2_ = 0;
    xfer_in_progress_ = false;
    result_waits_ = false;
    perpmode_ &= 0x3c;
    check_irq();
    if (dor_ & 0x04) end_reset();
    else poll_timer_->cancel();
}

void Fdc37c665::end_reset() {
    phase_ = PH_CMD;
    poll_base_ = now() + ticks_from_us(100);
    poll_timer_->at(poll_base_);
}

// MAME looks at the drives' ready lines every 1024 us from 100 us after a reset, and tells the host
// of each change (the "polling" interrupt, ST0 0xc0 + drive). The lines aren't wired here (every
// drive reads ready), so each drive changes once; the timer rests when none is left, or while the
// game has polling off (CONFIGURE), instead of ticking for nothing.
void Fdc37c665::poll_ready() {
    if (phase_ == PH_CMD && !(fifocfg_ & FIF_POLL) && !command_pos_) {
        for (int id = 0; id < 4; id++) {
            Unit &u = unit_[id];
            if (u.ready) continue;
            u.ready = true;
            if (!u.st0_filled) {
                u.st0 = (uint8_t)(ST0_ABRT | id);
                u.st0_filled = true;
                irq_ = true;
            }
        }
        check_irq();
    }
    bool left = false;
    for (auto &u : unit_) left |= !u.ready;
    if (left && !(fifocfg_ & FIF_POLL) && phase_ != PH_IDLE) poll_timer_->again(ticks_from_us(1024));
}

uint8_t Fdc37c665::msr() const {
    uint8_t m = 0;
    switch (phase_) {
    case PH_CMD:
        m |= MSR_RQM;
        if (command_pos_) m |= MSR_CB;
        break;
    case PH_EXEC: {
        m |= MSR_CB;
        // the non-DMA bit: the chip's through the execution phase, MAME's while a sector's bytes pass
        if ((spec_ & 1) && (xfer_in_progress_ || !mame_fifo_)) {
            m |= MSR_EXM;
            if (!fifo_write_) m |= MSR_DIO;
        }
        if (internal_drq_) {
            m |= MSR_RQM;
            if (!fifo_write_) m |= MSR_DIO;
        }
        break;
    }
    case PH_RESULT: m |= MSR_RQM | MSR_DIO | MSR_CB; break;
    case PH_IDLE: break;
    }
    for (int id = 0; id < 4; id++)
        if (unit_[id].moving != Unit::NONE) m |= 1u << id;
    return m | drive_busy_;
}

uint8_t Fdc37c665::model_read(int port) {
    switch (port & 7) {
    case 0: {                                            // status register A (PS/2 mode)
        const Unit &u = unit_[dor_ & 3];
        const bool drive = (dor_ & 3) == 0;
        uint8_t a = 0x40;                                // no second drive
        if (u.toward_zero) a |= 0x01;
        if (drive && turning() && (now() - rev_start_) % kRevTicks < ticks_from_us(2000)) a |= 0x04;   // index
        if ((dsr_ & 3) == 0 || (dsr_ & 3) == 3) a |= 0x08;
        if (drive && cyl_ != 0) a |= 0x10;
        if (u.sub == S_STEP_SIGNAL) a |= 0x20;
        if (cur_irq_) a |= 0x80;
        return a;
    }
    case 1:
        if (config_mode_) return config_regs_[config_index_ & 63];
        return (uint8_t)(0xc0 | ((dor_ & 1) << 6) | ((dor_ & 0x30) >> 4));   // status register B
    case 2: return dor_;
    case 3: return 0;                                    // tape drive register
    case 4: return msr();
    case 5: return fifo_read();
    case 7:                                              // digital input: the disk-change line, of a drive whose motor is on
        if ((dor_ & 3) == 0 && motor_on()) return dskchg_ ? 0x00 : 0x80;
        return 0x00;
    default: return 0;
    }
}

void Fdc37c665::model_write(int port, uint8_t v) {
    switch (port & 7) {
    case 0:                                              // config port: 0x55 0x55 enters, 0xaa leaves
        if (v == 0x55) config_mode_ = true;
        else if (v == 0xaa) config_mode_ = false;
        else if (config_mode_) config_index_ = v;
        break;
    case 1:
        if (config_mode_) config_regs_[config_index_ & 63] = v;
        break;
    case 2: {                                            // digital output: drive select, reset, motors
        const uint8_t diff = dor_ ^ v;
        dor_ = v;
        if (diff & 0x04) {
            if (v & 0x04) end_reset();
            else soft_reset();
        }
        if (diff & 0x10) set_motor(v & 0x10);
        check_irq();
        break;
    }
    case 4:                                              // data rate select, and a reset bit
        if (v & 0x80) soft_reset();
        dsr_ = v & 0x7f;
        break;
    case 5: fifo_write(v); break;
    case 7: dsr_ = (uint8_t)((dsr_ & 0xfc) | (v & 3)); break;   // configuration control: the rate
    default: break;
    }
}

uint8_t Fdc37c665::fifo_read() {
    uint8_t r = 0xff;
    irq_ = false;
    check_irq();
    switch (phase_) {
    case PH_CMD:
        if (command_pos_) fifo_write(0xff);
        break;
    case PH_EXEC:
        if (internal_drq_) return fifo_pop(false);
        break;
    case PH_RESULT:
        r = result_buf_[0];
        result_pos_--;
        memmove(result_buf_, result_buf_ + 1, (size_t)result_pos_);
        if (!result_pos_) phase_ = PH_CMD;
        else if (result_pos_ == 1) drive_busy_ = 0;      // the first byte of a sensed seek end: its drive is free
        break;
    case PH_IDLE: break;
    }
    return r;
}

int Fdc37c665::check_command() const {
    const uint8_t c = command_[0];
    const int n = command_pos_;
    switch (c & 0x1f) {                                  // the PS/2 controllers' own
    case 0x0e: return C_DUMP_REG;
    case 0x10: return C_VERSION;
    case 0x12: return n == 2 ? C_PERPENDICULAR : C_INCOMPLETE;
    case 0x13: return n == 4 ? C_CONFIGURE : C_INCOMPLETE;
    case 0x14: return C_LOCK;
    }
    switch (c) {
    case 0x03: return n == 3 ? C_SPECIFY : C_INCOMPLETE;
    case 0x04: return n == 2 ? C_SENSE_DRIVE_STATUS : C_INCOMPLETE;
    case 0x07: return n == 2 ? C_RECALIBRATE : C_INCOMPLETE;
    case 0x08: return C_SENSE_INTERRUPT_STATUS;
    case 0x0f: return n == 3 ? C_SEEK : C_INCOMPLETE;
    }
    switch (c & 0x1f) {
    case 0x05: return n == 9 ? C_WRITE_DATA : C_INCOMPLETE;
    case 0x06: return n == 9 ? C_READ_DATA : C_INCOMPLETE;
    case 0x0a: return n == 2 ? C_READ_ID : C_INCOMPLETE;
    }
    return C_INVALID;
}

void Fdc37c665::fifo_write(uint8_t v) {
    irq_ = false;
    check_irq();
    switch (phase_) {
    case PH_CMD: {
        command_[command_pos_++] = v;
        const int cmd = check_command();
        if (cmd == C_INCOMPLETE) break;
        if (cmd == C_INVALID) {
            // READ TRACK, FORMAT TRACK, the deleted-data and scan commands: no beatmania III uses them
            fprintf(stderr, "fdc: unsupported command %02x\n", command_[0]);
            phase_ = PH_RESULT;
            result_buf_[0] = ST0_UNK;
            result_pos_ = 1;
            command_pos_ = 0;
            return;
        }
        command_pos_ = 0, result_pos_ = 0;
        phase_ = PH_EXEC;
        fifo_pos_ = 0;
        if (internal_drq_) internal_drq_ = false, check_irq();
        execute_command(cmd);
        break;
    }
    case PH_EXEC:
        if (internal_drq_) fifo_push(v, false);
        break;
    default: break;
    }
}

void Fdc37c665::execute_command(int cmd) {
    const uint8_t *c = command_;
    Unit &u = unit_[c[1] & 3];
    switch (cmd) {
    case C_SPECIFY:
        spec_ = (uint16_t)((c[1] << 8) | c[2]);
        phase_ = PH_CMD;
        break;
    case C_SENSE_DRIVE_STATUS: {
        uint8_t st3 = c[1] & 7;
        if (u.ready) st3 |= ST3_RY;
        if ((c[1] & 3) == 0) st3 |= (uint8_t)((has_disk() && write_protected_ ? ST3_WP : 0) | (cyl_ == 0 ? ST3_T0 : 0) | ST3_TS);
        phase_ = PH_RESULT;
        result_buf_[0] = st3;
        result_pos_ = 1;
        break;
    }
    case C_SENSE_INTERRUPT_STATUS: {
        phase_ = PH_RESULT;
        int id = 0;
        while (id < 4 && !unit_[id].st0_filled) id++;
        if (id == 4) {
            result_buf_[0] = ST0_UNK;
            result_pos_ = 1;
            break;
        }
        unit_[id].st0_filled = false;
        result_buf_[0] = unit_[id].st0;
        result_buf_[1] = unit_[id].pcn;
        result_pos_ = 2;
        break;
    }
    case C_RECALIBRATE:
    case C_SEEK: {
        const int id = c[1] & 3;
        u.moving = cmd == C_SEEK ? Unit::SEEK : Unit::RECALIBRATE;
        u.sub = S_STEP_TIME_DONE;
        u.target = cmd == C_SEEK ? c[2] : 0;
        u.toward_zero = cmd == C_SEEK ? u.pcn > c[2] : true;
        u.counter = kRecalibrateSteps;
        u.ready = true;
        u.st0 = (uint8_t)id;
        phase_ = PH_CMD;
        seek_continue(id, false);
        break;
    }
    case C_CONFIGURE: {
        const bool was_off = fifocfg_ & FIF_POLL;
        fifocfg_ = c[2];
        precomp_ = c[3];
        phase_ = PH_CMD;
        if (was_off && !(fifocfg_ & FIF_POLL)) {         // polling again: at its next 1024 us
            const uint64_t period = ticks_from_us(1024), t = now();
            poll_timer_->at(t <= poll_base_ ? poll_base_ : poll_base_ + (t - poll_base_ + period - 1) / period * period);
        }
        break;
    }
    case C_DUMP_REG:
        phase_ = PH_RESULT;
        for (int id = 0; id < 4; id++) result_buf_[id] = unit_[id].pcn;
        result_buf_[4] = (uint8_t)(spec_ >> 8);
        result_buf_[5] = (uint8_t)spec_;
        result_buf_[6] = 0;                              // (MAME: the last sector's size, 512, in a byte)
        result_buf_[7] = (uint8_t)((locked_ ? 0x80 : 0) | (perpmode_ & 0x3f));
        result_buf_[8] = fifocfg_;
        result_buf_[9] = precomp_;
        result_pos_ = 10;
        break;
    case C_VERSION:
        phase_ = PH_RESULT;
        result_buf_[0] = 0x90;                           // an enhanced controller
        result_pos_ = 1;
        break;
    case C_LOCK:
        locked_ = c[0] & 0x80;
        phase_ = PH_RESULT;
        result_buf_[0] = locked_ ? 0x10 : 0x00;
        result_pos_ = 1;
        break;
    case C_PERPENDICULAR:
        perpmode_ = (c[1] & 0x80) ? (c[1] & 0x3f) : (uint8_t)((perpmode_ & 0x3c) | (c[1] & 3));
        phase_ = PH_CMD;
        break;
    case C_READ_DATA:
    case C_WRITE_DATA:
    case C_READ_ID:
        xunit_ = c[1] & 3;
        side_ = (c[1] & 4) ? 1 : 0;
        u.st0 = c[1] & 7;
        u.ready = true;
        st1_ = st2_ = 0;
        if (cmd == C_WRITE_DATA && xunit_ == 0 && has_disk() && write_protected_) {
            u.st0 |= ST0_FAIL;
            st1_ = ST1_NW;
            command_done();
            break;
        }
        if (cmd != C_READ_ID) st1_ = ST1_MA | ST1_ND;
        // (MAME starts a timer of the head load time here and goes on without waiting for it)
        head_loaded(now());
        break;
    }
}

// A seek or a recalibrate, a step at a time (MAME's seek_continue): the step pulse 2.5 us, then
// the step time the game set (SPECIFY), and 10 us (a seek) or 20 us (a recalibrate) at the end
void Fdc37c665::seek_continue(int id, bool from_timer) {
    Unit &u = unit_[id];
    auto wait = [&](uint64_t ticks) { from_timer ? u.timer->again(ticks) : u.timer->in(ticks); };
    switch (u.sub) {
    case S_STEP_TIME_DONE: {                             // where is the head now?
        bool done;
        if (u.moving == Unit::RECALIBRATE) {
            u.counter--;
            done = id == 0 && cyl_ == 0;                 // the drive's track 0 switch
            if (done) u.pcn = 0;
            else if (!u.counter) {
                u.st0 |= (uint8_t)(ST0_FAIL | ST0_SE | ST0_EC | id);
                command_end_seek(u, id);
                return;
            }
        } else {
            done = u.pcn == u.target;
        }
        if (done) {
            u.sub = S_WAIT_DONE;
            wait(ticks_from_us(u.moving == Unit::RECALIBRATE ? 20 : 10));
            return;
        }
        if (id == 0) step(u.toward_zero);
        u.sub = S_STEP_SIGNAL;
        wait(ticks_from_us(2.5));
        return;
    }
    case S_STEP_SIGNAL:
        if (u.moving == Unit::SEEK) u.pcn = (uint8_t)(u.pcn > u.target ? u.pcn - 1 : u.pcn + 1);
        u.sub = S_STEP_TIME;
        wait(delay_cycles(500 * (16 - (spec_ >> 12))));
        return;
    case S_WAIT_DONE:
        u.st0 |= (uint8_t)(ST0_SE | id);
        command_end_seek(u, id);
        return;
    default: return;
    }
}

void Fdc37c665::command_end_seek(Unit &u, int id) {
    u.moving = Unit::NONE, u.sub = S_IDLE;
    irq_ = true;
    u.st0_filled = true;
    drive_busy_ |= (uint8_t)(1u << id);
    check_irq();
}

// The end of a read or a write: the seven result bytes, and the interrupt
void Fdc37c665::command_done() {
    x_ = X_NONE;
    xfer_timer_->cancel();
    xfer_in_progress_ = false;
    // the chip: "reads require the host to remove the remaining data so that the result phase may
    // be entered" (MAME enters it at once, and what the FIFO still held is gone)
    if (!mame_fifo_ && (command_[0] & 0x1f) == 0x06 && fifo_pos_ && internal_drq_) {
        result_waits_ = true;
        return;
    }
    result_waits_ = false;
    phase_ = PH_RESULT;
    result_buf_[0] = unit_[xunit_].st0;
    result_buf_[1] = st1_;
    result_buf_[2] = st2_;
    memcpy(result_buf_ + 3, command_ + 2, 4);            // C H R N: where it stopped
    result_pos_ = 7;
    irq_ = true;
    check_irq();
}

// ---- the FIFO (MAME's): its interrupt is the non-DMA mode's request for service ----------------

void Fdc37c665::enable_transfer() {
    if ((spec_ & 1) && !internal_drq_) internal_drq_ = true, check_irq();
}

void Fdc37c665::disable_transfer() {
    if (spec_ & 1) internal_drq_ = false, check_irq();
}

void Fdc37c665::fifo_expect(int size, bool write) {
    fifo_expected_ = size;
    fifo_write_ = write;
    if (fifo_write_) enable_transfer();
}

void Fdc37c665::fifo_push(uint8_t v, bool internal) {
    if (fifo_expected_ == 0) return;
    if (fifo_pos_ == 16) {
        if (internal) st1_ |= ST1_OR;                    // the game didn't take the bytes in time: overrun
        return;
    }
    fifo_[fifo_pos_++] = v;
    fifo_expected_--;
    // a read's request for service: the chip's when the FIFO holds 16 - <threshold> bytes (the
    // host then has <threshold> byte times to come), MAME's when it holds <threshold> bytes
    const int thr = (fifocfg_ & FIF_THR) + 1;
    const int level = mame_fifo_ ? thr : std::max(16 - thr, 1);
    if (!fifo_write_ && (!fifo_expected_ || fifo_pos_ >= level || (fifocfg_ & FIF_DIS))) enable_transfer();
    if (fifo_write_ && (fifo_pos_ == 16 || !fifo_expected_ || (fifocfg_ & FIF_DIS))) disable_transfer();
}

uint8_t Fdc37c665::fifo_pop(bool internal) {
    if (!fifo_pos_) {
        if (internal) st1_ |= ST1_OR;                    // ... or didn't give them in time
        return 0;
    }
    const uint8_t r = fifo_[0];
    fifo_pos_--;
    memmove(fifo_, fifo_ + 1, (size_t)fifo_pos_);
    if (!fifo_write_ && !fifo_pos_) disable_transfer();
    const int thr = fifocfg_ & FIF_THR;
    if (fifo_write_ && fifo_expected_ && (fifo_pos_ <= thr || (fifocfg_ & FIF_DIS))) enable_transfer();
    if (result_waits_ && !fifo_pos_) command_done();     // the read's last byte is out: now the result
    return r;
}

// ---- a read or a write under the turning disk --------------------------------------------------

void Fdc37c665::xfer_at(Xfer x, uint64_t when) {
    x_ = x;
    xt_ = when;
    xfer_timer_->at(when);
}

// The command is taken (and, for a read with implied seeks on, the head on its cylinder): look for the sector
void Fdc37c665::head_loaded(uint64_t t) {
    Unit &u = unit_[xunit_];
    if ((command_[0] & 0x1f) == 0x06 && (fifocfg_ & FIF_EIS) && u.pcn != command_[2]) {
        u.st0 |= ST0_SE;
        if (xunit_ == 0) step(u.pcn > command_[2]);
        xfer_at(X_STEP_SIGNAL, t + ticks_from_us(2.5));
        return;
    }
    scan_begin(t);
}

// Look for the wanted sector's ID from t. Two index pulses without it and the command fails;
// with nothing turning there are no pulses, and it waits (until the game's own time-out resets it)
void Fdc37c665::scan_begin(uint64_t t) {
    if (xunit_ != 0 || !turning()) {
        x_ = X_WAIT_FLUX;
        xfer_timer_->cancel();
        return;
    }
    scan_fail_at_ = rev_start_ + ((t - rev_start_) / kRevTicks + 2) * kRevTicks;
    scan_next(t);
}

// The next sector ID whose mark starts at or after t
void Fdc37c665::scan_next(uint64_t t) {
    if (!readable()) {                                   // an empty drive, a track past the 80th, another data rate: no marks
        xfer_at(X_SCAN_FAIL, scan_fail_at_);
        return;
    }
    uint64_t rev = (t - rev_start_) / kRevTicks;
    const uint64_t pos = (t - rev_start_) % kRevTicks;
    const uint64_t first = (uint64_t)(kFirstSector + kIdMark) * kByteTicks, span = (uint64_t)kSectorSpan * kByteTicks;
    int s = pos <= first ? 0 : (int)((pos - first + span - 1) / span);
    if (s >= kSectors) s = 0, rev++;
    const uint64_t base = rev_start_ + rev * kRevTicks;
    const uint64_t id_end = base + (uint64_t)(kFirstSector + s * kSectorSpan + kIdEnd) * kByteTicks;
    if (id_end >= scan_fail_at_) {
        xfer_at(X_SCAN_FAIL, scan_fail_at_);
        return;
    }
    scan_sector_ = s;
    scan_rev_ = base;
    xfer_at(X_ID, id_end);
}

// After a sector: the next one, the other side (a multi-track command), or the end. With no
// terminal count line (non-DMA mode has none here) a transfer ends at the cylinder's last sector
// with "end of cylinder", which is how the games read and write a cylinder in one command.
bool Fdc37c665::next_sector() {
    uint8_t *c = command_;
    bool done = false;
    if (c[4] == c[6]) {
        if (c[0] & 0x80) {
            c[3] ^= 1;
            c[4] = 1;
            if (c[3] & 1) unit_[xunit_].st0 |= 4;
            side_ = c[3] & 1;
        }
        if (!(c[0] & 0x80) || !(c[3] & 1)) {
            unit_[xunit_].st0 |= ST0_FAIL;
            st1_ |= ST1_EN;
            done = true;
        }
    } else {
        c[4]++;
    }
    return done;
}

void Fdc37c665::xfer_event() {
    const uint64_t t = xt_;
    Unit &u = unit_[xunit_];
    const int op = command_[0] & 0x1f;                   // 0x06 read, 0x05 write, 0x0a read ID
    switch (x_) {
    case X_STEP_SIGNAL: xfer_at(X_STEP_TIME, t + delay_cycles(500 * (16 - (spec_ >> 12)))); break;
    case X_STEP_TIME:
        u.pcn = (uint8_t)(u.pcn > command_[2] ? u.pcn - 1 : u.pcn + 1);
        head_loaded(t);
        break;
    case X_SCAN_FAIL:
        u.st0 |= ST0_FAIL;
        if (op == 0x0a) st1_ |= ST1_ND | ST1_MA;
        command_done();
        break;
    case X_ID: {
        if (!readable()) {                               // the disk went meanwhile
            xfer_at(X_SCAN_FAIL, scan_fail_at_);
            break;
        }
        const uint8_t id[4] = {(uint8_t)cyl_, (uint8_t)side_, (uint8_t)(scan_sector_ + 1), 2};
        if (op == 0x0a) {
            memcpy(command_ + 2, id, 4);
            command_done();
            break;
        }
        st1_ &= ~ST1_MA;
        st1_ |= ST1_ND;
        if (memcmp(id, command_ + 2, 4) != 0) {
            if (op == 0x06 && id[0] != command_[2]) st2_ |= ST2_WC;
            scan_next(t + 1);
            break;
        }
        st1_ &= ~ST1_ND;
        if (op == 0x06) st2_ &= ~ST2_WC;
        xfer_in_progress_ = true;
        sector_at_ = scan_rev_ + (uint64_t)(kFirstSector + scan_sector_ * kSectorSpan) * kByteTicks;
        byte_ = 0;
        data_lost_ = false;
        if (op == 0x06) {
            fifo_expect(kSectorBytes, false);
            xfer_at(X_READ, sector_at_ + (uint64_t)(kData + 1) * kByteTicks);   // its first byte, read whole
        } else {
            fifo_expect(kSectorBytes, true);             // (asks the game for bytes at once)
            // gap 2 passes (22 bytes), then the write: sync 12, data mark 4, and the bytes
            xfer_at(X_WRITE, t + (uint64_t)(22 + 12 + 4) * kByteTicks);
        }
        break;
    }
    case X_READ:
        fifo_push(has_disk() && !data_lost_ ? disk_[sector_offset() + (size_t)byte_] : 0, true);
        if (++byte_ < kSectorBytes) xfer_at(X_READ, t + kByteTicks);
        else xfer_at(X_READ_END, t + 2 * kByteTicks);    // the CRC
        break;
    case X_READ_END:
        xfer_in_progress_ = false;
        if (data_lost_) {                                // read as nothing: a data CRC error
            u.st0 |= ST0_FAIL;
            st1_ |= ST1_DE;
            st2_ |= ST2_DD;
            command_done();
        } else if (next_sector()) {
            command_done();
        } else {
            scan_begin(t);
        }
        break;
    case X_WRITE: {
        const uint8_t b = fifo_pop(true);
        if (has_disk()) {
            uint8_t &d = disk_[sector_offset() + (size_t)byte_];
            if (d != b) d = b, disk_writes_++;
        }
        if (++byte_ < kSectorBytes) xfer_at(X_WRITE, t + kByteTicks);
        else xfer_at(X_WRITE_END, t + (uint64_t)(1 + 2 + command_[7]) * kByteTicks);   // its last byte, the CRC, the gap
        break;
    }
    case X_WRITE_END:
        xfer_in_progress_ = false;
        if (next_sector()) command_done();
        else scan_begin(t);
        break;
    case X_NONE:
    case X_WAIT_FLUX: break;
    }
}

}  // namespace hk
