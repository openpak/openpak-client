// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#include <filesystem>
#include <span>

#include <vector>

#include "openpak/types.h"

// Cloud save sync against the Nextendo account server (mirrors NextendoSaveSync.cs). Requires a
// linked account and a title in Nextendo::CompatibleTitles. Best-effort: failures are logged.
namespace Nextendo::SaveSync {

// Blocking. force skips the no-overwrite check (manual "Download from Cloud").
void Pull(const std::filesystem::path& save_dir, u64 title_id, bool force = false);

// Local I/O only -- safe to call before filesystem teardown. Empty if ineligible.
std::vector<u8> CaptureForPush(const std::filesystem::path& save_dir, u64 title_id);

// The network step for a zip from CaptureForPush -- call from a detached thread.
void UploadCaptured(u64 title_id, std::vector<u8> zip_bytes);

} // namespace Nextendo::SaveSync
