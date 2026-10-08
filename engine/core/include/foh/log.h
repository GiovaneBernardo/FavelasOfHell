#pragma once

#include <format>
#include <string_view>
#include <utility>

#include "foh/platform.h"

namespace foh {

enum class LogLevel { Trace, Debug, Info, Warn, Error, Fatal };

FOH_API void LogMessage(LogLevel level, std::string_view message);
FOH_API void SetMinLogLevel(LogLevel level);

template <typename... Args>
void Log(LogLevel level, std::format_string<Args...> fmt, Args&&... args) {
    LogMessage(level, std::format(fmt, std::forward<Args>(args)...));
}

} // namespace foh

#define FOH_TRACE(...) ::foh::Log(::foh::LogLevel::Trace, __VA_ARGS__)
#define FOH_DEBUG(...) ::foh::Log(::foh::LogLevel::Debug, __VA_ARGS__)
#define FOH_INFO(...)  ::foh::Log(::foh::LogLevel::Info,  __VA_ARGS__)
#define FOH_WARN(...)  ::foh::Log(::foh::LogLevel::Warn,  __VA_ARGS__)
#define FOH_ERROR(...) ::foh::Log(::foh::LogLevel::Error, __VA_ARGS__)
#define FOH_FATAL(...) ::foh::Log(::foh::LogLevel::Fatal, __VA_ARGS__)
