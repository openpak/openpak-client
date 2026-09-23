// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#include <filesystem>
#include <span>

#include <vector>

#include "openpak/types.h"

// Cloud save sync against the OpenPak account server (mirrors OpenPakSaveSync.cs). Requires a
// linked account and a title in OpenPak::CompatibleTitles. Best-effort: failures are logged.
namespace OpenPak::SaveSync {

// Blocking. force skips the no-overwrite check (manual "Download from Cloud").
void Pull(const std::filesystem::path& save_dir, u64 title_id, bool force = false);

// Local I/O only -- safe to call before filesystem teardown. Empty if ineligible.
std::vector<u8> CaptureForPush(const std::filesystem::path& save_dir, u64 title_id);

// The network step for a zip from CaptureForPush -- call from a detached thread.
void UploadCaptured(u64 title_id, std::vector<u8> zip_bytes);

// Cloud saves without anyone pressing anything, as Ryujinx does them: the newest cloud copy
// comes down before a title starts, and the local copy goes up when it exits. Every title, not
// only the compatibility table. A marker beside the save folder records which cloud version the
// local copy was last in step with, which tells the three cases apart: nothing changed in the
// cloud (start on what is here), the cloud moved on from another machine (take it, keep the old
// local copy as <folder>.openpak-backup), and both sides have a save with no shared history
// (touch nothing and say so; the Cloud saves page is where that choice is made).
enum class PullOutcome { Nothing, Pulled, BothExist };

// Blocking, network; call before the title boots. The host shows Pulled and BothExist.
PullOutcome PullBeforeLaunch(const std::filesystem::path& save_dir, u64 title_id);

// Local I/O only -- call before the filesystem is torn down. Empty when there is nothing to send.
std::vector<u8> CaptureOnExit(const std::filesystem::path& save_dir, u64 title_id);

// The network half of CaptureOnExit, from a detached thread. Empty on success, else why.
std::string PushCaptured(const std::filesystem::path& save_dir, u64 title_id,
                         std::vector<u8> zip_bytes);

} // namespace OpenPak::SaveSync
