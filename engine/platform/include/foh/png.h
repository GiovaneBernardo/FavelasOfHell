#pragma once

#include <filesystem>

#include "foh/platform.h"
#include "foh/types.h"

namespace foh {

// Minimal PNG encoder (RGBA8 in, opaque RGB out, stored/uncompressed deflate).
// For screenshots in smoke runs and bug reports; not for shipping data.
FOH_API bool WritePng(const std::filesystem::path& path, const u8* rgba, u32 width, u32 height, u32 rowPitch);

} // namespace foh
