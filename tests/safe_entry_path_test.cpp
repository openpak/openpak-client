// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The one check behind the save-restore path guard: a save archive comes off the network, and
// an entry name that climbs out of the save directory is the whole attack.

#include <cassert>
#include <cstdio>

#include "openpak/qt/safe_entry_path.h"

using OpenPak::SaveSync::SafeEntryPath;

int main() {
    const std::filesystem::path root = "/home/player/saves/0100abcd";

    for (const char* refused : {
             "../../.ssh/authorized_keys",
             "..",
             "a/../../b",
             "/etc/passwd",
             "/",
             "",
#ifdef _WIN32
             "C:\\Windows\\System32\\evil.dll",
             "..\\..\\evil",
#endif
         }) {
        if (SafeEntryPath(root, refused).has_value()) {
            std::printf("entry %s was accepted and must not be\n", refused);
            return 1;
        }
    }

    for (const char* allowed : {"save.bin", "data/slot0.bin", "./data/slot1.bin", "a/b/c/d.bin"}) {
        const auto got = SafeEntryPath(root, allowed);
        if (!got) {
            std::printf("entry %s was refused and should not be\n", allowed);
            return 1;
        }
        if (got->string().rfind(root.string() + "/", 0) != 0) {
            std::printf("entry %s landed outside: %s\n", allowed, got->string().c_str());
            return 1;
        }
    }
    return 0;
}
