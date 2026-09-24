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
#include <vector>
#include "openpak/save_sync.h"
#include "openpak/types.h"
#include "openpak/qt/controller_navigation.h"
class OpenPakChatClient;
namespace openpak::qt {
// Which console an emulator plays, for what the window's pages can offer it (UX spec §3.6): Wii
// and DS games speak Nintendo WFC, which has no console account, no invitations and no news.
enum class Family { Switch, WiiU, N3DS, Wii, DS };

inline bool IsWfc(Family family) {
    return family == Family::Wii || family == Family::DS;
}

// The notice's words (strings/en.json): a host that applies the redirects when a game starts
// says "Restart the game", one that applies them once per process says "Restart the emulator".
enum class RedirectsApplied { AtGameStart, AtEmulatorStart };
QString RedirectsChangedText(RedirectsApplied when);

class Host : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    static Host* Current();
    static void SetCurrent(Host* host);

    // Defaulted so the Switch hosts that predate families still build.
    virtual Family GetFamily() const { return Family::Switch; }
    // The local profile the OpenPak account belongs to, for the sign-out confirmation; empty on a
    // host with one identity.
    virtual QString ProfileName() const { return {}; }
    // The Account page's console rows on a Wii U or 3DS host (UX spec §3.6), which the host's
    // own identity call knows: "PNID {0}", or the 3DS friend code and principal, and the console
    // link. Empty shows "—". The Switch hosts' rows come from the session, the WFC hosts have none.
    virtual QString ConsoleIdentity() const { return {}; }
    virtual QString ConsoleLink() const { return {}; }

    // account and friends (the host owns the session and the friend cache)
    virtual bool IsLinked() const = 0;
    virtual void SignIn() = 0;
    virtual void SignOut() = 0;
    virtual void RefreshFriendCache() = 0;
    virtual void NotifyFriendRequestSent(const QString& friend_code) = 0;
    virtual QString JoinFriendSession(u64 pid) = 0;
    virtual void EnsureChatConnected() = 0;
    virtual OpenPakChatClient* GetChatClient() = 0;
    // A host's own chat experiment (Citron's), outside the OpenPak spec: the Friends page offers
    // ChatInviteText() in a friend's menu only while this is true.
    virtual bool ChatEnabled() const { return false; }
    virtual QString ChatInviteText() const { return {}; }

    // titles
    virtual QString ResolveGameName(const std::string& app_id_hex, const std::string& hint_name = {}) const = 0;
    virtual QString ResolveGameIcon(const std::string& app_id_hex) const = 0;
    virtual std::string GetLocalAppId() const = 0;
    virtual void QuickStart(u64 title_id) = 0;
    virtual void ManualSaveDownload(u64 title_id) = 0;
    virtual std::filesystem::path SaveDirectory(u64 title_id) = 0;
    // Defaulted so a host that predates the mods and news pages still builds: no library means
    // the pages offer the running title only, and no mod directory means Install is refused.
    struct Title {
        u64 id = 0;
        QString name;
    };
    virtual std::vector<Title> InstalledTitles() const { return {}; }
    virtual std::filesystem::path ModDirectory(u64 /*title_id*/) { return {}; }
    // True when the host applies one mod per title straight from ModDirectory (Azahar's
    // LayeredFS: load/mods/<title>/romfs), rather than one folder per mod inside it.
    virtual bool OneModPerTitle() const { return false; }

    // Cloud saves. The defaults are SaveSync's: a folder per title (SaveDirectory), keyed in the
    // cloud by the 16-digit title id. A host whose save is something else -- melonDS keeps one
    // .sav per ROM, keyed by the cartridge's game code -- answers these instead. SaveDirectory is
    // still asked on the UI thread and its answer handed back here; the other three run on the
    // window's work thread, so they must not touch the host's widgets.
    virtual std::string CloudSaveKey(u64 title_id) const;
    virtual u64 CloudSaveTitle(const std::string& key) const;
    virtual OpenPak::SaveSync::LocalCopy CompareSave(const std::filesystem::path& save_dir,
                                                      u64 title_id, int newest_cloud);
    virtual std::string DownloadSave(const std::filesystem::path& save_dir, u64 title_id);
    virtual std::string UploadSave(const std::filesystem::path& save_dir, u64 title_id,
                                   int newest_cloud);

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

    // OpenPak changed the network redirects for this host's console while it runs
    // (docs/signed-ceiling.md, rule 4): the new ones apply from the next start. Called on the UI
    // thread, once per change, never for another console's change. The host shows a non-modal
    // notice -- an OPENPAK toast with RedirectsChangedText() -- and never interrupts the game.
    virtual void NetworkRedirectsChanged() {}

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
    void SignInFinished();
    void ChatInviteReceived(QString room_id, QString room_name, u64 from_pid, QString from_name);
    void ChatInviteSent(u64 target_pid);
    void ChatMemberJoined(QString room_id, u64 pid, QString name);
    void ChatBanned(QString reason);
    void ChatRawMessage(QJsonObject obj);
};
} // namespace openpak::qt
