// SPDX-License-Identifier: GPL-3.0-or-later
// The network PCB unit, high-level (npu.h). The registers, the start-up and the calls' formats
// are windyfairy's (k573npu_card.cpp, BSD-3-Clause); the disks are folders.
#include "sys573/npu.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>

namespace fs = std::filesystem;

DEFINE_DEVICE_TYPE(KONAMI_573_NPU_HLE, k573npu_hle_device, "k573npu_hle", "Konami 573 Network PCB Unit (HLE)")

namespace {

enum : int32_t { FAIL = -2 };   // what the unit answers for any failure (COMMAND_UNSUCCESSFUL)

enum : uint32_t { FD_HDD = 0x00000000, FD_RAM = 0x40000000 };   // descriptors' top bits: the disk

// iomanX-like open flags
enum : uint32_t { O_READ = 0x01, O_WRITE = 0x02, O_CREATE = 0x200 };

constexpr uint32_t DIRENT_SIZE = 32;   // a directory record on the unit's disk (name[20], size, ?, offset)

// The most an answer can carry: its size is a 16-bit word, and the FIFO out holds 0x8000 words less
// one, three of them the header. A call asking for more (a read, a devctl) gets this much: what
// the game names as a length is never the size of something made here
constexpr uint32_t ANSWER_MAX = 0xfff0;

void put32(std::vector<uint8_t> &v, size_t at, uint32_t x) {
    if (v.size() < at + 4) v.resize(at + 4);
    for (int i = 0; i < 4; i++) v[at + i] = uint8_t(x >> (8 * i));
}

// "atam:/a/b" -> "/a/b"; "ram:x" -> "x"
std::string after_device(const std::string &p) {
    size_t i = p.find(':');
    if (i == std::string::npos) return p;
    while (i < p.size() && p[i] == ':') i++;
    return p.substr(i);
}

}  // namespace

// ---- a disk: a folder ----

class k573npu_hle_device::Disk {
public:
    Disk(std::string root, uint32_t fd_base, bool wipe) : root_(std::move(root)), fd_base_(fd_base) {
        if (root_.empty()) return;
        std::error_code ec;
        if (wipe) fs::remove_all(root_, ec);
        fs::create_directories(root_, ec);
    }

    void reset() {
        for (auto &[fd, f] : files_) if (f.fp) fclose(f.fp);
        files_.clear();
        next_fd_ = fd_base_;
        mounted_ = false;
        cwd_ = "/";
    }

    int32_t mount() {
        if (!present() || !fs::exists(root_ + "/.format")) return FAIL;
        mounted_ = true;
        cwd_ = "/";
        return 0;
    }
    int32_t umount() { return present() ? 0 : FAIL; }

    // the game's FORMAT: its size ("atam:0,4G"), the first sector, the partitions, the node table
    // count and the block size. The figures are windyfairy's PythonFS header's; the game reads
    // them back with devctl 2.
    int32_t format(const std::string &spec, uint32_t start_lba, uint32_t count1, uint32_t count2, uint32_t tables, uint32_t block) {
        if (!present() || count1 + count2 == 0) return FAIL;
        uint64_t size = 0;
        size_t i = spec.find(',');
        if (i != std::string::npos) {
            const char *s = spec.c_str() + i + 1;
            char *end = nullptr;
            size = strtoull(s, &end, 10);
            if (*end == 'k') size <<= 1;
            else if (*end == 'M') size <<= 11;
            else if (*end == 'G') size <<= 21;
            else size = 0;
        }
        uint32_t mult = 0;
        while ((block >> mult) > 0x200) mult++;
        mult &= 0x1f;
        const uint32_t per_partition = uint32_t((size - start_lba) / (count1 + count2));
        const uint32_t table_sectors = ((per_partition >> mult) * 4 + 0x1ff) >> 9;
        std::error_code ec;
        for (auto &e : fs::directory_iterator(root_, ec)) fs::remove_all(e.path(), ec);
        std::ofstream(root_ + "/.format") << per_partition << ' ' << count1 << ' ' << count2 << ' ' << table_sectors
                                           << ' ' << tables << ' ' << block << '\n';
        return mount();
    }

    int32_t devctl(uint32_t type, std::vector<uint8_t> &out) {
        if (!mounted_ || type != 2) return FAIL;
        uint32_t per_partition = 0, count1 = 0, count2 = 0, table_sectors = 0;
        std::ifstream(root_ + "/.format") >> per_partition >> count1 >> count2 >> table_sectors;
        put32(out, 0, per_partition);
        put32(out, 4, count1);
        put32(out, 8, count2);
        put32(out, 12, table_sectors >> 5);
        return 0;
    }

    int32_t chdir(const std::string &path) {
        std::string p;
        if (!resolve(path, p) || !fs::is_directory(host(p))) return FAIL;
        cwd_ = p;
        return 0;
    }

    int32_t open(const std::string &path, uint32_t flags) {
        std::string p;
        if (!resolve(path, p)) return FAIL;
        const std::string h = host(p);
        std::error_code ec;
        File f;
        f.flags = flags;
        f.path = p;
        if (fs::exists(h, ec) && !(flags & O_CREATE)) {
            f.is_dir = fs::is_directory(h, ec);
            if (!f.is_dir) {
                f.fp = fopen(h.c_str(), (flags & O_WRITE) ? "r+b" : "rb");
                if (!f.fp) return FAIL;
            }
        } else if (flags & O_WRITE) {
            if (fs::is_directory(h, ec) || !fs::is_directory(fs::path(h).parent_path(), ec)) return FAIL;
            f.fp = fopen(h.c_str(), "w+b");
            if (!f.fp) return FAIL;
        } else {
            return FAIL;
        }
        return add(std::move(f));
    }

    int32_t close(int32_t fd) {
        auto it = files_.find(fd);
        if (it == files_.end()) return FAIL;
        if (it->second.fp) fclose(it->second.fp);
        files_.erase(it);
        return 0;
    }

    int32_t read(int32_t fd, uint32_t len, std::vector<uint8_t> &out) {
        File *f = file(fd);
        if (!f || f->is_dir || f->listing) return FAIL;
        if (!(f->flags & O_READ)) return 0;
        len = std::min(len, ANSWER_MAX - 4);
        out.resize(len);
        fseek(f->fp, (long)f->pos, SEEK_SET);
        size_t n = fread(out.data(), 1, len, f->fp);
        out.resize(n);
        f->pos += uint32_t(n);
        return int32_t(n);
    }

    int32_t write(int32_t fd, const std::string &data) {
        File *f = file(fd);
        if (!f || f->is_dir || f->listing || !(f->flags & O_WRITE)) return FAIL;
        fseek(f->fp, (long)f->pos, SEEK_SET);
        size_t n = fwrite(data.data(), 1, data.size(), f->fp);
        fflush(f->fp);
        f->pos += uint32_t(n);
        return n == data.size() ? int32_t(n) : FAIL;
    }

    int32_t lseek(int32_t fd, uint32_t offset, uint32_t whence) {
        File *f = file(fd);
        if (!f || !f->fp) return FAIL;
        fseek(f->fp, 0, SEEK_END);
        const int64_t size = ftell(f->fp);
        int64_t at = whence == 0 ? int64_t(offset)
                   : whence == 1 ? int64_t(f->pos) + int32_t(offset)
                   : whence == 2 ? size + int32_t(offset) : -1;
        if (at < 0 || at > size) return FAIL;
        f->pos = uint32_t(at);
        return int32_t(at);
    }

    int32_t dopen(const std::string &path) {
        std::string p;
        if (!resolve(path, p) || !fs::is_directory(host(p))) return FAIL;
        File f;
        f.listing = true;
        f.path = p;
        f.entries.push_back({".", dir_size(host(p))});
        f.entries.push_back({"..", 0});
        for (auto &[name, size] : list(host(p))) f.entries.push_back({name, size});
        return add(std::move(f));
    }

    // a directory record (0x144 bytes: a stat, then the name), or false at the end
    bool dread(int32_t fd, std::vector<uint8_t> &out) {
        File *f = file(fd);
        if (!f || !f->listing || f->pos >= f->entries.size()) return false;
        auto &[name, size] = f->entries[f->pos++];
        out.assign(0x144, 0);
        put32(out, 8, size);   // stat.size; windyfairy's unit fills in nothing else
        std::copy_n(name.begin(), std::min<size_t>(name.size(), 255), out.begin() + 0x40);
        return true;
    }

    int32_t getstat(const std::string &path, std::vector<uint8_t> &out) {
        std::string p;
        if (!resolve(path, p)) return FAIL;
        const std::string h = host(p);
        std::error_code ec;
        if (!fs::exists(h, ec) || p == "/") return FAIL;
        out.assign(0x40, 0);
        const bool dir = fs::is_directory(h, ec);
        put32(out, 4, dir ? 1 : 0);   // attr: the record's top byte, 1 for a folder
        put32(out, 8, dir ? dir_size(h) : uint32_t(fs::file_size(h, ec)));
        return 0;
    }

    int32_t mkdir(const std::string &path) {
        std::string p;
        if (!resolve(path, p)) return FAIL;
        std::error_code ec;
        fs::create_directories(host(p), ec);
        return fs::is_directory(host(p)) ? 0 : FAIL;
    }

    int32_t rmdir(const std::string &path) {
        std::string p;
        if (!resolve(path, p) || p == "/" || !fs::is_directory(host(p))) return FAIL;
        std::error_code ec;
        return fs::remove(host(p), ec) ? 0 : FAIL;   // only when empty
    }

    int32_t remove(const std::string &path) {
        const std::string leaf = path.substr(path.find_last_of("/:") + 1);
        if (leaf == "." || leaf == "..") return 0;
        std::string p;
        if (!resolve(path, p)) return FAIL;
        std::error_code ec;
        if (fs::is_directory(host(p), ec)) return FAIL;
        return fs::remove(host(p), ec) ? 0 : FAIL;
    }

    bool owns(int32_t fd) const { return (uint32_t(fd) & 0xf000'0000) == fd_base_; }

private:
    struct File {
        uint32_t flags = 0;
        std::string path;
        FILE *fp = nullptr;
        bool is_dir = false, listing = false;
        uint32_t pos = 0;   // a file's position, a listing's next record
        std::vector<std::pair<std::string, uint32_t>> entries;
    };

    bool present() const { return !root_.empty(); }

    // the game's path to one from the disk's root, or false (no disk, not mounted, or above the root)
    bool resolve(const std::string &path, std::string &out) const {
        if (!present() || !mounted_) return false;
        std::string p = after_device(path);
        if (p.empty() || p[0] != '/') p = cwd_ + "/" + p;
        std::vector<std::string> parts;
        size_t i = 0;
        while (i <= p.size()) {
            size_t j = p.find('/', i);
            if (j == std::string::npos) j = p.size();
            std::string s = p.substr(i, j - i);
            if (s == "..") {
                if (parts.empty()) return false;
                parts.pop_back();
            } else if (!s.empty() && s != ".") {
                parts.push_back(s);
            }
            i = j + 1;
        }
        out.clear();
        for (auto &s : parts) out += "/" + s;
        if (out.empty()) out = "/";
        return true;
    }
    std::string host(const std::string &p) const { return p == "/" ? root_ : root_ + p; }

    // a folder's entries, sorted (the disk's own order is the order they were made in), without .format
    static std::vector<std::pair<std::string, uint32_t>> list(const std::string &h) {
        std::vector<std::pair<std::string, uint32_t>> v;
        std::error_code ec;
        for (auto &e : fs::directory_iterator(h, ec)) {
            std::string name = e.path().filename().string();
            if (name == ".format") continue;
            v.push_back({name, e.is_directory() ? dir_size(e.path().string()) : uint32_t(e.file_size(ec))});
        }
        std::sort(v.begin(), v.end());
        return v;
    }
    // a folder's size, as the unit's disk has it: its records, . and .. included
    static uint32_t dir_size(const std::string &h) {
        uint32_t n = 2;
        std::error_code ec;
        for (auto &e : fs::directory_iterator(h, ec)) n += e.path().filename() != ".format";
        return n * DIRENT_SIZE;
    }

    int32_t add(File f) {
        uint32_t fd = next_fd_;
        while (files_.count(int32_t(fd))) fd++;
        next_fd_ = fd + 1;
        files_[int32_t(fd)] = std::move(f);
        return int32_t(fd);
    }
    File *file(int32_t fd) {
        auto it = files_.find(fd);
        return it == files_.end() ? nullptr : &it->second;
    }

    std::string root_;
    uint32_t fd_base_;
    uint32_t next_fd_ = 0;
    bool mounted_ = false;
    std::string cwd_ = "/";
    std::map<int32_t, File> files_;
};

// ---- the card ----

std::string k573npu_hle_device::Args::bytes(size_t n) {
    std::string s;
    n = std::min(n, (w.size() - pos) * 2);   // (no more than the call's words hold, whatever length it names)
    if ((n & 1) && w.size() - pos >= n) {
        // windyfairy's unit: an odd count comes one byte a word
        for (size_t i = 0; i < n; i++) s += char(u16() & 0xff);
        return s;
    }
    for (size_t i = 0; i < n; i += 2) {
        uint16_t v = u16();
        s += char(v & 0xff);
        if (i + 1 < n) s += char(v >> 8);
    }
    return s;
}

k573npu_hle_device::k573npu_hle_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock)
    : device_t(mconfig, KONAMI_573_NPU_HLE, tag, owner, clock), device_pccard_interface(mconfig, *this) {}

void k573npu_hle_device::device_start() {
    const int level = getenv("HK_573_NPU_LOG") ? atoi(getenv("HK_573_NPU_LOG")) : 0;
    m_log = level >= 1;
    m_log_packets = level >= 2;
    m_log_regs = level >= 3;
    m_hdd = std::make_unique<Disk>(m_hdd_dir, FD_HDD, false);
    m_ram = std::make_unique<Disk>(m_ram_dir, FD_RAM, true);   // power-on: an empty RAM disk
    m_poll_timer = timer_alloc(&k573npu_hle_device::poll_network, this);
}

void k573npu_hle_device::device_reset() {
    m_hdd->reset();
    m_ram->reset();
    m_cd1_cb(0);   // card detect, active low: the card is in
    m_cd2_cb(0);
    m_state_requested = m_state_cur = m_val_unk = 0;
    m_init_cur_step = 0;
    m_buffer_write_length = m_buffer_read_length = 0;
    m_packet_id = 0;
    m_in.clear();
    m_out.clear();
    m_waiting.clear();
    m_poll_timer->adjust(attotime::never);
}

uint16_t k573npu_hle_device::read_memory(offs_t offset, uint16_t mem_mask) {
    uint16_t r = 0;
    switch (offset * 2) {
    case 0x00: r = m_state_requested; break;
    case 0x02: r = m_state_cur; break;
    case 0x06: r = 5; break;
    case 0x20:   // the FIFO, out
        if (!m_out.empty()) r = m_out.pop();
        if (m_state_requested != 8 && m_state_requested != 0x0f) r ^= 0xffff;
        if (m_buffer_read_length) m_buffer_read_length--;
        break;
    case 0x24:
    case 0x2c: r = uint16_t(0x100 - (m_in.length() % 0x100)); break;   // room in the FIFO in
    case 0x22:
    case 0x26: r = uint16_t(m_buffer_read_length); break;
    case 0x30: r = m_buffer_write_length > 0; break;
    case 0x34: r = m_out.wpos; break;   // how far the unit has written its answers
    case 0x36: r = m_in.rpos; break;    // how far it has taken the 573's calls
    case 0x3a: r = 0x5963; break;   // the FPGA is running
    case 0x3c: r = m_val_unk; break;
    }
    if (m_log_regs) fprintf(stderr, "npu: %.6f r %02x = %04x\n", machine().time().as_double(), offset * 2, r);
    return r;
}

void k573npu_hle_device::write_memory(offs_t offset, uint16_t data, uint16_t mem_mask) {
    if (m_log_regs) fprintf(stderr, "npu: %.6f w %02x = %04x\n", machine().time().as_double(), offset * 2, data);
    switch (offset * 2) {
    case 0x00: {   // the state the 573 asks for
        const uint16_t prev = m_state_requested;
        m_state_requested = m_state_cur = data;
        if (m_init_cur_step) {
            m_state_cur ^= 0x0f;
            m_init_cur_step--;
        }
        if (data == 2) {   // a FIFO test: the unit sends 0..0x9ff
            m_in.clear();
            m_out.clear();
            for (int i = 0; i < 0xa00; i++) m_out.push(uint16_t(i));
        } else if (prev == 8 && data == 0x0f) {   // running: a ping (message 1) says so; the game
                                                   // answers pings with call 0
            m_out.clear();
            m_out.push(1);
            m_out.push(0);
            m_out.push(0);
        }
        break;
    }
    case 0x04: m_init_cur_step = data ? 5 : 0; break;
    case 0x20:   // the FIFO, in
        if (m_buffer_write_length) {
            m_in.push(data);
            if (!--m_buffer_write_length && (m_state_requested == 8 || m_state_requested == 0x0f)) call();
        }
        break;
    case 0x30:   // how much the 573 will write, or (bit 15) read
        if (data & 0x8000) m_buffer_read_length = data & 0x7fff;
        else m_buffer_write_length += data;
        break;
    // The 573's own ring positions: how far it has written its calls (0x34) and read the
    // answers (0x36). Its driver waits for the unit's to match them (0x800756c8 in drmn10m:
    // "the unit has taken everything"), so with nothing in between the unit's follow them; it
    // also sets both to 0 when it starts the link over. windyfairy's unit ignores these and
    // counts words received instead, which drifts from the game's count (calls written before
    // the FIFO was told to expect them): the game then waits out its timeouts, 5 to 30 s each.
    case 0x34:
        if (m_in.empty()) m_in.wpos = m_in.rpos = data;
        break;
    case 0x36:
        if (m_out.empty()) m_out.wpos = m_out.rpos = data;
        break;
    case 0x3c: m_val_unk = data; break;
    }
}

void k573npu_hle_device::reply(uint16_t code, const std::vector<uint8_t> &payload) {
    if (m_log_packets) {
        fprintf(stderr, "npu: %.3f reply %04x %04x %04x |", machine().time().as_double(), code, m_packet_id, unsigned(payload.size()));
        for (size_t i = 0; i < std::min<size_t>(payload.size(), 16); i++) fprintf(stderr, " %02x", payload[i]);
        fprintf(stderr, "\n");
    }
    m_out.push(code);
    m_out.push(m_packet_id);
    m_out.push(uint16_t(payload.size()));
    for (size_t i = 0; i < payload.size(); i += 2)
        m_out.push(uint16_t(payload[i] | (i + 1 < payload.size() ? payload[i + 1] << 8 : 0)));
}

k573npu_hle_device::Disk *k573npu_hle_device::disk_for_path(const std::string &path) {
    if (path.compare(0, 4, "atam") == 0) return m_hdd.get();
    if (path.compare(0, 3, "ram") == 0) return m_ram.get();
    return nullptr;
}

k573npu_hle_device::Disk *k573npu_hle_device::disk_for_fd(int32_t fd) {
    if (m_hdd->owns(fd)) return m_hdd.get();
    if (m_ram->owns(fd)) return m_ram.get();
    return nullptr;
}

void k573npu_hle_device::call() {
    // one call: its opcode, its id, its arguments' length in bytes, the arguments
    // (no more words than the FIFO holds: the length is the game's)
    const size_t held = m_in.length() / 2;
    if (held < 3) {   // not a call: taken, as the game counts its words taken
        while (!m_in.empty()) m_in.pop();
        return;
    }
    const size_t words = std::min<size_t>(3 + (m_in.at(2) + 1) / 2, held);
    Args a;
    for (size_t i = 0; i < words; i++) a.w.push_back(m_in.pop());
    const int op = a.u16() & 0xff;
    m_packet_id = a.u16();
    a.u16();
    if (m_log_packets) {
        fprintf(stderr, "npu: %.3f call", machine().time().as_double());
        for (size_t i = 0; i < std::min<size_t>(a.w.size(), 24); i++) fprintf(stderr, " %04x", a.w[i]);
        fprintf(stderr, a.w.size() > 24 ? " ...\n" : "\n");
    }

#define log(...) log_call(__VA_ARGS__)
    auto path_call = [&](const std::string &p, auto fn) -> int32_t {
        Disk *d = disk_for_path(p);
        return d ? fn(*d) : FAIL;
    };
    auto fd_call = [&](int32_t fd, auto fn) -> int32_t {
        Disk *d = disk_for_fd(fd);
        return d ? fn(*d) : FAIL;
    };

    switch (op) {
    case 0x00: break;   // no answer
    case 0x01:
    case 0x80:   // code upload
        reply(0, {});
        break;
    case 0x81: {   // memory read: the unit's id and MAC address
        const uint32_t addr = a.u32();
        const uint16_t len = a.u16();
        std::vector<uint8_t> p;
        if (addr == 0x80e00000 || addr == 0x80800000) p = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
        else if (addr == 0x80e00008 || addr == 0x80800008) p = {0x00, 0x06, 0x79, 0x00, 0x00, 0x01};
        else if (addr == 0x80e00010 || addr == 0x80800010) p = {1, 0};
        p.resize((len + 1) & ~1);
        m_out.push(0);
        m_out.push(m_packet_id);
        m_out.push(len);
        for (size_t i = 0; i < p.size(); i += 2) m_out.push(uint16_t(p[i] | p[i + 1] << 8));
        log("read memory %08x %d", addr, len);
        break;
    }
    case 0x82:   // run the uploaded code
        reply(0, {});
        m_state_cur = 0x0f;
        break;
    case 0x37: {   // link events on: the network module (0x80058de8 in drmn10m) has set its
                   // handler for event 0x39 and waits for one saying the link is up (its first
                   // word, kept at 0x80139834) before it asks for the settings (0x3a)
        reply32(1);
        std::vector<uint8_t> up(4);
        put32(up, 0, 1);
        reply(0x39, up);
        log("link events on: link up");
        break;
    }
    case 0x38: {
        std::vector<uint8_t> one(4);
        put32(one, 0, 1);
        reply(0x39, one);
        reply(0, {});
        break;
    }

    case 0x22: case 0x23: case 0x28: case 0x29: case 0x2a: case 0x2b: case 0x2c: case 0x2e:
        socket_call(op, a);
        break;

    case 0x3a: {   // a network setting: the LAN of npu.h's step 2 (192.168.50.0/24)
        const uint32_t addr = a.u32(), len = a.u32();
        std::vector<uint8_t> v;
        switch (addr) {
        case 0x101: v = {192, 168, 50, 2}; break;                            // the unit's address
        case 0x01: v = {255, 255, 255, 0}; break;                             // subnet mask
        case 0x03: case 0x36: case 0x2a: v = {192, 168, 50, 1}; break;       // gateway, DHCP, NTP
        case 0x06: v = {192, 168, 50, 1, 192, 168, 50, 1}; break;             // DNS servers
        case 0x0f: v.assign(32, 0); break;                                    // the local domain: none yet
        default: log("unknown setting %x", addr); break;
        }
        std::vector<uint8_t> p(6);
        put32(p, 0, len);
        p.insert(p.end(), v.begin(), v.end());
        p.resize(len + 4);
        log("setting %x (%u bytes)", addr, len);
        reply(0, p);
        break;
    }

    case 0x40: {   // filesystem initialise
        const uint32_t x = a.u32(), y = a.u32();
        log("fs init %x %x", x, y);
        reply32(0);
        break;
    }
    case 0x54: {   // format
        const std::string dev = a.str(0x40), spec2 = a.str(0x40);
        a.u32();
        const uint32_t start = a.u32(), c1 = a.u32(), c2 = a.u32(), tables = a.u32(), block = a.u32();
        const int32_t r = path_call(dev, [&](Disk &d) { return d.format(dev, start, c1, c2, tables, block); });
        log("format \"%s\" \"%s\" %u %u %u %u %u = %d", dev.c_str(), spec2.c_str(), start, c1, c2, tables, block, r);
        reply32(r);
        break;
    }
    case 0x51: {   // mount
        const std::string type = a.str(0x40);
        a.u32();
        const std::string dev = a.str(0x40);
        const int32_t r = path_call(dev, [&](Disk &d) { return d.mount(); });
        log("mount \"%s\" \"%s\" = %d", type.c_str(), dev.c_str(), r);
        reply32(r);
        break;
    }
    case 0x52: {   // umount
        const std::string dev = a.str(0x40);
        const int32_t r = path_call(dev, [&](Disk &d) { return d.umount(); });
        log("umount \"%s\" = %d", dev.c_str(), r);
        reply32(r);
        break;
    }
    case 0x53: {   // devctl
        const std::string dev = a.str(0x40);
        const uint32_t type = a.u32();
        a.u32();
        const uint32_t len = std::min(a.u32(), ANSWER_MAX - 4);
        std::vector<uint8_t> data(len);
        const int32_t r = path_call(dev, [&](Disk &d) { return d.devctl(type, data); });
        data.resize(len);
        std::vector<uint8_t> p(4);
        put32(p, 0, uint32_t(r));
        p.insert(p.end(), data.begin(), data.end());
        log("devctl \"%s\" %u = %d", dev.c_str(), type, r);
        reply(0, p);
        break;
    }
    case 0x50: {   // chdir
        const std::string path = a.str(0x100);
        const int32_t r = path_call(path, [&](Disk &d) { return d.chdir(path); });
        log("chdir \"%s\" = %d", path.c_str(), r);
        reply32(r);
        break;
    }
    case 0x41: {   // open
        const std::string path = a.str(0x100);
        const uint32_t flags = a.u32(), mode = a.u32();
        const int32_t r = path_call(path, [&](Disk &d) { return d.open(path, flags); });
        log("open \"%s\" %x %x = %d", path.c_str(), flags, mode, r);
        reply32(r);
        break;
    }
    case 0x42: {   // close
        const int32_t fd = int32_t(a.u32());
        const int32_t r = fd_call(fd, [&](Disk &d) { return d.close(fd); });
        log("close %d = %d", fd, r);
        reply32(r);
        break;
    }
    case 0x43: {   // read
        const int32_t fd = int32_t(a.u32());
        const uint32_t len = a.u32();
        std::vector<uint8_t> data;
        const int32_t r = fd_call(fd, [&](Disk &d) { return d.read(fd, len, data); });
        std::vector<uint8_t> p(4);
        put32(p, 0, uint32_t(r));
        if (r > 0) p.insert(p.end(), data.begin(), data.end());
        log("read %d %x = %d", fd, len, r);
        reply(0, p);
        break;
    }
    case 0x44: {   // write
        const int32_t fd = int32_t(a.u32());
        const uint32_t len = a.u32();
        const std::string data = a.bytes(len);
        const int32_t r = fd_call(fd, [&](Disk &d) { return d.write(fd, data); });
        log("write %d %x = %d", fd, len, r);
        reply32(r);
        break;
    }
    case 0x45: {   // lseek
        const int32_t fd = int32_t(a.u32());
        const uint32_t off = a.u32(), whence = a.u32();
        const int32_t r = fd_call(fd, [&](Disk &d) { return d.lseek(fd, off, whence); });
        log("lseek %d %x %u = %d", fd, off, whence, r);
        reply32(r);
        break;
    }
    case 0x4d: {   // getstat
        const std::string path = a.str(0x100);
        std::vector<uint8_t> st;
        const int32_t r = path_call(path, [&](Disk &d) { return d.getstat(path, st); });
        std::vector<uint8_t> p(4);
        put32(p, 0, uint32_t(r));
        if (r >= 0) p.insert(p.end(), st.begin(), st.end());
        log("getstat \"%s\" = %d", path.c_str(), r);
        reply(0, p);
        break;
    }
    case 0x47: {   // dopen
        const std::string path = a.str(0x100);
        const int32_t r = path_call(path, [&](Disk &d) { return d.dopen(path); });
        log("dopen \"%s\" = %d", path.c_str(), r);
        reply32(r);
        break;
    }
    case 0x48: {   // dclose
        const int32_t fd = int32_t(a.u32());
        const int32_t r = fd_call(fd, [&](Disk &d) { return d.close(fd); });
        log("dclose %d = %d", fd, r);
        reply32(r);
        break;
    }
    case 0x49: {   // dread: a record (0x144 bytes) and its size, or 0 at the end
        const int32_t fd = int32_t(a.u32());
        std::vector<uint8_t> rec;
        Disk *d = disk_for_fd(fd);
        const bool ok = d && d->dread(fd, rec);
        std::vector<uint8_t> p(4);
        put32(p, 0, ok ? uint32_t(rec.size()) : 0);
        if (ok) p.insert(p.end(), rec.begin(), rec.end());
        log("dread %d = %s", fd, ok ? (const char *)rec.data() + 0x40 : "(end)");
        reply(0, p);
        break;
    }
    case 0x4b: {   // mkdir
        const std::string path = a.str(0x100);
        a.u32();
        const int32_t r = path_call(path, [&](Disk &d) { return d.mkdir(path); });
        log("mkdir \"%s\" = %d", path.c_str(), r);
        reply32(r);
        break;
    }
    case 0x4c: {   // rmdir
        const std::string path = a.str(0x100);
        const int32_t r = path_call(path, [&](Disk &d) { return d.rmdir(path); });
        log("rmdir \"%s\" = %d", path.c_str(), r);
        reply32(r);
        break;
    }
    case 0x4a: {   // remove
        const std::string path = a.str(0x100);
        const int32_t r = path_call(path, [&](Disk &d) { return d.remove(path); });
        log("remove \"%s\" = %d", path.c_str(), r);
        reply32(r);
        break;
    }
    default:
        fprintf(stderr, "npu: call %02x not handled\n", op);
        if (op >= 0x40) reply32(FAIL);
        else reply(0, {});
        break;
    }
#undef log
}

// The sockets (windyfairy's formats). An address comes as the game's sockaddr_in: family, then
// the port and the address in network byte order, two and four bytes as the words carry them.
void k573npu_hle_device::socket_call(int op, Args &a) {
    auto ip_of = [](uint32_t raw) { return __builtin_bswap32(raw); };
    auto port_of = [](uint16_t raw) { return uint16_t(raw >> 8 | raw << 8); };
    auto payload = [&](uint32_t n) {
        std::string d = a.bytes(((size_t)n + 1) & ~(size_t)1);   // two bytes a word
        if (d.size() > n) d.resize(n);
        return d;
    };
    if (!m_lan) {   // no network: every call fails, as with the cable out
        std::vector<uint8_t> p(op == 0x23 || op == 0x2b || op == 0x29 ? 8 : 4);
        put32(p, 0, uint32_t(-1));
        if (op == 0x29) put32(p, 4, 16);
        log_call("socket call %02x: no network", op);
        reply(0, p);
        return;
    }
    switch (op) {
    case 0x2e: {   // socket(domain, type, protocol): 1/0 or 1/1 TCP, 2/3 UDP, 3/2 ICMP
        a.u32();
        const uint32_t type = a.u32(), protocol = a.u32();
        const int fd = type == 1 ? m_lan->socket(hk::ea::Lan::STREAM)
                     : type == 2 ? m_lan->socket(hk::ea::Lan::DATAGRAM)
                     : type == 3 ? m_lan->socket(hk::ea::Lan::RAW) : -1;
        log_call("socket(%u, %u) = %d", type, protocol, fd);
        reply32(fd);
        break;
    }
    case 0x22: {   // close(fd)
        const int fd = int(a.u32());
        reply32(m_lan->close(fd));
        break;
    }
    case 0x23: {   // connect(fd, length, sockaddr): the result and a word
        const int fd = int(a.u32());
        a.u32();
        a.u16();
        const uint16_t port = port_of(a.u16());
        const uint32_t ip = ip_of(a.u32());
        std::vector<uint8_t> p(8);
        put32(p, 0, uint32_t(m_lan->connect(fd, ip, port)));
        reply(0, p);
        break;
    }
    case 0x2a: {   // send(fd, length, flags, data)
        const int fd = int(a.u32());
        const uint32_t n = a.u32();
        a.u32();
        const std::string d = payload(n);
        reply32(m_lan->send(fd, reinterpret_cast<const uint8_t *>(d.data()), d.size()));
        break;
    }
    case 0x2b: {   // sendto(fd, length, flags, address length, sockaddr, data): the result and a word
        const int fd = int(a.u32());
        const uint32_t n = a.u32();
        a.u32();
        a.u32();
        a.u16();
        const uint16_t port = port_of(a.u16());
        const uint32_t ip = ip_of(a.u32());
        for (int i = 0; i < 4; i++) a.u16();   // sin_zero
        const std::string d = payload(n);
        std::vector<uint8_t> p(8);
        put32(p, 0, uint32_t(m_lan->sendto(fd, reinterpret_cast<const uint8_t *>(d.data()), d.size(), ip, port)));
        reply(0, p);
        break;
    }
    case 0x28: {   // recv(fd, length, flags): the count, then the data
        const int fd = int(a.u32());
        const uint32_t max = a.u32();
        if (!recv_reply(fd, max)) {
            m_waiting.push_back({m_packet_id, fd, max});
            if (m_waiting.size() == 1) m_poll_timer->adjust(attotime::from_msec(1), 0, attotime::from_msec(1));
        }
        break;
    }
    case 0x29: {   // recvfrom(fd, length, flags): the count, the address length, the address, the data
        const int fd = int(a.u32());
        const uint32_t max = a.u32();
        std::string d;
        uint32_t ip = 0;
        uint16_t port = 0;
        std::vector<uint8_t> p(8);
        put32(p, 4, 16);
        if (m_lan->recvfrom(fd, max, &d, &ip, &port) != hk::ea::Lan::DATA) {
            put32(p, 0, uint32_t(-1));   // nothing: as the unit's receive timeout
        } else {
            put32(p, 0, uint32_t(d.size()));
            put32(p, 8, 16);
            p.resize(28);
            p[12] = 2;   // AF_INET
            p[14] = uint8_t(port >> 8); p[15] = uint8_t(port);
            p[16] = uint8_t(ip >> 24); p[17] = uint8_t(ip >> 16); p[18] = uint8_t(ip >> 8); p[19] = uint8_t(ip);
            p.insert(p.end(), d.begin(), d.end());
            if (p.size() & 1) p.push_back(0);
        }
        reply(0, p);
        break;
    }
    case 0x2c: {   // setsockopt(fd, level, name, length, value): nothing to set
        const int fd = int(a.u32());
        const uint32_t level = a.u32(), name = a.u32();
        log_call("setsockopt(%d, %x, %x)", fd, level, name);
        reply32(0);
        break;
    }
    }
}

// A recv's answer, if there is one to give: the bytes there are, or 0 when the server has closed
bool k573npu_hle_device::recv_reply(int fd, uint32_t max) {
    std::string d;
    const hk::ea::Lan::Status st = m_lan->recv(fd, max, &d);
    if (st == hk::ea::Lan::WAIT) return false;
    std::vector<uint8_t> p(4);
    put32(p, 0, st == hk::ea::Lan::DATA ? uint32_t(d.size()) : st == hk::ea::Lan::END ? 0 : uint32_t(-1));
    p.insert(p.end(), d.begin(), d.end());
    if (p.size() & 1) p.push_back(0);
    reply(0, p);
    return true;
}

// Every millisecond while a recv waits: the answers that came in
void k573npu_hle_device::poll_network(s32) {
    if (!m_lan->poll()) return;
    const uint16_t id = m_packet_id;
    for (size_t i = 0; i < m_waiting.size();) {
        m_packet_id = m_waiting[i].id;
        if (recv_reply(m_waiting[i].fd, m_waiting[i].max)) m_waiting.erase(m_waiting.begin() + ptrdiff_t(i));
        else i++;
    }
    m_packet_id = id;
    if (m_waiting.empty()) m_poll_timer->adjust(attotime::never);
}

void k573npu_hle_device::log_call(const char *fmt, ...) {
    if (!m_log) return;
    fprintf(stderr, "npu: %.3f ", machine().time().as_double());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
