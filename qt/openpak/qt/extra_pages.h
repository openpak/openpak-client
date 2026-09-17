// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later

// The account dialog's pages that are lists of rows with buttons rather than delegate-drawn
// views: invitations, mods, news, status. Each refreshes itself when it is shown and reports
// through the dialog's one status line.

#pragma once

#include <functional>
#include <QString>

class QWidget;

namespace openpak::qt {

class Host;

using StatusSink = std::function<void(const QString&)>;

QWidget* MakeInvitationsPage(Host* host, StatusSink status);
QWidget* MakeModsPage(Host* host, StatusSink status);
QWidget* MakeNewsPage(Host* host, StatusSink status);
QWidget* MakeStatusPage(Host* host, StatusSink status);

} // namespace openpak::qt
