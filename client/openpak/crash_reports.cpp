// SPDX-License-Identifier: GPL-2.0-or-later
#include "openpak/crash_reports.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>
#include <system_error>

#include <nlohmann/json.hpp>

#include "openpak/api.h"
#include "openpak/log.h"
#include "openpak/platform.h"

#if defined(__unix__) || defined(__APPLE__)
#define OPENPAK_CRASH_POSIX 1
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#if defined(__GLIBC__)
#include <execinfo.h>
#endif
#endif

namespace openpak::CrashReports {
namespace {

namespace fs = std::filesystem;

// The log marker: everything after it is the host's log, trimmed to its last lines when sent.
constexpr std::string_view kLogMarker = "\n---- log ----\n";
// How much of the log file a report copies; trimmed to kTailLines when it is sent.
constexpr std::size_t kLogCopyBytes = 256u * 1024u;

std::mutex g_mutex;
Options g_options;
fs::path g_dir;
std::atomic<u64> g_title{0};
std::atomic<bool> g_installed{false};
std::atomic<bool> g_never{false};
std::atomic<bool> g_recorded{false};
std::terminate_handler g_previous_terminate = nullptr;

// Everything a signal handler needs, prepared before it can run.
char g_dir_c[1024];
char g_log_c[1024];
char g_meta_prefix[2048];

fs::path Dir() {
    std::lock_guard lock{g_mutex};
    return g_dir.empty() ? openpak::Platform::ConfigDir() / "crash-reports" : g_dir;
}

void AppendHex(std::string& out, u64 value, int digits) {
    static constexpr char kDigits[] = "0123456789abcdef";
    for (int i = digits - 1; i >= 0; --i) {
        out.push_back(kDigits[(value >> (i * 4)) & 0xF]);
    }
}

std::string ReadTail(const fs::path& path, std::size_t bytes) {
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        return {};
    }
    file.seekg(0, std::ios::end);
    const auto size = static_cast<std::size_t>(std::max<std::streamoff>(file.tellg(), 0));
    const std::size_t start = size > bytes ? size - bytes : 0;
    file.seekg(static_cast<std::streamoff>(start));
    std::string out(size - start, '\0');
    file.read(out.data(), static_cast<std::streamsize>(out.size()));
    out.resize(static_cast<std::size_t>(file.gcount()));
    return out;
}

// "<13-digit epoch ms>-<pid>-<n>", so a name sorts by time and neither two processes nor two
// reports in the same millisecond collide.
std::string NewName() {
    static std::atomic<unsigned> sequence{0};
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    char name[64];
#ifdef OPENPAK_CRASH_POSIX
    std::snprintf(name, sizeof(name), "%013lld-%ld-%04u", static_cast<long long>(ms),
                  static_cast<long>(::getpid()), sequence++ % 10000);
#else
    std::snprintf(name, sizeof(name), "%013lld-%u-%04u", static_cast<long long>(ms),
                  static_cast<unsigned>(std::random_device{}()), sequence++ % 10000);
#endif
    return name;
}

void Prune() {
    auto reports = Pending();
    for (std::size_t i = 0; i + kMaxPending < reports.size(); ++i) {
        Discard(reports[i]);
    }
}

void OnTerminate() {
    std::string what = "unknown exception";
    if (const auto exception = std::current_exception()) {
        try {
            std::rethrow_exception(exception);
        } catch (const std::exception& e) {
            what = e.what();
        } catch (...) {
        }
    } else {
        what = "std::terminate without an exception";
    }
    if (!g_recorded.exchange(true)) {
        Record("unhandled_exception", what, "std::terminate: " + what,
               {{"kind", "unhandled_exception"}});
    }
    if (g_previous_terminate != nullptr) {
        g_previous_terminate();
    }
    std::abort();
}

#ifdef OPENPAK_CRASH_POSIX

constexpr int kSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
struct sigaction g_previous[64];

// Only async-signal-safe calls from here down: open, write, read, lseek, close, rename, getpid,
// clock_gettime, sigaction, raise. No allocation, no locks, no stdio.

void SafeWrite(int fd, const char* data, std::size_t size) {
    while (size > 0) {
        const ssize_t written = ::write(fd, data, size);
        if (written <= 0) {
            return;
        }
        data += written;
        size -= static_cast<std::size_t>(written);
    }
}

void SafeWrite(int fd, const char* text) {
    SafeWrite(fd, text, std::strlen(text));
}

// Decimal or hex into buf; returns the length. `width` zero-pads.
std::size_t FormatNumber(char* buf, u64 value, unsigned base, std::size_t width = 0) {
    char digits[32];
    std::size_t n = 0;
    do {
        digits[n++] = "0123456789abcdef"[value % base];
        value /= base;
    } while (value != 0 && n < sizeof(digits));
    while (n < width && n < sizeof(digits)) {
        digits[n++] = '0';
    }
    for (std::size_t i = 0; i < n; ++i) {
        buf[i] = digits[n - 1 - i];
    }
    buf[n] = '\0';
    return n;
}

// dst += src, bounded.
void Append(char* dst, std::size_t cap, const char* src) {
    std::size_t len = std::strlen(dst);
    while (*src != '\0' && len + 1 < cap) {
        dst[len++] = *src++;
    }
    dst[len] = '\0';
}

const char* SignalName(int sig) {
    switch (sig) {
    case SIGSEGV:
        return "SIGSEGV";
    case SIGBUS:
        return "SIGBUS";
    case SIGILL:
        return "SIGILL";
    case SIGFPE:
        return "SIGFPE";
    case SIGABRT:
        return "SIGABRT";
    default:
        return "signal";
    }
}

void WriteSignalReport(int sig, siginfo_t* info) {
    char number[40];
    char base[1200] = {};
    Append(base, sizeof(base), g_dir_c);
    Append(base, sizeof(base), "/");

    timespec now{};
    ::clock_gettime(CLOCK_REALTIME, &now);
    FormatNumber(number, static_cast<u64>(now.tv_sec) * 1000u + static_cast<u64>(now.tv_nsec) / 1000000u, 10, 13);
    Append(base, sizeof(base), number);
    Append(base, sizeof(base), "-");
    FormatNumber(number, static_cast<u64>(::getpid()), 10);
    Append(base, sizeof(base), number);

    char address[40] = "0x";
    FormatNumber(address + 2, reinterpret_cast<u64>(info != nullptr ? info->si_addr : nullptr), 16);

    // The attachment: what happened, the stack, then the log's tail.
    char path[1300] = {};
    Append(path, sizeof(path), base);
    Append(path, sizeof(path), ".log");
    const int log = ::open(path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (log >= 0) {
        SafeWrite(log, "Fatal signal ");
        SafeWrite(log, SignalName(sig));
        SafeWrite(log, " at address ");
        SafeWrite(log, address);
        SafeWrite(log, "\n");
#if defined(__GLIBC__)
        void* frames[64];
        const int count = ::backtrace(frames, 64);
        SafeWrite(log, "\nBacktrace:\n");
        ::backtrace_symbols_fd(frames, count, log);
#endif
        SafeWrite(log, kLogMarker.data(), kLogMarker.size());
        if (g_log_c[0] != '\0') {
            const int source = ::open(g_log_c, O_RDONLY);
            if (source >= 0) {
                const off_t end = ::lseek(source, 0, SEEK_END);
                const off_t start = end > static_cast<off_t>(kLogCopyBytes) ? end - static_cast<off_t>(kLogCopyBytes) : 0;
                ::lseek(source, start, SEEK_SET);
                char buffer[4096];
                ssize_t got;
                while ((got = ::read(source, buffer, sizeof(buffer))) > 0) {
                    SafeWrite(log, buffer, static_cast<std::size_t>(got));
                }
                ::close(source);
            }
        }
        ::close(log);
    }

    // The meta, written aside and renamed in, so a half-written one is never offered.
    char meta[4096] = {};
    Append(meta, sizeof(meta), g_meta_prefix);
    if (const u64 title = g_title.load(std::memory_order_relaxed); title != 0) {
        Append(meta, sizeof(meta), ",\"title_id\":\"");
        FormatNumber(number, title, 16, 16);
        Append(meta, sizeof(meta), number);
        Append(meta, sizeof(meta), "\"");
    }
    Append(meta, sizeof(meta), ",\"error_code\":\"");
    Append(meta, sizeof(meta), SignalName(sig));
    Append(meta, sizeof(meta), "\",\"message\":\"Fatal signal ");
    Append(meta, sizeof(meta), SignalName(sig));
    Append(meta, sizeof(meta), "\",\"fields\":{\"kind\":\"signal\",\"address\":\"");
    Append(meta, sizeof(meta), address);
    Append(meta, sizeof(meta), "\"}}");

    char temp[1300] = {};
    Append(temp, sizeof(temp), base);
    Append(temp, sizeof(temp), ".json.tmp");
    const int out = ::open(temp, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (out >= 0) {
        SafeWrite(out, meta);
        ::close(out);
        char final_path[1300] = {};
        Append(final_path, sizeof(final_path), base);
        Append(final_path, sizeof(final_path), ".json");
        ::rename(temp, final_path);
    }
}

void OnSignal(int sig, siginfo_t* info, void* /*context*/) {
    if (!g_recorded.exchange(true) && !g_never.load() && g_dir_c[0] != '\0') {
        WriteSignalReport(sig, info);
    }
    // Hand the signal to whoever had it before (the default: die, and dump core), and let it
    // be delivered again as soon as this returns.
    struct sigaction previous = g_previous[sig];
    if (previous.sa_handler == SIG_IGN) {
        previous.sa_handler = SIG_DFL;
    }
    ::sigaction(sig, &previous, nullptr);
    ::raise(sig);
}

void InstallSignalHandlers() {
#if defined(__GLIBC__)
    // backtrace() loads libgcc on first use; do it now, where allocating is allowed.
    void* warm[1];
    ::backtrace(warm, 1);
#endif
    for (const int sig : kSignals) {
        struct sigaction action{};
        action.sa_sigaction = &OnSignal;
        action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&action.sa_mask);
        ::sigaction(sig, &action, &g_previous[sig]);
    }
}

#endif // OPENPAK_CRASH_POSIX

void CopyInto(char* dst, std::size_t cap, const std::string& src) {
    const std::size_t n = (std::min)(src.size(), cap - 1);
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

} // namespace

void Install(Options options) {
    const fs::path dir = openpak::Platform::ConfigDir() / "crash-reports";
    std::error_code ec;
    fs::create_directories(dir, ec);
    {
        std::lock_guard lock{g_mutex};
        g_options = options;
        g_dir = dir;
    }
    g_never = Policy() == kNever;

    CopyInto(g_dir_c, sizeof(g_dir_c), dir.string());
    CopyInto(g_log_c, sizeof(g_log_c), options.log_file.string());
    // The meta minus its title, error and closing brace; a signal report finishes it by hand.
    std::string prefix = BuildMeta(options.source, options.version, OsName(), 0, {}, {}, {});
    prefix.pop_back();
    CopyInto(g_meta_prefix, sizeof(g_meta_prefix), prefix);

    g_installed = true;
    g_previous_terminate = std::set_terminate(&OnTerminate);
#ifdef OPENPAK_CRASH_POSIX
    if (options.catch_signals) {
        InstallSignalHandlers();
    }
#endif
}

std::string Policy() {
    const std::string stored = openpak::Platform::StripSpaces(openpak::Platform::ReadFile(Dir() / "policy"));
    return stored == kAlways || stored == kNever ? stored : std::string{kAsk};
}

void SetPolicy(std::string_view policy) {
    const std::string value{policy == kAlways || policy == kNever ? policy : kAsk};
    openpak::Platform::WriteFile(Dir() / "policy", value + "\n");
    g_never = value == kNever;
}

void SetTitleId(u64 title_id) {
    g_title.store(title_id, std::memory_order_relaxed);
}

void Record(std::string_view error_code, std::string_view message, std::string_view details,
            const std::map<std::string, std::string>& fields, u64 title_id) {
    // Not installed is a host with OpenPak switched off: upstream behaviour, nothing kept.
    if (!g_installed.load() || g_never.load()) {
        return;
    }
    try {
        const fs::path dir = Dir();
        std::error_code ec;
        fs::create_directories(dir, ec);
        Options options;
        {
            std::lock_guard lock{g_mutex};
            options = g_options;
        }
        const std::string name = NewName();
        std::string attachment{details};
        attachment += kLogMarker;
        if (!options.log_file.empty()) {
            attachment += TailLines(ReadTail(options.log_file, kLogCopyBytes), kTailLines);
        }
        openpak::Platform::WriteFile(dir / (name + ".log"), attachment);
        openpak::Platform::WriteFile(
            dir / (name + ".json.tmp"),
            BuildMeta(options.source.empty() ? "unknown" : options.source,
                      options.version.empty() ? "unknown" : options.version, OsName(),
                      title_id != 0 ? title_id : g_title.load(), error_code, message, fields));
        fs::rename(dir / (name + ".json.tmp"), dir / (name + ".json"), ec);
        Prune();
        OPENPAK_LOG_INFO("Crash report saved as {}; it is offered at the next launch", name);
    } catch (const std::exception& e) {
        OPENPAK_LOG_WARNING("Could not save a crash report: {}", e.what());
    }
}

std::vector<Report> Pending() {
    std::vector<Report> reports;
    std::error_code ec;
    const fs::path dir = Dir();
    if (!fs::is_directory(dir, ec)) {
        return reports;
    }
    std::vector<fs::path> metas;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.path().extension() == ".json") {
            metas.push_back(entry.path());
        }
    }
    std::sort(metas.begin(), metas.end());
    for (const auto& meta : metas) {
        Report report{meta, fs::path{meta}.replace_extension(".log"), {}};
        try {
            const auto json = nlohmann::json::parse(openpak::Platform::ReadFile(meta));
            report.message = json.value("message", std::string{});
        } catch (const std::exception&) {
            // Half-written by a process that died writing it: nothing to send.
            Discard(report);
            continue;
        }
        reports.push_back(std::move(report));
    }
    return reports;
}

void Discard(const Report& report) {
    std::error_code ec;
    fs::remove(report.meta_path, ec);
    fs::remove(report.attachment_path, ec);
}

void DiscardAll() {
    for (const auto& report : Pending()) {
        Discard(report);
    }
    // And whatever a crash left half-written.
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(Dir(), ec)) {
        if (entry.path().extension() == ".tmp") {
            fs::remove(entry.path(), ec);
        }
    }
}

bool Send(const Report& report) {
    const std::string meta = openpak::Platform::ReadFile(report.meta_path);
    if (meta.empty()) {
        return false;
    }
    std::string attachment = openpak::Platform::ReadFile(report.attachment_path);
    // A signal report copied a fixed number of bytes; what goes out is the last lines.
    if (const auto at = attachment.find(kLogMarker); at != std::string::npos) {
        const std::string log = TailLines(std::string_view{attachment}.substr(at + kLogMarker.size()), kTailLines);
        attachment.resize(at + kLogMarker.size());
        attachment += log;
    }
    if (attachment.size() > kMaxAttachmentBytes) {
        attachment.erase(0, attachment.size() - kMaxAttachmentBytes);
    }

    std::string boundary = "openpak-";
    std::mt19937_64 random{std::random_device{}()};
    AppendHex(boundary, random(), 16);
    AppendHex(boundary, random(), 16);

    const int status = WebService::OpenPakApi::PostCrashReport(
        BuildMultipart(boundary, meta, report.attachment_path.filename().string(), attachment),
        "multipart/form-data; boundary=" + boundary);
    if (status < 200 || status >= 300) {
        OPENPAK_LOG_WARNING("The crash report {} was not taken (HTTP {})",
                            report.meta_path.filename().string(), status);
        return false;
    }
    return true;
}

std::string OsName() {
#ifdef OPENPAK_CRASH_POSIX
    std::string name;
#ifdef __linux__
    std::ifstream release{"/etc/os-release"};
    for (std::string line; std::getline(release, line);) {
        if (line.rfind("PRETTY_NAME=", 0) == 0) {
            name = line.substr(12);
            name.erase(std::remove(name.begin(), name.end(), '"'), name.end());
            break;
        }
    }
#endif
    utsname info{};
    if (::uname(&info) == 0) {
        const std::string kernel = std::string{info.sysname} + " " + info.release + " (" + info.machine + ")";
        name = name.empty() ? kernel : name + ", " + kernel;
    }
    return name.empty() ? "unknown" : name;
#elif defined(_WIN32)
    return "Windows";
#else
    return "unknown";
#endif
}

std::string BuildMeta(std::string_view source, std::string_view version, std::string_view os,
                      u64 title_id, std::string_view error_code, std::string_view message,
                      const std::map<std::string, std::string>& fields) {
    // Insertion order kept, so the signal handler can finish a prefix of this by hand.
    nlohmann::ordered_json meta;
    meta["source"] = std::string{source};
    meta["version"] = std::string{version};
    meta["os"] = std::string{os};
    if (title_id != 0) {
        std::string hex;
        AppendHex(hex, title_id, 16);
        meta["title_id"] = hex;
    }
    if (!error_code.empty()) {
        meta["error_code"] = std::string{error_code};
    }
    if (!message.empty()) {
        meta["message"] = std::string{message.substr(0, 2000)};
    }
    if (!fields.empty()) {
        meta["fields"] = fields;
    }
    return meta.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

std::string TailLines(std::string_view text, std::size_t lines) {
    if (lines == 0) {
        return {};
    }
    std::size_t end = text.size();
    // A trailing newline ends the last line rather than starting an empty one.
    std::size_t at = end > 0 && text[end - 1] == '\n' ? end - 1 : end;
    std::size_t seen = 0;
    while (at > 0) {
        --at;
        if (text[at] == '\n' && ++seen == lines) {
            return std::string{text.substr(at + 1)};
        }
    }
    return std::string{text};
}

std::string BuildMultipart(std::string_view boundary, std::string_view meta,
                           std::string_view attachment_name, std::string_view attachment) {
    std::string body;
    body.reserve(meta.size() + attachment.size() + 512);
    body += "--";
    body += boundary;
    body += "\r\nContent-Disposition: form-data; name=\"meta\"\r\n"
            "Content-Type: application/json\r\n\r\n";
    body += meta;
    body += "\r\n";
    if (!attachment.empty()) {
        body += "--";
        body += boundary;
        body += "\r\nContent-Disposition: form-data; name=\"attachment\"; filename=\"";
        body += attachment_name.empty() ? std::string_view{"crash.log"} : attachment_name;
        body += "\"\r\nContent-Type: text/plain\r\n\r\n";
        body += attachment;
        body += "\r\n";
    }
    body += "--";
    body += boundary;
    body += "--\r\n";
    return body;
}

} // namespace openpak::CrashReports
