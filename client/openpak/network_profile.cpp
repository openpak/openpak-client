// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdio>
#include <map>
#include <mutex>
#include <vector>

#include <fmt/format.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include "openpak/log.h"
#include "openpak/network_profile.h"
#include "openpak/platform.h"

namespace openpak::client::profile {

namespace {

struct Profile {
    bool loaded = false;
    std::string server;
    std::vector<std::string> suffixes;
    std::vector<std::string> exact;
    std::vector<std::string> never;
    std::map<std::string, std::string> overrides;
};

std::mutex g_mutex;
Profile g_profile;

// The names a console asks for that OpenPak answers to, and the one address that is not the
// server: true until a profile says otherwise, exactly as the Ryujinx build carries it.
const std::vector<std::string> BuiltInSuffixes = {
    ".nintendo.net", ".nintendo.com", ".nintendo.co.jp", ".nintendowifi.net",
    ".nintendo-europe.com",
};

// The ceiling, shipped and never read from the network: the profile chooses WITHIN these
// families and a name outside them is dropped however the server phrased it. Widening this is
// widening what a compromised or mistaken profile can redirect.
const std::vector<std::string> AllowedFamilies = {
    ".nintendo.net",       ".nintendo.com",  ".nintendo.co.jp", ".nintendowifi.net",
    ".nintendo-europe.com", ".gamespy.com",  ".openpak.org",
    // Third-party services OpenPak serves in place of their retail ones: a title that matchmakes
    // outside Nintendo's own names can only be redirected if its family is one this build is
    // willing to be told about.
    ".among.us",           ".photonengine.io",
};

// Names with an address of their own. Empty on purpose: splitting the NAT check's two probes
// across two hosts is what a console does on retail, but against OpenPak it is what stops a
// title dead -- Ryujinx works with both probes landing on the server, and this build only
// diverged from that because it applied the entry below even when a profile was in force.
// The profile is where an address of a name's own belongs if one is ever needed again.
const std::map<std::string, std::string> BuiltInOverrides = {};

std::filesystem::path StorePath() {
    return Platform::ConfigDir() / "openpak" / "network-profile.json";
}

std::filesystem::path EtagPath() {
    return Platform::ConfigDir() / "openpak" / "network-profile.etag";
}

/// Inside a shipped family, suffix or exact name alike.
bool WithinCeiling(const std::string& name);

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

/// A console's own hostnames carry an environment segment ("-lp1", ".lp1."), and the profile
/// names them the way the server does. Compared case-insensitively and by suffix, which is how
/// the wildcard list has always worked.
bool Matches(const std::string& host, const std::string& pattern) {
    const std::string h = Lower(host);
    const std::string p = Lower(pattern);

    if (p.empty()) {
        return false;
    }
    if (p.front() == '.') {
        return h.size() > p.size() && h.compare(h.size() - p.size(), p.size(), p) == 0;
    }
    return h == p;
}

bool WithinCeiling(const std::string& name) {
    const std::string lowered = Lower(name);

    for (const std::string& family : AllowedFamilies) {
        if (lowered == family || lowered.ends_with(family)) {
            return true;
        }
    }

    return false;
}

/// A literal address, and not one that points back at the machine itself.
bool UsableServerAddress(const std::string& address) {
    if (address.empty()) {
        return false;
    }

    unsigned a{}, b{}, c{}, d{};
    if (std::sscanf(address.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) {
        return false; // not a literal IPv4; IPv6 literals are not what OpenPak publishes
    }
    if (a > 255 || b > 255 || c > 255 || d > 255) {
        return false;
    }

    const bool loopback = a == 127;
    const bool link_local = a == 169 && b == 254;
    const bool unspecified = a == 0;

    return !loopback && !link_local && !unspecified;
}

bool Adopt(const nlohmann::json& json) {
    Profile parsed;

    if (json.contains("server") && json["server"].contains("address")) {
        parsed.server = json["server"]["address"].get<std::string>();
    }

    if (json.contains("redirect")) {
        const nlohmann::json& redirect = json["redirect"];

        for (const char* key : {"suffixes", "exact", "never"}) {
            if (!redirect.contains(key) || !redirect[key].is_array()) {
                continue;
            }
            for (const nlohmann::json& entry : redirect[key]) {
                if (!entry.is_string()) {
                    continue;
                }
                if (std::string_view{key} == "suffixes") {
                    parsed.suffixes.push_back(entry.get<std::string>());
                } else if (std::string_view{key} == "exact") {
                    parsed.exact.push_back(entry.get<std::string>());
                } else {
                    parsed.never.push_back(entry.get<std::string>());
                }
            }
        }

        if (redirect.contains("overrides") && redirect["overrides"].is_object()) {
            for (const auto& [name, address] : redirect["overrides"].items()) {
                if (address.is_string()) {
                    parsed.overrides.emplace(name, address.get<std::string>());
                }
            }
        }
    }

    // Rejected whole, never in part: a profile that names anything outside the shipped families,
    // or a server address that is not a usable literal, is a log line and a fallback.
    if (!UsableServerAddress(parsed.server)) {
        OPENPAK_LOG_WARNING("[OpenPak] Network profile refused: server address '{}'", parsed.server);
        return false;
    }

    for (const std::vector<std::string>* list : {&parsed.suffixes, &parsed.exact}) {
        for (const std::string& name : *list) {
            if (!WithinCeiling(name)) {
                OPENPAK_LOG_WARNING("[OpenPak] Network profile refused: '{}' is outside the "
                                    "families this build ships with", name);
                return false;
            }
        }
    }

    parsed.loaded = true;

    std::lock_guard lock{g_mutex};
    g_profile = std::move(parsed);

    OPENPAK_LOG_INFO("[OpenPak] Network profile: {} suffix(es), {} exact, {} never, {} override(s)",
                     g_profile.suffixes.size(), g_profile.exact.size(), g_profile.never.size(),
                     g_profile.overrides.size());

    return true;
}

} // namespace

void Load() {
    const std::string stored = Platform::ReadFile(StorePath());
    if (stored.empty()) {
        return;
    }

    const nlohmann::json parsed = nlohmann::json::parse(stored, nullptr, false);
    if (!parsed.is_discarded() && Adopt(parsed)) {
        OPENPAK_LOG_INFO("[OpenPak] Network profile: cached, version {}",
                         parsed.value("version", 0));
    }
}

bool Refresh(const std::string& website_url, const std::string& platform) {
    std::string host = website_url.empty() ? "https://openpak.org" : website_url;
    const bool https = host.rfind("https://", 0) == 0;

    host.erase(0, host.find("//") == std::string::npos ? 0 : host.find("//") + 2);

    httplib::Client client{(https ? "https://" : "http://") + host};
    // Two seconds is generous, and a launch is never blocked on this.
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(2, 0);
    client.set_follow_location(true);

    httplib::Headers headers;
    const std::string etag = Platform::ReadFile(EtagPath());
    if (!etag.empty()) {
        headers.emplace("If-None-Match", etag);
    }

    const auto result =
        client.Get(fmt::format("/api/v1/network/profile?platform={}", platform), headers);

    if (result && result->status == 304) {
        OPENPAK_LOG_INFO("[OpenPak] Network profile: unchanged since the stored copy");
        Load();
        return true;
    }

    if (!result || result->status != 200) {
        OPENPAK_LOG_WARNING("[OpenPak] Network profile unavailable; using the built-in list");
        Load();
        return false;
    }

    const nlohmann::json parsed = nlohmann::json::parse(result->body, nullptr, false);
    if (parsed.is_discarded()) {
        return false;
    }

    if (!Adopt(parsed)) {
        Load(); // the stored copy, or the built-in list, but never half of a bad one
        return false;
    }

    Platform::WriteFile(StorePath(), result->body);
    Platform::WriteFile(EtagPath(), result->get_header_value("ETag"));

    OPENPAK_LOG_INFO("[OpenPak] Network profile: fetched, version {}", parsed.value("version", 0));

    return true;
}

bool Loaded() {
    std::lock_guard lock{g_mutex};
    return g_profile.loaded;
}

std::optional<std::string> RedirectFor(const std::string& host, const std::string& server_address) {
    std::lock_guard lock{g_mutex};

    // Names that must reach the real internet win over every wildcard below them: the console's
    // own connection test is the example, and pointing it at OpenPak measures OpenPak.
    for (const std::string& never : g_profile.never) {
        if (Matches(host, never)) {
            return std::nullopt;
        }
    }

    // An address of its own, before the wildcards collapse it onto the server.
    for (const auto& [name, address] : g_profile.overrides) {
        if (Matches(host, name)) {
            return address;
        }
    }
    for (const auto& [name, address] : BuiltInOverrides) {
        if (Matches(host, name)) {
            return address;
        }
    }

    const std::string& server = g_profile.server.empty() ? server_address : g_profile.server;

    for (const std::string& exact : g_profile.exact) {
        if (Matches(host, exact)) {
            return server;
        }
    }
    for (const std::string& suffix : g_profile.suffixes) {
        if (Matches(host, suffix)) {
            return server;
        }
    }

    // No profile in hand: the built-in list is what a fresh install redirects with.
    if (!g_profile.loaded) {
        for (const std::string& suffix : BuiltInSuffixes) {
            if (Matches(host, suffix)) {
                return server_address;
            }
        }
    }

    return std::nullopt;
}

} // namespace openpak::client::profile
