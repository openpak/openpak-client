// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#include <filesystem>
#include <functional>
#include <span>
#include <string>

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
// still_wanted, when given, is asked once more right before anything is written: a host that
// stopped waiting (its ceiling passed, or Skip) answers false and the local save is left alone.
PullOutcome PullBeforeLaunch(const std::filesystem::path& save_dir, u64 title_id,
                             const std::function<bool()>& still_wanted = {});

// Local I/O only -- call before the filesystem is torn down. Empty when there is nothing to send.
std::vector<u8> CaptureOnExit(const std::filesystem::path& save_dir, u64 title_id);

// The network half of CaptureOnExit, from a detached thread. Empty on success, else why.
std::string PushCaptured(const std::filesystem::path& save_dir, u64 title_id,
                         std::vector<u8> zip_bytes);

// How the local copy stands against the newest cloud version, as the Cloud saves page words it.
// The marker is what tells these apart: without one, a local save and a cloud save have no
// shared history and neither can be called newer.
enum class LocalState {
    NoLocal,      // nothing here yet; Download brings the cloud copy
    InStep,       // the local copy is the newest cloud version, untouched since
    ChangedHere,  // in step once, played since: the next exit uploads it
    CloudNewer,   // another machine uploaded after this copy was last in step
    NoHistory,    // both exist and never met: Take cloud or Keep local decides
};

struct LocalCopy {
    LocalState state = LocalState::NoLocal;
    std::string version; // the cloud version the marker says it was last in step with, or empty
    s64 last_written = 0; // unix seconds of the newest file in it, 0 when there is none
};

// Local I/O only. newest_cloud is the newest version's number, 0 when the cloud has none.
LocalCopy Compare(const std::filesystem::path& save_dir, int newest_cloud);

// The manual actions of the Cloud saves page, as the reference has them. Blocking, network; empty
// on success, else why. Download takes the newest cloud copy even over a local one (kept beside
// the folder as <folder>.openpak-backup, as PullBeforeLaunch keeps it): that is "take the cloud".
// Upload sends the local copy as the version after newest_cloud (0 when the cloud has none), so
// the server stores it as the next version rather than as a conflict: that is "keep local".
std::string Download(const std::filesystem::path& save_dir, u64 title_id);
std::string Upload(const std::filesystem::path& save_dir, u64 title_id, int newest_cloud);

} // namespace OpenPak::SaveSync
