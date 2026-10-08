#pragma once

#include <filesystem>

#include "foh/platform.h"

namespace foh {

// Directory containing the running executable (where the engine and game DLLs live).
FOH_API std::filesystem::path GetExecutableDirectory();

} // namespace foh
