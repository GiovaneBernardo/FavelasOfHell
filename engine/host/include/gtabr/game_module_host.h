#pragma once

#include <chrono>
#include <filesystem>

#include "gtabr/dynamic_library.h"
#include "gtabr/ecs.h"
#include "gtabr/game_module.h"
#include "gtabr/platform.h"

namespace gtabr {

// Loads the game DLL from a "shadow" copy so the build output is never locked,
// polls the original for changes, and reloads it in place:
//
//   new build detected (DLL and PDB both unchanged for a settle time)
//     -> copy dll+pdb to <shadowDir>/<generation>/ and load the copy
//     -> old module: teardown(), remove its systems, unload
//     -> new module: build() with kGameOwner active
//
// The PDB is copied next to the DLL copy (the DLL embeds only the PDB file name,
// see /PDBALTPATH) so a debugger attached to the host finds symbols for every
// generation and breakpoints re-bind after each reload. A missing or half-written
// PDB is treated as "build not finished yet" and retried, never loaded without
// symbols while a PDB exists.
//
// TODO (hot-reload spike): serialize the ECS world through reflection before
// unloading and restore it afterwards, so state survives a reload.
class GTABR_API GameModuleHost {
public:
    GameModuleHost(App& app, std::filesystem::path sourceDll, std::filesystem::path shadowDir);
    ~GameModuleHost();
    GameModuleHost(const GameModuleHost&) = delete;
    GameModuleHost& operator=(const GameModuleHost&) = delete;

    bool load();
    // Call once per frame. Returns true if a reload happened this call.
    bool pollReload();
    void unload();

    bool isLoaded() const { return m_api != nullptr; }
    u32 generation() const { return m_generation; }

private:
    // Identity of a build output: DLL and PDB timestamps and sizes.
    struct BuildStamp {
        std::filesystem::file_time_type dllTime{};
        std::filesystem::file_time_type pdbTime{};
        std::uintmax_t dllSize = 0;
        std::uintmax_t pdbSize = 0;
        bool operator==(const BuildStamp&) const = default;
    };
    enum class LoadResult { Ok, Retry, Failed };

    BuildStamp readStamp() const;
    LoadResult loadShadowCopy(DynamicLibrary& lib, const GameModuleApi*& api, std::filesystem::path& dir);
    bool loadInternal(LoadResult& result);

    App& m_app;
    std::filesystem::path m_sourceDll;
    std::filesystem::path m_sourcePdb;
    std::filesystem::path m_shadowDir;
    std::filesystem::path m_currentDir;
    DynamicLibrary m_lib;
    const GameModuleApi* m_api = nullptr;
    u32 m_generation = 0;

    BuildStamp m_loadedStamp{};
    BuildStamp m_pendingStamp{};
    std::chrono::steady_clock::time_point m_pendingSince{};
    bool m_hasPending = false;
};

} // namespace gtabr
