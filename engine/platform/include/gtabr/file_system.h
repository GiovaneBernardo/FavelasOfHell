#pragma once

#include <filesystem>

#include "gtabr/platform.h"

namespace gtabr {

// Directory containing the running executable (where the engine and game DLLs live).
GTABR_API std::filesystem::path GetExecutableDirectory();

} // namespace gtabr
