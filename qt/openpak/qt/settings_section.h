// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later

// The OpenPak section of a host's settings dialog (UX spec §3.13), the same in every Qt host: the
// enable switch, the account row, Open OpenPak..., the account at startup (Switch), cloud sync,
// the notifications and their corner, the DNS redirect where a host has one, and a collapsed
// Advanced part (the website, the host's console server fields, Refresh network settings, crash
// reports). The host puts it on its own page and calls Apply() when its dialog applies; signing
// in and out act at once.

#pragma once

#include <functional>
#include <utility>
#include <vector>

#include <QString>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;

namespace openpak::qt {

class Host;

struct SettingsHooks {
    // "Connect this emulator to OpenPak" (Wii and DS: "Connect Nintendo WFC to OpenPak").
    std::function<bool()> enabled;
    std::function<void(bool)> set_enabled;
    std::function<bool()> game_running;
    // The account row: the profile it belongs to (empty on a host with one identity) and who is
    // signed in (empty when nobody is).
    std::function<QString()> profile;
    std::function<QString()> signed_in_as;
    std::function<void()> sign_in;  // the host's sign-in (or setup) flow
    std::function<void()> sign_out; // after the confirmation, which the section shows
    std::function<void()> open_window;
    // Account at startup, Switch hosts only: "" is Last used, "ask" Ask every time, anything else
    // a profile's key, listed by profiles() as (key, name).
    std::function<std::vector<std::pair<QString, QString>>()> profiles;
    std::function<QString()> startup;
    std::function<void(const QString&)> set_startup;
    // "Point the emulated console's DNS at OpenPak", only where a host has that switch.
    std::function<bool()> redirect_dns;
    std::function<void(bool)> set_redirect_dns;
    // The host's console server fields (Citron's Server IP and NAT IP), shown under Advanced.
    QWidget* server_fields = nullptr;
};

class SettingsSection : public QWidget {
public:
    // Cloud sync and the notifications are read from and written to the host (Host::
    // CloudSyncEnabled, NotificationsEnabled, NotificationCorner).
    SettingsSection(Host* host, SettingsHooks hooks, QWidget* parent = nullptr);

    // Writes what the controls hold.
    void Apply();

private:
    void RefreshAccount();

    Host* host;
    SettingsHooks hooks;
    QCheckBox* enable = nullptr;
    QLabel* account_row = nullptr;
    QPushButton* account_button = nullptr;
    QComboBox* startup = nullptr;
    QCheckBox* cloud_sync = nullptr;
    QCheckBox* notifications = nullptr;
    QComboBox* corner = nullptr;
    QCheckBox* redirect_dns = nullptr;
    QComboBox* crash_reports = nullptr;
};

} // namespace openpak::qt
