#pragma once

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include <nvrhi/nvrhi.h>

#include "foh/platform.h"
#include "foh/types.h"

namespace foh {

// A shader as seen by pipeline users. `uid` changes whenever the shader is
// reloaded, so pipeline cache keys that include it rebuild automatically.
struct ShaderRef {
    nvrhi::ShaderHandle shader;
    u64 uid = 0;
    explicit operator bool() const { return shader != nullptr; }
};

// Loads compiled shader binaries (DXIL / SPIR-V, produced by slangc at build
// time, see shaders/CMakeLists.txt) and hot reloads them when the files change.
//
// A shader is addressed as "<file>.<entry>", e.g. "mesh_forward.vsMesh"; the
// binary lives at <dir>/<file>.<entry>.dxil (or .spv). The stage comes from the
// caller because the binary does not say.
//
// Thread-safety: get() may be called from any thread (pass bodies do). pollReload()
// must run on the main thread while no graph is executing.
class FOH_API ShaderLibrary {
public:
    ShaderLibrary(nvrhi::IDevice* device, nvrhi::GraphicsAPI api, std::filesystem::path directory);
    ~ShaderLibrary();
    ShaderLibrary(const ShaderLibrary&) = delete;
    ShaderLibrary& operator=(const ShaderLibrary&) = delete;

    // Empty ref (and one logged error per file) when the binary is missing/invalid.
    ShaderRef get(std::string_view name, nvrhi::ShaderType type);

    // Reloads binaries that changed on disk since they were loaded. Cheap: scans
    // at most a few times per second. Returns true if any shader was replaced.
    bool pollReload();

    // Bumps whenever any shader was reloaded.
    u64 version() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace foh
