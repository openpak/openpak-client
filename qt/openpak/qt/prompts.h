// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later

// The shared prompts every Qt host shows the same way (UX spec §3.2, §3.5, §3.9): the connect
// prompt of the hosts without profiles, the sign-out confirmation and the cloud-save conflict.
// Each is modal and returns what the person chose; the host does the work.

#pragma once

#include <QString>

#include "openpak/qt/host.h"
#include "openpak/types.h"

class QWidget;

namespace openpak::qt {

// "Sign out of OpenPak?" with Sign out and Cancel, Cancel the default and Escape. profile names
// the profile that goes offline; empty for a host with one identity (signout.body_single).
bool ConfirmSignOut(QWidget* parent, const QString& profile = {});

// "Connect to OpenPak?" on a first interactive launch of a host without profiles: the family's
// body, a Create an account link, Sign in (default) and Not now. True means Sign in.
bool AskToConnect(QWidget* parent, Family family);

// "Choose a save" for a title whose local and cloud saves never met.
enum class SaveChoice { Later, KeepLocal, TakeCloud };
struct ConflictSides {
    QString game;
    s64 local_written = 0;   // unix seconds, 0 when unknown
    int cloud_version = 0;
    QString cloud_device;
    s64 cloud_saved = 0;     // unix seconds, 0 when unknown
    bool game_running = false; // Take the cloud's is disabled while that game runs
};
SaveChoice ChooseSave(QWidget* parent, const ConflictSides& sides);

} // namespace openpak::qt
