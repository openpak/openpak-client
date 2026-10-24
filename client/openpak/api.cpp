// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The OpenPak account API, as the emulator uses it. Everything here talks to openpak.org over
// public TLS with the website's API token; the redirected Nintendo names never see this token.
//
//   POST /api/v1/token                       email + password -> bearer
//   GET  /api/v1/me                          display name, avatar
//   GET  /api/v1/me/switch                   pid, friend code, openpak token, friends with presence
//   POST /api/v1/me/switch/friend            request by friend code; accept / decline by pid
//   GET  /api/v1/me/friends[/requests]       the core's friend graph and pending requests
//   POST /api/v1/me/friends/requests[/accept] request by friend code; accept by account id
//   POST /api/v1/me/friends/remove|block     by account id (remove also declines and cancels)
//   GET  /api/v1/me/invitations[, /decline]  invitations the core arranged
//   GET  /api/v1/status                      players per title
//   GET  /api/v1/titles                      the catalogue: names and statuses
//   GET  /api/v1/me/saves                    every cloud save with its versions, and the allowance
//   DELETE /api/v1/me/saves/versions/<id>    one version, the cloud copy only
//   GET/PUT /api/v1/me/saves/switch/<title>  cloud saves, versioned: a PUT names the version it
//                                            loaded from and a stale one becomes a conflict
//   GET  /media/avatars/<account>/256, /titles/icon/<title>  public pictures
//   GET  status.<site>/api/status            the status box's per-service checks
//   GET  /ca.pem                             the OpenPak CA, pinned by the SSL service
//
// What the OpenPak server offered and OpenPak does not (lobby, recent players, reports, history,
// BCAT seeds, profile edits from the emulator) answers "not available" here so the UI degrades
// instead of failing.

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <openssl/sha.h>
#include <openssl/x509_vfy.h>

#if defined(__ANDROID__) && __has_include(<openssl/cert.h>)
// The patched OpenSSL the emulator builds against ships the public roots as a PEM blob.
#include <openssl/cert.h>
#define OPENPAK_CERT_BLOB 1
#endif

#include "openpak/platform.h"
#include "openpak/platform.h"
#include "openpak/platform.h"
#include "openpak/platform.h"
#include "openpak/log.h"
#include "openpak/account.h"
#include "openpak/friends_cache.h"
#include "openpak/session.h"
#include "openpak/api.h"
#include "openpak/network_profile.h"

namespace WebService::OpenPakApi {

namespace {

constexpr const char* CanonicalUrl = "https://openpak.org";
// Every Switch emulator keeps its saves in the one slot Ryujinx uses, so a save follows the
// person from one to another. Wii and DS hosts set their own.
std::string g_saves_platform = "switch";
std::string g_save_device; // empty: report the platform name
constexpr int TimeoutSeconds = 15;

constexpr std::string_view NotAvailable = Errors::NotAvailable;
constexpr std::string_view Unreachable = Errors::Unreachable;
constexpr std::string_view SessionExpired = Errors::SessionExpired;

std::string Base64StdEncode(std::span<const u8> data) {
    static constexpr char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    std::size_t i = 0;
    while (i + 2 < data.size()) {
        const u32 n = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out.push_back(table[(n >> 18) & 63]);
        out.push_back(table[(n >> 12) & 63]);
        out.push_back(table[(n >> 6) & 63]);
        out.push_back(table[n & 63]);
        i += 3;
    }
    if (i < data.size()) {
        u32 n = data[i] << 16;
        if (i + 1 < data.size()) {
            n |= data[i + 1] << 8;
        }
        out.push_back(table[(n >> 18) & 63]);
        out.push_back(table[(n >> 12) & 63]);
        out.push_back(i + 1 < data.size() ? table[(n >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

std::vector<u8> Base64StdDecode(std::string_view text) {
    static constexpr auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') {
            return c - 'A';
        }
        if (c >= 'a' && c <= 'z') {
            return c - 'a' + 26;
        }
        if (c >= '0' && c <= '9') {
            return c - '0' + 52;
        }
        if (c == '+' || c == '-') {
            return 62;
        }
        if (c == '/' || c == '_') {
            return 63;
        }
        return -1;
    };
    std::vector<u8> out;
    out.reserve(text.size() / 4 * 3);
    u32 buffer = 0;
    int bits = 0;
    for (const char c : text) {
        const int v = value(c);
        if (v < 0) {
            continue;
        }
        buffer = (buffer << 6) | static_cast<u32>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<u8>((buffer >> bits) & 0xff));
        }
    }
    return out;
}

bool IsLoopback(const std::string& host) {
    return host == "127.0.0.1" || host == "localhost" || host == "[::1]" || host == "::1";
}

// The static OpenSSL built from source has default cert paths that point inside the build tree.
// On desktop Linux, point at the distro bundle. On Android there is no bundle on disk at all, so
// load the public roots the patched OpenSSL ships in memory -- the same store the emulator's own
// web backend loads. Elsewhere httplib's native store loader takes over.
void ApplyCaCertPath(httplib::Client& client) {
#if defined(__ANDROID__) && defined(OPENPAK_CERT_BLOB)
    client.load_ca_cert_store(kCert, sizeof(kCert));
#elif defined(__ANDROID__)
    // A TLS library without the blob (Azahar's LibreSSL): the phone's own public roots.
    const std::string& roots = openpak::Platform::AndroidSystemRoots();
    client.load_ca_cert_store(roots.data(), roots.size());
#elif defined(__linux__)
    static constexpr std::array<const char*, 4> candidates{
        "/etc/ssl/certs/ca-certificates.crt", // Debian/Ubuntu/Arch
        "/etc/pki/tls/certs/ca-bundle.crt",   // Fedora/RHEL/CentOS
        "/etc/ssl/cert.pem",                  // Alpine
        "/etc/ssl/ca-bundle.pem",             // openSUSE
    };
    for (const char* path : candidates) {
        if (std::filesystem::exists(path)) {
            client.set_ca_cert_path(path);
            return;
        }
    }
    OPENPAK_LOG_ERROR("ApplyCaCertPath: no known system CA bundle found");
#else
    (void)client;
#endif
}

// "https://host[:port]" with no path. The token rides on these requests, so only https, or
// loopback for a local stack, may receive it.
std::optional<std::string> SanitizeBaseUrl(std::string raw) {
    raw = openpak::Platform::StripSpaces(raw);
    while (!raw.empty() && raw.back() == '/') {
        raw.pop_back();
    }
    const auto scheme_end = raw.find("://");
    if (raw.empty() || scheme_end == std::string::npos) {
        return std::nullopt;
    }
    const std::string scheme = raw.substr(0, scheme_end);
    const std::string authority = raw.substr(scheme_end + 3);
    if (authority.empty() || authority.find('/') != std::string::npos) {
        return std::nullopt;
    }
    std::string host = authority;
    if (const auto colon = host.rfind(':'); colon != std::string::npos && host.back() != ']') {
        host = host.substr(0, colon);
    }
    if (IsLoopback(host) || scheme == "https") {
        return raw;
    }
    return std::nullopt;
}

httplib::Client& SharedClient() {
    static httplib::Client client = [] {
        httplib::Client c{BaseUrl()};
        c.set_connection_timeout(TimeoutSeconds);
        c.set_read_timeout(TimeoutSeconds);
        c.set_follow_location(true);
        c.set_keep_alive(true);
        ApplyCaCertPath(c);
        return c;
    }();
    return client;
}

httplib::Result Send(const std::string& method, const std::string& path, const std::string& body,
                     const std::string& bearer, const httplib::Headers& extra_headers = {},
                     const std::string& content_type = "application/json") {
    static std::mutex client_mutex;
    std::lock_guard lock{client_mutex};
    httplib::Client& client = SharedClient();

    httplib::Headers headers{{"User-Agent", "citron-openpak"}};
    if (std::string client_header = openpak::Platform::ClientHeader(); !client_header.empty()) {
        headers.emplace("X-OpenPak-Client", std::move(client_header));
    }
    if (!bearer.empty()) {
        headers.emplace("Authorization", "Bearer " + bearer);
    }
    for (const auto& [key, value] : extra_headers) {
        headers.emplace(key, value);
    }

    auto result = method == "GET"      ? client.Get(path, headers)
                  : method == "PUT"    ? client.Put(path, headers, body, content_type)
                  : method == "DELETE" ? client.Delete(path, headers)
                                       : client.Post(path, headers, body, content_type);
    if (!result) {
        // ponytail: the OpenSSL verify result is only readable on httplib builds that expose
        // it; the httplib error already names a certificate failure.
        OPENPAK_LOG_ERROR("Send {} {}: httplib error={}", method, path,
                          httplib::to_string(result.error()));
    }
    return result;
}

// A 401 on an authenticated call means the stored token is expired or revoked. Only a real 401
// counts: network errors and 5xx must never clear a good session.
bool ClearSessionIfRejected(const httplib::Result& result) {
    if (!result || result->status != 401) {
        return false;
    }
    OPENPAK_LOG_WARNING("openpak.org rejected the stored account token; signing out");
    Common::OpenPakAccount::Clear();
    return true;
}

// The website answers failures with {"error": "..."}.
std::string ErrorFrom(const std::string& payload, const std::string& fallback) {
    try {
        const auto json = nlohmann::json::parse(payload);
        if (json.contains("error") && json["error"].is_string()) {
            return json["error"].get<std::string>();
        }
    } catch (const nlohmann::json::exception&) {
    }
    return fallback;
}

std::string Bearer() {
    return Common::OpenPakAccount::GetBearer();
}

Friend ParseFriend(const nlohmann::json& json) {
    Friend out;
    out.pid = json.value("pid", u64{0});
    out.account_id = json.value("account_id", std::string{});
    out.name = json.value("name", std::string{});
    out.friend_code = json.value("friend_code", std::string{});
    if (const auto presence = json.find("presence"); presence != json.end() && presence->is_object()) {
        out.presence_status = presence->value("status", s32{0});
        // The adapter hands back what the game server published: base64 text wrapping the raw
        // AppKeyValueStorage blob the guest's UserPresenceImpl.app_key_value expects.
        const auto decoded = Base64StdDecode(presence->value("app_field", std::string{}));
        out.app_field = std::string{reinterpret_cast<const char*>(decoded.data()), decoded.size()};
        out.app_id = presence->value("app_id", std::string{});
    }
    return out;
}

// Fetches /api/v1/me/switch: the whole Switch-side view of the signed-in account.
std::optional<nlohmann::json> FetchSwitch(std::string& error) {
    const std::string bearer = Bearer();
    if (bearer.empty()) {
        error = std::string{Errors::NotSignedIn};
        return std::nullopt;
    }
    const auto result = Send("GET", "/api/v1/me/switch", {}, bearer);
    if (ClearSessionIfRejected(result)) {
        error = std::string{SessionExpired};
        return std::nullopt;
    }
    if (!result) {
        error = std::string{Unreachable};
        return std::nullopt;
    }
    if (result->status != 200) {
        error = ErrorFrom(result->body, fmt::format("Could not load your Switch identity (HTTP {}).",
                                                    result->status));
        return std::nullopt;
    }
    try {
        return nlohmann::json::parse(result->body);
    } catch (const nlohmann::json::exception& e) {
        error = fmt::format("Unexpected response: {}", e.what());
        return std::nullopt;
    }
}

std::string FriendAction(const nlohmann::json& body) {
    const std::string bearer = Bearer();
    if (bearer.empty()) {
        return std::string{Errors::NotSignedIn};
    }
    const auto result = Send("POST", "/api/v1/me/switch/friend", body.dump(), bearer);
    if (ClearSessionIfRejected(result)) {
        return std::string{SessionExpired};
    }
    if (!result) {
        return std::string{Unreachable};
    }
    if (result->status != 200) {
        return ErrorFrom(result->body, fmt::format("Request failed (HTTP {}).", result->status));
    }
    return {};
}

// A catalogue url is a path on the site or an absolute https one (a CDN). Public either way: the
// account token never rides along.
std::optional<std::vector<u8>> FetchPublic(const std::string& url) {
    httplib::Result result{nullptr, httplib::Error::Unknown};
    if (url.starts_with("/")) {
        result = Send("GET", url, {}, {});
    } else if (url.starts_with("https://") || url.starts_with(BaseUrl())) {
        const std::size_t path_at = url.find('/', url.find("://") + 3);
        httplib::Client client{url.substr(0, path_at)};
        client.set_connection_timeout(TimeoutSeconds);
        client.set_read_timeout(TimeoutSeconds);
        client.set_follow_location(true);
        ApplyCaCertPath(client);
        result = client.Get(path_at == std::string::npos ? "/" : url.substr(path_at),
                            httplib::Headers{{"User-Agent", "citron-openpak"}});
    }
    if (!result || result->status != 200) {
        OPENPAK_LOG_WARNING("Fetch {} failed (HTTP {})", url, result ? result->status : 0);
        return std::nullopt;
    }
    return std::vector<u8>(result->body.begin(), result->body.end());
}

std::string Sha256Hex(std::span<const u8> data) {
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    SHA256(data.data(), data.size(), digest.data());
    std::string out;
    for (const unsigned char byte : digest) {
        out += fmt::format("{:02x}", byte);
    }
    return out;
}

// GET a JSON document; nullopt on anything but a parsed 200.
std::optional<nlohmann::json> GetJson(const std::string& path, const std::string& bearer) {
    const auto result = Send("GET", path, {}, bearer);
    if (!bearer.empty() && ClearSessionIfRejected(result)) {
        return std::nullopt;
    }
    if (!result || result->status != 200) {
        return std::nullopt;
    }
    try {
        return nlohmann::json::parse(result->body);
    } catch (const nlohmann::json::exception& e) {
        OPENPAK_LOG_WARNING("Unexpected response from {}: {}", path, e.what());
        return std::nullopt;
    }
}

// The site answers a missing string as null as often as it omits it; value() throws on null.
std::string Str(const nlohmann::json& json, const char* key) {
    const auto it = json.find(key);
    return it != json.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

// The version each title's save was last pulled at, so a push can say what it was based on.
std::mutex g_save_versions_mutex;
std::map<std::string, std::string> g_save_versions;

} // Anonymous namespace

// Public: the network profile module (and hosts) reuse the shared client's CA handling.
void ApplySystemCa(httplib::Client& client) { ApplyCaCertPath(client); }

std::string BaseUrl() {
    static const std::string url = [] {
        const char* env = std::getenv("OPENPAK_API");
        if (env && *env) {
            if (const auto sanitized = SanitizeBaseUrl(env)) {
                return *sanitized;
            }
            OPENPAK_LOG_WARNING("Ignoring OPENPAK_API=\"{}\": only https, or loopback, may receive the "
                        "account token",
                        env);
        }
        return std::string{CanonicalUrl};
    }();
    return url;
}

bool FetchCA() {
    const auto result = Send("GET", "/ca.pem", {}, {});
    if (!result || result->status != 200 || result->body.find("BEGIN CERTIFICATE") == std::string::npos) {
        OPENPAK_LOG_WARNING("FetchCA: no CA from {} (HTTP {})", BaseUrl(),
                    result ? result->status : 0);
        return false;
    }
    const auto path = openpak::Platform::ConfigDir() / "openpak" / "ca.pem";
    void(openpak::Platform::CreateParentDirs(path));
    if (!openpak::Platform::WriteFile(path, result->body)) {
        OPENPAK_LOG_WARNING("FetchCA: could not write the CA");
        return false;
    }
    OPENPAK_LOG_INFO("FetchCA: stored the OpenPak CA; it is pinned from the next launch");
    return true;
}

namespace {
std::mutex g_device_name_mutex;
std::string g_device_name;

// email + password, and the device name the account's device list shows when the host set one.
nlohmann::json Credentials(const std::string& email, const std::string& password) {
    nlohmann::json credentials{{"email", email}, {"password", password}};
    std::lock_guard lock{g_device_name_mutex};
    if (!g_device_name.empty()) {
        credentials["device_name"] = g_device_name;
    }
    return credentials;
}
} // namespace

void SetDeviceName(const std::string& device_name) {
    std::lock_guard lock{g_device_name_mutex};
    g_device_name = device_name;
}

LoginResult SignIn(const std::string& email, const std::string& password) {
    LoginResult out;
    const nlohmann::json credentials = Credentials(email, password);
    const auto minted = Send("POST", "/api/v1/token", credentials.dump(), {});
    if (!minted) {
        out.error = std::string{Unreachable};
        return out;
    }
    if (minted->status == 401) {
        out.error = std::string{Errors::Credentials};
        return out;
    }
    if (minted->status == 429) {
        out.error = std::string{Errors::RateLimited};
        return out;
    }
    if (minted->status != 201 && minted->status != 200) {
        out.error = ErrorFrom(minted->body, fmt::format("Sign-in failed (HTTP {}).", minted->status));
        return out;
    }
    try {
        out.bearer = nlohmann::json::parse(minted->body).value("token", std::string{});
    } catch (const nlohmann::json::exception& e) {
        out.error = fmt::format("Unexpected sign-in response: {}", e.what());
        return out;
    }
    if (out.bearer.empty()) {
        out.error = "The website issued no token.";
        return out;
    }

    const auto identity = Send("GET", "/api/v1/me/switch", {}, out.bearer);
    if (!identity) {
        out.error = std::string{Unreachable};
        return out;
    }
    if (identity->status != 200) {
        out.error = ErrorFrom(identity->body,
                              fmt::format("Could not load your Switch identity (HTTP {}).", identity->status));
        return out;
    }
    try {
        const auto json = nlohmann::json::parse(identity->body);
        out.pid = json.value("pid", u64{0});
        out.username = json.value("username", std::string{});
        out.friend_code = json.value("friend_code", std::string{});
        out.token = json.value("token", std::string{});
    } catch (const nlohmann::json::exception& e) {
        out.error = fmt::format("Unexpected identity response: {}", e.what());
        return out;
    }
    if (out.pid == 0 || out.token.empty()) {
        out.error = "The website answered without a Switch identity.";
        return out;
    }
    FetchCA();
    out.ok = true;
    OPENPAK_LOG_INFO("SignIn: pid={} username={}", out.pid, out.username);
    // A sign-in is one of the moments the redirects are looked at again (signed-ceiling.md).
    openpak::NetworkProfile::RecheckSoon();
    return out;
}

LoginResult SignInAccountOnly(const std::string& email, const std::string& password) {
    LoginResult out;
    const nlohmann::json credentials = Credentials(email, password);
    const auto minted = Send("POST", "/api/v1/token", credentials.dump(), {});
    if (!minted) {
        out.error = std::string{Unreachable};
        return out;
    }
    if (minted->status == 401) {
        out.error = std::string{Errors::Credentials};
        return out;
    }
    if (minted->status == 429) {
        out.error = std::string{Errors::RateLimited};
        return out;
    }
    if (minted->status != 201 && minted->status != 200) {
        out.error = ErrorFrom(minted->body, fmt::format("Sign-in failed (HTTP {}).", minted->status));
        return out;
    }
    try {
        out.bearer = nlohmann::json::parse(minted->body).value("token", std::string{});
    } catch (const nlohmann::json::exception& e) {
        out.error = fmt::format("Unexpected sign-in response: {}", e.what());
        return out;
    }
    if (out.bearer.empty()) {
        out.error = "The website issued no token.";
        return out;
    }
    out.ok = true;
    OPENPAK_LOG_INFO("SignInAccountOnly: signed in without a console identity");
    openpak::NetworkProfile::RecheckSoon();
    return out;
}

OnlineStatus GetOnlineStatus() {
    // OpenPak has no per-account online gate: a signed-in account may play.
    OnlineStatus out;
    if (Bearer().empty()) {
        return out;
    }
    out.queried = true;
    out.allow = true;
    return out;
}

void SetSavesPlatform(const std::string& platform) {
    g_saves_platform = platform;
}

std::string SavesPlatform() {
    return g_saves_platform;
}

std::string SaveVersion(const std::string& title_id_hex) {
    std::lock_guard lock{g_save_versions_mutex};
    const auto it = g_save_versions.find(title_id_hex);
    return it == g_save_versions.end() ? std::string{} : it->second;
}

void SetSaveVersion(const std::string& title_id_hex, const std::string& version) {
    std::lock_guard lock{g_save_versions_mutex};
    g_save_versions[title_id_hex] = version;
}

void SetSaveDevice(const std::string& device) {
    g_save_device = device;
}

std::optional<std::vector<u8>> PullSave(const std::string& title_id_hex) {
    const std::string bearer = Bearer();
    if (bearer.empty()) {
        return std::nullopt;
    }
    const std::string path = fmt::format("/api/v1/me/saves/{}/{}", g_saves_platform, title_id_hex);
    const auto result = Send("GET", path, {}, bearer);
    if (ClearSessionIfRejected(result)) {
        return std::nullopt;
    }
    if (!result || result->status == 204) {
        return std::nullopt; // nothing stored yet
    }
    if (result->status != 200) {
        OPENPAK_LOG_WARNING("OpenPak save pull failed (HTTP {})", result->status);
        return std::nullopt;
    }
    {
        std::lock_guard lock{g_save_versions_mutex};
        g_save_versions[title_id_hex] = result->get_header_value("X-Save-Version");
    }
    return std::vector<u8>(result->body.begin(), result->body.end());
}

std::string PushSave(const std::string& title_id_hex, const std::uint8_t* data,
                     std::size_t size) {
    const std::string bearer = Bearer();
    if (bearer.empty()) {
        return std::string{Errors::NotSignedIn};
    }
    httplib::Headers headers{{"X-Save-Device",
        g_save_device.empty() ? g_saves_platform : g_save_device}};
    {
        std::lock_guard lock{g_save_versions_mutex};
        if (const auto it = g_save_versions.find(title_id_hex); it != g_save_versions.end() && !it->second.empty()) {
            headers.emplace("X-Save-Base", it->second);
        }
    }
    const std::string body(reinterpret_cast<const char*>(data), size);
    const std::string path = fmt::format("/api/v1/me/saves/{}/{}", g_saves_platform, title_id_hex);
    const auto result = Send("PUT", path, body, bearer, headers, "application/octet-stream");
    if (ClearSessionIfRejected(result)) {
        return std::string{SessionExpired};
    }
    if (!result) {
        return std::string{Unreachable};
    }
    if (result->status == 507) {
        return "Your OpenPak storage allowance is full. Connect your own storage at openpak.org.";
    }
    if (result->status != 201 && result->status != 200) {
        return ErrorFrom(result->body, "Could not upload the save.");
    }
    try {
        const auto json = nlohmann::json::parse(result->body);
        std::lock_guard lock{g_save_versions_mutex};
        g_save_versions[title_id_hex] = std::to_string(json.value("number", 0));
    } catch (const nlohmann::json::exception&) {
    }
    return {};
}

std::map<std::string, int> GetOnlineCounts() {
    std::map<std::string, int> out;
    const auto result = Send("GET", "/api/v1/status", {}, {});
    if (!result || result->status != 200) {
        OPENPAK_LOG_WARNING("OpenPak status fetch failed (HTTP {})", result ? result->status : 0);
        return out;
    }
    try {
        const auto json = nlohmann::json::parse(result->body);
        for (const auto& row : json.value("titles", nlohmann::json::array())) {
            const std::string title_id = openpak::Platform::ToLower(row.value("title_id", std::string{}));
            if (!title_id.empty()) {
                out[title_id] += row.value("players", 0);
            }
        }
    } catch (const nlohmann::json::exception& e) {
        OPENPAK_LOG_WARNING("Unexpected status response: {}", e.what());
    }
    return out;
}

int GetNzpOnlineCount() {
    const char* env = std::getenv("NZP_API");
    if (!env || !*env) {
        return 0;
    }
    httplib::Client client{env};
    client.set_connection_timeout(3);
    client.set_read_timeout(3);
    const auto result = client.Get("/online-count");
    if (!result || result->status != 200) {
        return 0;
    }
    try {
        const auto json = nlohmann::json::parse(result->body);
        if (const auto it = json.find("count"); it != json.end() && it->is_number_integer()) {
            return it->get<int>();
        }
    } catch (const nlohmann::json::exception&) {
    }
    return 0;
}

Profile GetProfile() {
    Profile out;
    const std::string bearer = Bearer();
    if (bearer.empty()) {
        out.error = std::string{Errors::NotSignedIn};
        return out;
    }
    const auto result = Send("GET", "/api/v1/me", {}, bearer);
    if (ClearSessionIfRejected(result)) {
        out.error = std::string{SessionExpired};
        return out;
    }
    if (!result) {
        out.error = std::string{Unreachable};
        return out;
    }
    if (result->status != 200) {
        out.error = ErrorFrom(result->body, fmt::format("Could not load profile (HTTP {}).", result->status));
        return out;
    }
    try {
        const auto json = nlohmann::json::parse(result->body);
        out.name = json.value("display_name", std::string{});
        out.console_nickname = out.name;
        out.account_id = Str(json, "account_id");
        out.friend_code = Str(json, "friend_code");
        if (const auto linked = json.find("linked_platforms"); linked != json.end() && linked->is_array()) {
            for (const auto& platform : *linked) {
                if (platform.is_object() && !Str(platform, "namespace").empty()) {
                    out.linked_platforms.push_back(Str(platform, "namespace"));
                }
            }
        }
        if (const auto avatar = json.find("avatar_url"); avatar != json.end() && avatar->is_string()) {
            const auto image = Send("GET", avatar->get<std::string>(), {}, bearer);
            if (image && image->status == 200) {
                out.image_base64 = Base64StdEncode(std::span<const u8>{
                    reinterpret_cast<const u8*>(image->body.data()), image->body.size()});
            }
        }
        out.ok = true;
    } catch (const nlohmann::json::exception& e) {
        out.error = fmt::format("Unexpected profile response: {}", e.what());
    }
    return out;
}

std::string PushProfilePicture(const std::string& image_base64) {
    (void)image_base64;
    return "Change your picture on openpak.org.";
}

std::string SetUsername(const std::string& username) {
    (void)username;
    return "Change your name on openpak.org.";
}

HistoryList GetHistory() {
    HistoryList out;
    out.ok = true;
    return out;
}

void SyncHistory(const std::vector<HistoryEntry>& entries) {
    (void)entries;
}

FriendList GetFriends() {
    FriendList out;
    const auto json = FetchSwitch(out.error);
    if (!json) {
        OPENPAK_LOG_WARNING("GetFriends: {}", out.error);
        return out;
    }
    try {
        for (const auto& entry : json->value("friends", nlohmann::json::array())) {
            out.friends.push_back(ParseFriend(entry));
        }
        for (const auto& entry : json->value("requests", nlohmann::json::array())) {
            out.requests.push_back(ParseFriend(entry));
        }
        out.ok = true;
        OPENPAK_LOG_INFO("GetFriends: {} friend(s), {} request(s)", out.friends.size(),
                 out.requests.size());
    } catch (const nlohmann::json::exception& e) {
        out.error = fmt::format("Unexpected friends response: {}", e.what());
    }
    return out;
}

namespace {

// A POST to the website's friend routes; empty on success, else what to show.
std::string PostAccount(const std::string& path, const nlohmann::json& body) {
    const std::string bearer = Bearer();
    if (bearer.empty()) {
        return std::string{Errors::NotSignedIn};
    }
    const auto result = Send("POST", path, body.dump(), bearer);
    if (ClearSessionIfRejected(result)) {
        return std::string{SessionExpired};
    }
    if (!result) {
        return std::string{Unreachable};
    }
    if (result->status / 100 != 2) {
        return ErrorFrom(result->body, fmt::format("OpenPak answered {}.", result->status));
    }
    return {};
}

std::string UpperHex(u64 value) {
    return value == 0 ? std::string{} : fmt::format("{:016X}", value);
}

// The adapter's presence: status is a number or a word, app_id a 16-digit hex string, a shorter
// decimal one or a number, since unix seconds.
void ParseAdapterPresence(const nlohmann::json& presence, AccountFriend& out) {
    if (const auto status = presence.find("status"); status != presence.end()) {
        if (status->is_number()) {
            out.online = status->get<double>() > 0;
        } else if (status->is_string()) {
            const std::string word = openpak::Platform::ToLower(status->get<std::string>());
            out.online = word == "online" || word == "playing";
        }
    }
    if (const auto app = presence.find("app_id"); app != presence.end()) {
        u64 id = 0;
        try {
            if (app->is_number_unsigned()) {
                id = app->get<u64>();
            } else if (app->is_string() && !app->get<std::string>().empty()) {
                const std::string text = app->get<std::string>();
                id = std::stoull(text, nullptr, text.size() == 16 ? 16 : 10);
            }
        } catch (const std::exception&) {
        }
        out.title_id = out.online ? UpperHex(id) : std::string{};
    }
    const std::string name_space = Str(presence, "namespace");
    out.console_namespace = out.online ? (name_space.empty() ? "switch" : name_space) : std::string{};
    if (const auto since = presence.find("since"); since != presence.end() && since->is_number()) {
        out.online_since = since->get<s64>();
    }
}

AccountFriend ParseAdapterFriend(const nlohmann::json& json) {
    AccountFriend out;
    out.account_id = Str(json, "account_id");
    out.pid = json.value("pid", u64{0});
    out.display_name = Str(json, "name");
    out.friend_code = Str(json, "friend_code");
    if (const auto since = json.find("friends_since"); since != json.end() && since->is_number()) {
        out.friends_since = since->get<s64>();
    }
    if (const auto presence = json.find("presence"); presence != json.end() && presence->is_object()) {
        ParseAdapterPresence(*presence, out);
    }
    return out;
}

s64 UnixFromIso(const std::string& text) {
    // RFC3339 UTC as the website writes it ("2026-09-08T09:15:00Z"); anything else is unknown.
    std::tm tm{};
    if (text.size() < 19 || std::sscanf(text.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &tm.tm_year,
                                        &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min,
                                        &tm.tm_sec) != 6) {
        return 0;
    }
    // Days from civil (Howard Hinnant), so no timegm/_mkgmtime split between platforms.
    int y = tm.tm_year;
    const unsigned m = static_cast<unsigned>(tm.tm_mon);
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + static_cast<unsigned>(tm.tm_mday) - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const s64 days = static_cast<s64>(era) * 146097 + static_cast<s64>(doe) - 719468;
    return days * 86400 + tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
}

AccountFriend ParseCoreFriend(const nlohmann::json& json) {
    AccountFriend out;
    out.account_id = Str(json, "account_id");
    out.display_name = Str(json, "display_name");
    const auto online = json.find("online");
    out.online = online != json.end() && online->is_boolean() && online->get<bool>();
    if (out.online) {
        out.title_id = Str(json, "title_id");
        std::transform(out.title_id.begin(), out.title_id.end(), out.title_id.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        out.console_namespace = Str(json, "namespace");
        out.online_since = UnixFromIso(Str(json, "since"));
    }
    return out;
}

bool IsTameId(const std::string& id) {
    return !id.empty() && id.size() <= 64 && std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '-' || c == '_';
    });
}

} // namespace

AccountFriends GetAccountFriends() {
    AccountFriends out;
    const std::string bearer = Bearer();
    if (bearer.empty()) {
        out.error = std::string{Errors::NotSignedIn};
        return out;
    }

    std::vector<AccountFriend> adapter_friends;
    std::vector<AccountFriend> adapter_requests;
    std::string adapter_error;
    if (const auto identity = FetchSwitch(adapter_error)) {
        for (const auto& entry : identity->value("friends", nlohmann::json::array())) {
            adapter_friends.push_back(ParseAdapterFriend(entry));
        }
        for (const auto& entry : identity->value("requests", nlohmann::json::array())) {
            adapter_requests.push_back(ParseAdapterFriend(entry));
            adapter_requests.back().incoming = true;
        }
    }
    if (Bearer().empty()) {
        out.error = std::string{SessionExpired};
        return out;
    }

    // The core's lists answer for an account with no Switch identity, and they are the only
    // place outgoing requests are listed.
    const auto core_friends = GetJson("/api/v1/me/friends", bearer);
    const auto core_requests = GetJson("/api/v1/me/friends/requests", bearer);
    if (!core_friends && adapter_friends.empty() && !adapter_error.empty()) {
        out.error = adapter_error;
        return out;
    }

    if (!adapter_friends.empty()) {
        out.friends = std::move(adapter_friends);
    } else if (core_friends && core_friends->contains("friends") && (*core_friends)["friends"].is_array()) {
        for (const auto& entry : (*core_friends)["friends"]) {
            out.friends.push_back(ParseCoreFriend(entry));
        }
    }
    std::stable_sort(out.friends.begin(), out.friends.end(),
                     [](const AccountFriend& a, const AccountFriend& b) { return a.online && !b.online; });

    if (core_requests) {
        for (const auto* key : {"incoming", "outgoing"}) {
            if (!core_requests->contains(key) || !(*core_requests)[key].is_array()) {
                continue;
            }
            for (const auto& entry : (*core_requests)[key]) {
                AccountFriend request = ParseCoreFriend(entry);
                request.incoming = std::string_view{key} == "incoming";
                out.requests.push_back(std::move(request));
            }
        }
    }
    if (out.requests.empty()) {
        out.requests = std::move(adapter_requests);
    }
    out.ok = true;
    return out;
}

std::string SendFriendRequest(const std::string& friend_code) {
    return PostAccount("/api/v1/me/friends/requests", {{"friend_code", friend_code}});
}

std::string AcceptFriendRequest(const AccountFriend& request) {
    if (request.account_id.empty()) {
        return FriendAction({{"action", "accept"}, {"pid", request.pid}});
    }
    return PostAccount("/api/v1/me/friends/requests/accept", {{"account_id", request.account_id}});
}

std::string DeclineFriendRequest(const AccountFriend& request) {
    if (request.account_id.empty()) {
        return FriendAction({{"action", "decline"}, {"pid", request.pid}});
    }
    // The earlier port declines and cancels through remove, which drops a pending request in
    // either direction; /requests/decline only knows incoming ones.
    return PostAccount("/api/v1/me/friends/remove", {{"account_id", request.account_id}});
}

std::string RemoveAccountFriend(const std::string& account_id) {
    if (account_id.empty()) {
        return "That player has no OpenPak account to remove.";
    }
    return PostAccount("/api/v1/me/friends/remove", {{"account_id", account_id}});
}

std::string BlockAccount(const std::string& account_id) {
    if (account_id.empty()) {
        return "That player has no OpenPak account to block.";
    }
    return PostAccount("/api/v1/me/friends/block", {{"account_id", account_id}});
}

std::optional<std::vector<u8>> GetAccountAvatar(const std::string& account_id) {
    // The id goes into a path, so only the shape the site mints is accepted.
    if (!IsTameId(account_id)) {
        return std::nullopt;
    }
    return FetchPublic(fmt::format("/media/avatars/{}/256", account_id));
}

std::optional<std::vector<u8>> GetTitleIcon(const std::string& title_id_hex) {
    if (title_id_hex.size() != 16 || !std::all_of(title_id_hex.begin(), title_id_hex.end(),
                                                   [](unsigned char c) { return std::isxdigit(c); })) {
        return std::nullopt;
    }
    return FetchPublic(fmt::format("/titles/icon/{}", title_id_hex));
}

std::string AddFriendByCode(const std::string& friend_code) {
    return FriendAction({{"action", "request"}, {"friend_code", friend_code}});
}

std::string AcceptFriend(u64 pid) {
    return FriendAction({{"action", "accept"}, {"pid", pid}});
}

std::string DeclineFriend(u64 pid) {
    return FriendAction({{"action", "decline"}, {"pid", pid}});
}

std::string RemoveFriend(u64 pid) {
    // The website removes by account id; the pid maps to one through the friend list.
    std::string error;
    const auto json = FetchSwitch(error);
    if (!json) {
        return error;
    }
    std::string account_id;
    for (const auto& entry : json->value("friends", nlohmann::json::array())) {
        if (entry.value("pid", u64{0}) == pid) {
            account_id = entry.value("account_id", std::string{});
        }
    }
    if (account_id.empty()) {
        return "That player is not on your friend list.";
    }
    const auto result = Send("POST", "/api/v1/me/friends/remove",
                             nlohmann::json{{"account_id", account_id}}.dump(), Bearer());
    if (ClearSessionIfRejected(result)) {
        return std::string{SessionExpired};
    }
    if (!result) {
        return std::string{Unreachable};
    }
    if (result->status != 200) {
        return ErrorFrom(result->body, fmt::format("Request failed (HTTP {}).", result->status));
    }
    return {};
}

void PushProfileName(const std::string& name) {
    // The display name is the account's own, edited on the website; the console nickname is
    // not a separate thing on OpenPak.
    (void)name;
}

void PushPresence(s32 status, const std::string& app_field, const std::string& app_id,
                  const std::string& app_name) {
    // Presence on OpenPak comes from the game servers a title is playing on, which register
    // the session with the account core; the emulator has nothing to add.
    (void)status;
    (void)app_field;
    (void)app_id;
    (void)app_name;
}

std::string SendInvitation(const std::vector<u64>& target_pids, std::span<const u8> app_param) {
    (void)target_pids;
    (void)app_param;
    return std::string{NotAvailable};
}

// Invitations arrive on the native inbox, not here: a console sends one to
// app.lp1.five.nintendo.net and the core's own store never sees it. The session module holds
// what the last poll found; this pops it, which is the contract the friend service relies on --
// an invitation handed to the guest must not be handed over twice.
std::vector<ReceivedInvitation> PollInvitations() {
    const std::vector<openpak::client::session::Invitation> waiting =
        openpak::client::session::Invitations();

    if (waiting.empty()) {
        return {};
    }

    // The native wire names the sender by BAAS id; the guest wants the pid its friend list is
    // keyed by. The sender is necessarily a friend -- the server refuses an invitation between
    // strangers -- so the friend cache can name them.
    // ponytail: matched by name; have the adapter carry the pid on the inbox item if two friends
    // ever share one.
    const std::vector<Common::OpenPakFriends::Entry> friends = Common::OpenPakFriends::Get();

    std::vector<ReceivedInvitation> out;
    out.reserve(waiting.size());

    for (const openpak::client::session::Invitation& invitation : waiting) {
        ReceivedInvitation one;
        one.from_name = invitation.sender_name;
        one.app_param = invitation.app_param;

        for (const Common::OpenPakFriends::Entry& entry : friends) {
            if (entry.name == invitation.sender_name) {
                one.from_pid = entry.pid;
                break;
            }
        }

        out.push_back(std::move(one));

        openpak::client::session::DismissInvitation(invitation.id);
    }

    OPENPAK_LOG_INFO("[OpenPak] Handing {} invitation(s) to the guest", out.size());

    return out;
}

Lobby GetMyLobby() {
    return {};
}

std::vector<LobbyPlayer> GetRecentPlayers() {
    return {};
}

std::string GetAvatarByPid(u64 pid) {
    // Answered nothing, so the account's picture never reached an emulator profile.
    // ponytail: the signed-in account's own picture only, which is all any caller asks for;
    // another player's comes with the friend list.
    return pid != 0 && pid == Common::OpenPakAccount::GetPid() ? GetProfile().image_base64
                                                                : std::string{};
}

std::string ReportPlayer(u64 pid, const std::string& reason, const std::string& comment) {
    (void)pid;
    (void)reason;
    (void)comment;
    return std::string{NotAvailable};
}

std::vector<CoreInvitation> GetInvitations() {
    std::vector<CoreInvitation> out;
    const std::string bearer = Bearer();
    if (bearer.empty()) {
        return out;
    }
    const auto json = GetJson("/api/v1/me/invitations", bearer);
    if (!json || !json->contains("invitations") || !(*json)["invitations"].is_array()) {
        return out;
    }
    for (const auto& item : (*json)["invitations"]) {
        out.push_back({Str(item, "invitation_id"), Str(item, "from"), Str(item, "title_id"),
                       Str(item, "expires_at")});
    }
    return out;
}

std::string DeclineInvitation(const std::string& invitation_id) {
    const std::string bearer = Bearer();
    if (bearer.empty()) {
        return std::string{Errors::NotSignedIn};
    }
    const nlohmann::json body{{"invitation_id", invitation_id}};
    const auto result = Send("POST", "/api/v1/me/invitations/decline", body.dump(), bearer);
    if (ClearSessionIfRejected(result)) {
        return std::string{SessionExpired};
    }
    if (!result) {
        return std::string{Unreachable};
    }
    if (result->status / 100 != 2) {
        return ErrorFrom(result->body, fmt::format("Request failed (HTTP {}).", result->status));
    }
    return {};
}

namespace {
std::vector<Mod> ParseMods(const std::optional<nlohmann::json>& json, const char* key) {
    std::vector<Mod> out;
    if (!json || !json->contains(key) || !(*json)[key].is_array()) {
        return out;
    }
    for (const auto& item : (*json)[key]) {
        out.push_back({Str(item, "id"), Str(item, "slug"), Str(item, "name"), Str(item, "version"),
                       Str(item, "author"), Str(item, "licence"), Str(item, "summary"),
                       Str(item, "sha256"), Str(item, "package_url")});
    }
    return out;
}
} // namespace

std::vector<Mod> GetMods(const std::string& title_id_hex) {
    return ParseMods(GetJson(fmt::format("/api/v1/titles/{}/mods",
                                         openpak::Platform::ToLower(title_id_hex)), {}),
                     "mods");
}

std::vector<std::string> GetFavouriteModIds() {
    std::vector<std::string> out;
    const std::string bearer = Bearer();
    if (bearer.empty()) {
        return out;
    }
    for (const Mod& mod : ParseMods(GetJson("/api/v1/me/favourites", bearer), "favourites")) {
        out.push_back(mod.id);
    }
    return out;
}

bool SetModFavourite(const std::string& mod_id, bool favourite) {
    const std::string bearer = Bearer();
    // The id goes into a path: the catalogue's ids are tame, anything else is not one of them.
    if (bearer.empty() || mod_id.empty() ||
        !std::all_of(mod_id.begin(), mod_id.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '-' || c == '_';
        })) {
        return false;
    }
    const auto result =
        Send(favourite ? "PUT" : "DELETE", "/api/v1/me/favourites/" + mod_id, {}, bearer);
    return !ClearSessionIfRejected(result) && result && result->status / 100 == 2;
}

std::map<std::string, CatalogueTitle> Catalogue() {
    std::map<std::string, CatalogueTitle> out;
    const auto json = GetJson("/api/v1/titles", {});
    if (!json || !json->contains("titles") || !(*json)["titles"].is_array()) {
        return out;
    }
    for (const auto& title : (*json)["titles"]) {
        // Keyed in upper case, the way the catalogue itself spells ids, so a lookup does not
        // depend on which case the caller happened to have.
        // One row, several title ids (one per region): every id gets the row, or a European
        // copy shows no online label while the American one does.
        std::vector<std::string> ids{Str(title, "title_id")};
        if (title.contains("title_ids") && title["title_ids"].is_array()) {
            for (const auto& extra : title["title_ids"]) {
                if (extra.is_string()) {
                    ids.push_back(extra.get<std::string>());
                }
            }
        }
        std::map<std::string, std::string> playability;
        if (title.contains("playability") && title["playability"].is_object()) {
            for (const auto& [client, word] : title["playability"].items()) {
                if (word.is_string()) {
                    playability[client] = word.get<std::string>();
                }
            }
        }
        for (std::string id : ids) {
            std::transform(id.begin(), id.end(), id.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            if (!id.empty()) {
                out[id] = {Str(title, "name"), Str(title, "console"), Str(title, "status"),
                           playability};
            }
        }
    }
    return out;
}

std::map<std::string, std::string> CatalogueStatuses() {
    std::map<std::string, std::string> out;
    for (const auto& [id, title] : Catalogue()) {
        if (!title.status.empty()) {
            out[id] = title.status;
        }
    }
    return out;
}

CloudSaves GetCloudSaves() {
    CloudSaves out;
    const std::string bearer = Bearer();
    if (bearer.empty()) {
        out.error = std::string{Errors::NotSignedIn};
        return out;
    }
    const auto result = Send("GET", "/api/v1/me/saves", {}, bearer);
    if (ClearSessionIfRejected(result)) {
        out.error = std::string{SessionExpired};
        return out;
    }
    if (!result) {
        out.error = std::string{Unreachable};
        return out;
    }
    if (result->status != 200) {
        out.error = ErrorFrom(result->body,
                              fmt::format("Could not load your cloud saves (HTTP {}).", result->status));
        return out;
    }
    try {
        const auto json = nlohmann::json::parse(result->body);
        const auto number = [](const nlohmann::json& j, const char* key) -> u64 {
            const auto it = j.find(key);
            return it != j.end() && it->is_number_unsigned() ? it->get<u64>() : 0;
        };
        if (const auto usage = json.find("usage"); usage != json.end() && usage->is_object()) {
            out.allowance_used = number(*usage, "allowance_used");
            out.allowance = number(*usage, "allowance");
        }
        if (const auto saves = json.find("saves"); saves != json.end() && saves->is_array()) {
            for (const auto& item : *saves) {
                CloudSave save{Str(item, "platform"), Str(item, "title_id"), Str(item, "name"),
                               Str(item, "icon"), {}};
                if (const auto versions = item.find("versions");
                    versions != item.end() && versions->is_array()) {
                    for (const auto& v : *versions) {
                        const auto id = v.find("id");
                        const auto conflict = v.find("conflict");
                        save.versions.push_back(
                            {id != v.end() && id->is_number_integer() ? id->get<s64>() : 0,
                             v.value("number", 0),
                             conflict != v.end() && conflict->is_boolean() && conflict->get<bool>(),
                             number(v, "size"), Str(v, "device"), Str(v, "saved_at")});
                    }
                }
                // Newest first whatever order the site sent: the page reads versions.front().
                std::sort(save.versions.begin(), save.versions.end(),
                          [](const SaveVersionInfo& a, const SaveVersionInfo& b) {
                              return a.number > b.number;
                          });
                out.saves.push_back(std::move(save));
            }
        }
        out.ok = true;
    } catch (const nlohmann::json::exception& e) {
        out.error = fmt::format("Unexpected saves response: {}", e.what());
    }
    return out;
}

std::string DeleteSaveVersion(s64 version_id) {
    const std::string bearer = Bearer();
    if (bearer.empty()) {
        return std::string{Errors::NotSignedIn};
    }
    const auto result =
        Send("DELETE", fmt::format("/api/v1/me/saves/versions/{}", version_id), {}, bearer);
    if (ClearSessionIfRejected(result)) {
        return std::string{SessionExpired};
    }
    if (!result) {
        return std::string{Unreachable};
    }
    if (result->status / 100 != 2) {
        return ErrorFrom(result->body, fmt::format("Could not delete it (HTTP {}).", result->status));
    }
    return {};
}

std::optional<std::vector<u8>> FetchSitePath(const std::string& path) {
    // Only a path on the site: this is a value the site handed back, and it must not be able to
    // point the request anywhere else.
    if (!path.starts_with("/") || path.starts_with("//")) {
        return std::nullopt;
    }
    return FetchPublic(path);
}

std::string StatusUrl() {
    static const std::string url = [] {
        if (const char* env = std::getenv("OPENPAK_STATUS"); env && *env) {
            if (const auto sanitized = SanitizeBaseUrl(env)) {
                return *sanitized;
            }
        }
        // The status page lives beside the site: https://status.<site host>.
        const std::string base = BaseUrl();
        const auto authority = base.find("://");
        return base.substr(0, authority + 3) + "status." + base.substr(authority + 3);
    }();
    return url;
}

ServiceStatus GetServiceStatus() {
    ServiceStatus out;
    out.url = StatusUrl();
    httplib::Client client{out.url};
    client.set_connection_timeout(TimeoutSeconds);
    client.set_read_timeout(TimeoutSeconds);
    client.set_follow_location(true);
    ApplyCaCertPath(client);
    const auto result = client.Get("/api/status", httplib::Headers{{"User-Agent", "citron-openpak"}});
    if (!result || result->status != 200) {
        return out;
    }
    try {
        // The status page encodes its own view struct, so the keys are Go's field names.
        const auto json = nlohmann::json::parse(result->body);
        out.headline = Str(json, "Headline");
        out.sub = Str(json, "Sub");
        out.state = Str(json, "State");
        if (const auto groups = json.find("Groups"); groups != json.end() && groups->is_array()) {
            for (const auto& group : *groups) {
                const auto services = group.find("Services");
                if (services == group.end() || !services->is_array()) {
                    continue;
                }
                for (const auto& service : *services) {
                    const auto up = service.find("Up");
                    const auto uptime = service.find("Uptime");
                    out.services.push_back(
                        {Str(group, "Title"), Str(service, "Name"),
                         up != service.end() && up->is_boolean() && up->get<bool>(),
                         uptime != service.end() && uptime->is_number() ? uptime->get<double>() : 0,
                         Str(service, "Latency")});
                }
            }
        }
        out.ok = true;
    } catch (const nlohmann::json::exception& e) {
        OPENPAK_LOG_WARNING("Unexpected status page response: {}", e.what());
    }
    return out;
}

void RevokeToken(const std::string& bearer) {
    if (!bearer.empty()) {
        Send("DELETE", "/api/v1/token", {}, bearer);
    }
}

std::optional<std::vector<u8>> DownloadModPackage(const Mod& mod) {
    auto package = FetchPublic(mod.package_url);
    if (!package) {
        return std::nullopt;
    }
    const std::string actual = Sha256Hex(*package);
    if (!mod.sha256.empty() && actual != openpak::Platform::ToLower(mod.sha256)) {
        OPENPAK_LOG_WARNING("{} did not match its published hash ({} != {}); refusing it",
                            mod.name, actual, mod.sha256);
        return std::nullopt;
    }
    return package;
}

NewsManifest GetNewsManifest(const std::string& title_id_hex) {
    NewsManifest out;
    const auto json = GetJson(fmt::format("/api/emulator/v1/bcat/titles/{}",
                                          openpak::Platform::ToLower(title_id_hex)), {});
    if (!json) {
        return out;
    }
    out.ok = true;
    out.valid_from = Str(*json, "valid_from");
    if (json->contains("files") && (*json)["files"].is_array()) {
        for (const auto& file : (*json)["files"]) {
            const auto size = file.find("size");
            out.files.push_back({Str(file, "path"),
                                 size != file.end() && size->is_number_unsigned() ? size->get<u64>() : 0,
                                 Str(file, "url")});
        }
    }
    return out;
}

std::optional<std::vector<u8>> DownloadNewsFile(const NewsFile& file) {
    return FetchPublic(file.url);
}

NetworkStatus GetStatus() {
    NetworkStatus out;
    const auto json = GetJson("/api/v1/status", {});
    if (!json) {
        return out;
    }
    out.ok = true;
    const auto players = json->find("players_online");
    out.players_online = players != json->end() && players->is_number() ? players->get<int>() : 0;
    const auto read = [&](const char* list, const char* key, auto& into) {
        if (!json->contains(list) || !(*json)[list].is_array()) {
            return;
        }
        for (const auto& row : (*json)[list]) {
            const auto count = row.find("players");
            into.emplace_back(Str(row, key),
                              count != row.end() && count->is_number() ? count->get<int>() : 0);
        }
    };
    read("titles", "title_id", out.titles);
    read("networks", "namespace", out.networks);
    return out;
}

std::optional<int> PingBackend() {
    const auto start = std::chrono::steady_clock::now();
    const auto result = Send("GET", "/healthz", {}, {});
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    if (!result || result->status != 200) {
        return std::nullopt;
    }
    return static_cast<int>(elapsed_ms.count());
}

int PostCrashReport(const std::string& body, const std::string& content_type) {
    // The bearer when there is one, so the site can tell whose report it is. A 401 here does not
    // sign anybody out: a crash report is not the place to find out a token has lapsed.
    const auto result = Send("POST", "/api/v1/crash-reports", body,
                             Common::OpenPakAccount::GetBearer(), {}, content_type);
    return result ? result->status : 0;
}

#if defined(__cpp_lib_span)
std::string PushSave(const std::string& title_id_hex, std::span<const u8> data) {
    return PushSave(title_id_hex, data.data(), data.size());
}
#endif

} // namespace WebService::OpenPakApi
