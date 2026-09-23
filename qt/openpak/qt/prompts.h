// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later

// The shared prompts every Qt host shows the same way (UX spec §3.2, §3.4, §3.5, §3.8, §3.9):
// the first-run setup of the Switch hosts and the connect prompt of the others, the profile
// picker, the sign-out confirmation, the invitation prompt and the cloud-save conflict. Each is
// modal, works with a controller through the host's Navigation (the game's input suspended while
// it is up), and returns what the person chose; the host does the work.

#pragma once

#include <map>
#include <string>
#include <vector>

#include <QPixmap>
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

// "Set up this profile" (or, adding one, "Add an account") on a Switch host: Sign in with OpenPak
// (default), Play offline, and in Add mode Cancel; Create an account is a link, which opens the
// website's registration and answers Create -- the host then shows the sign-in dialog with
// setup.verify. Escape is Play offline on first run and Cancel in Add mode. Without a password
// store, Sign in with OpenPak is disabled and the body says why.
enum class SetupChoice { SignIn, Create, Offline, Cancel };
SetupChoice AskSetup(QWidget* parent, bool add_mode, bool keychain = true);

// "Profile name" after Play offline: the current name, all selected; empty when cancelled or
// left empty, which the host takes as "back to the setup".
QString AskProfileName(QWidget* parent, const QString& current);

// "Who is playing?" (UX spec §3.4): a row per profile with its picture, its name and the badge
// line (Offline, or the linked account's name). Continue (also a double click), Add account,
// Cancel; Remember my choice.
struct ProfileRow {
    QString key;     // what the host knows the profile by
    QString name;
    QString account; // the linked OpenPak account's name; empty is Offline
    QPixmap picture;
};
struct ProfileChoice {
    enum class Action { Continue, Add, Cancel } action = Action::Cancel;
    QString key;
    bool remember = false;
};
ProfileChoice PickProfile(QWidget* parent, const std::vector<ProfileRow>& rows,
                          const QString& current_key);

// "Invitation" (UX spec §3.8): the sender and the game, when it was sent, what they wrote. True
// means Join (the default); false Ignore.
struct InvitationCard {
    QString sender;
    QPixmap sender_picture;
    QString game;
    QPixmap game_icon;
    s64 sent = 0; // unix seconds, 0 when unknown
    QString message;
};
bool OfferInvitation(QWidget* parent, const InvitationCard& card);

// What the sender's game wrote, in the language the UI speaks: the exact tag, then the language
// alone, then English, then whatever the game wrote. Empty for no message.
QString InvitationMessage(const std::map<std::string, std::string>& messages);

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
