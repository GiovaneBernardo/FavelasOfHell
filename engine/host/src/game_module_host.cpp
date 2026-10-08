#include "gtabr/game_module_host.h"

#include <system_error>
#include <utility>

#include "gtabr/log.h"

namespace gtabr {

namespace fs = std::filesystem;

namespace {
// Time the DLL and PDB must stay untouched before we reload: the linker and
// mspdbsrv can still be writing for a moment after the DLL timestamp changes.
constexpr auto kSettleTime = std::chrono::milliseconds(500);
} // namespace

GameModuleHost::GameModuleHost(App& app, fs::path sourceDll, fs::path shadowDir)
    : m_app(app), m_sourceDll(std::move(sourceDll)), m_shadowDir(std::move(shadowDir)) {
    m_sourcePdb = m_sourceDll;
    m_sourcePdb.replace_extension(".pdb");
    std::error_code ec;
    fs::remove_all(m_shadowDir, ec); // leftovers from a previous session
}

GameModuleHost::~GameModuleHost() { unload(); }

GameModuleHost::BuildStamp GameModuleHost::readStamp() const {
    BuildStamp stamp;
    std::error_code ec;
    stamp.dllTime = fs::last_write_time(m_sourceDll, ec);
    stamp.dllSize = ec ? 0 : fs::file_size(m_sourceDll, ec);
    stamp.pdbTime = fs::last_write_time(m_sourcePdb, ec);
    stamp.pdbSize = ec ? 0 : fs::file_size(m_sourcePdb, ec);
    return stamp;
}

GameModuleHost::LoadResult GameModuleHost::loadShadowCopy(DynamicLibrary& lib, const GameModuleApi*& api,
                                                           fs::path& dir) {
    std::error_code ec;
    dir = m_shadowDir / std::to_string(m_generation + 1);
    fs::create_directories(dir, ec);

    const fs::path dllCopy = dir / m_sourceDll.filename();
    fs::copy_file(m_sourceDll, dllCopy, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        GTABR_WARN("Game module: cannot copy {} ({}), will retry", m_sourceDll.string(), ec.message());
        return LoadResult::Retry;
    }

    if (fs::exists(m_sourcePdb, ec)) {
        fs::copy_file(m_sourcePdb, dir / m_sourcePdb.filename(), fs::copy_options::overwrite_existing, ec);
        if (ec) {
            GTABR_WARN("Game module: cannot copy {} ({}), will retry", m_sourcePdb.string(), ec.message());
            return LoadResult::Retry;
        }
    } else {
        GTABR_WARN("Game module: no PDB next to {}, debugging this build will not work",
                   m_sourceDll.string());
    }

    if (!lib.load(dllCopy)) {
        GTABR_ERROR("Game module: failed to load {}", dllCopy.string());
        return LoadResult::Failed;
    }
    auto getModule = reinterpret_cast<GetGameModuleFn>(lib.symbol("gtabr_get_game_module"));
    if (!getModule) {
        GTABR_ERROR("Game module: missing export gtabr_get_game_module");
        return LoadResult::Failed;
    }
    api = getModule();
    if (!api || api->abiVersion != kGameModuleAbiVersion) {
        GTABR_ERROR("Game module: ABI mismatch (host {}, module {})", kGameModuleAbiVersion,
                    api ? api->abiVersion : 0u);
        api = nullptr;
        return LoadResult::Failed;
    }
    return LoadResult::Ok;
}

bool GameModuleHost::loadInternal(LoadResult& result) {
    // Snapshot first: if the files change while we copy, the next poll sees a
    // different stamp and reloads again.
    const BuildStamp stamp = readStamp();

    DynamicLibrary lib;
    const GameModuleApi* api = nullptr;
    fs::path dir;
    result = loadShadowCopy(lib, api, dir);
    if (result != LoadResult::Ok) {
        lib.unload();
        std::error_code ec;
        fs::remove_all(dir, ec);
        return false;
    }

    // Swap: tear down the old module (if any) only after the new one loaded fine.
    if (m_api) {
        if (m_api->teardown) m_api->teardown(m_app);
        m_app.removeSystemsOwnedBy(kGameOwner);
        m_lib.unload();
        std::error_code ec;
        fs::remove_all(m_currentDir, ec);
    }
    m_lib = std::move(lib);
    m_api = api;
    m_currentDir = std::move(dir);
    ++m_generation;
    m_loadedStamp = stamp;
    m_hasPending = false;

    m_app.setActiveOwner(kGameOwner);
    m_api->build(m_app);
    m_app.setActiveOwner(kEngineOwner);
    GTABR_INFO("Game module '{}' loaded (generation {}, pdb {} bytes)", m_api->name, m_generation,
               stamp.pdbSize);
    return true;
}

bool GameModuleHost::load() {
    LoadResult result{};
    return loadInternal(result);
}

bool GameModuleHost::pollReload() {
    const BuildStamp stamp = readStamp();
    if (stamp.dllSize == 0) return false; // mid-link: DLL briefly missing/empty
    if (stamp == m_loadedStamp) {
        m_hasPending = false;
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    if (!m_hasPending || !(stamp == m_pendingStamp)) {
        m_hasPending = true;
        m_pendingStamp = stamp;
        m_pendingSince = now;
        return false;
    }
    if (now - m_pendingSince < kSettleTime) return false;

    GTABR_INFO("Game module changed on disk, reloading...");
    LoadResult result{};
    if (loadInternal(result)) return true;

    if (result == LoadResult::Retry) {
        m_pendingSince = now; // files still busy: wait another settle period
    } else {
        // Broken build: keep running the old module and do not retry until the
        // next build changes the files.
        m_loadedStamp = stamp;
        m_hasPending = false;
    }
    return false;
}

void GameModuleHost::unload() {
    if (!m_api) return;
    if (m_api->teardown) m_api->teardown(m_app);
    m_app.removeSystemsOwnedBy(kGameOwner);
    m_lib.unload();
    m_api = nullptr;
    std::error_code ec;
    fs::remove_all(m_currentDir, ec);
}

} // namespace gtabr
