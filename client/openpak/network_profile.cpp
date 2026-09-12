// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The network profile, implemented against the contract in network_profile.h
// (prds/emulator-network-profile-prd.md §2 and §3). One conditional GET, two seconds, no
// retries; the compiled-in allowlist is the ceiling; last-known-good beats a fetch and the
// built-in default beats nothing.
#include "openpak/network_profile.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>

#include <arpa/inet.h>
#include <fmt/format.h>
#include <httplib.h>
#include <netdb.h>
#include <nlohmann/json.hpp>
#include <openssl/sha.h>

#include "openpak/api.h"
#include "openpak/log.h"
#include "openpak/platform.h"

namespace httplib {
class Client;  // defined in httplib/httplib.h
}

namespace openpak::NetworkProfile {

namespace {

std::filesystem::path StoredProfilePath() {
    return Platform::ConfigDir() / "openpak_network_profile.json";
}

std::filesystem::path StoredEtagPath() {
    return Platform::ConfigDir() / "openpak_network_profile.etag";
}

// One entry sits inside the ceiling when it is a family itself or a name under one, on a
// label boundary: ".nintendo.net" admits "a.nintendo.net" and not "evila.nintendo.net.evil.example".
bool UnderFamily(std::string const& name, std::string const& family) {
    if (name.size() < family.size()) {
        return false;
    }
    if (name.compare(name.size() - family.size(), family.size(), family) != 0) {
        return false;
    }
    return name.size() == family.size() ||   // the family itself
           name[name.size() - family.size() - 1] == '.';  // or a label under it
}

bool IsLoopbackOrLinkLocal(std::string const& address) {
    in_addr v4{};
    in6_addr v6{};
    if (inet_pton(AF_INET, address.c_str(), &v4) == 1) {
        std::uint32_t const ip = ntohl(v4.s_addr);
        return (ip >> 24) == 127 ||                      // 127.0.0.0/8
               (ip >> 16) == 0xA9FE ||                   // 169.254.0.0/16
               (ip >> 24) == 0 || ip == 0xFFFFFFFF;      // this-network and broadcast
    }
    if (inet_pton(AF_INET6, address.c_str(), &v6) == 1) {
        if (IN6_IS_ADDR_LOOPBACK(&v6) || IN6_IS_ADDR_LINKLOCAL(&v6) || IN6_IS_ADDR_UNSPECIFIED(&v6)) {
            return true;
        }
        return v6.s6_addr[0] == 0xFE && (v6.s6_addr[1] & 0xC0) == 0x80;  // fe80::/10 again, explicit
    }
    return true;  // not an address at all: treat as unusable
}

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

std::vector<std::string> const& AllowedFamilies() {
    static const std::vector<std::string> families = {
        ".nintendo.net",   ".nintendo.com", ".nintendo.co.jp", ".nintendowifi.net",
        ".nintendo-europe.com", ".gamespy.com", ".openpak.org",
    };
    return families;
}

bool NameAllowed(std::string const& host) {
    for (auto const& family : AllowedFamilies()) {
        if (UnderFamily(host, family)) {
            return true;
        }
    }
    return false;
}

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
    in_addr v4{};
    in6_addr v6{};
    bool const is_ip = inet_pton(AF_INET, profile.server_address.c_str(), &v4) == 1 ||
                       inet_pton(AF_INET6, profile.server_address.c_str(), &v6) == 1;
    if (!is_ip) {
        return "server.address is not a literal IP address";
    }
    if (IsLoopbackOrLinkLocal(profile.server_address)) {
        return "server.address is a loopback or link-local address";
    }
    for (auto const& name : profile.suffixes) {
        if (!NameAllowed(name)) {
            return fmt::format("suffix {} is outside the compiled-in families", name);
        }
    }
    for (auto const& name : profile.exact) {
        if (!NameAllowed(name)) {
            return fmt::format("exact name {} is outside the compiled-in families", name);
        }
    }
    return std::nullopt;  // valid
}

Profile BuiltIn(std::string const& platform) {
    Profile p;
    p.platform = platform;
    p.server_address.clear();  // the host keeps its own compiled-in address
    // What the Wii and DS families redirect today; the profile's job is to keep this fresh,
    // not to define it. Hosts with their own lists keep theirs as the fallback.
    p.suffixes = {".nintendowifi.net", ".gamespy.com", ".nintendo.net", ".openpak.org"};
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

    auto const etag = fetch->get_header_value("ETag");
    Store(platform, *fetched, etag.empty() ? etag_sent : etag);
    out.profile = *fetched;
    out.source = Source::Fetched;
    return out;
}

}  // namespace

Result Fetch(std::string const& platform) {
    auto stored = LoadStored(platform);
    httplib::Client client(WebService::OpenPakApi::BaseUrl());
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(2, 0);
    WebService::OpenPakApi::ApplySystemCa(client);
    httplib::Headers headers;
    std::string etag;
    if (stored) {
        etag = ReadStoredEtag();
        if (!etag.empty()) {
            headers.emplace("If-None-Match", etag);
        }
    }
    auto const result = client.Get(
        fmt::format("/api/v1/network/profile?platform={}", platform), headers);

    Result out = Finish(platform, result ? &*result : nullptr, etag);
    OPENPAK_LOG_INFO("network profile: platform {} -> {} source, version {}", platform,
                     [](Source s) {
                         switch (s) {
                         case Source::Fetched: return "fetched";
                         case Source::Cached: return "cached";
                         case Source::BuiltIn: return "built-in";
                         }
                         return "built-in";
                     }(out.source),
                     out.profile.version);
    return out;
}

Result Refresh(std::string const& platform) {
    httplib::Client client(WebService::OpenPakApi::BaseUrl());
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(2, 0);
    WebService::OpenPakApi::ApplySystemCa(client);
    auto const result =
        client.Get(fmt::format("/api/v1/network/profile?platform={}", platform));
    return Finish(platform, result ? &*result : nullptr, ReadStoredEtag());
}

}  // namespace openpak::NetworkProfile
