#pragma once

#include <chrono>
#include <filesystem>
#include <string>

#include "gtabr/platform.h"
#include "gtabr/process.h"

namespace gtabr {

struct DevBuilderConfig {
    std::filesystem::path sourceDir; // watched recursively (the game/ directory)
    std::filesystem::path buildDir;  // CMake build directory
    std::filesystem::path cmakeExe;
    std::filesystem::path vcvarsBat; // optional (MSVC environment); empty = inherit environment
    std::string target;              // CMake target to build, e.g. gtabr_game
    bool isValid() const { return !sourceDir.empty() && !buildDir.empty() && !cmakeExe.empty() && !target.empty(); }
};

// Development-only: watches the game sources and rebuilds the game module when
// a file is saved. Runs the build as a child process, so it works while the host
// is running under a debugger (where the IDE's own Build command is disabled).
// GameModuleHost then notices the new DLL and reloads it.
class GTABR_API DevBuilder {
public:
    explicit DevBuilder(DevBuilderConfig config);

    // Call once per frame; cheap (scans at most a few times per second).
    void poll();
    bool isBuilding() { return m_process.isRunning(); }

private:
    using Clock = std::chrono::steady_clock;
    std::filesystem::file_time_type latestSourceTime() const;
    void startBuild();
    void finishBuild();

    DevBuilderConfig m_config;
    Process m_process;
    std::filesystem::path m_logFile;
    std::filesystem::file_time_type m_seenTime{};
    Clock::time_point m_lastScan{};
    Clock::time_point m_dirtySince{};
    Clock::time_point m_buildStart{};
    bool m_dirty = false;
    bool m_building = false;
};

} // namespace gtabr
