// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <string>
#include "openpak/types.h"

namespace Common::OpenPakAccount {

// The signed-in OpenPak account. Written by the sign-in flow, read by the acc service so the
// NEX login presents the account's persistent principal id. Stored as key=value, not JSON, so it
// stays readable if a field is added later.
//
// Two tokens live here. The bearer is the website's API token (openpak.org /api/v1) and never
// leaves for anywhere but the website. The token is the openpak claim the adapter minted for this
// account: it rides inside the locally signed BAAS id_token so a title server can ask the
// adapter who is playing.

bool IsLinked();
u64 GetPid();
std::string GetUsername();
std::string GetFriendCode();
std::string GetToken();
std::string GetBearer();

// Bumped by every Save()/Clear(). Lets callers that cache derived data (e.g. the acc
// service's signed id_token, which embeds GetToken() in an "openpak" claim) detect a link
// state change and invalidate their cache instead of relying on a time-based expiry that
// can keep serving a token minted before the account was linked.
u64 GetGeneration();

// Bearer-only accounts (Wii and DS emulators): a signed-in OpenPak account with no console
// identity, because WFC minted none. Everything that speaks to the website — cloud saves
// above all — works from the bearer alone; nothing else may assume a pid.
bool HasBearer();
void SaveBearerOnly(std::string_view username, std::string_view bearer);

void Save(u64 pid, std::string_view username, std::string_view friend_code,
          std::string_view token, std::string_view bearer);
void Clear();

// Everything above is the active profile's account (openpak::Platform::ProfileId). These look at
// the others, which is how one account is kept to one profile and how a profile list can say who
// each profile is signed in as.

// The profile, other than except_profile, whose account is this pid; empty when there is none.
std::string HolderOf(u64 pid, const std::string& except_profile);

// The OpenPak name the profile is signed in as, or empty when it is offline.
std::string UsernameOf(const std::string& profile);

// The website token the profile is signed in with, so a deleted profile's can be revoked.
std::string BearerOf(const std::string& profile);

// A profile was deleted: its account goes with it.
void Forget(const std::string& profile);

} // namespace Common::OpenPakAccount
