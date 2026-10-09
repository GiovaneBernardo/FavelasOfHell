#pragma once

// Volumetric clouds: the GPU-side assets (three tileable noise volumes generated once at start-up), the binding
// layouts of the cloud passes and the per-view constants. The passes themselves live in viewport_graph.cpp
// (CloudTrace -> CloudResolve -> CloudComposite) and the shaders in shaders/clouds*.slang.
//
// Cost model (see the shaders): a half-resolution ray march through a thin slab, one jittered sample per pixel,
// accumulated over time; only sky pixels are marched. The noise volumes are 128^3 + 32^3 RGBA8 + 256^2 RGBA8
// (~9.4 MiB), created once and never touched again.
//
// Thread-safety: main thread only (the pass bodies only read the objects returned by the accessors).

#include <chrono>

#include <nvrhi/nvrhi.h>

#include "foh/renderer/renderer.h"
#include "foh/rhi/pipeline_cache.h"
#include "foh/rhi/shader_library.h"
#include "gpu_structs.h"

namespace foh {

// CPU mirror of CloudConstants in shaders/common/clouds.slangh.
struct CloudConstants {
    Mat4 prevViewProj;
    Vec4 layer;
    Vec4 weather; // x = coverage, y = type (0 stratus .. 1 cumulus), z = storminess, w = cross-fade between the two weather layers
    Vec4 noise;
    Vec4 wind;
    Vec4 misc;
    Vec4 march;
};
static_assert(sizeof(CloudConstants) == 64 + 6 * 16, "CloudConstants must match clouds.slangh");

class Clouds {
public:
    bool init(nvrhi::IDevice* device, ShaderLibrary* shaders, PipelineCache* pipelines);
    void shutdown();

    // Once per frame: generates the noise volumes as soon as their shaders are available, and advances the weather
    // (eases toward the target preset, starts the next one when the cycle timer runs out).
    void pump();
    bool ready() const { return m_ready; }

    void set(const CloudDesc& desc);
    const CloudDesc& desc() const { return m_desc; }
    CloudWeather state() const;

    // Fills the constants for a view. `time` in seconds, `historyWeight` = share of the new sample (1 = no history).
    void fill(CloudConstants& c, f32 time, u64 frame, f32 historyWeight) const;

    nvrhi::ITexture* shape() const { return m_shape; }
    nvrhi::ITexture* detail() const { return m_detail; }
    nvrhi::ITexture* weatherMap() const { return m_weatherTex; }
    nvrhi::ISampler* wrapSampler() const { return m_wrap; }
    // b0 view, b1 clouds, t0 depth, t1..t3 atmosphere, t4 shape, t5 detail, t6 weather, s0 atmosphere sampler, s1 wrap.
    nvrhi::IBindingLayout* traceLayout() const { return m_traceLayout; }
    // b0 view, b1 clouds, t0..t1 two 2D textures, s0 bilinear clamp (resolve and composite).
    nvrhi::IBindingLayout* postLayout() const { return m_postLayout; }

private:
    bool generate();
    void advanceWeather();

    nvrhi::IDevice* m_device = nullptr;
    ShaderLibrary* m_shaders = nullptr;
    PipelineCache* m_pipelines = nullptr;
    nvrhi::TextureHandle m_shape, m_detail, m_weatherTex;
    nvrhi::SamplerHandle m_wrap;
    nvrhi::BindingLayoutHandle m_traceLayout, m_postLayout, m_noiseLayout;
    CloudDesc m_desc;
    bool m_ready = false;

    // Weather: what is shown eases toward the target preset; the target is the desc preset or, with autoCycle, the
    // cycle's current pick.
    CloudWeather m_weather;
    CloudPreset m_lastDescPreset = CloudPreset::Fair;
    CloudPreset m_cyclePreset = CloudPreset::Fair;
    f32 m_cycleTimer = 0.0f;
    u32 m_cycleStep = 0;
    bool m_weatherInited = false;
    std::chrono::steady_clock::time_point m_lastPump = std::chrono::steady_clock::now();
};

} // namespace foh
