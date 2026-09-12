// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The network profile: what OpenPak is and what a client should send it, fetched once per
// launch instead of compiled into six emulators (prds/emulator-network-profile-prd.md).
//
// The contract, in one breath: one conditional GET, two seconds, never in a game's way. A
// fetched profile is validated against the compiled-in allowlist — the ceiling — before
// anything is applied; last-known-good beats a fetch and the built-in default beats nothing,
// and a game starts either way.

#pragma once

#include <optional>
#include <string>
#include <vector>

namespace openpak::NetworkProfile {

struct Service {
    std::string id;   // "act", "ecs", "nus", "ias", "ccs", "ccsu", "idbe", "boss", "tagaya", "olv"
    std::string url;  // what a client that addresses a URL (Cemu) uses for that service
};

struct Profile {
    long long version = 0;
    long long generated_at = 0;
    std::string platform;

    std::string server_address;  // a literal IPv4/IPv6 address, never a name
    int https_port = 443;
    int http_port = 80;

    std::string ca_url;        // empty means "no CA shipping in this profile"
    std::string ca_sha256;     // hex; the CA is only trusted when its digest matches
    long long ca_not_after = 0;

    std::vector<std::string> suffixes;  // ".nintendo.net" — every name under the family
    std::vector<std::string> exact;     // "gamesync.npln.nintendo.net" — one name
    std::vector<std::string> never;     // names that must reach the real internet

    std::vector<Service> services;
    long long recheck_after = 21600;    // seconds; a hint, not a contract
};

// Where the applied profile came from. A player's "it can't connect" is answered by logging
// this and the profile version — see Fetch().
enum class Source {
    Fetched,  // answered 200, validated, applied and cached
    Cached,   // 304, or the fetch failed and last-known-good is in use
    BuiltIn,  // nothing stored; the compiled-in default is in use
};

struct Result {
    bool ok = false;        // false only when even the built-in default could not be produced,
                            // which currently cannot happen; kept for future-proofing callers
    Source source = Source::BuiltIn;
    Profile profile;
    std::string error;      // why the fetch or the profile fell back, for the log
};

// The compiled-in ceiling: the domain families this emulator will ever redirect. The profile
// chooses within it; an entry outside it rejects the whole profile. Shipped as a constant so
// it is visible in review and can never be read from the network.
std::vector<std::string> const& AllowedFamilies();

// Whether one name is inside the ceiling, matched on a label boundary: "a.nintendo.net" is
// inside ".nintendo.net", "evila.nintendo.net.evil.example" is not.
bool NameAllowed(std::string const& host);

// The whole of §2 of the PRD. One conditional GET with the stored ETag; 200 validates, stores
// and applies; 304 applies the stored profile; anything else applies the stored profile; no
// stored profile means the built-in default. Times out in two seconds, never retries.
// `platform` is one of switch, wiiu, 3ds, wii, ds.
Result Fetch(std::string const& platform);

// "fetched", "cached" or "built-in" — the word the log line and a status row both want.
char const* SourceName(Source s);

// The "Refresh network settings" action: the same flow, ignoring recheck_after. Still never
// blocks a launch, because only a person asks for it.
Result Refresh(std::string const& platform);

// The compiled-in default, for hosts without a list of their own. Hosts that already ship one
// (Ryujinx's wildcards, Dolphin's suffixes, Cemu's URLs) keep theirs — the profile replaces
// what they use at runtime, this only replaces what they fall back to.
Profile BuiltIn(std::string const& platform);

// Parse and validate. Parse errors and validation failures both land in `error`; a rejected
// profile is never partially applied — the caller falls back whole.
std::optional<Profile> Parse(std::string const& json, std::string& error);
std::optional<std::string> Validate(Profile const& profile, std::string const& expected_platform);

// The stored profile and its ETag, next to the other OpenPak settings. Exposed for hosts that
// want to show what is in effect without fetching.
std::optional<Profile> LoadStored(std::string const& platform);
bool Store(std::string const& platform, Profile const& profile, std::string const& etag);

// Downloads profile.ca_url over public TLS and keeps it only when the SHA-256 matches
// ca_sha256, written where the SSL service and the redirected names expect it
// (config/openpak/ca.pem). Best effort: a failure is a log line, the stored CA — or none —
// stays whatever it was.
bool EnsureCA(Profile const& profile);

} // namespace openpak::NetworkProfile
