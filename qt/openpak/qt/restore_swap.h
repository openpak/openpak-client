// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <system_error>

namespace OpenPak::SaveSync {

// A restore replaces a player's progress. Extract beside the save directory and swap it in
// once, so a failure halfway through leaves the old save whole instead of half of each.
inline std::filesystem::path StagingPathFor(const std::filesystem::path& dest) {
    return dest.parent_path() / (dest.filename().string() + ".openpak-new");
}

inline std::filesystem::path PreviousPathFor(const std::filesystem::path& dest) {
    return dest.parent_path() / (dest.filename().string() + ".openpak-old");
}

// Moves `staged` onto `dest`, keeping the old save until the move has succeeded. False means
// the restore did not happen and the old save is still where it was.
inline bool SwapIntoPlace(const std::filesystem::path& staged,
                          const std::filesystem::path& dest) {
    std::error_code ec;
    const std::filesystem::path previous = PreviousPathFor(dest);
    std::filesystem::remove_all(previous, ec);
    if (std::filesystem::exists(dest)) {
        std::filesystem::rename(dest, previous, ec);
        if (ec) {
            std::filesystem::remove_all(staged, ec);
            return false;
        }
    }
    std::filesystem::rename(staged, dest, ec);
    if (ec) {
        // A failed restore must not also be a deletion: put the old save back. If even that
        // fails the save is still whole under .openpak-old -- never deleted to hide the mess.
        std::error_code back;
        if (std::filesystem::exists(previous)) {
            std::filesystem::rename(previous, dest, back);
        }
        std::filesystem::remove_all(staged, ec);
        return false;
    }
    std::filesystem::remove_all(previous, ec);
    return true;
}

} // namespace OpenPak::SaveSync
