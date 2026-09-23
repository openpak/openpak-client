// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later

// The friends applet (MyPage, AppletId 0x1A), as far as a game can drive it. Ported from Ryujinx
// (HOS/Applets/MyPage/MyPageApplet.cs); the host keeps only the applet shell that pops the
// argument and pushes the answer.
//
// The SDK pushes CommonArguments, then one 0x10A8 argument: u32 type at +0, the caller's Uid at
// +8, a type-specific payload from +0x18. For types 2, 8 and 9 it pops a u32 from the out channel
// and returns it as the call's Result; the other types read nothing back. Only the invitation
// types do anything here. The rest are the console's friend screens, which live in the OpenPak
// window instead, and complete at once.

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace openpak::my_page {

/// Up to `max` friends picked by the person, as BAAS user ids; empty when they cancelled or the
/// host has no picker. Called on a worker thread; the host marshals to its UI thread.
using FriendPicker = std::function<std::vector<std::uint64_t>(int max)>;

void SetFriendPicker(FriendPicker picker);

/// Runs one MyPage call. Blocking (a picker, then one POST); never on a guest thread. The value
/// is the u32 the game pops, or nothing for the types that read nothing back.
std::optional<std::uint32_t> Run(std::span<const std::uint8_t> argument,
                                 std::uint64_t application_id, std::uint64_t presence_group_id);

/// The Result a cancelled picker or a failed send gives. The one MyPage gives was never
/// recovered; this only has to be non-zero and not 2121-0030, which the SDK reserves for an
/// applet that did not run.
constexpr std::uint32_t ResultCancelled = 121 | (1 << 9);

} // namespace openpak::my_page
