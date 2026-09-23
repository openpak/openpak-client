// SPDX-License-Identifier: GPL-2.0-or-later
// Each profile is its own account: the one from before profiles goes to the first profile to ask,
// a switch shows the other profile's account, and one account is found on one profile only.
#undef NDEBUG // the checks are the point of this binary, in every build type
#include <cassert>
#include <filesystem>
#include <string>
#include "openpak/account.h"
#include "openpak/platform.h"

int main() {
    namespace Account = Common::OpenPakAccount;
    const auto dir = std::filesystem::temp_directory_path() / "openpak-profile-account-test";
    std::filesystem::remove_all(dir);
    openpak::Platform::SetDirectories(dir, dir / "cache");

    std::string profile = "aaaa";
    openpak::Platform::SetProfileSource([&profile] { return profile; });

    // An install from before profiles: its account becomes the open profile's.
    openpak::Platform::WriteFile(dir / "openpak_account.txt", "pid=7\nusername=old\n");
    assert(Account::IsLinked() && Account::GetPid() == 7);
    assert(!std::filesystem::exists(dir / "openpak_account.txt"));

    // Another profile is another account, and this one is already held.
    profile = "bbbb";
    assert(!Account::IsLinked());
    assert(Account::HolderOf(7, "bbbb") == "aaaa");
    assert(Account::HolderOf(7, "aaaa").empty());

    Account::Save(9, "new", "SW-1", "token", "bearer");
    assert(Account::UsernameOf("bbbb") == "new" && Account::UsernameOf("aaaa") == "old");
    assert(Account::BearerOf("bbbb") == "bearer" && Account::BearerOf("aaaa").empty());

    profile = "aaaa";
    assert(Account::GetPid() == 7);

    // A deleted profile frees its account.
    Account::Forget("aaaa");
    assert(Account::HolderOf(7, "bbbb").empty() && !Account::IsLinked());

    std::filesystem::remove_all(dir);
    return 0;
}
