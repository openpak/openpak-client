// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace OpenPak::SaveSync {

// The entry name in an archive off the network is attacker-controlled input. It has to be
// relative, must not climb, and the path it lands on has to still be inside the directory
// being extracted into once the path is normalised.
inline std::optional<std::filesystem::path> SafeEntryPath(const std::filesystem::path& root,
                                                           const std::string& name) {
    if (name.empty()) {
        return std::nullopt;
    }
    const std::filesystem::path rel(name);
    if (rel.is_absolute() || rel.has_root_directory() || !rel.root_name().empty()) {
        return std::nullopt;
    }
    for (const auto& part : rel) {
        if (part == "..") {
            return std::nullopt;
        }
    }
    const std::filesystem::path full = (root / rel).lexically_normal();
    const std::string base = (root / "").lexically_normal().string();
    if (full.string().rfind(base, 0) != 0) {
        return std::nullopt;
    }
    return full;
}

} // namespace OpenPak::SaveSync
