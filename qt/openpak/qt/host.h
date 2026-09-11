// SPDX-License-Identifier: GPL-2.0-or-later
// The one interface an emulator implements to get the OpenPak dialogs. Everything the dialogs
// need from the host (game names and icons, launching, settings, the save directory of a title,
// gamepad navigation) comes through here; everything they need from the network comes from the
// client library.
#pragma once
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <filesystem>
#include <string>
#include "openpak/types.h"
#include "openpak/qt/controller_navigation.h"
class NextendoChatClient;
namespace openpak::qt {
class Host : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    static Host* Current();
    static void SetCurrent(Host* host);

    // account and friends (the host owns the session and the friend cache)
    virtual bool IsLinked() const = 0;
    virtual void SignIn() = 0;
    virtual void SignOut() = 0;
    virtual void RefreshFriendCache() = 0;
    virtual void NotifyFriendRequestSent(const QString& friend_code) = 0;
    virtual QString JoinFriendSession(u64 pid) = 0;
    virtual void EnsureChatConnected() = 0;
    virtual NextendoChatClient* GetChatClient() = 0;

    // titles
    virtual QString ResolveGameName(const std::string& app_id_hex, const std::string& hint_name = {}) const = 0;
    virtual QString ResolveGameIcon(const std::string& app_id_hex) const = 0;
    virtual std::string GetLocalAppId() const = 0;
    virtual void QuickStart(u64 title_id) = 0;
    virtual void ManualSaveDownload(u64 title_id) = 0;
    virtual std::filesystem::path SaveDirectory(u64 title_id) = 0;

    // settings the dialogs read and write
    virtual QString AccentColor() const = 0;
    virtual bool IsDarkTheme() const = 0;
    virtual bool NotificationsEnabled() const = 0;
    virtual void SetNotificationsEnabled(bool enabled) = 0;
    virtual int NotificationCorner() const = 0;
    virtual void SetNotificationCorner(int corner) = 0;
    virtual bool RedirectEnabled() const = 0;
    virtual bool CloudSyncEnabled() const = 0;
    virtual void SetCloudSyncEnabled(bool enabled) = 0;
    virtual std::string ServerIp() const = 0;
    virtual std::string NatIp() const = 0;

    // input: suspend the game's input while a dialog is up; a Navigation for gamepad control
    virtual void SetGuestInputSuspended(bool suspended) = 0;
    virtual Navigation* CreateNavigation(QObject* parent) = 0;

signals:
    void AccountLinked();
    void AccountUnlinked();
    void FriendCameOnline(u64 pid, QString name, QString game_name, QString avatar_base64);
    void FriendWentOffline(u64 pid, QString name, QString avatar_base64);
    void FriendRequestReceived(u64 pid, QString name, QString avatar_base64);
    void FriendRequestSent(QString friend_code);
    void FriendInvitationReceived(u64 from_pid, QString from_name);
    void StatusChanged(QString message);
    void QuickStartRequested(u64 title_id);
    void SignInUrlReady(QString url);
    void SignInFinished();
    void ChatInviteReceived(QString room_id, QString room_name, u64 from_pid, QString from_name);
    void ChatInviteSent(u64 target_pid);
    void ChatMemberJoined(QString room_id, u64 pid, QString name);
    void ChatBanned(QString reason);
    void ChatRawMessage(QJsonObject obj);
};
} // namespace openpak::qt
