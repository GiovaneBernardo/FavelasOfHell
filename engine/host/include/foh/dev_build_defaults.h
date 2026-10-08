#pragma once

// Included by executables that host the game module. The FOH_DEV_* macros are
// defined by foh_enable_dev_builder() in CMake (never in Release builds).

#include "foh/dev_builder.h"

namespace foh {

inline DevBuilderConfig DefaultDevBuilderConfig() {
    DevBuilderConfig config;
#if defined(FOH_DEV_BUILD_DIR)
    config.sourceDir = FOH_DEV_GAME_SOURCE_DIR;
    config.buildDir = FOH_DEV_BUILD_DIR;
    config.cmakeExe = FOH_DEV_CMAKE;
    config.vcvarsBat = FOH_DEV_VCVARS; // may be ""
    config.target = FOH_DEV_GAME_TARGET;
#endif
    return config; // invalid (empty) when dev building is disabled
}

// Rebuilds the shader binaries when a .slang/.slangh file under shaders/ is saved;
// the renderer's ShaderLibrary then picks the new binaries up by itself.
inline DevBuilderConfig DefaultShaderBuilderConfig() {
    DevBuilderConfig config;
#if defined(FOH_DEV_BUILD_DIR) && defined(FOH_DEV_SHADER_TARGET)
    config.sourceDir = FOH_DEV_SHADER_SOURCE_DIR;
    config.buildDir = FOH_DEV_BUILD_DIR;
    config.cmakeExe = FOH_DEV_CMAKE;
    config.vcvarsBat = FOH_DEV_VCVARS;
    config.target = FOH_DEV_SHADER_TARGET;
    config.extensions = {".slang", ".slangh"};
#endif
    return config;
}

} // namespace foh
