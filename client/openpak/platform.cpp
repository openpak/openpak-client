// SPDX-License-Identifier: GPL-2.0-or-later
#include "openpak/platform.h"
#include "openpak/log.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
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
} // namespace openpak::Platform
