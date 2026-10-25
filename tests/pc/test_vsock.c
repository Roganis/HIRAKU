// SPDX-License-Identifier: GPL-3.0-or-later
// runtime/pc/vsock.c (NET TAISEN for the PC games over HIRAKU's relay), checked as the games use it:
// libsocket's calls, taken by name from hk_vsock_hook, between two processes on one relay.
//
//   hiraku-eamuse --port 18120 --data DIR      (its relay: TCP 5740, lobbies 5740 and 5741)
//   HK_PC_RELAY=127.0.0.1:5740 wine test_vsock.exe host     (first: the relay gives it 10.57.0.1)
//   HK_PC_RELAY=127.0.0.1:5740 wine test_vsock.exe client   (then: 10.57.0.2)
// tests/pc/test_vsock.sh runs all of it. Each prints what it checked and exits 0 when all went well.
//
// Checked: the lobby (pop'n 15's participate: the host enters, the client finds it), datagrams both
// ways (sender's address and port as the receiver sees them), a TCP connection (connect, accept with
// the peer's address, data both ways in pieces, the end seen by recv), a connection refused (nobody
// listening), and receive timeouts.
#include <windows.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../runtime/pc/cabinet.h"

void hk_log(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vfprintf(stderr, fmt, a);
    va_end(a);
    fputc('\n', stderr);
}
void *hk_program(void) { return GetModuleHandleA(NULL); }   // (cabinet.c's: the game's program)

typedef void(__cdecl *init_t)(void);
typedef int(__cdecl *open_t)(int);
typedef int(__cdecl *bind_t)(int, unsigned short);
typedef int(__cdecl *listen_t)(int, unsigned short);
typedef int(__cdecl *accept_t)(int, unsigned char *);
typedef int(__cdecl *connect_t)(int, unsigned char *, unsigned short);
typedef int(__cdecl *send_t)(int, void *, int);
typedef int(__cdecl *recv_t)(int, void *, int);
typedef int(__cdecl *sendto_t)(int, void *, int, unsigned char *, unsigned short);
typedef int(__cdecl *recvfrom_t)(int, void *, int, unsigned char *, unsigned short *);
typedef int(__cdecl *timeout_t)(int, int);
typedef void(__cdecl *close_t)(int);
typedef int(__cdecl *net_ctrl_t)(int, void *, int);

static int g_fail;
#define CHECK(c, ...) do { if (c) printf("ok    " __VA_ARGS__); else { printf("FAIL  " __VA_ARGS__); g_fail = 1; } printf("\n"); } while (0)

static void *call(const char *name) {
    void *p = hk_vsock_hook(name);
    if (!p) { printf("FAIL  no hook for %s\n", name); exit(1); }
    return p;
}

// a lobby datagram as globbyc makes it: "lob0", length, CRC-16/X-25 (its field 0), sequence, kind
static int lobby_packet(unsigned char *p, int kind, uint16_t seq, const unsigned char *body, int n) {
    memcpy(p, "lob0", 4);
    const int len = 12 + n;
    p[4] = (unsigned char)(len >> 8), p[5] = (unsigned char)len, p[6] = p[7] = 0;
    p[8] = (unsigned char)(seq >> 8), p[9] = (unsigned char)seq, p[10] = (unsigned char)kind, p[11] = 0;
    memcpy(p + 12, body, (size_t)n);
    uint32_t c = 0xffff;
    for (int i = 0; i < len; i++) {
        c ^= p[i];
        for (int b = 0; b < 8; b++) c = c & 1 ? c >> 1 ^ 0x8408 : c >> 1;
    }
    c = ~c & 0xffff;
    p[6] = (unsigned char)(c >> 8), p[7] = (unsigned char)c;
    return len;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const int host = argc > 1 && !strcmp(argv[1], "host");
    ((init_t)call("?SockInit@@YAXXZ"))();
    open_t s_open = (open_t)call("?SockOpen@@YAHH@Z");
    bind_t s_bind = (bind_t)call("?SockBind@@YAHHG@Z");
    listen_t s_listen = (listen_t)call("?SockListen@@YAHHG@Z");
    accept_t s_accept = (accept_t)call("?SockAccept@@YAHHQAE@Z");
    connect_t s_connect = (connect_t)call("?SockConnect@@YAHHQAEG@Z");
    send_t s_send = (send_t)call("?SockSend@@YAHHPAXH@Z");
    recv_t s_recv = (recv_t)call("?SockRecv@@YAHHPAXH@Z");
    sendto_t s_sendto = (sendto_t)call("?SockSendTo@@YAHHPAXHQAEG@Z");
    recvfrom_t s_recvfrom = (recvfrom_t)call("?SockRecvFrom@@YAHHPAXHQAEPAG@Z");
    timeout_t s_timeout = (timeout_t)call("?SockSetRecvTimeout@@YAHHH@Z");
    close_t s_close = (close_t)call("?SockClose@@YAXH@Z");
    net_ctrl_t net_ctrl = (net_ctrl_t)call("avs_net_ctrl");

    unsigned char me[4] = {0};
    CHECK(net_ctrl(0, me, 4) == 4 && me[0] == 10 && me[1] == 57, "the cabinet's address: %u.%u.%u.%u", me[0], me[1], me[2], me[3]);
    unsigned char lobby[4] = {127, 0, 0, 1}, peer[4];
    unsigned short port = 0;
    static unsigned char buf[110000];
    unsigned char pkt[64], body[28] = {0};
    body[3] = 1;   // room 1
    memcpy(body + 4, me, 4);
    body[8] = 0x16, body[9] = 0x44;   // 5700
    body[11] = 30;                    // kept 30 s
    memcpy(body + 12, me, 4);
    body[16] = 0x16, body[17] = 0x44;

    const int lob = s_open(0), udp = s_open(0);
    s_bind(lob, 32000);
    s_bind(udp, 5700);
    s_timeout(lob, 3000);
    s_timeout(udp, 5000);
    const int n = lobby_packet(pkt, 3, 1, body, 28);   // participate
    s_sendto(lob, pkt, n, lobby, 5741);
    const int got = s_recvfrom(lob, buf, sizeof buf, peer, &port);
    if (host) {
        CHECK(got == 12 && buf[11] == 0, "participate, alone: no one there (%d bytes)", got);
        // the client's datagram, then ours back to where it came from
        const int k = s_recvfrom(udp, buf, sizeof buf, peer, &port);
        CHECK(k == 5 && !memcmp(buf, "hello", 5) && peer[0] == 10 && peer[3] == 2 && port == 30000,
              "a datagram from %u.%u.%u.%u:%u (%d bytes)", peer[0], peer[1], peer[2], peer[3], port, k);
        CHECK(s_sendto(udp, "back", 4, peer, port) == 4, "a datagram back");
        // TCP: accept the client's connection and talk
        const int l = s_open(1);
        s_bind(l, 6000);
        s_listen(l, 4);
        s_timeout(l, 10000);
        const int c = s_accept(l, peer);
        CHECK(c >= 0 && peer[3] == 2, "a connection accepted from %u.%u.%u.%u", peer[0], peer[1], peer[2], peer[3]);
        s_timeout(c, 5000);
        int total = 0;
        while (total < 100000) {   // the client's 100000 bytes, in pieces
            const int r = s_recv(c, buf, 30000);
            if (r <= 0) break;
            for (int i = 0; i < r; i++) if (buf[i] != (unsigned char)(total + i)) { total = -1000000; break; }
            total += r;
        }
        CHECK(total == 100000, "100000 bytes received in order (%d)", total);
        CHECK(s_send(c, "reply", 5) == 5, "a reply sent");
        CHECK(s_recv(c, buf, sizeof buf) == 0, "the client's end seen");
        s_close(c);
        // nothing more comes: a receive times out
        s_timeout(udp, 500);
        CHECK(s_recvfrom(udp, buf, sizeof buf, peer, &port) == -1, "a receive with nothing there times out");
    } else {
        CHECK(got == 12 + 28 && buf[11] == 1 && buf[12 + 4] == 10 && buf[12 + 7] == 1, "participate: the host found (%d bytes, %d entries)", got,
              got >= 12 ? buf[11] : -1);
        const int out = s_open(0);
        s_bind(out, 30000);
        s_timeout(out, 5000);
        unsigned char h[4] = {10, 57, 0, 1};
        CHECK(s_sendto(out, "hello", 5, h, 5700) == 5, "a datagram to the host");
        const int k = s_recvfrom(out, buf, sizeof buf, peer, &port);
        CHECK(k == 4 && !memcmp(buf, "back", 4) && peer[3] == 1 && port == 5700, "its answer from %u.%u.%u.%u:%u", peer[0], peer[1], peer[2],
              peer[3], port);
        Sleep(500);   // the host is listening by now
        const int none = s_open(1);
        CHECK(s_connect(none, h, 6001) == -1, "a connection to a port nobody listens on is refused");
        const int c = s_open(1);
        CHECK(s_connect(c, h, 6000) == 0, "a connection to the host");
        for (int i = 0; i < 100000; i++) buf[i] = (unsigned char)i;
        CHECK(s_send(c, buf, 70000) == 70000 && s_send(c, buf + 70000, 30000) == 30000, "100000 bytes sent");
        s_timeout(c, 5000);
        const int r = s_recv(c, buf, sizeof buf);
        CHECK(r == 5 && !memcmp(buf, "reply", 5), "the host's reply (%d bytes)", r);
        s_close(c);
        Sleep(1000);   // for the host to see the end before this process (and its relay connection) goes
    }
    return g_fail;
}
