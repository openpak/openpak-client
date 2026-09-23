// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later

// The online chain a Switch walks at boot, walked here instead.
//
// The emulator HLEs the account sysmodule, so if this does not do it, nothing does: a game
// asking acc:u0 for an id_token gets whatever the emulator hands it, and Nintendo's own
// servers are gone. Five requests, all to OpenPak, under Nintendo's own hostnames because
// OpenPak answers to them and routes by SNI:
//
//   POST dauth /v8/challenge            a challenge to mix into the device MAC
//   POST dauth /v8/device_auth_tokens   a device token
//   POST aauth /v5/application_auth_token  a per-title token, when a title is bound below
//   POST baas  /1.0.0/application/token an application token, the Bearer for the rest
//   POST baas  /1.0.0/users             a device account -- once, then kept on disk
//   POST baas  /1.0.0/login             the id_token the game is actually asking for
//
// The device account identifies this install, not a person. Its id_token carries an OpenPak
// identity only after the account has been linked, which is the federation call below.
//
// This lives in the shared client rather than in one emulator because every OpenPak build
// needs the same five requests to put a real identity in front of a title server.

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace openpak::client::session {

/// One invitation waiting in the native inbox.
struct Invitation {
    std::string id;          ///< Decimal, as the friends module parses it (%llu).
    std::string sender_id;   ///< The sender's BAAS user id.
    std::string sender_name; ///< Their nickname, or the id when it cannot be looked up.
    std::string title_id;    ///< 16 hex digits: the title to launch.
    /// The title's own join payload, decoded. Opaque here and relayed to the guest byte for byte:
    /// it is what turns "somebody invited you" into a session the game can actually join.
    std::vector<std::uint8_t> app_param;
    std::int64_t created_at = 0;
    std::int64_t expires_at = 0;
    /// What the sender's game wrote, by language tag ("en-US", "ja"...). A slot the game left
    /// empty is not a message, so it is not kept.
    std::map<std::string, std::string> messages;
};

/// Where OpenPak answers, and the CA that proves it. Set once by the host before anything
/// else; changing the server starts a different device account, as it must.
void Configure(std::string server_host, int port, std::string ca_path);

/// Whether a server has been configured and its CA is readable.
bool Enabled();

/// The OpenPak CA in DER form, fetched and cached if it is not on disk yet, or empty.
///
/// A title that verifies the server itself -- Stardew reads the console's own certificate store
/// and checks the chain -- is handed Nintendo's real CAs by an emulator that emulates that store
/// properly, and no OpenPak certificate can ever satisfy those. This is what goes in their place.
std::vector<std::uint8_t> CaCertificateDer();

/// Make sure a usable id_token is cached, running the chain if not. Never throws: a build that
/// cannot reach OpenPak must behave like a console that cannot reach Nintendo.
bool Ensure();

/// The title that is about to ask for a token, as 16 hex digits and its own version string.
///
/// A console's baas login carries an application_auth_token naming the running title, and the
/// title's own online stack checks that binding before it will use the id_token: one minted for
/// another game -- or for no game at all -- connects fine and then never speaks, which is an
/// emulator that looks online and hangs. Empty (the game list, a sign-in before anything is
/// loaded) leaves the chain unbound, the shape it always had. The cached token is keyed by the
/// binding, so switching titles re-runs the chain on the next Ensure.
void SetApplication(std::string application_id, std::string application_version);

/// The id_token OpenPak issued, or empty when there is none to give.
std::string IdToken();

/// The BAAS user id as the u64 a guest calls a NetworkServiceAccountId, or 0.
std::uint64_t NetworkServiceAccountId();

/// The BAAS user id as its 16 hex digits, or empty.
std::string UserId();

/// The OpenPak account this install is linked to, or empty while it is anonymous.
std::string Nickname();

/// The account's friend code as other players type it, or empty.
std::string FriendCode();

/// Signed in is not linked: an unlinked device account gets a perfectly valid token that no
/// title server can attach to a person.
bool Linked();

/// Bind this device account to the OpenPak account that id_token belongs to (federation).
/// Returns an empty string on success, or a message worth showing somebody.
std::string Link(const std::string& account_id_token);

/// Link by signing in, which is the console's own flow with the keyboard a phone already has:
/// the server hands back an authorization code, the code buys an id_token, and that id_token
/// binds this device account to the person. Empty on success, else a message to show them.
std::string LinkWithPassword(const std::string& email, const std::string& password);

/// Keep saying we are here, which is the only way anyone can tell that we are, and keep the
/// friends module's caches warm on the same beat (the reference's cadence: every third beat, the
/// invitation inbox and baas::BackgroundSync; every beat, presence when it changed, and a forced
/// republish every third). A sign-in is followed at once by a forced publish and a
/// BackgroundSync, as a console syncs when its account becomes network-ready.
///
/// `current_title_id` answers with the 16 hex digits of whatever is running, or an empty string
/// on the game list. It is only a stand-in for a host that never calls SetRunningApplication:
/// the title it names is published as its own presence group.
///
/// This takes PresenceState::Instance()'s changed callback, so that what the running title
/// declares through friend:u is published the moment it changes.
void StartHeartbeat(std::function<std::string()> current_title_id);

/// The application that runs now and its NACP presence group id (0 when the NACP declares none,
/// and the title id stands in), or (0, 0) when nothing runs. The host calls this when a title
/// starts and when it stops; the change is published off the caller's thread. Presence then says
/// INACTIVE with no title, ONLINE in one, and PLAYING only while the title has declared an
/// online-play session (friend:u 10600/10601/10610), with the appField it set.
void SetRunningApplication(std::uint64_t application_id, std::uint64_t presence_group_id);

/// Whether the heartbeat is running and has a signed-in user to beat for: what the status page
/// means by "something is asking for notifications".
bool Beating();

/// The JSON-Patch body presence would publish right now for the active profile (PresencePatch of
/// PresenceState::Current).
std::string PresenceBody();

/// Say we are going, so the account drops offline now rather than when the lease runs out.
void GoOffline();

/// A profile was deleted: its device account is forgotten on every server it had one on.
void ForgetProfile(const std::string& profile);

/// Fill the friend cache the guest's friend:u reads, and publish the presence the running title
/// set. The Qt host does this on its own timers; a build without one -- Android -- has nothing
/// else that would, and a title whose friend list is empty cannot join anybody.
void RefreshGuestFriends();

/// What the native inbox held at the last poll, dismissals removed. Never stale-blocking: the
/// heartbeat refreshes it.
std::vector<Invitation> Invitations();

/// Ask the inbox now. False when there is nothing signed in to ask with.
bool RefreshInvitations();

/// Take one off the list and tell the server it has been read. There is no native decline:
/// read state and expiry are all the surface has.
void DismissInvitation(const std::string& invitation_id);

/// Called once for each invitation that was not there at the previous poll.
void SetInvitationHook(std::function<void(const Invitation&)> on_arrival);

/// Called whenever the number of invitations waiting in the native inbox may have changed: one
/// arrived, one expired, or one was dismissed. The guest's friends module answers a title's
/// badge (22010) with the count, and tells a system screen through its notification queue.
void SetInvitationsChangedHook(std::function<void()> on_change);

/// What one request as the signed-in user produced. Status 0 is a request that could not be made
/// at all: nothing signed in, or no answer from the server.
struct Response {
    int status = 0;
    std::string body;
};

/// One request to a Nintendo hostname as the signed-in BAAS user: the pinned client and the
/// user-scoped bearer from the login, with the sign-in refreshed first when it has aged out. This
/// is how the friends module's own REST calls (openpak/baas.h) leave. `path_and_query` is sent as
/// given; an empty `content_type` sends no body, and any other goes out with the body as UTF-8,
/// "; charset=utf-8" appended the way the reference's StringContent writes it. Never throws.
Response SendAs(const std::string& method, const std::string& host,
                const std::string& path_and_query, const std::string& content_type,
                const std::string& body);

} // namespace openpak::client::session
