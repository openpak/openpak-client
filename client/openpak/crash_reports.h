// SPDX-License-Identifier: GPL-2.0-or-later
// Crash reports for openpak.org (POST /api/v1/crash-reports: a `meta` JSON part and the log as an
// `attachment`), the port of Ryujinx's OpenPakCrashReports.
//
// Nothing is sent from a crash. A report is only written to <config>/crash-reports; the next
// launch offers what it finds (Pending) and sends it only when the person says so, or has said
// "always" before (Policy). "never" stops them being written at all.
//
// What is caught, once Install has run: an unhandled C++ exception (std::terminate), a fatal
// signal on POSIX (SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT), and whatever the host records itself
// through Record -- the guest's fatal:u, most of all. A signal handler may do almost nothing, so
// everything a signal report needs (paths, version, OS) is prepared at Install, the running title
// is an atomic, and the log is the tail of the host's own log file, copied with read/write.
#pragma once
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>
#include "openpak/types.h"

namespace openpak::CrashReports {

inline constexpr std::string_view kAsk = "ask";
inline constexpr std::string_view kAlways = "always";
inline constexpr std::string_view kNever = "never";

// The site refuses an attachment over this.
inline constexpr std::size_t kMaxAttachmentBytes = 20u * 1024u * 1024u;
// How much of the log goes with a report: its last lines, up to this many.
inline constexpr std::size_t kTailLines = 500;
// Reports kept waiting for an answer; the oldest go first.
inline constexpr std::size_t kMaxPending = 5;

struct Options {
    std::string source;              // "eden", "citron": the contract's `source`
    std::string version;             // the emulator build
    std::filesystem::path log_file;  // the host's log file, whose tail goes with a report
    bool catch_signals = true;       // install the fatal-signal handlers (POSIX only)
};

// Once, early, after Platform::SetDirectories and before any emulation starts: the JIT's own
// SIGSEGV handler (fastmem) has to be installed after this one, so that it sees its faults
// first and hands on only the real ones.
void Install(Options options);

// "ask" (the default), "always" or "never"; kept in <config>/crash-reports/policy.
std::string Policy();
void SetPolicy(std::string_view policy);

// The running title, 0 when none is. Safe to call from any thread.
void SetTitleId(u64 title_id);

// Save a report now, from ordinary code (not a signal handler). Never throws; does nothing
// before Install.
void Record(std::string_view error_code, std::string_view message, std::string_view details,
            const std::map<std::string, std::string>& fields = {}, u64 title_id = 0);

struct Report {
    std::filesystem::path meta_path;
    std::filesystem::path attachment_path;
    std::string message;
};

// Reports waiting for an answer, oldest first.
std::vector<Report> Pending();
void Discard(const Report& report);
void DiscardAll();

// Send one report, with the bearer when there is one. True when the site took it.
bool Send(const Report& report);

// The pieces, for tests and for anyone building a report by hand.
std::string OsName();
std::string BuildMeta(std::string_view source, std::string_view version, std::string_view os,
                      u64 title_id, std::string_view error_code, std::string_view message,
                      const std::map<std::string, std::string>& fields);
// The last `lines` lines of text.
std::string TailLines(std::string_view text, std::size_t lines);
// A multipart/form-data body with `meta` and, when not empty, `attachment`.
std::string BuildMultipart(std::string_view boundary, std::string_view meta,
                           std::string_view attachment_name, std::string_view attachment);

} // namespace openpak::CrashReports
