// SPDX-License-Identifier: GPL-2.0-or-later
// What the client needs from the host that is not the network: where to keep files, and a few
// string helpers the moved code used from the emulator's common library.
#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>
namespace openpak::Platform {
// Set once by the host before any other call. Config holds the account file and the CA; cache
// holds avatars and other regenerable data.
void SetDirectories(std::filesystem::path config_dir, std::filesystem::path cache_dir);
std::filesystem::path ConfigDir();
std::filesystem::path CacheDir();
// Which local profile the OpenPak identity belongs to. Each profile is its own account -- its own
// website sign-in and its own device account -- and one is active at a time, like a console with
// one user signed in. The host answers with a stable id for the active profile (a Switch user's
// UUID), asked each time it matters, so a switch is noticed wherever it happens. No source, or an
// empty answer, is the single shared identity every host had before profiles.
void SetProfileSource(std::function<std::string()> source);
std::string ProfileId();
// Which emulator this is and its build ("eden", "0.1.0-abc1234"), set once by the host at startup.
// Every request the library makes to OpenPak says so in X-OpenPak-Client: <name>/<version>, so the
// servers can tell the emulators apart. Not set, no header.
void SetClient(std::string name, std::string version);
// "<name>/<version>", or empty before SetClient.
std::string ClientHeader();
bool CreateParentDirs(const std::filesystem::path& path);
bool WriteFile(const std::filesystem::path& path, const std::string& contents);
std::string ReadFile(const std::filesystem::path& path);
// Where a path that came off the network lands under root, or empty when it would land anywhere
// else (absolute, or climbing out with "..").
std::filesystem::path ContainedPath(const std::filesystem::path& root, const std::string& relative);
void SplitString(const std::string& str, char delim, std::vector<std::string>& out);
std::string ToLower(std::string s);
std::string ToUpper(std::string s);
std::string StripSpaces(const std::string& s);
} // namespace openpak::Platform
