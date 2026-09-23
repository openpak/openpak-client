// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later

#include "openpak/qt/friend_notifier.h"

#include <memory>
#include <thread>

#include <QCoreApplication>
#include <QPointer>

#include "openpak/api.h"
#include "openpak/compatibility.h"
#include "openpak/qt/pages_p.h"
#include "openpak/qt/host.h"
#include "openpak/qt/toast.h"

namespace openpak::qt {

namespace Api = WebService::OpenPakApi;

FriendNotifier::FriendNotifier(OpenPakToast* toast_, QObject* parent)
    : QObject(parent), toast(toast_) {
    timer.setInterval(30000);
    connect(&timer, &QTimer::timeout, this, &FriendNotifier::Poll);
    timer.start();
    QTimer::singleShot(0, this, &FriendNotifier::Poll);
}

void FriendNotifier::Reset() {
    first = true;
    online.clear();
    requests.clear();
    QTimer::singleShot(0, this, &FriendNotifier::Poll);
}

void FriendNotifier::Poll() {
    if (polling || !SignedIn()) {
        return;
    }
    polling = true;
    QPointer<FriendNotifier> self{this};
    std::thread([self] {
        compatibility::LoadNames();
        auto fetched = std::make_shared<Api::AccountFriends>(Api::GetAccountFriends());
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [self, fetched] {
                if (!self) {
                    return;
                }
                self->polling = false;
                if (!fetched->ok || !SignedIn()) {
                    return;
                }
                Host* host = Host::Current();
                std::map<std::string, std::string> now;
                for (const auto& entry : fetched->friends) {
                    if (!entry.online || entry.account_id.empty()) {
                        continue;
                    }
                    now[entry.account_id] = entry.title_id;
                    if (self->first) {
                        continue;
                    }
                    const QString name = QString::fromStdString(entry.display_name);
                    const auto before = self->online.find(entry.account_id);
                    const QString game = GameName(host, entry.title_id);
                    if (before == self->online.end()) {
                        self->toast->Show(game.isEmpty() ? Tr("%1 is online").arg(name)
                                                         : Tr("%1 is playing %2").arg(name, game),
                                          {}, {}, OpenPakToast::Kind::Online);
                    } else if (before->second != entry.title_id && !game.isEmpty()) {
                        self->toast->Show(Tr("%1 is playing %2").arg(name, game), {}, {},
                                          OpenPakToast::Kind::Online);
                    }
                }
                std::set<std::string> asking;
                for (const auto& request : fetched->requests) {
                    if (!request.incoming) {
                        continue;
                    }
                    const std::string key =
                        request.account_id.empty() ? std::to_string(request.pid) : request.account_id;
                    asking.insert(key);
                    if (!self->first && !self->requests.contains(key)) {
                        self->toast->Show(Tr("%1 wants to be your friend")
                                              .arg(QString::fromStdString(request.display_name)),
                                          {}, {}, OpenPakToast::Kind::Request);
                    }
                }
                self->online = std::move(now);
                self->requests = std::move(asking);
                self->first = false;
            },
            Qt::QueuedConnection);
    }).detach();
}

} // namespace openpak::qt
