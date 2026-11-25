// SPDX-License-Identifier: GPL-3.0-or-later
// The cabinet's network at AVS2's imports (PLAN.md section 32), as runtime/pc/net.c gives pop'n
// 15-18's libavs:
//   - The network card (GetAdaptersInfo): 192.168.0.2/24 behind a router at 192.168.0.1, whatever
//     the PC has. Under Wine AVS2 read no address (avs_net_ctrl(7) failed), and the game's network
//     check saw it down (the boot check's NETWORK ERROR).
//   - e-AMUSEMENT's services name their modules' URLs without a port (http://127.0.0.1/): AVS2's
//     connections to port 80 of HK_PC64_EA's address go to its port.
//   - The keepalive's pings (ICMP echo through a raw socket, to the router, the server and the
//     center the services name): answered at once on a UDP socket of this machine, so they need
//     neither a raw socket's rights nor Wine's. ea3's status 0x11 reads online (3) once they come
//     back, and the game's own e-AMUSEMENT calls wait for that.
#include <winsock2.h>
#include <ws2tcpip.h>
#include "pc64.h"

#include <iphlpapi.h>
#include <stdlib.h>
#include <string.h>

// (with winsock2.h first, mingw's headers leave GetProcAddress undeclared in C)
extern FARPROC WINAPI GetProcAddress(HMODULE, LPCSTR);

static struct in_addr g_ea_addr;
static u_short g_ea_port;   // network order; 0: no server given
static int g_log;           // HK_PC64_NET_LOG on this game: what the game is told and does on the network

static void read_server(void) {
    const char *url = getenv("HK_PC64_EA"), *at = getenv("HK_PC_EA");   // a URL, or hiraku-pc's a.b.c.d:port
    unsigned a, b, c, d, port;
    if ((url && sscanf(url, "http://%u.%u.%u.%u:%u", &a, &b, &c, &d, &port) == 5) ||
        (!url && at && sscanf(at, "%u.%u.%u.%u:%u", &a, &b, &c, &d, &port) == 5)) {
        g_ea_addr.s_addr = htonl(a << 24 | b << 16 | c << 8 | d);
        g_ea_port = htons((u_short)port);
    }
}

static const char *ip4(struct in_addr a) { return inet_ntoa(a); }

// --- the card
static DWORD WINAPI GetAdaptersInfo_(PIP_ADAPTER_INFO info, PULONG size) {
    if (!size) return ERROR_INVALID_PARAMETER;
    if (!info || *size < sizeof(IP_ADAPTER_INFO)) {
        *size = sizeof(IP_ADAPTER_INFO);
        return ERROR_BUFFER_OVERFLOW;
    }
    if (g_log) pc64_log("net: GetAdaptersInfo -> the cabinet's card 192.168.0.2/24, gateway 192.168.0.1");
    memset(info, 0, sizeof *info);
    strcpy(info->AdapterName, "{48495241-4b55-0000-0000-000000000001}");
    strcpy(info->Description, "HIRAKU");
    info->AddressLength = 6;
    static const BYTE mac[6] = {0x02, 0x48, 0x4b, 0x00, 0x00, 0x01};   // locally administered
    memcpy(info->Address, mac, 6);
    info->Index = 1;
    info->Type = MIB_IF_TYPE_ETHERNET;
    strcpy(info->IpAddressList.IpAddress.String, "192.168.0.2");
    strcpy(info->IpAddressList.IpMask.String, "255.255.255.0");
    strcpy(info->GatewayList.IpAddress.String, "192.168.0.1");
    strcpy(info->GatewayList.IpMask.String, "255.255.255.255");
    return NO_ERROR;
}
static DWORD WINAPI AddIPAddress_(IPAddr a, IPMask m, DWORD i, PULONG context, PULONG instance) {
    (void)a, (void)m, (void)i;
    if (context) *context = 0;
    if (instance) *instance = 0;
    return NO_ERROR;
}
static DWORD WINAPI DeleteIPAddress_(ULONG context) {
    (void)context;
    return NO_ERROR;
}

// --- e-AMUSEMENT's port
static int(WSAAPI *r_connect)(SOCKET, const struct sockaddr *, int);
static int WSAAPI connect_(SOCKET s, const struct sockaddr *a, int n) {
    if (!r_connect) r_connect = (void *)GetProcAddress(GetModuleHandleA("ws2_32.dll"), "connect");
    if (g_ea_port && a && a->sa_family == AF_INET && n >= (int)sizeof(struct sockaddr_in)) {
        struct sockaddr_in in = *(const struct sockaddr_in *)a;
        if (in.sin_port == htons(80) && in.sin_addr.s_addr == g_ea_addr.s_addr) {
            if (g_log) pc64_log("net: connect(%s:%u) -> the e-AMUSEMENT server's port %u", ip4(in.sin_addr), 80, ntohs(g_ea_port));
            in.sin_port = g_ea_port;
            const int r = r_connect(s, (struct sockaddr *)&in, sizeof in);
            if (g_log) pc64_log("net: connect = %d (err %d)", r, r ? WSAGetLastError() : 0);
            return r;
        }
    }
    const int r = r_connect(s, a, n);
    if (g_log && a && a->sa_family == AF_INET && n >= (int)sizeof(struct sockaddr_in)) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)a;
        pc64_log("net: connect(%s:%u) = %d (err %d)", ip4(in->sin_addr), ntohs(in->sin_port), r, r ? WSAGetLastError() : 0);
    }
    return r;
}

// --- the keepalive's pings: a UDP socket stands in for each raw ICMP one
#define MAX_PINGERS 8
static struct {
    SOCKET s;
    struct sockaddr_in self, last_to;   // where the answers come back; the last one pinged
} g_ping[MAX_PINGERS];
static int g_npings;

static int pinger(SOCKET s) {
    for (int i = 0; i < g_npings; i++)
        if (g_ping[i].s == s) return i;
    return -1;
}
static SOCKET WSAAPI socket_(int af, int type, int proto) {
    if (af == AF_INET && type == SOCK_RAW && proto == IPPROTO_ICMP && g_npings < MAX_PINGERS) {
        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET) return s;
        memset(&g_ping[g_npings], 0, sizeof g_ping[g_npings]);
        g_ping[g_npings++].s = s;
        if (g_log) pc64_log("net: socket(AF_INET, SOCK_RAW, IPPROTO_ICMP) -> a UDP socket %d, its echo replies answered at once", (int)s);
        return s;
    }
    const SOCKET s = socket(af, type, proto);
    if (g_log) pc64_log("net: socket(%d, %d, %d) = %d (err %d)", af, type, proto, (int)s, s == INVALID_SOCKET ? WSAGetLastError() : 0);
    return s;
}
static int bind_pinger(int k) {
    if (g_ping[k].self.sin_port) return 0;
    struct sockaddr_in self;
    memset(&self, 0, sizeof self);
    self.sin_family = AF_INET;
    self.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int n = sizeof self;
    if (bind(g_ping[k].s, (struct sockaddr *)&self, sizeof self) || getsockname(g_ping[k].s, (struct sockaddr *)&self, &n)) return -1;
    g_ping[k].self = self;
    return 0;
}
static int WSAAPI bind_(SOCKET s, const struct sockaddr *a, int len) {
    const int k = pinger(s);
    if (k >= 0) {
        const int r = bind_pinger(k);
        if (g_log) pc64_log("net: bind(pinger %d) = %d", (int)s, r);
        return r;
    }
    const int r = bind(s, a, len);
    if (g_log && a && a->sa_family == AF_INET && len >= (int)sizeof(struct sockaddr_in)) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)a;
        pc64_log("net: bind(%s:%u) = %d (err %d)", ip4(in->sin_addr), ntohs(in->sin_port), r, r ? WSAGetLastError() : 0);
    }
    return r;
}
static int WSAAPI closesocket_(SOCKET s) {
    const int k = pinger(s);
    if (k >= 0) g_ping[k] = g_ping[--g_npings];
    return closesocket(s);
}
static u_short icmp_sum(const uint8_t *p, int n) {
    uint32_t sum = 0;
    for (int i = 0; i + 1 < n; i += 2) sum += (uint32_t)(p[i] << 8 | p[i + 1]);
    if (n & 1) sum += (uint32_t)(p[n - 1] << 8);
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (u_short)~sum;
}
// An echo request: its reply as a raw socket gives it (an IPv4 header first, from the one pinged),
// sent back to the stand-in for recvfrom and select to find
static int ping_send(int k, const char *buf, int len, const struct sockaddr *to, int tolen) {
    if (g_log && to && tolen >= (int)sizeof(struct sockaddr_in)) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)to;
        pc64_log("net: ping sendto(%s:%u) len %d, icmp type %d%s", ip4(in->sin_addr), ntohs(in->sin_port), len,
                 len > 0 ? (int)(uint8_t)buf[0] : -1, (len >= 8 && (uint8_t)buf[0] == 8) ? " (echo request: answered at once)" : "");
    }
    if (len < 8 || len > 1400 || !to || tolen < (int)sizeof(struct sockaddr_in) || bind_pinger(k) || (uint8_t)buf[0] != 8) return len;
    uint8_t reply[1500];
    const int n = 20 + len;
    memset(reply, 0, 20);
    reply[0] = 0x45;
    reply[2] = (uint8_t)(n >> 8), reply[3] = (uint8_t)n;
    reply[8] = 64, reply[9] = IPPROTO_ICMP;
    memcpy(reply + 12, &((const struct sockaddr_in *)to)->sin_addr, 4);
    memcpy(reply + 16, &g_ping[k].self.sin_addr, 4);
    const u_short hs = icmp_sum(reply, 20);
    reply[10] = (uint8_t)(hs >> 8), reply[11] = (uint8_t)hs;
    memcpy(reply + 20, buf, (size_t)len);
    reply[20] = 0;   // an echo reply
    reply[22] = reply[23] = 0;
    const u_short cs = icmp_sum(reply + 20, len);
    reply[22] = (uint8_t)(cs >> 8), reply[23] = (uint8_t)cs;
    g_ping[k].last_to = *(const struct sockaddr_in *)to;
    sendto(g_ping[k].s, (const char *)reply, n, 0, (const struct sockaddr *)&g_ping[k].self, sizeof g_ping[k].self);
    return len;
}
static int ping_receive(int k, char *buf, int len, int flags, struct sockaddr *from, int *fromlen) {
    struct sockaddr_in got;
    int gotlen = sizeof got;
    const int r = recvfrom(g_ping[k].s, buf, len, flags, (struct sockaddr *)&got, &gotlen);
    if (g_log) pc64_log("net: ping recvfrom = %d (err %d)", r, r < 0 ? WSAGetLastError() : 0);
    if (r >= 0 && from && fromlen && *fromlen >= (int)sizeof(struct sockaddr_in)) {
        memcpy(from, &g_ping[k].last_to, sizeof(struct sockaddr_in));
        if (r >= 16) memcpy(&((struct sockaddr_in *)from)->sin_addr, buf + 12, 4);   // the reply's sender
        *fromlen = sizeof(struct sockaddr_in);
    }
    return r;
}
// AVS2 pings through WSASendTo and WSARecvFrom (synchronously: no overlapped I/O)
static int WSAAPI WSASendTo_(SOCKET s, LPWSABUF bufs, DWORD n, LPDWORD sent, DWORD flags, const struct sockaddr *to, int tolen,
                             LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE done) {
    const int k = pinger(s);
    if (k < 0 || ov || done) return WSASendTo(s, bufs, n, sent, flags, to, tolen, ov, done);
    char one[1500];
    int len = 0;
    for (DWORD i = 0; i < n && len + (int)bufs[i].len <= (int)sizeof one; i++) {
        memcpy(one + len, bufs[i].buf, bufs[i].len);
        len += (int)bufs[i].len;
    }
    const int r = ping_send(k, one, len, to, tolen);
    if (sent) *sent = (DWORD)r;
    return 0;
}
static int WSAAPI WSARecvFrom_(SOCKET s, LPWSABUF bufs, DWORD n, LPDWORD got, LPDWORD flags, struct sockaddr *from, LPINT fromlen,
                               LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE done) {
    const int k = pinger(s);
    if (k < 0 || ov || done) return WSARecvFrom(s, bufs, n, got, flags, from, fromlen, ov, done);
    char one[1500];
    const int r = ping_receive(k, one, sizeof one, flags ? (int)*flags : 0, from, fromlen);
    if (r < 0) return SOCKET_ERROR;
    int at = 0;
    for (DWORD i = 0; i < n && at < r; i++) {
        const int c = r - at < (int)bufs[i].len ? r - at : (int)bufs[i].len;
        memcpy(bufs[i].buf, one + at, (size_t)c);
        at += c;
    }
    if (got) *got = (DWORD)at;
    if (flags) *flags = 0;
    return 0;
}

// HK_PC64_NET_LOG on this game: the calls the loader doesn't otherwise touch, logged and passed on.
// Each wrapper never calls a NULL r_: if its module didn't import the call, the real one is taken
// from the DLL's exports on first use
static int(WSAAPI *r_getaddrinfo)(PCSTR, PCSTR, const ADDRINFOA *, PADDRINFOA *);
static int WSAAPI getaddrinfo_(PCSTR node, PCSTR service, const ADDRINFOA *hints, PADDRINFOA *res) {
    if (!r_getaddrinfo) r_getaddrinfo = (void *)GetProcAddress(GetModuleHandleA("ws2_32.dll"), "getaddrinfo");
    const int r = r_getaddrinfo(node, service, hints, res);
    if (g_log) {
        pc64_log("net: getaddrinfo(\"%s\", \"%s\") = %d", node ? node : "", service ? service : "", r);
        for (PADDRINFOA a = res ? *res : NULL; a && g_log; a = a->ai_next)
            if (a->ai_family == AF_INET)
                pc64_log("net:   -> %s:%u", ip4(((struct sockaddr_in *)a->ai_addr)->sin_addr), ntohs(((struct sockaddr_in *)a->ai_addr)->sin_port));
    }
    return r;
}
static int(WSAAPI *r_getnameinfo)(const struct sockaddr *, socklen_t, PSTR, DWORD, PSTR, DWORD, int);
static int WSAAPI getnameinfo_(const struct sockaddr *a, socklen_t alen, PSTR host, DWORD hlen, PSTR serv, DWORD slen, int flags) {
    if (!r_getnameinfo) r_getnameinfo = (void *)GetProcAddress(GetModuleHandleA("ws2_32.dll"), "getnameinfo");
    const int r = r_getnameinfo(a, alen, host, hlen, serv, slen, flags);
    if (g_log && a && a->sa_family == AF_INET)
        pc64_log("net: getnameinfo(%s) = %d, host \"%s\" serv \"%s\"", ip4(((const struct sockaddr_in *)a)->sin_addr), r, host ? host : "", serv ? serv : "");
    return r;
}
static DWORD(WINAPI *r_GetNetworkParams)(PFIXED_INFO, PULONG);
static DWORD WINAPI GetNetworkParams_(PFIXED_INFO info, PULONG size) {
    if (!r_GetNetworkParams) r_GetNetworkParams = (void *)GetProcAddress(GetModuleHandleA("iphlpapi.dll"), "GetNetworkParams");
    const DWORD r = r_GetNetworkParams(info, size);
    if (g_log && r == NO_ERROR && info) pc64_log("net: GetNetworkParams -> host \"%s\", domain \"%s\"", info->HostName, info->DomainName);
    return r;
}
static DWORD(WINAPI *r_GetIfTable)(PMIB_IFTABLE, PULONG, BOOL);
static DWORD WINAPI GetIfTable_(PMIB_IFTABLE t, PULONG size, BOOL order) {
    if (!r_GetIfTable) r_GetIfTable = (void *)GetProcAddress(GetModuleHandleA("iphlpapi.dll"), "GetIfTable");
    const DWORD r = r_GetIfTable(t, size, order);
    if (g_log) pc64_log("net: GetIfTable = %lu, %lu row(s)", (unsigned long)r, r == NO_ERROR && t ? (unsigned long)t->dwNumEntries : 0ul);
    return r;
}

// Called once the game's DLL is loaded too: the name lookups and the machine's names go to Wine
// untouched today, so with the log on they are wrapped to be seen (and still passed on)
void pc64_netlog_install(HMODULE avs, HMODULE game) {
    if (!g_log) return;
    r_getaddrinfo = pc64_import(avs, "ws2_32.dll", "getaddrinfo", (void *)getaddrinfo_);
    r_getnameinfo = pc64_import(avs, "ws2_32.dll", "getnameinfo", (void *)getnameinfo_);
    r_GetNetworkParams = pc64_import(avs, "iphlpapi.dll", "GetNetworkParams", (void *)GetNetworkParams_);
    if (game) {
        r_GetIfTable = pc64_import(game, "iphlpapi.dll", "GetIfTable", (void *)GetIfTable_);
        void *g = pc64_import(game, "iphlpapi.dll", "GetNetworkParams", (void *)GetNetworkParams_);
        if (!r_GetNetworkParams) r_GetNetworkParams = g;   // AVS didn't import it: the game's real one serves
    }
    if (r_getaddrinfo) pc64_log("net: name lookups logged (HK_PC64_NET_LOG)");
}

void pc64_net_install(HMODULE avs) {
    read_server();
    g_log = pc64_game_is_iidx() && getenv("HK_PC64_NET_LOG") != NULL;
    // AVS2 imports Winsock by ordinal: 2 bind, 3 closesocket, 4 connect, 23 socket
    r_connect = pc64_import_ordinal(avs, "ws2_32.dll", 4, (void *)connect_);
    pc64_import_ordinal(avs, "ws2_32.dll", 2, (void *)bind_);
    pc64_import_ordinal(avs, "ws2_32.dll", 3, (void *)closesocket_);
    pc64_import_ordinal(avs, "ws2_32.dll", 23, (void *)socket_);
    pc64_import(avs, "ws2_32.dll", "WSASendTo", (void *)WSASendTo_);
    pc64_import(avs, "ws2_32.dll", "WSARecvFrom", (void *)WSARecvFrom_);
    pc64_import(avs, "iphlpapi.dll", "GetAdaptersInfo", (void *)GetAdaptersInfo_);
    pc64_import(avs, "iphlpapi.dll", "AddIPAddress", (void *)AddIPAddress_);
    pc64_import(avs, "iphlpapi.dll", "DeleteIPAddress", (void *)DeleteIPAddress_);
    if (!r_connect) pc64_log("AVS2 doesn't import connect by ordinal 4: its network as it is");
}
