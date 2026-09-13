// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later

// Which names the emulated console redirects and where they go, as OpenPak publishes it.
//
// The built-in list is a fallback, not the truth: OpenPak generates this from its own live
// routing, so a title served on a new hostname works without a new emulator build. Two parts
// matter beyond the wildcards -- an override sends one name to a different address (the NAT
// check compares what TWO addresses observe of one console, so its second probe must not
// collapse onto the first), and `never` names must reach the real internet, because a
// connection test pointed at OpenPak measures OpenPak.

#pragma once

#include <optional>
#include <string>

namespace openpak::client::profile {

/// Fetch the profile for this platform and keep it on disk. Best effort, never blocks a launch.
bool Refresh(const std::string& website_url, const std::string& platform);

/// Load whatever was stored, so a launch without network still redirects.
void Load();

/// Where `host` should resolve, or nothing when it must be left alone. `server_address` is what
/// the wildcards resolve to when the profile itself names no server.
std::optional<std::string> RedirectFor(const std::string& host, const std::string& server_address);

/// True when a profile (stored or fetched) is in hand rather than the built-in list.
bool Loaded();

} // namespace openpak::client::profile
