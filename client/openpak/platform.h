// SPDX-License-Identifier: GPL-2.0-or-later
// What the client needs from the host that is not the network: where to keep files, and a few
// string helpers the moved code used from the emulator's common library.
#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
namespace openpak::Platform {
// Set once by the host before any other call. Config holds the account file and the CA; cache
// holds avatars and other regenerable data.
void SetDirectories(std::filesystem::path config_dir, std::filesystem::path cache_dir);
std::filesystem::path ConfigDir();
std::filesystem::path CacheDir();
bool CreateParentDirs(const std::filesystem::path& path);
bool WriteFile(const std::filesystem::path& path, const std::string& contents);
std::string ReadFile(const std::filesystem::path& path);
void SplitString(const std::string& str, char delim, std::vector<std::string>& out);
std::string ToLower(std::string s);
std::string ToUpper(std::string s);
std::string StripSpaces(const std::string& s);
} // namespace openpak::Platform
