// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "openpak/qt/network_probe.h"

#include <cstdlib>
#include <optional>
#include <thread>

#include <QApplication>
#include <QPointer>

#include "openpak/nat_check.h"

#include "openpak/qt/host.h"
#include "openpak/qt/strings.h"

#ifdef ENABLE_WEB_SERVICE
#include "openpak/api.h"
#endif

namespace {

// The redirect's own fallback chain: the host's server address, with the second responder's
// own env var for a deployment whose network profile does not name it.
std::string GetConfiguredHost(const std::string& setting, const char* env_var) {
    if (!setting.empty()) {
        return setting;
    }
    if (const char* env = std::getenv(env_var); env && *env) {
        return env;
    }
    return {};
}
} // namespace

ExternalReferenceProbe::ExternalReferenceProbe(QObject* parent) : QObject(parent) {}

ExternalReferenceProbe::~ExternalReferenceProbe() = default;

void ExternalReferenceProbe::ProbeNat() {
    if (nat_running) {
        return;
    }
    nat_running = true;
    emit NatStatusChanged(NatStatus::Checking);

    const std::string server =
        GetConfiguredHost(openpak::qt::Host::Current()->ServerIp(), "OPENPAK_SERVER_IP");
    std::optional<std::pair<std::string, std::string>> targets = openpak::nat::Targets(server);
    if (!targets) {
        const std::string second =
            GetConfiguredHost(openpak::qt::Host::Current()->NatIp(), "OPENPAK_NAT_IP");
        if (!server.empty() && !second.empty() && second != server) {
            targets = std::pair{server, second};
        }
    }

    QPointer<ExternalReferenceProbe> self{this};
    std::thread{[self, targets] {
        const openpak::nat::Result result = targets
                                                ? openpak::nat::Run(targets->first, targets->second)
                                                : openpak::nat::Result{};
        QMetaObject::invokeMethod(
            qApp,
            [self, result] {
                if (!self) {
                    return;
                }
                self->nat_running = false;
                self->last_type = QChar::fromLatin1(result.Type());
                self->last_detail =
                    result.mapping == openpak::nat::Mapping::Unknown
                        ? openpak::qt::Tr("Neither NAT check server answered on UDP 10025.")
                        : openpak::qt::Tr("%1 mapping, %2 filtering, seen as %3")
                              .arg(QString::fromLatin1(openpak::nat::Name(result.mapping)),
                                   QString::fromLatin1(openpak::nat::Name(result.filtering)),
                                   QString::fromStdString(result.external));
                const char type = result.Type();
                emit self->NatStatusChanged(type == 'A' || type == 'B' ? NatStatus::Open
                                            : type == 'F'              ? NatStatus::Unknown
                                                                       : NatStatus::Strict);
            },
            Qt::QueuedConnection);
    }}.detach();
}

void ExternalReferenceProbe::PingBackend() {
#ifdef ENABLE_WEB_SERVICE
    std::thread{[this] {
        const auto ms = WebService::OpenPakApi::PingBackend();
        QMetaObject::invokeMethod(
            this, [this, ms] { emit PingResult(ms.value_or(-1)); }, Qt::QueuedConnection);
    }}.detach();
#else
    emit PingResult(-1);
#endif
}
