// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later

// The NAT type test a Switch runs from System Settings -> Internet -> Test Connection. Ported
// from Ryujinx's OpenPakNatCheck.cs.
//
// Only qlaunch's nn::netdiag names a NAT type on a console -- no system service answers one, and
// a game that wants it runs the same nncs exchange through its own sockets, which the DNS
// redirect points at OpenPak's nn-nncs. This runs that test from the host's UDP stack (the stack
// the emulated console's sockets use) against the two servers the redirect names: nncs1 on the
// main server and nncs2 on an address of its own.
//
// A probe is 16 bytes, four big-endian u32s (type, 0, 0, 0); the reply echoes the type with the
// port and address the server saw. Type 1 is answered from where it was sent, type 2 from the
// other server's address, type 3 from the same address on another port.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace openpak::nat {

inline constexpr const char* PrimaryHost = "nncs1-lp1.n.n.srv.nintendo.net";
inline constexpr const char* SecondaryHost = "nncs2-lp1.n.n.srv.nintendo.net";
inline constexpr std::uint16_t PrimaryPort = 10025;
inline constexpr std::uint16_t SecondaryPort = 10125;

enum class Mapping { Unknown, None, EndpointIndependent, AddressDependent, AddressAndPortDependent };
enum class Filtering { Unknown, EndpointIndependent, AddressDependent, AddressAndPortDependent };

struct Result {
    Mapping mapping = Mapping::Unknown;
    Filtering filtering = Filtering::Unknown;
    std::string external; ///< The address the servers saw, dotted quad; empty when none answered.

    /// The console's letter: A for no NAT or a cone anyone spoken to may answer through, B for a
    /// port-restricted cone, C for a per-destination mapping that filters loosely, D for
    /// symmetric and strict, F for no answer. Nintendo's own table is not published; this is the
    /// usual reading of it.
    char Type() const;
};

/// The two servers as the redirect sends a console to them (the applied network profile's
/// overrides, else `server_address`), or nothing when they land on one address.
std::optional<std::pair<std::string, std::string>> Targets(const std::string& server_address);

/// Run the test: type 1 to three endpoints from one socket for the mapping, then types 2 and 3
/// on a fresh socket that has only spoken to the primary, for the filtering. Blocks for a few
/// seconds at most.
Result Run(const std::string& primary, const std::string& secondary,
           std::uint16_t primary_port = PrimaryPort, std::uint16_t secondary_port = SecondaryPort);

const char* Name(Mapping mapping);
const char* Name(Filtering filtering);

} // namespace openpak::nat
