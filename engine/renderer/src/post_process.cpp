#include "post_process.h"

#include <algorithm>
#include <bit>

namespace foh {

u32 BloomLevelsFor(const RenderSettings& settings, u32 width, u32 height) {
    // Level i is (size >> (i + 1)); the smallest should keep a few texels or the last steps only blur a constant.
    const u32 smallest = std::max(1u, std::min(width, height));
    const u32 fit = static_cast<u32>(std::max(1, std::bit_width(smallest) - 1 - 2));
    return std::clamp(settings.bloomLevels, 1u, std::min(kMaxBloomLevels, fit));
}

void FillPostConstants(PostConstants& c, const RenderSettings& s, u32 width, u32 height, f32 dt, bool reset,
                       bool bloomActive) {
    c = PostConstants{};
    c.bloom = Vec4(bloomActive ? s.bloomStrength : 0.0f, 0.0f, 0.0f, 0.0f);
    c.exposure = Vec4(s.exposure, s.autoExposure ? 1.0f : 0.0f, s.exposureCompensation, reset ? 1.0f : 0.0f);
    // The histogram range must have a positive extent.
    const f32 lo = s.exposureMinLuminance;
    const f32 hi = std::max(s.exposureMaxLuminance, lo + 1.0f);
    c.adapt = Vec4(lo, hi, s.adaptBrighten, s.adaptDarken);
    // Percentiles: the darkest and brightest tenth of the pixels do not take part in the average.
    c.meter = Vec4(dt, 0.1f, 0.9f, s.meterCenterWeight);
    c.tonemap = Vec4(static_cast<f32>(s.tonemapper), s.whitePoint, 0.0f, 0.0f);
    c.size = Vec4(static_cast<f32>(width), static_cast<f32>(height), 0.0f, 0.0f);
}

bool PostProcess::init(nvrhi::IDevice* dev) {
    using L = nvrhi::BindingLayoutItem;
    auto make = [&](std::initializer_list<L> items) {
        nvrhi::BindingLayoutDesc ld;
        ld.visibility = nvrhi::ShaderType::All;
        for (const L& item : items) ld.addItem(item);
        return dev->createBindingLayout(ld);
    };
    m_bloomLayout = make({L::Texture_SRV(0), L::Sampler(0)});
    m_histogramLayout = make({L::VolatileConstantBuffer(0), L::Texture_SRV(0), L::Sampler(0), L::StructuredBuffer_UAV(0)});
    m_averageLayout = make({L::VolatileConstantBuffer(0), L::StructuredBuffer_SRV(1), L::StructuredBuffer_UAV(1)});
    m_tonemapLayout = make({L::VolatileConstantBuffer(0), L::VolatileConstantBuffer(1), L::Texture_SRV(0), L::Texture_SRV(1),
                            L::StructuredBuffer_SRV(2), L::Sampler(0)});

    nvrhi::SamplerDesc sd;
    sd.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
    m_sampler = dev->createSampler(sd);
    return m_bloomLayout && m_histogramLayout && m_averageLayout && m_tonemapLayout && m_sampler;
}

void PostProcess::shutdown() {
    m_bloomLayout = nullptr;
    m_histogramLayout = nullptr;
    m_averageLayout = nullptr;
    m_tonemapLayout = nullptr;
    m_sampler = nullptr;
}

} // namespace foh
