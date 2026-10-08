#pragma once

#include <filesystem>
#include <string>

#include "gtabr/platform.h"

namespace gtabr {

// A child process with stdout/stderr redirected to a log file. Non-blocking:
// start() returns immediately, poll isRunning().
class GTABR_API Process {
public:
    Process() = default;
    ~Process();
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    // `commandLine` is run through the platform shell (cmd.exe /c, /bin/sh -c).
    bool start(const std::string& commandLine, const std::filesystem::path& workingDir,
               const std::filesystem::path& logFile);
    bool isRunning();
    int exitCode() const { return m_exitCode; }

private:
    void closeHandle();
    void* m_handle = nullptr; // HANDLE on Windows
    long m_pid = 0;           // pid on POSIX
    bool m_started = false;
    int m_exitCode = -1;
};

} // namespace gtabr
