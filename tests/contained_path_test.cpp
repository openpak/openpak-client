// SPDX-License-Identifier: GPL-2.0-or-later
// A mod zip entry or a news file path comes off the network; it must never leave its directory.
#include <cassert>
#include "openpak/platform.h"

int main() {
    using openpak::Platform::ContainedPath;
    const std::filesystem::path root = "/mods/x";
    assert(ContainedPath(root, "romfs/a.bin") == root / "romfs/a.bin");
    assert(ContainedPath(root, "./romfs/../exefs/main") == root / "exefs/main");
    assert(ContainedPath(root, "../evil").empty());
    assert(ContainedPath(root, "romfs/../../evil").empty());
    assert(ContainedPath(root, "/etc/passwd").empty());
    assert(ContainedPath(root, "").empty());
    assert(ContainedPath(root, ".").empty());
    return 0;
}
