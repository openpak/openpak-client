// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The network profile, implemented against the contract in network_profile.h
// (emulators/prds/emulator-network-profile-prd.md §2 and §3, docs/signed-ceiling.md). One
// conditional GET, two seconds, no retries; the signed ceiling (network_ceiling.cpp) bounds what
// is applied, name by name; last-known-good beats a fetch and the built-in default beats nothing.
#include "openpak/network_profile.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <future>
#include <fstream>
#include <iterator>
#include <mutex>

#include <fmt/format.h>
#include <httplib.h>  // brings the platform's socket headers (inet_pton) on Windows too
#include <nlohmann/json.hpp>
#include <openssl/sha.h>

#include "openpak/api.h"
#include "openpak/log.h"
#include "openpak/platform.h"

namespace openpak::NetworkProfile {

namespace {

std::filesystem::path StoredProfilePath() {
    return Platform::ConfigDir() / "openpak_network_profile.json";
}

std::filesystem::path StoredEtagPath() {
    return Platform::ConfigDir() / "openpak_network_profile.etag";
}

bool IsLiteralAddress(std::string const& address) {
    in_addr v4{};
    in6_addr v6{};
    return inet_pton(AF_INET, address.c_str(), &v4) == 1 ||
           inet_pton(AF_INET6, address.c_str(), &v6) == 1;
}

bool IsLoopbackOrLinkLocal(std::string const& address) {
    unsigned char v4[4]{};
    unsigned char v6[16]{};
    if (inet_pton(AF_INET, address.c_str(), v4) == 1) {
        return v4[0] == 127 ||                            // 127.0.0.0/8
               (v4[0] == 169 && v4[1] == 254) ||          // 169.254.0.0/16
               v4[0] == 0 ||                              // this-network
               (v4[0] == 255 && v4[1] == 255 && v4[2] == 255 && v4[3] == 255);  // broadcast
    }
    if (inet_pton(AF_INET6, address.c_str(), v6) == 1) {
        bool zero_prefix = true;  // :: and ::1 share fifteen zero bytes
        for (int i = 0; i < 15; ++i) {
            zero_prefix = zero_prefix && v6[i] == 0;
        }
        if (zero_prefix && (v6[15] == 0 || v6[15] == 1)) {
            return true;
        }
        return v6[0] == 0xFE && (v6[1] & 0xC0) == 0x80;  // fe80::/10
    }
    return true;  // not an address at all: treat as unusable
}

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

// A pattern starting with a dot matches every name under it; anything else matches one name.
// Case-insensitive, as DNS is.
bool Matches(std::string const& host, std::string const& pattern) {
    std::string const h = Lower(host);
    std::string const p = Lower(pattern);
    if (p.empty()) {
        return false;
    }
    if (p.front() == '.') {
        return h.size() > p.size() && h.compare(h.size() - p.size(), p.size(), p) == 0;
    }
    return h == p;
}

std::mutex g_applied_mutex;
std::optional<Result> g_applied;

// A fresh response body and its ETag, saved beside the other OpenPak settings.
void WriteStored(std::string const& body, std::string const& etag) {
    Platform::CreateParentDirs(StoredProfilePath());
    {
        std::ofstream f(StoredProfilePath(), std::ios::binary | std::ios::trunc);
        f.write(body.data(), static_cast<std::streamsize>(body.size()));
    }
    {
        std::ofstream f(StoredEtagPath(), std::ios::binary | std::ios::trunc);
        f.write(etag.data(), static_cast<std::streamsize>(etag.size()));
    }
}

std::string ReadStoredEtag() {
    std::ifstream f(StoredEtagPath(), std::ios::binary);
    if (!f) {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

}  // namespace

std::optional<Profile> Parse(std::string const& json, std::string& error) {
    try {
        auto const doc = nlohmann::json::parse(json);
        Profile p;
        p.version = doc.value("version", 0LL);
        p.generated_at = doc.value("generated_at", 0LL);
        p.platform = doc.value("platform", std::string{});
        if (doc.contains("server") && doc.at("server").is_object()) {
            auto const& server = doc.at("server");
            p.server_address = server.value("address", std::string{});
            p.https_port = server.value("https_port", 443);
            p.http_port = server.value("http_port", 80);
        }
        if (doc.contains("ca") && doc.at("ca").is_object()) {
            auto const& ca = doc.at("ca");
            p.ca_url = ca.value("url", std::string{});
            p.ca_sha256 = ca.value("sha256", std::string{});
            std::transform(p.ca_sha256.begin(), p.ca_sha256.end(), p.ca_sha256.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            p.ca_not_after = ca.value("not_after", 0LL);
        }
        if (doc.contains("redirect") && doc.at("redirect").is_object()) {
            auto const& redirect = doc.at("redirect");
            for (auto const& s : redirect.value("suffixes", std::vector<std::string>{})) {
                p.suffixes.push_back(s);
            }
            for (auto const& s : redirect.value("exact", std::vector<std::string>{})) {
                p.exact.push_back(s);
            }
            for (auto const& s : redirect.value("never", std::vector<std::string>{})) {
                p.never.push_back(s);
            }
            if (redirect.contains("overrides") && redirect.at("overrides").is_object()) {
                for (auto const& [name, address] : redirect.at("overrides").items()) {
                    if (address.is_string()) {
                        p.overrides.emplace_back(name, address.get<std::string>());
                    }
                }
            }
        }
        if (doc.contains("services") && doc.at("services").is_array()) {
            for (auto const& s : doc.at("services")) {
                p.services.push_back(
                    Service{s.value("id", std::string{}), s.value("url", std::string{})});
            }
        }
        p.recheck_after = doc.value("recheck_after", 21600LL);
        return p;
    } catch (std::exception const& e) {
        error = fmt::format("unparseable profile body: {}", e.what());
        return std::nullopt;
    }
}

std::optional<std::string> Validate(Profile const& profile, std::string const& expected_platform) {
    if (!expected_platform.empty() && !profile.platform.empty() &&
        profile.platform != expected_platform) {
        return fmt::format("profile is for platform {}, not {}", profile.platform,
                           expected_platform);
    }
    if (!IsLiteralAddress(profile.server_address)) {
        return "server.address is not a literal IP address";
    }
    if (IsLoopbackOrLinkLocal(profile.server_address)) {
        return "server.address is a loopback or link-local address";
    }
    // Which names may be redirected is the ceiling's call, name by name (FilterToCeiling); only
    // the addresses they would go to are judged here.
    for (auto const& [name, address] : profile.overrides) {
        if (!IsLiteralAddress(address) || IsLoopbackOrLinkLocal(address)) {
            return fmt::format("override {} does not name a usable literal address", name);
        }
    }
    return std::nullopt;  // valid
}

char const* SourceName(Source s) {
    switch (s) {
    case Source::Fetched: return "fetched";
    case Source::Cached: return "cached";
    case Source::BuiltIn: return "built-in";
    }
    return "built-in";
}

Profile BuiltIn(std::string const& platform) {
    Profile p;
    p.platform = platform;
    p.server_address.clear();  // the host keeps its own compiled-in address
    if (platform == "switch") {
        // What a Switch reaches, exactly as the Ryujinx build carries it.
        p.suffixes = {".nintendo.net", ".nintendo.com", ".nintendo.co.jp", ".nintendowifi.net",
                      ".nintendo-europe.com"};
    } else {
        // What the Wii and DS families redirect today; the profile's job is to keep this fresh,
        // not to define it.
        p.suffixes = {".nintendowifi.net", ".gamespy.com", ".nintendo.net", ".openpak.org"};
    }
    // Inside the ceiling like anything else, and quietly: hosts ask for this per lookup.
    auto const ceiling = CurrentCeiling();
    auto const& families = ceiling.FamiliesFor(platform);
    std::erase_if(p.suffixes, [&](auto const& s) { return !NameInFamilies(s, families); });
    return p;
}

std::optional<Profile> LoadStored(std::string const& platform) {
    std::ifstream f(StoredProfilePath(), std::ios::binary);
    if (!f) {
        return std::nullopt;
    }
    std::string const body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (body.empty()) {
        return std::nullopt;
    }
    std::string error;
    auto parsed = Parse(body, error);
    if (!parsed) {
        OPENPAK_LOG_WARNING("network profile: stored copy unreadable: {}", error);
        return std::nullopt;
    }
    auto invalid = Validate(*parsed, platform);
    if (invalid) {
        OPENPAK_LOG_WARNING("network profile: stored copy rejected: {}", *invalid);
        return std::nullopt;
    }
    FilterToCeiling(*parsed, CurrentCeiling().FamiliesFor(platform));
    return parsed;
}

bool Store(std::string const& platform, Profile const& profile, std::string const& etag) {
    (void)platform;
    try {
        nlohmann::json doc;
        doc["version"] = profile.version;
        doc["generated_at"] = profile.generated_at;
        doc["platform"] = profile.platform;
        doc["server"] = {{"address", profile.server_address},
                         {"https_port", profile.https_port},
                         {"http_port", profile.http_port}};
        if (!profile.ca_url.empty()) {
            doc["ca"] = {{"url", profile.ca_url}, {"sha256", profile.ca_sha256},
                         {"not_after", profile.ca_not_after}};
        }
        doc["redirect"] = {{"suffixes", profile.suffixes},
                           {"exact", profile.exact},
                           {"never", profile.never}};
        if (!profile.overrides.empty()) {
            nlohmann::json overrides = nlohmann::json::object();
            for (auto const& [name, address] : profile.overrides) {
                overrides[name] = address;
            }
            doc["redirect"]["overrides"] = std::move(overrides);
        }
        doc["services"] = nlohmann::json::array();
        for (auto const& s : profile.services) {
            doc["services"].push_back({{"id", s.id}, {"url", s.url}});
        }
        doc["recheck_after"] = profile.recheck_after;
        WriteStored(doc.dump(2), etag);
        return true;
    } catch (std::exception const& e) {
        OPENPAK_LOG_WARNING("network profile: could not store the profile: {}", e.what());
        return false;
    }
}

bool EnsureCA(Profile const& profile) {
    if (profile.ca_url.empty()) {
        return true;  // no CA section: nothing to pin, and NP-2 may not have shipped
    }
    httplib::Client client(WebService::OpenPakApi::BaseUrl());
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(2, 0);
    WebService::OpenPakApi::ApplySystemCa(client);
    auto const result = client.Get(profile.ca_url);
    if (!result || result->status != 200) {
        OPENPAK_LOG_WARNING("network profile: could not download the CA from {}",
                            profile.ca_url);
        return false;
    }
    std::uint8_t digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<unsigned const char*>(result->body.data()), result->body.size(),
           digest);
    std::string hex;
    hex.reserve(SHA256_DIGEST_LENGTH * 2);
    for (std::uint8_t byte : digest) {
        hex += fmt::format("{:02x}", byte);
    }
    if (hex != profile.ca_sha256) {
        OPENPAK_LOG_WARNING("network profile: CA digest mismatch ({} != {})", profile.ca_sha256,
                            hex);
        return false;
    }
    auto const path = Platform::ConfigDir() / "openpak" / "ca.pem";
    Platform::WriteFile(path, result->body);
    OPENPAK_LOG_INFO("network profile: CA pinned at {}", path.string());
    return true;
}

namespace {

// The shared shape of Fetch and Refresh: everything in §2 of the PRD after the request line.
Result Finish(std::string const& platform, httplib::Response const* fetch,
              std::string const& etag_sent) {
    Result out;
    out.profile = BuiltIn(platform);

    auto stored = LoadStored(platform);
    if (stored) {
        out.profile = *stored;
        out.source = Source::Cached;
    }

    if (fetch == nullptr) {
        out.error = "no answer from openpak.org";
        return out;
    }

    auto const status = fetch->status;
    if (status == 304) {
        if (!stored) {
            out.error = "server says the profile is unchanged, but nothing is stored";
        }
        return out;  // stored, or built-in if nothing was stored
    }
    if (status != 200) {
        out.error = fmt::format("HTTP {}", status);
        return out;
    }

    std::string error;
    auto fetched = Parse(fetch->body, error);
    if (!fetched) {
        out.error = error;
        return out;
    }
    auto const invalid = Validate(*fetched, platform);
    if (invalid) {
        out.error = *invalid;
        return out;
    }
    // The CA is only pinned on a fresh profile: this is the one point §3 puts the digest
    // check, and it costs nothing on a 304.
    if (!fetched->ca_url.empty() && !EnsureCA(*fetched)) {
        out.error = "the CA did not match the digest the profile published";
        return out;
    }

    // Stored as the server sent it: a later ceiling may admit what this one drops.
    auto const etag = fetch->get_header_value("ETag");
    Store(platform, *fetched, etag.empty() ? etag_sent : etag);
    FilterToCeiling(*fetched, CurrentCeiling().FamiliesFor(platform));
    out.profile = *fetched;
    out.source = Source::Fetched;
    return out;
}

// A stored copy a 304 could stand on: it parses and validates (its names are filtered later).
bool StoredUsable(std::string const& platform) {
    std::ifstream f(StoredProfilePath(), std::ios::binary);
    if (!f) {
        return false;
    }
    std::string const body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::string error;
    auto const parsed = Parse(body, error);
    return parsed && !Validate(*parsed, platform);
}

// The ceiling first (contract rule 1), fetched beside the profile so the two share one two-second
// wait; the stored profile is only read once the ceiling that filters it is settled.
Result FetchWithCeiling(std::string const& platform, bool conditional, char const* verb) {
    auto ceiling = std::async(std::launch::async, [] { return FetchCeilingEnvelope(); });

    httplib::Client client(WebService::OpenPakApi::BaseUrl());
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(2, 0);
    WebService::OpenPakApi::ApplySystemCa(client);
    httplib::Headers headers;
    std::string const etag = ReadStoredEtag();
    if (conditional && !etag.empty() && StoredUsable(platform)) {
        headers.emplace("If-None-Match", etag);
    }
    auto const result =
        client.Get(fmt::format("/api/v1/network/profile?platform={}", platform), headers);

    LoadCeiling(ceiling.get());
    Result out = Finish(platform, result ? &*result : nullptr, etag);
    Apply(out);
    OPENPAK_LOG_INFO("network profile: {} platform {} -> {} source, version {}", verb, platform,
                     SourceName(out.source), out.profile.version);
    StartWatching(platform);
    return out;
}

}  // namespace

Result Fetch(std::string const& platform) {
    return FetchWithCeiling(platform, true, "fetched");
}

Result Refresh(std::string const& platform) {
    return FetchWithCeiling(platform, false, "refreshed");
}

void Apply(Result const& applied) {
    {
        std::lock_guard lock{g_applied_mutex};
        g_applied = applied;
    }
    SetDigestInUse(EffectiveDigest(applied.profile));
}

bool Loaded() {
    std::lock_guard lock{g_applied_mutex};
    return g_applied && g_applied->source != Source::BuiltIn;
}

std::optional<std::string> RedirectFor(std::string const& host,
                                       std::string const& server_address) {
    std::lock_guard lock{g_applied_mutex};
    if (!g_applied) {
        return std::nullopt;
    }
    Profile const& profile = g_applied->profile;

    // Names that must reach the real internet win over every wildcard below them: the
    // console's own connection test is the example, and pointing it at OpenPak measures OpenPak.
    for (auto const& never : profile.never) {
        if (Matches(host, never)) {
            return std::nullopt;
        }
    }
    // An address of its own, before the wildcards collapse it onto the server.
    for (auto const& [name, address] : profile.overrides) {
        if (Matches(host, name)) {
            return address;
        }
    }
    std::string const& server =
        profile.server_address.empty() ? server_address : profile.server_address;
    for (auto const& exact : profile.exact) {
        if (Matches(host, exact)) {
            return server;
        }
    }
    for (auto const& suffix : profile.suffixes) {
        if (Matches(host, suffix)) {
            return server;
        }
    }
    return std::nullopt;
}

}  // namespace openpak::NetworkProfile
