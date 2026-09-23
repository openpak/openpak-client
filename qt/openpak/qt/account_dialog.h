// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <QDialog>
#include <QPixmap>
#include <QTimer>

#include "openpak/types.h"
#include "openpak/qt/host.h"

class QAbstractButton;
class QButtonGroup;
class QHBoxLayout;
class QKeyEvent;
class QLabel;
class QLineEdit;
class QListView;
class QPushButton;
class QResizeEvent;
class QShowEvent;
class QStackedWidget;
class QStandardItemModel;
class QModelIndex;
class QToolButton;
class QVBoxLayout;
namespace openpak::qt { class Host; }
class OpenPakFriendDelegate;
class ExternalReferenceProbe;
class OpenPakPageNavArrow;

namespace Core::HID {
}

namespace openpak::qt {
class Page;
class WorkQueue;
struct PageContext;
} // namespace openpak::qt
namespace WebService::OpenPakApi {
struct AccountFriends;
}
class QProgressBar;

// The OpenPak window: the reference's seven pages in the reference's order -- Account, Friends,
// Invitations, Cloud saves, Mods, News, Status -- each loaded when the window opens and again
// when it is navigated to, with every network request queued behind the one before it.
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

signals:
    // Routed up to GMainWindow, which owns the one persistent chat window --
    // opens/reuses it, creates a room if needed, and sends the invite.
    void InviteToChatRequested(u64 pid, QString name);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    QWidget* MakeAccountPage();
    void ReloadPage(int index, bool force);
    void ReloadAll();
    void RefreshAccount();
    void OnSignInOrOut();

    void UpdateDashboard();
    void OnChangeAvatar();
    void OnEditUsername();
    void OnChangeBackground();
    void OnRemoveBackground();
    void ApplyBackground(const QPixmap& pixmap);
    void LoadSavedBackground();
    void UpdateHeroSizing();

    void GoToPage(int index);
    void UpdatePageNav();
    void WireControllerNav();

    void OnDirectionalNav(int dx, int dy);
    void OnControllerActivate();
    bool ConfirmAction(const QString& title, const QString& message, const QString& yes_text,
                       const QString& no_text, const std::string& icon_base64 = {});

    openpak::qt::Host* controller;
    int initial_page = kAccountPage;
    openpak::qt::Navigation* controller_navigation = nullptr;
    openpak::qt::WorkQueue* queue = nullptr;
    std::shared_ptr<WebService::OpenPakApi::AccountFriends> friends_snapshot;
    std::vector<openpak::qt::Page*> pages; // Friends onwards, in page order

    QLabel* header_avatar;
    QPixmap header_avatar_source; // undecorated (square) -- re-rounded at whatever size fits
    QLabel* header_name;
    QToolButton* edit_name_button;
    QLabel* header_code;
    QLabel* status;
    QProgressBar* busy = nullptr;

    QWidget* header_card = nullptr;

    QWidget* account_details = nullptr;
    QLabel* account_signed_out = nullptr;
    QLabel* account_code = nullptr;
    QLabel* account_pid = nullptr;
    QLabel* account_consoles = nullptr;
    QLabel* account_players = nullptr;
    QLabel* account_server = nullptr;
    QLabel* account_link = nullptr;
    QPushButton* sign_button = nullptr;

    QTimer refresh_timer;

    ExternalReferenceProbe* network_probe;
    QLabel* nat_label;
    QLabel* ping_label;
    QTimer* status_check_timer = nullptr;
    int status_check_dots = 0;
    bool nat_checking = false;
    bool ping_checking = false;

    QStackedWidget* pages_stack = nullptr;
    QStringList page_titles;
    int current_page = -1;
    QWidget* home_default_focus = nullptr;
    QWidget* active_popup = nullptr;
    QLabel* page_title_label = nullptr;
    OpenPakPageNavArrow* nav_left_arrow = nullptr;
    OpenPakPageNavArrow* nav_right_arrow = nullptr;

    QLabel* requests_badge;

    QWidget* dash_online_dot = nullptr;
    QLabel* dash_online_text = nullptr;
    QLabel* dash_friends_online_value = nullptr;
    QLabel* dash_in_game_value = nullptr;
};
