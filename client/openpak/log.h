// SPDX-License-Identifier: GPL-2.0-or-later
// Logging: the host installs a sink; until then messages go to stderr.
#pragma once
#include <fmt/format.h>
#include <functional>
#include <string>
namespace openpak {
enum class LogLevel { Trace, Debug, Info, Warning, Error, Critical };
using LogSink = std::function<void(LogLevel, const std::string&)>;
void SetLogSink(LogSink sink);
void Log(LogLevel level, const std::string& message);
} // namespace openpak
#define OPENPAK_LOG_TRACE(...) ::openpak::Log(::openpak::LogLevel::Trace, fmt::format(__VA_ARGS__))
#define OPENPAK_LOG_DEBUG(...) ::openpak::Log(::openpak::LogLevel::Debug, fmt::format(__VA_ARGS__))
#define OPENPAK_LOG_INFO(...) ::openpak::Log(::openpak::LogLevel::Info, fmt::format(__VA_ARGS__))
#define OPENPAK_LOG_WARNING(...) ::openpak::Log(::openpak::LogLevel::Warning, fmt::format(__VA_ARGS__))
#define OPENPAK_LOG_ERROR(...) ::openpak::Log(::openpak::LogLevel::Error, fmt::format(__VA_ARGS__))
#define OPENPAK_LOG_CRITICAL(...) ::openpak::Log(::openpak::LogLevel::Critical, fmt::format(__VA_ARGS__))
