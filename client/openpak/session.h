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

/// Keep saying we are here, which is the only way anyone can tell that we are, and poll the
/// invitation inbox on the same beat. `current_title_id` answers with the 16 hex digits of
/// whatever is running, or an empty string on the game list.
void StartHeartbeat(std::function<std::string()> current_title_id);

/// Say we are going, so the account drops offline now rather than when the lease runs out.
void GoOffline();

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

} // namespace openpak::client::session
