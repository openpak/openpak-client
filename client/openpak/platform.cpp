// SPDX-License-Identifier: GPL-2.0-or-later
#include "openpak/platform.h"
#include "openpak/log.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
namespace openpak {
static LogSink g_sink;
void SetLogSink(LogSink sink) { g_sink = std::move(sink); }
void Log(LogLevel level, const std::string& message) {
    if (g_sink) { g_sink(level, message); return; }
    static const char* const names[] = {"trace", "debug", "info", "warning", "error", "critical"};
    std::fprintf(stderr, "[openpak %s] %s\n", names[static_cast<int>(level)], message.c_str());
}
} // namespace openpak
namespace openpak::Platform {
static std::filesystem::path g_config, g_cache;
void SetDirectories(std::filesystem::path config_dir, std::filesystem::path cache_dir) { g_config = std::move(config_dir); g_cache = std::move(cache_dir); }
std::filesystem::path ConfigDir() { return g_config.empty() ? std::filesystem::temp_directory_path() / "openpak" : g_config; }
std::filesystem::path CacheDir() { return g_cache.empty() ? ConfigDir() / "cache" : g_cache; }
// Called under the lock: the host resets the source when what it reads goes away, and a heartbeat
// asking at that moment must not call into it.
//
// Made on first use, not at load: Android's emulator session is a static object, so its System is
// built and sets the source while this library's own statics are still being initialised. A
// namespace-scope std::function was then initialised after it had been set, and emptied: no active
// profile, no redirect list, and a title sent to the publisher's real servers.
namespace {
struct ProfileSource {
    std::mutex mutex;
    std::function<std::string()> source;
};
ProfileSource& Profile() {
    static ProfileSource profile;
    return profile;
}
} // namespace
void SetProfileSource(std::function<std::string()> source) { std::lock_guard lock{Profile().mutex}; Profile().source = std::move(source); }
std::string ProfileId() { std::lock_guard lock{Profile().mutex}; return Profile().source ? Profile().source() : std::string{}; }
static std::mutex g_client_mutex;
static std::string g_client;
static std::string g_client_name;
std::string_view OsName() {
#if defined(__ANDROID__)
    return "android";
#elif defined(_WIN32)
    return "windows";
#elif defined(__APPLE__) && TARGET_OS_IPHONE
    return "ios";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return {};
#endif
}
void SetClient(std::string name, std::string version) {
    std::lock_guard lock{g_client_mutex};
    if (name.empty()) { g_client.clear(); g_client_name.clear(); return; }
    g_client_name = name;
    g_client = name + "/" + (version.empty() ? "unknown" : version);
    if (const std::string_view os = OsName(); !os.empty()) { g_client += " ("; g_client += os; g_client += ")"; }
}
std::string ClientHeader() { std::lock_guard lock{g_client_mutex}; return g_client; }
std::string ClientName() { std::lock_guard lock{g_client_mutex}; return g_client_name; }
bool CreateParentDirs(const std::filesystem::path& path) { std::error_code ec; std::filesystem::create_directories(path.parent_path(), ec); return !ec; }
bool WriteFile(const std::filesystem::path& path, const std::string& contents) {
    CreateParentDirs(path);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    return static_cast<bool>(f);
}
std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss; ss << f.rdbuf(); return ss.str();
}
void SplitString(const std::string& str, char delim, std::vector<std::string>& out) {
    out.clear(); std::string cur;
    for (char c : str) { if (c == delim) { out.push_back(cur); cur.clear(); } else cur.push_back(c); }
    out.push_back(cur);
}
std::string ToLower(std::string s) { std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); }); return s; }
std::string ToUpper(std::string s) { std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); }); return s; }
std::string StripSpaces(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n"); if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n"); return s.substr(b, e - b + 1);
}
std::filesystem::path ContainedPath(const std::filesystem::path& root, const std::string& relative) {
    const std::filesystem::path normal = std::filesystem::path{relative}.lexically_normal();
    if (normal.empty() || normal.is_absolute() || normal.has_root_name() ||
        *normal.begin() == ".." || *normal.begin() == ".") {
        return {};
    }
    return root / normal;
}
const std::string& AndroidSystemRoots() {
    static const std::string roots = [] {
        std::string pem;
        for (const char* dir :
             {"/apex/com.android.conscrypt/cacerts", "/system/etc/security/cacerts"}) {
            std::error_code ec;
            for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
                if (entry.is_regular_file(ec)) {
                    pem += ReadFile(entry.path());
                    pem += '\n';
                }
            }
            if (!pem.empty()) {
                break; // the APEX copy, when there is one, is the current store
            }
        }
        return pem;
    }();
    return roots;
}
} // namespace openpak::Platform
