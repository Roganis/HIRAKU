// SPDX-License-Identifier: GPL-3.0-or-later
// A cabinet's side of a server's key (runtime/shell/keyed.h), for tests/eamuse/test_key.py:
//
//   key_client call HOST PORT KEY               the request on its input to the server behind the key; the
//                                               answer on its output ("error: ..." and exit 1 if none)
//   key_client garbled HOST PORT KEY            the same, the first record with a bit changed on its way
//   key_client forward HOST PORT KEY            a port on this computer passed on behind the key (as for a
//                                               PC game): "port N", until its input closes
//   key_client relay HOST PORT KEY [ADDRESS]    a cabinet on NET TAISEN's relay (KEY "-": none): "address
//                                               A.B.C.D"; with ADDRESS a datagram "hello" to that cabinet,
//                                               without it the first datagram that comes in 5 s: "got TEXT"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>

#include "python/netrelay.h"
#include "shell/keyed.h"
#include "shell/net.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

using namespace hk;

int main(int argc, char **argv) {
    if (argc < 5) return 2;
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);   // (a request's line ends are its own)
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    const std::string mode = argv[1], host = argv[2];
    const uint16_t port = (uint16_t)atoi(argv[3]);
    const net::Key key(strcmp(argv[4], "-") ? argv[4] : "");
    std::string why;
    if (mode == "call" || mode == "garbled") {
        const std::string req(std::istreambuf_iterator<char>(std::cin), {});
        const int fd = net::connect_to(host, port, 5, &why);
        if (fd < 0) { printf("error: %s\n", why.c_str()); return 1; }
        std::string answer;
        char buf[4096];
        if (mode == "call") {
            net::Keyed k;
            if (!k.connect(fd, key, &why)) { printf("error: %s\n", why.c_str()); return 1; }
            k.send(req.data(), req.size());
            for (ssize_t n; (n = k.recv(buf, sizeof buf)) > 0;) answer.append(buf, (size_t)n);
            if (k.wrong()) { printf("error: an answer that isn't the key's\n"); return 1; }
        } else {
            uint8_t hello[net::keyed::kHello], theirs[net::keyed::kServerHello];
            net::Sealer out;
            net::Opener in;
            net::keyed::client_hello(hello);
            net::send(fd, hello, sizeof hello);
            for (size_t got = 0; got < sizeof theirs;) {
                const ssize_t n = net::recv(fd, theirs + got, sizeof theirs - got);
                if (n <= 0) { printf("error: no hello\n"); return 1; }
                got += (size_t)n;
            }
            if (net::keyed::client_take(key, hello, theirs, out, in) != 0) { printf("error: the hello\n"); return 1; }
            std::string wire, raw;
            out.seal(req.data(), req.size(), wire);
            wire[wire.size() / 2] ^= 4;
            net::send(fd, wire.data(), wire.size());
            for (ssize_t n; (n = net::recv(fd, buf, sizeof buf)) > 0;) raw.append(buf, (size_t)n);
            in.open(raw, answer);
        }
        if (answer.empty()) { printf("error: no answer\n"); return 1; }
        fwrite(answer.data(), 1, answer.size(), stdout);
        return 0;
    }
    if (mode == "forward") {
        net::KeyForward f;
        if (!f.start(host, port, key, &why)) { printf("error: %s\n", why.c_str()); return 1; }
        printf("port %u\n", f.port());
        fflush(stdout);
        while (std::cin.get() != EOF) {}
        return 0;
    }
    if (mode == "relay") {
        py::RelayClient r;
        if (!r.open(host, port, 3, &why, key)) { printf("error: %s\n", why.c_str()); return 1; }
        const uint32_t a = r.address();
        printf("address %u.%u.%u.%u\n", a >> 24, a >> 16 & 255, a >> 8 & 255, a & 255);
        fflush(stdout);
        std::vector<py::LanCable::Packet> out, in;
        if (argc > 5) {
            unsigned b[4];
            sscanf(argv[5], "%u.%u.%u.%u", &b[0], &b[1], &b[2], &b[3]);
            out.push_back({a, b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3], 4000, 4001, {'h', 'e', 'l', 'l', 'o'}});
            r.exchange(out, in);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            return 0;
        }
        for (int i = 0; i < 100 && in.empty(); i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            r.exchange(out, in);
        }
        if (in.empty()) { printf("error: no datagram\n"); return 1; }
        printf("got %s\n", std::string(in[0].data.begin(), in[0].data.end()).c_str());
        return 0;
    }
    return 2;
}
