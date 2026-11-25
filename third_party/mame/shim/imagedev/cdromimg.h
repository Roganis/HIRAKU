// SPDX-License-Identifier: GPL-3.0-or-later
// HIRAKU: stand-ins for MAME's cdrom_file (util/cdrom.h) and cdrom_image_device
// (imagedev/cdromimg.h), reading a .cue/.bin or .iso instead of a CHD. The track
// types, MSF helpers and read_data() conversions are MAME's; what a CHD's metadata
// would hold (logical frame offsets, the lead-out) is built from the cue sheet.
// Pregaps as MAME lays them out: a PREGAP is not in the file (its frames read as zeros), an
// INDEX 00 pregap is (the track starts logically at INDEX 01); POSTGAP counts against the next track.
#pragma once

#include "emu.h"
#include "notifier.h"
#include "imagedev/image.h"

#include <map>
#include <memory>

namespace hk { class DiskAhead; }   // runtime/dev/diskahead.h

class cdrom_file {
public:
    static constexpr u32 MAX_TRACKS = 99, MAX_SECTOR_DATA = 2352, MAX_SUBCODE_DATA = 96;
    enum {
        CD_TRACK_MODE1 = 0, CD_TRACK_MODE1_RAW, CD_TRACK_MODE2, CD_TRACK_MODE2_FORM1, CD_TRACK_MODE2_FORM2,
        CD_TRACK_MODE2_FORM_MIX, CD_TRACK_MODE2_RAW, CD_TRACK_AUDIO, CD_TRACK_RAW_DONTCARE
    };
    enum { CD_SUB_NORMAL = 0, CD_SUB_RAW, CD_SUB_NONE };
    enum { CD_FLAG_GDROM = 1, CD_FLAG_GDROMLE = 2, CD_FLAG_MULTISESSION = 4 };
    enum { CD_FLAG_CONTROL_PREEMPHASIS = 1, CD_FLAG_CONTROL_DIGITAL_COPY_PERMITTED = 2, CD_FLAG_CONTROL_DATA_TRACK = 4, CD_FLAG_CONTROL_4CH = 8 };
    enum { CD_FLAG_ADR_START_TIME = 1, CD_FLAG_ADR_CATALOG_CODE, CD_FLAG_ADR_ISRC_CODE };
    struct track_info {
        u32 trktype = 0, subtype = CD_SUB_NONE, datasize = 0, subsize = 0, frames = 0, extraframes = 0, pregap = 0, postgap = 0;
        u32 pgtype = 0, pgsub = 0, pgdatasize = 0, pgsubsize = 0, control_flags = 0, session = 0;
        u32 padframes = 0, splitframes = 0, logframeofs = 0, physframeofs = 0, chdframeofs = 0, logframes = 0, multicuearea = 0;
    };
    struct toc {
        u32 numtrks = 0, numsessions = 1, flags = 0;
        track_info tracks[MAX_TRACKS + 1];
    };

    static inline u32 msf_to_lba(u32 msf) { return (((msf & 0x00ff0000) >> 16) * 60 * 75) + (((msf & 0x0000ff00) >> 8) * 75) + ((msf & 0x000000ff) >> 0); }
    static inline u32 lba_to_msf(u32 lba) {
        u8 m = lba / (60 * 75);
        lba -= m * (60 * 75);
        u8 s = lba / 75, f = lba % 75;
        return ((m / 10) << 20) | ((m % 10) << 16) | ((s / 10) << 12) | ((s % 10) << 8) | ((f / 10) << 4) | ((f % 10) << 0);
    }
    static inline u32 lba_to_msf_alt(int lba) {
        return (((lba / (60 * 75)) & 0xff) << 16) | ((((lba / 75) % 60) & 0xff) << 8) | ((lba % 75) & 0xff);
    }

    bool open(const std::string &path);
    bool read_data(u32 lbasector, void *buffer, u32 datatype, bool phys = false);
    bool read_subcode(u32, void *, bool = false) { return false; }
    u32 get_track(u32 frame) const;
    u32 get_track_start(u32 track) const { return cdtoc.tracks[track == 0xaa ? cdtoc.numtrks : track].logframeofs; }
    u32 get_track_index(u32) const { return 1; }
    int get_last_session() const { return cdtoc.numsessions ? cdtoc.numsessions : 1; }
    int get_last_track() const { return cdtoc.numtrks; }
    int get_adr_control(int track) const {
        if (track == 0xaa) track = get_last_track() - 1;
        int adrctl = (CD_FLAG_ADR_START_TIME << 4) | (cdtoc.tracks[track].control_flags & 0x0f);
        if (cdtoc.tracks[track].trktype != CD_TRACK_AUDIO) adrctl |= CD_FLAG_CONTROL_DATA_TRACK;
        return adrctl;
    }
    int get_track_type(int track) const { return cdtoc.tracks[track].trktype; }
    const toc &get_toc() const { return cdtoc; }
    ~cdrom_file();

private:
    bool read_partial(void *dst, u32 lba, u32 track, u32 offset, u32 len);
    struct file_part { FILE *f = nullptr; u64 offset = 0; bool swap = false; };   // where each track's frames start;
                                                                               // swap: little-endian audio (cue BINARY)
    toc cdtoc;
    file_part m_part[MAX_TRACKS + 1];
    std::vector<FILE *> m_files;
    // HIRAKU: each plain file's read-ahead (a CHD's has its own), which times the reads too
    std::map<FILE *, std::unique_ptr<hk::DiskAhead>> m_ahead;
    void add_ahead(FILE *f, const std::string &path);
};

class cdrom_image_device : public device_t, public device_image_interface {
public:
    cdrom_image_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock = 0);
    cdrom_image_device(const machine_config &mconfig, device_type type, const char *tag, device_t *owner, u32 clock);
    cdrom_image_device &set_interface(const char *) { return *this; }
    // HIRAKU: the disc image (.cue or .iso), before the machine starts; none = no disc
    static void hk_set_path(const std::string &tag, const std::string &path);

    bool exists() const { return m_cd != nullptr; }
    bool is_loaded() const { return m_cd != nullptr; }
    int get_last_track() const { return m_cd ? m_cd->get_last_track() : 0; }
    int get_last_session() const { return m_cd ? m_cd->get_last_session() : 0; }
    u32 get_track(u32 frame) const { return m_cd ? m_cd->get_track(frame) : 0; }
    u32 get_track_index(u32 frame) const { return m_cd ? m_cd->get_track_index(frame) : 0; }
    u32 get_track_start(u32 track) const { return m_cd ? m_cd->get_track_start(track) : 0; }
    bool read_data(u32 lba, void *buf, u32 type, bool phys = false) { return m_cd && m_cd->read_data(lba, buf, type, phys); }
    bool read_subcode(u32, void *, bool = false) { return false; }
    int get_adr_control(int track) const { return m_cd ? m_cd->get_adr_control(track) : 0; }
    const cdrom_file::toc &get_toc() const { return m_cd->get_toc(); }
    int get_track_type(int track) const { return m_cd ? m_cd->get_track_type(track) : 0; }
    bool is_cd() const { return m_cd != nullptr; }
    bool is_gd() const { return false; }
    bool is_dvd() const { return false; }
    u32 sequence_counter() const { return m_sequence; }

protected:
    void device_start() override;

private:
    std::unique_ptr<cdrom_file> m_cd;
    u32 m_sequence = 0;
};

DECLARE_DEVICE_TYPE(CDROM, cdrom_image_device)
DECLARE_DEVICE_TYPE(GDROM, gdrom_image_device)
DECLARE_DEVICE_TYPE(DVDROM, dvdrom_image_device)
// GD-ROM and DVD images: not available (the 573 has neither); creating one is fatal
class gdrom_image_device : public cdrom_image_device {
public:
    gdrom_image_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock = 0);
};
class dvdrom_image_device : public cdrom_image_device {
public:
    dvdrom_image_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock = 0);
};

// MAME's hand-operated CD player interface (the UI's media controls; unused here)
class device_cd_player_interface : public device_interface {
public:
    static constexpr u32 FRAMES_PER_SECOND = 75;
    enum class transport : u8 { STOPPED, PLAYING, PAUSED };
    virtual ~device_cd_player_interface() = default;
    virtual cdrom_image_device &cd_image() = 0;
    virtual void play() = 0;
    virtual void pause() = 0;
    virtual void stop() = 0;
    virtual void previous_track() = 0;
    virtual void next_track() = 0;
    virtual void select_track(int track) = 0;
    virtual transport state() = 0;
    virtual int track() = 0;
    virtual int track_count() = 0;
    virtual u32 track_elapsed_frames() = 0;
    virtual u32 track_length_frames() = 0;
protected:
    device_cd_player_interface(const machine_config &, device_t &device) : device_interface(device, "cdplayer") {}
};
