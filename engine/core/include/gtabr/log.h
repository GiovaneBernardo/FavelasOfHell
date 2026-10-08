#pragma once

#include <format>
#include <string_view>
#include <utility>

#include "gtabr/platform.h"

namespace gtabr {

enum class LogLevel { Trace, Debug, Info, Warn, Error, Fatal };

GTABR_API void LogMessage(LogLevel level, std::string_view message);
GTABR_API void SetMinLogLevel(LogLevel level);

template <typename... Args>
void Log(LogLevel level, std::format_string<Args...> fmt, Args&&... args) {
    LogMessage(level, std::format(fmt, std::forward<Args>(args)...));
}

} // namespace gtabr

#define GTABR_TRACE(...) ::gtabr::Log(::gtabr::LogLevel::Trace, __VA_ARGS__)
#define GTABR_DEBUG(...) ::gtabr::Log(::gtabr::LogLevel::Debug, __VA_ARGS__)
#define GTABR_INFO(...)  ::gtabr::Log(::gtabr::LogLevel::Info,  __VA_ARGS__)
#define GTABR_WARN(...)  ::gtabr::Log(::gtabr::LogLevel::Warn,  __VA_ARGS__)
#define GTABR_ERROR(...) ::gtabr::Log(::gtabr::LogLevel::Error, __VA_ARGS__)
#define GTABR_FATAL(...) ::gtabr::Log(::gtabr::LogLevel::Fatal, __VA_ARGS__)
