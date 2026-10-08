#pragma once

// The ABI between the host (editor / runtime) and a hot-reloadable game module.
//
// A game module is a DLL that exports exactly one C symbol, foh_get_game_module,
// returning a table of plain function pointers. Declare it with FOH_GAME_MODULE.
//
// Rules for game modules (see AGENTS.md):
//   * no mutable global/static state: state lives in the ECS world / resources
//   * everything the module registers (systems, later components/resources) is
//     removed by the host before the DLL is unloaded
//   * never pass module-owned pointers or vtables to engine-owned storage

#include "foh/ecs.h"
#include "foh/platform.h"

namespace foh {

inline constexpr u32 kGameModuleAbiVersion = 1;

struct GameModuleApi {
    u32 abiVersion;
    const char* name;
    // Register plugins/systems. Called after (re)load, with kGameOwner active.
    void (*build)(App& app);
    // Called before the module is unloaded (flush anything referencing module code).
    void (*teardown)(App& app);
};

using GetGameModuleFn = const GameModuleApi* (*)();

} // namespace foh

#define FOH_GAME_MODULE(moduleName, buildFn, teardownFn)                                      \
    FOH_MODULE_EXPORT const ::foh::GameModuleApi* foh_get_game_module() {                 \
        static const ::foh::GameModuleApi api{::foh::kGameModuleAbiVersion, moduleName,     \
                                                buildFn, teardownFn};                           \
        return &api;                                                                            \
    }
