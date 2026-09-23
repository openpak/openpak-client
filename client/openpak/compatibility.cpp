// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later

#include "openpak/compatibility.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <string_view>

#include "openpak/api.h"
#include "openpak/log.h"

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

std::mutex g_mutex;
std::map<std::uint64_t, Status> g_live; ///< What the site said at the last Refresh.

const Shipped* FindShipped(std::uint64_t title_id) {
    const auto it = std::find_if(std::begin(kShipped), std::end(kShipped),
                                 [title_id](const Shipped& s) { return s.title_id == title_id; });
    return it == std::end(kShipped) ? nullptr : it;
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
    return Apply(WebService::OpenPakApi::CatalogueStatuses());
}

} // namespace openpak::compatibility
