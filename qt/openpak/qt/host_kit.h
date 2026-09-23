// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later

// What every Qt host builds the same way around the OpenPak window (UX spec §3.1, §3.3): the
// top-level OpenPak menu, and -- for the hosts whose consoles have no account of their own (Wii,
// DS) -- signing in and out with the website alone.

#pragma once

#include <functional>

#include <QPixmap>
#include <QString>

class QMenu;
class QMenuBar;
class QWidget;

namespace openpak::qt {

// What the menu asks the host each time it opens, and what its items do.
struct MenuHooks {
    std::function<bool()> game_running;   // sign in and sign out wait for the game to stop
    std::function<bool()> openpak_on;     // off: the header opens settings; 2-4 and 10 disabled
    std::function<QString()> signed_in_as; // empty when signed out
    std::function<QPixmap()> avatar;       // 20 px round, or null
    std::function<void()> sign_in;         // the sign-in dialog (or setup, on a Switch host)
    std::function<void()> sign_out;        // after the confirmation, which the menu shows
    std::function<void(int page)> open_window; // OpenPakAccountDialog::k*Page
    std::function<void()> open_settings;   // the host's settings, at its OpenPak section
    QString profile;                       // for the sign-out confirmation; empty for one identity
    std::function<QString()> profile_name; // the same, asked when the menu signs out (profiles change)
};

// The OpenPak menu, rebuilt each time it opens, placed immediately left of Help (the menu bar's
// last menu).
QMenu* AddOpenPakMenu(QMenuBar* menu_bar, MenuHooks hooks);

// The same items in a menu the host already placed (a .ui file's, or a top bar button's).
void PopulateOpenPakMenu(QMenu* menu, MenuHooks hooks);

// Bearer-only accounts (Wii, DS): mints the website token, stores it under the account's name,
// and says which machine uploads the saves. Blocking, network: an OpenPakSignInDialog submitter.
// Empty on success, else the text to show.
QString SignInAccountOnly(const QString& email, const QString& password, const QString& device);

// Forgets the stored account now and revokes its token on the server off the UI thread.
void SignOutAndRevoke();

// "{emulator} on {machine}" (signin.device_default_single), and with a profile
// "{emulator} — {profile} on {machine}" (signin.device_default).
QString DefaultDeviceName(const QString& emulator);
QString DefaultDeviceName(const QString& emulator, const QString& profile);

} // namespace openpak::qt
