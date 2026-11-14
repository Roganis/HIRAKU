// SPDX-License-Identifier: GPL-3.0-or-later
// ATA bus with ATAPI CD/DVD-ROM drives (PIO), as the Firebeat's main and SPU
// buses use them. Register-level behaviour was matched against MAME's ATA/ATAPI
// emulation through the oracle trace (oracle/), not ported line by line.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace hk {

// A read-only disc image: .iso (2048-byte sectors), or .bin/.cue / raw track
// (2352-byte MODE1 sectors).
// A CD/DVD image: an .iso, a raw MODE1 track, or a .cue sheet (one FILE, any mix
// of data and AUDIO tracks, as chdman extractcd writes them).
class DiscImage {
public:
    struct Track {
        uint32_t start = 0, frames = 0;   // logical sectors (start: INDEX 01, after any PREGAP)
        uint32_t file_start = 0;          // position of INDEX 01 in the file, in sectors
        bool audio = false;
        uint32_t size = 2048, skip = 0;   // bytes per sector in the file, header bytes before the user data
        uint64_t offset = 0;              // file offset of the track's first sector
    };
    bool open(const std::string &path);
    bool read_sector(uint32_t lba, uint8_t *out2048) const;   // data tracks
    bool read_sectors(uint32_t lba, uint32_t n, uint8_t *out) const;   // n of them, 2048 bytes each
    bool read_audio(uint32_t lba, uint8_t *out2352) const;    // audio tracks: 588 stereo 16-bit LE samples
    void prefetch(uint32_t lba, uint32_t count) const;        // ask the kernel to start reading these sectors
    uint32_t sectors() const { return nsectors_; }
    const std::vector<Track> &tracks() const { return tracks_; }
    int track_of(uint32_t lba) const;                         // index into tracks(), clamped
    ~DiscImage();

private:
    bool parse_cue(const std::string &cue, std::string &bin);
    FILE *f_ = nullptr;
    mutable uint64_t advised_from_ = 0, advised_to_ = 0;   // prefetch's last range (ata.cpp)
    std::string name_;   // the file's name, for the slow-read report
    uint32_t nsectors_ = 0;
    std::vector<Track> tracks_;
};

// Red Book audio playback state of a drive (MAME's cdda device).
struct CddaState {
    bool playing = false, paused = false, ended = false;
    uint32_t lba = 0, length = 0;     // next sector to fetch, sectors left to fetch
    uint32_t samples = 0, bptr = 0;   // stereo samples left in the cache, read index (int16 units)
    int16_t cache[588 * 2 * 4] = {};
    float gain[2] = {1.0f, 1.0f};     // MODE SELECT audio page volumes (MAME set_output_gain)
    uint32_t position() const { return lba - (samples + 587) / 588; }   // MAME get_audio_lba
};

// A raw hard-disk image (512-byte sectors, file order = sector order).
class DiskImage {
public:
    bool open(const std::string &path);
    bool read_sector(uint32_t lba, uint8_t *out512) const;
    bool read_sectors(uint32_t lba, uint32_t n, uint8_t *out) const;   // n of them, 512 bytes each
    void prefetch(uint32_t lba, uint32_t count) const;
    uint32_t sectors() const { return nsectors_; }
    // the geometry the drive reports (IDENTIFY) and addresses CHS by: 16 heads and 63 sectors a
    // track unless set (a DJ Main disk's, as MAME gets it from the CHD); cylinders from the size
    void set_geometry(uint32_t cylinders, uint32_t heads, uint32_t sectors) { cyls_ = cylinders, heads_ = heads, secs_ = sectors; }
    uint32_t heads() const { return heads_; }
    uint32_t track_sectors() const { return secs_; }
    uint32_t cylinders() const { return cyls_ ? cyls_ : std::min<uint32_t>(nsectors_ / (heads_ * secs_), 16383); }
    ~DiskImage();

private:
    FILE *f_ = nullptr;
    mutable uint64_t advised_from_ = 0, advised_to_ = 0;   // prefetch's last range (ata.cpp)
    std::string name_;
    uint32_t nsectors_ = 0;
    uint32_t cyls_ = 0, heads_ = 16, secs_ = 63;
};

// A device on the bus: an ATAPI CD/DVD drive (disc may be absent) or an ATA hard disk.
class AtaDevice {
public:
    explicit AtaDevice(std::string name, bool atapi = true) : name_(std::move(name)), atapi_(atapi) {}
    std::string name_;
    bool atapi_;
    std::unique_ptr<DiscImage> disc;   // ATAPI: null = drive present, no medium
    std::unique_ptr<DiskImage> disk;   // ATA hard disk
    CddaState cdda;
    // ATA security (MAME ata_mass_storage): a hard disk with a password set is
    // locked at power-on and soft reset; reads fail until SECURITY UNLOCK gets
    // the password (32 bytes; empty = none). DJ Main complete MIX and later.
    std::vector<uint8_t> master_password, user_password;
    bool master_locked = false, user_locked = false;
    void lock() { master_locked = !master_password.empty(); user_locked = !user_password.empty(); }
    bool locked() const { return master_locked || user_locked; }
};
using AtapiDrive = AtaDevice;

class AtaBus {
public:
    AtaBus();
    void attach(int unit, std::unique_ptr<AtaDevice> d);

    // Register access as the ATA bus sees it: cs 0/1, register 0..7, 16-bit
    // value in ATA (little-endian) order. The board does the Firebeat byte swap.
    uint16_t read(int cs, int reg);
    void write(int cs, int reg, uint16_t v);

    std::function<void(bool)> irq;
    std::function<void(bool)> dmarq;  // DMA request (READ DMA): the host pulls words with read_dma()
    uint16_t read_dma();
    bool log = false;
    // A hard disk's PIO read raises an interrupt when its last word has been read.
    // MAME and the ATA spec don't; the Firebeat SPU's oracle-matched timing does.
    bool pio_end_irq = true;
    // CD audio: `sync` renders the bus's audio up to the present (called before
    // playback state changes or is reported); cdda_render adds n 44.1 kHz stereo
    // frames of unit `unit`'s playback to lr, scaled by gain.
    std::function<void()> sync;
    void cdda_render(int unit, float *lr, int n, float gain);

private:
    void set_irq(bool on);
    void command(uint8_t cmd);
    void disk_command(uint8_t cmd);
    uint32_t lba() const;
    void packet_done();
    void start_data_in(std::vector<uint8_t> data, bool lazy = false);
    void finish(bool error, uint8_t sense_key = 0, uint8_t asc = 0);
    AtaDevice *cur();
    void stop_audio(AtaDevice *d);

    std::unique_ptr<AtaDevice> drives_[2];
    uint8_t feature_ = 0, error_ = 0, nsector_ = 0, sector_ = 0, cyl_lo_ = 0, cyl_hi_ = 0, device_ = 0;
    uint8_t status_ = 0, devctl_ = 0;
    bool irq_ = false;

    enum class Phase { idle, packet_in, data_in, dma_in, data_out };
    Phase phase_ = Phase::idle;
    uint8_t packet_[12]{};
    int packet_pos_ = 0;
    std::vector<uint8_t> data_;
    size_t data_pos_ = 0, chunk_end_ = 0, data_out_len_ = 0;
    // A READ's sectors are fetched from the image as the host consumes them (as a
    // drive streams them), not all at command time: spreads host I/O over the transfer.
    const DiscImage *lazy_disc_ = nullptr;
    const DiskImage *lazy_disk_ = nullptr;
    uint32_t lazy_lba_ = 0;
    size_t filled_ = 0;
    void fill(size_t upto);
    void data_out_done();
    void security_error();
    uint8_t data_out_cmd_ = 0;        // the ATA command taking data out (0: an ATAPI packet's)
    uint16_t byte_limit_ = 0;
    uint8_t multiple_ = 1;            // sectors per DRQ block (READ MULTIPLE)
    uint8_t sense_key_[2]{}, asc_[2]{};
};

}  // namespace hk
