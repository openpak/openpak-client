// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <string>
#include <vector>
#include <QDialog>
#include <QPixmap>
#include <QTimer>

#include "openpak/types.h"
#include "openpak/qt/host.h"

class QKeyEvent;
class QLabel;
class QListWidget;
class QProgressBar;
class QPushButton;
class QShowEvent;
class QStackedWidget;
class QToolButton;
class QWidget;

namespace openpak::qt {
class Page;
class WorkQueue;
} // namespace openpak::qt
namespace WebService::OpenPakApi {
struct AccountFriends;
}

// The OpenPak window (UX spec §3.6): the seven pages in a left list -- Account, Friends,
// Invitations, Cloud saves, Mods, News, Status -- each loaded when the window opens and again
// when it is navigated to, with every network request queued behind the one before it. The page
// header carries the title and Refresh, the footer the busy bar, one status line and Close.
class OpenPakAccountDialog : public QDialog {
    Q_OBJECT

public:
    static constexpr int kAccountPage = 0;
    static constexpr int kFriendsPage = 1; // requests in both directions live here too
    static constexpr int kInvitationsPage = 2;
    static constexpr int kCloudSavesPage = 3;
    static constexpr int kModsPage = 4;
    static constexpr int kNewsPage = 5;
    static constexpr int kStatusPage = 6;
    // Kept so a host that opens the window at one of these still builds. Home became the
    // Account page; Players and Recently Played were dropped (OpenPak has nothing behind their
    // calls), so they open where the closest content went.
    static constexpr int kHomePage = kAccountPage;
    static constexpr int kPlayersPage = kFriendsPage;
    static constexpr int kHistoryPage = kAccountPage;

    explicit OpenPakAccountDialog(openpak::qt::Host* controller,
                                   QWidget* parent = nullptr, int initial_page = kAccountPage);
    ~OpenPakAccountDialog() override;

    // The page the window last showed in this run, for the Open OpenPak hotkey (UX spec §3.1),
    // which opens the window where it was left.
    static int LastPage();

    void GoToPage(int index);

signals:
    // Routed up to the host's main window, which owns the chat window (Citron's experiment; the
    // Friends page offers it only when Host::ChatEnabled()).
    void InviteToChatRequested(u64 pid, QString name);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    QWidget* MakeAccountPage();
    void ReloadPage(int index, bool force);
    void ReloadAll();
    void RefreshAccount();
    void ShowAvatar();

    void UpdateBadge();
    void OnChangeAvatar();
    void OnEditUsername();
    void OnSignOut();

    void WireControllerNav();
    void OnDirectionalNav(int dx, int dy);
    void OnControllerActivate();
    bool ConfirmAction(const QString& title, const QString& message, const QString& yes_text,
                       const QString& no_text);

    openpak::qt::Host* controller;
    openpak::qt::Navigation* controller_navigation = nullptr;
    openpak::qt::WorkQueue* queue = nullptr;
    std::shared_ptr<WebService::OpenPakApi::AccountFriends> friends_snapshot;
    std::vector<openpak::qt::Page*> pages; // Friends onwards, in page order

    QListWidget* nav = nullptr;
    QStackedWidget* pages_stack = nullptr;
    QLabel* page_title = nullptr;
    QPushButton* refresh_button = nullptr;
    QLabel* status = nullptr;
    QProgressBar* busy = nullptr;
    int current_page = -1;

    // The Account page.
    QWidget* account_signed_in = nullptr;
    QWidget* account_signed_out = nullptr;
    QLabel* avatar = nullptr;
    QPixmap avatar_source;
    QLabel* name = nullptr;
    QLabel* code_pill = nullptr;
    QLabel* account_code = nullptr;
    QLabel* account_identity = nullptr;
    QLabel* account_consoles = nullptr;
    QLabel* account_link = nullptr;
    QPushButton* link_retry = nullptr;
    QWidget* first_focus = nullptr;

    QTimer refresh_timer;
};
