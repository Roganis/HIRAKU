// SPDX-License-Identifier: GPL-3.0-or-later
#include "ata.h"

#include "../core/prof.h"
#include "image.h"
#include "shell/sys.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace hk {

namespace {
// pread, timed: a read the game waits on stops the machine, so a slow one is reported (a cold
// cache on a slow drive, a USB stick, a disk waking up). HK_DISK_LOG=1 reports every read.
ssize_t timed_pread(FILE *f, void *buf, size_t n, off_t off, const std::string &name) {
    static const bool all = getenv("HK_DISK_LOG") != nullptr;
    static int reported = 0;
    const uint64_t t0 = prof::now_ns();
    const ssize_t r = image::pread(f, buf, n, (uint64_t)off);   // a plain file's, or a CHD's (image.h)
    const double ms = (double)(prof::now_ns() - t0) / 1e6;
    if (all || (ms >= 50.0 && reported < 20)) {
        if (!all) reported++;
        fprintf(stderr, "disk: %sread %.1f ms: %.2f MiB at %.1f MiB of %s\n", all ? "" : "slow ", ms,
                (double)n / 1048576.0, (double)off / 1048576.0, name.c_str());
    }
    return r;
}

std::string base_name(const std::string &path) {
    auto slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}
}  // namespace

// ---------------------------------------------------------------------------
// Disc images
// ---------------------------------------------------------------------------

DiscImage::~DiscImage() {
    image::close(f_);
}

// A cue sheet with one FILE: tracks, their modes and INDEX 01 positions. Sector
// sizes follow the modes, so a file may mix them.
bool DiscImage::parse_cue(const std::string &cue, std::string &bin) {
    FILE *c = image::fopen_cue(cue);   // a CD CHD's: the sheet chdman extractcd writes
    if (!c) return false;
    char line[1024];
    uint32_t pregap = 0;
    tracks_.clear();
    while (fgets(line, sizeof line, c)) {
        char *t = line;
        while (*t == ' ' || *t == '\t') t++;
        char *q1 = strchr(t, '"'), *q2 = q1 ? strrchr(t, '"') : nullptr;
        if (strncmp(t, "FILE", 4) == 0 && q1 && q2 > q1) {
            if (!bin.empty()) { fclose(c); return false; }   // one FILE only
            bin.assign(q1 + 1, q2);
        } else if (strncmp(t, "TRACK", 5) == 0) {
            Track tr;
            if (strstr(t, "AUDIO")) { tr.audio = true; tr.size = 2352; }
            else if (strstr(t, "MODE1/2352")) { tr.size = 2352; tr.skip = 16; }
            else if (strstr(t, "MODE2/2352")) { tr.size = 2352; tr.skip = 24; }
            else if (strstr(t, "MODE1/2048")) { tr.size = 2048; }
            else { fclose(c); return false; }
            tracks_.push_back(tr);
        } else if (strncmp(t, "INDEX 01", 8) == 0 && !tracks_.empty()) {
            int m = 0, sec = 0, f = 0;
            if (sscanf(t + 8, "%d:%d:%d", &m, &sec, &f) == 3) {
                tracks_.back().file_start = (uint32_t)((m * 60 + sec) * 75 + f);
                tracks_.back().start = tracks_.back().file_start + pregap;
            }
        } else if (strncmp(t, "PREGAP", 6) == 0 && !tracks_.empty()) {
            int m = 0, sec = 0, f = 0;   // silence before the track that isn't in the file: shifts later LBAs
            if (sscanf(t + 6, "%d:%d:%d", &m, &sec, &f) == 3) pregap += (uint32_t)((m * 60 + sec) * 75 + f);
        }
    }
    fclose(c);
    return !bin.empty() && !tracks_.empty();
}

// Warm the page cache with a CD image in the background. Games read in bursts (a
// 3.8 MB READ moved by memory-to-memory DMA at once), so a cold cache costs
// several ms of host I/O in one emulated instant; the whole image takes about a
// second to read sequentially, during boot. Large images (DVDs, disks) are left
// to the kernel.
void warm_cache(const std::string &path, uint64_t size) {
    if (size > (1536ull << 20) || getenv("HK_NO_PREFETCH")) return;
    std::thread([path] {
        int fd = ::open(path.c_str(), O_RDONLY | O_BINARY);
        if (fd < 0) return;
        std::vector<char> buf(1 << 20);
        for (uint64_t off = 0; sys::pread(fd, buf.data(), buf.size(), off) > 0; off += buf.size()) {}
        ::close(fd);
    }).detach();
}

bool DiscImage::open(const std::string &path0) {
    std::string path = path0;
    bool cue = path.size() > 4 && (path.substr(path.size() - 4) == ".cue" || path.substr(path.size() - 4) == ".CUE");
    cue = cue || image::chd_kind(path) == "cd";   // a CD CHD reads as its .cue and .bin
    if (cue) {
        std::string bin;
        if (!parse_cue(path, bin)) return false;
        auto slash = path.find_last_of('/');
        path = (slash == std::string::npos ? std::string() : path.substr(0, slash + 1)) + bin;
    }
    f_ = image::fopen_image(path);
    if (!f_) return false;
    name_ = base_name(path);
    const int64_t size0 = image::size(f_);
    if (size0 < 0) return false;
    const uint64_t size = (uint64_t)size0;
    struct stat st;   // a CHD: its compressed file is what gets read
    warm_cache(path, image::is_chd(path) && stat(path.c_str(), &st) == 0 ? (uint64_t)st.st_size : size);
    if (cue && (tracks_.size() > 1 || tracks_[0].audio)) {
        // file offsets: each track's sectors at its own size, in order
        uint64_t off = 0;
        for (size_t k = 0; k < tracks_.size(); k++) {
            Track &t = tracks_[k];
            if (k) off += (uint64_t)(t.file_start - tracks_[k - 1].file_start) * tracks_[k - 1].size;
            t.offset = off;
        }
        Track &last = tracks_.back();
        last.frames = size > last.offset ? (uint32_t)((size - last.offset) / last.size) : 0;
        for (size_t k = 0; k + 1 < tracks_.size(); k++) tracks_[k].frames = tracks_[k + 1].file_start - tracks_[k].file_start;
        nsectors_ = last.start + last.frames;
        return true;
    }
    // a single data track: tell the sector format from the file itself
    uint8_t head[16];
    if (image::pread(f_, head, 16, 0) != 16) return false;
    static const uint8_t sync[12] = {0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0};
    Track t;
    if (memcmp(head, sync, 12) == 0 && size % 2352 == 0) { t.size = 2352; t.skip = 16; }
    else if (size % 2048 == 0) { t.size = 2048; t.skip = 0; }
    else return false;
    nsectors_ = (uint32_t)(size / t.size);
    t.frames = nsectors_;
    tracks_.assign(1, t);
    return true;
}

int DiscImage::track_of(uint32_t lba) const {
    int k = 0;
    while (k + 1 < (int)tracks_.size() && lba >= tracks_[k + 1].start) k++;
    return k;
}

bool DiscImage::read_sector(uint32_t lba, uint8_t *out) const {
    prof::Scope ps(prof::DISK);
    if (lba >= nsectors_) return false;
    const Track &t = tracks_[track_of(lba)];
    if (t.audio || lba - t.start >= t.frames) return false;
    return image::pread(f_, out, 2048, t.offset + (uint64_t)(lba - t.start) * t.size + t.skip) == 2048;
}

// n data sectors from lba: one pread per run within a track (raw 2352-byte
// sectors are read as a run, then their 2048 data bytes picked out).
bool DiscImage::read_sectors(uint32_t lba, uint32_t n, uint8_t *out) const {
    prof::Scope ps(prof::DISK);
    static thread_local std::vector<uint8_t> raw;
    while (n) {
        if (lba >= nsectors_) return false;
        const Track &t = tracks_[track_of(lba)];
        if (t.audio || lba - t.start >= t.frames) return false;
        const uint32_t run = std::min(n, t.frames - (lba - t.start));
        const off_t off = (off_t)(t.offset + (uint64_t)(lba - t.start) * t.size);
        if (t.size == 2048 && t.skip == 0) {
            if (timed_pread(f_, out, (size_t)run * 2048, off, name_) != (ssize_t)run * 2048) return false;
        } else {
            raw.resize((size_t)run * t.size);
            if (timed_pread(f_, raw.data(), raw.size(), off, name_) != (ssize_t)raw.size()) return false;
            for (uint32_t k = 0; k < run; k++) memcpy(out + (size_t)k * 2048, raw.data() + (size_t)k * t.size + t.skip, 2048);
        }
        lba += run;
        n -= run;
        out += (size_t)run * 2048;
    }
    return true;
}

// Read-ahead: with each READ, the kernel is asked for its range and for the next
// READ_AHEAD bytes of the image. Nearly all of a game's READs continue the
// previous one (bm3: 600 of 615), so the next is then cached or on its way,
// rather than read in one go when the game's CPU asks for its first sector.
constexpr uint64_t READ_AHEAD = 8ull << 20;

void DiscImage::prefetch(uint32_t lba, uint32_t count) const {
    if (!count || lba >= nsectors_) return;
    const Track &t = tracks_[track_of(lba)];
    uint64_t off = t.offset + (uint64_t)(lba - t.start) * t.size;
    sys::will_need(fileno(f_), off, (uint64_t)count * t.size + READ_AHEAD);
}

void DiskImage::prefetch(uint32_t lba, uint32_t count) const {
    if (count) sys::will_need(fileno(f_), (uint64_t)lba * 512, (uint64_t)count * 512 + READ_AHEAD);
}

bool DiskImage::read_sectors(uint32_t lba, uint32_t n, uint8_t *out) const {
    prof::Scope ps(prof::DISK);
    if (lba >= nsectors_ || n > nsectors_ - lba) return false;
    return timed_pread(f_, out, (size_t)n * 512, (off_t)lba * 512, name_) == (ssize_t)n * 512;
}

bool DiscImage::read_audio(uint32_t lba, uint8_t *out) const {
    prof::Scope ps(prof::DISK);
    if (lba >= nsectors_) return false;
    const Track &t = tracks_[track_of(lba)];
    if (!t.audio || lba - t.start >= t.frames) return false;   // data, or a pregap: silence
    return image::pread(f_, out, 2352, t.offset + (uint64_t)(lba - t.start) * t.size) == 2352;
}

DiskImage::~DiskImage() {
    image::close(f_);
}

bool DiskImage::open(const std::string &path) {
    f_ = image::fopen_image(path);   // a raw image, or a hard disk CHD (image.h)
    if (!f_) return false;
    name_ = base_name(path);
    const int64_t size = image::size(f_);
    nsectors_ = size > 0 ? (uint32_t)((uint64_t)size / 512) : 0;
    return nsectors_ > 0;
}

bool DiskImage::read_sector(uint32_t lba, uint8_t *out) const {
    prof::Scope ps(prof::DISK);
    if (lba >= nsectors_) return false;
    return image::pread(f_, out, 512, (uint64_t)lba * 512) == 512;
}

namespace {
// MSF addresses as MAME's t10mmc converts them (LBA 0 = 00:02:00)
uint32_t to_msf_raw(uint32_t f) { return ((f / (75 * 60)) % 100) << 16 | ((f / 75) % 60) << 8 | (f % 75); }
uint32_t to_msf(uint32_t lba) { int32_t f = (int32_t)lba + 150; if (f < 0) f += 450000; return to_msf_raw((uint32_t)f); }
uint32_t msf_frames(uint32_t msf) { return ((msf >> 16) & 0xff) * 60 * 75 + ((msf >> 8) & 0xff) * 75 + (msf & 0xff); }
uint32_t from_msf(uint32_t msf) {
    uint32_t lba = msf_frames(msf) - 150;
    if (((msf >> 16) & 0xff) >= 90) lba -= 450000;
    return lba;
}
}  // namespace

// ATA status and interrupt-reason bits
namespace {
enum : uint8_t { ST_ERR = 0x01, ST_DRQ = 0x08, ST_DSC = 0x10, ST_DRDY = 0x40, ST_BSY = 0x80 };
enum : uint8_t { IR_COD = 0x01, IR_IO = 0x02 };
}  // namespace

// MODE SELECT(10) parameter list (MAME t10mmc::WriteData): the audio control page
// sets the CD audio volume per channel.
void AtaBus::data_out_done() {
    AtaDevice *d = cur();
    if (!d) {   // the host chose the unit with nothing on it while its data was going out
        data_out_cmd_ = 0;
        phase_ = Phase::idle;
        data_.clear();
        return;
    }
    const std::vector<uint8_t> &p = data_;
    if (data_out_cmd_ == 0xf2) {                         // SECURITY UNLOCK (MAME process_buffer)
        const bool master = p[0] & 0x01;
        const std::vector<uint8_t> &pw = master ? d->master_password : d->user_password;
        if (pw.size() == 32 && p.size() >= 34 && memcmp(pw.data(), p.data() + 2, 32) == 0) (master ? d->master_locked : d->user_locked) = false;
        if (log) fprintf(stderr, "ata: %s: unlock %s password %s\n", d->name_.c_str(), master ? "master" : "user", d->locked() ? "still locked" : "ok");
        data_out_cmd_ = 0;
        phase_ = Phase::idle;
        data_.clear();
        status_ = ST_DRDY | ST_DSC;                      // DRQ off; MAME raises no interrupt here
        if (d->locked()) security_error();
        return;
    }
    if (data_out_cmd_ == 0xf6) {                         // SECURITY DISABLE PASSWORD: MAME ignores it
        data_out_cmd_ = 0;
        phase_ = Phase::idle;
        data_.clear();
        status_ = ST_DRDY | ST_DSC;
        return;
    }
    size_t len = std::min(p.size(), data_out_len_), ptr = 8;
    if (len >= 8) ptr += (size_t)(p[6] << 8 | p[7]);    // block descriptors
    while (ptr + 1 < len) {
        if ((p[ptr] & 0x3f) == 0x0e && ptr + 11 < len) {
            if (sync) sync();
            d->cdda.gain[0] = p[ptr + 9] / 255.0f;
            d->cdda.gain[1] = p[ptr + 11] / 255.0f;
        }
        ptr += (size_t)p[ptr + 1] + 2;
    }
    finish(false);
}

void AtaBus::stop_audio(AtaDevice *d) {
    d->cdda.playing = false;
    d->cdda.ended = true;
}

// MAME cdda_device::get_audio_data: fetch up to 4 sectors at a time, stop when
// the requested length runs out.
void AtaBus::cdda_render(int unit, float *lr, int n, float gain) {
    AtaDevice *d = drives_[unit & 1].get();
    if (!d || !d->disc) return;
    auto &a = d->cdda;
    for (int i = 0; i < n;) {
        if (!a.playing || a.paused || (!a.length && !a.samples)) {
            if (a.playing && !a.paused && !a.length) {
                a.playing = false;
                a.ended = true;
            }
            return;
        }
        uint32_t k = std::min<uint32_t>((uint32_t)(n - i), a.samples);
        for (uint32_t j = 0; j < k; j++, i++) {
            lr[2 * i] += a.cache[a.bptr++] * (gain * a.gain[0] / 32768.0f);
            lr[2 * i + 1] += a.cache[a.bptr++] * (gain * a.gain[1] / 32768.0f);
        }
        a.samples -= k;
        if (a.samples == 0) {
            uint32_t sectors = std::min<uint32_t>(a.length, 4);
            for (uint32_t s = 0; s < sectors; s++) {
                uint8_t raw[2352];
                if (!d->disc->read_audio(a.lba, raw)) memset(raw, 0, sizeof raw);   // data track or beyond: silence
                for (int b = 0; b < 1176; b++) a.cache[s * 1176 + b] = (int16_t)(raw[2 * b] | raw[2 * b + 1] << 8);
                a.lba++;
            }
            a.samples = sectors * 588;
            a.length -= sectors;
            a.bptr = 0;
        }
    }
}


// ---------------------------------------------------------------------------
// ATA registers
// ---------------------------------------------------------------------------


AtaBus::AtaBus() = default;

void AtaBus::attach(int unit, std::unique_ptr<AtaDevice> d) {
    // power-on state: a hard disk is ready (DRDY|DSC); an ATAPI device shows 0 and its signature
    if ((unit & 1) == 0 && !d->atapi_) status_ = ST_DRDY | ST_DSC;
    if (!d->atapi_) d->lock();                           // set its passwords before attaching it
    drives_[unit & 1] = std::move(d);
}

AtaDevice *AtaBus::cur() { return drives_[(device_ >> 4) & 1].get(); }

uint32_t AtaBus::lba() const {
    if (device_ & 0x40) return (uint32_t)(device_ & 0x0f) << 24 | (uint32_t)cyl_hi_ << 16 | (uint32_t)cyl_lo_ << 8 | sector_;
    // CHS with the geometry IDENTIFY reports (16 heads and 63 sectors a track unless the disk has its own)
    const AtaDevice *d = drives_[(device_ >> 4) & 1].get();
    const uint32_t heads = d && d->disk ? d->disk->heads() : 16, secs = d && d->disk ? d->disk->track_sectors() : 63;
    uint32_t cyl = (uint32_t)cyl_hi_ << 8 | cyl_lo_;
    return (cyl * heads + (device_ & 0x0f)) * secs + (sector_ ? sector_ - 1 : 0);
}

uint16_t AtaBus::read_dma() {
    if (phase_ != Phase::dma_in || data_pos_ >= data_.size()) return 0;
    fill(data_pos_ + 2);
    uint16_t v = data_[data_pos_] | (data_pos_ + 1 < data_.size() ? data_[data_pos_ + 1] << 8 : 0);
    data_pos_ += 2;
    if (data_pos_ >= data_.size()) {
        if (dmarq) dmarq(false);
        finish(false);
    }
    return v;
}

void AtaBus::set_irq(bool on) {
    bool line = on && !(devctl_ & 0x02);    // nIEN masks the line
    if (on != irq_ || line != irq_) {}
    irq_ = on;
    if (irq) irq(line);
}

uint16_t AtaBus::read(int cs, int reg) {
    if (!cur() && !(cs == 0 && reg == 6)) return 0;     // nothing on this unit
    if (cs == 1) {
        if (reg == 6) return status_;                    // alternate status: no side effects
        return 0;
    }
    switch (reg) {
    case 0: {                                            // data
        if (phase_ != Phase::data_in || data_pos_ + 1 >= data_.size() + 1) return 0;
        fill(data_pos_ + 2);
        uint16_t v = data_[data_pos_] | (data_pos_ + 1 < data_.size() ? data_[data_pos_ + 1] << 8 : 0);
        data_pos_ += 2;
        if (data_pos_ >= chunk_end_) {
            if (data_pos_ >= data_.size()) {
                if (!cur()->atapi_ && !pio_end_irq) {
                    // hard disk PIO read: done when the host has the last word. No
                    // interrupt (each block's came when its data was ready), as in
                    // the ATA spec and MAME's atastorage.cpp
                    phase_ = Phase::idle;
                    data_.clear();
                    lazy_disk_ = nullptr;
                    status_ = ST_DRDY | ST_DSC;
                    error_ = 0;
                } else {
                    finish(false);
                }
            } else if (!cur()->atapi_) {                 // hard disk: next block of sectors
                chunk_end_ = std::min(data_.size(), data_pos_ + (size_t)512 * multiple_);
                status_ = ST_DRDY | ST_DSC | ST_DRQ;
                set_irq(true);
            } else {                                     // next DRQ block
                size_t n = std::min<size_t>(byte_limit_, data_.size() - data_pos_);
                chunk_end_ = data_pos_ + n;
                cyl_lo_ = n & 0xff;
                cyl_hi_ = (n >> 8) & 0xff;
                status_ = ST_DRQ;
                nsector_ = IR_IO;
                set_irq(true);
            }
        }
        return v;
    }
    case 1: return error_;
    case 2: return nsector_;
    case 3: return sector_;
    case 4: return cyl_lo_;
    case 5: return cyl_hi_;
    case 6: return device_;
    case 7:                                              // status: reading acknowledges the interrupt
        set_irq(false);
        return status_;
    }
    return 0;
}

void AtaBus::write(int cs, int reg, uint16_t v) {
    if (cs == 1) {
        if (reg == 6) {                                  // device control
            uint8_t old = devctl_;
            devctl_ = v & 0xff;
            if ((old & 0x04) && !(devctl_ & 0x04)) {     // end of soft reset: device signature
                for (auto &dr : drives_)
                    if (dr && !dr->atapi_) dr->lock();   // MAME soft_reset(): the passwords apply again
                phase_ = Phase::idle;
                data_.clear();   // (and nothing of the command it cut is left to read on from)
                data_out_cmd_ = 0;
                lazy_disc_ = nullptr;
                lazy_disk_ = nullptr;
                error_ = 1;
                nsector_ = 1;
                sector_ = 1;
                device_ = 0;
                bool atapi = drives_[0] && drives_[0]->atapi_;
                status_ = atapi ? 0 : (ST_DRDY | ST_DSC);
                cyl_lo_ = atapi ? 0x14 : 0;
                cyl_hi_ = atapi ? 0xeb : 0;
                set_irq(false);
            }
            set_irq(irq_);
        }
        return;
    }
    switch (reg) {
    case 0:                                              // data
        if (phase_ == Phase::packet_in && packet_pos_ < 12) {
            packet_[packet_pos_++] = v & 0xff;
            packet_[packet_pos_++] = v >> 8;
            if (packet_pos_ >= 12) packet_done();
        } else if (phase_ == Phase::data_out) {
            data_.push_back(v & 0xff);
            data_.push_back(v >> 8);
            if (data_.size() >= data_out_len_) data_out_done();
        }
        break;
    case 1: feature_ = v & 0xff; break;
    case 2: nsector_ = v & 0xff; break;
    case 3: sector_ = v & 0xff; break;
    case 4: cyl_lo_ = v & 0xff; break;
    case 5: cyl_hi_ = v & 0xff; break;
    case 6: device_ = v & 0xff; break;
    case 7:
        if (cur()) command(v & 0xff);
        break;
    }
}

void AtaBus::finish(bool error, uint8_t key, uint8_t asc) {
    int u = (device_ >> 4) & 1;
    phase_ = Phase::idle;
    data_.clear();
    lazy_disc_ = nullptr;
    lazy_disk_ = nullptr;
    if (cur() && !cur()->atapi_) {                       // hard disk: DRDY/DSC, error register = ABRT etc.
        status_ = ST_DRDY | ST_DSC | (error ? ST_ERR : 0);
        error_ = error ? key : 0;
        set_irq(true);
        return;
    }
    sense_key_[u] = key;
    asc_[u] = asc;
    status_ = error ? ST_ERR : 0;
    error_ = error ? (uint8_t)(key << 4) : 0;
    nsector_ = IR_IO | IR_COD;
    set_irq(true);
}

// MAME security_error(): ERR with ABRT, no data and no interrupt.
void AtaBus::security_error() {
    phase_ = Phase::idle;
    data_.clear();
    lazy_disc_ = nullptr;
    lazy_disk_ = nullptr;
    status_ = ST_DRDY | ST_DSC | ST_ERR;
    error_ = 0x04;
}

// Fetch the lazily-read sectors covering data_[0, upto), 16 at a time.
void AtaBus::fill(size_t upto) {
    if (!lazy_disc_ && !lazy_disk_) return;
    const size_t sec = lazy_disc_ ? 2048 : 512;
    upto = std::min(data_.size(), std::max(upto, filled_ + 16 * sec));
    if (filled_ >= upto) return;
    // the whole stretch in one read; sector by sector only if that fails
    const uint32_t first = lazy_lba_ + (uint32_t)(filled_ / sec), n = (uint32_t)((upto - filled_ + sec - 1) / sec);
    if (lazy_disc_ ? lazy_disc_->read_sectors(first, n, data_.data() + filled_) : lazy_disk_->read_sectors(first, n, data_.data() + filled_)) {
        filled_ += (size_t)n * sec;
        return;
    }
    while (filled_ < upto) {
        uint32_t lba = lazy_lba_ + (uint32_t)(filled_ / sec);
        bool ok = lazy_disc_ ? lazy_disc_->read_sector(lba, data_.data() + filled_) : lazy_disk_->read_sector(lba, data_.data() + filled_);
        if (!ok) memset(data_.data() + filled_, 0, sec);   // checked at command time; can't happen
        filled_ += sec;
    }
}

void AtaBus::start_data_in(std::vector<uint8_t> d, bool lazy) {
    if (!lazy) lazy_disc_ = nullptr, lazy_disk_ = nullptr;
    if (d.empty()) { finish(false); return; }
    // the host may limit the size of each DRQ block (byte count registers on entry)
    byte_limit_ = (uint16_t)(cyl_lo_ | cyl_hi_ << 8);
    if (byte_limit_ == 0 || byte_limit_ == 0xffff) byte_limit_ = 0xfffe;
    byte_limit_ &= ~1;
    data_ = std::move(d);
    data_pos_ = 0;
    if (feature_ & 0x01) {                               // PACKET with the DMA flag: the host DMAs it all
        phase_ = Phase::dma_in;
        status_ = ST_DRQ;
        nsector_ = IR_IO;
        if (dmarq) dmarq(true);
        return;
    }
    size_t n = std::min<size_t>(byte_limit_, data_.size());
    chunk_end_ = n;
    cyl_lo_ = n & 0xff;
    cyl_hi_ = (n >> 8) & 0xff;
    phase_ = Phase::data_in;
    status_ = ST_DRQ;
    nsector_ = IR_IO;
    set_irq(true);
}

void AtaBus::disk_command(uint8_t cmd) {
    AtaDevice *d = cur();
    switch (cmd) {
    case 0xec: {                                         // IDENTIFY DEVICE
        std::vector<uint8_t> id(512, 0);
        auto w = [&](int i, uint16_t v) { id[2 * i] = v & 0xff; id[2 * i + 1] = v >> 8; };
        auto str = [&](int i, int words, const char *s) {
            for (int k = 0; k < words * 2; k++) id[2 * i + (k ^ 1)] = (uint8_t)((size_t)k < strlen(s) ? s[k] : ' ');
        };
        uint32_t n = d->disk->sectors();
        const uint32_t cyls = d->disk->cylinders(), heads = d->disk->heads(), secs = d->disk->track_sectors();
        w(0, 0x0040);                                    // fixed disk
        w(1, (uint16_t)cyls);
        w(3, (uint16_t)heads);
        w(6, (uint16_t)secs);
        str(10, 10, "HIRAKU0000000001");
        str(23, 4, "1.0");
        str(27, 20, "HIRAKU ATA DISK");
        w(47, 0x8010);                                   // READ/WRITE MULTIPLE: up to 16 sectors
        w(49, 0x0300);                                   // LBA, DMA
        w(53, 0x0003);
        w(54, (uint16_t)cyls);
        w(55, (uint16_t)heads);
        w(56, (uint16_t)secs);
        w(57, (uint16_t)(cyls * heads * secs));
        w(58, (uint16_t)((cyls * heads * secs) >> 16));
        w(60, (uint16_t)n);
        w(61, (uint16_t)(n >> 16));
        w(63, 0x0007);                                   // multiword DMA 0-2
        lazy_disc_ = nullptr;                            // (a read cut before its first word would
        lazy_disk_ = nullptr;                            // fill this block with its sectors)
        data_ = std::move(id);
        data_pos_ = 0;
        chunk_end_ = 512;
        phase_ = Phase::data_in;
        status_ = ST_DRDY | ST_DSC | ST_DRQ;
        set_irq(true);
        return;
    }
    case 0x20: case 0x21:                                // READ SECTORS
    case 0xc4:                                           // READ MULTIPLE
    case 0xc8: case 0xc9: {                              // READ DMA
        if (d->locked()) { security_error(); return; }   // MAME read_first_sector()
        uint32_t count = nsector_ ? nsector_ : 256, first = lba();
        if ((uint64_t)first + count > d->disk->sectors()) {
            finish(true, 0x10);                          // IDNF: sector not found
            return;
        }
        data_.assign((size_t)count * 512, 0);
        data_pos_ = 0;
        lazy_disk_ = d->disk.get();
        lazy_disc_ = nullptr;
        lazy_lba_ = first;
        filled_ = 0;
        d->disk->prefetch(first, count);
        if (cmd == 0xc8 || cmd == 0xc9) {
            phase_ = Phase::dma_in;
            status_ = ST_DRDY | ST_DSC | ST_DRQ;
            if (dmarq) dmarq(true);
        } else {
            phase_ = Phase::data_in;
            chunk_end_ = std::min(data_.size(), (size_t)512 * (cmd == 0xc4 ? multiple_ : 1));
            status_ = ST_DRDY | ST_DSC | ST_DRQ;
            set_irq(true);
        }
        return;
    }
    case 0xf2:                                           // SECURITY UNLOCK: 512 bytes from the host
    case 0xf6:                                           // SECURITY DISABLE PASSWORD (ignored, as in MAME)
        data_.clear();
        data_out_len_ = 512;
        data_out_cmd_ = cmd;
        phase_ = Phase::data_out;
        status_ = ST_DRDY | ST_DSC | ST_DRQ;
        set_irq(true);
        return;
    case 0xc6:                                           // SET MULTIPLE MODE
        multiple_ = nsector_ ? nsector_ : 1;
        finish(false);
        return;
    case 0x40: case 0x41:                                // READ VERIFY
        if (d->locked()) { security_error(); return; }   // MAME read_first_sector()
        finish(false);
        return;
    case 0x10:                                           // RECALIBRATE
    case 0x91:                                           // INITIALIZE DEVICE PARAMETERS
    case 0xef:                                           // SET FEATURES
    case 0xe0: case 0xe1: case 0xe3:                     // STANDBY / IDLE
    case 0xe7:                                           // FLUSH CACHE
        finish(false);
        return;
    case 0x00:                                           // NOP: always aborts (ATA spec)
        finish(true, 0x04);
        return;
    default:
        fprintf(stderr, "ata: %s: unsupported ATA command %02x\n", d->name_.c_str(), cmd);
        finish(true, 0x04);                              // ABRT
        return;
    }
}

void AtaBus::command(uint8_t cmd) {
    AtaDevice *d = cur();
    if (log) fprintf(stderr, "ata: %s command %02x\n", d->name_.c_str(), cmd);
    if (!d->atapi_) { disk_command(cmd); return; }
    switch (cmd) {
    case 0xa0:                                           // PACKET
        phase_ = Phase::packet_in;
        packet_pos_ = 0;
        status_ = ST_DRQ;
        nsector_ = IR_COD;
        break;
    case 0xa1: {                                         // IDENTIFY PACKET DEVICE
        std::vector<uint8_t> id(512, 0);
        auto w = [&](int i, uint16_t v) { id[2 * i] = v & 0xff; id[2 * i + 1] = v >> 8; };
        auto str = [&](int i, int words, const char *s) {
            for (int k = 0; k < words * 2; k++) {
                char ch = (size_t)k < strlen(s) ? s[k] : ' ';
                id[2 * i + (k ^ 1)] = (uint8_t)ch;   // ATA strings: byte-swapped words
            }
        };
        w(0, 0x8580);                                    // ATAPI, CD-ROM, removable, 12-byte packets
        str(23, 4, "1.0");
        str(27, 20, "HIRAKU ATAPI CD-ROM");
        w(49, 0x0300);                                   // LBA + DMA capable
        byte_limit_ = 512;
        cyl_lo_ = 0;
        cyl_hi_ = 2;
        start_data_in(std::move(id));
        break;
    }
    case 0x08:                                           // DEVICE RESET
        finish(false);
        status_ = 0;
        cyl_lo_ = 0x14;
        cyl_hi_ = 0xeb;
        break;
    case 0xef:                                           // SET FEATURES
    case 0xe1:                                           // IDLE IMMEDIATE
    case 0xe0:                                           // STANDBY IMMEDIATE
        finish(false);
        break;
    case 0xe5:                                           // CHECK POWER MODE
        finish(false);
        nsector_ = 0xff;                                 // active or idle (MAME atapi_hle)
        break;
    case 0xe3:                                           // IDLE: pop'n sends it to the DVD drive, and
        finish(true, 0x05, 0x20);                        // MAME's ATAPI drive rejects it too (ABRT)
        break;
    default:
        fprintf(stderr, "ata: %s: unsupported ATA command %02x\n", d->name_.c_str(), cmd);
        finish(true, 0x05, 0x20);
        break;
    }
}

void AtaBus::packet_done() {
    AtaDevice *d = cur();
    if (!d) {   // the host chose the unit with nothing on it while the packet was going out
        phase_ = Phase::idle;
        return;
    }
    const uint8_t *p = packet_;
    int u = (device_ >> 4) & 1;
    if (log) {
        fprintf(stderr, "ata: %s packet", d->name_.c_str());
        for (int k = 0; k < 12; k++) fprintf(stderr, " %02x", p[k]);
        fprintf(stderr, "\n");
    }
    const bool medium = d->disc != nullptr;
    auto be16 = [&](int i) { return (uint32_t)p[i] << 8 | p[i + 1]; };
    auto be32 = [&](int i) { return be16(i) << 16 | be16(i + 2); };
    switch (p[0]) {
    case 0x00:                                           // TEST UNIT READY
        if (!medium) finish(true, 0x02, 0x3a);
        else finish(false);
        break;
    case 0x03: {                                         // REQUEST SENSE
        std::vector<uint8_t> s(18, 0);
        s[0] = 0x70;
        s[2] = sense_key_[u];
        s[7] = 10;
        s[12] = asc_[u];
        s.resize(std::min<size_t>(s.size(), p[4] ? p[4] : 18));
        sense_key_[u] = asc_[u] = 0;
        start_data_in(std::move(s));
        break;
    }
    case 0x12: {                                         // INQUIRY
        std::vector<uint8_t> s(36, 0);
        s[0] = 0x05;                                     // CD/DVD device
        s[1] = 0x80;                                     // removable
        s[3] = 0x21;
        s[4] = 31;
        memcpy(&s[8], "HIRAKU  CD-ROM          1.0 ", 28);
        s.resize(std::min<size_t>(s.size(), p[4] ? p[4] : 36));
        start_data_in(std::move(s));
        break;
    }
    case 0x1b:   // START STOP UNIT: stops CD audio (MAME's t10mmc abort_audio): Keyboardmania ends its demo's song so
        if (d->cdda.playing) {
            if (sync) sync();
            stop_audio(d);
        }
        finish(false);
        break;
    case 0x1e:                                           // PREVENT ALLOW MEDIUM REMOVAL
    case 0x2b:                                           // SEEK
    case 0xbb:                                           // SET CD SPEED
        finish(false);
        break;
    case 0x55:                                           // MODE SELECT(10): take the parameter list
        data_out_len_ = be16(7);
        if (!data_out_len_) { finish(false); break; }
        data_.clear();
        phase_ = Phase::data_out;
        cyl_lo_ = data_out_len_ & 0xff;
        cyl_hi_ = (data_out_len_ >> 8) & 0xff;
        status_ = ST_DRQ;
        nsector_ = 0;                                    // data to the device
        set_irq(true);
        break;
    case 0x25: {                                         // READ CAPACITY
        if (!medium) { finish(true, 0x02, 0x3a); break; }
        uint32_t last = d->disc->sectors() - 1;
        std::vector<uint8_t> s = {(uint8_t)(last >> 24), (uint8_t)(last >> 16), (uint8_t)(last >> 8), (uint8_t)last, 0, 0, 8, 0};
        start_data_in(std::move(s));
        break;
    }
    case 0x28:                                           // READ(10)
    case 0xa8: {                                         // READ(12)
        if (!medium) { finish(true, 0x02, 0x3a); break; }
        if (d->cdda.playing) {                           // abort_audio
            if (sync) sync();
            stop_audio(d);
        }
        uint32_t lba = be32(2);
        uint32_t n = p[0] == 0x28 ? be16(7) : be32(6);
        if ((uint64_t)lba + n > d->disc->sectors()) { finish(true, 0x05, 0x21); return; }   // LBA out of range
        for (int t = d->disc->track_of(lba); n && t <= d->disc->track_of(lba + n - 1); t++)
            if (d->disc->tracks()[t].audio) { finish(true, 0x05, 0x64); return; }          // illegal mode for this track
        lazy_disc_ = d->disc.get();
        lazy_disk_ = nullptr;
        lazy_lba_ = lba;
        filled_ = 0;
        d->disc->prefetch(lba, n);
        start_data_in(std::vector<uint8_t>((size_t)n * 2048), true);
        break;
    }
    case 0x43: {                                         // READ TOC (MAME t10mmc)
        if (!medium) { finish(true, 0x02, 0x3a); break; }
        if (sync) sync();
        if (d->cdda.playing) stop_audio(d);              // abort_audio
        const auto &tr = d->disc->tracks();
        int format = p[2] & 0x0f;
        if (format == 0) format = p[9] >> 6;             // SFF8020 legacy field
        bool msf = p[1] & 0x02;
        uint32_t alloc = be16(7);
        std::vector<uint8_t> s;
        auto u32 = [&](uint32_t v) { for (int k = 3; k >= 0; k--) s.push_back((uint8_t)(v >> (8 * k))); };
        auto adr = [&](int t) -> uint8_t { return t < (int)tr.size() && tr[t].audio ? 0x10 : 0x14; };
        if (format == 0) {                               // tracks
            int first = p[6] ? p[6] : 1, last = (int)tr.size();
            int n = std::max(0, last - first + 1) + 1;    // ... + lead-out
            s = {(uint8_t)((2 + 8 * n) >> 8), (uint8_t)(2 + 8 * n), 1, (uint8_t)last};
            for (int i = 0; i < n; i++) {
                bool lead = i == n - 1;
                int t = lead ? 0xaa : first + i;
                uint32_t start = lead ? d->disc->sectors() : tr[t - 1].start;
                s.insert(s.end(), {0, adr(lead ? (int)tr.size() - 1 : t - 1), (uint8_t)t, 0});
                u32(msf ? to_msf(start) : start);
            }
        } else if (format == 1) {                        // sessions: one
            s = {0, 10, 1, 1, 0, adr(0), 1, 0};
            u32(msf ? to_msf(tr[0].start) : tr[0].start);
        } else {
            fprintf(stderr, "ata: %s: READ TOC format %d not supported\n", d->name_.c_str(), format);
            s = {0, 2, 1, 1};
        }
        if (alloc && s.size() > alloc) s.resize(alloc);
        start_data_in(std::move(s));
        break;
    }
    case 0x42: {                                         // READ SUB-CHANNEL: current position
        if (!medium) { finish(true, 0x02, 0x3a); break; }
        if (sync) sync();
        auto &a = d->cdda;
        bool msf = p[1] & 0x02;
        std::vector<uint8_t> s(16, 0);
        uint32_t pos = 0;
        if (a.playing) {
            pos = a.position();
            s[1] = a.paused ? 0x12 : 0x11;               // paused / in progress
        } else {
            s[1] = a.ended ? 0x13 : 0x15;                // ended successfully / no status
        }
        if (p[3] == 1 && (p[2] & 0x40)) {
            int t = d->disc->track_of(pos);
            const auto &tr = d->disc->tracks()[t];
            s[3] = 12;
            s[4] = 0x01;
            s[5] = tr.audio ? 0x10 : 0x14;
            s[6] = (uint8_t)(t + 1);
            s[7] = 1;
            uint32_t abs = msf ? to_msf(pos) : pos, rel = msf ? to_msf_raw(pos - tr.start) : pos - tr.start;
            for (int k = 0; k < 4; k++) { s[8 + k] = (uint8_t)(abs >> (24 - 8 * k)); s[12 + k] = (uint8_t)(rel >> (24 - 8 * k)); }
        } else {
            s.resize(4);
        }
        uint32_t alloc = be16(7);
        if (alloc && s.size() > alloc) s.resize(alloc);
        start_data_in(std::move(s));
        break;
    }
    case 0x45:                                           // PLAY AUDIO(10)
    case 0xa5:                                           // PLAY AUDIO(12)
    case 0x47:                                           // PLAY AUDIO MSF
    case 0x48: {                                         // PLAY AUDIO TRACK/INDEX
        if (!medium) { finish(true, 0x02, 0x3a); break; }
        if (sync) sync();
        const auto &tr = d->disc->tracks();
        auto &a = d->cdda;
        uint32_t lba, blocks;
        if (p[0] == 0x45) { lba = be32(2); blocks = be16(7); }
        else if (p[0] == 0xa5) { lba = be32(2); blocks = be32(6); }
        else if (p[0] == 0x47) {
            uint32_t ms = (uint32_t)p[3] << 16 | p[4] << 8 | p[5], me = (uint32_t)p[6] << 16 | p[7] << 8 | p[8];
            if (ms == 0xffffff) ms = to_msf(a.position());
            if (ms == me) { finish(false); break; }       // nothing to play, not an error
            if (ms > me) { finish(true, 0x05, 0x24); break; }
            lba = from_msf(ms);
            blocks = msf_frames(me) - msf_frames(ms);
        } else {
            int ts = p[4], te = std::min<int>(p[7], (int)tr.size());
            if (ts < 1 || ts > te) { finish(true, 0x05, 0x24); break; }
            if (p[4] != p[7] && p[5] != p[8] && p[8] <= 1) te--;   // MAME's index heuristic
            lba = tr[ts - 1].start;
            blocks = (te < (int)tr.size() ? tr[te].start : d->disc->sectors()) - lba;
        }
        if (lba == 0xffffffff) lba = a.position();
        if (lba == 0) lba = tr[0].audio ? tr[0].start : 150;
        if (!tr[d->disc->track_of(lba)].audio) { finish(true, 0x05, 0x64); break; }   // illegal mode for this track
        a.playing = true;
        a.paused = false;
        a.ended = false;
        a.lba = lba;
        a.length = blocks;
        a.samples = 0;
        if (log) fprintf(stderr, "ata: %s plays LBA %u for %u sectors\n", d->name_.c_str(), lba, blocks);
        finish(false);
        break;
    }
    case 0x4b:                                           // PAUSE/RESUME
        if (sync) sync();
        d->cdda.paused = !(p[8] & 0x01);
        finish(false);
        break;
    case 0x4e:                                           // STOP PLAY/SCAN
        if (sync) sync();
        if (d->cdda.playing) stop_audio(d);
        finish(false);
        break;
    case 0x5a: {                                         // MODE SENSE(10): header only
        std::vector<uint8_t> s(8, 0);
        s[1] = 6;
        s.resize(std::min<size_t>(s.size(), be16(7) ? be16(7) : s.size()));
        start_data_in(std::move(s));
        break;
    }
    default:
        fprintf(stderr, "ata: %s: unsupported ATAPI packet %02x\n", d->name_.c_str(), p[0]);
        finish(true, 0x05, 0x20);                        // ILLEGAL REQUEST / invalid command
        break;
    }
}

}  // namespace hk
