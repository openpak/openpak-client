// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later

// The push connection a console holds (Penne/NPNS, nx-baas docs/penne-protocol.md), ported from
// Ryujinx's OpenPakSession.Push.cs and PenneFrames.cs: register a Penne id, take a login ticket,
// and keep a POST to the frontline open, reading the length-prefixed FlatBuffers frames the
// server streams. A delivered friend request, acceptance, removal, invitation or presence change
// re-reads the cache it concerns at once, instead of at the heartbeat's next poll, which stays
// as the fallback.
//
// Downlink only, as the console's own frontline body is empty; presence keeps going out on the
// REST PATCH the heartbeat already makes. OPENPAK_NO_PUSH=1 turns it off.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace openpak::client::push {

inline constexpr std::uint8_t TypePutRecord = 0x04;
inline constexpr std::uint8_t TypeHandoverResult = 0x0c;
inline constexpr std::uint8_t TypeReset = 0x0d;

/// The largest frame accepted.
inline constexpr std::uint32_t MaxFrame = 64 * 1024;

/// The envelope's type byte, or nothing when the frame is not a readable envelope.
std::optional<std::uint8_t> EnvelopeType(std::span<const std::uint8_t> frame);

/// A notification: the kind-1 message a PutRecord carries.
struct Message {
    std::string name;
    std::string body;
};

/// The message in a PutRecord frame, or nothing for any other frame. Never reads outside it.
std::optional<Message> ReadMessage(std::span<const std::uint8_t> frame);

/// The event kind a message names: the body's "type", else the message name.
std::string Kind(const Message& message);

/// Splits a byte stream into frames (four-byte little-endian length, then the frame).
class FrameReader {
public:
    /// Take more bytes. False when the stream is broken (a zero or oversize length).
    bool Feed(std::span<const std::uint8_t> bytes);

    /// The next whole frame, if one has arrived.
    std::optional<std::string> Next();

private:
    std::string buffer;
};

/// Hold the connection on a thread of its own, for whoever is signed in; idempotent. The
/// session's heartbeat calls it, so a host never has to.
void Start();

/// End the connection and the thread. The session calls it on the way out.
void Stop();

/// The account changed: the connection speaking for the old one ends now.
void Drop();

/// Whether a frontline connection is open right now.
bool Connected();

/// A deleted profile's registration, on every server it had one on.
void ForgetProfile(const std::string& profile);

} // namespace openpak::client::push
