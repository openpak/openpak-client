// SPDX-License-Identifier: GPL-2.0-or-later
#include "openpak/qt/host.h"

#include <fmt/format.h>

namespace openpak::qt {
static Host* g_current = nullptr;
Host* Host::Current() { return g_current; }
void Host::SetCurrent(Host* host) { g_current = host; }

std::string Host::CloudSaveKey(u64 title_id) const {
    return fmt::format("{:016x}", title_id);
}

u64 Host::CloudSaveTitle(const std::string& key) const {
    try {
        return std::stoull(key, nullptr, 16);
    } catch (const std::exception&) {
        return 0;
    }
}

OpenPak::SaveSync::LocalCopy Host::CompareSave(const std::filesystem::path& save_dir, u64,
                                                int newest_cloud) {
    return OpenPak::SaveSync::Compare(save_dir, newest_cloud);
}

std::string Host::DownloadSave(const std::filesystem::path& save_dir, u64 title_id) {
    return OpenPak::SaveSync::Download(save_dir, title_id);
}

std::string Host::UploadSave(const std::filesystem::path& save_dir, u64 title_id, int newest_cloud) {
    return OpenPak::SaveSync::Upload(save_dir, title_id, newest_cloud);
}
} // namespace openpak::qt
