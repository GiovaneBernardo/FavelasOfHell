#pragma once

// Placeholder for the renderer module. NVRHI is a PRIVATE dependency: public
// renderer headers expose engine types only, never NVRHI types.

#include "gtabr/platform.h"
#include "gtabr/types.h"

namespace gtabr {

enum class GpuFormat : u8 { Rgba8Unorm, Rgba16Float, Bc7Unorm };

GTABR_API u32 BytesPerBlock(GpuFormat format);

} // namespace gtabr
