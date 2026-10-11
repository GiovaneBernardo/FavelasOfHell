#pragma once

// Water: the wave simulation (FFT cascades, run once per frame on its own command list), the GPU objects the water
// passes bind, and the per-view constants. The passes themselves are in viewport_graph.cpp (SkyProbe -> SceneCopy ->
// WaterSurface) and the shaders in shaders/water*.slang and shaders/sky_probe.slang. See docs/adr/0009-water.md.
//
// Cost model: the simulation is 4 + (mip levels) compute dispatches per frame, independent of the number of viewports
// (about 0.3 ms at 3 x 256^2 on an RTX 2060-class GPU, estimated); the surface is ~60k triangles from a vertex-less
// clipmap, and the pixel shader only runs where the water is visible (it tests the scene depth first).
//
// Thread-safety: main thread only (the pass bodies only read the objects returned by the accessors).

#include <chrono>
#include <vector>

#include <nvrhi/nvrhi.h>

#include "foh/renderer/renderer.h"
#include "foh/rhi/pipeline_cache.h"
#include "foh/rhi/shader_library.h"

namespace foh {

inline constexpr u32 kWaterCascades = 3;
inline constexpr u32 kWaterGerstnerWaves = 6;
inline constexpr u32 kWaterGridCells = 64;  // quads per side of one clipmap ring
inline constexpr u32 kWaterHoleLo = 17;     // the hole in a ring: quads [lo, hi) in both directions. A little smaller than
inline constexpr u32 kWaterHoleHi = 47;     // half the grid, so neighbouring rings overlap and never leave a crack.
inline constexpr u32 kWaterProbeSize = 128; // texels per side of the sky probe (kWaterProbeSize in water.slangh)

// CPU mirror of WaterConstants in shaders/common/water.slangh.
struct WaterConstants {
    Vec4 level, cascade, grid, absorb, body, foam, bed, bedParams, reflection, misc;
    Vec4 gerstner[kWaterGerstnerWaves];
};
static_assert(sizeof(WaterConstants) == 16 * 16, "WaterConstants must match water.slangh");

// Index list of one ring of the clipmap grid ((kWaterGridCells + 1)^2 vertices, 16-bit indices). `ring` leaves out the
// quads of the hole in the middle (they belong to the next finer ring); otherwise the grid is complete.
std::vector<u16> BuildWaterGridIndices(bool ring);
// Number of rings (including the innermost) needed to reach `farZ` metres with a level-0 cell of `cell` metres.
u32 WaterRingLevels(f32 cell, f32 farZ);

class Water {
public:
    bool init(nvrhi::IDevice* device, ShaderLibrary* shaders, PipelineCache* pipelines);
    void shutdown();

    void set(const WaterDesc& desc);
    const WaterDesc& desc() const { return m_desc; }
    bool setBed(const WaterBedDesc& bed);
    void clearBed();

    // True when the water can be drawn this frame (enabled, GPU objects built).
    bool active() const { return m_desc.enabled && m_gridReady; }

    // Once per frame, before the viewport graphs are built: advances the waves (own command list, executed at once).
    // `wanted` = some active viewport draws the sea; otherwise only the clock runs.
    void simulate(bool wanted);

    // Constants for a view. `farZ` sizes the clipmap, `cloudAltitude` is the middle of the cloud layer (reflection parallax).
    void fill(WaterConstants& c, u64 frame, f32 cloudAltitude) const;
    // How many clipmap rings a view with this far plane needs.
    u32 ringLevels(f32 farZ) const;

    bool fft() const { return m_desc.quality > 0; }
    nvrhi::ITexture* displacement() const { return m_disp; }
    nvrhi::ITexture* slope() const { return m_slope[m_current]; }
    nvrhi::ITexture* bed() const { return m_bed; }
    // The ground height field the terrain feeds (WaterBedDesc): CPU lookups for the light probes and the GPU mapping
    // (xy = origin, zw = 1 / size). `groundAt` clamps to the edge of the field.
    bool hasBed() const { return !m_bedData.empty(); }
    f32 groundAt(f32 x, f32 z) const;
    Vec4 bedMapping() const { return Vec4(m_bedOrigin.x, m_bedOrigin.y, 1.0f / m_bedSize.x, 1.0f / m_bedSize.y); }
    nvrhi::ISampler* waveSampler() const { return m_waveSampler; }
    nvrhi::IBuffer* ringIndices() const { return m_ringIb; }
    nvrhi::IBuffer* centerIndices() const { return m_centerIb; }
    u32 ringIndexCount() const { return m_ringIndexCount; }
    u32 centerIndexCount() const { return m_centerIndexCount; }
    // b0 view, b1 water, t0 depth, t1 scene copy, t2 sky probe, t3 displacement, t4 slope, t5 sea floor,
    // t6..t8 atmosphere, s0 wave sampler, s1 bilinear clamp.
    nvrhi::IBindingLayout* surfaceLayout() const { return m_surfaceLayout; }

private:
    bool createSimResources();
    void releaseSimResources();
    void uploadBed();
    void dispatch(const ShaderRef& shader, nvrhi::IBindingLayout* layout, nvrhi::IBindingSet* set, u32 gx, u32 gy, u32 gz);
    u32 resolution() const { return m_desc.quality >= 2 ? 256u : 128u; }
    f32 tileLength(u32 cascade) const;

    nvrhi::IDevice* m_device = nullptr;
    ShaderLibrary* m_shaders = nullptr;
    PipelineCache* m_pipelines = nullptr;
    WaterDesc m_desc;
    bool m_gridReady = false;

    // Static: the clipmap index lists, samplers and the surface layout.
    nvrhi::BufferHandle m_ringIb, m_centerIb;
    u32 m_ringIndexCount = 0, m_centerIndexCount = 0;
    nvrhi::SamplerHandle m_waveSampler;
    nvrhi::BindingLayoutHandle m_surfaceLayout;

    // Simulation state, (re)built when the resolution changes.
    u32 m_simN = 0;
    nvrhi::BindingLayoutHandle m_lSpectrum, m_lEvolve, m_lFft, m_lFinalize, m_lMip;
    nvrhi::BufferHandle m_simCb;
    nvrhi::CommandListHandle m_cmd;
    nvrhi::TextureHandle m_h0, m_fftA, m_fftB, m_disp, m_slope[2];
    nvrhi::BindingSetHandle m_sSpectrum, m_sEvolve, m_sFftH, m_sFftV;
    nvrhi::BindingSetHandle m_sFinalize[2];
    std::vector<nvrhi::BindingSetHandle> m_sMip[2]; // [parity][level]: mip level -> level + 1
    u32 m_mipLevels = 0;
    u32 m_current = 0; // which slope texture holds this frame's result
    bool m_spectrumDirty = true;

    // Sea floor height field.
    nvrhi::TextureHandle m_bed;
    std::vector<f32> m_bedData; // kept until uploaded
    u32 m_bedWidth = 0, m_bedHeight = 0;
    Vec2 m_bedOrigin{0.0f}, m_bedSize{1.0f};
    bool m_bedDirty = false;

    std::chrono::steady_clock::time_point m_start = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point m_lastSim = std::chrono::steady_clock::now();
    double m_time = 0.0; // seconds since start, advanced by simulate()
    f32 m_dt = 0.016f;
};

} // namespace foh
