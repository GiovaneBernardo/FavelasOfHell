#include "gtabr/dev_builder.h"

#include <algorithm>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "gtabr/log.h"

namespace gtabr {

namespace fs = std::filesystem;

namespace {
constexpr auto kScanInterval = std::chrono::milliseconds(250);
constexpr auto kSaveSettle = std::chrono::milliseconds(300);

bool IsSourceFile(const fs::path& p) {
    const std::string ext = p.extension().string();
    const std::string name = p.filename().string();
    return ext == ".cpp" || ext == ".cc" || ext == ".h" || ext == ".hpp" || ext == ".inl" ||
           ext == ".cmake" || name == "CMakeLists.txt";
}

std::string Quote(const fs::path& p) { return "\"" + fs::path(p).make_preferred().string() + "\""; }
} // namespace

DevBuilder::DevBuilder(DevBuilderConfig config) : m_config(std::move(config)) {
    std::error_code ec;
    m_logFile = fs::temp_directory_path(ec) / "gtabr_game_build.log";
    m_seenTime = latestSourceTime(); // do not rebuild at startup
    m_lastScan = Clock::now();
    GTABR_INFO("Dev builder: watching {} (target '{}')", m_config.sourceDir.string(), m_config.target);
}

fs::file_time_type DevBuilder::latestSourceTime() const {
    fs::file_time_type latest{};
    std::error_code ec;
    for (fs::recursive_directory_iterator it(m_config.sourceDir, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || !IsSourceFile(it->path())) continue;
        const auto t = it->last_write_time(ec);
        if (!ec) latest = std::max(latest, t);
    }
    return latest;
}

void DevBuilder::startBuild() {
    std::string command;
    if (!m_config.vcvarsBat.empty()) command += "call " + Quote(m_config.vcvarsBat) + " >nul 2>&1 && ";
    command += Quote(m_config.cmakeExe) + " --build " + Quote(m_config.buildDir) + " --target " + m_config.target;

    m_dirty = false;
    if (!m_process.start(command, m_config.buildDir, m_logFile)) {
        GTABR_ERROR("Dev builder: could not start the build command");
        return;
    }
    m_building = true;
    m_buildStart = Clock::now();
    GTABR_INFO("Dev builder: change detected, building '{}'...", m_config.target);
}

void DevBuilder::finishBuild() {
    m_building = false;
    const double seconds = std::chrono::duration<double>(Clock::now() - m_buildStart).count();
    if (m_process.exitCode() == 0) {
        GTABR_INFO("Dev builder: build succeeded in {:.1f}s", seconds);
        return;
    }
    GTABR_ERROR("Dev builder: build FAILED (exit {}), see {}", m_process.exitCode(), m_logFile.string());
    std::ifstream in(m_logFile);
    std::vector<std::string> errors;
    for (std::string line; std::getline(in, line);) {
        if (line.find("error") != std::string::npos) errors.push_back(line);
    }
    const size_t first = errors.size() > 15 ? errors.size() - 15 : 0;
    for (size_t i = first; i < errors.size(); ++i) GTABR_ERROR("  {}", errors[i]);
}

void DevBuilder::poll() {
    if (m_building && !m_process.isRunning()) finishBuild();

    const auto now = Clock::now();
    if (now - m_lastScan >= kScanInterval) {
        m_lastScan = now;
        const auto latest = latestSourceTime();
        if (latest != m_seenTime) {
            m_seenTime = latest;
            m_dirty = true;
            m_dirtySince = now;
        }
    }

    // Edits during a build are picked up by a follow-up build.
    if (m_dirty && !m_building && now - m_dirtySince >= kSaveSettle) startBuild();
}

} // namespace gtabr
