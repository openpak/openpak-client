// SPDX-License-Identifier: GPL-2.0-or-later
#include "openpak/zip_store.h"

#include <array>
#include <cstring>
#include <limits>
#include <algorithm>
#include <fstream>

namespace openpak::ZipStore {

namespace {

#pragma pack(push, 1)
struct LocalHeader {
    std::uint32_t signature;   // 0x04034b50
    std::uint16_t version;
    std::uint16_t flags;
    std::uint16_t method;
    std::uint16_t mod_time;
    std::uint16_t mod_date;
    std::uint32_t crc32;
    std::uint32_t compressed_size;
    std::uint32_t uncompressed_size;
    std::uint16_t name_length;
    std::uint16_t extra_length;
};

struct CentralEntry {
    std::uint32_t signature;   // 0x02014b50
    std::uint16_t version_made;
    std::uint16_t version_needed;
    std::uint16_t flags;
    std::uint16_t method;
    std::uint16_t mod_time;
    std::uint16_t mod_date;
    std::uint32_t crc32;
    std::uint32_t compressed_size;
    std::uint32_t uncompressed_size;
    std::uint16_t name_length;
    std::uint16_t extra_length;
    std::uint16_t comment_length;
    std::uint16_t disk_start;
    std::uint16_t internal_attrs;
    std::uint32_t external_attrs;
    std::uint32_t local_offset;
};

struct EndOfCentral {
    std::uint32_t signature;   // 0x06054b50
    std::uint16_t disk;
    std::uint16_t dir_disk;
    std::uint16_t entries_on_disk;
    std::uint16_t entries;
    std::uint32_t dir_size;
    std::uint32_t dir_offset;
    std::uint16_t comment_length;
};
#pragma pack(pop)

constexpr std::uint32_t kLocalSig = 0x04034b50;
constexpr std::uint32_t kCentralSig = 0x02014b50;
constexpr std::uint32_t kEocdSig = 0x06054b50;
constexpr std::uint16_t kStoreMethod = 0;

std::uint32_t Crc32(const std::uint8_t* data, std::size_t size) {
    static std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; i++) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; k++) {
                c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; i++) {
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

bool ReadFileBytes(const std::filesystem::path& path, std::vector<std::uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return false;
    }
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    if (size < 0) {
        return false;
    }
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<std::size_t>(size));
    f.read(reinterpret_cast<char*>(out.data()), size);
    return static_cast<bool>(f) || size == 0;
}

void Append(std::vector<std::uint8_t>& out, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    out.insert(out.end(), bytes, bytes + size);
}

template <typename T>
void AppendStruct(std::vector<std::uint8_t>& out, const T& value) {
    Append(out, &value, sizeof(T));
}

} // namespace

std::vector<std::uint8_t> ZipDirectory(const std::filesystem::path& dir) {
    std::vector<std::uint8_t> out;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return out;
    }

    struct Entry {
        std::string name;
        std::uint32_t crc;
        std::uint32_t size;
        std::size_t local_offset;
    };
    std::vector<Entry> entries;

    // Deterministic order: the same directory zips to the same bytes.
    std::vector<std::filesystem::path> paths;
    for (const auto& item : std::filesystem::recursive_directory_iterator(dir, ec)) {
        if (item.is_regular_file(ec)) {
            paths.push_back(item.path());
        }
    }
    std::sort(paths.begin(), paths.end());

    for (const auto& path : paths) {
        std::string rel = std::filesystem::relative(path, dir, ec).generic_string();
        if (rel.empty() || ec) {
            continue;
        }
        std::vector<std::uint8_t> contents;
        if (!ReadFileBytes(path, contents)) {
            continue;
        }
        Entry entry;
        entry.name = rel;
        entry.size = static_cast<std::uint32_t>(contents.size());
        entry.crc = Crc32(contents.data(), contents.size());
        entry.local_offset = out.size();

        LocalHeader header{};
        header.signature = kLocalSig;
        header.version = 20;
        header.method = kStoreMethod;
        header.crc32 = entry.crc;
        header.compressed_size = entry.size;
        header.uncompressed_size = entry.size;
        header.name_length = static_cast<std::uint16_t>(rel.size());
        AppendStruct(out, header);
        Append(out, rel.data(), rel.size());
        Append(out, contents.data(), contents.size());
        entries.push_back(entry);
    }

    const std::size_t dir_offset = out.size();
    for (const auto& entry : entries) {
        CentralEntry central{};
        central.signature = kCentralSig;
        central.version_made = 20;
        central.version_needed = 20;
        central.method = kStoreMethod;
        central.crc32 = entry.crc;
        central.compressed_size = entry.size;
        central.uncompressed_size = entry.size;
        central.name_length = static_cast<std::uint16_t>(entry.name.size());
        central.local_offset = static_cast<std::uint32_t>(entry.local_offset);
        AppendStruct(out, central);
        Append(out, entry.name.data(), entry.name.size());
    }
    const std::size_t dir_size = out.size() - dir_offset;

    EndOfCentral eocd{};
    eocd.signature = kEocdSig;
    eocd.entries = static_cast<std::uint16_t>(entries.size());
    eocd.entries_on_disk = eocd.entries;
    eocd.dir_size = static_cast<std::uint32_t>(dir_size);
    eocd.dir_offset = static_cast<std::uint32_t>(dir_offset);
    AppendStruct(out, eocd);
    return out;
}

bool UnzipToDirectory(std::span<const std::uint8_t> zip, const std::filesystem::path& dest) {
    if (zip.size() < sizeof(EndOfCentral)) {
        return false;
    }

    // Scan back for the EOCD; the archive may carry a trailing comment.
    std::size_t eocd_pos = std::numeric_limits<std::size_t>::max();
    const std::size_t first_candidate = zip.size() - sizeof(EndOfCentral);
    for (std::size_t i = 0;; i++) {
        const std::size_t pos = first_candidate - i;
        std::uint32_t sig{};
        std::memcpy(&sig, zip.data() + pos, 4);
        if (sig == kEocdSig) {
            EndOfCentral eocd{};
            std::memcpy(&eocd, zip.data() + pos, sizeof(eocd));
            if (pos + sizeof(EndOfCentral) + eocd.comment_length <= zip.size()) {
                eocd_pos = pos;
                break;
            }
        }
        if (pos == 0) {
            return false;
        }
    }

    EndOfCentral eocd{};
    std::memcpy(&eocd, zip.data() + eocd_pos, sizeof(eocd));
    if (eocd.dir_offset + eocd.dir_size > eocd_pos) {
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(dest, ec);

    std::size_t pos = eocd.dir_offset;
    for (std::uint16_t i = 0; i < eocd.entries; i++) {
        if (pos + sizeof(CentralEntry) > zip.size()) {
            break;
        }
        CentralEntry central{};
        std::memcpy(&central, zip.data() + pos, sizeof(central));
        pos += sizeof(CentralEntry);
        if (central.signature != kCentralSig) {
            break;
        }
        if (pos + central.name_length + central.extra_length + central.comment_length > zip.size()) {
            break;
        }
        const std::string name(reinterpret_cast<const char*>(zip.data() + pos), central.name_length);
        pos += static_cast<std::size_t>(central.name_length) + central.extra_length +
               central.comment_length;

        if (central.method != kStoreMethod || !name.size() || name.back() == '/') {
            continue; // only stored files
        }
        if (name.find("..") != std::string::npos || name.front() == '/') {
            continue; // never write outside dest
        }

        std::size_t local = central.local_offset;
        if (local + sizeof(LocalHeader) > zip.size()) {
            continue;
        }
        LocalHeader header{};
        std::memcpy(&header, zip.data() + local, sizeof(header));
        if (header.signature != kLocalSig) {
            continue;
        }
        local += sizeof(LocalHeader) + header.name_length + header.extra_length;
        if (local + central.compressed_size > zip.size()) {
            continue;
        }

        const std::filesystem::path out_path = dest / std::filesystem::path(name);
        std::filesystem::create_directories(out_path.parent_path(), ec);
        std::ofstream f(out_path, std::ios::binary | std::ios::trunc);
        if (!f) {
            continue;
        }
        f.write(reinterpret_cast<const char*>(zip.data() + local), central.compressed_size);
    }
    return true;
}

} // namespace openpak::ZipStore
