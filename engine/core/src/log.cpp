#include "foh/log.h"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>

#if defined(_WIN32)
    #include <windows.h>
#endif

namespace foh {

namespace {
std::atomic<LogLevel> g_minLevel{LogLevel::Trace};
std::mutex g_mutex;

const char* LevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
    }
    return "?????";
}
} // namespace

void SetMinLogLevel(LogLevel level) { g_minLevel.store(level); }

void LogMessage(LogLevel level, std::string_view message) {
    if (level < g_minLevel.load()) return;
    const std::string line = std::format("[{}] {}\n", LevelName(level), message);
    std::lock_guard lock(g_mutex);
    std::FILE* out = level >= LogLevel::Warn ? stderr : stdout;
    std::fputs(line.c_str(), out);
    std::fflush(out); // logs must survive a crash or a killed process
#if defined(_WIN32)
    OutputDebugStringA(line.c_str());
#endif
}

} // namespace foh
