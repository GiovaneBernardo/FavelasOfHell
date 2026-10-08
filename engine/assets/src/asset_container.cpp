#include "foh/asset_container.h"

#include <lz4.h>
#include <zstd.h>

namespace foh {

size_t CompressBound(ChunkCodec codec, size_t srcSize) {
    switch (codec) {
        case ChunkCodec::None: return srcSize;
        case ChunkCodec::Lz4:  return static_cast<size_t>(LZ4_compressBound(static_cast<int>(srcSize)));
        case ChunkCodec::Zstd: return ZSTD_compressBound(srcSize);
    }
    return srcSize;
}

} // namespace foh
