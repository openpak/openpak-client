// SPDX-License-Identifier: GPL-2.0-or-later
#include "openpak/zip_store.h"

#include <array>
#include <cstring>
#include <limits>
#include <algorithm>
#include <fstream>
#include <optional>
#include <string>

#ifdef OPENPAK_HAVE_ZLIB
#include <zlib.h>
#endif

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
constexpr std::uint16_t kDeflateMethod = 8;

// Where an entry name off the network lands under root: relative, never climbing, still inside.
std::optional<std::filesystem::path> SafePath(const std::filesystem::path& root,
                                              const std::string& name) {
    const std::filesystem::path rel(name);
    if (rel.is_absolute() || rel.has_root_directory() || !rel.root_name().empty()) {
        return std::nullopt;
    }
    for (const auto& part : rel) {
        if (part == "..") {
            return std::nullopt;
        }
    }
    const std::filesystem::path full = (root / rel).lexically_normal();
    const std::string base = (root / "").lexically_normal().string();
    if (full.string().rfind(base, 0) != 0) {
        return std::nullopt;
    }
    return full;
}

// A deflated entry, as every other zip writer makes them. Without zlib there is no inflating, and
// the archive is refused rather than half restored.
bool Inflate(const std::uint8_t* data, std::size_t size, std::uint32_t expected,
             std::vector<std::uint8_t>& out) {
#ifdef OPENPAK_HAVE_ZLIB
    out.resize(expected);
    z_stream stream{};
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
        return false;
    }
    stream.next_in = const_cast<Bytef*>(data);
    stream.avail_in = static_cast<uInt>(size);
    stream.next_out = out.data();
    stream.avail_out = static_cast<uInt>(out.size());
    const int result = inflate(&stream, Z_FINISH);
    const bool ok = result == Z_STREAM_END && stream.total_out == expected;
    inflateEnd(&stream);
    return ok;
#else
    (void)data;
    (void)size;
    (void)expected;
    (void)out;
    return false;
#endif
}

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
    std::size_t eocd_pos = (std::numeric_limits<std::size_t>::max)();
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
    if (ec) {
        return false;
    }

    std::size_t pos = eocd.dir_offset;
    for (std::uint16_t i = 0; i < eocd.entries; i++) {
        if (pos + sizeof(CentralEntry) > zip.size()) {
            return false;
        }
        CentralEntry central{};
        std::memcpy(&central, zip.data() + pos, sizeof(central));
        pos += sizeof(CentralEntry);
        if (central.signature != kCentralSig) {
            return false;
        }
        if (pos + central.name_length + central.extra_length + central.comment_length > zip.size()) {
            return false;
        }
        const std::string name(reinterpret_cast<const char*>(zip.data() + pos), central.name_length);
        pos += static_cast<std::size_t>(central.name_length) + central.extra_length +
               central.comment_length;

        if (name.empty()) {
            return false;
        }
        const auto out_path = SafePath(dest, name);
        if (!out_path) {
            return false; // never write outside dest
        }
        if (name.back() == '/') {
            std::filesystem::create_directories(*out_path, ec);
            continue;
        }

        std::size_t local = central.local_offset;
        if (local + sizeof(LocalHeader) > zip.size()) {
            return false;
        }
        LocalHeader header{};
        std::memcpy(&header, zip.data() + local, sizeof(header));
        if (header.signature != kLocalSig) {
            return false;
        }
        local += sizeof(LocalHeader) + header.name_length + header.extra_length;
        if (local + central.compressed_size > zip.size()) {
            return false;
        }

        std::vector<std::uint8_t> inflated;
        const std::uint8_t* data = zip.data() + local;
        std::size_t size = central.compressed_size;
        if (central.method == kDeflateMethod) {
            if (!Inflate(data, size, central.uncompressed_size, inflated)) {
                return false;
            }
            data = inflated.data();
            size = inflated.size();
        } else if (central.method != kStoreMethod) {
            return false; // a save missing a file is a broken save, not a smaller one
        }

        std::filesystem::create_directories(out_path->parent_path(), ec);
        std::ofstream f(*out_path, std::ios::binary | std::ios::trunc);
        if (!f) {
            return false;
        }
        f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
        if (!f) {
            return false;
        }
    }
    return true;
}

} // namespace openpak::ZipStore
