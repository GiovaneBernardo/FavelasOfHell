#include "gtabr/gpu_format.h"

#include <nvrhi/nvrhi.h>

namespace gtabr {

namespace {
nvrhi::Format ToNvrhi(GpuFormat format) {
    switch (format) {
        case GpuFormat::Rgba8Unorm:  return nvrhi::Format::RGBA8_UNORM;
        case GpuFormat::Rgba16Float: return nvrhi::Format::RGBA16_FLOAT;
        case GpuFormat::Bc7Unorm:    return nvrhi::Format::BC7_UNORM;
    }
    return nvrhi::Format::UNKNOWN;
}
} // namespace

u32 BytesPerBlock(GpuFormat format) { return nvrhi::getFormatInfo(ToNvrhi(format)).bytesPerBlock; }

} // namespace gtabr
