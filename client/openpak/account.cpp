// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <mutex>
#include <vector>

#include <fmt/format.h>

#include "openpak/platform.h"
#include "openpak/platform.h"
#include "openpak/platform.h"
#include "openpak/account.h"
#include "openpak/platform.h"

namespace Common::OpenPakAccount {

namespace {

std::mutex g_mutex;
bool g_loaded = false;

u64 g_pid = 0;
std::string g_username;
std::string g_friend_code;
std::string g_token;
std::string g_bearer;
u64 g_generation = 0;

std::filesystem::path FilePath() {
    return openpak::Platform::ConfigDir() / "openpak_account.txt";
}

// Caller holds g_mutex.
void EnsureLoaded() {
    if (g_loaded) {
        return;
    }
    g_loaded = true;

    const std::string contents = openpak::Platform::ReadFile(FilePath());
    std::vector<std::string> lines;
    openpak::Platform::SplitString(contents, '\n', lines);

    for (const auto& line : lines) {
        const auto eq = line.find('=');
        if (eq == std::string::npos || eq == 0) {
            continue;
        }
        const std::string key = openpak::Platform::StripSpaces(line.substr(0, eq));
        const std::string value = openpak::Platform::StripSpaces(line.substr(eq + 1));

        if (key == "pid") {
            try {
                g_pid = std::stoull(value);
            } catch (...) {
                g_pid = 0;
            }
        } else if (key == "username") {
            g_username = value;
        } else if (key == "friend_code") {
            g_friend_code = value;
        } else if (key == "token") {
            g_token = value;
        } else if (key == "bearer") {
            g_bearer = value;
        }
    }
}

// Caller holds g_mutex.
void WriteFile() {
    void(openpak::Platform::CreateParentDirs(FilePath()));
    const std::string contents =
        fmt::format("pid={}\nusername={}\nfriend_code={}\ntoken={}\nbearer={}\n", g_pid,
                    g_username, g_friend_code, g_token, g_bearer);
    void(openpak::Platform::WriteFile(FilePath(), contents));
}

} // Anonymous namespace

bool IsLinked() {
    std::lock_guard lock{g_mutex};
    EnsureLoaded();
    return g_pid != 0;
}

u64 GetPid() {
    std::lock_guard lock{g_mutex};
    EnsureLoaded();
    return g_pid;
}

std::string GetUsername() {
    std::lock_guard lock{g_mutex};
    EnsureLoaded();
    return g_username;
}

std::string GetFriendCode() {
    std::lock_guard lock{g_mutex};
    EnsureLoaded();
    return g_friend_code;
}

std::string GetToken() {
    std::lock_guard lock{g_mutex};
    EnsureLoaded();
    return g_token;
}

std::string GetBearer() {
    std::lock_guard lock{g_mutex};
    EnsureLoaded();
    return g_bearer;
}

u64 GetGeneration() {
    std::lock_guard lock{g_mutex};
    EnsureLoaded();
    return g_generation;
}

void Save(u64 pid, std::string_view username, std::string_view friend_code,
          std::string_view token, std::string_view bearer) {
    std::lock_guard lock{g_mutex};
    g_loaded = true;
    g_pid = pid;
    g_username = username;
    g_friend_code = friend_code;
    g_token = token;
    g_bearer = bearer;
    ++g_generation;
    WriteFile();
}

void Clear() {
    std::lock_guard lock{g_mutex};
    g_loaded = true;
    g_pid = 0;
    g_username.clear();
    g_friend_code.clear();
    g_token.clear();
    g_bearer.clear();
    ++g_generation;
    void(std::filesystem::remove(FilePath()));
}

void WriteGuestBridge(const std::filesystem::path& sdmc_root) {
    std::lock_guard lock{g_mutex};
    EnsureLoaded();

    const auto bridge_path = sdmc_root / "config" / "openpak" / "session.txt";
    if (g_pid == 0) {
        void(std::filesystem::remove(bridge_path)); // not linked -- clear any stale bridge
        return;
    }

    void(openpak::Platform::CreateParentDirs(bridge_path));
    const std::string contents =
        fmt::format("pid={}\nusername={}\ntoken={}\n", g_pid, g_username, g_token);
    void(openpak::Platform::WriteFile(bridge_path, contents));
}

} // namespace Common::OpenPakAccount
