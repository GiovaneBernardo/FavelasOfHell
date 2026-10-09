#pragma once

// Precomputed atmospheric scattering (Bruneton & Neyret 2008; reference implementation 2017): the GPU lookup
// tables, their generation with compute shaders (shaders/atmosphere_precompute.slang) and the cache file.
//
// Three tables are enough to render the sky, aerial perspective and the sun/sky light for any camera altitude
// and sun position, with a few texture fetches per pixel and no ray marching:
//   transmittance  256 x 64          RGBA16F   light surviving the trip to the top of the atmosphere
//   scattering     256 x 128 x 32    RGBA16F   in-scattered radiance (Rayleigh rgb + single Mie in alpha)
//   irradiance     64 x 16           RGBA16F   sky light reaching the ground
// ~8.2 MiB of GPU memory in total. The tables are created once and live as long as the renderer; a (re)generation
// overwrites their contents in place, in queue order, so frames already submitted keep seeing the old data.
//
// Thread-safety: main thread only (the cache file is written by a background job that touches no GPU object).

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include <nvrhi/nvrhi.h>

#include "foh/jobs.h"
#include "foh/renderer/renderer.h"
#include "foh/rhi/pipeline_cache.h"
#include "foh/rhi/shader_library.h"
#include "gpu_structs.h"

namespace foh {

class Atmosphere {
public:
    Atmosphere();
    ~Atmosphere();
    Atmosphere(const Atmosphere&) = delete;
    Atmosphere& operator=(const Atmosphere&) = delete;

    // Creates the (empty) tables, then loads `cacheFile` or queues a generation of the default atmosphere.
    bool init(nvrhi::IDevice* device, ShaderLibrary* shaders, PipelineCache* pipelines, JobSystem* jobs,
              std::filesystem::path cacheFile);
    // Waits for the background save and releases the GPU objects.
    void shutdown();

    void request(const AtmosphereDesc& desc);
    // Once per frame, before anything samples the tables: starts a queued generation, finishes a running one.
    void pump();

    // True once the tables hold valid data (from the moment a generation is submitted: the GPU queue is in order).
    bool ready() const { return m_ready; }
    // Writes the atmosphere constants of ViewConstants (atmosphere1.y = 0 while not ready).
    void fillView(ViewConstants& c) const;

    nvrhi::ITexture* transmittance() const { return m_transmittance; }
    nvrhi::ITexture* scattering() const { return m_scattering; }
    nvrhi::ITexture* irradiance() const { return m_irradiance; }
    nvrhi::ISampler* sampler() const { return m_sampler; }

    const AtmosphereDesc& desc() const { return m_active; }
    AtmosphereStatus status() const;

private:
    struct SaveJob;
    struct Pack; // GPU -> CPU dump of the finished tables

    bool load();
    bool generate(const AtmosphereDesc& desc);
    bool beginPack(); // after a generation: copy the tables to readback buffers
    void finishPack();
    void createDummies();

    nvrhi::IDevice* m_device = nullptr;
    ShaderLibrary* m_shaders = nullptr;
    PipelineCache* m_pipelines = nullptr;
    JobSystem* m_jobs = nullptr;
    std::filesystem::path m_cacheFile;

    nvrhi::TextureHandle m_transmittance, m_scattering, m_irradiance;
    nvrhi::SamplerHandle m_sampler;
    nvrhi::BindingLayoutHandle m_layoutPrecompute, m_layoutPack;
    // Stand-ins for the slots a pass does not use (a binding set must be complete and each slot needs a texture of
    // the dimension the shader declares).
    nvrhi::TextureHandle m_dummySrv[8], m_dummyUav[5], m_dummyPackSrv3D;

    AtmosphereDesc m_active; // what the tables in use were generated with
    bool m_ready = false;
    bool m_fromCache = false;

    bool m_hasRequest = false;
    AtmosphereDesc m_requested;

    enum class State : u8 { Idle, Reading, Saving };
    State m_state = State::Idle;
    std::unique_ptr<Pack> m_pack;
    std::unique_ptr<SaveJob> m_save;
    std::chrono::steady_clock::time_point m_genStart;

    f32 m_lastGenerateMs = 0.0f;
    u64 m_cacheBytes = 0;
    std::string m_message;
};

} // namespace foh
