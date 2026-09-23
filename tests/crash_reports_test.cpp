// SPDX-License-Identifier: GPL-2.0-or-later
// Crash reports (openpak/crash_reports.h): what the meta and the body say, that a report waits on
// disk until somebody answers, and -- on Linux -- that a real fatal signal leaves one behind.
// The same cases as Ryujinx's OpenPakCrashReportsTests.cs, plus the signal.
#undef NDEBUG
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "openpak/crash_reports.h"
#include "openpak/platform.h"

#ifdef __linux__
#include <sys/prctl.h>
#include <sys/wait.h>
#endif
#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

namespace cr = openpak::CrashReports;
namespace fs = std::filesystem;

int main() {
    // The meta carries the contract's fields and leaves out empty ones.
    {
        const auto meta = nlohmann::json::parse(cr::BuildMeta(
            "eden", "0.1.2", "Linux (x86_64)", 0x0100000000010000ULL, "2001-0123", "boom",
            {{"kind", "guest_fatal"}}));
        assert(meta["source"] == "eden");
        assert(meta["version"] == "0.1.2");
        assert(meta["os"] == "Linux (x86_64)");
        assert(meta["title_id"] == "0100000000010000");
        assert(meta["error_code"] == "2001-0123");
        assert(meta["message"] == "boom");
        assert(meta["fields"]["kind"] == "guest_fatal");

        const auto bare = nlohmann::json::parse(cr::BuildMeta("eden", "1", "os", 0, {}, {}, {}));
        assert(!bare.contains("title_id") && !bare.contains("error_code") &&
               !bare.contains("message") && !bare.contains("fields"));
        // Strings that need escaping stay valid JSON.
        const auto quoted = nlohmann::json::parse(cr::BuildMeta("eden", "1", "os", 0, {}, "a \"b\"\n", {}));
        assert(quoted["message"] == "a \"b\"\n");
    }

    // The last lines, and the whole text when it is shorter.
    assert(cr::TailLines("a\nb\nc\n", 2) == "b\nc\n");
    assert(cr::TailLines("a\nb\nc", 2) == "b\nc");
    assert(cr::TailLines("a\nb\n", 5) == "a\nb\n");

    // The body: meta first, then the attachment, then the closing boundary.
    {
        const std::string body = cr::BuildMultipart("XYZ", "{}", "crash.log", "log");
        assert(body.find("--XYZ\r\nContent-Disposition: form-data; name=\"meta\"") == 0);
        assert(body.find("name=\"attachment\"; filename=\"crash.log\"") != std::string::npos);
        assert(body.find("{}") < body.find("log\r\n--XYZ--\r\n"));
        assert(body.ends_with("--XYZ--\r\n"));
        assert(cr::BuildMultipart("XYZ", "{}", "x", "").find("attachment") == std::string::npos);
    }

    const fs::path root = fs::temp_directory_path() / ("openpak-crash-test-" + std::to_string(::getpid()));
    fs::create_directories(root);
    openpak::Platform::SetDirectories(root, root / "cache");
    const fs::path log_file = root / "emu.log";
    {
        std::string log;
        for (int i = 0; i < 1000; ++i) {
            log += "line " + std::to_string(i) + "\n";
        }
        openpak::Platform::WriteFile(log_file, log);
    }

    // A report waits on disk, carries the log's last lines, and only the newest few are kept.
    {
        cr::Install({"eden", "0.1.2", log_file, false});
        assert(cr::Policy() == cr::kAsk);
        cr::SetTitleId(0x0100000000010000ULL);
        for (int i = 0; i < 7; ++i) {
            cr::Record("E", "crash " + std::to_string(i), "details");
        }
        const auto reports = cr::Pending();
        assert(reports.size() == cr::kMaxPending);
        assert(reports.back().message == "crash 6");
        const auto meta = nlohmann::json::parse(openpak::Platform::ReadFile(reports.back().meta_path));
        assert(meta["title_id"] == "0100000000010000");
        const std::string attachment = openpak::Platform::ReadFile(reports.back().attachment_path);
        assert(attachment.starts_with("details"));
        assert(attachment.find("line 999\n") != std::string::npos);
        assert(attachment.find("line 499\n") == std::string::npos);
        cr::DiscardAll();
        assert(cr::Pending().empty());
    }

    // "never" means nothing is saved, and the answer is kept.
    {
        cr::SetPolicy(cr::kNever);
        cr::Record("E", "crash", "details");
        assert(cr::Pending().empty());
        assert(cr::Policy() == cr::kNever);
        cr::SetPolicy(cr::kAsk);
    }

#ifdef __linux__
    // A real SIGSEGV in a child leaves a report the parent can read, and the child still dies of it.
    {
        const pid_t child = ::fork();
        if (child == 0) {
            // No core dump for a crash this test causes on purpose.
            ::prctl(PR_SET_DUMPABLE, 0);
            cr::Install({"eden", "0.1.2", log_file, true});
            cr::SetTitleId(0x0100000000020000ULL);
            volatile int* nothing = nullptr;
            *nothing = 1;
            ::_exit(0);
        }
        int status = 0;
        ::waitpid(child, &status, 0);
        assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
        const auto reports = cr::Pending();
        assert(reports.size() == 1);
        const auto meta = nlohmann::json::parse(openpak::Platform::ReadFile(reports[0].meta_path));
        assert(meta["source"] == "eden");
        assert(meta["error_code"] == "SIGSEGV");
        assert(meta["title_id"] == "0100000000020000");
        assert(meta["fields"]["kind"] == "signal");
        const std::string attachment = openpak::Platform::ReadFile(reports[0].attachment_path);
        assert(attachment.starts_with("Fatal signal SIGSEGV"));
        assert(attachment.find("line 999\n") != std::string::npos);
        cr::DiscardAll();
    }
#endif

    fs::remove_all(root);
    return 0;
}
