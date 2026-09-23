// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <map>
#include <optional>
#if __has_include(<span>)
#include <span>
#endif
#include <string>
#include <vector>
#include "openpak/types.h"

namespace httplib { class Client; }

namespace WebService::OpenPakApi {

// The base URL the account API talks to (openpak.org, or OPENPAK_API for a local stack) and
// the shared HTTPS client, for the library's other modules and for hosts that need a raw
// request beside the account API.
std::string BaseUrl();
httplib::Client& SharedClient();
void ApplySystemCa(httplib::Client& client);

struct LoginResult {
    bool ok = false;
    std::string error;       // Set when ok is false.
    u64 pid = 0;
    std::string username;
    std::string friend_code;
    std::string token;       // the openpak token the adapter minted; rides in the BAAS id_token
    std::string bearer;      // the website API token; only ever sent to the website
};

// Reason a NEX login was refused, as evaluated by the account server's gates.
struct OnlineStatus {
    bool queried = false;    // False when the request itself failed (offline, no token).
    bool allow = false;
    std::string reason;      // unknown | disabled | unverified | discord_unlinked | elsewhere
    std::string message;     // Human-readable text for the reason.
};

// The API base url. OPENPAK_API overrides it; only https or loopback is accepted, because this
// request carries the account token.
std::string BaseUrl();

// The shared HTTPS client's CA handling, for the library's other modules and for hosts that
// need a raw request beside the account API.
void ApplySystemCa(httplib::Client& client);

// Signs in with the OpenPak account's email and password: mints a website API token, then asks
// for the account's Switch identity. Also fetches the OpenPak CA into config/openpak/ca.pem so
// the redirected Nintendo names can be verified from the next launch on.
LoginResult SignIn(const std::string& email, const std::string& password);

// How this machine appears in the account's device list ("Eden on Pixel 8"): sent as device_name
// with every sign-in from now on. Empty sends none, and the website names the token itself.
void SetDeviceName(const std::string& device_name);

// Sign-in for families with no console identity of their own (Wii and DS: WFC
// minted no accounts). Mints the website bearer and nothing else — no /me/<family>
// fetch, no pid, no token. A host that signs in this way reads Common::OpenPakAccount::
// HasBearer() rather than IsLinked(), and cloud saves work from the bearer alone.
LoginResult SignInAccountOnly(const std::string& email, const std::string& password);

// Fetches the OpenPak CA over public TLS and stores it for the SSL service. Best effort.
bool FetchCA();

// POST /api/v1/crash-reports with a body openpak/crash_reports.h built; the HTTP status, or 0
// when the request never got an answer.
int PostCrashReport(const std::string& body, const std::string& content_type);


// Uses the stored account token. Answers only about the caller's own account.
OnlineStatus GetOnlineStatus();

struct HistoryEntry {
    std::string title_id; // 16 uppercase hex digits
    std::string name;     // may be empty; the server keeps the name it already has
    u64 seconds = 0;
    std::string last_played;  // RFC3339 UTC
    std::string icon_base64;  // may be empty
};

// Pushes play time for the account's history. The server merges, so sending only what changed is
// enough. Best-effort: a failure is logged and dropped.
void SyncHistory(const std::vector<HistoryEntry>& entries);

struct Friend {
    u64 pid = 0;
    std::string account_id;  // the OpenPak account behind the pid; what the website API keys on
    std::string name;        // console nickname if set, else account username
    std::string friend_code;
    s32 presence_status = 0; // 0 offline, 1 online, 2 in a game
    std::string app_field;   // opaque per-title presence blob
    std::string app_id;      // running title id, 16 hex digits; empty when nothing is running
    std::string app_name;    // display name the playing client already resolved; may be empty
    std::string image_base64; // profile picture, base64 JPEG; empty if none set
};

struct FriendList {
    bool ok = false;
    std::string error;
    std::vector<Friend> friends;
    std::vector<Friend> requests; // incoming, awaiting accept/decline
};

FriendList GetFriends();

// A friend or friend request as the account window shows it. The adapter's graph
// (/api/v1/me/switch) is preferred because it carries the pid, friend code and friends-since; the
// core's (/api/v1/me/friends, /api/v1/me/friends/requests) fills in when the adapter has nothing.
struct AccountFriend {
    std::string account_id; // empty when the pid has no OpenPak account
    u64 pid = 0;            // 0 when the row came from the core
    std::string display_name;
    std::string friend_code;
    bool online = false;
    std::string title_id;          // 16 upper-case hex digits, empty when nothing is running
    std::string console_namespace; // "switch", "wiiu", "3ds"...; empty when not known
    s64 online_since = 0;          // unix seconds, 0 when not known
    s64 friends_since = 0;         // unix seconds, 0 when not known
    bool incoming = false;         // for a request: true when they asked
};

struct AccountFriends {
    bool ok = false;
    std::string error;
    std::vector<AccountFriend> friends;  // online first, the server's order within each group
    std::vector<AccountFriend> requests; // incoming, then outgoing
};

AccountFriends GetAccountFriends();

// The window's friend actions, by account id as the website keys them. Each returns an empty
// string on success, else a message fit to show the user. A request with no account behind it
// (the adapter's alone) is answered through /api/v1/me/switch/friend by pid instead.
std::string SendFriendRequest(const std::string& friend_code);
std::string AcceptFriendRequest(const AccountFriend& request);
std::string DeclineFriendRequest(const AccountFriend& request); // also cancels an outgoing one
std::string RemoveAccountFriend(const std::string& account_id);
std::string BlockAccount(const std::string& account_id);

// Public pictures the site serves: an account's avatar (/media/avatars/{id}/256) and a title's
// icon (/titles/icon/{title id}). nullopt when there is none or the id is not one.
std::optional<std::vector<u8>> GetAccountAvatar(const std::string& account_id);
std::optional<std::vector<u8>> GetTitleIcon(const std::string& title_id_hex);

// All return an empty string on success, else a message fit to show the user.
std::string AddFriendByCode(const std::string& friend_code);
std::string AcceptFriend(u64 pid);
std::string DeclineFriend(u64 pid);
std::string RemoveFriend(u64 pid);

// Pushes the console nickname so friends and games see the account's name, not "Player".
void PushProfileName(const std::string& name);

// A real-time game-session invitation, e.g. nn::friends SendFriendInvitation/
// TryPopFromFriendInvitationStorageChannel. app_param is the opaque, game-defined bytes the
// sending title itself constructed (its own room code / join info) -- never interpreted here,
// only relayed byte-for-byte.
struct ReceivedInvitation {
    u64 from_pid = 0;
    std::string from_name;
    std::vector<u8> app_param;
};

// Sends target_pids (must already be mutual friends of the caller) an invitation carrying
// app_param verbatim. Returns an empty string on success, else a message fit to show the user.
#if defined(__cpp_lib_span)
std::string SendInvitation(const std::vector<u64>& target_pids, std::span<const u8> app_param);
#endif

// Pops (consumes) every invitation currently queued for this account. Empty on failure or if
// none are waiting -- there is no way to distinguish the two from this call alone, matching
// TryPopFromFriendInvitationStorageChannel's own "no data" vs "call failed" ambiguity on
// real hardware.
std::vector<ReceivedInvitation> PollInvitations();

// A player as seen in the current lobby or in the recent-encounters list. The server never
// includes an IP address in either. avatar_url identifies whether the player has a picture;
// fetch the bytes with GetAvatarByPid, which reconstructs the request path from pid rather than
// trusting this field as a request target.
struct LobbyPlayer {
    u64 pid = 0;
    std::string name;
    bool known = false; // false when pid isn't a known OpenPak account (can't add/report)
    std::string avatar_url;
    std::string friend_code;
    bool host = false;
    bool is_me = false;
    std::string title_id;
    std::string seen_at; // RFC3339 UTC; empty when not applicable (current lobby members)
};

struct Lobby {
    bool in_lobby = false;
    std::string title_id;
    std::string type;
    std::string state;      // raw label from the game server; not for display
    std::string state_code; // "searching" | "matched" | "" if the game server didn't classify
    u64 id = 0;
    int count = 0;
    int max = 0;
    std::vector<LobbyPlayer> players;
};

// The lobby the account is in right now, and who else is in it.
Lobby GetMyLobby();

// The last players encountered online, most recent first.
std::vector<LobbyPlayer> GetRecentPlayers();

// Downloads a player's avatar (base64 JPEG). Public, unauthenticated endpoint; the account token
// is never attached. Empty on failure or if the player has no avatar set.
std::string GetAvatarByPid(u64 pid);

// Reports a player. The server refuses if the account never actually shared a lobby with pid.
// Returns an empty string on success, else an error code ("not_encountered", "quota", or a
// message fit to show the user).
std::string ReportPlayer(u64 pid, const std::string& reason, const std::string& comment);

// Publishes this player's presence so friends see them online and, for titles that use it,
// joinable. app_id is the running title id; empty when nothing is running. app_name is the
// display name already resolved locally (e.g. from NACP) so a friend can show it even without
// that title in their own library; empty when nothing is running or the name couldn't be resolved.
void PushPresence(s32 status, const std::string& app_field, const std::string& app_id,
                  const std::string& app_name);

// Downloads the BCAT schedule seed (a zip of vsdata/coopdata/fesdata) for titles that need one
// locally to go online (Splatoon 2). title_id_hex is 16 uppercase hex digits. Empty on failure.
//
// Always fetches the full zip rather than relying on a conditional GET (If-Modified-Since):
// this endpoint's Last-Modified handling isn't reliable enough to trust as the sole freshness
// signal (an earlier Ryujinx fork hits the same server and has the same finding), so the caller should
// hash the returned bytes with HashBcatSeedHex and compare against what's locally installed.
std::vector<u8> DownloadBcatSeed(const std::string& title_id_hex);

// Lowercase hex SHA-256 of the given bytes, for comparing a freshly downloaded BCAT seed zip
// against the hash of what's currently installed locally.
std::string HashBcatSeedHex(const std::vector<u8>& data);

// Public, unauthenticated. Maps lowercase-hex title id -> currently connected player count.
std::map<std::string, int> GetOnlineCounts();

// Public, unauthenticated. Total players in Nazi Zombies Portable's own legacy-nzp server.
int GetNzpOnlineCount();

// Downloads this title's cloud save. title_id_hex is 16 hex digits. nullopt when there is no
// cloud save stored yet, the account can't use cloud saves (guest), or the request failed.
std::optional<std::vector<u8>> PullSave(const std::string& title_id_hex);

// Uploads this title's save. The server rejects a drastically smaller upload against an existing
// larger save rather than clobbering it (kept=true in that case, still reported as success here).
// Returns an error message fit to show the user, or empty on success. The pointer overload is
// the portable one: hosts built as C++17 (melonDS) have no std::span to pass.
std::string PushSave(const std::string& title_id_hex, const std::uint8_t* data, std::size_t size);
#if defined(__cpp_lib_span)
std::string PushSave(const std::string& title_id_hex, std::span<const u8> data);
#endif

// Which platform segment the saves API paths use (default "citron"; the Wii and DS emulators
// pass "wii" and "ds" — emulators/prds/emulator-integration-prd.md S-1) and what name uploads report as
// the machine that made them (default: the platform).
void SetSavesPlatform(const std::string& platform);
std::string SavesPlatform();
// The cloud version the last pull or push of this title saw ("" when none), and a way to seed it
// from what the host kept on disk, so the next push names the version it builds on.
std::string SaveVersion(const std::string& title_id_hex);
void SetSaveVersion(const std::string& title_id_hex, const std::string& version);
void SetSaveDevice(const std::string& device);

struct Profile {
    bool ok = false;
    std::string error;
    std::string name;         // acct.Username, the authoritative account nickname
    std::string image_base64; // own profile picture, base64 JPEG; empty if none set

    // The rest of the server's Profile blob. The server replaces this blob wholesale on every
    // push (no per-field merge), so any push that changes one of these must resend the others
    // to avoid silently wiping them.
    std::string console_nickname; // profile.name, the console nickname; may be empty
    std::string mii_base64;       // profile.mii; may be empty
    std::string avatar_id;        // profile.avatar, a gallery icon id; may be empty
    std::string color_hex;        // profile.color; may be empty

    std::string account_id;
    std::string friend_code;                   // the website's; the Switch identity's wins
    std::vector<std::string> linked_platforms; // namespaces of the consoles linked to it
};

// Uses the stored account token. Own profile only.
Profile GetProfile();

// Uploads a new profile picture (base64 JPEG), preserving the rest of the profile blob (the
// server replaces it wholesale, so this fetches the current profile first and resends it with
// just the image changed). Returns an error message, or empty on success.
std::string PushProfilePicture(const std::string& image_base64);

// Renames the account (3-16 chars: letters, digits, '_' or '-'). Returns an error message, or
// empty on success.
std::string SetUsername(const std::string& username);

struct HistoryItem {
    std::string title_id;     // 16 hex digits
    std::string name;
    std::string icon_base64;  // may be empty
    u64 seconds = 0;
    std::string last_played;  // RFC3339 UTC, may be empty if never recorded
};

struct HistoryList {
    bool ok = false;
    std::string error;
    std::vector<HistoryItem> entries;
};

HistoryList GetHistory();

// An invitation the core arranged (GET /api/v1/me/invitations). Distinct from the native inbox in
// openpak::client::session, which holds the ones a console sent and the core never sees.
struct CoreInvitation {
    std::string id;
    std::string from;
    std::string title_id;
    std::string expires_at; // RFC3339 UTC, may be empty
};

std::vector<CoreInvitation> GetInvitations();

// Returns an empty string on success, else a message fit to show the user.
std::string DeclineInvitation(const std::string& invitation_id);

// One published mod for a title.
struct Mod {
    std::string id;
    std::string slug;
    std::string name;
    std::string version;
    std::string author;
    std::string licence;
    std::string summary;
    std::string sha256;
    std::string package_url;
};

// Public. title_id_hex is 16 hex digits, either case.
std::vector<Mod> GetMods(const std::string& title_id_hex);

// The signed-in account's favourite mod ids. Empty when signed out.
std::vector<std::string> GetFavouriteModIds();
bool SetModFavourite(const std::string& mod_id, bool favourite);

// Signing out revokes the website token on the server (DELETE /api/v1/token) rather than only
// forgetting it here: a copy of it anywhere else stops working too. Best effort, blocking; call
// it off the UI thread.
void RevokeToken(const std::string& bearer);

// The catalogue's playability per title (GET /api/v1/titles, public): title id hex -> status.
// Empty when the site could not be asked. Blocking.
std::map<std::string, std::string> CatalogueStatuses();

// The mod's zip, verified against the sha256 the catalogue published. nullopt on a failed
// download or a hash mismatch -- the bytes came off the network and are refused either way.
std::optional<std::vector<u8>> DownloadModPackage(const Mod& mod);

// One file in a title's current news (BCAT) dataset.
struct NewsFile {
    std::string path;
    u64 size = 0;
    std::string url;
};

struct NewsManifest {
    bool ok = false;
    std::string valid_from; // RFC3339 UTC
    std::vector<NewsFile> files;
};

// Public. What a title would receive over BCAT right now.
NewsManifest GetNewsManifest(const std::string& title_id_hex);
std::optional<std::vector<u8>> DownloadNewsFile(const NewsFile& file);

// The public status page's numbers. ok is false when the site could not be reached.
struct NetworkStatus {
    bool ok = false;
    int players_online = 0;
    std::vector<std::pair<std::string, int>> titles;   // title id -> players
    std::vector<std::pair<std::string, int>> networks; // namespace -> players
};

NetworkStatus GetStatus();

// One version of a cloud save, as GET /api/v1/me/saves lists it.
struct SaveVersionInfo {
    s64 id = 0;         // what the versions/{id} routes take
    int number = 0;     // the per-title counter X-Save-Version and the version marker carry
    bool conflict = false;
    u64 size = 0;
    std::string device; // the machine that uploaded it; may be empty
    std::string saved_at; // RFC3339 UTC
};

// One title's cloud saves. name is the site's, and the site falls back to the title id when it
// does not know the game, so callers resolve a display name themselves.
struct CloudSave {
    std::string platform;
    std::string title_id;
    std::string name;
    std::string icon_path; // a path on the site; empty when it has no icon
    std::vector<SaveVersionInfo> versions; // newest first
};

struct CloudSaves {
    bool ok = false;
    std::string error;
    std::vector<CloudSave> saves;
    u64 allowance_used = 0;
    u64 allowance = 0; // 0 when the site did not say
};

// GET /api/v1/me/saves: every title with a cloud save, and how much of the allowance is used.
CloudSaves GetCloudSaves();

// DELETE /api/v1/me/saves/versions/{id}. Removes the cloud copy only. Empty on success.
std::string DeleteSaveVersion(s64 version_id);

// The bytes of an image the site serves by path (a save's icon); nullopt when there are none.
std::optional<std::vector<u8>> FetchSitePath(const std::string& path);

// The status page's verdict and per-service checks (GET {status host}/api/status). The status
// host is OPENPAK_STATUS when set, else "status." in front of the site's own host.
struct ServiceCheck {
    std::string group;
    std::string name;
    bool up = false;
    double uptime = 0; // percent over the page's window
    std::string latency; // as the status page words it, e.g. "142 ms"
};

struct ServiceStatus {
    bool ok = false;
    std::string url;      // where it asked, for the "could not reach" line
    std::string headline; // "All systems operational" and the like
    std::string sub;
    std::string state;    // ok | some | major
    std::vector<ServiceCheck> services;
};

std::string StatusUrl();
ServiceStatus GetServiceStatus();

// The site's catalogue (GET /api/v1/titles, public): title id hex -> name and status. Empty when
// the site could not be asked. Blocking.
struct CatalogueTitle {
    std::string name;
    std::string console;
    std::string status;
};
std::map<std::string, CatalogueTitle> Catalogue();

// Round-trip time to the backend's /api/health in milliseconds, or nullopt on failure.
std::optional<int> PingBackend();

} // namespace WebService::OpenPakApi
