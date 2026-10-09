#pragma once

// Post-processing shared state: auto exposure, bloom and the tonemap. The passes themselves are in viewport_graph.cpp,
// the shaders in shaders/exposure.slang, bloom.slang and tonemap.slang. See docs/adr/0010-post-processing.md.
//
// Cost model (1080p): bloom is `levels` downsample + (levels - 1) upsample fullscreen passes on halved targets
// (R11G11B10, ~0.1-0.2 ms in total on an RTX 2060-class GPU, estimated); the exposure histogram reads the HDR image once
// at half resolution (a bilinear tap per 2x2 block) plus one single-thread dispatch.
//
// Thread-safety: init/shutdown on the main thread; the accessors only hand out immutable objects.

#include <nvrhi/nvrhi.h>

#include "foh/math.h"
#include "foh/renderer/renderer.h"
#include "foh/types.h"

namespace foh {

inline constexpr u32 kHistogramBins = 256;
inline constexpr u32 kMaxBloomLevels = 8;

// CPU mirror of PostConstants in shaders/common/post.slangh.
struct PostConstants {
    Vec4 bloom, exposure, adapt, meter, tonemap, size;
};
static_assert(sizeof(PostConstants) == 6 * 16, "PostConstants must match post.slangh");

// Number of downsample steps for a viewport: as requested, but never so many that the smallest level is below 4 texels.
u32 BloomLevelsFor(const RenderSettings& settings, u32 width, u32 height);

// `dt` seconds since the viewport's last frame; `reset` snaps the exposure to its target (first frame, camera cut).
void FillPostConstants(PostConstants& c, const RenderSettings& settings, u32 width, u32 height, f32 dt, bool reset,
                       bool bloomActive);

class PostProcess {
public:
    bool init(nvrhi::IDevice* device);
    void shutdown();

    // t0 source + s0 bilinear clamp (bloom down/upsample).
    nvrhi::IBindingLayout* bloomLayout() const { return m_bloomLayout; }
    // b0 post constants, t0 HDR, s0, u0 histogram bins (structured uint).
    nvrhi::IBindingLayout* histogramLayout() const { return m_histogramLayout; }
    // b0 post constants, t1 histogram, u1 exposure state (structured float4).
    nvrhi::IBindingLayout* averageLayout() const { return m_averageLayout; }
    // b0 view, b1 post, t0 HDR, t1 bloom, t2 exposure state, s0 bilinear clamp.
    nvrhi::IBindingLayout* tonemapLayout() const { return m_tonemapLayout; }
    nvrhi::ISampler* sampler() const { return m_sampler; }

private:
    nvrhi::BindingLayoutHandle m_bloomLayout, m_histogramLayout, m_averageLayout, m_tonemapLayout;
    nvrhi::SamplerHandle m_sampler;
};

} // namespace foh
