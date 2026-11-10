// SPDX-License-Identifier: GPL-3.0-or-later
// HIRAKU: the subset of MAME's emulator framework (emu.h) that the H8 CPU
// core and the SWP30 need, so their MAME sources compile nearly unchanged
// (third_party/mame/README.md lists every edit). Not MAME code: a small
// reimplementation of the same interfaces.
//
// - devices: a tree built at construction time (device_add_mconfig runs as each
//   device is created), started and reset in tree order;
// - finders resolve to the device created with them (or to the device given to
//   set_tag), not by tag lookup;
// - address spaces are flat page tables (direct pages for RAM/ROM, per-byte
//   handler tables for I/O), compiled from address_map entries;
// - running_machine is a small scheduler: CPUs in timeslices up to the next
//   timer, timers fired in time order; time() is the executing CPU's local time.
#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <ctime>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

// ---- basic types ----------------------------------------------------------------------
using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s8 = int8_t;
using s16 = int16_t;
using s32 = int32_t;
using s64 = int64_t;
using offs_t = u32;
using attoseconds_t = s64;

#define ATTR_COLD
#define ATTR_PRINTF(x, y)
#define FUNC(x) &x
#define NAME(x) x, #x   // as MAME: the object and its name (save states, lambda handlers)
#define STRUCT_MEMBER(s, m) #s "." #m
#define ROM_NAME(name) nullptr
// ROM declarations (the XT446 glue loads the files itself)
#define ROM_START(name) [[maybe_unused]] static const tiny_rom_entry *rom_##name##_unused = nullptr; namespace {
#define ROM_END }
#define ROM_REGION16_LE(...)
#define ROM_REGION32_LE(...)
#define ROM_REGION(...)
#define ROM_LOAD(...)
#define ROM_LOAD16_WORD_SWAP(...)
#define ROM_LOAD32_WORD(...)
#define ACCESSING_BITS_0_7 ((mem_mask & 0x000000ffU) != 0)
#define ACCESSING_BITS_8_15 ((mem_mask & 0x0000ff00U) != 0)
#define ACCESSING_BITS_0_15 ((mem_mask & 0x0000ffffU) != 0)
#define ACCESSING_BITS_16_31 ((mem_mask & 0xffff0000U) != 0)
#define COMBINE_DATA(varptr) (*(varptr) = (*(varptr) & ~mem_mask) | (data & mem_mask))
#define DEVICE_SELF ""
#define DEVICE_SELF_OWNER "^"
#define TIMER_CALLBACK_MEMBER(name) void name(s32 param)

constexpr int ASSERT_LINE = 1;
constexpr int CLEAR_LINE = 0;
constexpr int HOLD_LINE = 2;
constexpr int INPUT_LINE_NMI = 32;
constexpr int INPUT_LINE_IRQ0 = 0, INPUT_LINE_IRQ1 = 1, INPUT_LINE_IRQ2 = 2, INPUT_LINE_IRQ3 = 3, INPUT_LINE_IRQ4 = 4,
              INPUT_LINE_IRQ5 = 5, INPUT_LINE_IRQ6 = 6, INPUT_LINE_IRQ7 = 7, INPUT_LINE_IRQ8 = 8, INPUT_LINE_IRQ9 = 9,
              INPUT_LINE_IRQ10 = 10, INPUT_LINE_IRQ11 = 11, INPUT_LINE_IRQ12 = 12, INPUT_LINE_IRQ13 = 13,
              INPUT_LINE_IRQ14 = 14, INPUT_LINE_IRQ15 = 15;

enum { STATE_GENPC = -1, STATE_GENPCBASE = -2, STATE_GENSP = -3, STATE_GENFLAGS = -4 };
enum { AS_PROGRAM = 0, AS_DATA = 1, AS_IO = 2, AS_OPCODES = 3 };
enum endianness_t { ENDIANNESS_LITTLE, ENDIANNESS_BIG };
enum { SUSPEND_REASON_HALT = 1, SUSPEND_REASON_RESET = 2, SUSPEND_REASON_SPIN = 4, SUSPEND_REASON_TRIGGER = 8,
       SUSPEND_REASON_DISABLE = 16, SUSPEND_REASON_TIMESLICE = 32, SUSPEND_REASON_CLOCK = 64 };

template <typename T> constexpr T BIT(T x, int n) noexcept { return (x >> n) & T(1); }
template <typename T> constexpr T BIT(T x, int n, int w) noexcept { return (x >> n) & ((T(1) << w) - 1); }

template <typename T, typename U> constexpr T downcast(U *p) { return static_cast<T>(p); }
template <typename T, typename U> constexpr T downcast(U &r) { return static_cast<T>(r); }
template <typename T, typename... B> constexpr T bitswap(T val, B... bits) {
    T r = 0;
    ((r = (r << 1) | ((val >> bits) & 1)), ...);
    return r;
}
template <int N, typename T, typename... B> constexpr T bitswap(T val, B... bits) { return bitswap<T>(val, bits...); }

inline s64 mul_32x32(s32 a, s32 b) { return s64(a) * s64(b); }
inline u64 mulu_32x32(u32 a, u32 b) { return u64(a) * u64(b); }
inline u32 divu_64x32(u64 a, u32 b) { return u32(a / b); }
inline s32 div_64x32(s64 a, s32 b) { return s32(a / b); }
// MAME's DERIVED_CLOCK: a clock given as a ratio of the owner's
constexpr u32 DERIVED_CLOCK(u32 num, u32 den) { return 0xff000000u | (num << 12) | den; }

namespace util {
template <typename T> constexpr T make_bitmask(unsigned n) { return n >= sizeof(T) * 8 ? ~T(0) : T((T(1) << n) - 1); }
template <typename T> constexpr std::make_signed_t<T> sext(T value, unsigned width) noexcept {
    using S = std::make_signed_t<T>;
    unsigned sh = sizeof(T) * 8 - width;
    return S(value << sh) >> sh;
}
}  // namespace util
using util::make_bitmask;
// MAME's read_or_write (emumem.h): which accesses a handler or tap sees
enum class read_or_write { READ = 1, WRITE = 2, READWRITE = 3 };

// MAME's PAIR: a 32-bit register viewed as bytes and words (little-endian host)
union PAIR {
    struct { u8 l, h, h2, h3; } b;
    struct { u16 l, h; } w;
    struct { s8 l, h, h2, h3; } sb;
    struct { s16 l, h; } sw;
    u32 d;
    s32 sd;
};
union PAIR16 {
    struct { u8 l, h; } b;
    struct { s8 l, h; } sb;
    u16 w;
    s16 sw;
};
constexpr u16 swapendian_int16(u16 v) { return u16((v << 8) | (v >> 8)); }
constexpr u16 big_endianize_int16(u16 v) { return swapendian_int16(v); }
constexpr u32 big_endianize_int32(u32 v) { return (v << 24) | ((v << 8) & 0xff0000) | ((v >> 8) & 0xff00) | (v >> 24); }
constexpr u16 little_endianize_int16(u16 v) { return v; }
constexpr u32 swapendian_int32(u32 v) { return (v << 24) | ((v << 8) & 0xff0000) | ((v >> 8) & 0xff00) | (v >> 24); }

// ---- printf-style formatting that accepts std::string and any integer width (MAME's strformat)
namespace hk_mame {
std::string vformat(const char *fmt, const std::vector<std::string> &conv);
struct fmt_arg {
    enum kind_t { INT, UINT, DBL, STR, PTR } kind;
    long long i = 0;
    unsigned long long u = 0;
    double d = 0;
    std::string s;
    const void *p = nullptr;
};
template <typename T> fmt_arg make_arg(const T &v) {
    fmt_arg a;
    if constexpr (std::is_same_v<T, bool>) { a.kind = fmt_arg::UINT; a.u = v; a.i = v; }
    else if constexpr (std::is_enum_v<T>) { a.kind = fmt_arg::INT; a.i = (long long)v; a.u = (unsigned long long)v; }
    else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) { a.kind = fmt_arg::INT; a.i = v; a.u = (unsigned long long)(std::make_unsigned_t<T>)v; }
    else if constexpr (std::is_integral_v<T>) { a.kind = fmt_arg::UINT; a.u = v; a.i = (long long)v; }
    else if constexpr (std::is_floating_point_v<T>) { a.kind = fmt_arg::DBL; a.d = v; }
    else if constexpr (std::is_convertible_v<T, std::string_view>) { a.kind = fmt_arg::STR; a.s = std::string(std::string_view(v)); }
    else if constexpr (std::is_pointer_v<T>) { a.kind = fmt_arg::PTR; a.p = (const void *)v; }
    else { a.kind = fmt_arg::STR; a.s = "?"; }
    return a;
}
std::string format_args(const char *fmt, const std::vector<fmt_arg> &args);
}  // namespace hk_mame

namespace util {
template <typename... A> std::string string_format(const char *fmt, A &&...a) {
    return hk_mame::format_args(fmt, {hk_mame::make_arg(a)...});
}
template <typename... A> std::string string_format(const std::string &fmt, A &&...a) {
    return hk_mame::format_args(fmt.c_str(), {hk_mame::make_arg(a)...});
}
template <typename... A> void stream_format(std::ostream &o, const char *fmt, A &&...a) {
    o << hk_mame::format_args(fmt, {hk_mame::make_arg(a)...});
}
}  // namespace util
using util::string_format;

[[noreturn]] void hk_fatal(const std::string &text);
// type-safe like MAME's (std::string arguments)
template <typename... A> [[noreturn]] void fatalerror(const char *fmt, A &&...a) {
    hk_fatal(util::string_format(fmt, std::forward<A>(a)...));
}
class emu_fatalerror {
public:
    template <typename... A> emu_fatalerror(const char *fmt, A &&...a) : m_text(util::string_format(fmt, std::forward<A>(a)...)) {}
    const char *what() const { return m_text.c_str(); }
private:
    std::string m_text;
};

// ---- attotime -------------------------------------------------------------------------
constexpr attoseconds_t ATTOSECONDS_PER_SECOND = 1'000'000'000'000'000'000LL;
constexpr attoseconds_t ATTOSECONDS_PER_MILLISECOND = ATTOSECONDS_PER_SECOND / 1000;
constexpr attoseconds_t ATTOSECONDS_PER_MICROSECOND = ATTOSECONDS_PER_SECOND / 1000000;
constexpr attoseconds_t ATTOSECONDS_PER_NANOSECOND = ATTOSECONDS_PER_SECOND / 1000000000;
constexpr attoseconds_t HZ_TO_ATTOSECONDS(u64 hz) { return ATTOSECONDS_PER_SECOND / (attoseconds_t)hz; }
constexpr attoseconds_t HZ_TO_ATTOSECONDS(u32 hz) { return ATTOSECONDS_PER_SECOND / (attoseconds_t)hz; }
constexpr attoseconds_t HZ_TO_ATTOSECONDS(int hz) { return ATTOSECONDS_PER_SECOND / (attoseconds_t)hz; }
constexpr attoseconds_t HZ_TO_ATTOSECONDS(double hz) { return attoseconds_t(ATTOSECONDS_PER_SECOND / hz); }
template <typename T> constexpr attoseconds_t ATTOSECONDS_IN_MSEC(T x) { return attoseconds_t(x) * ATTOSECONDS_PER_MILLISECOND; }
template <typename T> constexpr attoseconds_t ATTOSECONDS_IN_USEC(T x) { return attoseconds_t(x) * ATTOSECONDS_PER_MICROSECOND; }
template <typename T> constexpr attoseconds_t ATTOSECONDS_IN_NSEC(T x) { return attoseconds_t(x) * ATTOSECONDS_PER_NANOSECOND; }

class attotime {
public:
    using i128 = __int128;
    // HIRAKU: n / d and n % d, when n >= 0 and the quotient fits in 64 bits, with one x86-64 divq:
    // the same numbers as the 128-bit division (a libgcc call, ~3% of a 573 install's frame
    // time), which the callers keep for every other case and on other hosts
    static inline bool div64(i128 n, u64 d, u64 &q, u64 &r) {
#if defined(__x86_64__)
        const unsigned __int128 u = (unsigned __int128)n;
        const u64 hi = (u64)(u >> 64), lo = (u64)u;
        if (n >= 0 && hi < d) {
            __asm__("divq %4" : "=a"(q), "=d"(r) : "a"(lo), "d"(hi), "rm"(d) : "cc");
            return true;
        }
#endif
        (void)n; (void)d; (void)q; (void)r;
        return false;
    }
    constexpr attotime() : m_v(0) {}
    constexpr attotime(s64 seconds, attoseconds_t attos) : m_v(i128(seconds) * ATTOSECONDS_PER_SECOND + attos) {}
    static constexpr attotime raw(i128 v) { attotime t; t.m_v = v; return t; }
    static const attotime zero;
    static const attotime never;

    bool is_never() const { return m_v >= NEVER; }
    bool is_zero() const { return m_v == 0; }
    s64 seconds() const {
        if (is_never()) return 1000000000;
        u64 q, r;
        return div64(m_v, ATTOSECONDS_PER_SECOND, q, r) ? (s64)q : (s64)(m_v / ATTOSECONDS_PER_SECOND);
    }
    attoseconds_t attoseconds() const {
        if (is_never()) return 0;
        u64 q, r;
        return div64(m_v, ATTOSECONDS_PER_SECOND, q, r) ? (attoseconds_t)r : (attoseconds_t)(m_v % ATTOSECONDS_PER_SECOND);
    }
    double as_double() const { return is_never() ? 1e9 : (double)m_v / 1e18; }
    double as_hz() const { return 1.0 / as_double(); }
    u64 as_ticks(u32 frequency) const {
        if (is_never()) return ~u64(0);
        const i128 n = m_v * frequency;
        u64 q, r;
        return div64(n, ATTOSECONDS_PER_SECOND, q, r) ? q : (u64)(n / ATTOSECONDS_PER_SECOND);
    }
    u64 as_ticks(u64 frequency) const {
        if (is_never()) return ~u64(0);
        const i128 n = m_v * (i128)frequency;
        u64 q, r;
        return div64(n, ATTOSECONDS_PER_SECOND, q, r) ? q : (u64)(n / ATTOSECONDS_PER_SECOND);
    }
    // as MAME: whole seconds exact, the rest in whole attoseconds per tick
    static attotime from_ticks(u64 ticks, u32 frequency) {
        if (!frequency) return never;
        const attoseconds_t apt = HZ_TO_ATTOSECONDS(frequency);
        if (ticks < frequency) return raw(i128(ticks) * apt);
        return attotime((s64)(ticks / frequency), (attoseconds_t)(ticks % frequency) * apt);
    }
    static attotime from_ticks(u64 ticks, u64 frequency) { return from_ticks(ticks, (u32)frequency); }
    static attotime from_ticks(u64 ticks, int frequency) { return from_ticks(ticks, (u32)frequency); }
    static attotime from_hz(double hz) { return raw((i128)(1e18 / hz)); }
    static attotime from_hz(u32 hz) { return hz > 1 ? raw(HZ_TO_ATTOSECONDS(hz)) : hz == 1 ? attotime(1, 0) : never; }
    static attotime from_hz(int hz) { return hz > 0 ? from_hz(u32(hz)) : never; }
    static attotime from_msec(s64 ms) { return raw(i128(ms) * ATTOSECONDS_PER_MILLISECOND); }
    static constexpr attotime from_seconds(s64 s) { return attotime(s, 0); }
    attoseconds_t as_attoseconds() const { return is_never() ? ATTOSECONDS_PER_SECOND : (attoseconds_t)m_v; }
    static attotime from_usec(s64 us) { return raw(i128(us) * 1'000'000'000'000LL); }
    static attotime from_nsec(s64 ns) { return raw(i128(ns) * 1'000'000'000LL); }
    static attotime from_double(double s) { return raw((i128)(s * 1e18)); }
    i128 raw_value() const { return m_v; }
    std::string as_string(int precision = 9) const;
    std::string to_string() const { return as_string(); }

    friend attotime operator+(attotime a, attotime b) {
        if (a.is_never() || b.is_never()) return never;
        return raw(a.m_v + b.m_v);
    }
    friend attotime operator-(attotime a, attotime b) {
        if (a.is_never()) return never;
        return raw(a.m_v - b.m_v);
    }
    attotime &operator+=(attotime b) { return *this = *this + b; }
    attotime &operator-=(attotime b) { return *this = *this - b; }
    attotime &operator*=(u32 f) { return *this = *this * f; }
    attotime &operator/=(u32 f) { return *this = *this / f; }
    friend attotime operator*(attotime a, u32 f) { return a.is_never() ? never : raw(a.m_v * f); }
    friend attotime operator/(attotime a, u32 f) {
        if (a.is_never()) return never;
        u64 q, r;
        return f && div64(a.m_v, f, q, r) ? raw((i128)q) : raw(a.m_v / f);
    }
    friend bool operator<(attotime a, attotime b) { return a.m_v < b.m_v; }
    friend bool operator>(attotime a, attotime b) { return a.m_v > b.m_v; }
    friend bool operator<=(attotime a, attotime b) { return a.m_v <= b.m_v; }
    friend bool operator>=(attotime a, attotime b) { return a.m_v >= b.m_v; }
    friend bool operator==(attotime a, attotime b) { return a.m_v == b.m_v; }
    friend bool operator!=(attotime a, attotime b) { return a.m_v != b.m_v; }

private:
    static constexpr i128 NEVER = i128(1000000000) * ATTOSECONDS_PER_SECOND;
    i128 m_v;
};
inline const attotime attotime::zero = attotime();
inline const attotime attotime::never = attotime::raw(attotime::NEVER);

// ---- XTAL -----------------------------------------------------------------------------
class XTAL {
public:
    constexpr explicit XTAL(double hz) : m_hz(hz) {}
    constexpr u32 value() const { return (u32)(m_hz + 1e-3); }   // as MAME
    constexpr double dvalue() const { return m_hz; }
    constexpr operator u32() const { return value(); }
    constexpr XTAL operator/(int d) const { return XTAL(m_hz / d); }
    constexpr XTAL operator*(int m) const { return XTAL(m_hz * m); }
private:
    double m_hz;
};
constexpr XTAL operator""_MHz_XTAL(long double v) { return XTAL(double(v * 1e6)); }
constexpr XTAL operator""_MHz_XTAL(unsigned long long v) { return XTAL(double(v) * 1e6); }

// ---- forward declarations -------------------------------------------------------------
using ioport_constructor = const void *;
class device_t;
class cpu_device;
class running_machine;
class address_map;
class address_space;
class emu_timer;
class memory_region;
struct tiny_rom_entry {};
struct device_state_entry {
    int m_index;
    int index() const { return m_index; }
};

// ---- device types: TYPE(config, finder, args...) creates the device ----------------------
class machine_config {
public:
    device_t *m_owner = nullptr;              // device whose device_add_mconfig is running
    running_machine *m_machine = nullptr;
    struct opts_t {
        bool drc_rwx() const { return false; }
        const char *share_directory() const { return ""; }
    };
    opts_t options() const { return {}; }
};

class device_type_base {
public:
    constexpr device_type_base(const char *shortname, const char *fullname) : m_shortname(shortname), m_fullname(fullname) {}
    const char *shortname() const { return m_shortname; }
    const char *fullname() const { return m_fullname; }
    // a device of this type, as TYPE(config, tag) would make it (slot cards)
    virtual device_t *create(machine_config &config, const char *tag, u32 clock) const = 0;
private:
    const char *m_shortname, *m_fullname;
};
using device_type = const device_type_base &;
inline bool operator==(const device_type_base &a, const device_type_base &b) { return &a == &b; }

// MAME's emulation feature flags (imperfect_features() and friends)
struct feature {
    enum type : u32 { NONE = 0, PROTECTION = 1 << 0, TIMING = 1 << 1, GRAPHICS = 1 << 2, PALETTE = 1 << 3, SOUND = 1 << 4,
                      CAPTURE = 1 << 5, CAMERA = 1 << 6, MICROPHONE = 1 << 7, CONTROLS = 1 << 8, KEYBOARD = 1 << 9,
                      MOUSE = 1 << 10, MEDIA = 1 << 11, DISK = 1 << 12, PRINTER = 1 << 13, TAPE = 1 << 14,
                      PUNCH = 1 << 15, DRUM = 1 << 16, ROM = 1 << 17, COMMS = 1 << 18, LAN = 1 << 19, WAN = 1 << 20 };
};
using feature_type = feature::type;
inline int core_stricmp(std::string_view a, std::string_view b) {
    for (size_t i = 0;; i++) {
        int ca = i < a.size() ? tolower((unsigned char)a[i]) : -1, cb = i < b.size() ? tolower((unsigned char)b[i]) : -1;
        if (ca != cb || ca < 0) return ca - cb;
    }
}
template <typename T> std::enable_if_t<std::is_array_v<T>, std::unique_ptr<T>> make_unique_clear(size_t n) {
    using E = std::remove_extent_t<T>;
    return std::unique_ptr<T>(new E[n]());
}
template <typename T> std::enable_if_t<!std::is_array_v<T>, std::unique_ptr<T>> make_unique_clear() { return std::unique_ptr<T>(new T()); }

class finder_base {
public:
    finder_base() = default;
    finder_base(device_t *owner, const char *tag) : m_owner(owner), m_tag(tag ? tag : "") {}
    virtual ~finder_base() = default;
    virtual device_t *device_ptr() const = 0;
    static constexpr const char *DUMMY_TAG = "finder_dummy_tag";
    const char *finder_tag() const { return m_tag.c_str(); }
    device_t *m_owner = nullptr;
    std::string m_tag;
};

void hk_register_device(machine_config &config, device_t *dev);   // tree order bookkeeping
device_t *hk_owner_of(device_t *dev);
device_t *hk_find_device(device_t *owner, const char *tag);   // by tag relative to owner

template <class D> class device_creator : public device_type_base {
public:
    using device_type_base::device_type_base;
    // TYPE(config, finder, args...) and TYPE(config, "tag", args...)
    template <typename F, typename... A> D &operator()(machine_config &config, F &&finder, A &&...args) const;
    device_t *create(machine_config &config, const char *tag, u32 clock) const override;
};

#define DECLARE_DEVICE_TYPE(Type, Class) class Class; extern const device_creator<Class> Type;
#define DEFINE_DEVICE_TYPE(Type, Class, Short, Full) const device_creator<Class> Type(Short, Full);

device_t *hk_config_current(device_t *owner);              // the device whose mconfig is running
class device_interface;
device_t *hk_interface_device(device_interface &i);
namespace hk_mame { template <class O> device_t *owner_ptr(O &o) { if constexpr (std::is_base_of_v<device_interface, O>) return hk_interface_device(o); else return (device_t *)&o; } }

// ---- member-function binding (delegates, devcb .set(device, FUNC(...))) -----------------
namespace hk_mame {
template <typename F> struct member_class;
template <class C, typename R, typename... A> struct member_class<R (C::*)(A...)> { using type = C; };
template <class C, typename R, typename... A> struct member_class<R (C::*)(A...) const> { using type = C; };
template <class C, typename R, typename... A> struct member_class<R (C::*)(A...) noexcept> { using type = C; };
template <class C, typename R, typename... A> struct member_class<R (C::*)(A...) const noexcept> { using type = C; };
template <typename F> using member_class_t = typename member_class<F>::type;

// a callable forwarding exactly the arguments the member function takes (SFINAE-friendly,
// so devcb's "which signature is this" checks see the real arity)
template <class C, typename F> auto bind_member(C *obj, F f) {
    return [obj, f](auto &&...a) -> decltype(std::invoke(f, obj, std::forward<decltype(a)>(a)...)) {
        return std::invoke(f, obj, std::forward<decltype(a)>(a)...);
    };
}
// the device behind a finder, a pointer or a reference
template <typename D> device_t *device_of(D &&d) {
    using T = std::remove_cv_t<std::remove_reference_t<D>>;
    if constexpr (std::is_convertible_v<D, const char *>) return nullptr;   // a tag: see bind_device_member
    else if constexpr (std::is_base_of_v<finder_base, T>) return d.device_ptr();
    else if constexpr (std::is_pointer_v<T>) return d;
    else return &d;
}
template <typename D, typename F> auto bind_device_member(D &&dev, F f, device_t *base = nullptr) {
    using C = member_class_t<F>;
    if constexpr (std::is_convertible_v<D, const char *>) {
        std::string tag = dev;
        return [base, tag, f, obj = (C *)nullptr](auto &&...a) mutable -> decltype(std::invoke(f, (C *)nullptr, std::forward<decltype(a)>(a)...)) {
            if (!obj) obj = dynamic_cast<C *>(tag.empty() ? base : hk_find_device(base, tag.c_str()));
            return std::invoke(f, obj, std::forward<decltype(a)>(a)...);
        };
    } else {
        return bind_member(dynamic_cast<C *>(device_of(dev)), f);
    }
}
}  // namespace hk_mame

// MAME's device_delegate<R (A...)>: (member, object), (device, member) or a callable
template <typename Sig> class device_delegate;
template <typename R, typename... A> class device_delegate<R(A...)> {
public:
    device_delegate() = default;
    template <typename F, class C, std::enable_if_t<std::is_member_function_pointer_v<F>, int> = 0>
    device_delegate(F f, C *obj) : m_f(hk_mame::bind_member(static_cast<hk_mame::member_class_t<F> *>(obj), f)) {}
    template <class D, typename F, std::enable_if_t<std::is_member_function_pointer_v<F>, int> = 0>
    device_delegate(D &dev, F f, const char * = nullptr) : m_f(hk_mame::bind_device_member(dev, f)) {}
    template <typename L, std::enable_if_t<std::is_invocable_r_v<R, L, A...> && !std::is_same_v<std::decay_t<L>, device_delegate>, int> = 0>
    device_delegate(L l) : m_f(std::move(l)) {}
    // MAME's owner-only constructor: an unset delegate, bound later with set()
    template <class D, std::enable_if_t<std::is_class_v<D> && !std::is_invocable_v<D &, A...> && !std::is_same_v<std::decay_t<D>, device_delegate>, int> = 0>
    explicit device_delegate(D &) {}
    template <typename... X> void set(X &&...x) { *this = device_delegate(std::forward<X>(x)...); }
    void resolve() {}
    void resolve_safe(...) {}
    bool isnull() const { return !m_f; }
    explicit operator bool() const { return bool(m_f); }
    R operator()(A... a) const { return m_f(std::forward<A>(a)...); }
    void reset() { m_f = nullptr; }
private:
    std::function<R(A...)> m_f;
};
template <typename Sig> using delegate = device_delegate<Sig>;
using timer_expired_delegate = device_delegate<void(s32)>;
using read8smo_delegate = device_delegate<u8()>;
using read8sm_delegate = device_delegate<u8(offs_t)>;
using read8s_delegate = device_delegate<u8(offs_t, u8)>;
using read16smo_delegate = device_delegate<u16()>;
using read16sm_delegate = device_delegate<u16(offs_t)>;
using read16s_delegate = device_delegate<u16(offs_t, u16)>;
using read32smo_delegate = device_delegate<u32()>;
using read32sm_delegate = device_delegate<u32(offs_t)>;
using read32s_delegate = device_delegate<u32(offs_t, u32)>;
using write8smo_delegate = device_delegate<void(u8)>;
using write8sm_delegate = device_delegate<void(offs_t, u8)>;
using write8s_delegate = device_delegate<void(offs_t, u8, u8)>;
using write16smo_delegate = device_delegate<void(u16)>;
using write16sm_delegate = device_delegate<void(offs_t, u16)>;
using write16s_delegate = device_delegate<void(offs_t, u16, u16)>;
using write32smo_delegate = device_delegate<void(u32)>;
using write32sm_delegate = device_delegate<void(offs_t, u32)>;
using write32s_delegate = device_delegate<void(offs_t, u32, u32)>;
using write_line_delegate = device_delegate<void(int)>;

// ---- devcb ----------------------------------------------------------------------------
template <typename T> class devcb_read_base {
public:
    devcb_read_base() = default;
    template <class O> devcb_read_base(O &owner, T defval = 0) : m_owner(hk_mame::owner_ptr(owner)), m_def(defval) {}
    device_t *m_owner = nullptr;
    // bind() hands out a proxy: MAME's `auto read_portN() { return ...bind(); }` copies it
    struct binder {
        devcb_read_base *p;
        template <typename L, std::enable_if_t<!std::is_member_function_pointer_v<std::decay_t<L>>, int> = 0>
        binder &set(L &&l) { p->set(std::forward<L>(l)); return *this; }
        template <typename F, std::enable_if_t<std::is_member_function_pointer_v<F>, int> = 0>
        binder &set(F f) { p->set(hk_mame::bind_device_member("", f, hk_config_current(p->m_owner))); return *this; }
        template <typename D, typename F, std::enable_if_t<std::is_member_function_pointer_v<F>, int> = 0>
        binder &set(D &&dev, F f) { p->set(hk_mame::bind_device_member(dev, f, hk_config_current(p->m_owner))); return *this; }
        binder &set_constant(T v) { p->set_constant(v); return *this; }
    };
    binder bind() { return {this}; }
    template <typename L> void set(L &&l) {
        if constexpr (std::is_invocable_v<L, offs_t, T>) m_f = [l = std::forward<L>(l)](offs_t o, T m) mutable { return T(l(o, m)); };
        else if constexpr (std::is_invocable_v<L, offs_t>) m_f = [l = std::forward<L>(l)](offs_t o, T) mutable { return T(l(o)); };
        else m_f = [l = std::forward<L>(l)](offs_t, T) mutable { return T(l()); };
    }
    void set_constant(T v) { m_f = [v](offs_t, T) { return v; }; }
    bool isunset() const { return !m_f; }
    T operator()(offs_t o = 0, T mask = ~T(0)) { return m_f ? m_f(o, mask) : m_def; }
    template <unsigned N> class array : public std::array<devcb_read_base, N> {
    public:
        array(device_t &d, T defval = 0) { for (auto &e : *this) e = devcb_read_base(d, defval); }
    };
private:
    std::function<T(offs_t, T)> m_f;
    T m_def = 0;
};

template <typename T> class devcb_write_base {
public:
    devcb_write_base() = default;
    template <class O> devcb_write_base(O &owner) : m_owner(hk_mame::owner_ptr(owner)) {}
    device_t *m_owner = nullptr;
    struct binder {
        devcb_write_base *p;
        template <typename L, std::enable_if_t<!std::is_member_function_pointer_v<std::decay_t<L>>, int> = 0>
        binder &set(L &&l) { p->set(std::forward<L>(l)); return *this; }
        template <typename F, std::enable_if_t<std::is_member_function_pointer_v<F>, int> = 0>
        binder &set(F f) { p->set(hk_mame::bind_device_member("", f, hk_config_current(p->m_owner))); return *this; }
        template <typename D, typename F, std::enable_if_t<std::is_member_function_pointer_v<F>, int> = 0>
        binder &set(D &&dev, F f) { p->set(hk_mame::bind_device_member(dev, f, hk_config_current(p->m_owner))); return *this; }
        binder &set_nop() { p->set_nop(); return *this; }
    };
    binder bind() { return {this}; }
    template <typename L> void set(L &&l) {
        if constexpr (std::is_invocable_v<L, offs_t, T, T>) m_f = [l = std::forward<L>(l)](offs_t o, T d, T m) mutable { l(o, d, m); };
        else if constexpr (std::is_invocable_v<L, offs_t, T>) m_f = [l = std::forward<L>(l)](offs_t o, T d, T) mutable { l(o, d); };
        else m_f = [l = std::forward<L>(l)](offs_t, T d, T) mutable { l(d); };
    }
    void set_nop() { m_f = [](offs_t, T, T) {}; }
    bool isunset() const { return !m_f; }
    void operator()(T data) { if (m_f) m_f(0, data, ~T(0)); }
    void operator()(offs_t o, T data, T mask = ~T(0)) { if (m_f) m_f(o, data, mask); }
    template <unsigned N> class array : public std::array<devcb_write_base, N> {
    public:
        array(device_t &d) { for (auto &e : *this) e = devcb_write_base(d); }
    };
private:
    std::function<void(offs_t, T, T)> m_f;
};

using devcb_read_line = devcb_read_base<int>;
using devcb_read8 = devcb_read_base<u8>;
using devcb_read16 = devcb_read_base<u16>;
using devcb_read32 = devcb_read_base<u32>;
using devcb_write8 = devcb_write_base<u8>;
using devcb_write16 = devcb_write_base<u16>;
using devcb_write32 = devcb_write_base<u32>;

void hk_set_input_line(device_t *cpu, int line, int state);   // a cpu_device's set_input_line

class devcb_write_line {
public:
    devcb_write_line() = default;
    template <class O> devcb_write_line(O &owner) : m_owner(hk_mame::owner_ptr(owner)) {}
    struct binder {
        devcb_write_line *p;
        template <typename L, std::enable_if_t<!std::is_member_function_pointer_v<std::decay_t<L>>, int> = 0>
        binder &set(L &&l) { p->set(std::forward<L>(l)); return *this; }
        template <typename F, std::enable_if_t<std::is_member_function_pointer_v<F>, int> = 0>
        binder &set(F f) { p->set(hk_mame::bind_device_member("", f, hk_config_current(p->m_owner))); return *this; }
        template <typename D, typename F, std::enable_if_t<std::is_member_function_pointer_v<F>, int> = 0>
        binder &set(D &&dev, F f) { p->set(hk_mame::bind_device_member(dev, f, hk_config_current(p->m_owner))); return *this; }
        binder &set_nop() { p->set_nop(); return *this; }
        binder &invert() { auto f = std::move(p->m_f); p->m_f = [f](int st) { if (f) f(!st); }; return *this; }
        // an input line of a CPU, by tag relative to the device being configured
        binder &set_inputline(const char *tag, int line) {
            device_t *base = hk_config_current(p->m_owner);
            std::string t = tag;
            p->set([base, t, line, cpu = (device_t *)nullptr](int state) mutable {
                if (!cpu) cpu = t.empty() ? base : hk_find_device(base, t.c_str());
                hk_set_input_line(cpu, line, state);
            });
            return *this;
        }
        template <typename D, std::enable_if_t<!std::is_convertible_v<D, const char *>, int> = 0>
        binder &set_inputline(D &&dev, int line) {
            device_t *cpu = hk_mame::device_of(dev);
            p->set([cpu, line](int state) { hk_set_input_line(cpu, line, state); });
            return *this;
        }
    };
    device_t *m_owner = nullptr;
    binder bind() { return {this}; }
    template <typename L> void set(L &&l) { m_f = std::forward<L>(l); }
    void set_nop() { m_f = [](int) {}; }
    bool isunset() const { return !m_f; }
    void operator()(int state) { if (m_f) m_f(state); }
    template <unsigned N> class array : public std::array<devcb_write_line, N> {
    public:
        array(device_t &d) { for (auto &e : *this) e = devcb_write_line(d); }
    };
private:
    std::function<void(int)> m_f;
};

// ---- util streams / disassembler interface ----------------------------------------------
namespace util {
// MAME's read/write streams (NVRAM files): here, plain files
class read_stream {
public:
    virtual ~read_stream() = default;
    virtual size_t read_some(void *buf, size_t n) = 0;
};
class write_stream {
public:
    virtual ~write_stream() = default;
    virtual size_t write_some(const void *buf, size_t n) = 0;
};
}  // namespace util
inline std::pair<std::error_condition, size_t> read(util::read_stream &s, void *buf, size_t n) {
    size_t got = s.read_some(buf, n);
    return {got == n ? std::error_condition() : std::make_error_condition(std::errc::io_error), got};
}
inline std::pair<std::error_condition, size_t> write(util::write_stream &s, const void *buf, size_t n) {
    size_t put = s.write_some(buf, n);
    return {put == n ? std::error_condition() : std::make_error_condition(std::errc::io_error), put};
}
namespace util {
using ::read;
using ::write;
// MAME's util::ram_read: a read stream over a block of memory (a region as default NVRAM)
class ram_read_stream : public read_stream {
public:
    ram_read_stream(const void *data, size_t size) : m_data((const u8 *)data), m_size(size) {}
    size_t read_some(void *buf, size_t n) override {
        n = std::min(n, m_size - m_pos);
        memcpy(buf, m_data + m_pos, n);
        m_pos += n;
        return n;
    }
private:
    const u8 *m_data;
    size_t m_size, m_pos = 0;
};
inline std::unique_ptr<read_stream> ram_read(const void *data, size_t size) { return std::make_unique<ram_read_stream>(data, size); }
}
namespace util {
class disasm_interface {
public:
    enum : u32 { SUPPORTED = 0x80000000, STEP_OVER = 0x20000000, STEP_OUT = 0x40000000, STEP_COND = 0x10000000,
                 LENGTHMASK = 0x0000ffff, OVERINSTMASK = 0x0f000000, OVERINSTSHIFT = 24 };
    class data_buffer {
    public:
        std::function<u8(offs_t)> m_r8;
        u8 r8(offs_t pc) const { return m_r8(pc); }
        u16 r16(offs_t pc) const { return (u16(r8(pc)) << 8) | r8(pc + 1); }
        u32 r32(offs_t pc) const { return (u32(r16(pc)) << 16) | r16(pc + 2); }
        u64 r64(offs_t pc) const { return (u64(r32(pc)) << 32) | r32(pc + 4); }
    };
    virtual ~disasm_interface() = default;
    static constexpr u32 step_over_extra(u32 x) { return x << OVERINSTSHIFT; }
    virtual u32 opcode_alignment() const = 0;
    virtual offs_t disassemble(std::ostream &stream, offs_t pc, const data_buffer &opcodes, const data_buffer &params) = 0;
};
}  // namespace util

// ---- memory ---------------------------------------------------------------------------
class memory_region {
public:
    std::string m_name;
    std::vector<u8> m_data;   // the bus-width words, in host order
    u32 bytes() const { return (u32)m_data.size(); }
    u8 bytewidth() const { return m_bytewidth; }
    u8 m_bytewidth = 1;
    u8 *base() { return m_data.data(); }
    const char *name() const { return m_name.c_str(); }
    u16 &as_u16(offs_t i = 0) { return reinterpret_cast<u16 *>(m_data.data())[i]; }
    u8 &as_u8(offs_t i = 0) { return m_data[i]; }
    u32 &as_u32(offs_t i = 0) { return reinterpret_cast<u32 *>(m_data.data())[i]; }
};

class address_map_constructor {
public:
    address_map_constructor() = default;
    template <class C> address_map_constructor(void (C::*m)(address_map &), C *obj)
        : m_f([m, obj](address_map &map) { (obj->*m)(map); }) { if constexpr (std::is_base_of_v<device_t, C>) m_owner = obj; }
    template <class C> address_map_constructor(void (C::*m)(address_map &) const, const C *obj)
        : m_f([m, obj](address_map &map) { (obj->*m)(map); }) {}
    explicit operator bool() const { return bool(m_f); }
    void operator()(address_map &map) const { m_f(map); }
    device_t *m_owner = nullptr;   // the device the map function belongs to: its tags resolve from there
private:
    std::function<void(address_map &)> m_f;
};

class address_space_config {
public:
    address_space_config() = default;
    address_space_config(const char *name, endianness_t endian, u8 datawidth, u8 addrwidth, s8 addrshift = 0,
                         address_map_constructor internal = address_map_constructor())
        : m_name(name), m_endianness(endian), m_data_width(datawidth), m_addr_width(addrwidth),
          m_logaddr_width(addrwidth), m_addr_shift(addrshift), m_internal_map(internal) {}
    address_space_config(const char *name, endianness_t endian, u8 datawidth, u8 addrwidth, s8 addrshift, u8 /*logwidth*/, u8 /*pageshift*/,
                         address_map_constructor internal = address_map_constructor())
        : address_space_config(name, endian, datawidth, addrwidth, addrshift, internal) {}
    const char *m_name = "";
    endianness_t m_endianness = ENDIANNESS_LITTLE;
    u8 m_data_width = 8, m_addr_width = 0, m_logaddr_width = 0;
    s8 m_addr_shift = 0;
    address_map_constructor m_internal_map;
};
using space_config_vector = std::vector<std::pair<int, const address_space_config *>>;
struct device_memory_interface {
    using space_config_vector = ::space_config_vector;
    device_memory_interface() = default;
    device_memory_interface(const machine_config &, device_t &) {}
    // MAME's default translation (no MMU): the address as it is; static here, as the interface
    // is folded into device_t
    static bool memory_translate(int, int, offs_t &, address_space *&) { return true; }
};

// One map entry. Handlers are normalised to (offset, data, mem_mask) at their own width.
struct address_map_entry {
    enum kind_t { NONE, MEM, HANDLER, NOP, SUBMAP };
    address_map &m_map;
    offs_t m_start, m_end, m_mirror = 0, m_select = 0;
    u64 m_umask = ~u64(0);
    kind_t m_rkind = NONE, m_wkind = NONE;
    bool m_rom = false;
    std::string m_region;
    offs_t m_region_offset = 0;
    bool m_has_region = false;
    int m_rwidth = 0, m_wwidth = 0;     // handler widths in bits
    std::function<u64(offs_t, u64)> m_rh;
    std::function<void(offs_t, u64, u64)> m_wh;
    address_map_constructor m_submap;
    device_t *m_subdev = nullptr;
    u8 *m_extmem = nullptr;             // MEM backed by memory the installer owns (install_ram/rom)

    address_map_entry(address_map &map, offs_t s, offs_t e) : m_map(map), m_start(s), m_end(e) {}

    address_map_entry &mirror(offs_t m) { m_mirror = m; return *this; }
    address_map_entry &select(offs_t s) { m_select = s; return *this; }
    // replicated over the bus as MAME does: umask16(0xff) on a 32-bit bus is 0x00ff00ff
    address_map_entry &umask16(u16 m) { m_umask = u64(m) * 0x0001000100010001ull; return *this; }
    address_map_entry &umask32(u32 m) { m_umask = u64(m) * 0x0000000100000001ull; return *this; }
    address_map_entry &umask64(u64 m) { m_umask = m; return *this; }
    address_map_entry &ram() { m_rkind = m_wkind = MEM; return *this; }
    address_map_entry &rom() { m_rkind = MEM; m_wkind = NOP; m_rom = true; return *this; }
    address_map_entry &nopw() { m_wkind = NOP; return *this; }
    address_map_entry &nopr() { m_rkind = NOP; return *this; }
    address_map_entry &noprw() { m_rkind = m_wkind = NOP; return *this; }
    address_map_entry &unmaprw() { m_rkind = m_wkind = NONE; return *this; }
    address_map_entry &region(const char *tag, offs_t off) { m_region = tag; m_region_offset = off; m_has_region = true; return *this; }
    // MAME's wait states for CPUs whose accesses can be redone (cpu_is_interruptible, the 68000):
    // before_time gives the cycle the access may happen at, after_delay the cycles it then takes
    template <typename D, typename F> address_map_entry &before_time(D &&dev, F f);
    template <typename D, typename F> address_map_entry &after_delay(D &&dev, F f);
    std::function<u64(offs_t, u64)> m_before_time;
    std::function<u32(offs_t)> m_after_delay;
    // MAME's share(): the entry's memory, findable by tag (required_shared_ptr) from the map's device
    address_map_entry &share(const char *tag) { m_share = tag; return *this; }
    std::string m_share;

    // handlers of the map's own device ...
    template <typename RF> address_map_entry &r(RF rf);
    template <typename WF> address_map_entry &w(WF wf);
    template <typename RF, typename WF> address_map_entry &rw(RF rf, WF wf) { r(rf); return w(wf); }
    // ... or of another device
    template <typename D, typename RF> address_map_entry &r(D &&dev, RF rf);
    template <typename D, typename WF> address_map_entry &w(D &&dev, WF wf);
    template <typename D, typename RF, typename WF> address_map_entry &rw(D &&dev, RF rf, WF wf) { r(dev, rf); return w(dev, wf); }
    // submaps
    template <typename D, class C> address_map_entry &m(D &&dev, void (C::*f)(address_map &));
    // lambda handlers (MAME's lr8/lw8 and wider): the name is for MAME's debugger only
    template <typename T, typename F> address_map_entry &lr(const char *, F f) {
        m_rkind = HANDLER;
        m_rwidth = sizeof(T) * 8;
        if constexpr (std::is_invocable_v<F>) m_rh = [f](offs_t, u64) { return u64(T(f())); };
        else if constexpr (std::is_invocable_v<F, offs_t>) m_rh = [f](offs_t o, u64) { return u64(T(f(o))); };
        else m_rh = [f](offs_t o, u64 m) { return u64(T(f(o, T(m)))); };
        return *this;
    }
    template <typename T, typename F> address_map_entry &lw(const char *, F f) {
        m_wkind = HANDLER;
        m_wwidth = sizeof(T) * 8;
        if constexpr (std::is_invocable_v<F, T>) m_wh = [f](offs_t, u64 d, u64) { f(T(d)); };
        else if constexpr (std::is_invocable_v<F, offs_t, T>) m_wh = [f](offs_t o, u64 d, u64) { f(o, T(d)); };
        else m_wh = [f](offs_t o, u64 d, u64 m) { f(o, T(d), T(m)); };
        return *this;
    }
    // called as MAME's lr8(NAME(lambda)): the lambda, then its name
    template <typename F> address_map_entry &lr8(F f, const char *n) { return lr<u8>(n, f); }
    template <typename F> address_map_entry &lr16(F f, const char *n) { return lr<u16>(n, f); }
    template <typename F> address_map_entry &lr32(F f, const char *n) { return lr<u32>(n, f); }
    template <typename F> address_map_entry &lw8(F f, const char *n) { return lw<u8>(n, f); }
    template <typename F> address_map_entry &lw16(F f, const char *n) { return lw<u16>(n, f); }
    template <typename F> address_map_entry &lw32(F f, const char *n) { return lw<u32>(n, f); }

    template <class C, typename F> void set_r(C *obj, F f);
    template <class C, typename F> void set_w(C *obj, F f);
};

class address_map {
public:
    explicit address_map(device_t &dev) : m_device(&dev) {}
    void global_mask(offs_t m) { m_global_mask = m; }
    offs_t m_global_mask = ~offs_t(0);
    address_map_entry &operator()(offs_t start, offs_t end) {
        m_entries.push_back(std::make_unique<address_map_entry>(*this, start, end));
        return *m_entries.back();
    }
    device_t *m_device;
    std::vector<std::unique_ptr<address_map_entry>> m_entries;
};

// A flat address space: 4 KiB pages, either direct (one memory block covers the
// page) or per-byte entry tables. Addresses in the API are in the space's units
// (bytes when addr_shift is 0, bus words when it is negative), as in MAME.
class address_space {
public:
    address_space(device_t &dev, const address_space_config &cfg, int spacenum);
    void install(std::unique_ptr<address_map> map);    // later entries win
    void finalize();

    u8 read_byte(offs_t a) { return (u8)read_sized(unit_to_byte(a), 1); }
    u16 read_word(offs_t a) { return (u16)read_sized(unit_to_byte(a), 2); }
    u32 read_dword(offs_t a) { return (u32)read_sized(unit_to_byte(a), 4); }
    u64 read_qword(offs_t a) { return read_sized(unit_to_byte(a), 8); }
    void write_byte(offs_t a, u8 d) { write_sized(unit_to_byte(a), 1, d); }
    void write_word(offs_t a, u16 d) { write_sized(unit_to_byte(a), 2, d); }
    void write_dword(offs_t a, u32 d) { write_sized(unit_to_byte(a), 4, d); }
    void write_qword(offs_t a, u64 d) { write_sized(unit_to_byte(a), 8, d); }

    // masked accesses (MAME's read_word(address, mem_mask) and friends): the lanes in mask
    u64 read_masked(offs_t a, int size, u64 mask) {
        u64 ba = unit_to_byte(a) & m_byte_mask;
        const u64 full = size == 8 ? ~u64(0) : (u64(1) << (size * 8)) - 1;
        if ((mask & full) == full) return read_sized(ba, size);
        if (m_pages[ba >> m_pbits].direct_r) return read_sized(ba, size) & mask;
        u64 sh = lane_shift(ba, size);
        const u64 bus = ba & ~u64(m_bus_bytes - 1), bm = (mask & full) << sh;
        const u64 v = read_bus(bus, bm);
        if (m_read_tap) [[unlikely]] m_read_tap((offs_t)bus, v & bm, bm);
        return (v >> sh) & mask;
    }
    void write_masked(offs_t a, int size, u64 d, u64 mask) {
        u64 ba = unit_to_byte(a) & m_byte_mask;
        const u64 full = size == 8 ? ~u64(0) : (u64(1) << (size * 8)) - 1;
        if ((mask & full) == full) { write_sized(ba, size, d); return; }
        u64 sh = lane_shift(ba, size);
        if (m_write_tap) [[unlikely]] m_write_tap((offs_t)(ba & ~u64(m_bus_bytes - 1)), (d & mask & full) << sh, (mask & full) << sh);
        write_bus(ba & ~u64(m_bus_bytes - 1), (d & mask & full) << sh, (mask & full) << sh);
    }
    u8 read_byte(offs_t a, u8 mask) { return (u8)read_masked(a, 1, mask); }
    u16 read_word(offs_t a, u16 mask) { return (u16)read_masked(a, 2, mask); }
    void write_byte(offs_t a, u8 d, u8 mask) { write_masked(a, 1, d, mask); }
    void write_word(offs_t a, u16 d, u16 mask) { write_masked(a, 2, d, mask); }
    // a masked bus access (MAME's read_dword(address, mem_mask)): the lanes in mask
    u32 read_dword(offs_t a, u32 mask) {
        u64 ba = unit_to_byte(a) & m_byte_mask;
        if (mask == 0xffffffffu) return (u32)read_sized(ba, 4);
        page_t &p = m_pages[ba >> m_pbits];
        if (p.direct_r) return (u32)read_sized(ba, 4) & mask;
        const u32 v = (u32)read_bus(ba & ~u64(3), mask) & mask;
        if (m_read_tap) [[unlikely]] m_read_tap((offs_t)(ba & ~u64(3)), v, mask);
        return v;
    }
    void write_dword(offs_t a, u32 d, u32 mask) {
        u64 ba = unit_to_byte(a) & m_byte_mask;
        if (mask == 0xffffffffu) { write_sized(ba, 4, d); return; }
        if (m_write_tap) m_write_tap((offs_t)(ba & ~u64(3)), d & mask, mask);
        write_bus(ba & ~u64(3), d & mask, mask);
    }

    u64 unmap() const { return 0; }   // what an unmapped read gives (MAME's default, 0)
    // MAME's read_interruptible / write_interruptible: a bus-wide access by a CPU that can redo it.
    // Wait-state entries (before_time, after_delay) apply to the space's CPU; without any, these
    // are plain masked accesses.
    u64 read_interruptible(offs_t a, u64 mask);
    void write_interruptible(offs_t a, u64 d, u64 mask);
    bool m_waits = false;   // an entry with wait states is mapped
    template <class C> void cache(C &c) { c.m_space = this; }
    template <class C> void specific(C &c) { c.m_space = this; }

    // run-time changes to the map (MAME's install_*), in byte addresses; a later
    // install wins over what was there
    void install_ram(offs_t start, offs_t end, void *base) { install_mem(start, end, (u8 *)base, false); }
    void install_rom(offs_t start, offs_t end, const void *base) { install_mem(start, end, (u8 *)base, true); }
    template <typename RD> void install_read_handler(offs_t start, offs_t end, RD rd) {
        address_map_entry &e = runtime_entry(start, end);
        set_runtime_read(e, std::move(rd));
        apply(e);
    }
    template <typename WD> void install_write_handler(offs_t start, offs_t end, WD wd) {
        address_map_entry &e = runtime_entry(start, end);
        set_runtime_write(e, std::move(wd));
        apply(e);
    }
    template <typename RD, typename WD> void install_readwrite_handler(offs_t start, offs_t end, RD rd, WD wd) {
        address_map_entry &e = runtime_entry(start, end);
        set_runtime_read(e, std::move(rd));
        set_runtime_write(e, std::move(wd));
        apply(e);
    }
    void nop_write(offs_t start, offs_t end) { address_map_entry &e = runtime_entry(start, end); e.m_wkind = address_map_entry::NOP; apply(e); }
    void nop_read(offs_t start, offs_t end) { address_map_entry &e = runtime_entry(start, end); e.m_rkind = address_map_entry::NOP; apply(e); }
    void nop_readwrite(offs_t start, offs_t end) { address_map_entry &e = runtime_entry(start, end); e.m_rkind = e.m_wkind = address_map_entry::NOP; apply(e); }
    void unmap_readwrite(offs_t start, offs_t end);

    const address_space_config &m_config;
    device_t &m_device;
    int m_spacenum;
    // HIRAKU debugging: observes writes that take the slow path (I/O), as MAME's install_write_tap
    std::function<void(offs_t byte_addr, u64 data, u64 mask)> m_write_tap, m_read_tap;

    struct page_t {
        u8 *direct_r = nullptr, *direct_w = nullptr;   // one memory entry covers the page: host pointer for its base
        u16 did_r = 0, did_w = 0;                       // one entry covers the page: its index + 1 (memory or not)
        std::unique_ptr<u16[]> ridx, widx;              // otherwise per byte: entry index + 1 (0 = unmapped)
    };
    // HIRAKU's recompiled 573 code reads and writes RAM through the page table directly
    // (runtime/sys573/mips_ops.h): pages of 1 << page_bits() bytes, indexed by byte address
    const page_t *pages() const { return m_pages.data(); }
    int page_bits() const { return m_pbits; }
    u32 version() const { return m_version; }   // changes whenever the map does
    // the host memory behind byte address ba, if a memory entry covers it (nullptr otherwise)
    u8 *mem_ptr(offs_t ba, bool write) const;

private:
    struct entry_rt {
        address_map_entry *e;
        u8 *mem;       // MEM: the bus words backing [start, end] (byte address start -> mem)
        offs_t base;   // byte address that maps to mem[0]
    };

    address_map_entry &runtime_entry(offs_t start, offs_t end);
    void install_mem(offs_t start, offs_t end, u8 *base, bool rom);
    void apply(address_map_entry &e);   // put one entry into the page tables
    template <typename R, typename... A> void set_runtime_read(address_map_entry &e, device_delegate<R(A...)> d) {
        e.m_rkind = address_map_entry::HANDLER;
        e.m_rwidth = sizeof(R) * 8;
        if constexpr (sizeof...(A) == 0) e.m_rh = [d](offs_t, u64) { return u64(d()); };
        else if constexpr (sizeof...(A) == 1) e.m_rh = [d](offs_t o, u64) { return u64(d(o)); };
        else e.m_rh = [d](offs_t o, u64 m) { return u64(d(o, R(m))); };
    }
    template <typename R, typename... A> void set_runtime_write(address_map_entry &e, device_delegate<R(A...)> d) {
        using V = std::tuple_element_t<(sizeof...(A) >= 2 ? 1 : 0), std::tuple<A...>>;   // the data argument
        e.m_wkind = address_map_entry::HANDLER;
        e.m_wwidth = sizeof(V) * 8;
        if constexpr (sizeof...(A) == 1) e.m_wh = [d](offs_t, u64 v, u64) { d(V(v)); };
        else if constexpr (sizeof...(A) == 2) e.m_wh = [d](offs_t o, u64 v, u64) { d(o, V(v)); };
        else e.m_wh = [d](offs_t o, u64 v, u64 m) { d(o, V(v), V(m)); };
    }

    u64 read_sized(u64 ba, int size);
    void write_sized(u64 ba, int size, u64 data);
    u64 read_slow(u64 ba, int size);
    const address_map_entry *one_handler(u64 ba, int size, bool write) const;
    void write_slow(u64 ba, int size, u64 data);
    u64 read_bus(u64 bus_ba, u64 mask);   // one bus word at aligned byte address, lanes in mask
    void write_bus(u64 bus_ba, u64 data, u64 mask);
    u64 unit_to_byte(offs_t a) const { return m_shift < 0 ? (u64)a << (-m_shift) : (u64)a; }
    void collect(address_map &map, offs_t base, offs_t mirror);
    void collect_units(address_map &sub, const address_map_entry &outer, offs_t base, offs_t mirror);
    u64 lane_shift(u64 ba, int size) const;   // shift of the size-byte lane at ba inside its bus word

    int m_bus_bytes;
    int m_pbits;
    int m_shift;
    bool m_big;
    u64 m_byte_mask;
    std::vector<page_t> m_pages;
    std::vector<entry_rt> m_rt;
    std::vector<address_map_entry *> m_all;
    std::vector<std::unique_ptr<address_map>> m_maps;
    std::vector<std::unique_ptr<std::vector<u8>>> m_owned;
    std::unique_ptr<address_map> m_runtime;   // entries installed after start
    u32 m_version = 0;
};

inline u64 address_space::lane_shift(u64 ba, int size) const {
    int off = (int)(ba & (m_bus_bytes - 1));
    return m_big ? (u64)(m_bus_bytes - off - size) * 8 : (u64)off * 8;
}

inline u64 address_space::read_sized(u64 ba, int size) {
    ba &= m_byte_mask;
    page_t &p = m_pages[ba >> m_pbits];
    if (p.direct_r) {
        u64 bus = ba & ~u64(m_bus_bytes - 1);
        const u8 *w = p.direct_r + (bus & ((u64(1) << m_pbits) - 1));
        u64 v;
        switch (m_bus_bytes) {
        case 1: v = *w; break;
        case 2: v = *(const u16 *)w; break;
        case 4: v = *(const u32 *)w; break;
        default: v = *(const u64 *)w; break;
        }
        if (size == m_bus_bytes) return v;
        return (v >> lane_shift(ba, size)) & ((u64(1) << (size * 8)) - 1);
    }
    return read_slow(ba, size);
}

inline void address_space::write_sized(u64 ba, int size, u64 data) {
    ba &= m_byte_mask;
    page_t &p = m_pages[ba >> m_pbits];
    if (p.direct_w && size == m_bus_bytes) {
        u8 *w = p.direct_w + (ba & ((u64(1) << m_pbits) - 1));
        switch (m_bus_bytes) {
        case 1: *w = (u8)data; break;
        case 2: *(u16 *)w = (u16)data; break;
        case 4: *(u32 *)w = (u32)data; break;
        default: *(u64 *)w = data; break;
        }
        return;
    }
    write_slow(ba, size, data);
}

template <int A, int D, int S, endianness_t E> struct memory_access {
    class cache {
    public:
        address_space *m_space = nullptr;
        u8 read_byte(offs_t a) { return m_space->read_byte(a); }
        u16 read_word(offs_t a) { return m_space->read_word(a); }
        u32 read_dword(offs_t a) { return m_space->read_dword(a); }
        u32 read_dword(offs_t a, u32 mask) { return m_space->read_dword(a, mask); }
        void write_dword(offs_t a, u32 d, u32 mask) { m_space->write_dword(a, d, mask); }
        u64 read_qword(offs_t a) { return m_space->read_qword(a); }
        void write_byte(offs_t a, u8 d) { m_space->write_byte(a, d); }
        void write_word(offs_t a, u16 d) { m_space->write_word(a, d); }
        void write_dword(offs_t a, u32 d) { m_space->write_dword(a, d); }
        void write_qword(offs_t a, u64 d) { m_space->write_qword(a, d); }
    };
    using specific = cache;
};
// the 68000's specifics (MAME's memory_access<24, 1, 0, BIG>::specific) also make interruptible accesses
template <> class memory_access<24, 1, 0, ENDIANNESS_BIG>::cache {
public:
    address_space *m_space = nullptr;
    u8 read_byte(offs_t a) { return m_space->read_byte(a); }
    u16 read_word(offs_t a) { return m_space->read_word(a); }
    u16 read_word(offs_t a, u16 mask) { return m_space->read_word(a, mask); }
    void write_byte(offs_t a, u8 d) { m_space->write_byte(a, d); }
    void write_word(offs_t a, u16 d) { m_space->write_word(a, d); }
    void write_word(offs_t a, u16 d, u16 mask) { m_space->write_word(a, d, mask); }
    u16 read_interruptible(offs_t a, u16 mask = 0xffff) { return (u16)m_space->read_interruptible(a, mask); }
    void write_interruptible(offs_t a, u16 d, u16 mask = 0xffff) { m_space->write_interruptible(a, d, mask); }
};

// ---- shared pointers / region pointers ---------------------------------------------------
// MAME's memory_share_creator: a device-owned block other devices find by tag
void hk_register_share(const std::string &fulltag, void *ptr, size_t bytes);
std::pair<void *, size_t> hk_find_share(const std::string &fulltag);
template <typename T> class memory_share_creator {
public:
    memory_share_creator(device_t &dev, const char *tag, size_t bytes, endianness_t) : m_data(bytes / sizeof(T)), m_tag(tag), m_dev(&dev) { m_registered = false; }
    T *target() { reg(); return m_data.data(); }
    T &operator[](size_t i) { return m_data[i]; }
    size_t bytes() const { return m_data.size() * sizeof(T); }
    size_t length() const { return m_data.size(); }
    operator T *() { return target(); }
    void reg();
    std::vector<T> m_data;
    std::string m_tag;
    device_t *m_dev;
    bool m_registered;
};
template <typename T> class required_shared_ptr {
public:
    required_shared_ptr(device_t &dev, const char *tag) : m_dev(&dev), m_tag(tag) {}
    template <typename U> void set_tag(memory_share_creator<U> &c) { m_ptr = reinterpret_cast<T *>(c.target()); m_bytes = c.bytes(); }
    void set_tag(const char *tag) { m_tag = tag; m_ptr = nullptr; }
    T *target();
    T &operator[](size_t i) { return target()[i]; }
    operator T *() { return target(); }
    size_t bytes() { target(); return m_bytes; }
    size_t length() { target(); return m_bytes / sizeof(T); }
private:
    device_t *m_dev;
    std::string m_tag;
    T *m_ptr = nullptr;
    size_t m_bytes = 0;
};
template <typename T> class optional_shared_ptr {
public:
    optional_shared_ptr(device_t &, const char *) {}
    explicit operator bool() const { return false; }
    T &operator[](size_t i) { return m_dummy; }
    u32 bytes() const { return 0; }
    u32 length() const { return 0; }
private:
    T m_dummy{};
};

memory_region *hk_find_region(device_t &dev, const char *tag);
class required_memory_region {
public:
    required_memory_region(device_t &dev, const char *tag) : m_dev(&dev), m_tag(tag) {}
    memory_region *target() const { if (!m_r) m_r = hk_find_region(*m_dev, m_tag.c_str()); return m_r; }
    memory_region *operator->() const { return target(); }
    explicit operator bool() const { return target() != nullptr; }
    bool found() const { return target() != nullptr; }
    u32 bytes() const { return target()->bytes(); }
    u8 *base() const { return target()->base(); }
private:
    device_t *m_dev;
    std::string m_tag;
    mutable memory_region *m_r = nullptr;
};
using optional_memory_region = required_memory_region;
template <typename T> class optional_region_ptr;
template <typename T> class required_region_ptr {
public:
    required_region_ptr(device_t &dev, const char *tag) : m_dev(&dev), m_tag(tag) {}
    T &operator[](size_t i) { return target()[i]; }
    T *target() {
        if (!m_ptr) {
            memory_region *r = hk_find_region(*m_dev, m_tag.c_str());
            if (!r) { if (m_optional) return nullptr; fatalerror("required region %s missing", m_tag.c_str()); }
            m_ptr = reinterpret_cast<T *>(r->base());
            m_bytes = r->bytes();
        }
        return m_ptr;
    }
    bool found() { return target() != nullptr; }
    explicit operator bool() { return found(); }
    operator T *() { return target(); }
    size_t length() { target(); return m_bytes / sizeof(T); }
    size_t bytes() { target(); return m_bytes; }
    bool m_optional = false;
    size_t m_bytes = 0;
private:
    device_t *m_dev;
    std::string m_tag;
    T *m_ptr = nullptr;
};

template <typename T> class optional_region_ptr : public required_region_ptr<T> {
public:
    optional_region_ptr(device_t &dev, const char *tag) : required_region_ptr<T>(dev, tag) { this->m_optional = true; }
};

// ---- input ports: device-level INPUT_PORTS blocks compile to nothing --------------------------
#define INPUT_PORTS_START(name) void hk_ioports_##name() {
#define INPUT_PORTS_END }
#define INPUT_PORTS_NAME(name) nullptr
#define PORT_START(...)
#define PORT_BIT(...)
#define PORT_CONFNAME(...)
#define PORT_CONFSETTING(...)
#define PORT_DIPNAME(...)
#define PORT_DIPSETTING(...)
#define PORT_DIPLOCATION(...)
#define PORT_WRITE_LINE_DEVICE_MEMBER(...)
#define PORT_READ_LINE_DEVICE_MEMBER(...)
#define PORT_NAME(...)
#define DEF_STR(x) #x
#define PORT_TOGGLE
#define PORT_PLAYER(...)
#define PORT_8WAY
#define PORT_16WAY
#define PORT_CODE(...)
#define PORT_INCLUDE(...)
#define PORT_MODIFY(...)
using ioport_value = u32;
class ioport_port {
public:
    ioport_value read() const { return m_value; }
    void write(ioport_value data, ioport_value mask = ~ioport_value(0)) { m_value = (m_value & ~mask) | (data & mask); }
    ioport_value m_value = 0;
};
class required_ioport {
public:
    required_ioport(device_t &, const char *) {}
    ioport_port *operator->() { return &m_port; }
    ioport_port &operator*() { return m_port; }
    explicit operator bool() const { return true; }
    ioport_port *target() { return &m_port; }
private:
    ioport_port m_port;
};
using optional_ioport = required_ioport;
#define ALLOW_SAVE_TYPE(...)

// ---- state interface (debugger registers: accepted, not used) ------------------------------
class device_state_entry_stub {
public:
    device_state_entry_stub &mask(u64) { return *this; }
    device_state_entry_stub &noshow() { return *this; }
    device_state_entry_stub &formatstr(const char *) { return *this; }
    device_state_entry_stub &callimport() { return *this; }
    device_state_entry_stub &callexport() { return *this; }
};


// ---- video: rectangles, bitmaps, colours, palettes ------------------------------------------
class rectangle {
public:
    constexpr rectangle() = default;
    constexpr rectangle(s32 minx, s32 maxx, s32 miny, s32 maxy) : min_x(minx), max_x(maxx), min_y(miny), max_y(maxy) {}
    constexpr s32 left() const { return min_x; }
    constexpr s32 right() const { return max_x; }
    constexpr s32 top() const { return min_y; }
    constexpr s32 bottom() const { return max_y; }
    constexpr s32 width() const { return max_x + 1 - min_x; }
    constexpr s32 height() const { return max_y + 1 - min_y; }
    void set(s32 minx, s32 maxx, s32 miny, s32 maxy) { min_x = minx; max_x = maxx; min_y = miny; max_y = maxy; }
    void setx(s32 minx, s32 maxx) { min_x = minx; max_x = maxx; }
    void sety(s32 miny, s32 maxy) { min_y = miny; max_y = maxy; }
    bool contains(s32 x, s32 y) const { return x >= min_x && x <= max_x && y >= min_y && y <= max_y; }
    rectangle &operator&=(const rectangle &r) {
        min_x = std::max(min_x, r.min_x); max_x = std::min(max_x, r.max_x);
        min_y = std::max(min_y, r.min_y); max_y = std::min(max_y, r.max_y);
        return *this;
    }
    bool empty() const { return min_x > max_x || min_y > max_y; }
    s32 min_x = 0, max_x = 0, min_y = 0, max_y = 0;
};

class rgb_t {
public:
    constexpr rgb_t() = default;
    constexpr rgb_t(u32 v) : m_v(v) {}
    constexpr rgb_t(u8 r, u8 g, u8 b) : m_v(0xff000000u | (u32(r) << 16) | (u32(g) << 8) | b) {}
    constexpr rgb_t(u8 a, u8 r, u8 g, u8 b) : m_v((u32(a) << 24) | (u32(r) << 16) | (u32(g) << 8) | b) {}
    constexpr operator u32() const { return m_v; }
    constexpr u8 r() const { return (m_v >> 16) & 0xff; }
    constexpr u8 g() const { return (m_v >> 8) & 0xff; }
    constexpr u8 b() const { return m_v & 0xff; }
    static constexpr rgb_t black() { return rgb_t(0, 0, 0); }
private:
    u32 m_v = 0;
};
constexpr u8 pal5bit(u8 bits) { bits &= 0x1f; return (bits << 3) | (bits >> 2); }
constexpr rgb_t pal555(u32 data, u8 rshift, u8 gshift, u8 bshift) {
    return rgb_t(pal5bit(data >> rshift), pal5bit(data >> gshift), pal5bit(data >> bshift));
}

template <typename T> class bitmap_t {
public:
    bitmap_t() = default;
    bitmap_t(int w, int h) { allocate(w, h); }
    void allocate(int w, int h) { m_w = w; m_h = h; m_pix.assign((size_t)w * h, T(0)); }
    void resize(int w, int h) { if (w != m_w || h != m_h) allocate(w, h); }
    int width() const { return m_w; }
    int height() const { return m_h; }
    int rowpixels() const { return m_w; }
    bool valid() const { return !m_pix.empty(); }
    T &pix(s32 y, s32 x = 0) { return m_pix[(size_t)y * m_w + x]; }
    const T &pix(s32 y, s32 x = 0) const { return m_pix[(size_t)y * m_w + x]; }
    rectangle cliprect() const { return rectangle(0, m_w - 1, 0, m_h - 1); }
    void fill(T v) { std::fill(m_pix.begin(), m_pix.end(), v); }
    void fill(T v, const rectangle &r) {
        rectangle c = r;
        c &= cliprect();
        for (s32 y = c.min_y; y <= c.max_y; y++)
            for (s32 x = c.min_x; x <= c.max_x; x++) pix(y, x) = v;
    }
    T *data() { return m_pix.data(); }
private:
    int m_w = 0, m_h = 0;
    std::vector<T> m_pix;
};
using bitmap_rgb32 = bitmap_t<u32>;
using bitmap_ind16 = bitmap_t<u16>;
using bitmap_ind8 = bitmap_t<u8>;

// MAME's draw_scanline16: 16-bit source pixels through a pen table into an RGB bitmap
inline void draw_scanline16(bitmap_rgb32 &bitmap, s32 destx, s32 desty, s32 length, const u16 *srcptr, const rgb_t *paldata) {
    u32 *d = &bitmap.pix(desty, destx);
    for (s32 i = 0; i < length; i++) d[i] = paldata ? u32(paldata[srcptr[i]]) : srcptr[i];
}

class device_palette_interface {
public:
    device_palette_interface(const machine_config &, device_t &) {}
    virtual ~device_palette_interface() = default;
    virtual u32 palette_entries() const noexcept = 0;
    virtual u32 palette_indirect_entries() const noexcept { return 0; }
    void set_pen_color(u32 i, rgb_t c) { ensure(); m_pens[i] = c; }
    const rgb_t *pens() { ensure(); return m_pens.data(); }
    rgb_t pen_color(u32 i) { ensure(); return m_pens[i]; }
private:
    void ensure() { if (m_pens.empty()) m_pens.assign(palette_entries(), rgb_t()); }
    std::vector<rgb_t> m_pens;
};

class screen_device;
class device_video_interface {
public:
    device_video_interface(const machine_config &, device_t &dev, bool screen_required = true) : m_vdev(&dev) {}
    virtual ~device_video_interface() = default;
    void set_screen(const char *tag) { m_screen_tag = tag; }
    template <typename T> void set_screen(T &&finder) { m_screen_tag = hk_mame::device_of(finder) ? "" : ""; m_screen_dev = hk_mame::device_of(finder); }
    screen_device &screen() const;
    bool has_screen() const { return m_screen_dev || !m_screen_tag.empty(); }
private:
    device_t *m_vdev;
    std::string m_screen_tag;
    mutable device_t *m_screen_dev = nullptr;
};

// input (keyboard checks in debug-only paths): nothing pressed
enum { KEYCODE_A = 1, KEYCODE_D, KEYCODE_I, KEYCODE_L, KEYCODE_M, KEYCODE_S, KEYCODE_V };
struct hk_input_manager { bool code_pressed_once(int) const { return false; } bool code_pressed(int) const { return false; } };
inline void popmessage(...) {}
template <typename... A> void osd_printf_debug(A &&...) {}
template <typename... A> void osd_printf_info(A &&...) {}
template <typename... A> void osd_printf_verbose(A &&...) {}
template <typename... A> void osd_printf_warning(A &&...) {}
template <typename... A> void osd_printf_error(A &&...) {}


// logerror where no device's is in scope (a device interface's members, as diserial's log macros):
// printed when HK_XT446_LOG is set, as a device's
bool hk_log_on();
void hk_log_text(const std::string &text);
template <typename... A> void logerror(const char *fmt, A &&...a) {
    if (hk_log_on()) hk_log_text(util::string_format(fmt, std::forward<A>(a)...));
}

// ---- device interfaces (MAME's device_interface: hooks around start and reset) ----------------
class device_interface {
public:
    device_interface(device_t &device, const char *type);
    virtual ~device_interface() = default;
    device_t &device() { return *m_device; }
    const device_t &device() const { return *m_device; }
    const char *interface_type() const { return m_type; }
    virtual void interface_config_complete() {}
    virtual void interface_pre_start() {}
    virtual void interface_post_start() {}
    virtual void interface_pre_reset() {}
    virtual void interface_post_reset() {}
    virtual void interface_pre_stop() {}
    virtual void interface_post_stop() {}
    virtual void interface_pre_save() {}
    virtual void interface_post_load() {}
    virtual void interface_clock_changed(bool) {}
private:
    device_t *m_device;
    const char *m_type;
};

// MAME's slots: a slot device holds named options (device types); the selected one is
// created as the slot's child, named after the option, once the whole machine is configured
class device_slot_interface : public device_interface {
public:
    device_slot_interface(const machine_config &, device_t &device) : device_interface(device, "slot") {}
    device_slot_interface &option_add(const char *name, const device_type_base &type) { m_options[name] = &type; return *this; }
    device_slot_interface &option_add_internal(const char *name, const device_type_base &type) { return option_add(name, type); }
    device_slot_interface &option_replace(const char *name, const device_type_base &type) { return option_add(name, type); }
    device_slot_interface &option_remove(const char *name) { m_options.erase(name); return *this; }
    device_slot_interface &option_reset() { m_options.clear(); return *this; }
    device_slot_interface &set_default_option(const char *name) { m_default = name ? name : ""; return *this; }
    device_slot_interface &set_fixed(bool f) { m_fixed = f; return *this; }
    device_slot_interface &set_option_machine_config(const char *name, std::function<void(device_t *)> f) { m_option_cfg[name] = std::move(f); return *this; }
    template <typename... A> device_slot_interface &set_option_default_bios(A &&...) { return *this; }
    template <typename T> device_slot_interface &set_options(T &&opts, const char *dflt, bool fixed) {
        opts(*this);
        set_default_option(dflt);
        set_fixed(fixed);
        return *this;
    }
    template <typename... A> device_slot_interface &set_option_device_input_defaults(A &&...) { return *this; }
    const char *default_option() const { return m_default.empty() ? nullptr : m_default.c_str(); }
    bool fixed() const { return m_fixed; }
    device_t *get_card_device() const { return m_card; }
    void hk_create_card(machine_config &config);   // the selected option, as a child device
private:
    std::map<std::string, const device_type_base *> m_options;
    std::map<std::string, std::function<void(device_t *)>> m_option_cfg;
    std::string m_default;
    bool m_fixed = false;
    device_t *m_card = nullptr;
};
template <class Card> class device_single_card_slot_interface : public device_slot_interface {
public:
    device_single_card_slot_interface(const machine_config &mconfig, device_t &device) : device_slot_interface(mconfig, device) {}
    Card *get_card_device() const { return dynamic_cast<Card *>(device_slot_interface::get_card_device()); }
};

// MAME's system_time (the RTCs' start time)
class system_time {
public:
    system_time() { set(0); }
    explicit system_time(time_t t) { set(t); }
    void set(time_t t);
    struct full_time {
        void set(struct tm &t);
        u8 second, minute, hour, mday, month;
        s32 year;
        u8 weekday;
        u16 day;
        u8 is_dst;
    };
    s64 time = 0;
    full_time local_time{}, utc_time{};
};
enum class machine_phase { PREINIT, INIT, RESET, RUNNING, EXIT };

// ---- device_t ---------------------------------------------------------------------------
class validity_checker;
class device_t {
public:
    device_t(const machine_config &mconfig, device_type type, const char *tag, device_t *owner, u32 clock);
    virtual ~device_t() = default;

    const char *tag() const { return m_tag.c_str(); }
    const char *basetag() const { return m_basetag.c_str(); }
    const char *shortname() const { return m_type.shortname(); }
    device_type type() const { return m_type; }
    device_t *owner() const { return m_owner; }
    u32 clock() const { return m_clock; }
    void set_clock(u32 c) { m_clock = c; }
    void set_clock(const XTAL &x) { m_clock = x.value(); }
    // MAME's set_unscaled_clock: a clock change at run time, which the device and its interfaces see
    void set_unscaled_clock(u32 c) {
        m_clock = c;
        if (m_started) { device_clock_changed(); for (device_interface *i : m_interfaces) i->interface_clock_changed(false); }
    }
    // as MAME's device_t: whole seconds exact, the rest in whole attoseconds per clock
    attotime clocks_to_attotime(u64 clocks) const {
        if (!m_clock) return attotime::never;
        const attoseconds_t apc = HZ_TO_ATTOSECONDS(m_clock);
        if (clocks < m_clock) return attotime::raw((attotime::i128)clocks * apc);
        return attotime((s64)(clocks / m_clock), (attoseconds_t)(clocks % m_clock) * apc);
    }
    u64 attotime_to_clocks(const attotime &d) const {
        if (!m_clock) return 0;
        return (u64)d.seconds() * m_clock + (u64)d.attoseconds() / (u64)HZ_TO_ATTOSECONDS(m_clock);
    }
    template <class T> T *subdevice(const char *tag) const { return dynamic_cast<T *>(hk_find_device(const_cast<device_t *>(this), tag)); }
    device_t *subdevice(const char *tag) const { return hk_find_device(const_cast<device_t *>(this), tag); }
    device_t *subdevice(const std::string &tag) const { return subdevice(tag.c_str()); }
    bool started() const { return m_started; }
    bool m_started = false;
    running_machine &machine() const { return *m_machine; }
    const machine_config &mconfig() const { return *m_mconfig; }
    std::string subtag(const char *tag) const { return m_tag + ":" + tag; }

    template <typename... A> void save_item(A &&...) {}
    template <typename... A> void save_pointer(A &&...) {}
    void logerror(const char *fmt) const { hk_log(fmt); }
    void logerror(const std::string &text) const { hk_log(text.c_str()); }
    template <typename... A> void logerror(const std::string &fmt, A &&...a) const {
        if (s_log) hk_log(util::string_format(fmt, std::forward<A>(a)...).c_str());
    }
    template <typename... A> void logerror(const char *fmt, A &&...a) const {
        if (s_log) hk_log(util::string_format(fmt, std::forward<A>(a)...).c_str());
    }
    template <typename C> emu_timer *timer_alloc(void (C::*f)(s32), C *obj);
    memory_region *memregion(const char *tag) { return hk_find_region(*this, tag); }

    // lifecycle (MAME's device_t interface)
    virtual void device_config_complete() {}
    virtual void device_validity_check(validity_checker &) const {}
    virtual void device_resolve_objects() {}   // MAME: once the machine is configured, before any device starts
    virtual void device_start() {}
    virtual void device_reset() {}
    virtual void device_reset_after_children() {}
    void reset();   // this device and its children, as MAME's device_t::reset
    virtual void device_stop() {}
    virtual void device_post_load() {}
    virtual void device_clock_changed() {}
    virtual void device_add_mconfig(machine_config &) {}
    virtual const tiny_rom_entry *device_rom_region() const { return nullptr; }
    virtual ioport_constructor device_input_ports() const { return nullptr; }

    // device_memory_interface (folded in)
    virtual space_config_vector memory_space_config() const { return {}; }
    enum { TR_READ, TR_WRITE, TR_FETCH };   // memory_translate() intentions
    virtual bool memory_translate(int, int, offs_t &, address_space *&) { return true; }
    // the space by number: cached once made (CPU cores ask for it on every access)
    address_space &space(int spacenum = 0) {
        if (unsigned(spacenum) < std::size(m_space_by_num) && m_space_by_num[spacenum]) [[likely]]
            return *m_space_by_num[spacenum];
        return make_space(spacenum);
    }
    address_space &make_space(int spacenum);
    bool has_space(int spacenum = 0) const;
    void set_addrmap(int spacenum, address_map_constructor map);
    bool has_configured_map(int spacenum = 0) const {
        for (auto &[n, m] : m_external_maps) if (n == spacenum) return true;
        return false;
    }
    // HIRAKU: after start, and before and after reset (MAME's device_execute_interface work there)
    virtual void hk_post_start() {}
    virtual void hk_pre_reset() {}
    virtual void hk_post_reset() {}

    std::vector<device_t *> m_children;
    std::vector<device_interface *> m_interfaces;
    void start();   // MAME's device_t::start: interface pre-start, device_start, post-start
    bool has_running_machine() const { return m_machine != nullptr; }
    static bool s_log;

protected:
    void hk_log(const char *s) const;

private:
    friend class running_machine;
    friend void hk_register_device(machine_config &, device_t *);
    device_type m_type;
    std::string m_tag, m_basetag;
    device_t *m_owner;
    u32 m_clock;
    running_machine *m_machine;
    const machine_config *m_mconfig;
    std::vector<std::pair<int, address_map_constructor>> m_external_maps;
    std::vector<std::unique_ptr<address_space>> m_spaces;
public:
    // spaces a device interface adds (MAME's device_rom_interface): used when memory_space_config()
    // doesn't name the space
    std::vector<std::pair<int, const address_space_config *>> m_hk_interface_spaces;
private:
    address_space *m_space_by_num[4] = {};
};

// ---- finders ----------------------------------------------------------------------------
template <class T> class device_finder : public finder_base {
public:
    device_finder() = default;
    device_finder(device_t &owner, const char *tag) : finder_base(&owner, tag) {}

    void set_tag(device_t &dev) { m_target = dynamic_cast<T *>(&dev); m_link = nullptr; }
    // a tag, relative to the device being configured (MAME: the finder's base is that device)
    void set_tag(const char *tag) { m_owner = hk_config_current(m_owner); m_tag = tag; m_target = nullptr; m_link = nullptr; }
    template <class U> void set_tag(device_finder<U> &f) { m_link = [p = &f] { return p->device_ptr(); }; }
    template <class U> void set_tag(const device_finder<U> &f) { m_link = [p = &f] { return p->device_ptr(); }; }
    void set_target(T *t) { m_target = t; }

    // a finder told to follow another finder resolves to that one's device, lazily
    T *target() const {
        if (!m_target && m_link) m_target = dynamic_cast<T *>(m_link());
        if (!m_target && m_tag == DEVICE_SELF_OWNER && m_owner) m_target = dynamic_cast<T *>(hk_owner_of(m_owner));
        if (!m_target && m_owner && !m_tag.empty() && m_tag != DUMMY_TAG) m_target = dynamic_cast<T *>(hk_find_device(m_owner, m_tag.c_str()));
        return m_target;
    }
    device_t *device_ptr() const override { return dynamic_cast<device_t *>(target()); }
    T *operator->() const { return target(); }
    T &operator*() const { return *target(); }
    operator T *() const { return target(); }
    explicit operator bool() const { return target() != nullptr; }
    T *lookup() const { return target(); }
    bool found() const { return target() != nullptr; }

private:
    mutable T *m_target = nullptr;
    std::function<device_t *()> m_link;
};

template <class T> using required_device = device_finder<T>;
template <class T> using optional_device = device_finder<T>;

template <class T, unsigned N> class device_array_finder : public std::array<device_finder<T>, N> {
public:
    device_array_finder(device_t &owner, const char *fmt, unsigned start) {
        for (unsigned i = 0; i < N; i++) {
            char buf[64];
            snprintf(buf, sizeof buf, fmt, start + i);
            (*this)[i] = device_finder<T>(owner, buf);
        }
    }
};
template <class T, unsigned N> using required_device_array = device_array_finder<T, N>;
template <class T, unsigned N> using optional_device_array = device_array_finder<T, N>;

// MAME's device_interface_enumerator<T>: every device interface of type T in the machine
template <class T> class device_interface_enumerator {
public:
    device_interface_enumerator(device_t &root);
    auto begin() { return m_list.begin(); }
    auto end() { return m_list.end(); }
    T *first() { return m_list.empty() ? nullptr : m_list.front(); }
    int count() const { return (int)m_list.size(); }
    struct iter_ref {};
private:
    std::vector<T *> m_list;
};

template <class T> struct device_type_enumerator {
    device_type_enumerator(device_t &) {}
    T *begin() { return nullptr; }
    T *end() { return nullptr; }
};

template <class D>
template <typename F, typename... A>
D &device_creator<D>::operator()(machine_config &config, F &&finder, A &&...args) const {
    device_t *owner = config.m_owner;
    D *dev;
    if constexpr (std::is_convertible_v<F, const char *>) {
        dev = new D(config, finder, owner, std::forward<A>(args)...);
    } else {
        dev = new D(config, finder.m_tag.c_str(), owner, std::forward<A>(args)...);
        finder.set_target(dev);
    }
    hk_register_device(config, dev);
    return *dev;
}

template <class D>
device_t *device_creator<D>::create(machine_config &config, const char *tag, u32 clock) const {
    if constexpr (std::is_constructible_v<D, const machine_config &, const char *, device_t *, u32>) {
        D *dev = new D(config, tag, config.m_owner, clock);
        hk_register_device(config, dev);
        return dev;
    } else {
        fatalerror("%s: no (config, tag, owner, clock) constructor", shortname());
    }
}

// ---- emu_timer --------------------------------------------------------------------------
class emu_timer {
public:
    void adjust(attotime duration, s32 param = 0, attotime period = attotime::never);
    bool enable(bool on = true);
    bool enabled() const { return m_enabled; }
    attotime remaining() const;
    attotime elapsed() const;
    attotime expire() const { return m_expire; }
    attotime start() const { return m_start; }
    s32 param() const { return m_param; }
    void set_param(s32 p) { m_param = p; }
    void reset(attotime duration = attotime::never) { adjust(duration, m_param, m_period); }

    running_machine *m_machine = nullptr;
    std::function<void(s32)> m_cb;
    attotime m_expire = attotime::never, m_period = attotime::never, m_start;
    s32 m_param = 0;
    bool m_enabled = false;
    bool m_temporary = false;   // a synchronize() timer: freed after it fires
};

// ---- device interfaces ----------------------------------------------------------------------
class device_nvram_interface {
public:
    device_nvram_interface(const machine_config &, device_t &device, bool backup_enabled = true) : m_nvram_device(&device), m_backup(backup_enabled) {}
    virtual ~device_nvram_interface() = default;
    void nvram_enable_backup(bool on) { m_backup = on; }
    bool nvram_backup_enabled() const { return m_backup; }
    void nvram_reset() { nvram_default(); }
    bool nvram_load(util::read_stream &f) { return nvram_read(f); }
    bool nvram_save(util::write_stream &f) { return nvram_write(f); }
    bool nvram_can_save() const { return m_backup && nvram_can_write(); }
    virtual void nvram_default() {}
    virtual bool nvram_read(util::read_stream &) { return true; }
    virtual bool nvram_write(util::write_stream &) { return true; }
    virtual bool nvram_can_write() const { return true; }
    device_t &nvram_device() { return *m_nvram_device; }

private:
    device_t *m_nvram_device;
    bool m_backup = false;
};

// MAME's sound_stream (the 2025 sound system): samples are indexed from time zero;
// update() generates up to and including the sample of the current time. With the
// exact scheduler, every stream is also brought up to date 50 times a second (MAME's
// sound_manager::update). The produced samples collect in m_out for the host.
class sound_stream {
public:
    using sample_t = float;
    sound_stream(device_t &dev, int inputs, int outputs, u32 rate, std::function<void(sound_stream &)> cb)
        : m_device(&dev), m_inputs(inputs), m_outputs(outputs), m_sample_rate(rate), m_cb(std::move(cb)), m_buf(outputs), m_out(outputs) {}
    u32 sample_rate() const { return m_sample_rate; }
    void set_sample_rate(u32 rate) { m_sample_rate = rate; }
    attotime sample_period() const { return attotime::from_hz(m_sample_rate); }
    int input_count() const { return m_inputs; }
    int output_count() const { return m_outputs; }
    s32 samples() const { return m_samples_to_update; }
    u64 start_index() const { return m_write_sample; }
    u64 end_index() const { return m_write_sample + samples(); }
    attotime sample_to_time(u64 index) const;
    attotime start_time() const { return sample_to_time(start_index()); }
    attotime end_time() const { return sample_to_time(end_index()); }
    void update();
    // HIRAKU: how many samples the stream holds once updated to `t` (update() brings it to its
    // index at the machine's time, that sample included)
    u64 index_at(const attotime &t) const;

    void put(int out, int index, float v) { m_buf[out][index] = v; }
    void put_int(int out, int index, s32 v, s32 max) { put(out, index, float(double(v) / max)); }
    void put_int_clamp(int out, int index, s32 v, s32 maxclamp) { put_int(out, index, std::clamp(v, -maxclamp, maxclamp - 1), maxclamp); }
    void add(int out, int index, float v) { m_buf[out][index] += v; }
    void add_int(int out, int index, s32 v, s32 max) { add(out, index, float(double(v) / max)); }
    void fill(int out, float v) { std::fill(m_buf[out].begin(), m_buf[out].begin() + samples(), v); }
    float get(int /*input*/, int /*index*/) const { return 0.0f; }   // no stream inputs are routed here

    device_t *m_device;
    int m_inputs, m_outputs;
    u32 m_sample_rate;
    std::function<void(sound_stream &)> m_cb;
    u64 m_write_sample = 0;
    s32 m_samples_to_update = 0;
    bool m_in_update = false;
    std::vector<std::vector<float>> m_buf;   // this update's samples
    std::vector<std::vector<float>> m_out;   // HIRAKU: everything produced so far, until the host takes it
};
enum sound_stream_flags : u32 { STREAM_DEFAULT_FLAGS = 0, STREAM_SYNCHRONOUS = 1 };
class device_sound_interface {
public:
    device_sound_interface(const machine_config &, device_t &dev) : m_sound_device(&dev) {}
    virtual ~device_sound_interface() = default;
    virtual void sound_stream_update(sound_stream &) {}
    sound_stream *stream_alloc(int inputs, int outputs, u32 rate, sound_stream_flags flags = STREAM_DEFAULT_FLAGS);
    sound_stream *stream_alloc(int inputs, int outputs, u32 rate, int flags) { return stream_alloc(inputs, outputs, rate, sound_stream_flags(flags)); }
    device_sound_interface &add_route(int, device_t &, double, int = 0) { return *this; }
    // MAME's per-output gain (a CD drive's MODE SELECT volumes); applied by the host mix
    void set_output_gain(int output, float gain) {
        if (output < 0) { for (auto &g : m_output_gain) g = gain; return; }
        if ((size_t)output >= m_output_gain.size()) m_output_gain.resize(output + 1, 1.0f);
        m_output_gain[output] = gain;
    }
    float output_gain(int output) const { return (size_t)output < m_output_gain.size() ? m_output_gain[output] : 1.0f; }
    std::vector<float> m_output_gain = std::vector<float>(2, 1.0f);
    template <typename T> device_sound_interface &add_route(int, T &&, double, int = 0) { return *this; }
    std::vector<std::unique_ptr<sound_stream>> m_streams;
private:
    device_t *m_sound_device;
};
class device_mixer_interface : public device_sound_interface {
public:
    using device_sound_interface::device_sound_interface;
};

class cpu_device : public device_t {
public:
    cpu_device(const machine_config &mconfig, device_type type, const char *tag, device_t *owner, u32 clock)
        : device_t(mconfig, type, tag, owner, clock) {}

    // device_execute_interface
    virtual bool cpu_is_interruptible() const { return false; }
    virtual u32 execute_min_cycles() const noexcept { return 1; }
    virtual u32 execute_max_cycles() const noexcept { return 1; }
    virtual u32 execute_input_lines() const noexcept { return 0; }
    virtual bool execute_input_edge_triggered(int) const noexcept { return false; }
    virtual void execute_run() = 0;
    virtual void execute_set_input(int, int) {}
    virtual u64 execute_clocks_to_cycles(u64 clocks) const noexcept { return clocks; }
    virtual u64 execute_cycles_to_clocks(u64 cycles) const noexcept { return cycles; }

    void set_icountptr(int &ic) { m_icountptr = &ic; }
    // HIRAKU's frame-time report: the board's main CPU, or another (a sound board's)
    bool hk_main() const { return std::strcmp(basetag(), "maincpu") == 0; }
    u64 total_cycles() const { return m_executing ? m_totalcycles + (u64)(m_cycles_running - *m_icountptr) : m_totalcycles; }
    attotime local_time() const;
    u64 cycles_per_second() const { return execute_clocks_to_cycles(clock()); }
    // as MAME: through the device clock (device_t::clocks_to_attotime)
    // MAME's (through the device clock) with the exact scheduler; the simple one keeps
    // whole cycles times the cycle length, which the XT446's recorded runs were made with
    attotime cycles_to_attotime(u64 cycles) const;
    u64 attotime_to_cycles(attotime t) const { return execute_clocks_to_cycles(attotime_to_clocks(t)); }
    // the scheduler's cycle length (MAME's m_attoseconds_per_cycle)
    attoseconds_t attoseconds_per_cycle() const { return HZ_TO_ATTOSECONDS(cycles_per_second()); }

    // MAME's set_input_line: through an event queue emptied by a synchronize() timer
    // (the exact scheduler); applied at once otherwise
    void set_input_line(int line, int state);
    bool suspended(u32 reason = ~0u) const { return (m_suspend & reason) != 0; }
    void suspend(u32 reason, bool /*eatcycles*/) { m_suspend |= reason; }
    void resume(u32 reason) { m_suspend &= ~reason; }
    void abort_timeslice();
    void adjust_icount(int delta) { *m_icountptr += delta; }
    void eat_cycles(int cycles) { if (m_executing) { if (cycles > *m_icountptr) cycles = *m_icountptr; *m_icountptr -= cycles; } }
    bool executing() const { return m_executing; }
    // MAME's redone accesses (devcpu.cpp): only CPUs with interruptible accesses (the 68000) see them
    bool access_to_be_redone() noexcept { return std::exchange(m_access_to_be_redone, false); }
    bool access_to_be_redone_noeat() noexcept { return false; }
    bool access_to_be_redone_noclear() noexcept { return m_access_to_be_redone; }
    bool access_before_time(u64 access_time, u64 current_time) noexcept {
        s32 delta = s32(access_time - current_time);
        if (*m_icountptr <= delta) { defer_access(); return true; }
        *m_icountptr -= delta;
        return false;
    }
    void access_after_delay(u32 cycles) noexcept { *m_icountptr -= cycles; }
    void defer_access() noexcept { if (*m_icountptr > 0) *m_icountptr = 0; m_access_to_be_redone = true; }
    void retry_access() noexcept { abort_timeslice(); m_access_to_be_redone = true; }
    bool m_access_to_be_redone = false;
    virtual space_config_vector memory_logical_space_config() const { return memory_space_config(); }
    // MAME's set_periodic_int: a timer, armed at reset, calling cb while the CPU isn't held
    void set_periodic_int(std::function<void()> cb, const attotime &rate) { m_periodic_cb = std::move(cb); m_periodic_rate = rate; }
    void hk_post_start() override;
    void hk_pre_reset() override;
    void hk_post_reset() override;
    std::function<void()> m_periodic_cb;
    attotime m_periodic_rate;
    emu_timer *m_periodic_timer = nullptr;
    int standard_irq_callback(int, offs_t) { return 0; }
    bool debugger_enabled() const { return false; }
    // HIRAKU: the last instruction's PC, and an optional per-instruction trace
    void debugger_privilege_hook() {}
    void debugger_instruction_hook(offs_t pc) { m_hook_pc = pc; if (m_hk_trace) [[unlikely]] m_hk_trace(pc); }
    std::function<void(offs_t)> m_hk_trace;
    void debugger_exception_hook(int) {}
    void debugger_wait_hook() {}
    void debugger_interrupt_hook(int, offs_t = 0) {}
    bool allow_drc() const { return false; }
    offs_t pc() const { return m_pc_for_log ? m_pc_for_log() : m_hook_pc; }
    offs_t m_hook_pc = 0;

    template <typename T> device_state_entry_stub state_add(int, const char *, T &) { return {}; }
    template <typename T> device_state_entry_stub state_add(int, const std::string &, T &) { return {}; }
    template <typename T, typename G, typename S> device_state_entry_stub state_add(int, const char *, G &&, S &&) { return {}; }
    virtual void state_import(const device_state_entry &) {}
    virtual void state_export(const device_state_entry &) {}
    virtual void state_string_export(const device_state_entry &, std::string &) const {}
    virtual std::unique_ptr<util::disasm_interface> create_disassembler() { return nullptr; }

    // scheduler state
    int *m_icountptr = nullptr;
    u64 m_totalcycles = 0;
    int m_cycles_running = 0;
    int m_cycles_stolen = 0;
    bool m_executing = false;
    attotime m_localtime;
    u32 m_suspend = 0;
    int m_divshift = 0;          // MAME's cycle-count division: (delta >> divshift) / divisor
    u32 m_divisor = 0;
    std::function<offs_t()> m_pc_for_log;
    std::vector<std::pair<int, std::vector<int>>> m_input_queue;   // line, pending states
};

// ---- the machine: device tree, regions, scheduler ---------------------------------------------
class running_machine {
public:
    running_machine();
    ~running_machine();
    machine_config &config() { return m_config; }

    // build: devices created with TYPE(config(), ...) at the root, then start()
    void start();
    void reset();
    void run_until(attotime target);   // all CPUs and timers up to target (the simple scheduler)

    // MAME's own scheduler, exactly (device_scheduler::timeslice): CPUs run up to the
    // next timer, timers that are due fire. Set m_exact before start().
    bool m_exact = false;
    machine_phase m_phase = machine_phase::PREINIT;
    machine_phase phase() const { return m_phase; }
    time_t m_base_time = 0;   // the RTCs' time at power-on (MAME: the host's clock)
    // NVRAM files (MAME's nvram_load/nvram_save), named as MAME names them; "" = none
    std::string m_nvram_dir;
    void nvram_load();
    void nvram_save();       // at exit: every memory looked at, those that changed written, and waited for
    // While a game is played (HIRAKU's, shell/saves.h): the memories that changed since they were last
    // written, as they are at this moment, handed to the writer thread as one set. The small ones
    // (clock chips, EEPROMs) are looked at each time; the large ones (flash chips, 2 MiB each and
    // 50 MB of them on a 573) only after the board said one was written (m_nvram_large_written):
    // looking at them all takes the time of a frame.
    void nvram_save_now();
    bool m_nvram_large_written = false;
    std::function<void()> m_after_nvram_load;   // run after nvram_load at start (MAME sets the RTCs there)
    static std::string nvram_filename(device_t &dev);
    struct nvram_kept { uint64_t hash; size_t size; };
    std::map<std::string, nvram_kept> m_nvram_kept;   // by file: what the save folder has (unknown: not there)
    unsigned m_nvram_failed = 0;                      // the writer's failures seen so far
    void nvram_changed(std::vector<std::pair<std::string, std::string>> &out, bool all);
    void timeslice();
    emu_timer *synchronize(std::function<void(s32)> cb, s32 param = 0);
    void abort_timeslice();
    void timer_reinsert(emu_timer *t);   // after its expiry or enable changed

    attotime time() const;
    bool side_effects_disabled() const { return false; }
    struct hk_side_effects_guard {};   // MAME's disable_side_effects(): the debugger's peeks; none here
    hk_side_effects_guard disable_side_effects(bool = true) const { return {}; }
    std::string describe_context() const;
    machine_config::opts_t options() const { return {}; }
    // MAME's schedule_soft_reset (a watchdog): every device resets at the end of the timeslice
    void schedule_soft_reset();
    bool m_soft_reset_pending = false;
    hk_input_manager input() const { return {}; }
    void debug_break() {}
    u32 rand() {
        m_rand_seed = 1664525 * m_rand_seed + 1013904223;
        return (m_rand_seed >> 16) | (m_rand_seed << 16);
    }
    memory_region *add_region(const std::string &tag, std::vector<u8> data);
    memory_region *find_region(const std::string &tag);

    emu_timer *alloc_timer(std::function<void(s32)> cb);
    // MAME's machine().scheduler(): timers allocated by a delegate
    struct scheduler_ref {
        running_machine *m;
        template <typename D> emu_timer *timer_alloc(D &&d) { auto f = std::forward<D>(d); return m->alloc_timer([f](s32 p) { f(p); }); }
        template <typename D> void synchronize(D &&d, s32 param = 0) { auto f = std::forward<D>(d); m->synchronize([f](s32 p) { f(p); }, param); }
        attotime time() const { return m->time(); }
    };
    scheduler_ref scheduler() { return {this}; }

    std::vector<device_t *> m_devices;     // creation order (a pre-order walk of the tree)
    std::vector<cpu_device *> m_cpus;
    std::vector<std::unique_ptr<emu_timer>> m_timers;
    std::vector<std::unique_ptr<memory_region>> m_regions;
    std::vector<sound_stream *> m_streams;   // in allocation order (the 50 Hz update)
    attotime m_basetime;
    cpu_device *m_executing = nullptr;
    const emu_timer *m_firing = nullptr;
    attotime m_fire_time;

    // the exact scheduler's state
    std::vector<emu_timer *> m_active;     // enabled timers by expiry; equal times in insertion order
    attoseconds_t m_quantum = 0;
    emu_timer *m_callback_timer = nullptr;
    bool m_callback_timer_modified = false;
    attotime m_callback_timer_expire;

private:
    void execute_timers();
    machine_config m_config;
    u32 m_rand_seed = 0x9d14abd7;
};

template <class T> device_interface_enumerator<T>::device_interface_enumerator(device_t &root) {
    for (device_t *d : root.machine().m_devices)
        for (device_interface *i : d->m_interfaces)
            if (T *t = dynamic_cast<T *>(i)) m_list.push_back(t);
}

template <typename T> void memory_share_creator<T>::reg() {
    if (!m_registered) { hk_register_share(m_dev->subtag(m_tag.c_str()), m_data.data(), bytes()); m_registered = true; }
}
template <typename T> T *required_shared_ptr<T>::target() {
    if (!m_ptr) {
        auto [p, n] = hk_find_share(m_dev->subtag(m_tag.c_str()));
        if (!p) fatalerror("%s: no memory share %s", m_dev->tag(), m_tag.c_str());
        m_ptr = reinterpret_cast<T *>(p);
        m_bytes = n;
    }
    return m_ptr;
}

template <typename C> emu_timer *device_t::timer_alloc(void (C::*f)(s32), C *obj) {
    return machine().alloc_timer([f, obj](s32 p) { (obj->*f)(p); });
}

// ---- map entry handler plumbing -------------------------------------------------------------
namespace hk_mame {
template <typename T> struct fn_traits;
#define HK_FN_TRAITS(...)                                                                              \
    template <class C, typename R, typename... A> struct fn_traits<R (C::*)(A...) __VA_ARGS__> {          \
        using cls = C;                                                                                  \
        using ret = R;                                                                                  \
        static constexpr int nargs = sizeof...(A);                                                      \
        using args = std::tuple<A...>;                                                                  \
    };
HK_FN_TRAITS()
HK_FN_TRAITS(const)
HK_FN_TRAITS(noexcept)
HK_FN_TRAITS(const noexcept)
#undef HK_FN_TRAITS

template <typename D> device_t *as_device(D &&d, device_t *base = nullptr) {
    using T = std::remove_cv_t<std::remove_reference_t<D>>;
    if constexpr (std::is_convertible_v<D, const char *>) return hk_find_device(base, d);
    else if constexpr (std::is_base_of_v<finder_base, T>) return d.device_ptr();
    else if constexpr (std::is_pointer_v<T>) return d;
    else return &d;
}
}  // namespace hk_mame

template <class C, typename F> void address_map_entry::set_r(C *obj, F f) {
    using tr = hk_mame::fn_traits<F>;
    using R = typename tr::ret;
    m_rkind = HANDLER;
    m_rwidth = sizeof(R) * 8;
    if constexpr (tr::nargs == 0) m_rh = [obj, f](offs_t, u64) { return u64((obj->*f)()); };
    else if constexpr (tr::nargs == 1) m_rh = [obj, f](offs_t o, u64) { return u64((obj->*f)(o)); };
    else m_rh = [obj, f](offs_t o, u64 m) { return u64((obj->*f)(o, R(m))); };
}

template <class C, typename F> void address_map_entry::set_w(C *obj, F f) {
    using tr = hk_mame::fn_traits<F>;
    m_wkind = HANDLER;
    if constexpr (tr::nargs == 1) {
        using V = std::tuple_element_t<0, typename tr::args>;
        m_wwidth = sizeof(V) * 8;
        m_wh = [obj, f](offs_t, u64 d, u64) { (obj->*f)(V(d)); };
    } else if constexpr (tr::nargs == 2) {
        using V = std::tuple_element_t<1, typename tr::args>;
        m_wwidth = sizeof(V) * 8;
        m_wh = [obj, f](offs_t o, u64 d, u64) { (obj->*f)(o, V(d)); };
    } else {
        using V = std::tuple_element_t<1, typename tr::args>;
        m_wwidth = sizeof(V) * 8;
        m_wh = [obj, f](offs_t o, u64 d, u64 m) { (obj->*f)(o, V(d), V(m)); };
    }
}

template <typename RF> address_map_entry &address_map_entry::r(RF rf) {
    using C = typename hk_mame::fn_traits<RF>::cls;
    set_r(dynamic_cast<C *>(m_map.m_device), rf);
    return *this;
}
template <typename WF> address_map_entry &address_map_entry::w(WF wf) {
    using C = typename hk_mame::fn_traits<WF>::cls;
    set_w(dynamic_cast<C *>(m_map.m_device), wf);
    return *this;
}
template <typename D, typename RF> address_map_entry &address_map_entry::r(D &&dev, RF rf) {
    using C = typename hk_mame::fn_traits<RF>::cls;
    set_r(dynamic_cast<C *>(hk_mame::as_device(dev, m_map.m_device)), rf);
    return *this;
}
template <typename D, typename WF> address_map_entry &address_map_entry::w(D &&dev, WF wf) {
    using C = typename hk_mame::fn_traits<WF>::cls;
    set_w(dynamic_cast<C *>(hk_mame::as_device(dev, m_map.m_device)), wf);
    return *this;
}
template <typename D, typename F> address_map_entry &address_map_entry::before_time(D &&dev, F f) {
    using C = typename hk_mame::fn_traits<F>::cls;
    C *obj = dynamic_cast<C *>(hk_mame::as_device(dev, m_map.m_device));
    m_before_time = [obj, f](offs_t o, u64 t) { return u64((obj->*f)(o, t)); };
    return *this;
}
template <typename D, typename F> address_map_entry &address_map_entry::after_delay(D &&dev, F f) {
    using C = typename hk_mame::fn_traits<F>::cls;
    C *obj = dynamic_cast<C *>(hk_mame::as_device(dev, m_map.m_device));
    m_after_delay = [obj, f](offs_t o) { return u32((obj->*f)(o)); };
    return *this;
}
template <typename D, class C> address_map_entry &address_map_entry::m(D &&dev, void (C::*f)(address_map &)) {
    C *obj = dynamic_cast<C *>(hk_mame::as_device(dev, m_map.m_device));
    m_rkind = m_wkind = SUBMAP;
    m_subdev = obj;
    m_submap = address_map_constructor(f, obj);
    return *this;
}

#include "hk_osd.h"
#include "logmacro.h"
