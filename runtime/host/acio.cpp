// SPDX-License-Identifier: GPL-3.0-or-later
// After spice2x's reader/ (reader.cpp, message.cpp: GPLv3, the sp2xdev team and the SpiceTools
// contributors), on POSIX serial ports.
#include "acio.h"
#include "../shell/sys.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace hk::host {

AcioPort::AcioPort(std::string path) : path_(std::move(path)), log_(getenv("HK_ACIO_LOG") != nullptr) {}

AcioPort::~AcioPort() { close(); }

#ifdef _WIN32
// Windows: a COM port ("COM3"; the \\.\ in front, which COM10 and up need, is added) through
// Win32's serial calls. Not yet
// tried on a real port (PLAN.md section 26).
namespace {
HANDLE handle(intptr_t fd) { return reinterpret_cast<HANDLE>(fd); }

// a read returns when a byte is there, or after ms with none
void read_wait(intptr_t fd, int ms) {
    COMMTIMEOUTS t{};
    t.ReadIntervalTimeout = MAXDWORD;
    t.ReadTotalTimeoutMultiplier = MAXDWORD;
    t.ReadTotalTimeoutConstant = (DWORD)std::max(ms, 1);
    t.WriteTotalTimeoutConstant = 1000;
    SetCommTimeouts(handle(fd), &t);
}
}  // namespace

bool AcioPort::open() {
    close();
    const std::string name = path_.compare(0, 4, "\\\\.\\") == 0 ? path_ : "\\\\.\\" + path_;
    HANDLE h = CreateFileA(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    id_ = 0;
    if (h == INVALID_HANDLE_VALUE) return false;
    fd_ = reinterpret_cast<intptr_t>(h);
    return true;
}

void AcioPort::close() {
    if (fd_ >= 0) CloseHandle(handle(fd_));
    fd_ = -1;
}

bool AcioPort::set_rate(int baud) {
    DCB d{};
    d.DCBlength = sizeof d;
    if (!GetCommState(handle(fd_), &d)) return false;
    d.BaudRate = (DWORD)baud;
    d.ByteSize = 8;   // 8N1, no flow control
    d.Parity = NOPARITY;
    d.StopBits = ONESTOPBIT;
    d.fBinary = TRUE;
    d.fParity = FALSE;
    d.fOutxCtsFlow = FALSE;
    d.fOutxDsrFlow = FALSE;
    d.fDtrControl = DTR_CONTROL_ENABLE;
    d.fRtsControl = RTS_CONTROL_ENABLE;
    d.fOutX = FALSE;
    d.fInX = FALSE;
    if (!SetCommState(handle(fd_), &d)) return false;
    PurgeComm(handle(fd_), PURGE_RXCLEAR | PURGE_TXCLEAR);
    return true;
}

bool AcioPort::write_all(const uint8_t *d, size_t n) {
    read_wait(fd_, 1);
    while (n) {
        DWORD w = 0;
        if (!WriteFile(handle(fd_), d, (DWORD)n, &w, nullptr) || w == 0) return false;
        d += w;
        n -= w;
    }
    return true;
}

std::vector<uint8_t> AcioPort::read_chunk(int wait_ms) {
    read_wait(fd_, wait_ms);
    uint8_t b[512];
    DWORD r = 0;
    if (!ReadFile(handle(fd_), b, sizeof b, &r, nullptr)) r = 0;
    return std::vector<uint8_t>(b, b + r);
}

void AcioPort::line_break(int ms) {
    SetCommBreak(handle(fd_));
    hk::sys::sleep_for(std::chrono::milliseconds(ms));
    ClearCommBreak(handle(fd_));
}

std::vector<uint8_t> AcioPort::read_some(int first_ms, size_t most) {
    std::vector<uint8_t> in;
    int wait = first_ms;
    while (in.size() < most) {
        read_wait(fd_, wait);
        uint8_t b[512];
        DWORD r = 0;
        if (!ReadFile(handle(fd_), b, (DWORD)std::min(sizeof b, most - in.size()), &r, nullptr) || r == 0) break;
        in.insert(in.end(), b, b + r);
        wait = 30;
    }
    return in;
}

#else

bool AcioPort::open() {
    close();
    fd_ = ::open(path_.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    id_ = 0;
    return fd_ >= 0;
}

void AcioPort::close() {
    if (fd_ >= 0) ::close((int)fd_);
    fd_ = -1;
}

bool AcioPort::set_rate(int baud) {
    speed_t rate;
    switch (baud) {
    case 9600: rate = B9600; break;
    case 19200: rate = B19200; break;
    case 38400: rate = B38400; break;
    case 57600: rate = B57600; break;
    case 115200: rate = B115200; break;
    default: return false;
    }
    termios t{};
    const int fd = (int)fd_;
    if (tcgetattr(fd, &t) != 0) return false;
    cfmakeraw(&t);
    cfsetispeed(&t, rate);
    cfsetospeed(&t, rate);
    t.c_cflag |= CLOCAL | CREAD;
    t.c_cflag &= ~(CSTOPB | PARENB | CRTSCTS);   // 8N1, no flow control
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &t) != 0) return false;
    tcflush(fd, TCIOFLUSH);
    return true;
}

bool AcioPort::write_all(const uint8_t *d, size_t n) {
    while (n) {
        ssize_t w = write((int)fd_, d, n);
        if (w < 0 && errno == EAGAIN) {
            pollfd p{(int)fd_, POLLOUT, 0};
            if (poll(&p, 1, 1000) <= 0) return false;
            continue;
        }
        if (w <= 0) return false;
        d += w;
        n -= (size_t)w;
    }
    return true;
}

std::vector<uint8_t> AcioPort::read_chunk(int wait_ms) {
    pollfd p{(int)fd_, POLLIN, 0};
    if (poll(&p, 1, wait_ms) <= 0) return {};
    uint8_t b[512];
    ssize_t r = read((int)fd_, b, sizeof b);
    return r > 0 ? std::vector<uint8_t>(b, b + r) : std::vector<uint8_t>();
}

void AcioPort::line_break(int ms) {
    ioctl((int)fd_, TIOCSBRK);
    hk::sys::sleep_for(std::chrono::milliseconds(ms));
    ioctl((int)fd_, TIOCCBRK);
}

std::vector<uint8_t> AcioPort::read_some(int first_ms, size_t most) {
    std::vector<uint8_t> in;
    int wait = first_ms;
    while (in.size() < most) {
        pollfd p{(int)fd_, POLLIN, 0};
        if (poll(&p, 1, wait) <= 0) break;
        uint8_t b[512];
        ssize_t r = read((int)fd_, b, std::min(sizeof b, most - in.size()));
        if (r <= 0) break;
        in.insert(in.end(), b, b + r);
        wait = 30;
    }
    return in;
}

#endif

void AcioPort::dump(const char *dir, const std::vector<uint8_t> &bytes) const {
    if (!log_) return;
    fprintf(stderr, "acio %s %s", path_.c_str(), dir);
    for (uint8_t b : bytes) fprintf(stderr, " %02x", b);
    fprintf(stderr, "\n");
}

int AcioPort::handshake(const std::vector<int> &rates, const std::function<bool()> &keep_going) {
    std::vector<uint8_t> hello(525, 0x00);
    hello.insert(hello.end(), 40, 0xaa);
    for (int rate : rates) {
        if (!set_rate(rate)) return 0;
        for (int n = 0; n < 10 && keep_going(); n++) {
            if (!write_all(hello.data(), hello.size())) return 0;
            auto in = read_some(300, 565);
            if (!in.empty() && in.back() == 0xaa) return rate;
            hk::sys::sleep_for(std::chrono::milliseconds(50));
        }
    }
    return 0;
}

bool AcioPort::handshake_bio2(const std::function<bool()> &keep_going) {
    if (!set_rate(115200)) return false;
    line_break(250);
    hk::sys::sleep_for(std::chrono::milliseconds(200));
    const std::vector<uint8_t> sync(10, 0xaa);
    for (int n = 0; n < 100 && keep_going(); n++) {
        if (!write_all(sync.data(), sync.size())) return false;
        int got = 0;
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
        while (got < 10 && std::chrono::steady_clock::now() < end)
            for (uint8_t b : read_chunk(10)) got = b == 0xaa ? got + 1 : 0;
        if (got >= 10) return true;
    }
    return false;
}

namespace {

// The packets in what a port reads: 0xaa starts one, 0xff marks the next byte as sent inverted.
// A packet: node, command (2 bytes), id, length, payload, sum (of the rest).
struct Unframe {
    std::vector<uint8_t> d;   // the packet so far
    size_t want = 0;          // its length once known (0: outside a packet)
    bool escape = false;
    bool feed(uint8_t b) {   // true: d is a whole packet
        if (b == 0xaa && !escape) {
            d.clear();
            want = 6;
            return false;
        }
        if (!want) return false;
        if (b == 0xff && !escape) {
            escape = true;
            return false;
        }
        if (escape) b = (uint8_t)~b;
        escape = false;
        d.push_back(b);
        if (d.size() == 5) want = 6 + d[4];
        if (d.size() < want) return false;
        want = 0;
        return true;
    }
    bool sum_ok() const {
        uint8_t sum = 0;
        for (size_t i = 0; i + 1 < d.size(); i++) sum += d[i];
        return sum == d.back();
    }
    void to(AcioPort::Message &m) const {
        m.node = d[0];
        m.cmd = (uint16_t)(d[1] << 8 | d[2]);
        m.id = d[3];
        m.payload.assign(d.begin() + 5, d.end() - 1);
    }
};

}  // namespace

bool AcioPort::receive_one(Message &m, int wait_ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
    Unframe u;
    while (std::chrono::steady_clock::now() < end) {
        const int left = (int)std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now()).count();
        for (uint8_t b : read_chunk(std::max(left, 1))) {
            if (!u.feed(b)) continue;
            const bool ok = u.sum_ok();
            dump(ok ? "<" : "< (bad sum)", u.d);
            if (ok && u.d != last_sent_) {
                u.to(m);
                return true;
            }
        }
    }
    return false;
}

bool AcioPort::send(uint8_t node, uint16_t cmd, const std::vector<uint8_t> &payload) {
    std::vector<uint8_t> body = {node, uint8_t(cmd >> 8), uint8_t(cmd), tag_zero_ ? uint8_t(0) : ++id_, (uint8_t)payload.size()};
    body.insert(body.end(), payload.begin(), payload.end());
    uint8_t sum = 0;
    for (uint8_t b : body) sum += b;
    body.push_back(sum);
    dump(">", body);
    last_sent_ = body;
    std::vector<uint8_t> out = {0xaa};
    for (uint8_t b : body) {
        if (b == 0xaa || b == 0xff) {
            out.push_back(0xff);
            out.push_back((uint8_t)~b);
        } else {
            out.push_back(b);
        }
    }
    return write_all(out.data(), out.size());
}

bool AcioPort::receive(Message &m, int wait_ms) {
    Unframe u;
    bool got = false;
    for (uint8_t b : read_some(wait_ms, 4096)) {
        if (!u.feed(b)) continue;
        const bool ok = u.sum_ok();
        dump(ok ? "<" : "< (bad sum)", u.d);
        if (ok) u.to(m), got = true;
    }
    return got;
}

}  // namespace hk::host
