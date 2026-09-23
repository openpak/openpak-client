// SPDX-License-Identifier: GPL-2.0-or-later
// The NAT type test (openpak/nat_check.h) against a pair of in-process nncs responders on two
// loopback addresses, answering as nn-nncs does: type 1 from where it was asked, type 2 from the
// other server, type 3 from another port on the same one. Ported from Ryujinx's
// OpenPakNatCheckTests.cs.
#undef NDEBUG
#include <cassert>
#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

#include "openpak/nat_check.h"

namespace nat = openpak::nat;

#ifdef __linux__
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

int Udp(const char* address, std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in at{};
    at.sin_family = AF_INET;
    at.sin_port = htons(port);
    inet_pton(AF_INET, address, &at.sin_addr);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&at), sizeof(at)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

std::uint16_t PortOf(int fd) {
    sockaddr_in at{};
    socklen_t size = sizeof(at);
    getsockname(fd, reinterpret_cast<sockaddr*>(&at), &size);
    return ntohs(at.sin_port);
}

struct Responder {
    std::vector<int> fds;
    std::atomic<bool> stop{false};
    std::thread thread;
    std::uint16_t port_a = 0, port_b = 0;

    Responder(bool type2, bool type3) {
        for (;;) {
            const int a1 = Udp("127.0.0.1", 0), b1 = Udp("127.0.0.1", 0);
            port_a = PortOf(a1);
            port_b = PortOf(b1);
            const int a2 = Udp("127.0.0.2", port_a), b2 = Udp("127.0.0.2", port_b);
            const int alt1 = Udp("127.0.0.1", 0), alt2 = Udp("127.0.0.2", 0);
            if (a2 >= 0 && b2 >= 0) {
                fds = {a1, b1, a2, b2, alt1, alt2};
                break;
            }
            for (int fd : {a1, b1, a2, b2, alt1, alt2}) {
                if (fd >= 0) ::close(fd);
            }
        }
        thread = std::thread([this, type2, type3] {
            while (!stop) {
                pollfd polls[4];
                for (int i = 0; i < 4; i++) polls[i] = {fds[i], POLLIN, 0};
                if (::poll(polls, 4, 50) <= 0) continue;
                for (int i = 0; i < 4; i++) {
                    if (!(polls[i].revents & POLLIN)) continue;
                    unsigned char buffer[16];
                    sockaddr_in from{};
                    socklen_t size = sizeof(from);
                    if (::recvfrom(fds[i], buffer, 16, 0, reinterpret_cast<sockaddr*>(&from), &size) != 16) continue;
                    const bool primary = i < 2;
                    unsigned char reply[16]{};
                    std::copy(buffer, buffer + 4, reply);
                    const std::uint16_t port = ntohs(from.sin_port);
                    reply[6] = port >> 8;
                    reply[7] = port & 0xff;
                    std::memcpy(reply + 8, &from.sin_addr.s_addr, 4);
                    int out = fds[i];
                    if (buffer[3] == 2) out = type2 ? fds[primary ? 5 : 4] : -1;
                    if (buffer[3] == 3) out = type3 ? fds[primary ? 4 : 5] : -1;
                    if (out >= 0) ::sendto(out, reply, 16, 0, reinterpret_cast<sockaddr*>(&from), size);
                }
            }
        });
    }
    ~Responder() {
        stop = true;
        thread.join();
        for (int fd : fds) ::close(fd);
    }
};

} // namespace
#endif

int main() {
    assert((nat::Result{nat::Mapping::EndpointIndependent, nat::Filtering::AddressAndPortDependent, ""}.Type() == 'B'));
    assert((nat::Result{nat::Mapping::AddressDependent, nat::Filtering::AddressDependent, ""}.Type() == 'C'));
    assert((nat::Result{nat::Mapping::AddressAndPortDependent, nat::Filtering::AddressAndPortDependent, ""}.Type() == 'D'));
    assert((nat::Result{nat::Mapping::EndpointIndependent, nat::Filtering::EndpointIndependent, ""}.Type() == 'A'));

    // Nothing listening: F.
    assert(nat::Run("127.0.0.1", "127.0.0.2", 9, 9).Type() == 'F');

#ifdef __linux__
    {
        Responder open{true, true};
        const nat::Result result = nat::Run("127.0.0.1", "127.0.0.2", open.port_a, open.port_b);
        assert(result.mapping == nat::Mapping::None);
        assert(result.filtering == nat::Filtering::EndpointIndependent);
        assert(result.external == "127.0.0.1" && result.Type() == 'A');
    }
    {
        Responder only_port{false, true};
        assert(nat::Run("127.0.0.1", "127.0.0.2", only_port.port_a, only_port.port_b).filtering ==
               nat::Filtering::AddressDependent);
    }
    {
        Responder neither{false, false};
        assert(nat::Run("127.0.0.1", "127.0.0.2", neither.port_a, neither.port_b).filtering ==
               nat::Filtering::AddressAndPortDependent);
    }
#endif
    return 0;
}
