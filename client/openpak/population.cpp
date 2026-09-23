// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "openpak/population.h"

#include <array>
#include <filesystem>

#include <httplib.h>

#if defined(__ANDROID__) && __has_include(<openssl/cert.h>)
// The patched OpenSSL the emulator builds against ships the public roots as a PEM blob.
#include <openssl/cert.h>
#define OPENPAK_CERT_BLOB 1
#endif

#include "openpak/log.h"
#include "openpak/platform.h"

namespace WebService {

namespace {

void ApplyCaCertPath(httplib::Client& client) {
#if defined(__ANDROID__) && defined(OPENPAK_CERT_BLOB)
    client.load_ca_cert_store(kCert, sizeof(kCert));
#elif defined(__ANDROID__)
    // A TLS library without the blob (Azahar's LibreSSL): the phone's own public roots.
    const std::string& roots = openpak::Platform::AndroidSystemRoots();
    client.load_ca_cert_store(roots.data(), roots.size());
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

std::optional<std::string> FetchOpenPakPopulationJson() {
    httplib::Client client{"https://raw.githubusercontent.com"};
    client.set_connection_timeout(10);
    client.set_read_timeout(10);
    client.set_follow_location(true);
    ApplyCaCertPath(client);

    const auto result = client.Get("/CollectingW/legacy-population/main/data/population.json");
    if (!result || result->status != 200) {
        OPENPAK_LOG_WARNING("legacy-population fetch failed (HTTP {})",
                    result ? result->status : 0);
        return std::nullopt;
    }
    return result->body;
}

} // namespace WebService
