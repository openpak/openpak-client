// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later

// The friend toasts of a host that does not run the Switch friends service (UX spec §3.10): the
// website's friend list, asked for every 30 seconds while signed in, and a toast when a friend
// comes online, starts a game, or asks to be a friend. The first answer after start or sign-in is
// silent. Clicking one of these toasts is the host's to wire (OpenPakToast::clicked).

#pragma once

#include <map>
#include <set>
#include <string>

#include <QObject>
#include <QTimer>

class OpenPakToast;

namespace openpak::qt {

class FriendNotifier : public QObject {
public:
    FriendNotifier(OpenPakToast* toast, QObject* parent);

    // Call after signing in or out: the next answer is taken as the starting point again.
    void Reset();

private:
    void Poll();

    OpenPakToast* toast;
    QTimer timer;
    bool polling = false;
    bool first = true;
    std::map<std::string, std::string> online; // account id -> title id ("" when not playing)
    std::set<std::string> requests;
};

} // namespace openpak::qt
