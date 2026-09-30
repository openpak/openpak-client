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

/// How far the emulator itself runs a title, as the catalogue words it. Nothing to do with
/// OpenPak's online Status above: a title can be Live online and Boots here. The five words are
/// the site's, unchanged.
enum class Playability { Playable, InGame, Menus, Boots, Nothing };

/// This emulator's playability for a title -- the site's if Refresh() heard one, else the one this
/// build shipped with. nullopt means untested: no verdict for this emulator, an emulator the
/// catalogue does not name, a word it does not know, or a host that never called
/// Platform::SetClient. Never guess a value from nullopt; say "untested".
std::optional<Playability> FindPlayability(std::uint64_t title_id);

/// "playable", "ingame", "menus", "boots" or "nothing" (any case) -> the verdict; anything else
/// is untested.
std::optional<Playability> ParsePlayability(const std::string& playability);

/// The word for a verdict, as the site spells it. A translation key, not a translation: the
/// emulators already ship the five words and localize them themselves.
const char* PlayabilityName(Playability playability);

/// Asks the site (GET /api/v1/titles). Failure keeps what is there. True when the site says
/// something this build did not, so a list already drawn is out of date. Blocking.
bool Refresh();

/// Refresh's second half, on statuses already fetched (title id hex -> status); for tests.
bool Apply(const std::map<std::string, std::string>& statuses);

/// Apply for playability (title id hex -> client name -> word); for tests. An answer with nothing
/// this build can use keeps the shipped list, like Apply.
bool ApplyPlayability(const std::map<std::string, std::map<std::string, std::string>>& playability);

/// The catalogue's name for a title: what the site said at the last Refresh or LoadNames, else
/// the name this build shipped with, else empty. Never the title id -- a caller with nothing
/// better words it for people, not for a debugger.
std::string CatalogueName(std::uint64_t title_id);

/// Asks the site for the catalogue's names once per run (Refresh does too); the account window
/// calls this off the UI thread before it names anything. Blocking.
void LoadNames();

/// LoadNames' second half, on names already fetched (title id hex -> name); for tests.
void ApplyNames(const std::map<std::string, std::string>& names);

} // namespace openpak::compatibility
