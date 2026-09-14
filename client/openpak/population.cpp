// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "openpak/population.h"

#include <array>
#include <filesystem>

#include <httplib.h>

#ifdef __ANDROID__
// The patched OpenSSL the emulator builds against ships the public roots as a PEM blob.
#include <openssl/cert.h>
#endif

#include "openpak/log.h"

namespace WebService {

namespace {

void ApplyCaCertPath(httplib::Client& client) {
#ifdef __ANDROID__
    client.load_ca_cert_store(kCert, sizeof(kCert));
#elif defined(__linux__)
    static constexpr std::array<const char*, 4> candidates{
        "/etc/ssl/certs/ca-certificates.crt",
        "/etc/pki/tls/certs/ca-bundle.crt",
        "/etc/ssl/cert.pem",
        "/etc/ssl/ca-bundle.pem",
    };
    for (const char* path : candidates) {
        if (std::filesystem::exists(path)) {
            client.set_ca_cert_path(path);
            return;
        }
    }
#else
    (void)client;
#endif
}

} // Anonymous namespace

std::optional<std::string> FetchNextendoPopulationJson() {
    httplib::Client client{"https://raw.githubusercontent.com"};
    client.set_connection_timeout(10);
    client.set_read_timeout(10);
    client.set_follow_location(true);
    ApplyCaCertPath(client);

    const auto result = client.Get("/CollectingW/nextendo-population/main/data/population.json");
    if (!result || result->status != 200) {
        OPENPAK_LOG_WARNING("nextendo-population fetch failed (HTTP {})",
                    result ? result->status : 0);
        return std::nullopt;
    }
    return result->body;
}

} // namespace WebService
