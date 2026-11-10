// SPDX-License-Identifier: GPL-3.0-or-later
// The session cable between two System 573 processes, or a 573 and a Twinkle (link.h).
#include "sys573/link.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <thread>

#include "shell/net.h"

namespace hk::k573 {

namespace {

constexpr uint32_t MAGIC = 0x48'4b'35'37;   // "HK57"
constexpr uint32_t VERSION = 3;   // 2: sound with the steps; 3: whether a side sends sound

struct Hello {
    uint32_t magic, version;
    int64_t quantum_attos;
    uint32_t audio, pad;   // this side sends sound with its steps
};

struct StepHeader {
    uint64_t step;
    uint32_t count;          // line events
    uint32_t audio_frames;   // then 44.1 kHz stereo floats
};

// "host:port" (TCP) unless it looks like a path
bool is_tcp(const std::string &w) { return w.find_first_of("/\\") == std::string::npos && w.rfind(':') != std::string::npos; }

int tcp_socket(const std::string &w, bool host) {
    const size_t c = w.rfind(':');
    std::string name = w.substr(0, c), port = w.substr(c + 1);
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = host ? AI_PASSIVE : 0;
    net::start();
    if (getaddrinfo(name.empty() ? nullptr : name.c_str(), port.c_str(), &hints, &res) != 0) return -1;
    int fd = -1;
    for (addrinfo *a = res; a && fd < 0; a = a->ai_next) {
        fd = net::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        if (host) {
            net::set_int(fd, SOL_SOCKET, SO_REUSEADDR, 1);
            if (bind(fd, a->ai_addr, (int)a->ai_addrlen) == 0 && listen(fd, 1) == 0) break;
        } else if (connect(fd, a->ai_addr, (int)a->ai_addrlen) == 0) {
            net::set_int(fd, IPPROTO_TCP, TCP_NODELAY, 1);
            break;
        }
        net::close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

int unix_socket(const std::string &path, bool host) {
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    if (path.size() >= sizeof a.sun_path) return -1;
    memcpy(a.sun_path, path.c_str(), path.size() + 1);
    int fd = net::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (host) {
        net::remove_path(path);
        if (bind(fd, (sockaddr *)&a, sizeof a) == 0 && listen(fd, 1) == 0) return fd;
    } else if (connect(fd, (sockaddr *)&a, sizeof a) == 0) {
        return fd;
    }
    net::close(fd);
    return -1;
}

}  // namespace

bool SessionLink::open(const std::string &where, bool host, attotime quantum, bool audio) {
    m_host = host;
    m_quantum = quantum;
    const bool tcp = is_tcp(where);
    if (host) {
        int ls = tcp ? tcp_socket(where, true) : unix_socket(where, true);
        if (ls < 0) {
            fprintf(stderr, "session: can't listen at %s: %s\n", where.c_str(), net::error().c_str());
            return false;
        }
        fprintf(stderr, "session: waiting for the other cabinet at %s\n", where.c_str());
        m_fd = net::accept(ls);
        net::close(ls);
        if (!tcp) net::remove_path(where);
        if (m_fd >= 0 && tcp) net::set_int(m_fd, IPPROTO_TCP, TCP_NODELAY, 1);
    } else {
        for (int tries = 0; tries < 6000 && m_fd < 0; tries++) {   // ten minutes (regress: the host may still be booting)
            m_fd = tcp ? tcp_socket(where, false) : unix_socket(where, false);
            if (m_fd < 0) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    if (m_fd < 0) {
        fprintf(stderr, "session: no other cabinet at %s\n", where.c_str());
        return false;
    }
    Hello mine{MAGIC, VERSION, (int64_t)quantum.as_attoseconds(), audio ? 1u : 0u, 0}, theirs{};
    if (!send_all(&mine, sizeof mine) || !recv_all(&theirs, sizeof theirs) || theirs.magic != MAGIC ||
        theirs.version != VERSION || theirs.quantum_attos != mine.quantum_attos) {
        fprintf(stderr, "session: the other side isn't a matching HIRAKU session cable (version or quantum)\n");
        net::close(m_fd);
        m_fd = -1;
        return false;
    }
    m_far_audio = theirs.audio != 0;
    fprintf(stderr, "session: connected (%s, %.3f ms quantum)\n", host ? "host" : "guest", quantum.as_double() * 1e3);
    return true;
}

void SessionLink::attach(running_machine &m) {
    m_machine = &m;
    if (m_fd < 0) return;
    m_sync_timer = m.alloc_timer([this](s32 p) { sync(p); });
    m_deliver_timer = m.alloc_timer([this](s32 p) { deliver(p); });
    m_sync_timer->adjust(m_quantum, 0, m_quantum);
}

void SessionLink::out(Line line, int state) {
    state = state ? 1 : 0;
    // every TXD call, not only changes: the fork's SIO1 (psxsio1_uart_device::write_rxd) takes a
    // bit per call, as the UARTs' bit clocks call it on a wire; DTR and RTS only when they change
    if (m_fd < 0 || (line != TXD && m_last[line] == state)) return;
    m_last[line] = state;
    const attotime t = m_machine ? m_machine->time() : attotime::zero;
    static const bool trace = getenv("HK_SESSION_LOG") != nullptr;
    if (trace) fprintf(stderr, "session out %d %d at %.9f\n", line, state, t.as_double());
    // HK_SESSION_BYTES=1: the bytes sent, from TXD's bits (a call a bit: a start bit, eight data
    // bits, then the next 1 ends it; parity, if any, is not told apart)
    static const bool bytes = getenv("HK_SESSION_BYTES") != nullptr;
    if (bytes && line == TXD) {
        if (m_tx_bits < 0) {
            if (!state) m_tx_bits = 0, m_tx_byte = 0;
        } else if (m_tx_bits < 8) {
            m_tx_byte |= state << m_tx_bits++;
        } else if (state) {
            fprintf(stderr, "session tx %02x at %.6f\n", m_tx_byte, t.as_double());
            m_tx_bits = -1;
        }
    }
    m_out.push_back({t.seconds(), t.attoseconds(), (uint8_t)line, (uint8_t)state});
}

// At each quantum boundary T: send our changes before T, take theirs (before T, as they had them
// at their T), to deliver a quantum later.
void SessionLink::sync(s32) {
    if (m_fd < 0) return;
    const attotime now = m_machine->time();
    m_step++;
    std::vector<Event> send;
    size_t keep = 0;
    for (const Event &e : m_out) {
        if (attotime(e.seconds, e.attoseconds) < now) send.push_back(e);
        else m_out[keep++] = e;   // after T (the CPU ran a little past it): next time
    }
    m_out.resize(keep);
    StepHeader h{m_step, (uint32_t)send.size(), (uint32_t)(m_audio_out.size() / 2)};
    if (!send_all(&h, sizeof h) || (!send.empty() && !send_all(send.data(), send.size() * sizeof(Event))) ||
        (!m_audio_out.empty() && !send_all(m_audio_out.data(), m_audio_out.size() * sizeof(float))))
        return lost("send");
    m_audio_out.clear();
    StepHeader th{};
    if (!recv_all(&th, sizeof th)) return lost("closed");
    if (th.step != m_step) return lost("out of step");
    // (a step is a quantum's line changes and sound: a header asking for more than any cabinet sends
    // isn't one, and isn't given the memory it names)
    if (th.count > 0x10000 || th.audio_frames > 0x20000) return lost("not a step");
    std::vector<Event> got(th.count);
    if (th.count && !recv_all(got.data(), got.size() * sizeof(Event))) return lost("closed");
    if (th.audio_frames) {
        if (m_audio_next) {
            m_audio_in.erase(m_audio_in.begin(), m_audio_in.begin() + (ptrdiff_t)m_audio_next);
            m_audio_next = 0;
        }
        const size_t at = m_audio_in.size();
        m_audio_in.resize(at + (size_t)th.audio_frames * 2);
        if (!recv_all(m_audio_in.data() + at, (size_t)th.audio_frames * 2 * sizeof(float))) return lost("closed");
    }
    if (m_in_next) {
        m_in.erase(m_in.begin(), m_in.begin() + (ptrdiff_t)m_in_next);
        m_in_next = 0;
    }
    for (Event &e : got) {
        const attotime at = attotime(e.seconds, e.attoseconds) + m_quantum;
        e.seconds = at.seconds();
        e.attoseconds = at.attoseconds();
        m_in.push_back(e);
    }
    arm_delivery();
}

void SessionLink::arm_delivery() {
    if (m_in_next >= m_in.size()) return;
    const Event &e = m_in[m_in_next];
    attotime at(e.seconds, e.attoseconds), now = m_machine->time();
    m_deliver_timer->adjust(at > now ? at - now : attotime::zero);
}

void SessionLink::deliver(s32) {
    const attotime now = m_machine->time();
    while (m_in_next < m_in.size() && !(now < attotime(m_in[m_in_next].seconds, m_in[m_in_next].attoseconds))) {
        const Event &e = m_in[m_in_next++];
        static const bool trace = getenv("HK_SESSION_LOG") != nullptr;
        if (trace) fprintf(stderr, "session in %d %d at %.9f (now %.9f)\n", e.line, e.state, attotime(e.seconds, e.attoseconds).as_double(), now.as_double());
        if (in) in((Line)e.line, e.state);
    }
    arm_delivery();
}

void SessionLink::send_audio(const float *lr, size_t frames) {
    if (m_fd >= 0) m_audio_out.insert(m_audio_out.end(), lr, lr + frames * 2);
}

size_t SessionLink::mix_audio(float *lr, size_t frames) {
    const size_t n = std::min(frames, (m_audio_in.size() - m_audio_next) / 2);
    const float *src = m_audio_in.data() + m_audio_next;
    for (size_t i = 0; i < n * 2; i++) lr[i] += src[i];
    m_audio_next += n * 2;
    return n;
}

bool SessionLink::send_all(const void *p, size_t n) {
    const char *c = (const char *)p;
    while (n) {
        ssize_t k = net::send(m_fd, c, n);
        if (k < 0 && net::interrupted()) continue;
        if (k <= 0) return false;
        c += k;
        n -= (size_t)k;
    }
    return true;
}

bool SessionLink::recv_all(void *p, size_t n) {
    char *c = (char *)p;
    while (n) {
        ssize_t k = net::recv(m_fd, c, n);
        if (k < 0 && net::interrupted()) continue;
        if (k <= 0) return false;
        c += k;
        n -= (size_t)k;
    }
    return true;
}

void SessionLink::lost(const char *why) {
    fprintf(stderr, "session: the other cabinet is gone (%s) at %.3f s; the cable is idle now\n", why,
            m_machine ? m_machine->time().as_double() : 0.0);
    net::close(m_fd);
    m_fd = -1;
    if (m_sync_timer) m_sync_timer->adjust(attotime::never);
}

}  // namespace hk::k573
