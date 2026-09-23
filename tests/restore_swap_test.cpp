// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The one check behind the restore swap: a restore that fails must leave the player's old save
// exactly where it was. Losing progress to a half-applied download is the failure that matters.

#include <cassert>
#include <cstdio>
#include <fstream>

#include "openpak/qt/restore_swap.h"

using OpenPak::SaveSync::StagingPathFor;
using OpenPak::SaveSync::SwapIntoPlace;

namespace {

void Write(const std::filesystem::path& file, const std::string& text) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file) << text;
}

std::string Read(const std::filesystem::path& file) {
    std::ifstream in(file);
    std::string text;
    std::getline(in, text);
    return text;
}

} // namespace

int main() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "openpak_restore_swap_test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    const std::filesystem::path dest = root / "0100abcd";

    // A restore that arrives whole replaces the old save, and leaves no scratch behind.
    Write(dest / "slot0.bin", "old");
    Write(StagingPathFor(dest) / "slot0.bin", "new");
    if (!SwapIntoPlace(StagingPathFor(dest), dest)) {
        std::printf("a complete restore was refused\n");
        return 1;
    }
    if (Read(dest / "slot0.bin") != "new") {
        std::printf("the restore did not land: %s\n", Read(dest / "slot0.bin").c_str());
        return 1;
    }
    if (std::filesystem::exists(StagingPathFor(dest))) {
        std::printf("staging survived a successful restore\n");
        return 1;
    }

    // A restore with nothing staged (extraction refused the archive) must not be a deletion.
    if (SwapIntoPlace(StagingPathFor(dest), dest)) {
        std::printf("a restore with nothing staged reported success\n");
        return 1;
    }
    if (Read(dest / "slot0.bin") != "new") {
        std::printf("a failed restore lost the save: %s\n", Read(dest / "slot0.bin").c_str());
        return 1;
    }

    std::filesystem::remove_all(root, ec);
    return 0;
}
