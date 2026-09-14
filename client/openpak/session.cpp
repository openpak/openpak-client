// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>

#include <fmt/format.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>

#include "openpak/account.h"
#include "openpak/api.h"
#include "openpak/friends_cache.h"
#include "openpak/log.h"
#include "openpak/platform.h"
#include "openpak/session.h"

namespace openpak::client::session {

namespace {

// Nintendo's own names, on purpose: OpenPak answers to them and routes by SNI, so one address
// serves the whole chain and a title sees the names it was built to see.
constexpr const char* DauthHost = "dauth-lp1.ndas.srv.nintendo.net";
constexpr const char* AauthHost = "aauth-lp1.ndas.srv.nintendo.net";
constexpr const char* BaasHost = "e0d67c509fb203858ebcb2fe3f88c2aa.baas.nintendo.com";

// Where an invitation sent by a console lands. The website's /api/v1/me/invitations is the
// core's own store and never sees one of these, so this is the only place to look.
constexpr const char* FiveHost = "app.lp1.five.nintendo.net";

// The Nintendo Account surface: the sign-in page and the token exchange behind linking.
constexpr const char* NaHost = "accounts.nintendo.com";

// Echoed back by the server, which knows the console from its client certificate rather than
// from anything in the request body.
constexpr const char* BaasClientId = "8f849b5d34778d8e";

constexpr int TimeoutSeconds = 20;

// Three renewals inside the lease the server hands out, so one lost request is not a person
// blinking offline.
constexpr int HeartbeatSeconds = 10;

// The inbox is store-and-forward and changes rarely; every third beat is often enough to hear
// about an invitation while the person still cares.
constexpr int InvitationEveryNthBeat = 3;

// The wire carries no expiry: the server keeps an invitation for a day and prunes it.
constexpr std::int64_t InvitationLifetimeSeconds = 24 * 60 * 60;

struct State {
    std::mutex mutex;

    std::string server_host;
    int port = 443;
    std::string ca_path;

    std::string device_id;
    std::string device_password;

    std::string id_token;
    std::chrono::system_clock::time_point id_token_expiry;
    std::string id_token_application; ///< The title the cached id_token was minted for.
    std::string application_token; ///< User-scoped after login: what presence speaks with.
    std::string user_id;
    std::uint64_t nsa_id = 0;
    std::string nickname;
    std::string friend_code;

    std::string application_id;      ///< The bound title, 16 hex digits, or empty on the game list.
    std::string application_version; ///< The bound title's own version string.

    std::vector<std::uint8_t> ca_der; ///< The CA as the guest's certificate store wants it.
    std::vector<Invitation> invitations;
    std::map<std::string, std::string> sender_names;
    std::vector<std::string> dismissed;

    std::function<void(const Invitation&)> on_arrival;
    std::function<std::string()> current_title_id;

    std::thread heartbeat;
    std::condition_variable heartbeat_wake;
    std::mutex heartbeat_mutex;
    std::atomic<bool> heartbeat_running{false};

    /// The heartbeat outlives everything else here, so it has to be stopped before the members it
    /// reads are destroyed. A std::thread that is still joinable when it is destroyed calls
    /// std::terminate, which is an abort on every quit -- this runs from __cxa_atexit when the
    /// function-local static below is torn down. Stop and join, never detach: detaching trades
    /// the abort for a thread reading this object after it is gone.
    ~State() {
        if (heartbeat_running.exchange(false)) {
            heartbeat_wake.notify_all();
        }

        if (heartbeat.joinable()) {
            heartbeat.join();
        }
    }

    State() = default;
    State(const State&) = delete;
    State& operator=(const State&) = delete;
};

State& Get() {
    static State state;
    return state;
}

std::string Base64(const unsigned char* data, std::size_t size) {
    static constexpr char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    std::size_t i = 0;
    while (i + 2 < size) {
        const std::uint32_t n = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out.push_back(table[(n >> 18) & 63]);
        out.push_back(table[(n >> 12) & 63]);
        out.push_back(table[(n >> 6) & 63]);
        out.push_back(table[n & 63]);
        i += 3;
    }
    if (i < size) {
        std::uint32_t n = static_cast<std::uint32_t>(data[i]) << 16;
        if (i + 1 < size) {
            n |= static_cast<std::uint32_t>(data[i + 1]) << 8;
        }
        out.push_back(table[(n >> 18) & 63]);
        out.push_back(table[(n >> 12) & 63]);
        out.push_back(i + 1 < size ? table[(n >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

std::vector<std::uint8_t> Base64Decode(std::string_view text) {
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };

    std::vector<std::uint8_t> out;
    int bits = 0;
    int accumulator = 0;

    for (const char c : text) {
        const int digit = value(c);
        if (digit < 0) {
            continue; // padding and whitespace carry nothing
        }
        accumulator = (accumulator << 6) | digit;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((accumulator >> bits) & 0xFF));
        }
    }

    return out;
}

/// A key for file names: one device account and one certificate per server, because pointing
/// the emulator at a different OpenPak means a different device and reusing one would send the
/// wrong password to a stranger.
std::string ServerKey(const State& state) {
    std::string key = fmt::format("{}-{}", state.server_host, state.port);
    for (char& c : key) {
        if (c == ':' || c == '/' || c == '\\') {
            c = '-';
        }
    }
    return key;
}

/// Where to reach OpenPak, copied out of the state rather than read through it.
///
/// Two threads want these at once: the heartbeat, which is asking every ten seconds, and
/// whatever calls Configure -- on Android that is the app at launch AND the account service the
/// first time a game asks for an id_token. Reading the strings straight out of the state while
/// the other thread rewrites them is a data race, and the crash it produces lands exactly where
/// a title reaches the network.
struct Endpoint {
    std::string host;
    int port = 443;
    std::string ca_path;
    std::string key; ///< For file names: one device account and certificate per server.
};

Endpoint EndpointOfLocked(const State& state) {
    return {state.server_host, state.port, state.ca_path, ServerKey(state)};
}

Endpoint EndpointSnapshot(State& state) {
    std::lock_guard lock{state.mutex};
    return EndpointOfLocked(state);
}

std::filesystem::path DevicePath(const Endpoint& endpoint, const char* suffix) {
    return Platform::ConfigDir() / "openpak" / fmt::format("device-{}.{}", endpoint.key, suffix);
}

/// A self-signed certificate, kept per server, presented on every OpenPak host.
///
/// A real console proves itself with the certificate in its NAND, and that is how the server
/// tells consoles apart; an emulator has none, and without this every install would arrive as
/// the same anonymous device. It asserts nothing about hardware -- it is a stable identifier.
bool EnsureClientCertificate(const Endpoint& endpoint) {
    const std::filesystem::path cert_path = DevicePath(endpoint, "pem");
    const std::filesystem::path key_path = DevicePath(endpoint, "key");

    if (std::filesystem::exists(cert_path) && std::filesystem::exists(key_path)) {
        return true;
    }

    EVP_PKEY* key = EVP_RSA_gen(2048);
    if (key == nullptr) {
        OPENPAK_LOG_ERROR("[OpenPak] Could not generate a device key");
        return false;
    }

    X509* cert = X509_new();
    ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
    X509_set_version(cert, 2);

    // A day of backdating, because a clock that is behind rejects a not-yet-valid certificate.
    X509_gmtime_adj(X509_getm_notBefore(cert), -86400);
    X509_gmtime_adj(X509_getm_notAfter(cert), 10L * 365 * 86400);
    X509_set_pubkey(cert, key);

    // Built rather than borrowed from the certificate: OpenSSL 3 hands out the subject name
    // mutable and OpenSSL 4 hands out the same call as const, and Android builds against the
    // newer one. Setting a name we own is the spelling both accept.
    X509_NAME* name = X509_NAME_new();
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>("OpenPak Eden device"), -1,
                               -1, 0);

    bool ok = X509_set_subject_name(cert, name) != 0 && X509_set_issuer_name(cert, name) != 0;

    X509_NAME_free(name);

    ok = ok && X509_sign(cert, key, EVP_sha256()) != 0;

    if (ok) {
        Platform::CreateParentDirs(cert_path);

        FILE* cert_file = std::fopen(cert_path.string().c_str(), "wb");
        ok = cert_file != nullptr && PEM_write_X509(cert_file, cert) != 0;
        if (cert_file != nullptr) {
            std::fclose(cert_file);
        }

        FILE* key_file = std::fopen(key_path.string().c_str(), "wb");
        ok = ok && key_file != nullptr &&
             PEM_write_PrivateKey(key_file, key, nullptr, nullptr, 0, nullptr, nullptr) != 0;
        if (key_file != nullptr) {
            std::fclose(key_file);
        }
    }

    X509_free(cert);
    EVP_PKEY_free(key);

    if (!ok) {
        OPENPAK_LOG_ERROR("[OpenPak] Could not write the device certificate");
    }

    return ok;
}

/// An HTTPS client for one Nintendo hostname that reaches the OpenPak server instead.
///
/// The url keeps the Nintendo name so SNI and the Host header carry it -- that name is how
/// OpenPak decides which service answers -- while the socket goes where the user pointed it.
std::unique_ptr<httplib::SSLClient> MakeClient(const Endpoint& endpoint, const char* host) {
    auto client = std::make_unique<httplib::SSLClient>(host, endpoint.port,
                                                       DevicePath(endpoint, "pem").string(),
                                                       DevicePath(endpoint, "key").string());

    client->set_hostname_addr_map({{host, endpoint.host}});

    // Pin the OpenPak CA when there is one. Without it the chain still runs unverified, which is
    // the posture the guest's own TLS already takes while OpenPak is on (ssl_backend_openssl):
    // refusing here would mean a fresh install could not sign in to fetch the CA in the first
    // place. ponytail: fetch ca.pem on first run and make this unconditional.
    if (!endpoint.ca_path.empty() && std::filesystem::exists(endpoint.ca_path)) {
        client->set_ca_cert_path(endpoint.ca_path);
        client->enable_server_certificate_verification(true);
    } else {
        client->enable_server_certificate_verification(false);
    }

    client->set_connection_timeout(TimeoutSeconds, 0);
    client->set_read_timeout(TimeoutSeconds, 0);
    client->set_write_timeout(TimeoutSeconds, 0);
    client->set_follow_location(false);

    return client;
}

httplib::Headers Bearer(const std::string& token) {
    if (token.empty()) {
        return {};
    }
    return {{"Authorization", "Bearer " + token}};
}

/// A response body parsed, or nothing at all. The body carries the reason a request failed; a
/// bare status code has cost days on this chain before, so it is logged with one.
std::optional<nlohmann::json> Parsed(const httplib::Result& result, std::string_view what) {
    if (!result) {
        OPENPAK_LOG_WARNING("[OpenPak] {} did not reach the server: {}", what,
                            httplib::to_string(result.error()));
        return std::nullopt;
    }
    if (result->status < 200 || result->status >= 300) {
        OPENPAK_LOG_WARNING("[OpenPak] {} returned {}: {}", what, result->status, result->body);
        return std::nullopt;
    }

    nlohmann::json parsed = nlohmann::json::parse(result->body, nullptr, false);
    if (parsed.is_discarded()) {
        OPENPAK_LOG_WARNING("[OpenPak] {} answered with something that is not JSON", what);
        return std::nullopt;
    }

    return parsed;
}

std::string Text(const nlohmann::json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

void LoadDeviceAccount(State& state) {
    const std::string contents = Platform::ReadFile(DevicePath(EndpointOfLocked(state), "json"));
    if (contents.empty()) {
        return;
    }

    const nlohmann::json parsed = nlohmann::json::parse(contents, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        OPENPAK_LOG_WARNING("[OpenPak] Ignoring an unreadable device account file");
        return;
    }

    state.device_id = Text(parsed, "id");
    state.device_password = Text(parsed, "password");
}

void SaveDeviceAccount(const State& state) {
    const nlohmann::json out{{"id", state.device_id}, {"password", state.device_password}};
    const std::filesystem::path path = DevicePath(EndpointOfLocked(state), "json");

    Platform::CreateParentDirs(path);
    Platform::WriteFile(path, out.dump());
}

/// Five minutes of slack: a token that expires mid-session is worse than one fetched early.
/// A token also belongs to the title it names: one minted for another game (or for no game) is
/// refused by the title's own online stack, so a changed binding is as stale as an old expiry.
bool Fresh(const State& state) {
    return !state.id_token.empty() && state.id_token_application == state.application_id &&
           std::chrono::system_clock::now() < state.id_token_expiry - std::chrono::minutes(5);
}

std::int64_t Now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// The login chain. With an OpenPak account's id_token it goes to /federation instead, which is
/// the same login plus the binding to a person -- after it, the id_token the guest receives
/// carries the nnex claim a title server needs to know who is playing.
bool LoginLocked(State& state, const std::string& account_id_token, std::string& error) {
    const Endpoint endpoint = EndpointOfLocked(state);

    if (!EnsureClientCertificate(endpoint)) {
        error = "Could not create this install's device certificate.";
        return false;
    }

    auto dauth = MakeClient(endpoint, DauthHost);

    const auto challenge_body = Parsed(dauth->Post("/v8/challenge"), "dauth challenge");
    if (!challenge_body) {
        error = fmt::format("Could not reach OpenPak at {}.", state.server_host);
        return false;
    }

    unsigned char mac[32];
    RAND_bytes(mac, sizeof(mac));

    // The server verifies none of this -- it has no Nintendo key to check the MAC with, and says
    // so in its own source. Sent because the shape is the console's.
    const nlohmann::json device_request{
        {"system_version", "22.2.0-0.0"},
        {"fw_revision", "0"},
        {"key_generation", 11},
        {"challenge", Text(*challenge_body, "challenge")},
        {"mac", Base64(mac, sizeof(mac))},
        {"ist", false},
        {"token_requests", nlohmann::json::array({{{"client_id", BaasClientId}}})},
    };

    const auto device_tokens =
        Parsed(dauth->Post("/v8/device_auth_tokens", device_request.dump(), "application/json"),
               "device auth tokens");
    if (!device_tokens || !device_tokens->contains("results") ||
        (*device_tokens)["results"].empty()) {
        error = "OpenPak refused this device.";
        return false;
    }

    const std::string device_token =
        Text((*device_tokens)["results"][0], "device_auth_token");

    // The per-title binding. A console's baas login carries an application_auth_token naming the
    // running title, and the title's own online stack checks it: an id_token minted for another
    // game, or for no game at all, connects and then never speaks -- the emulator equivalent of a
    // console that is online but will not start a session. aauth issues one against the device
    // the token above just vouched for, so the chain keeps the console's shape end to end.
    std::string application_auth_token;
    if (!state.application_id.empty()) {
        auto aauth = MakeClient(endpoint, AauthHost);

        const httplib::Params application{
            {"application_id", state.application_id},
            {"application_version", state.application_version},
        };

        const auto answered =
            Parsed(aauth->Post("/v5/application_auth_token", Bearer(device_token), application),
                   "application auth token");
        if (answered) {
            application_auth_token = Text(*answered, "application_auth_token");
        }
    }

    auto baas = MakeClient(endpoint, BaasHost);

    const httplib::Params exchange{
        {"assertion", device_token},
        {"grant_type", "urn:ietf:params:oauth:grant-type:token-exchange"},
    };

    const auto application =
        Parsed(baas->Post("/1.0.0/application/token", exchange), "application token");
    if (!application) {
        error = "OpenPak would not issue an application token.";
        return false;
    }

    std::string application_token = Text(*application, "accessToken");
    state.application_token = application_token;

    if (state.device_id.empty()) {
        LoadDeviceAccount(state);
    }

    if (state.device_id.empty()) {
        const auto created = Parsed(
            baas->Post("/1.0.0/users", Bearer(application_token), "{}", "application/json"),
            "device account");
        if (!created || !created->contains("deviceAccounts") ||
            (*created)["deviceAccounts"].empty()) {
            error = "OpenPak would not create a device account.";
            return false;
        }

        const nlohmann::json& account = (*created)["deviceAccounts"][0];
        state.device_id = Text(account, "id");
        state.device_password = Text(account, "password");

        SaveDeviceAccount(state);
    }

    httplib::Params login_params{{"id", state.device_id}, {"password", state.device_password}};
    if (!account_id_token.empty()) {
        login_params.emplace("idToken", account_id_token);
    }
    if (!application_auth_token.empty()) {
        login_params.emplace("appAuthNToken", application_auth_token);
    }

    const char* path = account_id_token.empty() ? "/1.0.0/login" : "/1.0.0/federation";

    const auto login = Parsed(baas->Post(path, Bearer(application_token), login_params), "login");
    if (!login) {
        error = account_id_token.empty() ? "OpenPak would not sign this device in."
                                         : "OpenPak would not link this account.";
        return false;
    }

    state.id_token = Text(*login, "idToken");
    state.id_token_application = state.application_id;

    const auto expires = login->find("expiresIn");
    const int seconds = expires != login->end() && expires->is_number() ? expires->get<int>() : 3600;
    state.id_token_expiry = std::chrono::system_clock::now() + std::chrono::seconds(seconds);

    if (login->contains("user")) {
        const nlohmann::json& user = (*login)["user"];

        state.user_id = Text(user, "id");
        state.nickname = Text(user, "nickname");

        if (user.contains("links") && user["links"].contains("friendCode")) {
            state.friend_code = Text(user["links"]["friendCode"], "id");
        }

        state.nsa_id = std::strtoull(state.user_id.c_str(), nullptr, 16);
    }

    // Presence is user-scoped: the token it speaks with must carry the BAAS user as its subject,
    // which is the login's own accessToken and not the device-scoped one the exchange minted --
    // two tokens, same issuer, same typ, different subject, and the user-scoped route refuses the
    // device-shaped one with an opaque 401.
    const std::string user_token = Text(*login, "accessToken");
    if (!user_token.empty()) {
        state.application_token = user_token;
    }

    OPENPAK_LOG_INFO("[OpenPak] Signed in as {} on {}",
                     state.nickname.empty() ? state.device_id : state.nickname, state.server_host);

    return true;
}

void Presence(State& state, const std::string& status) {
    std::string user_id;
    std::string device_id;
    std::string token;
    std::function<std::string()> title_of;
    {
        std::lock_guard lock{state.mutex};
        user_id = state.user_id;
        device_id = state.device_id;
        token = state.application_token;
        title_of = state.current_title_id;
    }

    if (user_id.empty() || device_id.empty() || token.empty()) {
        return;
    }

    const std::string title = status == "OFFLINE" || !title_of ? std::string{} : title_of();

    // While a title runs the presence says what is being played, not merely that the console is
    // on: the friends module reads the four app fields to draw "playing X", and acdIndex goes
    // over the wire as a JSON number because the console sends it unquoted.
    nlohmann::json patch = nlohmann::json::array();
    if (title.empty()) {
        patch.push_back({{"op", "replace"}, {"path", "/presence/state"}, {"value", status}});
    } else {
        patch.push_back({{"op", "replace"}, {"path", "/presence/state"}, {"value", "PLAYING"}});
        patch.push_back({{"op", "replace"},
                         {"path", "/presence/extras/friends/appInfo:appId"},
                         {"value", title}});
        patch.push_back({{"op", "replace"},
                         {"path", "/presence/extras/friends/appInfo:presenceGroupId"},
                         {"value", title}});
        patch.push_back({{"op", "replace"},
                         {"path", "/presence/extras/friends/appInfo:acdIndex"},
                         {"value", 0}});
    }

    auto baas = MakeClient(EndpointSnapshot(state), BaasHost);

    const auto result =
        baas->Patch(fmt::format("/1.0.0/users/{}/device_accounts/{}", user_id, device_id),
                    Bearer(token), patch.dump(), "application/json");

    if (!result || result->status != 200) {
        OPENPAK_LOG_DEBUG("[OpenPak] Presence {} did not land", status);
    }
}

/// Who a BAAS id belongs to, by the lookup the friends module uses, kept once found.
std::string SenderName(State& state, const std::string& sender_id, const std::string& token) {
    {
        std::lock_guard lock{state.mutex};
        const auto it = state.sender_names.find(sender_id);
        if (it != state.sender_names.end()) {
            return it->second;
        }
    }

    auto baas = MakeClient(EndpointSnapshot(state), BaasHost);

    const auto found =
        Parsed(baas->Get(fmt::format("/1.0.0/users?filter.id.$in={}", sender_id), Bearer(token)),
               "user lookup");
    if (!found || !found->contains("items")) {
        return {};
    }

    for (const nlohmann::json& user : (*found)["items"]) {
        const std::string nickname = Text(user, "nickname");
        if (!nickname.empty()) {
            std::lock_guard lock{state.mutex};
            state.sender_names[sender_id] = nickname;
            return nickname;
        }
    }

    return {};
}

} // namespace

std::vector<std::uint8_t> CaCertificateDer() {
    State& state = Get();

    std::string path;
    {
        std::lock_guard lock{state.mutex};
        if (!state.ca_der.empty()) {
            return state.ca_der;
        }
        path = state.ca_path;
    }

    if (path.empty()) {
        return {};
    }

    std::string pem = Platform::ReadFile(path);

    if (pem.empty()) {
        // Not on disk yet: the website serves it over ordinary public TLS, which is the one
        // request in this whole stack that does not depend on trusting OpenPak first.
        if (!WebService::OpenPakApi::FetchCA()) {
            OPENPAK_LOG_WARNING("[OpenPak] No CA available; a title that checks the chain itself "
                                "will refuse the connection");
            return {};
        }
        pem = Platform::ReadFile(path);
    }

    // PEM is the DER the store wants, base64 between the armour lines. The first certificate is
    // the CA; anything after it in the file is not what a root slot holds.
    const std::size_t begin = pem.find("-----BEGIN CERTIFICATE-----");
    const std::size_t end = pem.find("-----END CERTIFICATE-----", begin);

    if (begin == std::string::npos || end == std::string::npos) {
        OPENPAK_LOG_WARNING("[OpenPak] {} is not a PEM certificate", path);
        return {};
    }

    const std::size_t body = begin + std::strlen("-----BEGIN CERTIFICATE-----");
    std::vector<std::uint8_t> der = Base64Decode(std::string_view{pem}.substr(body, end - body));

    {
        std::lock_guard lock{state.mutex};
        state.ca_der = der;
    }

    OPENPAK_LOG_INFO("[OpenPak] CA loaded for the guest's own certificate store ({} bytes)",
                     der.size());

    return der;
}

void Configure(std::string server_host, int port, std::string ca_path) {
    State& state = Get();
    std::lock_guard lock{state.mutex};

    state.server_host = std::move(server_host);
    state.port = port;
    state.ca_path = ca_path.empty() ? (Platform::ConfigDir() / "openpak" / "ca.pem").string()
                                    : std::move(ca_path);

    LoadDeviceAccount(state);
}

void SetApplication(std::string application_id, std::string application_version) {
    State& state = Get();
    std::lock_guard lock{state.mutex};

    state.application_id = std::move(application_id);
    state.application_version = std::move(application_version);
}

bool Enabled() {
    State& state = Get();
    std::lock_guard lock{state.mutex};

    return !state.server_host.empty();
}

bool Ensure() {
    if (!Enabled()) {
        return false;
    }

    State& state = Get();
    std::lock_guard lock{state.mutex};

    if (Fresh(state)) {
        return true;
    }

    std::string error;
    if (!LoginLocked(state, {}, error)) {
        // Leaving the token empty is the whole error path: the caller falls back to the offline
        // token and the game sees a console that is simply not online.
        OPENPAK_LOG_WARNING("[OpenPak] Sign-in failed: {}", error);
        return false;
    }

    return true;
}

std::string Link(const std::string& account_id_token) {
    if (!Enabled()) {
        return "No OpenPak server is configured.";
    }

    State& state = Get();
    std::lock_guard lock{state.mutex};

    std::string error;
    if (!LoginLocked(state, account_id_token, error)) {
        return error;
    }

    return {};
}

std::string LinkWithPassword(const std::string& email, const std::string& password) {
    if (!Enabled()) {
        return "No OpenPak server is configured.";
    }

    State& state = Get();
    std::lock_guard lock{state.mutex};

    const Endpoint endpoint = EndpointOfLocked(state);

    if (!EnsureClientCertificate(endpoint)) {
        return "Could not create this install's device certificate.";
    }

    auto accounts = MakeClient(endpoint, NaHost);

    const std::string authorize =
        fmt::format("/connect/1.0.0/authorize?response_type=code&client_id={}&redirect_uri={}",
                    BaasClientId, "openpak%3A%2F%2Flinked");

    const httplib::Params credentials{{"email", email}, {"password", password}};

    const auto answered = accounts->Post(authorize, credentials);
    if (!answered) {
        return fmt::format("Could not reach OpenPak at {}.", state.server_host);
    }

    // Right credentials redirect. Wrong ones come back as the sign-in page again, with the reason
    // written on it -- a 200, which is why this looks for the redirect rather than for success.
    const std::string location = answered->get_header_value("Location");
    if (location.empty()) {
        return "That e-mail and password were not accepted.";
    }

    std::string code;
    const std::size_t query = location.find_first_of("?#");
    for (std::size_t at = query == std::string::npos ? location.size() : query + 1;
         at < location.size();) {
        const std::size_t end = std::min(location.find('&', at), location.size());
        const std::string_view pair{location.data() + at, end - at};

        if (pair.starts_with("code=")) {
            code = std::string{pair.substr(5)};
            break;
        }

        at = end + 1;
    }

    if (code.empty()) {
        return "The sign-in came back without an authorization code.";
    }

    const httplib::Params exchange{
        {"code", code}, {"client_id", BaasClientId}, {"grant_type", "authorization_code"}};

    const auto token = Parsed(accounts->Post("/connect/1.0.0/api/token", exchange), "account token");
    if (!token) {
        return "OpenPak would not issue an account token.";
    }

    // Federation is the login that also binds, so this replaces the cached token with one that
    // carries the account -- and the nnex claim a title server reads.
    std::string error;
    if (!LoginLocked(state, Text(*token, "id_token"), error)) {
        return error;
    }

    return {};
}

std::string IdToken() {
    State& state = Get();
    std::lock_guard lock{state.mutex};
    return state.id_token;
}

std::uint64_t NetworkServiceAccountId() {
    State& state = Get();
    std::lock_guard lock{state.mutex};
    return state.nsa_id;
}

std::string UserId() {
    State& state = Get();
    std::lock_guard lock{state.mutex};
    return state.user_id;
}

std::string Nickname() {
    State& state = Get();
    std::lock_guard lock{state.mutex};
    return state.nickname;
}

std::string FriendCode() {
    State& state = Get();
    std::lock_guard lock{state.mutex};
    return state.friend_code;
}

bool Linked() {
    return !Nickname().empty();
}

void SetInvitationHook(std::function<void(const Invitation&)> on_arrival) {
    State& state = Get();
    std::lock_guard lock{state.mutex};
    state.on_arrival = std::move(on_arrival);
}

std::vector<Invitation> Invitations() {
    State& state = Get();
    std::lock_guard lock{state.mutex};
    return state.invitations;
}

bool RefreshInvitations() {
    State& state = Get();

    std::string user_id;
    std::string token;
    std::vector<std::string> dismissed;
    std::vector<Invitation> previous;
    {
        std::lock_guard lock{state.mutex};
        user_id = state.user_id;
        token = state.application_token;
        dismissed = state.dismissed;
        previous = state.invitations;
    }

    if (user_id.empty() || token.empty()) {
        return false;
    }

    auto five = MakeClient(EndpointSnapshot(state), FiveHost);

    // Read state is not a filter: the same account signed in on a console marks these read from
    // over there, and that is no reason for this machine to have missed it.
    const auto inbox = Parsed(
        five->Get(fmt::format("/v2/users/{}/invitations/inbox?invitation_types=friend", user_id),
                  Bearer(token)),
        "invitation inbox");

    if (!inbox || !inbox->contains("items")) {
        return false;
    }

    std::vector<Invitation> waiting;
    std::vector<Invitation> arrived;

    for (const nlohmann::json& item : (*inbox)["items"]) {
        // Ids are JSON numbers on the wire because the friends module parses them with %llu; a
        // string id would not survive that parser. Everything above here wants the digits.
        const auto id_field = item.find("id");
        if (id_field == item.end() || !id_field->is_number()) {
            continue;
        }

        Invitation invitation;
        invitation.id = std::to_string(id_field->get<std::uint64_t>());

        if (std::find(dismissed.begin(), dismissed.end(), invitation.id) != dismissed.end()) {
            continue;
        }

        invitation.sender_id = Text(item, "sender_id");
        invitation.title_id = Text(item, "application_id");

        // Absent when the sender carried no payload, which the wire says by omitting the field.
        invitation.app_param = Base64Decode(Text(item, "application_data"));

        const auto created = item.find("created_at");
        invitation.created_at =
            created != item.end() && created->is_number() ? created->get<std::int64_t>() : Now();
        invitation.expires_at = invitation.created_at + InvitationLifetimeSeconds;

        invitation.sender_name = SenderName(state, invitation.sender_id, token);
        if (invitation.sender_name.empty()) {
            invitation.sender_name = invitation.sender_id;
        }

        const bool known = std::any_of(previous.begin(), previous.end(),
                                       [&](const Invitation& seen) { return seen.id == invitation.id; });

        waiting.push_back(invitation);

        if (!known) {
            arrived.push_back(invitation);
        }
    }

    std::function<void(const Invitation&)> hook;
    {
        std::lock_guard lock{state.mutex};
        state.invitations = waiting;
        hook = state.on_arrival;
    }

    if (hook) {
        for (const Invitation& invitation : arrived) {
            hook(invitation);
        }
    }

    return true;
}

void DismissInvitation(const std::string& invitation_id) {
    State& state = Get();

    std::string token;
    {
        std::lock_guard lock{state.mutex};
        state.dismissed.push_back(invitation_id);
        std::erase_if(state.invitations,
                      [&](const Invitation& one) { return one.id == invitation_id; });
        token = state.application_token;
    }

    if (token.empty()) {
        return;
    }

    auto five = MakeClient(EndpointSnapshot(state), FiveHost);

    const nlohmann::json patch = nlohmann::json::array({
        {{"op", "replace"},
         {"path", fmt::format("/{}/extras/receiver/read", invitation_id)},
         {"value", true}},
    });

    const auto result =
        five->Patch("/v1/invitations", Bearer(token), patch.dump(), "application/json");

    if (!result || result->status != 200) {
        OPENPAK_LOG_DEBUG("[OpenPak] Marking invitation {} read did not land", invitation_id);
    }
}

void RefreshGuestFriends() {
    if (!Common::OpenPakAccount::IsLinked()) {
        return;
    }

    const WebService::OpenPakApi::FriendList fetched = WebService::OpenPakApi::GetFriends();
    if (!fetched.ok) {
        return;
    }

    std::vector<Common::NextendoFriends::Entry> cache;
    cache.reserve(fetched.friends.size());

    for (const auto& entry : fetched.friends) {
        cache.push_back({
            entry.pid,
            entry.name,
            entry.presence_status,
            entry.app_field,
            Base64Decode(entry.image_base64),
        });
    }

    Common::NextendoFriends::Set(std::move(cache));

    // What the running title published about itself goes the other way, or friends see somebody
    // sitting in a menu while they are hosting a farm.
    s32 status = 0;
    std::string app_field;

    if (Common::NextendoFriends::TakeLocalPresenceForPublish(status, app_field)) {
        WebService::OpenPakApi::PushPresence(status, app_field, {}, {});
    }
}

void StartHeartbeat(std::function<std::string()> current_title_id) {
    State& state = Get();

    {
        std::lock_guard lock{state.mutex};
        state.current_title_id = std::move(current_title_id);
    }

    if (state.heartbeat_running.exchange(true)) {
        return;
    }

    state.heartbeat = std::thread([&state] {
        for (int tick = 0;; tick++) {
            std::unique_lock lock{state.heartbeat_mutex};
            state.heartbeat_wake.wait_for(lock, std::chrono::seconds(HeartbeatSeconds));
            lock.unlock();

            if (!state.heartbeat_running.load()) {
                return;
            }

            if (tick % InvitationEveryNthBeat == 0) {
                RefreshInvitations();
            }

            // The guest's own friend list, kept warm here because nothing else on this build
            // does it. NEX titles poll friend:u in a loop and must never wait on a network call.
            RefreshGuestFriends();

            // Presence is not worth a louder failure than a debug line: the account simply goes
            // quiet, which is exactly what it should look like.
            Presence(state, "ONLINE");
        }
    });
}

void GoOffline() {
    State& state = Get();

    if (state.heartbeat_running.exchange(false)) {
        state.heartbeat_wake.notify_all();

        if (state.heartbeat.joinable()) {
            state.heartbeat.join();
        }
    }

    // Nothing reaches the server when a process is closed -- a build that is gone sends nothing,
    // including "I am gone" -- so this is the one moment it can be said. If it never arrives, the
    // lease says the same thing half a minute later.
    Presence(state, "OFFLINE");
}

} // namespace openpak::client::session
