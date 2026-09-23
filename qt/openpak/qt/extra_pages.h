// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later

// The account dialog's pages after the Account page, in the reference's order: Friends,
// Invitations, Cloud saves, Mods, News, Status. Each is handed the window's context (the one work
// queue, the status line, the confirmation card) and reloads itself when the dialog asks: every
// page when the window opens, and the page that is shown each time it is navigated to.

#pragma once

namespace openpak::qt {

class Page;
struct PageContext;

Page* MakeFriendsPage(const PageContext& context);
Page* MakeInvitationsPage(const PageContext& context);
Page* MakeSavesPage(const PageContext& context);
Page* MakeModsPage(const PageContext& context);
Page* MakeNewsPage(const PageContext& context);
Page* MakeStatusPage(const PageContext& context);

} // namespace openpak::qt
