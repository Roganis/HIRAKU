// SPDX-License-Identifier: GPL-3.0-or-later
// HIRAKU: the MAME framework subset declared in emu.h.
#include "emu.h"
#include "core/prof.h"
#include "shell/sys.h"

#include <ctime>
#include <map>

bool device_t::s_log = getenv("HK_XT446_LOG") != nullptr;

// ---- formatting -----------------------------------------------------------------------
namespace hk_mame {

std::string format_args(const char *fmt, const std::vector<fmt_arg> &args) {
    std::string out;
    size_t ai = 0;
    char buf[512];
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { out += *p; continue; }
        if (p[1] == '%') { out += '%'; p++; continue; }
        // %[flags][width][.prec][length]conv
        std::string spec = "%";
        p++;
        while (*p && strchr("-+ #0", *p)) spec += *p++;
        while (*p && (isdigit((unsigned char)*p) || *p == '*')) spec += *p++;
        if (*p == '.') { spec += *p++; while (*p && isdigit((unsigned char)*p)) spec += *p++; }
        while (*p && strchr("hlLqjzt", *p)) p++;   // lengths are implied by the argument
        char conv = *p;
        if (!conv) break;
        if (ai >= args.size()) { out += "<?>"; continue; }
        const fmt_arg &a = args[ai++];
        switch (conv) {
        case 'd': case 'i':
            snprintf(buf, sizeof buf, (spec + "lld").c_str(), a.kind == fmt_arg::DBL ? (long long)a.d : a.i);
            break;
        case 'u': case 'x': case 'X': case 'o':
            snprintf(buf, sizeof buf, (spec + "ll" + conv).c_str(), a.kind == fmt_arg::DBL ? (unsigned long long)a.d : a.u);
            break;
        case 'c':
            snprintf(buf, sizeof buf, (spec + "c").c_str(), (int)a.i);
            break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
            snprintf(buf, sizeof buf, (spec + conv).c_str(), a.kind == fmt_arg::DBL ? a.d : (double)a.i);
            break;
        case 's':
            if (a.kind == fmt_arg::STR) snprintf(buf, sizeof buf, (spec + "s").c_str(), a.s.c_str());
            else snprintf(buf, sizeof buf, (spec + "lld").c_str(), a.i);
            break;
        case 'p':
            snprintf(buf, sizeof buf, "%p", a.p);
            break;
        default:
            snprintf(buf, sizeof buf, "<%%%c?>", conv);
            break;
        }
        out += buf;
    }
    return out;
}

}  // namespace hk_mame

void hk_fatal(const std::string &text) {
    fprintf(stderr, "mame shim: fatal: %s\n", text.c_str());
    abort();
}

bool hk_log_on() { return device_t::s_log; }
void hk_log_text(const std::string &text) {
    if (device_t::s_log) fprintf(stderr, "%s\n", text.c_str());
}

std::string attotime::as_string(int precision) const {
    if (is_never()) return "(never)";
    char buf[64];
    snprintf(buf, sizeof buf, "%.*f", precision, as_double());
    return buf;
}

// ---- devices ----------------------------------------------------------------------------
device_t::device_t(const machine_config &mconfig, device_type type, const char *tag, device_t *owner, u32 clock)
    : m_type(type), m_basetag(tag ? tag : ""), m_owner(owner), m_clock(clock), m_machine(mconfig.m_machine), m_mconfig(&mconfig) {
    m_tag = owner ? std::string(owner->tag()) + ":" + m_basetag : m_basetag;
    // DERIVED_CLOCK(num, den): the owner's clock times num / den
    if ((m_clock & 0xff000000u) == 0xff000000u && owner)
        m_clock = (u32)((u64)owner->clock() * ((m_clock >> 12) & 0xfff) / (m_clock & 0xfff));
}

void device_t::hk_log(const char *s) const {
    if (!s_log) return;
    fprintf(stderr, "[%s] %s %s", machine().time().as_string(6).c_str(), tag(), s);
}

void hk_register_device(machine_config &config, device_t *dev) {
    running_machine &m = *config.m_machine;
    m.m_devices.push_back(dev);
    if (dev->owner()) dev->owner()->m_children.push_back(dev);
    if (auto *cpu = dynamic_cast<cpu_device *>(dev)) m.m_cpus.push_back(cpu);
    device_t *saved = config.m_owner;
    config.m_owner = dev;
    dev->device_add_mconfig(config);
    config.m_owner = saved;
}

device_t *hk_owner_of(device_t *dev) { return dev->owner(); }

device_t *hk_find_device(device_t *owner, const char *tag) {
    if (!owner) return nullptr;
    if (tag[0] == '^') return hk_find_device(owner->owner() ? owner->owner() : owner, tag + 1);
    if (tag[0] == ':') { for (device_t *d : owner->machine().m_devices) if (std::string(tag) == d->tag()) return d; return nullptr; }
    std::string full = owner->subtag(tag);
    for (device_t *d : owner->machine().m_devices)
        if (full == d->tag()) return d;
    return nullptr;
}

void device_t::reset() {
    hk_pre_reset();
    for (device_interface *i : m_interfaces) i->interface_pre_reset();
    device_reset();
    for (device_t *c : m_children) c->reset();
    device_reset_after_children();
    for (device_interface *i : m_interfaces) i->interface_post_reset();
    hk_post_reset();
}

void device_t::start() {
    for (device_interface *i : m_interfaces) i->interface_pre_start();
    device_start();
    for (device_interface *i : m_interfaces) i->interface_post_start();
    hk_post_start();
    m_started = true;
}

// MAME's device_execute_interface: the periodic interrupt's timer (interface_pre_start), armed at
// reset (interface_post_reset); it calls back unless the CPU is halted, held in reset or disabled
void cpu_device::hk_post_start() {
    if (m_periodic_cb && !m_periodic_rate.is_zero())
        m_periodic_timer = machine().alloc_timer([this](s32) {
            if (!suspended(SUSPEND_REASON_HALT | SUSPEND_REASON_RESET | SUSPEND_REASON_DISABLE | SUSPEND_REASON_CLOCK)) m_periodic_cb();
        });
}

// MAME's interface_pre_reset: the cycle count starts again from 0 (the 68000 syncs to its E clock,
// a tenth of it, by that count: kept over a watchdog's reset, an autovectored interrupt came up to 9
// cycles off MAME's), and the CPU runs, unless disabled
void cpu_device::hk_pre_reset() {
    m_totalcycles = 0;
    if (suspended(SUSPEND_REASON_DISABLE)) return;
    if (clock() != 0) resume(~0u);
}

void cpu_device::hk_post_reset() {
    if (m_periodic_timer) m_periodic_timer->adjust(m_periodic_rate, 0, m_periodic_rate);
}

// MAME's read_interruptible / write_interruptible (emumem_hws.cpp): before_time defers the access
// to the cycle it gives (or redoes it in the next timeslice), after_delay adds its cycles after
u64 address_space::read_interruptible(offs_t a, u64 mask) {
    const u64 ba = unit_to_byte(a) & m_byte_mask & ~u64(m_bus_bytes - 1);
    const address_map_entry *w = nullptr;
    if (m_waits) [[unlikely]] {
        const page_t &p = m_pages[ba >> m_pbits];
        const u64 pm = (u64(1) << m_pbits) - 1;
        for (int b = 0; b < m_bus_bytes && !w; b++) {
            if (!(mask & (u64(0xff) << lane_shift(ba + b, 1)))) continue;
            const u16 id = p.ridx ? p.ridx[(ba + b) & pm] : p.did_r;
            if (id && (m_rt[id - 1].e->m_before_time || m_rt[id - 1].e->m_after_delay)) w = m_rt[id - 1].e;
        }
        if (w && w->m_before_time) {
            cpu_device &cpu = dynamic_cast<cpu_device &>(m_device);
            const u64 tc = cpu.total_cycles();
            u64 ac = w->m_before_time(a, tc);
            if (ac < tc) ac = tc;
            if (cpu.access_before_time(ac, tc)) return 0;
        }
    }
    const u64 v = read_masked(a, m_bus_bytes, mask);
    if (w && w->m_after_delay) dynamic_cast<cpu_device &>(m_device).access_after_delay(w->m_after_delay(a));
    return v;
}

void address_space::write_interruptible(offs_t a, u64 d, u64 mask) {
    // no write wait states are mapped anywhere here (MAME's autovectors are reads)
    write_masked(a, m_bus_bytes, d, mask);
}

device_t *hk_interface_device(device_interface &i) { return &i.device(); }

device_interface::device_interface(device_t &device, const char *type) : m_device(&device), m_type(type) {
    device.m_interfaces.push_back(this);
}

void device_slot_interface::hk_create_card(machine_config &config) {
    if (m_default.empty() || m_card) return;
    auto it = m_options.find(m_default);
    if (it == m_options.end()) fatalerror("%s: no slot option %s", device().tag(), m_default.c_str());
    device_t *saved = config.m_owner;
    config.m_owner = &device();
    m_card = it->second->create(config, m_default.c_str(), 0);
    config.m_owner = saved;
    auto cfg = m_option_cfg.find(m_default);
    if (cfg != m_option_cfg.end() && cfg->second) cfg->second(m_card);
}

void system_time::set(time_t t) {
    time = t;
    struct tm lt = hk::sys::local(t), ut = hk::sys::utc(t);
    local_time.set(lt);
    utc_time.set(ut);
}

void system_time::full_time::set(struct tm &t) {
    second = t.tm_sec;
    minute = t.tm_min;
    hour = t.tm_hour;
    mday = t.tm_mday;
    month = t.tm_mon;
    year = t.tm_year + 1900;
    weekday = t.tm_wday;
    day = t.tm_yday;
    is_dst = t.tm_isdst;
}

memory_region *hk_find_region(device_t &dev, const char *tag) {
    // by full tag, as MAME (DEVICE_SELF = the device's own region); then by base
    // name (the XT446 registers its regions under their short names)
    running_machine &m = dev.machine();
    std::string t = tag ? tag : "";
    memory_region *r = nullptr;
    if (t.empty()) r = m.find_region(dev.tag());
    else if (t[0] == ':') r = m.find_region(t);
    else r = m.find_region(dev.subtag(t.c_str()));
    if (!r && !t.empty()) r = m.find_region(t);
    return r;
}

void device_t::set_addrmap(int spacenum, address_map_constructor map) {
    m_external_maps.emplace_back(spacenum, map);
}

bool device_t::has_space(int spacenum) const {
    for (auto &[n, c] : memory_space_config())
        if (n == spacenum && c) return true;
    for (auto &[n, c] : m_hk_interface_spaces)
        if (n == spacenum && c) return true;
    return false;
}

address_space &device_t::make_space(int spacenum) {
    for (auto &s : m_spaces)
        if (s->m_spacenum == spacenum) return *s;
    const address_space_config *cfg = nullptr;
    for (auto &[n, c] : memory_space_config())
        if (n == spacenum) cfg = c;
    if (!cfg)
        for (auto &[n, c] : m_hk_interface_spaces)
            if (n == spacenum) cfg = c;
    if (!cfg) fatalerror("%s: no address space %d", tag(), spacenum);
    auto sp = std::make_unique<address_space>(*this, *cfg, spacenum);
    for (auto &[n, ctor] : m_external_maps) {
        if (n != spacenum) continue;
        auto map = std::make_unique<address_map>(ctor.m_owner ? *ctor.m_owner : *this);
        ctor(*map);
        sp->install(std::move(map));
    }
    if (cfg->m_internal_map) {
        auto map = std::make_unique<address_map>(*this);
        cfg->m_internal_map(*map);
        sp->install(std::move(map));
    }
    sp->finalize();
    m_spaces.push_back(std::move(sp));
    if (unsigned(spacenum) < std::size(m_space_by_num)) m_space_by_num[spacenum] = m_spaces.back().get();
    return *m_spaces.back();
}

// ---- address spaces ----------------------------------------------------------------------
address_space::address_space(device_t &dev, const address_space_config &cfg, int spacenum)
    : m_config(cfg), m_device(dev), m_spacenum(spacenum) {
    m_bus_bytes = cfg.m_data_width / 8;
    m_shift = cfg.m_addr_shift;
    m_big = cfg.m_endianness == ENDIANNESS_BIG;
    int bits = cfg.m_addr_width + (m_shift < 0 ? -m_shift : 0);
    m_byte_mask = bits >= 64 ? ~u64(0) : (u64(1) << bits) - 1;
    m_pbits = std::max(8, bits - 16);
    m_pages.resize(size_t(1) << std::max(0, bits - m_pbits));
}

// A submap installed with a unit mask (MAME's .m(...).umask16(0x00ff) on a wider bus): its
// offsets count the device's units, as many to a bus word as the mask has groups of lanes
// (umask16(0x00ff) on 32 bits: two bytes, in lanes 0 and 2). Each unit becomes an entry of its
// own, at its byte address, with the offset the device's handler expects.
void address_space::collect_units(address_map &sub, const address_map_entry &outer, offs_t base, offs_t mirror) {
    const int B = m_bus_bytes;
    auto lane_on = [&](int b) { return ((outer.m_umask >> (8 * (m_big ? B - 1 - b : b))) & 0xff) != 0; };
    std::vector<std::pair<int, int>> units;   // (byte offset in the bus word, width in bytes), address order
    for (int b = 0; b < B;) {
        if (!lane_on(b)) { b++; continue; }
        int w = 0;
        while (b + w < B && lane_on(b + w)) w++;
        units.emplace_back(b, w);
        b += w;
    }
    auto holder = std::make_unique<address_map>(sub.m_device ? *sub.m_device : m_device);
    for (auto &ie : sub.m_entries) {
        if (ie->m_rkind == address_map_entry::SUBMAP || ie->m_rkind == address_map_entry::MEM || ie->m_wkind == address_map_entry::MEM)
            fatalerror("%s: a masked submap with memory or a submap of its own", m_device.tag());
        if (ie->m_end - ie->m_start > 0xffff) fatalerror("%s: a masked submap entry too large", m_device.tag());
        for (u64 x = ie->m_start; x <= ie->m_end; x++) {
            auto c = std::unique_ptr<address_map_entry>(new address_map_entry(*ie));
            const auto [off, w] = units[x % units.size()];
            c->m_start = (offs_t)(base + outer.m_start + (x / units.size()) * B + off);
            c->m_end = c->m_start + w - 1;
            c->m_mirror = ie->m_mirror | mirror | outer.m_mirror;
            c->m_umask = ~u64(0);
            const offs_t delta = (offs_t)(x - ie->m_start);
            if (c->m_rh) c->m_rh = [r = ie->m_rh, delta](offs_t o, u64 m) { return r(o + delta, m); };
            if (c->m_wh) c->m_wh = [wh = ie->m_wh, delta](offs_t o, u64 d, u64 m) { wh(o + delta, d, m); };
            m_all.push_back(c.get());
            holder->m_entries.push_back(std::move(c));
        }
    }
    m_maps.push_back(std::move(holder));
}

void address_space::collect(address_map &map, offs_t base, offs_t mirror) {
    for (auto &e : map.m_entries) {
        if (e->m_rkind == address_map_entry::SUBMAP) {
            auto sub = std::make_unique<address_map>(*e->m_subdev);
            e->m_submap(*sub);
            const u64 busmask = m_bus_bytes == 8 ? ~u64(0) : (u64(1) << (m_bus_bytes * 8)) - 1;
            if ((e->m_umask & busmask) != busmask) collect_units(*sub, *e, base, mirror);
            else collect(*sub, base + e->m_start, mirror | e->m_mirror);
            m_maps.push_back(std::move(sub));
            continue;
        }
        // map addresses are in the space's units (bus words when the shift is negative): to bytes
        if (m_shift < 0) {
            int sh = -m_shift;
            e->m_start <<= sh;
            e->m_end = ((e->m_end + 1) << sh) - 1;
            e->m_mirror <<= sh;
            e->m_select <<= sh;
        }
        e->m_start += base;
        e->m_end += base;
        e->m_mirror |= mirror;
        m_all.push_back(e.get());
    }
}

void address_space::install(std::unique_ptr<address_map> map) {
    if (map->m_global_mask != ~offs_t(0)) m_byte_mask &= (u64)map->m_global_mask;
    collect(*map, 0, 0);
    m_maps.push_back(std::move(map));   // the entries live as long as the space
}

void address_space::finalize() {
    for (address_map_entry *e : m_all) apply(*e);
}

address_map_entry &address_space::runtime_entry(offs_t start, offs_t end) {
    if (!m_runtime) m_runtime = std::make_unique<address_map>(m_device);
    address_map_entry &e = (*m_runtime)(start, end);
    m_all.push_back(&e);
    return e;
}

void address_space::install_mem(offs_t start, offs_t end, u8 *base, bool rom) {
    address_map_entry &e = runtime_entry(start, end);
    e.m_rkind = address_map_entry::MEM;
    e.m_wkind = rom ? address_map_entry::NOP : address_map_entry::MEM;
    e.m_rom = rom;
    e.m_extmem = base;
    apply(e);
}

u8 *address_space::mem_ptr(offs_t a, bool write) const {
    const u64 ba = a & m_byte_mask;
    const page_t &p = m_pages[ba >> m_pbits];
    if (u8 *d = write ? p.direct_w : p.direct_r) return d + (ba & ((u64(1) << m_pbits) - 1));
    const u16 id = write ? (p.widx ? p.widx[ba & ((u64(1) << m_pbits) - 1)] : p.did_w)
                         : (p.ridx ? p.ridx[ba & ((u64(1) << m_pbits) - 1)] : p.did_r);
    if (!id) return nullptr;
    const entry_rt &r = m_rt[id - 1];
    if ((write ? r.e->m_wkind : r.e->m_rkind) != address_map_entry::MEM) return nullptr;
    return r.mem + ((ba & ~(u64)(r.e->m_mirror | r.e->m_select)) - r.e->m_start);
}

void address_space::unmap_readwrite(offs_t start, offs_t end) {
    // an entry that claims the range and does nothing (reads 0)
    address_map_entry &e = runtime_entry(start, end);
    e.m_rkind = e.m_wkind = address_map_entry::NOP;
    apply(e);
}

void address_space::apply(address_map_entry &er) {
    m_version++;
    address_map_entry *e = &er;
    const u64 psize = u64(1) << m_pbits;
    // backing memory
    u8 *mem = nullptr;
    if (e->m_rkind == address_map_entry::MEM || e->m_wkind == address_map_entry::MEM) {
        u64 len = (u64)e->m_end - e->m_start + 1;
        if (e->m_extmem) {
            mem = e->m_extmem;
        } else if (e->m_has_region) {
            // relative to the device whose function made the map, as MAME (then by full or short name)
            memory_region *r = hk_find_region(*e->m_map.m_device, e->m_region.c_str());
            if (!r) fatalerror("%s: region %s missing", m_device.tag(), e->m_region.c_str());
            if (e->m_region_offset + len > r->bytes()) fatalerror("%s: region %s too small (%x-%x + %x, %x bytes)", m_device.tag(), e->m_region.c_str(), e->m_start, e->m_end, e->m_region_offset, r->bytes());
            mem = r->base() + e->m_region_offset;
        } else if (memory_region *own = e->m_rom ? m_device.machine().find_region(m_device.tag()) : nullptr) {
            // MAME: a ROM entry without a region reads the device's own region, at its address
            const u64 off = unit_to_byte(e->m_start);
            if (off + len > own->bytes()) fatalerror("%s: region %s too small for %x-%x", m_device.tag(), m_device.tag(), e->m_start, e->m_end);
            mem = own->base() + off;
        } else {
            m_owned.push_back(std::make_unique<std::vector<u8>>(len + 8, 0));
            mem = m_owned.back()->data();
        }
    }
    if (m_rt.size() >= 0xffff) fatalerror("%s: too many address map entries", m_device.tag());
    m_rt.push_back({e, mem, e->m_start});
    if (e->m_before_time || e->m_after_delay) m_waits = true;
    if (!e->m_share.empty() && mem) hk_register_share(e->m_map.m_device->subtag(e->m_share.c_str()), mem, (size_t)((u64)e->m_end - e->m_start + 1));
    const u16 id = (u16)m_rt.size();
    const offs_t extra = e->m_mirror | e->m_select;
    // every combination of the mirror and select bits
    offs_t sub = 0;
    do {
        u64 lo = (u64)(e->m_start | sub) & m_byte_mask, hi = (u64)(e->m_end | sub) & m_byte_mask;
        for (int side = 0; side < 2; side++) {
            address_map_entry::kind_t k = side ? e->m_wkind : e->m_rkind;
            if (k == address_map_entry::NONE) continue;
            u64 a = lo;
            while (a <= hi) {
                page_t &p = m_pages[a >> m_pbits];
                u64 pbase = a & ~(psize - 1);
                bool whole = a == pbase && hi >= pbase + psize - 1;
                if (whole && k == address_map_entry::MEM && !(extra & (psize - 1)) && e->m_umask == ~u64(0)) {
                    u8 *host = mem + ((pbase & ~(u64)extra) - e->m_start);
                    (side ? p.direct_w : p.direct_r) = host;
                    (side ? p.did_w : p.did_r) = id;
                    (side ? p.widx : p.ridx).reset();
                    a += psize;
                    if (a == 0) break;   // wrapped at the top of a 64-bit-wide walk
                    continue;
                }
                auto &idx = side ? p.widx : p.ridx;
                u8 *&direct = side ? p.direct_w : p.direct_r;
                u16 &did = side ? p.did_w : p.did_r;
                if (whole && e->m_umask == ~u64(0)) {
                    // a whole page of one non-memory entry: the page's id, no per-byte table (MAME's
                    // PS1 unmaps ~1.5 GB above its RAM: per-byte tables for that took over 12 GB)
                    idx.reset();
                    direct = nullptr;
                    did = id;
                    a += psize;
                    if (a == 0) break;
                    continue;
                }
                if (!idx) {
                    idx.reset(new u16[psize]());
                    if (did) std::fill_n(idx.get(), psize, did);   // the page was one entry's: per byte now
                    direct = nullptr;
                    did = 0;
                }
                u64 end = std::min(hi, pbase + psize - 1);
                for (; a <= end; a++) {
                    if (e->m_umask != ~u64(0)) {
                        u64 lm = u64(0xff) << lane_shift(a, 1);
                        if (!(e->m_umask & lm)) continue;
                    }
                    idx[a - pbase] = id;
                }
                if (end == m_byte_mask) break;
            }
        }
        sub = (sub - extra) & extra;
    } while (sub);
}

// one bus word at bus_ba (aligned), the lanes in mask
u64 address_space::read_bus(u64 bus_ba, u64 mask) {
    page_t &p = m_pages[bus_ba >> m_pbits];
    const u64 psize = u64(1) << m_pbits;
    u64 result = 0;
    // distinct entries among the requested lanes
    u16 ids[8];
    u64 masks[8];
    int n = 0;
    for (int b = 0; b < m_bus_bytes; b++) {
        u64 lm = u64(0xff) << lane_shift(bus_ba + b, 1);
        if (!(mask & lm)) continue;
        u16 id = p.ridx ? p.ridx[(bus_ba + b) & (psize - 1)] : p.did_r;
        int j = 0;
        while (j < n && ids[j] != id) j++;
        if (j == n) { ids[n] = id; masks[n] = 0; n++; }
        masks[j] |= lm;
    }
    for (int j = 0; j < n; j++) {
        if (!ids[j]) continue;
        const entry_rt &r = m_rt[ids[j] - 1];
        const address_map_entry *e = r.e;
        const u64 em = masks[j];
        if (e->m_rkind == address_map_entry::MEM) {
            const u8 *w = r.mem + ((bus_ba & ~(u64)(e->m_mirror | e->m_select)) - e->m_start);
            u64 v;
            switch (m_bus_bytes) {
            case 1: v = *w; break;
            case 2: v = *(const u16 *)w; break;
            case 4: v = *(const u32 *)w; break;
            default: v = *(const u64 *)w; break;
            }
            result |= v & em;
        } else if (e->m_rkind == address_map_entry::HANDLER) {
            u64 offb = (bus_ba & ~(u64)e->m_mirror) - e->m_start;
            int hw = e->m_rwidth / 8;
            if (hw >= m_bus_bytes) {
                result |= e->m_rh((offs_t)(offb / m_bus_bytes), em & e->m_umask) & em;
            } else {
                u64 smask = (u64(1) << e->m_rwidth) - 1;
                int units = 0;
                // a unit with any umask bits is the handler's (MAME's units): umask16(0xff) on a
                // 32-bit bus is two 16-bit units, each called with mem_mask 0x00ff
                for (int k = 0; k < m_bus_bytes / hw; k++) {
                    u64 lm = smask << lane_shift(bus_ba + k * hw, hw);
                    if (e->m_umask & lm) units++;
                }
                int u = 0;
                for (int k = 0; k < m_bus_bytes / hw; k++) {
                    u64 sh = lane_shift(bus_ba + k * hw, hw);
                    u64 lm = smask << sh;
                    if (!(e->m_umask & lm)) continue;
                    if (em & lm & e->m_umask) result |= (e->m_rh((offs_t)((offb / m_bus_bytes) * units + u), (e->m_umask >> sh) & smask) & smask) << sh;
                    u++;
                }
            }
        }
    }
    return result;
}

void address_space::write_bus(u64 bus_ba, u64 data, u64 mask) {
    page_t &p = m_pages[bus_ba >> m_pbits];
    const u64 psize = u64(1) << m_pbits;
    if (p.direct_w) {
        u8 *w = p.direct_w + (bus_ba & (psize - 1));
        switch (m_bus_bytes) {
        case 1: *w = (u8)((*w & ~mask) | (data & mask)); break;
        case 2: *(u16 *)w = (u16)((*(u16 *)w & ~mask) | (data & mask)); break;
        case 4: *(u32 *)w = (u32)((*(u32 *)w & ~mask) | (data & mask)); break;
        default: *(u64 *)w = (*(u64 *)w & ~mask) | (data & mask); break;
        }
        return;
    }
    if (!p.widx && !p.did_w) return;
    u16 ids[8];
    u64 masks[8];
    int n = 0;
    for (int b = 0; b < m_bus_bytes; b++) {
        u64 lm = u64(0xff) << lane_shift(bus_ba + b, 1);
        if (!(mask & lm)) continue;
        u16 id = p.widx ? p.widx[(bus_ba + b) & (psize - 1)] : p.did_w;
        int j = 0;
        while (j < n && ids[j] != id) j++;
        if (j == n) { ids[n] = id; masks[n] = 0; n++; }
        masks[j] |= lm;
    }
    for (int j = 0; j < n; j++) {
        if (!ids[j]) continue;
        const entry_rt &r = m_rt[ids[j] - 1];
        const address_map_entry *e = r.e;
        const u64 em = masks[j];
        if (e->m_wkind == address_map_entry::MEM) {
            u8 *w = r.mem + ((bus_ba & ~(u64)(e->m_mirror | e->m_select)) - e->m_start);
            switch (m_bus_bytes) {
            case 1: *w = (u8)((*w & ~em) | (data & em)); break;
            case 2: *(u16 *)w = (u16)((*(u16 *)w & ~em) | (data & em)); break;
            case 4: *(u32 *)w = (u32)((*(u32 *)w & ~em) | (data & em)); break;
            default: *(u64 *)w = (*(u64 *)w & ~em) | (data & em); break;
            }
        } else if (e->m_wkind == address_map_entry::HANDLER) {
            u64 offb = (bus_ba & ~(u64)e->m_mirror) - e->m_start;
            int hw = e->m_wwidth / 8;
            if (hw >= m_bus_bytes) {
                e->m_wh((offs_t)(offb / m_bus_bytes), data & em, em & e->m_umask);
            } else {
                u64 smask = (u64(1) << e->m_wwidth) - 1;
                int units = 0;
                for (int k = 0; k < m_bus_bytes / hw; k++) {
                    u64 lm = smask << lane_shift(bus_ba + k * hw, hw);
                    if (e->m_umask & lm) units++;
                }
                int u = 0;
                for (int k = 0; k < m_bus_bytes / hw; k++) {
                    u64 sh = lane_shift(bus_ba + k * hw, hw);
                    u64 lm = smask << sh;
                    if (!(e->m_umask & lm)) continue;
                    if (em & lm & e->m_umask) e->m_wh((offs_t)((offb / m_bus_bytes) * units + u), (data >> sh) & smask, (e->m_umask >> sh) & smask);
                    u++;
                }
            }
        }
    }
}

// The common slow access, done directly: a little-endian space and one handler using all its
// lanes, covering the bytes accessed. Each handler call (offset, data, mask) and its order are
// the ones read_bus/write_bus make after working them out lane by lane:
// - a handler as wide as the bus gets the accessed lanes as its mask;
// - a narrower one is called per unit the access touches, with the unit's whole mask (a byte read
//   from a 16-bit handler reads the whole 16 bits).
const address_map_entry *address_space::one_handler(u64 ba, int size, bool write) const {
    if (m_big || size > m_bus_bytes || (ba & (size - 1))) return nullptr;
    const page_t &p = m_pages[ba >> m_pbits];
    const u64 pm = (u64(1) << m_pbits) - 1;
    const u16 *idx = write ? p.widx.get() : p.ridx.get();
    const u16 id = idx ? idx[ba & pm] : write ? p.did_w : p.did_r;
    if (!id || (idx && idx[(ba + size - 1) & pm] != id)) return nullptr;
    const address_map_entry *e = m_rt[id - 1].e;
    if ((write ? e->m_wkind : e->m_rkind) != address_map_entry::HANDLER) return nullptr;
    const u64 busmask = m_bus_bytes == 8 ? ~u64(0) : (u64(1) << (m_bus_bytes * 8)) - 1;
    if ((e->m_umask & busmask) != busmask || ((e->m_start | e->m_mirror) & (m_bus_bytes - 1))) return nullptr;
    const int hw = (write ? e->m_wwidth : e->m_rwidth) / 8;
    if (hw > m_bus_bytes || (hw < size && size % hw)) return nullptr;
    return e;
}

u64 address_space::read_slow(u64 ba, int size) {
    if (!m_read_tap) {
        if (const address_map_entry *e = one_handler(ba, size, false)) {
            const u64 smask = size == 8 ? ~u64(0) : (u64(1) << (size * 8)) - 1;
            const u64 off = (ba & ~(u64)e->m_mirror) - e->m_start;
            const int hw = e->m_rwidth / 8;
            if (hw == m_bus_bytes) {
                const u64 sh = lane_shift(ba, size);
                return (e->m_rh((offs_t)(off / hw), smask << sh) >> sh) & smask;
            }
            const u64 hmask = (u64(1) << (hw * 8)) - 1;
            if (size <= hw)
                return ((e->m_rh((offs_t)(off / hw), hmask) & hmask) >> ((off % hw) * 8)) & smask;
            u64 v = 0;
            for (int k = 0; k < size / hw; k++) v |= (e->m_rh((offs_t)(off / hw + k), hmask) & hmask) << (k * hw * 8);
            return v;
        }
    }
    u64 bus = ba & ~u64(m_bus_bytes - 1);
    u64 sh = lane_shift(ba, size);
    u64 smask = size == 8 ? ~u64(0) : (u64(1) << (size * 8)) - 1;
    u64 v = read_bus(bus, smask << sh);
    if (m_read_tap) m_read_tap((offs_t)bus, v & (smask << sh), smask << sh);
    return (v >> sh) & smask;
}

void address_space::write_slow(u64 ba, int size, u64 data) {
    u64 bus = ba & ~u64(m_bus_bytes - 1);
    u64 sh = lane_shift(ba, size);
    u64 smask = size == 8 ? ~u64(0) : (u64(1) << (size * 8)) - 1;
    if (m_write_tap) m_write_tap((offs_t)bus, (data & smask) << sh, smask << sh);
    write_bus(bus, (data & smask) << sh, smask << sh);
}

// ---- timers ------------------------------------------------------------------------------
void emu_timer::adjust(attotime duration, s32 param, attotime period) {
    running_machine &m = *m_machine;
    if (!m.m_exact) {
        if (duration < attotime::zero) duration = attotime::zero;
        m_expire = m.time() + duration;
        m_param = param;
        m_period = period;
        m_enabled = true;
        return;
    }
    // MAME's emu_timer::adjust
    if (m.m_callback_timer == this) m.m_callback_timer_modified = true;
    m_param = param;
    m_enabled = true;
    if (duration.seconds() < 0) duration = attotime::zero;
    m_start = m.time();
    m_expire = m_start + duration;
    m_period = period;
    m.timer_reinsert(this);
    if (!m.m_active.empty() && m.m_active.front() == this) m.abort_timeslice();
}

bool emu_timer::enable(bool on) {
    const bool old = m_enabled;
    if (old != on) {
        m_enabled = on;
        if (m_machine->m_exact) m_machine->timer_reinsert(this);
    }
    return old;
}

attotime emu_timer::remaining() const {
    attotime now = m_machine->time();
    if (!m_enabled || m_expire.is_never()) return attotime::never;
    return m_expire <= now ? attotime::zero : m_expire - now;
}

attotime emu_timer::elapsed() const { return m_machine->time() - m_start; }

// ---- CPUs and the machine ------------------------------------------------------------------
attotime cpu_device::cycles_to_attotime(u64 cycles) const {
    if (machine().m_exact) return clocks_to_attotime(execute_cycles_to_clocks(cycles));
    return attotime::raw((attotime::i128)cycles * attoseconds_per_cycle());
}

attotime cpu_device::local_time() const {
    if (!m_executing) return m_localtime;
    return m_localtime + cycles_to_attotime((u64)(m_cycles_running - *m_icountptr));
}

void cpu_device::abort_timeslice() {
    // MAME's device_execute_interface::abort_timeslice: swallow the remaining cycles
    if (!m_executing || !machine().m_exact) return;
    if (m_icountptr && *m_icountptr > 0) {
        m_cycles_stolen += *m_icountptr;
        m_cycles_running -= *m_icountptr;
        *m_icountptr = 0;
    }
}

void cpu_device::set_input_line(int line, int state) {
    if (!machine().m_exact) { execute_set_input(line, state); return; }
    // MAME's device_input::set_state_synced: queue; the first event schedules the flush
    std::vector<int> *q = nullptr;
    for (auto &[l, v] : m_input_queue)
        if (l == line) q = &v;
    if (!q) { m_input_queue.emplace_back(line, std::vector<int>()); q = &m_input_queue.back().second; }
    q->push_back(state);
    if (q->size() == 1) {
        machine().synchronize([this, line](s32) {
            for (auto &[l, v] : m_input_queue) {
                if (l != line) continue;
                std::vector<int> events;
                events.swap(v);
                for (int st : events) execute_set_input(line, st == CLEAR_LINE ? CLEAR_LINE : ASSERT_LINE);
            }
        });
    }
}

void hk_set_input_line(device_t *dev, int line, int state) {
    auto *cpu = dynamic_cast<cpu_device *>(dev);
    if (!cpu) fatalerror("set_inputline: %s is not a CPU", dev ? dev->tag() : "(null)");
    cpu->set_input_line(line, state);
}

device_t *hk_config_current(device_t *owner) {
    device_t *cur = owner ? owner->mconfig().m_owner : nullptr;
    return cur ? cur : owner;
}

running_machine::running_machine() { m_config.m_machine = this; }

running_machine::~running_machine() {
    // MAME's stop_all_devices: every device stopped before any is freed (the PS1 GPU's drawing
    // thread writes into a RAM device created after it)
    for (device_t *d : m_devices) d->device_stop();
    for (auto it = m_devices.rbegin(); it != m_devices.rend(); ++it) delete *it;
}

memory_region *running_machine::add_region(const std::string &tag, std::vector<u8> data) {
    auto r = std::make_unique<memory_region>();
    r->m_name = tag;
    r->m_data = std::move(data);
    m_regions.push_back(std::move(r));
    return m_regions.back().get();
}

memory_region *running_machine::find_region(const std::string &tag) {
    for (auto &r : m_regions)
        if (r->m_name == tag) return r.get();
    return nullptr;
}

emu_timer *running_machine::alloc_timer(std::function<void(s32)> cb) {
    auto t = std::make_unique<emu_timer>();
    t->m_machine = this;
    t->m_cb = std::move(cb);
    m_timers.push_back(std::move(t));
    return m_timers.back().get();
}

attotime running_machine::time() const {
    if (m_callback_timer) return m_callback_timer_expire;
    if (m_firing) return m_fire_time;
    if (m_executing) return m_executing->local_time();
    return m_basetime;
}

void running_machine::start() {
    if (m_exact) {
        // MAME's sound_manager: every stream brought up to date 50 times a second
        emu_timer *t = alloc_timer([this](s32) { for (sound_stream *st : m_streams) st->update(); });
        t->adjust(attotime::from_hz(50), 0, attotime::from_hz(50));
    }
    // slot cards, then the device list in tree order again (MAME starts devices in tree order)
    for (size_t k = 0; k < m_devices.size(); k++)
        for (device_interface *i : m_devices[k]->m_interfaces)
            if (auto *slot = dynamic_cast<device_slot_interface *>(i)) slot->hk_create_card(m_config);
    {
        std::vector<device_t *> order;
        std::function<void(device_t *)> walk = [&](device_t *d) { order.push_back(d); for (device_t *c : d->m_children) walk(c); };
        for (device_t *d : m_devices) if (!d->owner()) walk(d);
        m_devices = order;
    }
    m_phase = machine_phase::INIT;
    for (device_t *d : m_devices) { d->device_config_complete(); for (device_interface *i : d->m_interfaces) i->interface_config_complete(); }
    for (device_t *d : m_devices) d->device_resolve_objects();
    for (device_t *d : m_devices) d->start();
    if (m_exact) {
        // MAME: the NVRAM (a file, or the device's defaults), then the time on battery-backed RTCs
        nvram_load();
        if (m_after_nvram_load) m_after_nvram_load();   // MAME: set_rtc_datetime (the board sets its RTCs)
    }
    m_phase = machine_phase::RUNNING;
    if (m_exact) {
        // MAME's update_divisor, and the quantum: 1/60 s, or the perfect interleave
        // (the second-smallest CPU cycle; about a second with one CPU) if longer
        attoseconds_t smallest = 0, perfect = ATTOSECONDS_PER_SECOND - 1;
        bool first = true;
        for (cpu_device *c : m_cpus) {
            s64 attos = c->attoseconds_per_cycle();
            c->m_divshift = 0;
            while (attos >= (s64(1) << 31)) { c->m_divshift++; attos >>= 1; }
            c->m_divisor = (u32)attos;
            attoseconds_t q = c->attoseconds_per_cycle() * c->execute_min_cycles();
            if (first) { smallest = q; first = false; }
            else if (q < smallest) { perfect = smallest; smallest = q; }
            else if (q < perfect) perfect = q;
        }
        m_quantum = std::max(HZ_TO_ATTOSECONDS(60), perfect);
    }
}

void running_machine::reset() {
    for (device_t *d : m_devices)
        if (!d->owner()) d->reset();
}

void running_machine::run_until(attotime target) {
    while (m_basetime < target) {
        attotime next = target;
        for (auto &t : m_timers)
            if (t->m_enabled && t->m_expire < next) next = t->m_expire;
        if (next < m_basetime) next = m_basetime;

        for (cpu_device *cpu : m_cpus) {
            if (cpu->m_suspend) {
                if (cpu->m_localtime < next) cpu->m_localtime = next;
                continue;
            }
            if (!(next > cpu->m_localtime)) continue;
            u64 cycles = cpu->attotime_to_cycles(next - cpu->m_localtime);
            if (!cycles) continue;
            if (cycles > 0x3fffffff) cycles = 0x3fffffff;
            cpu->m_cycles_running = (int)cycles;
            *cpu->m_icountptr = (int)cycles;
            cpu->m_executing = true;
            m_executing = cpu;
            hk::prof::Scope ps(cpu->hk_main() ? hk::prof::CPU : hk::prof::SPU68K);   // the frame's time by part
            cpu->execute_run();
            m_executing = nullptr;
            cpu->m_executing = false;
            u64 ran = (u64)(cpu->m_cycles_running - *cpu->m_icountptr);
            cpu->m_totalcycles += ran;
            cpu->m_localtime += cpu->cycles_to_attotime(ran);
        }
        m_basetime = next;

        // timers due, in time order
        for (;;) {
            emu_timer *due = nullptr;
            for (auto &t : m_timers)
                if (t->m_enabled && t->m_expire <= m_basetime && (!due || t->m_expire < due->m_expire)) due = t.get();
            if (!due) break;
            m_fire_time = due->m_expire;
            if (due->m_period.is_never() || due->m_period.is_zero()) due->m_enabled = false;
            else due->m_expire += due->m_period;
            m_firing = due;
            hk::prof::Scope ps(hk::prof::DEVICES);
            due->m_cb(due->m_param);
            m_firing = nullptr;
        }
    }
}

// ---- the exact scheduler (MAME's device_scheduler) ----------------------------------------------
std::string running_machine::describe_context() const {
    if (m_executing) return util::string_format("'%s' (%08x)", m_executing->tag(), m_executing->pc());
    return "(no context)";
}

void running_machine::timer_reinsert(emu_timer *t) {
    auto it = std::find(m_active.begin(), m_active.end(), t);
    if (it != m_active.end()) m_active.erase(it);
    if (t->m_expire.is_never() || !t->m_enabled) return;   // MAME keeps these on its inactive list
    // before the first timer that expires later: after those with the same time
    auto pos = m_active.begin();
    while (pos != m_active.end() && !((*pos)->m_expire > t->m_expire)) ++pos;
    m_active.insert(pos, t);
}

emu_timer *running_machine::synchronize(std::function<void(s32)> cb, s32 param) {
    emu_timer *t = alloc_timer(std::move(cb));
    t->m_temporary = true;
    t->m_param = param;
    t->m_period = attotime::never;
    t->m_start = time();
    t->m_expire = t->m_start;
    t->m_enabled = true;
    timer_reinsert(t);
    if (m_active.front() == t) abort_timeslice();
    return t;
}

void running_machine::abort_timeslice() {
    if (m_executing) m_executing->abort_timeslice();
}

void running_machine::timeslice() {
    auto head = [this] { return m_active.empty() ? attotime::never : m_active.front()->m_expire; };
    while (m_basetime < head()) {
        attotime target = m_basetime + attotime::raw(m_quantum);
        if (head() < target) target = head();
        for (cpu_device *exec : m_cpus) {
            if (exec->m_suspend != 0 || target.seconds() < exec->m_localtime.seconds()) continue;
            const attoseconds_t apc = exec->attoseconds_per_cycle();
            attoseconds_t delta = (attoseconds_t)(target - exec->m_localtime).raw_value();
            if (delta < apc) continue;
            int ran = exec->m_cycles_running = (int)((u64(delta) >> exec->m_divshift) / exec->m_divisor);
            exec->m_cycles_stolen = 0;
            m_executing = exec;
            exec->m_executing = true;
            *exec->m_icountptr = exec->m_cycles_running;
            hk::prof::Scope ps(exec->hk_main() ? hk::prof::CPU : hk::prof::SPU68K);   // the frame's time by part
            exec->execute_run();
            exec->m_executing = false;
            ran -= *exec->m_icountptr;
            ran -= exec->m_cycles_stolen;
            exec->m_totalcycles += ran;
            const u64 cps = exec->cycles_per_second();
            if ((u64)ran < cps) exec->m_localtime += attotime::raw((attotime::i128)apc * ran);
            else exec->m_localtime += attotime((s64)(ran / cps), (attoseconds_t)(ran % cps) * apc);
            if (exec->m_localtime < target) target = std::max(exec->m_localtime, m_basetime);
        }
        m_executing = nullptr;
        m_basetime = target;
    }
    execute_timers();
}

// MAME's schedule_soft_reset: a zero-time timer that resets every device (running_machine::
// soft_reset), and eat_all_cycles(): an executing CPU's remaining cycles count as run, so its
// timeslice still ends where it was to. Only a watchdog asks for it here, from its timer, when no
// CPU is executing.
void running_machine::schedule_soft_reset() {
    fprintf(stderr, "hiraku: soft reset (watchdog) at %.6f s\n", time().as_double());
    synchronize([this](s32) { reset(); });
    if (m_executing) m_executing->eat_cycles(1000000000);
}

void running_machine::execute_timers() {
    while (!m_active.empty() && m_active.front()->m_expire <= m_basetime) {
        emu_timer &t = *m_active.front();
        const bool was_enabled = t.m_enabled;
        if (t.m_period.is_zero() || t.m_period.is_never()) t.m_enabled = false;
        m_callback_timer_modified = false;
        m_callback_timer = &t;
        m_callback_timer_expire = t.m_expire;
        if (was_enabled && t.m_cb) {
            hk::prof::Scope ps(hk::prof::DEVICES);
            t.m_cb(t.m_param);
        }
        if (!m_callback_timer_modified) {
            if (!t.m_temporary) {
                t.m_start = t.m_expire;
                t.m_expire += t.m_period;
                timer_reinsert(&t);
            } else {
                auto it = std::find(m_active.begin(), m_active.end(), &t);
                if (it != m_active.end()) m_active.erase(it);
                for (auto i = m_timers.begin(); i != m_timers.end(); ++i)
                    if (i->get() == &t) { m_timers.erase(i); break; }
            }
        }
    }
    m_callback_timer = nullptr;
}

// ---- sound streams ------------------------------------------------------------------------------
sound_stream *device_sound_interface::stream_alloc(int inputs, int outputs, u32 rate, sound_stream_flags) {
    auto *self = this;
    m_streams.push_back(std::make_unique<sound_stream>(*m_sound_device, inputs, outputs, rate,
                                                       [self](sound_stream &st) { self->sound_stream_update(st); }));
    m_sound_device->machine().m_streams.push_back(m_streams.back().get());
    return m_streams.back().get();
}

attotime sound_stream::sample_to_time(u64 index) const {
    // MAME: seconds exact, the remainder rounded up to whole attoseconds
    const u64 secs = index / m_sample_rate, rem = index % m_sample_rate;
    const attotime::i128 num = (attotime::i128)rem * ATTOSECONDS_PER_SECOND + m_sample_rate - 1;
    u64 q, r;
    const attoseconds_t attos = attotime::div64(num, m_sample_rate, q, r) ? (attoseconds_t)q : (attoseconds_t)(num / m_sample_rate);
    return attotime((s64)secs, attos);
}

u64 sound_stream::index_at(const attotime &t) const {
    const attotime::i128 part = (attotime::i128)t.attoseconds() * m_sample_rate;
    u64 q, r;
    return (u64)t.seconds() * m_sample_rate +
           (attotime::div64(part, ATTOSECONDS_PER_SECOND, q, r) ? q : (u64)(part / ATTOSECONDS_PER_SECOND)) + 1;
}

void sound_stream::update() {
    if (!m_device->machine().m_exact || !m_sample_rate || m_in_update) return;
    const attotime now = m_device->machine().time();
    if (now.is_zero()) return;
    const s64 n = (s64)(index_at(now) - m_write_sample);   // up to and including the current sample
    if (n <= 0) return;
    m_samples_to_update = (s32)n;
    m_in_update = true;
    for (auto &b : m_buf) b.assign((size_t)n, 0.0f);
    m_cb(*this);
    for (int o = 0; o < m_outputs; o++) m_out[o].insert(m_out[o].end(), m_buf[o].begin(), m_buf[o].end());
    m_write_sample += (u64)n;
    m_in_update = false;
    m_samples_to_update = 0;
}

// ---- memory shares -------------------------------------------------------------------------------
namespace {
std::map<std::string, std::pair<void *, size_t>> &shares() { static std::map<std::string, std::pair<void *, size_t>> m; return m; }
}
void hk_register_share(const std::string &fulltag, void *ptr, size_t bytes) { shares()[fulltag] = {ptr, bytes}; }
std::pair<void *, size_t> hk_find_share(const std::string &fulltag) {
    auto it = shares().find(fulltag);
    return it == shares().end() ? std::pair<void *, size_t>{nullptr, 0} : it->second;
}

// ---- NVRAM files ---------------------------------------------------------------------------------
namespace {
struct file_read_stream : util::read_stream {
    FILE *f;
    explicit file_read_stream(FILE *fp) : f(fp) {}
    size_t read_some(void *buf, size_t n) override { return fread(buf, 1, n, f); }
};
struct memory_write_stream : util::write_stream {
    std::string data;
    size_t write_some(const void *buf, size_t n) override { return data.append(static_cast<const char *>(buf), n), n; }
};
}  // namespace

std::string running_machine::nvram_filename(device_t &dev) {
    // MAME: the tag without its leading ':', with ':' as '_' (the root device's file is the set's own)
    std::string t = dev.tag();
    if (!t.empty() && t[0] == ':') t.erase(0, 1);
    for (char &c : t) if (c == ':') c = '_';
    return t;
}

void running_machine::nvram_load() {
    for (device_t *d : m_devices) {
        auto *nv = dynamic_cast<device_nvram_interface *>(d);
        if (!nv) continue;
        FILE *f = nullptr;
        if (nv->nvram_backup_enabled() && !m_nvram_dir.empty()) f = fopen((m_nvram_dir + "/" + nvram_filename(*d)).c_str(), "rb");
        if (f) {
            file_read_stream st(f);
            if (!nv->nvram_load(st)) fprintf(stderr, "error reading NVRAM file %s\n", nvram_filename(*d).c_str());
            fclose(f);
        } else {
            nv->nvram_reset();
        }
    }
}

void running_machine::nvram_save() {
    if (m_nvram_dir.empty()) return;
    for (device_t *d : m_devices) {
        auto *nv = dynamic_cast<device_nvram_interface *>(d);
        if (!nv || !nv->nvram_can_save()) continue;
        // whole or not at all (hk::sys::write_file): a full or failing disk, or a kill half way, keeps
        // the file from before (a 573's installed game is in these)
        std::string path = m_nvram_dir + "/" + nvram_filename(*d);
        memory_write_stream st;
        nv->nvram_save(st);
        if (!hk::sys::write_file(path, st.data)) fprintf(stderr, "cannot write %s: the one from before is kept\n", path.c_str());
    }
}
