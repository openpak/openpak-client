// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "openpak/qt/save_sync.h"
#include "openpak/platform.h"

#include <filesystem>
#include <vector>

#ifdef _WIN32
#include <cstdlib>
#include <fstream>
#include <iterator>
#endif

#include <fmt/format.h>

#include "openpak/compatible_titles.h"
#include "openpak/log.h"
#include "openpak/account.h"

#ifdef ENABLE_WEB_SERVICE
#include "openpak/api.h"
#endif

#ifdef OPENPAK_HAVE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>
#endif

namespace Nextendo::SaveSync {

namespace {

#if defined(OPENPAK_HAVE_LIBARCHIVE) || defined(_WIN32)

// Only ever called from the real-backend paths below (libarchive, or the Windows PowerShell
// fallback) -- kept inside this guard so an unused-function error doesn't fire on builds with
// neither (e.g. a non-Windows build without libarchive).
bool IsEligible(u64 title_id) {
    return Nextendo::CompatibleTitles::Table().count(title_id) != 0 &&
          Common::OpenPakAccount::IsLinked();
}

bool HasLocalContent(const std::filesystem::path& dir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return false;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, ec)) {
        if (entry.is_regular_file(ec)) {
            return true;
        }
    }
    return false;
}

#endif // OPENPAK_HAVE_LIBARCHIVE || _WIN32

#ifdef OPENPAK_HAVE_LIBARCHIVE

void AddDirectoryToArchive(struct archive* a, const std::filesystem::path& dir,
                           const std::string& prefix) {
    std::error_code ec;
    for (const auto& item : std::filesystem::directory_iterator(dir, ec)) {
        if (item.is_directory(ec)) {
            AddDirectoryToArchive(a, item.path(), prefix + item.path().filename().string() + "/");
            continue;
        }
        if (!item.is_regular_file(ec)) {
            continue;
        }
        const std::string contents = openpak::Platform::ReadFile(item.path());
        const std::vector<u8> bytes(contents.begin(), contents.end());
        struct archive_entry* entry = archive_entry_new();
        archive_entry_set_pathname(entry, (prefix + item.path().filename().string()).c_str());
        archive_entry_set_size(entry, static_cast<la_int64_t>(bytes.size()));
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0644);
        if (archive_write_header(a, entry) == ARCHIVE_OK && !bytes.empty()) {
            archive_write_data(a, bytes.data(), bytes.size());
        }
        archive_entry_free(entry);
    }
}

la_ssize_t ArchiveAppendCallback(struct archive*, void* client_data, const void* buff,
                                 size_t length) {
    auto* out = static_cast<std::vector<u8>*>(client_data);
    const auto* bytes = static_cast<const u8*>(buff);
    out->insert(out->end(), bytes, bytes + length);
    return static_cast<la_ssize_t>(length);
}

std::vector<u8> ZipDirectory(const std::filesystem::path& dir) {
    std::vector<u8> out;
    struct archive* a = archive_write_new();
    if (!a) {
        return out;
    }
    archive_write_set_format_zip(a);
    archive_write_open(a, &out, nullptr, ArchiveAppendCallback, nullptr);
    AddDirectoryToArchive(a, dir, "");
    archive_write_close(a);
    archive_write_free(a);
    return out;
}

bool UnzipToDirectory(std::span<const u8> zip_data, const std::filesystem::path& dest) {
    struct archive* a = archive_read_new();
    struct archive* ext = archive_write_disk_new();
    if (!a || !ext) {
        return false;
    }

    archive_read_support_format_zip(a);
    archive_read_support_filter_all(a);
    archive_write_disk_set_options(ext, ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM);
    archive_write_disk_set_standard_lookup(ext);

    if (archive_read_open_memory(a, zip_data.data(), zip_data.size()) != ARCHIVE_OK) {
        archive_read_free(a);
        archive_write_free(ext);
        return false;
    }

    std::filesystem::create_directories(dest);

    struct archive_entry* entry;
    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        const std::filesystem::path entry_path = dest / archive_entry_pathname(entry);
        archive_entry_set_pathname(entry, entry_path.string().c_str());

        if (archive_write_header(ext, entry) != ARCHIVE_OK) {
            continue;
        }
        if (archive_entry_size(entry) > 0) {
            const void* buff;
            size_t size;
            la_int64_t offset;
            while (archive_read_data_block(a, &buff, &size, &offset) == ARCHIVE_OK) {
                if (archive_write_data_block(ext, buff, size, offset) != ARCHIVE_OK) {
                    break;
                }
            }
        }
        archive_write_finish_entry(ext);
    }

    archive_read_free(a);
    archive_write_free(ext);
    return true;
}

#endif // OPENPAK_HAVE_LIBARCHIVE

#if !defined(OPENPAK_HAVE_LIBARCHIVE) && defined(_WIN32)

// No libarchive available for this target (e.g. the llvm-mingw cross-compiled Windows build has
// no mingw port of it wired up). GetFullPath() on a save directory is always a real path on
// disk, so shell out to PowerShell's Compress-Archive/Expand-Archive against it directly --
// the same fallback idiom GMainWindow::ExtractZipToDirectory already uses for firmware zips.

std::vector<u8> ZipDirectoryPowerShell(const std::filesystem::path& dir, u64 title_id) {
    const std::filesystem::path real_dir = dir;
    const std::filesystem::path tmp_zip =
        std::filesystem::temp_directory_path() / fmt::format("nextendo_save_{:016x}.zip", title_id);
    std::error_code ec;
    std::filesystem::remove(tmp_zip, ec);

    const std::string cmd = "powershell -NoProfile -NonInteractive -Command \"Compress-Archive -Path \\\"" +
                             real_dir.string() + "\\*\\\" -DestinationPath \\\"" + tmp_zip.string() +
                             "\\\" -Force\"";
    if (std::system(cmd.c_str()) != 0) {
        std::filesystem::remove(tmp_zip, ec);
        return {};
    }

    std::vector<u8> out;
    {
        // Windows can't delete a file with an open handle (unlike POSIX) -- close in first.
        std::ifstream in(tmp_zip, std::ios::binary);
        out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    std::filesystem::remove(tmp_zip, ec);
    return out;
}

bool UnzipToDirectoryPowerShell(std::span<const u8> zip_data, const std::filesystem::path& dest,
                                 u64 title_id) {
    std::filesystem::create_directories(dest);
    const std::filesystem::path tmp_zip = std::filesystem::temp_directory_path() /
                                           fmt::format("nextendo_save_in_{:016x}.zip", title_id);
    {
        std::ofstream out(tmp_zip, std::ios::binary);
        if (!out) {
            return false;
        }
        out.write(reinterpret_cast<const char*>(zip_data.data()),
                  static_cast<std::streamsize>(zip_data.size()));
    }

    const std::string cmd = "powershell -NoProfile -NonInteractive -Command \"Expand-Archive -Path \\\"" +
                             tmp_zip.string() + "\\\" -DestinationPath \\\"" + dest.string() +
                             "\\\" -Force\"";
    const bool ok = std::system(cmd.c_str()) == 0;
    std::error_code ec;
    std::filesystem::remove(tmp_zip, ec);
    return ok;
}

#endif // !OPENPAK_HAVE_LIBARCHIVE && _WIN32

} // namespace

void Pull(const std::filesystem::path& save_dir, u64 title_id, bool force) {
#if defined(ENABLE_WEB_SERVICE) && (defined(OPENPAK_HAVE_LIBARCHIVE) || defined(_WIN32))
    if (!IsEligible(title_id)) {
        return;
    }
    if (!force && HasLocalContent(save_dir)) {
        OPENPAK_LOG_INFO("OpenPak save pull {:016X}: local save present -> kept (no overwrite)",
                 title_id);
        return;
    }
    if (save_dir.empty()) {
        return;
    }

    const auto zip = WebService::OpenPakApi::PullSave(fmt::format("{:016x}", title_id));
    if (!zip || zip->empty()) {
        return;
    }

#ifdef OPENPAK_HAVE_LIBARCHIVE
    const bool applied = UnzipToDirectory(*zip, save_dir);
#else
    const bool applied = UnzipToDirectoryPowerShell(*zip, save_dir, title_id);
#endif
    if (applied) {
        OPENPAK_LOG_INFO("OpenPak save pull {:016X}: applied ({} B)", title_id, zip->size());
    }
#else
    (void)title_id;
#endif
}

std::vector<u8> CaptureForPush(const std::filesystem::path& save_dir, u64 title_id) {
#if defined(OPENPAK_HAVE_LIBARCHIVE) || defined(_WIN32)
    if (!IsEligible(title_id)) {
        return {};
    }
    if (save_dir.empty()) {
        return {};
    }
#ifdef OPENPAK_HAVE_LIBARCHIVE
    return ZipDirectory(save_dir);
#else
    return ZipDirectoryPowerShell(save_dir, title_id);
#endif
#else
    (void)title_id;
    return {};
#endif
}

void UploadCaptured(u64 title_id, std::vector<u8> zip_bytes) {
#ifdef ENABLE_WEB_SERVICE
    if (zip_bytes.empty()) {
        return;
    }
    const std::string error =
        WebService::OpenPakApi::PushSave(fmt::format("{:016x}", title_id), zip_bytes);
    if (!error.empty()) {
        OPENPAK_LOG_WARNING("OpenPak save push {:016X} failed: {}", title_id, error);
    } else {
        OPENPAK_LOG_INFO("OpenPak save push {:016X}: {} B", title_id, zip_bytes.size());
    }
#else
    (void)title_id;
    (void)zip_bytes;
#endif
}

} // namespace Nextendo::SaveSync
