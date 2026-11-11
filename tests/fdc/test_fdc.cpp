// SPDX-License-Identifier: GPL-3.0-or-later
// beatmania III's floppy controller and drive (runtime/dev/fdc.h) against a made-up disk, driven
// the way the games drive it (non-DMA, a cylinder a command):
//   - a drive that never held a disk answers the games' FDD check as it always has
//   - a cylinder read whole, written whole and read back; the bytes land where they belong
//   - the disk-change line across taking the disk out and putting one in
//   - a write-protected disk refuses the write and says so
//   - a FIFO left unserved is an overrun, a sector that isn't there a "no data", no disk a wait
//   - a host that comes 100 us late still gets every byte and a true result with the FIFO as the
//     chip's data sheet has it (the default); with MAME's (--mame-fdc) it takes data for the result
#include <cstdio>
#include <cstring>
#include <vector>

#include "core/sched.h"
#include "dev/fdc.h"

using namespace hk;

namespace {
int g_fail = 0;
#define CHECK(c)                                                                 \
    do {                                                                         \
        if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } \
    } while (0)
#define CHECK_EQ(a, b)                                                           \
    do {                                                                         \
        const long long va = (long long)(a), vb = (long long)(b);                \
        if (va != vb) { fprintf(stderr, "FAIL %s:%d: %s = 0x%llx, not 0x%llx\n", __FILE__, __LINE__, #a, va, vb); g_fail++; } \
    } while (0)

enum { DOR = 2, MSR = 4, FIFO = 5, DIR = 7 };
const size_t kCylinderBytes = (size_t)Fdc37c665::kSides * Fdc37c665::kSectors * Fdc37c665::kSectorBytes;

void run_us(double us) { sched_idle(ticks_from_us(us)); }
double seconds() { return (double)now() / CPU_HZ; }

struct Machine {
    Fdc37c665 fdc;
    int interrupts = 0;
    bool line = false;
    explicit Machine(bool mame_fifo = false) {
        sched_reset();
        fdc.set_mame_fifo(mame_fifo);
        fdc.irq = [this](bool on) {
            if (on && !line) interrupts++;
            line = on;
        };
    }
    void command(std::initializer_list<uint8_t> bytes) {
        for (uint8_t b : bytes) {
            CHECK_EQ(fdc.read(MSR) & 0xc0, 0x80);   // ready for a byte from the host
            fdc.write(FIFO, b);
        }
    }
    std::vector<uint8_t> result(int n) {
        std::vector<uint8_t> r;
        for (int k = 0; k < n; k++) {
            CHECK_EQ(fdc.read(MSR) & 0xd0, 0xd0);   // a result byte waits
            r.push_back(fdc.read(FIFO));
        }
        return r;
    }
    // The games' start: reset, FIFO on with a threshold of 15 and polling off, 500 kbit/s, an 11 ms
    // step and non-DMA mode, then the motor
    void start() {
        fdc.write(DOR, 0x00);
        fdc.write(DOR, 0x08);
        fdc.write(DOR, 0x0c);
        command({0x13, 0x00, 0x1e, 0x00});
        fdc.write(7, 0x00);
        command({0x03, 0x5f, 0x11});
        fdc.write(DOR, 0x1c);
        run_us(500000);
    }
    // a seek or a recalibrate to its interrupt, then SENSE INTERRUPT STATUS: {ST0, cylinder}
    std::vector<uint8_t> seek(std::initializer_list<uint8_t> bytes) {
        const int before = interrupts;
        command(bytes);
        CHECK(fdc.read(MSR) & 0x01);                // drive 0 is busy
        for (int k = 0; k < 2000000 && interrupts == before; k++) run_us(1);
        CHECK(interrupts == before + 1);
        command({0x08});
        return result(2);
    }
    // READ DATA of a whole cylinder, served as the game's interrupt handler does: what came, and
    // the seven result bytes
    std::vector<uint8_t> read_cylinder(int cyl, std::vector<uint8_t> &res, int first_sector = 1, bool serve = true, double every_us = 16) {
        std::vector<uint8_t> data;
        command({0xc6, 0x00, (uint8_t)cyl, 0x00, (uint8_t)first_sector, 0x02, 0x12, 0x1b, 0xff});
        const double until = seconds() + 2.0;
        // (unserved, the FIFO's bytes between two sectors look like a result to the status register,
        // as in MAME: there the read is left alone for longer than it can take)
        if (!serve) run_us(700000);
        for (;;) {
            uint8_t m = fdc.read(MSR);
            if ((m & 0xf0) == 0xd0) break;          // the result phase
            while (((m = fdc.read(MSR)) & 0xf0) == 0xf0) data.push_back(fdc.read(FIFO));
            if (seconds() > until) { CHECK(!"the read never ended"); break; }
            run_us(every_us);
        }
        res = result(7);
        if (every_us == 16) CHECK_EQ(fdc.read(MSR), 0x80);
        return data;
    }
    std::vector<uint8_t> write_cylinder(int cyl, const std::vector<uint8_t> &data) {
        command({0xc5, 0x00, (uint8_t)cyl, 0x00, 0x01, 0x02, 0x12, 0x1b, 0xff});
        size_t at = 0;
        const double until = seconds() + 2.0;
        for (;;) {
            uint8_t m = fdc.read(MSR);
            if ((m & 0xf0) == 0xd0) break;
            while (((m = fdc.read(MSR)) & 0xf0) == 0xb0 && at < data.size()) fdc.write(FIFO, data[at++]);
            if (seconds() > until) { CHECK(!"the write never ended"); break; }
            run_us(16);
        }
        CHECK_EQ(at, data.size());
        return result(7);
    }
};

// a disk whose every byte says where it is
std::vector<uint8_t> made_up_disk() {
    std::vector<uint8_t> d(Fdc37c665::kDiskBytes);
    for (size_t k = 0; k < d.size(); k++) {
        const size_t sector = k / 512, in = k % 512;
        d[k] = (uint8_t)(sector * 7 + in * 13 + (in >> 8) + (sector >> 8) * 3);
    }
    return d;
}

// ---- a drive that never held a disk: the FDD check's answers, as before disks were known --------
void test_empty_drive() {
    Machine m;
    Fdc37c665 &f = m.fdc;
    CHECK_EQ(f.read(0), 0xff);
    CHECK_EQ(f.read(DIR), 0x80);
    f.write(DOR, 0x00);
    f.write(DOR, 0x08);
    CHECK_EQ(m.interrupts, 0);
    f.write(DOR, 0x0c);                             // leaving reset: the polling interrupt, at once
    CHECK_EQ(m.interrupts, 1);
    CHECK_EQ(f.read(MSR), 0x80);
    f.write(FIFO, 0x13);
    CHECK_EQ(f.read(MSR), 0x90);
    f.write(FIFO, 0x00), f.write(FIFO, 0x1e), f.write(FIFO, 0x00);   // CONFIGURE, polling off
    CHECK_EQ(f.read(MSR), 0x80);
    CHECK(!m.line);
    f.write(FIFO, 0x08);                            // nothing to sense any more
    CHECK_EQ(f.read(MSR), 0xd0);
    CHECK_EQ(f.read(FIFO), 0x80);
    f.write(FIFO, 0x03), f.write(FIFO, 0x5f), f.write(FIFO, 0x11);   // SPECIFY
    f.write(DOR, 0x1c);
    CHECK_EQ(f.read(DOR), 0x1c);

    f.write(FIFO, 0x07), f.write(FIFO, 0x00);       // RECALIBRATE: 3 ms, whatever the distance
    CHECK_EQ(f.read(MSR), 0x81);
    run_us(2990);
    CHECK_EQ(m.interrupts, 1);
    run_us(20);
    CHECK_EQ(m.interrupts, 2);
    f.write(FIFO, 0x08);
    CHECK_EQ(f.read(MSR), 0xd1);
    CHECK_EQ(f.read(FIFO), 0x20);
    CHECK_EQ(f.read(MSR), 0xd0);                    // the drive's busy bit goes with the first byte
    CHECK_EQ(f.read(FIFO), 0x00);
    CHECK_EQ(f.read(MSR), 0x80);

    f.write(FIFO, 0x0f), f.write(FIFO, 0x04), f.write(FIFO, 0x01);   // SEEK to cylinder 1
    run_us(3010);
    CHECK_EQ(m.interrupts, 3);
    f.write(FIFO, 0x08);
    CHECK_EQ(f.read(FIFO), 0x20);
    CHECK_EQ(f.read(FIFO), 0x01);
    CHECK_EQ(f.read(DIR), 0x80);                    // no disk
    f.write(FIFO, 0x04), f.write(FIFO, 0x00);       // SENSE DRIVE STATUS
    CHECK_EQ(f.read(FIFO), 0x28);
    f.write(FIFO, 0x10);                            // VERSION
    CHECK_EQ(f.read(FIFO), 0x90);
    f.write(FIFO, 0x94);                            // LOCK
    CHECK_EQ(f.read(FIFO), 0x10);
    f.write(FIFO, 0xc6);                            // READ DATA: not a command of this drive
    CHECK_EQ(f.read(MSR), 0xd0);
    CHECK_EQ(f.read(FIFO), 0x80);
    CHECK(!f.has_disk());
}

// ---- a disk: read, write, read back -------------------------------------------------------------
void test_read_write(bool mame) {
    Machine m(mame);
    Fdc37c665 &f = m.fdc;
    const std::vector<uint8_t> disk = made_up_disk();
    CHECK(!f.insert(std::vector<uint8_t>(1000), false));   // not a disk's image
    CHECK(!f.has_disk());
    CHECK(f.insert(disk, false, true));
    CHECK_EQ(f.read(DIR), 0x00);                    // the motor is off: the change line isn't shown
    m.start();
    CHECK_EQ(f.read(DIR), 0x00);                    // in the drive since power-on: no change

    auto s = m.seek({0x07, 0x00});                  // already at track 0: 20 us
    CHECK_EQ(s[0], 0x20);
    CHECK_EQ(s[1], 0x00);
    double t0 = seconds();
    s = m.seek({0x0f, 0x04, 0x01});                 // one step: 11 ms
    CHECK_EQ(s[0], 0x20);
    CHECK_EQ(s[1], 0x01);
    CHECK(seconds() - t0 > 0.0110 && seconds() - t0 < 0.0111);
    CHECK_EQ(f.read(DIR), 0x00);
    m.command({0x04, 0x04});                        // SENSE DRIVE STATUS: ready, two-sided, head 1, not track 0
    CHECK_EQ(m.result(1)[0], 0x2c);
    s = m.seek({0x0f, 0x00, 0x00});
    CHECK_EQ(s[1], 0x00);
    m.command({0x04, 0x00});
    CHECK_EQ(m.result(1)[0], 0x38);                 // ... and at track 0

    // cylinder 0, both sides, in one command; it ends with "end of cylinder", as non-DMA reads do
    std::vector<uint8_t> res;
    const int irq0 = m.interrupts;
    t0 = seconds();
    std::vector<uint8_t> got = m.read_cylinder(0, res);
    CHECK_EQ(got.size(), kCylinderBytes);
    CHECK(got.size() == kCylinderBytes && memcmp(got.data(), disk.data(), kCylinderBytes) == 0);
    CHECK(res == (std::vector<uint8_t>{0x44, 0x80, 0x00, 0x00, 0x00, 0x01, 0x02}));
    // up to a turn for sector 1 to come round, two turns of data
    CHECK(seconds() - t0 > 0.38 && seconds() - t0 < 0.60);
    // the chip: an interrupt as soon as the FIFO has a byte (served every 16 us here: one a byte);
    // MAME: each time the FIFO reaches the threshold of 15 bytes. And one at the end
    if (mame) CHECK(m.interrupts - irq0 >= (int)(kCylinderBytes / 16) && m.interrupts - irq0 <= (int)(kCylinderBytes / 15) + 40);
    else CHECK(m.interrupts - irq0 >= (int)kCylinderBytes - 500 && m.interrupts - irq0 <= (int)kCylinderBytes + 40);

    // cylinder 2: write it whole, read it back
    s = m.seek({0x0f, 0x00, 0x02});
    CHECK_EQ(s[1], 0x02);
    std::vector<uint8_t> fresh(kCylinderBytes);
    for (size_t k = 0; k < fresh.size(); k++) fresh[k] = (uint8_t)(k * 31 + (k >> 9) + 5);
    const uint64_t writes0 = f.disk_writes();
    res = m.write_cylinder(2, fresh);
    CHECK(res == (std::vector<uint8_t>{0x44, 0x80, 0x00, 0x02, 0x00, 0x01, 0x02}));
    CHECK(f.disk_writes() != writes0);
    CHECK(!f.writing());
    const size_t at = 2 * kCylinderBytes;
    CHECK(memcmp(f.disk().data() + at, fresh.data(), kCylinderBytes) == 0);
    CHECK(memcmp(f.disk().data(), disk.data(), at) == 0);                                       // nothing before it
    CHECK(memcmp(f.disk().data() + at + kCylinderBytes, disk.data() + at + kCylinderBytes, disk.size() - at - kCylinderBytes) == 0);   // or after
    got = m.read_cylinder(2, res);
    CHECK(got == fresh);
    CHECK(res == (std::vector<uint8_t>{0x44, 0x80, 0x00, 0x02, 0x00, 0x01, 0x02}));
    // the same bytes written again: the disk hasn't changed
    const uint64_t writes1 = f.disk_writes();
    m.write_cylinder(2, fresh);
    CHECK(f.disk_writes() == writes1);

    // READ ID: a sector of the cylinder under the head
    m.command({0x4a, 0x00});
    while ((f.read(MSR) & 0xf0) != 0xd0) run_us(100);
    res = m.result(7);
    CHECK_EQ(res[0], 0x00);
    CHECK_EQ(res[3], 0x02);
    CHECK_EQ(res[4], 0x00);
    CHECK(res[5] >= 1 && res[5] <= 18);
    CHECK_EQ(res[6], 0x02);

    // a sector that isn't on the track: two index pulses, then "no data"
    t0 = seconds();
    m.command({0x46, 0x00, 0x02, 0x00, 0x13, 0x02, 0x13, 0x1b, 0xff});
    while ((f.read(MSR) & 0xf0) != 0xd0 && seconds() - t0 < 2) run_us(100);
    res = m.result(7);
    CHECK_EQ(res[0], 0x40);
    CHECK_EQ(res[1], 0x04);
    CHECK(seconds() - t0 > 0.2 && seconds() - t0 < 0.45);

    // a host that looks every 100 us. The chip asked at the first byte and keeps its non-DMA bit up:
    // nothing is lost, and the result is the result. MAME asks at 15 bytes and drops the bit between
    // sectors: the bytes waiting there look like the result (what makes the games try again)
    got = m.read_cylinder(2, res, 1, true, 100);
    if (mame) {
        CHECK(got.size() < kCylinderBytes);
    } else {
        CHECK(got == fresh);
        CHECK(res == (std::vector<uint8_t>{0x44, 0x80, 0x00, 0x02, 0x00, 0x01, 0x02}));
    }
    f.write(DOR, 0x18);                             // (MAME's is somewhere in the cylinder: start again)
    f.write(DOR, 0x1c);
    m.command({0x13, 0x00, 0x1e, 0x00});
    m.command({0x03, 0x5f, 0x11});

    // a FIFO nobody serves: the bytes past its sixteen are lost, and the result says so. The chip
    // keeps the result until the sixteen are taken; MAME gives it at once
    got = m.read_cylinder(2, res, 1, false);
    CHECK_EQ(got.size(), mame ? 0 : 16);
    CHECK_EQ(res[0], 0x44);
    CHECK_EQ(res[1], 0x90);

    // the motor off: the disk stops, a read finds nothing and waits; a reset ends the wait
    f.write(DOR, 0x0c);
    m.command({0xc6, 0x00, 0x02, 0x00, 0x01, 0x02, 0x12, 0x1b, 0xff});
    run_us(700000);
    CHECK_EQ(f.read(MSR), mame ? 0x10 : 0x70);      // (the non-DMA bit: an execution phase)
    f.write(DOR, 0x08);
    f.write(DOR, 0x0c);
    CHECK_EQ(f.read(MSR), 0x80);
}

// ---- the disk-change line, and write protect -----------------------------------------------------
void test_change_and_protect() {
    Machine m;
    Fdc37c665 &f = m.fdc;
    const std::vector<uint8_t> disk = made_up_disk();
    CHECK(f.insert(disk, false, true));
    m.start();
    m.seek({0x07, 0x00});
    CHECK_EQ(f.read(DIR), 0x00);

    f.eject();                                      // out: the line says so, and steps don't quiet it
    CHECK(!f.has_disk());
    CHECK_EQ(f.read(DIR), 0x80);
    m.seek({0x0f, 0x04, 0x01});
    CHECK_EQ(f.read(DIR), 0x80);
    f.write(DOR, 0x0c);                             // (the line is only shown with the motor on)
    CHECK_EQ(f.read(DIR), 0x00);
    f.write(DOR, 0x1c);
    CHECK_EQ(f.read(DIR), 0x80);
    // a read with no disk: nothing comes, nothing ends
    m.command({0xc6, 0x00, 0x01, 0x00, 0x01, 0x02, 0x12, 0x1b, 0xff});
    run_us(900000);
    CHECK_EQ(f.read(MSR), 0x70);
    f.write(DOR, 0x18);
    f.write(DOR, 0x1c);
    m.command({0x13, 0x00, 0x1e, 0x00});
    m.command({0x03, 0x5f, 0x11});

    std::vector<uint8_t> other = disk;              // another disk, write-protected
    other[0] ^= 0xff;
    CHECK(f.insert(other, true));
    CHECK_EQ(f.read(DIR), 0x80);                    // changed, until the head is stepped
    m.seek({0x07, 0x00});
    CHECK_EQ(f.read(DIR), 0x00);
    m.command({0x04, 0x00});
    CHECK_EQ(m.result(1)[0], 0x78);                 // write-protected
    run_us(300000);
    std::vector<uint8_t> res;
    std::vector<uint8_t> got = m.read_cylinder(0, res);
    CHECK(got.size() == kCylinderBytes && memcmp(got.data(), other.data(), kCylinderBytes) == 0);

    const uint64_t writes = f.disk_writes();
    m.command({0xc5, 0x00, 0x00, 0x00, 0x01, 0x02, 0x12, 0x1b, 0xff});
    CHECK_EQ(f.read(MSR), 0xd0);                    // refused at once
    res = m.result(7);
    CHECK_EQ(res[0], 0x40);
    CHECK_EQ(res[1], 0x02);                         // not writable
    CHECK(f.disk_writes() == writes);
    CHECK(f.disk() == other);

    // the same disk without its protection takes the write
    f.eject();
    CHECK(f.insert(other, false));
    m.seek({0x0f, 0x00, 0x01});
    m.seek({0x07, 0x00});
    m.command({0x04, 0x00});
    CHECK_EQ(m.result(1)[0], 0x38);
    run_us(300000);
    std::vector<uint8_t> fresh(kCylinderBytes, 0x5a);
    res = m.write_cylinder(0, fresh);
    CHECK_EQ(res[1], 0x80);
    CHECK(memcmp(f.disk().data(), fresh.data(), kCylinderBytes) == 0);
}

// ---- a blank disk is a formatted one --------------------------------------------------------------
void test_blank() {
    const std::vector<uint8_t> d = Fdc37c665::blank_disk(0x12345678);
    CHECK_EQ(d.size(), Fdc37c665::kDiskBytes);
    CHECK_EQ(d[510], 0x55);
    CHECK_EQ(d[511], 0xaa);
    CHECK_EQ(d[11] | d[12] << 8, 512);              // bytes a sector
    CHECK_EQ(d[19] | d[20] << 8, 2880);             // sectors
    CHECK_EQ(d[21], 0xf0);
    CHECK_EQ(d[22] | d[23] << 8, 9);                // sectors a FAT
    CHECK_EQ(d[39] | d[40] << 8 | d[41] << 16 | (unsigned)d[42] << 24, 0x12345678);
    CHECK(memcmp(d.data() + 54, "FAT12   ", 8) == 0);
    CHECK_EQ(d[512], 0xf0);                         // both FATs: the media byte and the end marks
    CHECK_EQ(d[512 + 1], 0xff);
    CHECK_EQ(d[512 + 2], 0xff);
    CHECK_EQ(d[512 * 10], 0xf0);
    for (size_t k = 512 * 19; k < d.size(); k++)    // an empty root directory and nothing after it
        if (d[k]) { CHECK(!"the blank disk has something on it"); break; }
}
}  // namespace

int main() {
    test_empty_drive();
    test_read_write(false);   // the FIFO as the chip's data sheet has it
    test_read_write(true);    // ... as MAME has it
    test_change_and_protect();
    test_blank();
    if (g_fail) {
        fprintf(stderr, "%d check(s) failed\n", g_fail);
        return 1;
    }
    printf("floppy controller: ok\n");
    return 0;
}
