// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later

// How far OpenPak serves a title's online play, shown beside the emulator's own playability.
// Ported from Ryujinx (Systems/OpenPak/OpenPakCompatibility.cs). compatibility_list.inc is
// generated from the website's catalogue by tools/update-compatibility.py, the same list Ryujinx
// embeds; Refresh() lays what the site says right now over it, so a title promoted since the
// build is live without a new one.

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace openpak::compatibility {

enum class Status { Live, Beta, Alpha };

struct Entry {
    Status status = Status::Alpha;
    std::string name;
    std::string backend; ///< What serves it (NEX, NPLN, ...); empty for a title only the site knows.
};

/// The title's status, the site's if Refresh() heard one, else the one this build shipped with.
std::optional<Entry> Find(std::uint64_t title_id);

/// "live", "beta" or "alpha" -> the status; anything else is not a playable status.
std::optional<Status> Parse(const std::string& status);

/// The word for a status, as the site spells it.
const char* Name(Status status);

/// Asks the site (GET /api/v1/titles). Failure keeps what is there. True when the site says
/// something this build did not, so a list already drawn is out of date. Blocking.
bool Refresh();

/// Refresh's second half, on statuses already fetched (title id hex -> status); for tests.
bool Apply(const std::map<std::string, std::string>& statuses);

} // namespace openpak::compatibility
