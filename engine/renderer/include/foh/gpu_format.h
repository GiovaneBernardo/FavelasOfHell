#pragma once

// Placeholder for the renderer module. NVRHI is a PRIVATE dependency: public
// renderer headers expose engine types only, never NVRHI types.

#include "foh/platform.h"
#include "foh/types.h"

namespace foh {

enum class GpuFormat : u8 { Rgba8Unorm, Rgba16Float, Bc7Unorm };

FOH_API u32 BytesPerBlock(GpuFormat format);

} // namespace foh
