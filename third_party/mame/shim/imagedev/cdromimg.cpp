// SPDX-License-Identifier: GPL-3.0-or-later
// HIRAKU: see cdromimg.h.
#include <algorithm>
#include "imagedev/cdromimg.h"

#include "dev/diskahead.h"   // HIRAKU's read-ahead of a plain image
#include "dev/image.h"   // HIRAKU's image reader: a CHD reads as chdman's extract

#include <map>

DEFINE_DEVICE_TYPE(CDROM, cdrom_image_device, "cdrom_image", "CD-ROM Image")

namespace {
std::map<std::string, std::string> &paths() { static std::map<std::string, std::string> p; return p; }

// a cue sheet track type: MAME's track type and bytes per sector in the file
bool track_type(const std::string &t, u32 &type, u32 &size) {
    if (t == "MODE1/2048") { type = cdrom_file::CD_TRACK_MODE1; size = 2048; }
    else if (t == "MODE1/2352") { type = cdrom_file::CD_TRACK_MODE1_RAW; size = 2352; }
    else if (t == "MODE2/2336") { type = cdrom_file::CD_TRACK_MODE2; size = 2336; }
    else if (t == "MODE2/2352") { type = cdrom_file::CD_TRACK_MODE2_RAW; size = 2352; }
    else if (t == "AUDIO") { type = cdrom_file::CD_TRACK_AUDIO; size = 2352; }
    else return false;
    return true;
}
}  // namespace

cdrom_file::~cdrom_file() {
    m_ahead.clear();
    for (FILE *f : m_files) hk::image::close(f);
}

void cdrom_file::add_ahead(FILE *f, const std::string &path) {
    const int64_t size = hk::image::size(f);
    if (size > 0 && !hk::image::is_chd(path) && !m_ahead.count(f)) m_ahead[f] = std::make_unique<hk::DiskAhead>(path, (uint64_t)size);
}

bool cdrom_file::open(const std::string &path) {
    auto ends = [&](const char *e) { size_t n = strlen(e); return path.size() >= n && strcasecmp(path.c_str() + path.size() - n, e) == 0; };
    // a CD CHD reads as the .cue and .bin chdman extractcd writes (runtime/dev/image.h)
    const bool chd_cd = hk::image::chd_kind(path) == "cd";
    if (!ends(".cue") && !chd_cd) {
        // an .iso: one MODE1/2048 track
        FILE *f = hk::image::fopen_image(path);
        if (!f) return false;
        m_files.push_back(f);
        add_ahead(f, path);
        u32 frames = (u32)(hk::image::size(f) / 2048);
        cdtoc.numtrks = 1;
        track_info &t = cdtoc.tracks[0];
        t.trktype = CD_TRACK_MODE1;
        t.datasize = 2048;
        t.frames = t.logframes = frames;
        t.control_flags = CD_FLAG_CONTROL_DATA_TRACK;
        m_part[0] = {f, 0};
        cdtoc.tracks[1].logframeofs = frames;
        return true;
    }
    FILE *cue = hk::image::fopen_cue(path);
    if (!cue) return false;
    std::string dir = path.substr(0, path.find_last_of('/') + 1);
    char line[1024];
    FILE *cur = nullptr;
    bool cur_le = true;   // the FILE's byte order for audio: BINARY little-endian, MOTOROLA big-endian
    int ntr = -1;
    // per track: its file; INDEX 00 and 01 within it (frames; -1: none); PREGAP and POSTGAP
    struct cue_track { FILE *f = nullptr; int index0 = -1, index1 = 0; u32 pregap_cmd = 0, postgap = 0; bool le = true; };
    std::vector<cue_track> ct;
    auto msf = [](const char *t) { int m = 0, s = 0, f = 0; sscanf(t, "%d:%d:%d", &m, &s, &f); return (m * 60 + s) * 75 + f; };
    while (fgets(line, sizeof line, cue)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!strncasecmp(p, "FILE ", 5)) {
            std::string name;
            char *q = p + 5;
            const char *rest = q;
            if (*q == '"') { char *e = strchr(q + 1, '"'); name.assign(q + 1, e ? e : q + 1); rest = e ? e + 1 : q + 1; }
            else { char *e = strpbrk(q, " \t\r\n"); name.assign(q, e ? e : q + strlen(q)); rest = e ? e : q + strlen(q); }
            cur_le = true;   // "MOTOROLA" anywhere after the name, any case (no strcasestr on Windows)
            for (const char *s = rest; *s; s++)
                if (!strncasecmp(s, "MOTOROLA", 8)) { cur_le = false; break; }
            const std::string file = name.size() && name[0] == '/' ? name : dir + name;
            cur = hk::image::fopen_image(file);
            if (!cur) { fclose(cue); return false; }
            m_files.push_back(cur);
            add_ahead(cur, file);
        } else if (!strncasecmp(p, "TRACK ", 6)) {
            char type[32] = {};
            int n = 0;
            sscanf(p + 6, "%d %31s", &n, type);
            ntr++;
            u32 tt, size;
            if (!track_type(type, tt, size)) { fclose(cue); return false; }
            track_info &t = cdtoc.tracks[ntr];
            t.trktype = tt;
            t.datasize = size;
            t.control_flags = tt == CD_TRACK_AUDIO ? 0 : CD_FLAG_CONTROL_DATA_TRACK;
            ct.push_back({cur, -1, 0, 0, 0, cur_le});
        } else if (!strncasecmp(p, "INDEX 00 ", 9) && ntr >= 0) {
            ct[ntr].index0 = msf(p + 9);
        } else if (!strncasecmp(p, "INDEX 01 ", 9) && ntr >= 0) {
            ct[ntr].index1 = msf(p + 9);
        } else if (!strncasecmp(p, "PREGAP ", 7) && ntr >= 0) {
            ct[ntr].pregap_cmd = msf(p + 7);
        } else if (!strncasecmp(p, "POSTGAP ", 8) && ntr >= 0) {
            ct[ntr].postgap = msf(p + 8);
        }
    }
    fclose(cue);
    if (ntr < 0) return false;
    cdtoc.numtrks = ntr + 1;
    // MAME's cue reading and TOC (util/cdrom.cpp): a track's data in the file starts at INDEX 00
    // when there is one (its pregap is in the file: pgdatasize), else at INDEX 01; a PREGAP isn't
    // in the file. Logical frames (what the drive reports and reads by) as MAME lays them out.
    u32 logofs = 0, physofs = 0;
    u64 byte = 0;   // where the track's data starts in its file: the tracks before it at their own sizes
    auto file_start = [&](u32 k) { return ct[k].index0 >= 0 ? (u32)ct[k].index0 : (u32)ct[k].index1; };
    for (u32 k = 0; k < cdtoc.numtrks; k++) {
        track_info &t = cdtoc.tracks[k];
        cue_track &c = ct[k];
        if (c.index0 >= 0 && !c.pregap_cmd) { t.pregap = c.index1 - c.index0; t.pgdatasize = t.datasize; }
        else { t.pregap = c.pregap_cmd; t.pgdatasize = 0; }
        t.postgap = c.postgap;
        if (k == 0 || ct[k - 1].f != c.f) byte = (u64)file_start(k) * t.datasize;   // a file's first track
        // frames in the file: up to the next track's start in the same file (cue positions count
        // frames whatever their size), or the file's end
        const u64 start = byte;
        if (k + 1 < cdtoc.numtrks && ct[k + 1].f == c.f) {
            t.frames = file_start(k + 1) - file_start(k);
        } else {
            // (image::size, not stdio: on Windows a CHD's stream answers only image.h's calls)
            t.frames = (u32)(((u64)std::max<int64_t>(hk::image::size(c.f), 0) - start) / t.datasize);
        }
        byte += (u64)t.frames * t.datasize;
        t.logframeofs = 0;
        if (t.pgdatasize == 0) logofs += t.pregap;
        else t.logframeofs = t.pregap;
        t.physframeofs = physofs;
        t.logframeofs += logofs;
        t.logframes = t.frames - t.pregap;
        logofs += t.postgap;
        physofs += t.frames;
        logofs += t.frames;
        m_part[k] = {c.f, start, t.trktype == CD_TRACK_AUDIO && c.le};
    }
    cdtoc.tracks[cdtoc.numtrks].logframeofs = logofs;   // the lead-out
    cdtoc.tracks[cdtoc.numtrks].physframeofs = physofs;
    return true;
}

u32 cdrom_file::get_track(u32 frame) const {
    u32 track = 0;
    for (track = 0; track < cdtoc.numtrks; track++)
        if (frame < cdtoc.tracks[track + 1].logframeofs) break;
    return track;
}

bool cdrom_file::read_partial(void *dst, u32 lba, u32 track, u32 offset, u32 len) {
    const track_info &t = cdtoc.tracks[track];
    const file_part &fp = m_part[track];
    // MAME read_partial_sector(): a pregap that isn't in the file reads as zeros; one that is
    // comes before the track's logical start (INDEX 01)
    if (t.pgdatasize == 0 && lba < t.logframeofs) {
        memset(dst, 0, len);
        return true;
    }
    const u32 frame = lba - t.logframeofs + (t.pgdatasize ? t.pregap : 0);
    u64 pos = fp.offset + (u64)frame * t.datasize + offset;
    auto a = m_ahead.find(fp.f);
    hk::DiskAhead *ahead = a == m_ahead.end() ? nullptr : a->second.get();
    if (ahead) ahead->before(pos, len);
    const bool ok = hk::image::pread(fp.f, dst, len, pos) == (ssize_t)len;
    if (ahead) ahead->after();
    return ok;
}

bool cdrom_file::read_data(u32 lbasector, void *buffer, u32 datatype, bool) {
    // MAME's conversions between the stored and the requested sector type
    u32 track = get_track(lbasector);
    if (track >= cdtoc.numtrks) return false;
    u32 tracktype = cdtoc.tracks[track].trktype;
    if (datatype == tracktype || datatype == CD_TRACK_RAW_DONTCARE) {
        if (!read_partial(buffer, lbasector, track, 0, cdtoc.tracks[track].datasize)) return false;
        // MAME keeps CD audio big-endian (as a CHD stores it) and swaps a little-endian file's
        if (m_part[track].swap) {
            u8 *b = (u8 *)buffer;
            for (u32 i = 0; i + 1 < cdtoc.tracks[track].datasize; i += 2) std::swap(b[i], b[i + 1]);
        }
        return true;
    }
    if (datatype == CD_TRACK_MODE1 && tracktype == CD_TRACK_MODE1_RAW) return read_partial(buffer, lbasector, track, 16, 2048);
    if (datatype == CD_TRACK_MODE1_RAW && tracktype == CD_TRACK_MODE1) {
        u8 *b = (u8 *)buffer;
        static const u8 sync[12] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
        memcpy(b, sync, 12);
        u32 msf = lba_to_msf(lbasector);
        b[12] = u8(msf >> 16); b[13] = u8(msf >> 8); b[14] = u8(msf);
        b[15] = 1;
        return read_partial(b + 16, lbasector, track, 0, 2048);
    }
    if (datatype == CD_TRACK_MODE1 && (tracktype == CD_TRACK_MODE2_FORM1 || tracktype == CD_TRACK_MODE2_RAW)) return read_partial(buffer, lbasector, track, 24, 2048);
    if (datatype == CD_TRACK_MODE1 && tracktype == CD_TRACK_MODE2_FORM_MIX) return read_partial(buffer, lbasector, track, 8, 2048);
    if (datatype == CD_TRACK_MODE2 && (tracktype == CD_TRACK_MODE1_RAW || tracktype == CD_TRACK_MODE2_RAW)) return read_partial(buffer, lbasector, track, 16, 2336);
    return false;
}

cdrom_image_device::cdrom_image_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock)
    : device_t(mconfig, CDROM, tag, owner, clock), device_image_interface(mconfig, *this) {}
cdrom_image_device::cdrom_image_device(const machine_config &mconfig, device_type type, const char *tag, device_t *owner, u32 clock)
    : device_t(mconfig, type, tag, owner, clock), device_image_interface(mconfig, *this) {}

DEFINE_DEVICE_TYPE(GDROM, gdrom_image_device, "gdrom_image", "GD-ROM Image (unavailable)")
DEFINE_DEVICE_TYPE(DVDROM, dvdrom_image_device, "dvdrom_image", "DVD-ROM Image (unavailable)")
gdrom_image_device::gdrom_image_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock)
    : cdrom_image_device(mconfig, GDROM, tag, owner, clock) { fatalerror("%s: GD-ROM images are not available in HIRAKU", tag); }
dvdrom_image_device::dvdrom_image_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock)
    : cdrom_image_device(mconfig, DVDROM, tag, owner, clock) { fatalerror("%s: DVD images are not available here", tag); }

void cdrom_image_device::hk_set_path(const std::string &tag, const std::string &path) { paths()[tag] = path; }

void cdrom_image_device::device_start() {
    auto it = paths().find(tag());
    if (it == paths().end() || it->second.empty()) return;
    m_cd = std::make_unique<cdrom_file>();
    if (!m_cd->open(it->second)) fatalerror("%s: cannot open the disc image %s", tag(), it->second.c_str());
    m_sequence++;
}
