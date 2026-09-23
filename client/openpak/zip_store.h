// SPDX-License-Identifier: GPL-2.0-or-later
// Minimal zip archiver for cloud saves: writes the store method, reads store
// and (with zlib) deflate, no other dependencies. A save directory is zipped as it is and unzipped as
// it was; the server treats the bytes as opaque, so nothing else needs to
// agree with it. Readers skip entries they do not understand rather than
// failing the whole archive.
#pragma once
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace openpak::ZipStore {

// Zips every regular file under dir (recursively) into a zip, with paths
// relative to dir and forward slashes. Empty when dir does not exist.
std::vector<std::uint8_t> ZipDirectory(const std::filesystem::path& dir);

// Extracts a zip into dest (created as needed), overwriting files it names.
// Stored entries always, deflated ones when built with zlib. False when the
// bytes are not a zip, or any entry is one it cannot read or would land
// outside dest: a save missing a file is a broken save, so all or nothing.
bool UnzipToDirectory(std::span<const std::uint8_t> zip, const std::filesystem::path& dest);

} // namespace openpak::ZipStore
