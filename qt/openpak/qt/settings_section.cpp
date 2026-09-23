// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later

#include "openpak/qt/settings_section.h"

#include <algorithm>
#include <memory>
#include <thread>

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

#include "openpak/api.h"
#include "openpak/crash_reports.h"
#include "openpak/network_profile.h"
#include "openpak/qt/host.h"
#include "openpak/qt/pages_p.h"
#include "openpak/qt/prompts.h"
#include "openpak/qt/toast.h"

namespace openpak::qt {

namespace {

// The corner choices in the spec's order, and the OpenPakToast::Corner each one is.
constexpr std::pair<const char*, OpenPakToast::Corner> kCorners[] = {
    {"Bottom right", OpenPakToast::Corner::BottomRight},
    {"Bottom left", OpenPakToast::Corner::BottomLeft},
    {"Top right", OpenPakToast::Corner::TopRight},
    {"Top left", OpenPakToast::Corner::TopLeft},
};

const char* Platform(Family family) {
    switch (family) {
    case Family::WiiU:
        return "wiiu";
    case Family::N3DS:
        return "3ds";
    case Family::Wii:
        return "wii";
    case Family::DS:
        return "ds";
    case Family::Switch:
        break;
    }
    return "switch";
}

QString SourceWord(NetworkProfile::Source source) {
    switch (source) {
    case NetworkProfile::Source::Fetched:
        return Tr("fetched");
    case NetworkProfile::Source::Cached:
        return Tr("cached");
    case NetworkProfile::Source::BuiltIn:
        break;
    }
    return Tr("built-in");
}

QLabel* Heading(const QString& text) {
    QLabel* label = BoldLabel(text, 1);
    label->setContentsMargins(0, 8, 0, 2);
    return label;
}

} // namespace

SettingsSection::SettingsSection(Host* host_, SettingsHooks hooks_, QWidget* parent)
    : QWidget(parent), host(host_), hooks(std::move(hooks_)) {
    setAccessibleName(Tr("OpenPak"));
    const Family family = host ? host->GetFamily() : Family::Switch;
    const bool running = hooks.game_running && hooks.game_running();
    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(8);

    // 1-4. Account.
    layout->addWidget(Heading(Tr("Account")));
    enable = new QCheckBox(IsWfc(family) ? Tr("Connect Nintendo WFC to OpenPak")
                                         : Tr("Connect this emulator to OpenPak"));
    enable->setToolTip(Tr("When this is off the emulator behaves exactly as upstream does: "
                          "offline, and nothing is sent anywhere."));
    enable->setChecked(!hooks.enabled || hooks.enabled());
    enable->setEnabled(!running && hooks.set_enabled != nullptr);
    if (running) {
        enable->setToolTip(Tr("Stop the running game first."));
    }
    layout->addWidget(enable);

    auto* account = new QHBoxLayout;
    account_row = new QLabel;
    account_row->setWordWrap(true);
    account_button = new QPushButton;
    connect(account_button, &QPushButton::clicked, this, [this] {
        const QString who = hooks.signed_in_as ? hooks.signed_in_as() : QString{};
        if (who.isEmpty()) {
            if (hooks.sign_in) {
                hooks.sign_in();
            }
        } else if (ConfirmSignOut(window(), hooks.profile ? hooks.profile() : QString{}) &&
                   hooks.sign_out) {
            hooks.sign_out();
        }
        RefreshAccount();
    });
    account->addWidget(account_row, 1);
    account->addWidget(account_button);
    layout->addLayout(account);

    auto* open = new QPushButton(Tr("Open OpenPak..."));
    connect(open, &QPushButton::clicked, this, [this] {
        if (hooks.open_window) {
            hooks.open_window();
        }
    });
    layout->addWidget(open, 0, Qt::AlignLeft);

    // 5. Account at startup (Switch).
    if (family == Family::Switch && hooks.set_startup) {
        auto* row = new QFormLayout;
        startup = new QComboBox;
        startup->addItem(Tr("Last used"), QString{});
        startup->addItem(Tr("Ask every time"), QStringLiteral("ask"));
        if (hooks.profiles) {
            for (const auto& [key, name] : hooks.profiles()) {
                startup->addItem(name, key);
            }
        }
        const int chosen = startup->findData(hooks.startup ? hooks.startup() : QString{});
        startup->setCurrentIndex(std::max(chosen, 0));
        startup->setToolTip(Tr("Which profile opens when the emulator starts, if there is more than "
                               "one. A game started from a shortcut or launcher always uses the "
                               "last one."));
        row->addRow(Tr("Account at startup"), startup);
        layout->addLayout(row);
    }

    // 6. Cloud sync.
    cloud_sync = new QCheckBox(Tr("Sync cloud saves automatically when a game starts and stops"));
    cloud_sync->setChecked(!host || host->CloudSyncEnabled());
    layout->addWidget(cloud_sync);

    // 7-9. Notifications.
    layout->addWidget(Heading(Tr("Notifications")));
    notifications = new QCheckBox(Tr("Show notifications"));
    notifications->setChecked(!host || host->NotificationsEnabled());
    layout->addWidget(notifications);
    auto* corner_row = new QFormLayout;
    corner = new QComboBox;
    const int current_corner = host ? host->NotificationCorner()
                                    : static_cast<int>(OpenPakToast::Corner::BottomRight);
    for (const auto& [label, value] : kCorners) {
        corner->addItem(Tr(label), static_cast<int>(value));
        if (static_cast<int>(value) == current_corner) {
            corner->setCurrentIndex(corner->count() - 1);
        }
    }
    corner_row->addRow(Tr("Notification corner"), corner);
    layout->addLayout(corner_row);

    // 10. The DNS redirect, where a host has one.
    if (hooks.set_redirect_dns) {
        redirect_dns = new QCheckBox(Tr("Point the emulated console's DNS at OpenPak"));
        redirect_dns->setChecked(hooks.redirect_dns && hooks.redirect_dns());
        layout->addWidget(redirect_dns);
    }

    // 11. Advanced, collapsed.
    auto* advanced_toggle = new QToolButton;
    advanced_toggle->setText(Tr("Advanced"));
    advanced_toggle->setCheckable(true);
    advanced_toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    advanced_toggle->setArrowType(Qt::RightArrow);
    advanced_toggle->setAutoRaise(true);
    layout->addWidget(advanced_toggle, 0, Qt::AlignLeft);
    auto* advanced = new QWidget;
    auto* advanced_layout = new QFormLayout(advanced);
    auto* website = new QLineEdit(QString::fromStdString(WebService::OpenPakApi::BaseUrl()));
    // The website is chosen at start (OPENPAK_API); it is shown here, not changed.
    website->setReadOnly(true);
    website->setToolTip(Tr("Where the account lives and where you sign in. Leave this at "
                           "openpak.org unless you run your own deployment."));
    advanced_layout->addRow(Tr("Website"), website);
    if (hooks.server_fields) {
        advanced_layout->addRow(hooks.server_fields);
    }
    auto* refresh = new QPushButton(Tr("Refresh network settings"));
    auto* network_status = DimLabel({});
    connect(refresh, &QPushButton::clicked, this, [refresh, network_status, family] {
        // Off the UI thread (UX spec §5.2): the fetch may take its two seconds.
        refresh->setEnabled(false);
        QPointer<QPushButton> button{refresh};
        QPointer<QLabel> line{network_status};
        std::thread([button, line, platform = std::string{Platform(family)}] {
            const auto result = NetworkProfile::Refresh(platform);
            NetworkProfile::Apply(result);
            QMetaObject::invokeMethod(
                QCoreApplication::instance(),
                [button, line, source = result.source] {
                    if (button) {
                        button->setEnabled(true);
                    }
                    if (line) {
                        line->setText(Tr("Network settings: %1.").arg(SourceWord(source)));
                    }
                },
                Qt::QueuedConnection);
        }).detach();
    });
    advanced_layout->addRow(refresh, network_status);
    // What the next launch does with a crash report (openpak/crash_reports.h).
    crash_reports = new QComboBox;
    crash_reports->addItem(Tr("Ask"), QString::fromUtf8(CrashReports::kAsk.data(), static_cast<qsizetype>(CrashReports::kAsk.size())));
    crash_reports->addItem(Tr("Always send"), QString::fromUtf8(CrashReports::kAlways.data(), static_cast<qsizetype>(CrashReports::kAlways.size())));
    crash_reports->addItem(Tr("Never send"), QString::fromUtf8(CrashReports::kNever.data(), static_cast<qsizetype>(CrashReports::kNever.size())));
    crash_reports->setCurrentIndex(
        std::max(crash_reports->findData(QString::fromStdString(CrashReports::Policy())), 0));
    advanced_layout->addRow(Tr("Crash reports"), crash_reports);
    advanced->hide();
    connect(advanced_toggle, &QToolButton::toggled, this, [advanced, advanced_toggle](bool open_now) {
        advanced->setVisible(open_now);
        advanced_toggle->setArrowType(open_now ? Qt::DownArrow : Qt::RightArrow);
    });
    layout->addWidget(advanced);
    layout->addStretch(1);

    if (host) {
        connect(host, &Host::AccountLinked, this, [this] { RefreshAccount(); });
        connect(host, &Host::AccountUnlinked, this, [this] { RefreshAccount(); });
    }
    RefreshAccount();
}

void SettingsSection::RefreshAccount() {
    const QString profile = hooks.profile ? hooks.profile() : QString{};
    const QString who = hooks.signed_in_as ? hooks.signed_in_as() : QString{};
    if (profile.isEmpty()) {
        account_row->setText(who.isEmpty() ? Tr("Not signed in") : Tr("Signed in as %1").arg(who));
    } else {
        account_row->setText(who.isEmpty() ? Tr("%1 — Not signed in").arg(profile)
                                           : Tr("%1 — Signed in as %2").arg(profile, who));
    }
    account_button->setText(who.isEmpty() ? Tr("Sign in...") : Tr("Sign out..."));
    const bool running = hooks.game_running && hooks.game_running();
    account_button->setEnabled(!running);
    account_button->setToolTip(running ? Tr("Stop the running game first.") : QString{});
}

void SettingsSection::Apply() {
    if (hooks.set_enabled && enable->isEnabled()) {
        hooks.set_enabled(enable->isChecked());
    }
    if (startup && hooks.set_startup) {
        hooks.set_startup(startup->currentData().toString());
    }
    if (host) {
        host->SetCloudSyncEnabled(cloud_sync->isChecked());
        host->SetNotificationsEnabled(notifications->isChecked());
        host->SetNotificationCorner(corner->currentData().toInt());
    }
    if (redirect_dns && hooks.set_redirect_dns) {
        hooks.set_redirect_dns(redirect_dns->isChecked());
    }
    CrashReports::SetPolicy(crash_reports->currentData().toString().toStdString());
}

} // namespace openpak::qt
