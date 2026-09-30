// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later

#include "openpak/compatibility.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <string_view>

#include "openpak/api.h"
#include "openpak/log.h"
#include "openpak/platform.h"

namespace openpak::compatibility {

namespace {

struct Shipped {
    std::uint64_t title_id;
    std::string_view name;
    std::string_view backend;
    std::string_view status;
};

constexpr Shipped kShipped[] = {
#include "openpak/compatibility_list.inc"
};

struct ShippedPlay {
    std::uint64_t title_id;
    std::string_view client;
    std::string_view playability;
};

constexpr ShippedPlay kShippedPlay[] = {
    // The site may have no verdict for anything yet, and C++ has no empty array; title id 0 is no
    // title, so this row never matches.
    {0, "", ""},
#include "openpak/playability_list.inc"
};

std::mutex g_mutex;
std::map<std::uint64_t, Status> g_live; ///< What the site said at the last Refresh.
/// What the site said at the last Refresh about THIS emulator, the only key it reads.
std::map<std::uint64_t, Playability> g_live_play;
std::map<std::uint64_t, std::string> g_names; ///< The catalogue's names, from the site.
bool g_names_loaded = false;

const Shipped* FindShipped(std::uint64_t title_id) {
    const auto it = std::find_if(std::begin(kShipped), std::end(kShipped),
                                 [title_id](const Shipped& s) { return s.title_id == title_id; });
    return it == std::end(kShipped) ? nullptr : it;
}

/// Which emulator this build is, as the catalogue keys it, folded for comparison; empty before
/// Platform::SetClient, and then nothing has a verdict.
std::string ThisClient() {
    return Platform::ToLower(Platform::ClientName());
}

std::optional<Playability> FindShippedPlay(std::uint64_t title_id, const std::string& client) {
    for (const ShippedPlay& row : kShippedPlay) {
        if (row.title_id == title_id && Platform::ToLower(std::string{row.client}) == client) {
            return ParsePlayability(std::string{row.playability});
        }
    }
    return std::nullopt;
}

/// A title id as the catalogue writes it, 16 hex digits; nullopt for anything else.
std::optional<std::uint64_t> ParseTitleId(const std::string& id) {
    if (id.size() != 16) {
        return std::nullopt;
    }
    try {
        return std::stoull(id, nullptr, 16);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

} // namespace

std::optional<Status> Parse(const std::string& status) {
    std::string lower = status;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower == "live") {
        return Status::Live;
    }
    if (lower == "beta") {
        return Status::Beta;
    }
    if (lower == "alpha") {
        return Status::Alpha;
    }
    return std::nullopt;
}

const char* Name(Status status) {
    switch (status) {
    case Status::Live:
        return "live";
    case Status::Beta:
        return "beta";
    case Status::Alpha:
        return "alpha";
    }
    return "alpha";
}

std::optional<Playability> ParsePlayability(const std::string& playability) {
    const std::string lower = Platform::ToLower(playability);
    if (lower == "playable") {
        return Playability::Playable;
    }
    if (lower == "ingame") {
        return Playability::InGame;
    }
    if (lower == "menus") {
        return Playability::Menus;
    }
    if (lower == "boots") {
        return Playability::Boots;
    }
    if (lower == "nothing") {
        return Playability::Nothing;
    }
    return std::nullopt;
}

const char* PlayabilityName(Playability playability) {
    switch (playability) {
    case Playability::Playable:
        return "playable";
    case Playability::InGame:
        return "ingame";
    case Playability::Menus:
        return "menus";
    case Playability::Boots:
        return "boots";
    case Playability::Nothing:
        return "nothing";
    }
    return "nothing";
}

std::optional<Playability> FindPlayability(std::uint64_t title_id) {
    const std::string client = ThisClient();
    if (client.empty()) {
        return std::nullopt; // no host said which emulator this is: nothing is tested here
    }
    {
        std::lock_guard lock{g_mutex};
        if (const auto it = g_live_play.find(title_id); it != g_live_play.end()) {
            return it->second;
        }
    }
    // ponytail: the site can add and change a verdict, not take one away -- a title the site
    // stops mentioning keeps the shipped one until the next release regenerates the list. Storing
    // "the site answered about this title at all" would fix it; nobody has withdrawn a verdict.
    return FindShippedPlay(title_id, client);
}

bool ApplyPlayability(const std::map<std::string, std::map<std::string, std::string>>& playability) {
    const std::string client = ThisClient();
    std::map<std::uint64_t, Playability> live;
    if (!client.empty()) {
        for (const auto& [id, by_client] : playability) {
            const auto title_id = ParseTitleId(id);
            if (!title_id) {
                continue;
            }
            for (const auto& [name, word] : by_client) {
                // Only this emulator's key, and only one of the five words.
                if (Platform::ToLower(name) != client) {
                    continue;
                }
                if (const auto parsed = ParsePlayability(word)) {
                    live[*title_id] = *parsed;
                }
            }
        }
    }
    if (live.empty()) {
        // Nothing for this build is a site that could not say, not a build where nothing runs.
        OPENPAK_LOG_INFO("[OpenPak] Catalogue playability: nothing for this client, keeping the built-in list");
        return false;
    }

    bool changed = false;
    for (const auto& [id, verdict] : live) {
        if (FindShippedPlay(id, client) != verdict) {
            changed = true;
            break;
        }
    }
    const std::size_t count = live.size();
    {
        std::lock_guard lock{g_mutex};
        g_live_play = std::move(live);
    }
    OPENPAK_LOG_INFO("[OpenPak] Catalogue playability: {} title(s) for this client, {}", count,
                     changed ? "at least one differs from this build" : "all matching this build");
    return changed;
}

std::optional<Entry> Find(std::uint64_t title_id) {
    const Shipped* shipped = FindShipped(title_id);
    std::optional<Status> live;
    {
        std::lock_guard lock{g_mutex};
        if (const auto it = g_live.find(title_id); it != g_live.end()) {
            live = it->second;
        }
    }
    if (!shipped && !live) {
        return std::nullopt;
    }
    Entry entry;
    if (shipped) {
        entry.name = shipped->name;
        entry.backend = shipped->backend;
        entry.status = Parse(std::string{shipped->status}).value_or(Status::Alpha);
    }
    if (live) {
        entry.status = *live;
    }
    return entry;
}

bool Apply(const std::map<std::string, std::string>& statuses) {
    std::map<std::uint64_t, Status> live;
    for (const auto& [id, word] : statuses) {
        const auto status = Parse(word);
        if (!status || id.size() != 16) {
            continue;
        }
        try {
            live[std::stoull(id, nullptr, 16)] = *status;
        } catch (const std::exception&) {
        }
    }
    if (live.empty()) {
        // An empty answer is a site that could not say, not one where nothing is playable.
        OPENPAK_LOG_INFO("[OpenPak] Catalogue statuses: none from the site, keeping the built-in list");
        return false;
    }

    bool changed = false;
    for (const auto& [id, status] : live) {
        const Shipped* shipped = FindShipped(id);
        if (!shipped || Parse(std::string{shipped->status}) != status) {
            changed = true;
            break;
        }
    }
    {
        std::lock_guard lock{g_mutex};
        g_live = std::move(live);
    }
    OPENPAK_LOG_INFO("[OpenPak] Catalogue statuses: {} from the site, {}", statuses.size(),
                     changed ? "at least one differs from this build" : "all matching this build");
    return changed;
}

bool Refresh() {
    std::map<std::string, std::string> statuses;
    std::map<std::string, std::string> names;
    std::map<std::string, std::map<std::string, std::string>> playability;
    for (const auto& [id, title] : WebService::OpenPakApi::Catalogue()) {
        if (!title.status.empty()) {
            statuses[id] = title.status;
        }
        names[id] = title.name;
        if (!title.playability.empty()) {
            playability[id] = title.playability;
        }
    }
    ApplyNames(names);
    const bool status_news = Apply(statuses);
    const bool play_news = ApplyPlayability(playability); // both run: neither may be short-circuited
    return status_news || play_news;
}

void ApplyNames(const std::map<std::string, std::string>& names) {
    std::map<std::uint64_t, std::string> parsed;
    for (const auto& [id, name] : names) {
        if (id.size() != 16 || name.empty()) {
            continue;
        }
        try {
            parsed[std::stoull(id, nullptr, 16)] = name;
        } catch (const std::exception&) {
        }
    }
    std::lock_guard lock{g_mutex};
    // An empty answer is a site that could not say; the names already known stay.
    if (!parsed.empty()) {
        g_names = std::move(parsed);
        g_names_loaded = true;
    }
}

void LoadNames() {
    {
        std::lock_guard lock{g_mutex};
        if (g_names_loaded) {
            return;
        }
    }
    std::map<std::string, std::string> names;
    for (const auto& [id, title] : WebService::OpenPakApi::Catalogue()) {
        names[id] = title.name;
    }
    ApplyNames(names);
}

std::string CatalogueName(std::uint64_t title_id) {
    {
        std::lock_guard lock{g_mutex};
        if (const auto it = g_names.find(title_id); it != g_names.end()) {
            return it->second;
        }
    }
    const Shipped* shipped = FindShipped(title_id);
    return shipped ? std::string{shipped->name} : std::string{};
}

} // namespace openpak::compatibility
