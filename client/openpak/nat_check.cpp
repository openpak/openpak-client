// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later

// Ported from Ryujinx's OpenPakNatCheck.cs.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <map>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
using SocketHandle = SOCKET;
constexpr SocketHandle InvalidSocket = INVALID_SOCKET;
#define OPENPAK_CLOSE_SOCKET closesocket
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
constexpr SocketHandle InvalidSocket = -1;
#define OPENPAK_CLOSE_SOCKET close
#endif

#include "openpak/nat_check.h"
#include "openpak/network_profile.h"

namespace openpak::nat {

namespace {

struct Endpoint {
    std::uint32_t address = 0; ///< Network order.
    std::uint16_t port = 0;    ///< Host order.

    bool operator<(const Endpoint& other) const {
        return std::pair{address, port} < std::pair{other.address, other.port};
    }
    bool operator==(const Endpoint& other) const = default;
};

struct Seen {
    std::uint32_t port = 0;
    std::uint32_t address = 0; ///< Network order.
};

class Socket {
public:
    Socket() {
#ifdef _WIN32
        static const bool started = [] {
            WSADATA data;
            return WSAStartup(MAKEWORD(2, 2), &data) == 0;
        }();
        (void)started;
#endif
        handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (handle != InvalidSocket) {
            sockaddr_in any{};
            any.sin_family = AF_INET;
            ::bind(handle, reinterpret_cast<sockaddr*>(&any), sizeof(any));
        }
    }
    ~Socket() {
        if (handle != InvalidSocket) {
            OPENPAK_CLOSE_SOCKET(handle);
        }
    }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    SocketHandle handle = InvalidSocket;
};

std::uint32_t Be32(const unsigned char* data) {
    return (std::uint32_t{data[0]} << 24) | (std::uint32_t{data[1]} << 16) |
           (std::uint32_t{data[2]} << 8) | std::uint32_t{data[3]};
}

std::optional<std::uint32_t> Parse(const std::string& address) {
    in_addr parsed{};
    if (inet_pton(AF_INET, address.c_str(), &parsed) != 1) {
        return std::nullopt;
    }
    return parsed.s_addr;
}

/// Send one probe type to each target (three rounds, 400 ms apart) and collect replies of that
/// type. With `from` set, only replies from those endpoints count and are keyed by them; with it
/// empty, a reply from anywhere counts -- the point of the filtering probes.
std::map<Endpoint, Seen> Exchange(Socket& socket, std::uint32_t type,
                                  const std::vector<Endpoint>& targets,
                                  const std::vector<Endpoint>& from) {
    std::map<Endpoint, Seen> seen;
    if (socket.handle == InvalidSocket) {
        return seen;
    }

    std::array<unsigned char, 16> probe{};
    probe[0] = static_cast<unsigned char>(type >> 24);
    probe[1] = static_cast<unsigned char>(type >> 16);
    probe[2] = static_cast<unsigned char>(type >> 8);
    probe[3] = static_cast<unsigned char>(type);

    for (int round = 0; round < 3; round++) {
        for (const Endpoint& target : targets) {
            if (!from.empty() && seen.contains(target)) {
                continue;
            }
            sockaddr_in to{};
            to.sin_family = AF_INET;
            to.sin_addr.s_addr = target.address;
            to.sin_port = htons(target.port);
            ::sendto(socket.handle, reinterpret_cast<const char*>(probe.data()),
                     static_cast<int>(probe.size()), 0, reinterpret_cast<sockaddr*>(&to),
                     sizeof(to));
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
        while (true) {
            const auto left = std::chrono::duration_cast<std::chrono::microseconds>(
                deadline - std::chrono::steady_clock::now());
            if (left.count() <= 0) {
                break;
            }
            fd_set read;
            FD_ZERO(&read);
            FD_SET(socket.handle, &read);
            // Each field in its own type: tv_usec is 32-bit on macOS, and a long there is a
            // narrowing error inside braces.
            timeval timeout{static_cast<decltype(timeval::tv_sec)>(left.count() / 1000000),
                            static_cast<decltype(timeval::tv_usec)>(left.count() % 1000000)};
            if (::select(static_cast<int>(socket.handle) + 1, &read, nullptr, nullptr, &timeout) <= 0) {
                break;
            }

            std::array<unsigned char, 64> buffer{};
            sockaddr_in sender{};
            socklen_t sender_size = sizeof(sender);
            const auto received =
                ::recvfrom(socket.handle, reinterpret_cast<char*>(buffer.data()),
                           static_cast<int>(buffer.size()), 0,
                           reinterpret_cast<sockaddr*>(&sender), &sender_size);
            // Windows reports an earlier probe's ICMP unreachable as a failed receive.
            if (received < 16 || Be32(buffer.data()) != type) {
                continue;
            }

            const Endpoint key{sender.sin_addr.s_addr, ntohs(sender.sin_port)};
            if (!from.empty() && std::find(from.begin(), from.end(), key) == from.end()) {
                continue;
            }

            std::uint32_t external = 0;
            std::memcpy(&external, buffer.data() + 8, 4);
            seen[key] = {Be32(buffer.data() + 4), external};

            if (from.empty() || seen.size() == from.size()) {
                return seen;
            }
        }
    }
    return seen;
}

bool IsLocal(std::uint32_t address) {
    if ((ntohl(address) >> 24) == 127) {
        return true;
    }
#ifndef _WIN32
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) {
        return false;
    }
    bool found = false;
    for (ifaddrs* entry = list; entry != nullptr && !found; entry = entry->ifa_next) {
        if (entry->ifa_addr != nullptr && entry->ifa_addr->sa_family == AF_INET) {
            found = reinterpret_cast<sockaddr_in*>(entry->ifa_addr)->sin_addr.s_addr == address;
        }
    }
    freeifaddrs(list);
    return found;
#else
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buffer(size);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    if (GetAdaptersAddresses(AF_INET, 0, nullptr, adapters, &size) != NO_ERROR) {
        return false;
    }
    for (auto* adapter = adapters; adapter != nullptr; adapter = adapter->Next) {
        for (auto* unicast = adapter->FirstUnicastAddress; unicast != nullptr; unicast = unicast->Next) {
            if (reinterpret_cast<sockaddr_in*>(unicast->Address.lpSockaddr)->sin_addr.s_addr == address) {
                return true;
            }
        }
    }
    return false;
#endif
}

std::string Dotted(std::uint32_t address) {
    char text[INET_ADDRSTRLEN]{};
    in_addr value{};
    value.s_addr = address;
    inet_ntop(AF_INET, &value, text, sizeof(text));
    return text;
}

} // namespace

char Result::Type() const {
    switch (mapping) {
    case Mapping::Unknown:
        return 'F';
    case Mapping::None:
        return 'A';
    case Mapping::EndpointIndependent:
        return filtering == Filtering::AddressAndPortDependent || filtering == Filtering::Unknown ? 'B' : 'A';
    default:
        return filtering == Filtering::EndpointIndependent || filtering == Filtering::AddressDependent ? 'C' : 'D';
    }
}

std::optional<std::pair<std::string, std::string>> Targets(const std::string& server_address) {
    const std::string primary = NetworkProfile::RedirectFor(PrimaryHost, server_address).value_or(server_address);
    const std::string secondary = NetworkProfile::RedirectFor(SecondaryHost, server_address).value_or(server_address);
    if (primary.empty() || secondary.empty() || primary == secondary) {
        return std::nullopt;
    }
    return std::pair{primary, secondary};
}

Result Run(const std::string& primary, const std::string& secondary, std::uint16_t primary_port,
           std::uint16_t secondary_port) {
    Result result;
    const auto first_address = Parse(primary);
    const auto second_address = Parse(secondary);
    if (!first_address || !second_address) {
        return result;
    }

    const std::vector<Endpoint> targets{{*first_address, primary_port},
                                        {*first_address, secondary_port},
                                        {*second_address, primary_port}};

    Socket mapping;
    const auto seen = Exchange(mapping, 1, targets, targets);

    const auto first = seen.find(targets[0]);
    if (first == seen.end()) {
        return result;
    }
    result.external = Dotted(first->second.address);

    const auto same_host = seen.find(targets[1]);
    const auto other_host = seen.find(targets[2]);

    if (IsLocal(first->second.address)) {
        result.mapping = Mapping::None;
    } else if (other_host == seen.end()) {
        // One server answering and the other not is a server problem, not a NAT type.
        result.mapping = same_host != seen.end() && same_host->second.port != first->second.port
                             ? Mapping::AddressAndPortDependent
                             : Mapping::EndpointIndependent;
    } else if (other_host->second.port == first->second.port) {
        result.mapping = Mapping::EndpointIndependent;
    } else {
        result.mapping = same_host != seen.end() && same_host->second.port == first->second.port
                             ? Mapping::AddressDependent
                             : Mapping::AddressAndPortDependent;
    }

    Socket filtering;
    if (!Exchange(filtering, 2, {targets[0]}, {}).empty()) {
        result.filtering = Filtering::EndpointIndependent;
    } else if (!Exchange(filtering, 3, {targets[0]}, {}).empty()) {
        result.filtering = Filtering::AddressDependent;
    } else {
        result.filtering = Filtering::AddressAndPortDependent;
    }
    return result;
}

const char* Name(Mapping mapping) {
    switch (mapping) {
    case Mapping::None:
        return "no NAT";
    case Mapping::EndpointIndependent:
        return "endpoint-independent";
    case Mapping::AddressDependent:
        return "address-dependent";
    case Mapping::AddressAndPortDependent:
        return "address-and-port-dependent";
    default:
        return "unknown";
    }
}

const char* Name(Filtering filtering) {
    switch (filtering) {
    case Filtering::EndpointIndependent:
        return "endpoint-independent";
    case Filtering::AddressDependent:
        return "address-dependent";
    case Filtering::AddressAndPortDependent:
        return "address-and-port-dependent";
    default:
        return "unknown";
    }
}

} // namespace openpak::nat
