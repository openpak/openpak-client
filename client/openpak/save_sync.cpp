// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "openpak/restore_swap.h"
#include "openpak/safe_entry_path.h"
#include "openpak/save_sync.h"
#include "openpak/platform.h"
#include "openpak/zip_store.h"

#include <algorithm>
#include <chrono>
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

// Every title with a signed-in account, as Ryujinx does: the old compatibility table only pins
// the versions a title's online play needs, and says nothing about whether its save may sync.
bool IsEligible(u64 /*title_id*/) {
    return Common::OpenPakAccount::IsLinked();
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

#if !defined(OPENPAK_HAVE_LIBARCHIVE) && !defined(_WIN32)

// Neither libarchive nor PowerShell (Android): the library's own zip reader and writer. It writes
// stored entries and reads stored and, with zlib, deflated ones -- what Ryujinx and the desktop
// builds upload -- refusing the whole archive over any entry it cannot place safely.
bool UnzipToDirectoryStore(std::span<const u8> zip_data, const std::filesystem::path& dest) {
    const std::filesystem::path staging = StagingPathFor(dest);
    std::error_code ec;
    std::filesystem::remove_all(staging, ec);
    if (!openpak::ZipStore::UnzipToDirectory(zip_data, staging)) {
        std::filesystem::remove_all(staging, ec);
        return false;
    }
    return SwapIntoPlace(staging, dest);
}

#endif

// The one zip and unzip this build has.
std::vector<u8> ZipSave(const std::filesystem::path& dir, [[maybe_unused]] u64 title_id) {
#if defined(OPENPAK_HAVE_LIBARCHIVE)
    return ZipDirectory(dir);
#elif defined(_WIN32)
    return ZipDirectoryPowerShell(dir, title_id);
#else
    return openpak::ZipStore::ZipDirectory(dir);
#endif
}

bool UnzipSave(std::span<const u8> zip, const std::filesystem::path& dest,
               [[maybe_unused]] u64 title_id) {
#if defined(OPENPAK_HAVE_LIBARCHIVE)
    return UnzipToDirectory(zip, dest);
#elif defined(_WIN32)
    return UnzipToDirectoryPowerShell(zip, dest, title_id);
#else
    return UnzipToDirectoryStore(zip, dest);
#endif
}

} // namespace

void Pull(const std::filesystem::path& save_dir, u64 title_id, bool force) {
#if defined(ENABLE_WEB_SERVICE)
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

    const bool applied = UnzipSave(*zip, save_dir, title_id);
    if (applied) {
        OPENPAK_LOG_INFO("OpenPak save pull {:016X}: applied ({} B)", title_id, zip->size());
    } else {
        OPENPAK_LOG_WARNING("OpenPak save pull {:016X}: archive refused -> local save kept",
                            title_id);
    }
#else
    (void)title_id;
#endif
}

std::vector<u8> CaptureForPush(const std::filesystem::path& save_dir, u64 title_id) {
    if (!IsEligible(title_id) || save_dir.empty()) {
        return {};
    }
    return ZipSave(save_dir, title_id);
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

namespace {

// Beside the save folder, not inside it: the guest owns everything inside. Per title, because in
// this layout the folder's parent is the user's, shared by every title.
std::filesystem::path MarkerFor(const std::filesystem::path& save_dir) {
    return save_dir.parent_path() / (save_dir.filename().string() + ".openpak-version");
}

std::string ReadMarker(const std::filesystem::path& save_dir) {
    std::string version = openpak::Platform::ReadFile(MarkerFor(save_dir));
    while (!version.empty() && (version.back() == '\n' || version.back() == '\r' ||
                                version.back() == ' ')) {
        version.pop_back();
    }
    return version;
}

void WriteMarker(const std::filesystem::path& save_dir, const std::string& version) {
    if (!version.empty()) {
        openpak::Platform::WriteFile(MarkerFor(save_dir), version);
    }
}

// Puts a cloud copy in place of the local one and records the version it is in step with. A
// local copy is kept beside the folder first; if it cannot be kept, nothing is touched.
bool ApplyCloud(const std::filesystem::path& save_dir, u64 title_id, const std::vector<u8>& zip,
                const std::string& cloud) {
    std::error_code ec;
    if (HasLocalContent(save_dir)) {
        const auto backup =
            save_dir.parent_path() / (save_dir.filename().string() + ".openpak-backup");
        std::filesystem::remove_all(backup, ec);
        std::filesystem::copy(save_dir, backup, std::filesystem::copy_options::recursive, ec);
        if (ec) {
            OPENPAK_LOG_WARNING("OpenPak save pull {:016X}: could not keep a backup ({}); local "
                                "save kept",
                                title_id, ec.message());
            return false;
        }
    }
    std::filesystem::create_directories(save_dir, ec);

    const bool applied = UnzipSave(zip, save_dir, title_id);
    if (!applied) {
        OPENPAK_LOG_WARNING("OpenPak save pull {:016X}: archive refused -> local save kept",
                            title_id);
        return false;
    }
    WriteMarker(save_dir, cloud);
    OPENPAK_LOG_INFO("OpenPak save pull {:016X}: version {} applied", title_id, cloud);
    return true;
}

} // namespace

PullOutcome PullBeforeLaunch(const std::filesystem::path& save_dir, u64 title_id) {
#if defined(ENABLE_WEB_SERVICE)
    if (save_dir.empty() || !Common::OpenPakAccount::IsLinked()) {
        return PullOutcome::Nothing;
    }
    const std::string title = fmt::format("{:016x}", title_id);
    const auto zip = WebService::OpenPakApi::PullSave(title);
    if (!zip || zip->empty()) {
        return PullOutcome::Nothing;
    }

    const std::string cloud = WebService::OpenPakApi::SaveVersion(title);
    const std::string known = ReadMarker(save_dir);
    const bool local = HasLocalContent(save_dir);
    if (local && cloud == known) {
        return PullOutcome::Nothing;
    }
    if (local && known.empty()) {
        return PullOutcome::BothExist;
    }
    return ApplyCloud(save_dir, title_id, *zip, cloud) ? PullOutcome::Pulled : PullOutcome::Nothing;
#else
    (void)save_dir;
    (void)title_id;
    return PullOutcome::Nothing;
#endif
}

std::vector<u8> CaptureOnExit(const std::filesystem::path& save_dir, u64 title_id) {
    // ponytail: every exit uploads; skipping an unchanged save needs a content hash of the
    // folder, add it if the allowance fills with identical versions (Ryujinx has the same note).
    if (save_dir.empty() || !Common::OpenPakAccount::IsLinked() || !HasLocalContent(save_dir)) {
        return {};
    }
    return ZipSave(save_dir, title_id);
}

std::string PushCaptured(const std::filesystem::path& save_dir, u64 title_id,
                         std::vector<u8> zip_bytes) {
#ifdef ENABLE_WEB_SERVICE
    if (zip_bytes.empty()) {
        return {};
    }
    const std::string title = fmt::format("{:016x}", title_id);
    // The version this copy was last in step with, from disk: the in-memory one is gone after a
    // restart, and a push without it cannot be told apart from a conflicting one.
    if (const std::string known = ReadMarker(save_dir); !known.empty()) {
        WebService::OpenPakApi::SetSaveVersion(title, known);
    }
    std::string error = WebService::OpenPakApi::PushSave(title, zip_bytes);
    if (!error.empty()) {
        OPENPAK_LOG_WARNING("OpenPak save push {:016X} failed: {}", title_id, error);
        return error;
    }
    WriteMarker(save_dir, WebService::OpenPakApi::SaveVersion(title));
    OPENPAK_LOG_INFO("OpenPak save push {:016X}: {} B", title_id, zip_bytes.size());
    return {};
#else
    (void)save_dir;
    (void)title_id;
    (void)zip_bytes;
    return {};
#endif
}

LocalCopy Compare(const std::filesystem::path& save_dir, int newest_cloud) {
    LocalCopy out;
    if (save_dir.empty() || !HasLocalContent(save_dir)) {
        return out;
    }
    std::error_code ec;
    std::filesystem::file_time_type newest{};
    for (const auto& entry : std::filesystem::recursive_directory_iterator(save_dir, ec)) {
        if (entry.is_regular_file(ec)) {
            newest = std::max(newest, entry.last_write_time(ec));
        }
    }
    // file_clock has no portable epoch; its distance from now is, and system_clock's epoch is
    // the Unix one.
    const auto age = std::filesystem::file_time_type::clock::now() - newest;
    out.last_written = std::chrono::duration_cast<std::chrono::seconds>(
                           (std::chrono::system_clock::now() -
                            std::chrono::duration_cast<std::chrono::system_clock::duration>(age))
                               .time_since_epoch())
                           .count();

    out.version = ReadMarker(save_dir);
    if (out.version.empty()) {
        out.state = newest_cloud > 0 ? LocalState::NoHistory : LocalState::ChangedHere;
    } else if (out.version != std::to_string(newest_cloud)) {
        out.state = LocalState::CloudNewer;
    } else {
        // In step at the last sync; a file written after the marker was played on since.
        const auto synced = std::filesystem::last_write_time(MarkerFor(save_dir), ec);
        out.state = !ec && newest > synced ? LocalState::ChangedHere : LocalState::InStep;
    }
    return out;
}

std::string Download(const std::filesystem::path& save_dir, u64 title_id) {
#if defined(ENABLE_WEB_SERVICE)
    if (save_dir.empty()) {
        return "This emulator did not say where that game keeps its save.";
    }
    const std::string title = fmt::format("{:016x}", title_id);
    const auto zip = WebService::OpenPakApi::PullSave(title);
    if (!zip || zip->empty()) {
        return "There is no cloud save for that game.";
    }
    if (!ApplyCloud(save_dir, title_id, *zip, WebService::OpenPakApi::SaveVersion(title))) {
        return "The cloud save could not be put in place; the local one is unchanged.";
    }
    return {};
#else
    (void)save_dir;
    (void)title_id;
    return "This build cannot unpack cloud saves.";
#endif
}

std::string Upload(const std::filesystem::path& save_dir, u64 title_id, int newest_cloud) {
#ifdef ENABLE_WEB_SERVICE
    std::vector<u8> zip = CaptureOnExit(save_dir, title_id);
    if (zip.empty()) {
        return "There is no local save to upload.";
    }
    const std::string title = fmt::format("{:016x}", title_id);
    // Based on what the cloud has now, not on the marker: pressing Upload is the person saying
    // this copy is the one to keep, whatever it was last in step with. The marker moves only
    // once the server has the copy.
    WebService::OpenPakApi::SetSaveVersion(title,
                                           newest_cloud > 0 ? std::to_string(newest_cloud) : "");
    std::string error = WebService::OpenPakApi::PushSave(title, zip);
    if (!error.empty()) {
        return error;
    }
    WriteMarker(save_dir, WebService::OpenPakApi::SaveVersion(title));
    return {};
#else
    (void)save_dir;
    (void)title_id;
    (void)newest_cloud;
    return "This build has no web services support.";
#endif
}

} // namespace OpenPak::SaveSync
