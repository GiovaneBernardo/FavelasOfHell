#pragma once

// Placeholder for the asset system (see README, "Assets"). For now: the stable
// asset identity and the codec enum of the chunked container format.

#include <cstddef>

#include "gtabr/platform.h"
#include "gtabr/types.h"

namespace gtabr {

struct AssetId {
    u64 hi = 0;
    u64 lo = 0;
    bool operator==(const AssetId&) const = default;
    bool isValid() const { return (hi | lo) != 0; }
};

enum class ChunkCodec : u8 { None, Lz4, Zstd };

// Worst-case compressed size for a payload (used to size output buffers).
GTABR_API size_t CompressBound(ChunkCodec codec, size_t srcSize);

} // namespace gtabr
