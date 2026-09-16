// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "openpak/qt/safe_entry_path.h"
#include "openpak/qt/save_sync.h"
#include "openpak/platform.h"

#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
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

namespace OpenPak::SaveSync {

namespace {

#if defined(OPENPAK_HAVE_LIBARCHIVE) || defined(_WIN32)

// Only ever called from the real-backend paths below (libarchive, or the Windows PowerShell
// fallback) -- kept inside this guard so an unused-function error doesn't fire on builds with
// neither (e.g. a non-Windows build without libarchive).
// A restore replaces a player's progress. Extract beside the save directory and swap it in
// once, so a failure halfway through leaves the old save whole instead of half of each.
std::filesystem::path StagingPathFor(const std::filesystem::path& dest) {
    return dest.parent_path() / (dest.filename().string() + ".openpak-new");
}

bool SwapIntoPlace(const std::filesystem::path& staged, const std::filesystem::path& dest) {
    std::error_code ec;
    const std::filesystem::path previous =
        dest.parent_path() / (dest.filename().string() + ".openpak-old");
    std::filesystem::remove_all(previous, ec);
    if (std::filesystem::exists(dest)) {
        std::filesystem::rename(dest, previous, ec);
        if (ec) {
            std::filesystem::remove_all(staged, ec);
            return false;
        }
    }
    std::filesystem::rename(staged, dest, ec);
    if (ec) {
        // A failed restore must not also be a deletion: put the old save back.
        std::error_code back;
        if (std::filesystem::exists(previous)) {
            std::filesystem::rename(previous, dest, back);
        }
        std::filesystem::remove_all(staged, ec);
        return false;
    }
    std::filesystem::remove_all(previous, ec);
    return true;
}

bool IsEligible(u64 title_id) {
    return OpenPak::CompatibleTitles::Table().count(title_id) != 0 &&
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

// ARCHIVE_EXTRACT_SECURE_NOABSOLUTEPATHS is deliberately absent: the path handed to
// archive_write_disk is an absolute one this function built, and that flag would refuse it.
// Absolute names inside the archive are refused by SafeEntryPath instead, before the join.
constexpr int kExtractFlags = ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM |
                               ARCHIVE_EXTRACT_SECURE_NODOTDOT |
                               ARCHIVE_EXTRACT_SECURE_SYMLINKS;

bool UnzipToDirectory(std::span<const u8> zip_data, const std::filesystem::path& dest) {
    const std::filesystem::path staging = StagingPathFor(dest);
    std::error_code ec;
    std::filesystem::remove_all(staging, ec);
    std::filesystem::create_directories(staging, ec);
    if (ec) {
        return false;
    }

    struct archive* a = archive_read_new();
    struct archive* ext = archive_write_disk_new();
    if (!a || !ext) {
        if (a) {
            archive_read_free(a);
        }
        if (ext) {
            archive_write_free(ext);
        }
        std::filesystem::remove_all(staging, ec);
        return false;
    }

    archive_read_support_format_zip(a);
    archive_read_support_filter_all(a);
    archive_write_disk_set_options(ext, kExtractFlags);
    archive_write_disk_set_standard_lookup(ext);

    if (archive_read_open_memory(a, zip_data.data(), zip_data.size()) != ARCHIVE_OK) {
        archive_read_free(a);
        archive_write_free(ext);
        std::filesystem::remove_all(staging, ec);
        return false;
    }

    bool ok = true;
    struct archive_entry* entry;
    int header = ARCHIVE_OK;
    while ((header = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
        // A save is files and directories. A symlink, a device or a hardlink in one is not a
        // save, and the only reason to put one there is to write somewhere else.
        const auto filetype = archive_entry_filetype(entry);
        const char* name = archive_entry_pathname(entry);
        if ((filetype != AE_IFREG && filetype != AE_IFDIR) || archive_entry_hardlink(entry) ||
            archive_entry_symlink(entry) || name == nullptr) {
            OPENPAK_LOG_ERROR("OpenPak save restore: refusing archive entry {}",
                              name != nullptr ? name : "(unnamed)");
            ok = false;
            break;
        }
        const auto target = SafeEntryPath(staging, name);
        if (!target) {
            OPENPAK_LOG_ERROR("OpenPak save restore: refusing archive entry {}", name);
            ok = false;
            break;
        }
        archive_entry_set_pathname(entry, target->string().c_str());

        if (archive_write_header(ext, entry) != ARCHIVE_OK) {
            ok = false;
            break;
        }
        if (archive_entry_size(entry) > 0) {
            const void* buff;
            size_t size;
            la_int64_t offset;
            int block = ARCHIVE_OK;
            while ((block = archive_read_data_block(a, &buff, &size, &offset)) == ARCHIVE_OK) {
                if (archive_write_data_block(ext, buff, size, offset) != ARCHIVE_OK) {
                    ok = false;
                    break;
                }
            }
            if (block != ARCHIVE_EOF) {
                ok = false;
            }
        }
        if (archive_write_finish_entry(ext) != ARCHIVE_OK) {
            ok = false;
        }
        if (!ok) {
            break;
        }
    }
    if (header != ARCHIVE_EOF) {
        ok = false;
    }

    archive_read_free(a);
    archive_write_free(ext);
    if (!ok) {
        std::filesystem::remove_all(staging, ec);
        return false;
    }
    return SwapIntoPlace(staging, dest);
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
        std::filesystem::temp_directory_path() / fmt::format("legacy_save_{:016x}.zip", title_id);
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

// ponytail: Expand-Archive is the only unzip this build has, so entry names are judged by
// PowerShell and not by us. Expanding into a staging directory and swapping keeps a failure
// from eating the old save; an entry that escapes staging is a hole only a real zip reader
// closes, which is what wiring libarchive into this target would buy.
bool UnzipToDirectoryPowerShell(std::span<const u8> zip_data, const std::filesystem::path& dest,
                                 u64 title_id) {
    const std::filesystem::path staging = StagingPathFor(dest);
    std::error_code ec;
    std::filesystem::remove_all(staging, ec);
    std::filesystem::create_directories(staging, ec);
    if (ec) {
        return false;
    }
    const std::filesystem::path tmp_zip = std::filesystem::temp_directory_path() /
                                           fmt::format("legacy_save_in_{:016x}.zip", title_id);
    {
        std::ofstream out(tmp_zip, std::ios::binary);
        if (!out) {
            std::filesystem::remove_all(staging, ec);
            return false;
        }
        out.write(reinterpret_cast<const char*>(zip_data.data()),
                  static_cast<std::streamsize>(zip_data.size()));
    }

    const std::string cmd = "powershell -NoProfile -NonInteractive -Command \"Expand-Archive -Path \\\"" +
                             tmp_zip.string() + "\\\" -DestinationPath \\\"" + staging.string() +
                             "\\\" -Force\"";
    const bool ok = std::system(cmd.c_str()) == 0;
    std::filesystem::remove(tmp_zip, ec);
    if (!ok) {
        std::filesystem::remove_all(staging, ec);
        return false;
    }
    return SwapIntoPlace(staging, dest);
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

} // namespace OpenPak::SaveSync
