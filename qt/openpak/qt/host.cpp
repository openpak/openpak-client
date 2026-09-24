// SPDX-License-Identifier: GPL-2.0-or-later
#include "openpak/qt/host.h"

#include <QCoreApplication>
#include <QMetaObject>

#include <fmt/format.h>

#include "openpak/network_profile.h"
#include "openpak/qt/strings.h"

namespace openpak::qt {
static Host* g_current = nullptr;
Host* Host::Current() { return g_current; }
void Host::SetCurrent(Host* host) {
    g_current = host;
    if (host == nullptr) {
        return;
    }
    // The re-check runs on its own thread; the host hears about it on the UI thread.
    NetworkProfile::SetChangeNotifier([] {
        QCoreApplication* app = QCoreApplication::instance();
        if (app == nullptr) {
            return;
        }
        QMetaObject::invokeMethod(
            app,
            [] {
                if (Host* current = Host::Current()) {
                    current->NetworkRedirectsChanged();
                }
            },
            Qt::QueuedConnection);
    });
}

QString RedirectsChangedText(RedirectsApplied when) {
    return when == RedirectsApplied::AtGameStart
               ? Tr("OpenPak updated this system's network redirects. Restart the game to use "
                    "them.")
               : Tr("OpenPak updated this system's network redirects. Restart the emulator to "
                    "use them.");
}

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
