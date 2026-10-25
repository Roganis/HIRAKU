// SPDX-License-Identifier: GPL-3.0-or-later
// NET TAISEN for the PC pop'n games (pop'n 15: docs/EAMUSE3.md, "NET TAISEN"): the cabinets'
// network, carried by HIRAKU's relay (runtime/python/netrelay.h, the one pop'n 12-14 meet through
// online) instead of a shop's LAN and the internet.
//
// The game's battle network (libcomm, and globbyc for the lobby) does all it does through
// libsocket.dll, a thin layer over Winsock: 32 sockets by number, UDP or TCP, addresses as four
// bytes and a port, a receive that blocks or not, with a timeout, and no select. Its calls are
// taken here (cabinet.c hooks the imports of libsocket's exports) and served without any socket
// of the PC's: the DLL holds one TCP connection to the relay (HK_PC_RELAY="a.b.c.d:port", from
// hiraku-pc: hiraku-eamuse's with -url, else one of hiraku-pc's own), which gives the cabinet an
// address, 10.57.x.y. Then:
//   - a datagram to the lobby (the services' globby URL, port 5741: the PC games' lobby, apart
//     from pop'n 12-14's) goes to the relay as it is, and its answer comes back the same way;
//   - a datagram to another cabinet (10.57.x.y) goes to it through the relay;
//   - a TCP connection (the battle's MASTER takes the others' connections) is carried as messages
//     through the relay, which delivers them whole and in order: open, opened, data, closed.
// Everything else (a broadcast for a shop's LAN) goes nowhere. The game is told the relay's
// address is its own (avs_net_ctrl) and its router's (ea3_query's portfw address), so the lobby
// hands out addresses the relay knows.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cabinet.h"

#define LOBBY_PORT 5741          // runtime/python/lobby.h: Lobby::PC_PORT, one port a game from it
#define NSOCK 32                 // as libsocket
#define QUEUE_MAX 256            // datagrams kept for a socket that doesn't read them
enum { K_DGRAM, K_OPEN, K_OPENED, K_DATA, K_CLOSED, K_REFUSED };
#define K_FROM_HOST 0x80         // a stream's message from the side that accepted it

struct item {   // a datagram, or a stream's bytes
    struct item *next;
    uint32_t addr;
    uint16_t port;
    int size, at;
    unsigned char data[];
};

struct vsock {
    int used, stream;
    uint16_t port;               // bound (or given when first used)
    int nonblock, timeout_ms;    // libsocket's receive mode
    struct item *in, *in_last;   // datagrams, or the stream's bytes
    int queued;
    // a stream
    int listening, connected, peer_closed, refused, i_connected;
    uint32_t peer, id;
    uint16_t peer_port;
    struct item *pending, *pending_last;   // a listener's connections not yet accepted (addr, port, id)
};

static CRITICAL_SECTION g_lock;
static CONDITION_VARIABLE g_moved;
static struct vsock g_s[NSOCK];
static SOCKET g_relay = INVALID_SOCKET;
static uint32_t g_addr;          // ours, from the relay
static int g_state;              // 0 not tried, 1 connected, -1 none
static uint32_t g_next_id = 1;
static uint16_t g_next_port = 49152;
static int g_log = -1;

static int logging(void) {
    if (g_log < 0) g_log = getenv("HK_PC_NET_LOG") != NULL;
    return g_log;
}

static void init_once(void) {
    static LONG done;
    if (InterlockedCompareExchange(&done, 1, 0) == 0) {
        InitializeCriticalSection(&g_lock);
        InitializeConditionVariable(&g_moved);
    }
}

static uint32_t addr_of(const unsigned char *a) { return (uint32_t)a[0] << 24 | (uint32_t)a[1] << 16 | (uint32_t)a[2] << 8 | a[3]; }
static void put_addr(unsigned char *a, uint32_t v) { a[0] = (unsigned char)(v >> 24), a[1] = (unsigned char)(v >> 16), a[2] = (unsigned char)(v >> 8), a[3] = (unsigned char)v; }
static int is_cabinet(uint32_t a) { return (a >> 16) == 0x0a39; }   // 10.57.x.y

#pragma pack(push, 1)
struct hello { uint32_t magic, version, address; };
struct header { uint32_t src, dst; uint16_t sport, dport; uint32_t size; };
#pragma pack(pop)
#define MAGIC 0x484b5052u        // "HKPR"
#define VERSION 1u

static int send_all(const void *p, int n) {
    const char *c = (const char *)p;
    while (n > 0) {
        const int k = send(g_relay, c, n, 0);
        if (k <= 0) return 0;
        c += k, n -= k;
    }
    return 1;
}

static int recv_all(void *p, int n) {
    char *c = (char *)p;
    while (n > 0) {
        const int k = recv(g_relay, c, n, 0);
        if (k <= 0) return 0;
        c += k, n -= k;
    }
    return 1;
}

// One message to the relay (the lock held: one writer at a time)
static void relay_send(uint32_t dst, uint16_t sport, uint16_t dport, int kind, uint32_t id, const void *data, int size) {
    if (g_state != 1) return;
    unsigned char pre[5];
    int npre = 0;
    if (kind >= 0) {   // between cabinets: what it is, and the stream's id
        pre[0] = (unsigned char)kind;
        memcpy(pre + 1, &id, 4);
        npre = (kind & 0x7f) == K_DGRAM ? 1 : 5;
    }
    struct header h = {0, dst, sport, dport, (uint32_t)(npre + size)};
    if (!send_all(&h, sizeof h) || (npre && !send_all(pre, npre)) || (size && !send_all(data, size))) {
        hk_log("hiraku-pc: the NET TAISEN relay is gone");
        closesocket(g_relay);
        g_relay = INVALID_SOCKET;
        g_state = -1;
        WakeAllConditionVariable(&g_moved);
    }
}

static struct item *new_item(uint32_t addr, uint16_t port, const void *data, int size) {
    struct item *it = (struct item *)malloc(sizeof *it + (size_t)(size > 0 ? size : 0));
    if (!it) return NULL;
    it->next = NULL, it->addr = addr, it->port = port, it->size = size, it->at = 0;
    if (size > 0) memcpy(it->data, data, (size_t)size);
    return it;
}

static void push(struct item **first, struct item **last, struct item *it) {
    if (!it) return;
    if (*last) (*last)->next = it;
    else *first = it;
    *last = it;
}

static struct item *pop(struct item **first, struct item **last) {
    struct item *it = *first;
    if (it) {
        *first = it->next;
        if (!*first) *last = NULL;
    }
    return it;
}

static struct vsock *stream_of(uint32_t peer, uint32_t id, int i_connected) {
    for (int i = 0; i < NSOCK; i++)
        if (g_s[i].used && g_s[i].stream && g_s[i].peer == peer && g_s[i].id == id && g_s[i].i_connected == i_connected &&
            !g_s[i].listening)
            return &g_s[i];
    return NULL;
}

// What came from the relay, put where the game will read it (the lock held)
static void arrived(const struct header *h, const unsigned char *d, uint32_t n) {
    if (!is_cabinet(h->src)) {   // the lobby's answer, as it is
        for (int i = 0; i < NSOCK; i++)
            if (g_s[i].used && !g_s[i].stream && g_s[i].port == h->dport && g_s[i].queued < QUEUE_MAX) {
                push(&g_s[i].in, &g_s[i].in_last, new_item(h->src, h->sport, d, (int)n));
                g_s[i].queued++;
                break;
            }
        return;
    }
    if (n < 1) return;
    const int kind = d[0] & 0x7f, from_host = (d[0] & K_FROM_HOST) != 0;
    if (kind == K_DGRAM) {
        for (int i = 0; i < NSOCK; i++)
            if (g_s[i].used && !g_s[i].stream && g_s[i].port == h->dport && g_s[i].queued < QUEUE_MAX) {
                push(&g_s[i].in, &g_s[i].in_last, new_item(h->src, h->sport, d + 1, (int)n - 1));
                g_s[i].queued++;
                break;
            }
        return;
    }
    if (n < 5) return;
    uint32_t id;
    memcpy(&id, d + 1, 4);
    if (kind == K_OPEN) {   // to a listener on that port
        for (int i = 0; i < NSOCK; i++)
            if (g_s[i].used && g_s[i].stream && g_s[i].listening && g_s[i].port == h->dport) {
                struct item *it = new_item(h->src, h->sport, &id, 4);
                push(&g_s[i].pending, &g_s[i].pending_last, it);
                return;
            }
        relay_send(h->src, h->dport, h->sport, K_REFUSED | K_FROM_HOST, id, NULL, 0);
        return;
    }
    // the stream's own end: ours connected when the message is from the host
    struct vsock *s = stream_of(h->src, id, from_host);
    if (!s) return;
    if (kind == K_OPENED) s->connected = 1;
    else if (kind == K_REFUSED) s->refused = 1;
    else if (kind == K_CLOSED) s->peer_closed = 1;
    else if (kind == K_DATA && n > 5) push(&s->in, &s->in_last, new_item(h->src, h->sport, d + 5, (int)n - 5));
}

static DWORD WINAPI reader(void *arg) {
    (void)arg;
    for (;;) {
        struct header h;
        if (!recv_all(&h, sizeof h) || h.size > 70000) break;
        unsigned char *d = (unsigned char *)malloc(h.size ? h.size : 1);
        if (!d || (h.size && !recv_all(d, (int)h.size))) { free(d); break; }
        EnterCriticalSection(&g_lock);
        arrived(&h, d, h.size);
        WakeAllConditionVariable(&g_moved);
        LeaveCriticalSection(&g_lock);
        free(d);
    }
    EnterCriticalSection(&g_lock);
    if (g_state == 1) hk_log("hiraku-pc: the NET TAISEN relay is gone");
    g_state = -1;
    WakeAllConditionVariable(&g_moved);
    LeaveCriticalSection(&g_lock);
    return 0;
}

// The relay, at the first call that needs it (not in DllMain: Winsock there could deadlock)
static int relay(void) {
    init_once();
    EnterCriticalSection(&g_lock);
    if (g_state == 0) {
        g_state = -1;
        const char *r = getenv("HK_PC_RELAY");
        unsigned a, b, c, dd, port;
        if (r && sscanf(r, "%u.%u.%u.%u:%u", &a, &b, &c, &dd, &port) == 5) {
            WSADATA wsa;
            WSAStartup(MAKEWORD(2, 2), &wsa);
            SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            struct sockaddr_in sa;
            memset(&sa, 0, sizeof sa);
            sa.sin_family = AF_INET;
            sa.sin_port = htons((u_short)port);
            sa.sin_addr.s_addr = htonl(a << 24 | b << 16 | c << 8 | dd);
            const BOOL on = TRUE;
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof on);
            struct hello mine = {MAGIC, VERSION, 0}, theirs;
            g_relay = s;
            if (s != INVALID_SOCKET && !connect(s, (struct sockaddr *)&sa, sizeof sa) && send_all(&mine, sizeof mine) &&
                recv_all(&theirs, sizeof theirs) && theirs.magic == MAGIC && theirs.version == VERSION) {
                g_addr = theirs.address;
                g_state = 1;
                CloseHandle(CreateThread(NULL, 0, reader, NULL, 0, NULL));
                hk_log("hiraku-pc: NET TAISEN through the relay at %s, as %u.%u.%u.%u", r, g_addr >> 24, (g_addr >> 16) & 255,
                       (g_addr >> 8) & 255, g_addr & 255);
            } else {
                hk_log("hiraku-pc: no NET TAISEN relay at %s (%d)", r, WSAGetLastError());
                if (s != INVALID_SOCKET) closesocket(s);
                g_relay = INVALID_SOCKET;
            }
        }
    }
    const int ok = g_state == 1;
    LeaveCriticalSection(&g_lock);
    return ok;
}

static struct vsock *sock(int s) { return s >= 0 && s < NSOCK && g_s[s].used ? &g_s[s] : NULL; }

static uint16_t own_port(struct vsock *v) {
    if (!v->port) v->port = g_next_port++;
    return v->port;
}

// Waits for `ready` as the socket's receive mode says (the lock held): 1 ready, 0 not
static int wait_for(struct vsock *v, int (*ready)(struct vsock *)) {
    if (ready(v)) return 1;
    if (v->nonblock || g_state != 1) return 0;
    const DWORD until = GetTickCount() + (DWORD)(v->timeout_ms > 0 ? v->timeout_ms : 0);
    for (;;) {
        DWORD wait = INFINITE;
        if (v->timeout_ms > 0) {
            const LONG left = (LONG)(until - GetTickCount());
            if (left <= 0) return ready(v);
            wait = (DWORD)left;
        }
        SleepConditionVariableCS(&g_moved, &g_lock, wait);
        if (ready(v)) return 1;
        if (g_state != 1 || !v->used) return 0;
    }
}

static int has_input(struct vsock *v) { return v->in != NULL || v->peer_closed || v->refused; }
static int has_pending(struct vsock *v) { return v->pending != NULL; }
static int has_answer(struct vsock *v) { return v->connected || v->refused; }

// --- libsocket's calls (__cdecl, C++ names: cabinet.c maps them)

static void __cdecl v_init(void) { relay(); }

static int __cdecl v_open(int type) {   // 0 UDP, 1 TCP
    relay();
    EnterCriticalSection(&g_lock);
    int s = -1;
    for (int i = 0; i < NSOCK && s < 0; i++)
        if (!g_s[i].used) s = i;
    if (s >= 0) {
        memset(&g_s[s], 0, sizeof g_s[s]);
        g_s[s].used = 1;
        g_s[s].stream = type == 1;
    }
    LeaveCriticalSection(&g_lock);
    if (logging()) hk_log("vsock: open(%d) = %d", type, s);
    return s;
}

static int __cdecl v_bind(int s, unsigned short port) {
    EnterCriticalSection(&g_lock);
    struct vsock *v = sock(s);
    if (v) v->port = port;
    LeaveCriticalSection(&g_lock);
    if (logging()) hk_log("vsock: bind(%d, %u)", s, port);
    return v != NULL;
}

static int __cdecl v_listen(int s, unsigned short backlog) {
    (void)backlog;
    EnterCriticalSection(&g_lock);
    struct vsock *v = sock(s);
    if (v) v->listening = 1, own_port(v);
    LeaveCriticalSection(&g_lock);
    if (logging()) hk_log("vsock: listen(%d) on %u", s, v ? v->port : 0);
    return v != NULL;
}

static int __cdecl v_accept(int s, unsigned char *const addr) {
    EnterCriticalSection(&g_lock);
    struct vsock *v = sock(s);
    int n = -1;
    if (v && v->listening && wait_for(v, has_pending)) {
        struct item *it = pop(&v->pending, &v->pending_last);
        for (int i = 0; i < NSOCK && n < 0; i++)
            if (!g_s[i].used) n = i;
        if (n >= 0) {
            struct vsock *c = &g_s[n];
            memset(c, 0, sizeof *c);
            c->used = c->stream = c->connected = 1;
            c->port = v->port, c->peer = it->addr, c->peer_port = it->port;
            memcpy(&c->id, it->data, 4);
            c->nonblock = v->nonblock, c->timeout_ms = v->timeout_ms;
            if (addr) put_addr(addr, c->peer);
            relay_send(c->peer, c->port, c->peer_port, K_OPENED | K_FROM_HOST, c->id, NULL, 0);
        } else {
            uint32_t id;
            memcpy(&id, it->data, 4);
            relay_send(it->addr, v->port, it->port, K_REFUSED | K_FROM_HOST, id, NULL, 0);
        }
        free(it);
    }
    LeaveCriticalSection(&g_lock);
    if (logging() && n >= 0) hk_log("vsock: accept(%d) = %d", s, n);
    return n;
}

static int __cdecl v_connect(int s, unsigned char *const addr, unsigned short port) {
    EnterCriticalSection(&g_lock);
    struct vsock *v = sock(s);
    int r = -1;
    if (v && v->stream && addr && is_cabinet(addr_of(addr)) && g_state == 1) {
        v->peer = addr_of(addr), v->peer_port = port, v->id = g_next_id++, v->i_connected = 1;
        v->connected = v->refused = v->peer_closed = 0;
        relay_send(v->peer, own_port(v), port, K_OPEN, v->id, NULL, 0);
        // as a blocking connect: an answer, or 10 s
        const int nb = v->nonblock, t = v->timeout_ms;
        v->nonblock = 0, v->timeout_ms = 10000;
        wait_for(v, has_answer);
        v->nonblock = nb, v->timeout_ms = t;
        r = v->connected ? 0 : -1;
    }
    LeaveCriticalSection(&g_lock);
    if (logging()) hk_log("vsock: connect(%d, %u.%u.%u.%u:%u) = %d", s, addr ? addr[0] : 0, addr ? addr[1] : 0, addr ? addr[2] : 0,
                          addr ? addr[3] : 0, port, r);
    return r;
}

static int __cdecl v_send(int s, void *buf, int len) {
    EnterCriticalSection(&g_lock);
    struct vsock *v = sock(s);
    int r = -1;
    if (v && v->stream && v->connected && !v->peer_closed && g_state == 1 && len >= 0) {
        for (int at = 0; at < len; at += 60000) {
            const int n = len - at < 60000 ? len - at : 60000;
            relay_send(v->peer, v->port, v->peer_port, K_DATA | (v->i_connected ? 0 : K_FROM_HOST), v->id, (char *)buf + at, n);
        }
        r = g_state == 1 ? len : -1;
    }
    LeaveCriticalSection(&g_lock);
    return r;
}

static int __cdecl v_recv(int s, void *buf, int len) {
    EnterCriticalSection(&g_lock);
    struct vsock *v = sock(s);
    int r = -1;
    if (v && v->stream && wait_for(v, has_input)) {
        if (v->in) {
            r = 0;
            while (v->in && r < len) {
                struct item *it = v->in;
                int n = it->size - it->at;
                if (n > len - r) n = len - r;
                memcpy((char *)buf + r, it->data + it->at, (size_t)n);
                r += n, it->at += n;
                if (it->at == it->size) free(pop(&v->in, &v->in_last));
            }
        } else {
            r = v->refused ? -1 : 0;   // closed
        }
    }
    LeaveCriticalSection(&g_lock);
    return r;
}

static int __cdecl v_sendto(int s, void *buf, int len, unsigned char *const addr, unsigned short port) {
    EnterCriticalSection(&g_lock);
    struct vsock *v = sock(s);
    int r = -1;
    if (v && !v->stream && addr && len >= 0) {
        const uint32_t to = addr_of(addr);
        if (is_cabinet(to)) relay_send(to, own_port(v), port, K_DGRAM, 0, buf, len);
        else if (port >= LOBBY_PORT && port < LOBBY_PORT + 9) relay_send(to, own_port(v), port, -1, 0, buf, len);   // a lobby: as it is
        r = len;   // anything else (a broadcast): gone, as on a LAN with nobody there
    }
    LeaveCriticalSection(&g_lock);
    if (logging()) {
        char hex[3 * 48 + 1] = "";
        for (int i = 0; i < len && i < 48; i++) snprintf(hex + 3 * i, 4, " %02x", ((const unsigned char *)buf)[i]);
        hk_log("vsock: sendto(%d, %d bytes, %u.%u.%u.%u:%u) = %d:%s", s, len, addr ? addr[0] : 0, addr ? addr[1] : 0, addr ? addr[2] : 0,
               addr ? addr[3] : 0, port, r, hex);
    }
    return r;
}

static int __cdecl v_recvfrom(int s, void *buf, int len, unsigned char *const addr, unsigned short *port) {
    EnterCriticalSection(&g_lock);
    struct vsock *v = sock(s);
    int r = -1;
    if (v && !v->stream && wait_for(v, has_input) && v->in) {
        struct item *it = pop(&v->in, &v->in_last);
        v->queued--;
        r = it->size < len ? it->size : len;
        memcpy(buf, it->data, (size_t)r);
        if (addr) put_addr(addr, it->addr);
        if (port) *port = it->port;
        free(it);
        if (r == 0) r = -1;   // as libsocket: an empty datagram is none
    }
    LeaveCriticalSection(&g_lock);
    if (logging() && r > 0) hk_log("vsock: recvfrom(%d) = %d", s, r);
    return r;
}

static int __cdecl v_broadcast(int s, int on) { (void)on; return sock(s) != NULL; }

static int __cdecl v_blocking(int s, unsigned long mode) {
    EnterCriticalSection(&g_lock);
    struct vsock *v = sock(s);
    if (v) v->nonblock = mode != 0;
    LeaveCriticalSection(&g_lock);
    return v != NULL;
}

static int __cdecl v_timeout(int s, int ms) {
    EnterCriticalSection(&g_lock);
    struct vsock *v = sock(s);
    if (v) v->timeout_ms = ms;
    LeaveCriticalSection(&g_lock);
    return v != NULL;
}

static void close_stream(struct vsock *v) {
    if (v->stream && (v->connected || v->i_connected) && !v->listening)
        relay_send(v->peer, v->port, v->peer_port, K_CLOSED | (v->i_connected ? 0 : K_FROM_HOST), v->id, NULL, 0);
    v->connected = 0;
}

static void __cdecl v_shutdown(int s) {
    EnterCriticalSection(&g_lock);
    struct vsock *v = sock(s);
    if (v) close_stream(v);
    LeaveCriticalSection(&g_lock);
}

static void __cdecl v_close(int s) {
    EnterCriticalSection(&g_lock);
    struct vsock *v = sock(s);
    if (v) {
        close_stream(v);
        while (v->in) free(pop(&v->in, &v->in_last));
        while (v->pending) free(pop(&v->pending, &v->pending_last));
        v->used = 0;
        WakeAllConditionVariable(&g_moved);
    }
    LeaveCriticalSection(&g_lock);
    if (logging()) hk_log("vsock: close(%d)", s);
}

// --- the game's own address and its router's, as the relay sees them (libavs's answers otherwise)

typedef int(__cdecl *ea3_query_t)(int what, int index, void *buf, int size);
typedef int(__cdecl *avs_net_ctrl_t)(int what, void *buf, int size);

static int __cdecl my_ea3_query(int what, int index, void *buf, int size) {
    static ea3_query_t real;
    if (!real) {   // libavs's own, or (pop'n 17 on) its e-AMUSEMENT half's
        HMODULE ea3 = GetModuleHandleA("libavs-win32-ea3.dll");
        real = (ea3_query_t)(void *)GetProcAddress(ea3 ? ea3 : GetModuleHandleA("libavs-win32.dll"), "ea3_query");
    }
    const int r = real ? real(what, index, buf, size) : -1;
    if (what == 8 && buf && size >= 4 && relay()) {   // the portfw address (the router as the world sees it)
        put_addr((unsigned char *)buf, g_addr);
        return 4;
    }
    return r;
}

static int __cdecl my_avs_net_ctrl(int what, void *buf, int size) {
    static avs_net_ctrl_t real;
    if (!real) real = (avs_net_ctrl_t)(void *)GetProcAddress(GetModuleHandleA("libavs-win32.dll"), "avs_net_ctrl");
    const int r = real ? real(what, buf, size) : -1;
    if (what == 0 && buf && size >= 4 && relay()) {   // the cabinet's own address
        put_addr((unsigned char *)buf, g_addr);
        return 4;
    }
    if (what == 3 && buf && size >= 4 && relay()) {   // its network's mask
        put_addr((unsigned char *)buf, 0xffff0000u);
        return 4;
    }
    return r;
}

// --- for HK_PC_NET_LOG: the game's messages as it hands them to libcomm

typedef int(__cdecl *send_que_t)(unsigned char *addr, int type, char *data, int len, int flags);
typedef void(__cdecl *read_addr_t)(unsigned char *addr);

static int __cdecl log_send_que(unsigned char *addr, int type, char *data, int len, int flags) {
    static send_que_t real;
    if (!real) real = (send_que_t)(void *)GetProcAddress(GetModuleHandleA("libcomm.dll"), "?CommSetSendQueData@@YAHPAUT_COMM_ADDR@@HPADHH@Z");
    const int r = real(addr, type, data, len, flags);
    hk_log("comm: send %u.%u.%u.%u:%u type %d, %d bytes, flags %d = %d", addr[0], addr[1], addr[2], addr[3], addr[4] | addr[5] << 8, type, len,
           flags, r);
    return r;
}

static void __cdecl log_my_addr(unsigned char *addr) {
    static read_addr_t real;
    if (!real) real = (read_addr_t)(void *)GetProcAddress(GetModuleHandleA("libcomm.dll"), "?CommReadMyAddr@@YAXPAUT_COMM_ADDR@@@Z");
    real(addr);
    static int once;
    if (!once++) hk_log("comm: my address %u.%u.%u.%u:%u", addr[0], addr[1], addr[2], addr[3], addr[4] | addr[5] << 8);
}

static void __cdecl log_portfw_addr(unsigned char *addr) {
    static read_addr_t real;
    if (!real) real = (read_addr_t)(void *)GetProcAddress(GetModuleHandleA("libcomm.dll"), "?CommReadPortfwAddr@@YAXPAUT_COMM_ADDR@@@Z");
    real(addr);
    static int once;
    if (!once++) hk_log("comm: portfw address %u.%u.%u.%u:%u", addr[0], addr[1], addr[2], addr[3], addr[4] | addr[5] << 8);
}

// pop'n 16-18 have libsocket built into the program (its 15 calls static, over Winsock imported
// by ordinal; libcomm and libkernel built in too, exported by name): each call is the one function
// that calls its Winsock function (setsockopt twice: SO_BROADCAST, SO_RCVTIMEO), found so and made
// a jump to ours. All or none: a program where one isn't found keeps its own.
static void *ws2_slot(char *base, int ordinal) {
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY *dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir->VirtualAddress); dir->VirtualAddress && imp->Name; imp++) {
        if (_stricmp(base + imp->Name, "ws2_32.dll")) continue;
        IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        IMAGE_THUNK_DATA *addrs = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, addrs++)
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal) && IMAGE_ORDINAL(names->u1.Ordinal) == (DWORD)ordinal) return &addrs->u1.Function;
    }
    return NULL;
}

void hk_vsock_patch_program(void) {
    if (!getenv("HK_PC_RELAY")) return;
    char *base = (char *)hk_program();
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    static const struct { int ordinal; int option; void *to; const char *name; } calls[] = {
        {115, 0, (void *)v_init, "SockInit"},        {23, 0, (void *)v_open, "SockOpen"},
        {2, 0, (void *)v_bind, "SockBind"},          {13, 0, (void *)v_listen, "SockListen"},
        {1, 0, (void *)v_accept, "SockAccept"},      {4, 0, (void *)v_connect, "SockConnect"},
        {19, 0, (void *)v_send, "SockSend"},         {16, 0, (void *)v_recv, "SockRecv"},
        {20, 0, (void *)v_sendto, "SockSendTo"},     {17, 0, (void *)v_recvfrom, "SockRecvFrom"},
        {21, 0x20, (void *)v_broadcast, "SockSetBroadCast"},
        {10, 0, (void *)v_blocking, "SockSetRecvBlockingMode"},
        {21, 0x1006, (void *)v_timeout, "SockSetRecvTimeout"},
        {22, 0, (void *)v_shutdown, "SockShutdown"}, {3, 0, (void *)v_close, "SockClose"},
    };
    enum { N = sizeof calls / sizeof *calls };
    uint8_t *found[N] = {0};
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    for (int c = 0; c < N; c++) {
        const uint32_t slot = (uint32_t)(uintptr_t)ws2_slot(base, calls[c].ordinal);
        if (!slot) return;   // no Winsock of its own: pop'n 15's is libsocket.dll (hk_vsock_hook)
        int sites = 0;
        sec = IMAGE_FIRST_SECTION(nt);
        for (WORD k = 0; k < nt->FileHeader.NumberOfSections; k++, sec++) {
            if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            uint8_t *code = (uint8_t *)base + sec->VirtualAddress;
            const size_t size = sec->Misc.VirtualSize;
            for (size_t i = 0x100; i + 6 < size; i++) {
                if (code[i] != 0xff || code[i + 1] != 0x15 || memcmp(code + i + 2, &slot, 4)) continue;
                // the function: from the int3 padding before it, 16 bytes aligned
                uint8_t *f = NULL;
                for (size_t b = i & ~(size_t)15; b + 0x200 > i && b >= 16; b -= 16)
                    if (code[b - 1] == 0xcc) { f = code + b; break; }
                if (!f) continue;
                if (calls[c].option) {   // setsockopt's option pushed: 6a 20, or 68 06 10 00 00
                    int has = 0;
                    for (uint8_t *q = f; q < code + i; q++)
                        has |= calls[c].option < 0x80 ? q[0] == 0x6a && q[1] == calls[c].option
                                                      : q[0] == 0x68 && !memcmp(q + 1, &calls[c].option, 4);
                    if (!has) continue;
                }
                if (found[c] && found[c] != f) sites = 99;   // two functions: not the shape known
                found[c] = f, sites++;
            }
        }
        if (!found[c] || sites != 1) {
            hk_log("hiraku-pc: NET TAISEN: the program's %s not found (%d): its own sockets kept", calls[c].name, sites);
            return;
        }
    }
    for (int c = 0; c < N; c++) {
        DWORD old;
        VirtualProtect(found[c], 5, PAGE_EXECUTE_READWRITE, &old);
        found[c][0] = 0xe9;
        const int32_t rel = (int32_t)((uintptr_t)calls[c].to - (uintptr_t)(found[c] + 5));
        memcpy(found[c] + 1, &rel, 4);
        VirtualProtect(found[c], 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), found[c], 5);
    }
    init_once();
    hk_log("hiraku-pc: NET TAISEN: the program's own sockets (libsocket's calls, %p...) go to HIRAKU's relay", (void *)found[0]);
}

void *hk_comm_hook(const char *fn) {
    if (!getenv("HK_PC_NET_LOG") || !getenv("HK_PC_RELAY")) return NULL;
    if (!strcmp(fn, "?CommSetSendQueData@@YAHPAUT_COMM_ADDR@@HPADHH@Z")) return (void *)log_send_que;
    if (!strcmp(fn, "?CommReadMyAddr@@YAXPAUT_COMM_ADDR@@@Z")) return (void *)log_my_addr;
    if (!strcmp(fn, "?CommReadPortfwAddr@@YAXPAUT_COMM_ADDR@@@Z")) return (void *)log_portfw_addr;
    return NULL;
}

// The hook for one of libsocket's calls (C++ names), or NULL; and libavs's two above
void *hk_vsock_hook(const char *fn) {
    if (!getenv("HK_PC_RELAY")) return NULL;
    init_once();
    static const struct { const char *name; void *to; } calls[] = {
        {"?SockInit@@YAXXZ", (void *)v_init},
        {"?SockOpen@@YAHH@Z", (void *)v_open},
        {"?SockBind@@YAHHG@Z", (void *)v_bind},
        {"?SockListen@@YAHHG@Z", (void *)v_listen},
        {"?SockAccept@@YAHHQAE@Z", (void *)v_accept},
        {"?SockConnect@@YAHHQAEG@Z", (void *)v_connect},
        {"?SockSend@@YAHHPAXH@Z", (void *)v_send},
        {"?SockRecv@@YAHHPAXH@Z", (void *)v_recv},
        {"?SockSendTo@@YAHHPAXHQAEG@Z", (void *)v_sendto},
        {"?SockRecvFrom@@YAHHPAXHQAEPAG@Z", (void *)v_recvfrom},
        {"?SockSetBroadCast@@YAHHH@Z", (void *)v_broadcast},
        {"?SockSetRecvBlockingMode@@YAHHK@Z", (void *)v_blocking},
        {"?SockSetRecvTimeout@@YAHHH@Z", (void *)v_timeout},
        {"?SockShutdown@@YAXH@Z", (void *)v_shutdown},
        {"?SockClose@@YAXH@Z", (void *)v_close},
        {"ea3_query", (void *)my_ea3_query},
        {"avs_net_ctrl", (void *)my_avs_net_ctrl},
    };
    for (size_t i = 0; i < sizeof calls / sizeof *calls; i++)
        if (!strcmp(fn, calls[i].name)) return calls[i].to;
    return NULL;
}
