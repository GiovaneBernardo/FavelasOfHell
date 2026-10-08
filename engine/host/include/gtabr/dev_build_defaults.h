#pragma once

// Included by executables that host the game module. The GTABR_DEV_* macros are
// defined by gtabr_enable_dev_builder() in CMake (never in Release builds).

#include "gtabr/dev_builder.h"

namespace gtabr {

inline DevBuilderConfig DefaultDevBuilderConfig() {
    DevBuilderConfig config;
#if defined(GTABR_DEV_BUILD_DIR)
    config.sourceDir = GTABR_DEV_GAME_SOURCE_DIR;
    config.buildDir = GTABR_DEV_BUILD_DIR;
    config.cmakeExe = GTABR_DEV_CMAKE;
    config.vcvarsBat = GTABR_DEV_VCVARS; // may be ""
    config.target = GTABR_DEV_GAME_TARGET;
#endif
    return config; // invalid (empty) when dev building is disabled
}

} // namespace gtabr
