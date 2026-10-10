#pragma once

// Private state of foh::Renderer, shared by renderer.cpp (lifecycle, scenes,
// viewports) and viewport_graph.cpp (the per-viewport render graph).

#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <nvrhi/nvrhi.h>

#include "foh/jobs.h"
#include "foh/renderer/renderer.h"
#include "foh/rhi/graphics_device.h"
#include "foh/rhi/pipeline_cache.h"
#include "foh/rhi/render_graph.h"
#include "foh/rhi/render_graph_helpers.h"
#include "foh/rhi/shader_library.h"
#include "foh/rhi/swapchain.h"
#include "atmosphere.h"
#include "clouds.h"
#include "gpu_structs.h"
#include "material_store.h"
#include "mesh_store.h"
#include "post_process.h"
#include "texture_store.h"
#include "water.h"

namespace foh {

// A scene's instances packed for the GPU, shared by every viewport that shows it.
struct SceneGpu {
    struct Batch {
        u32 mesh;
        u32 first; // first instance in `instances`
        u32 count;
        i32 skinBase = -1; // >= 0: a skinned character, drawn from the skinned vertex buffer starting at this vertex
        Vec4 sphere{0.0f}; // xyz centre, w radius: a static instance set, culled per viewport and cascade (w <= 0: always drawn)
    };
    // One mesh block to deform (mirrored by SkinJob in shaders/skin.slang). Every job repeats the job count, which the
    // shader needs for its search and which saves a constant buffer.
    struct SkinJob {
        u32 srcVertex, vertexCount, dstVertex, palette;
        u32 firstGroup, jobCount, pad0, pad1;
    };
    std::vector<GpuInstance> instances; // sorted by (draw class, mesh)
    // One instanced draw per mesh and draw class; classBegin[c]..classBegin[c + 1] are the batches of class c.
    std::vector<Batch> batches;
    u32 classBegin[kDrawClassCount + 1] = {};
    // Skinned characters (ADR 0009): the pre-skinning pass deforms `skinJobs` into one vertex buffer; the batches with
    // skinBase >= 0 draw from it. `palettes` are the skinning matrices of all characters, back to back.
    std::vector<SkinJob> skinJobs;
    std::vector<SkinMatrix> palettes;
    u32 skinnedVertexCount = 0;
    u32 skinGroups = 0;
    std::vector<Batch> classScratch;                    // prepareScene scratch
    std::unordered_map<u64, u32> skinDestination;       // (block, palette) -> first output vertex, shares work between submeshes
    std::vector<u32> counts, cursor;    // counting-sort scratch, one slot per (class, mesh) (kept to avoid per-frame allocation)
};

struct SceneSlot {
    u32 id = 0;
    std::string name;
    std::unique_ptr<RenderScene> scene;
    SceneGpu gpu;
    bool inUse = false; // some active viewport shows it this frame
};

// Everything a pass body reads. Lives in the Viewport, so its address is stable
// for the frame and pass lambdas can capture a pointer to it.
struct ViewFrame {
    ViewConstants constants{};
    ShadowCascades cascades{};
    bool shadowsOn = false;
    u32 shadowTile = 0; // texels per cascade side
    const SceneGpu* scene = nullptr;
    CloudConstants clouds{};
    nvrhi::IBuffer* cloudCb = nullptr;
    WaterConstants water{};
    nvrhi::IBuffer* waterCb = nullptr;
    PostConstants post{};
    nvrhi::IBuffer* postCb = nullptr;
    nvrhi::IBuffer* exposureState = nullptr;
    u32 bloomLevels = 0; // 0 = bloom off for this frame
    u32 waterRings = 0; // clipmap rings (levels) this view draws
    nvrhi::IBuffer* viewCb = nullptr;
    nvrhi::IBuffer* shadowCb = nullptr;
    u32 width = 0;
    u32 height = 0;
    ViewportFlags flags = ViewportFlags::None;
    // Picking (see Renderer::requestPick): set only on the frame the id pass runs.
    u32 pickX = 0, pickY = 0;
    nvrhi::IStagingTexture* pickStaging = nullptr;
};

struct Viewport {
    u32 id = 0;
    std::string name;
    u32 width = 1280, height = 720;
    u32 pendingWidth = 1280, pendingHeight = 720;
    SceneId scene;
    u64 cameraEntity = 0;
    ViewportFlags flags = ViewportFlags::Sky;
    bool active = true;
    bool hasCameraOverride = false;
    RenderViewParams cameraOverride;

    nvrhi::TextureHandle output; // RGBA8, sRGB-encoded; ShaderResource between frames
    // Outputs replaced by a resize stay alive for one more frame: the UI built before
    // beginFrame() still holds raw pointers to them in this frame's draw data.
    std::vector<nvrhi::TextureHandle> retired;
    nvrhi::BufferHandle viewCb;  // volatile constant buffer
    nvrhi::BufferHandle shadowCb; // volatile: the view-projection of the cascade being drawn
    std::unique_ptr<RenderGraph> graph;
    ViewFrame frame;
    bool graphReady = false; // compiled this frame and waiting to be recorded/submitted

    // Clouds: temporal history (half resolution, ping-pong), created on first use and when the size changes.
    nvrhi::BufferHandle cloudCb; // volatile
    nvrhi::TextureHandle cloudHistory[2];
    u32 cloudHistoryWidth = 0, cloudHistoryHeight = 0;
    u32 cloudWrite = 0; // index of the history texture written this frame
    bool cloudHistoryValid = false;
    u64 cloudLastFrame = 0;
    Mat4 cloudPrevViewProj{1.0f};

    nvrhi::BufferHandle waterCb; // volatile; created on first use

    // Post-processing: constants for the exposure / bloom / tonemap passes, and the adapted exposure (it persists across
    // frames, so every viewport adapts on its own). `exposureValid` false = the next frame snaps to the target.
    nvrhi::BufferHandle postCb; // volatile
    nvrhi::BufferHandle exposureState;
    bool exposureValid = false;
    u64 exposureLastFrame = 0;
    f32 exposureLastTime = 0.0f;

    ViewportDebugInfo debug;

    enum class PickState : u8 { Idle, Requested, InFlight, Ready };
    PickState pickState = PickState::Idle;
    u32 pickX = 0, pickY = 0;
    u64 pickSubmittedFrame = 0;
    u64 pickResult = 0;
    nvrhi::StagingTextureHandle pickStaging;
};

// Shared immutable GPU objects.
struct GpuResources {
    nvrhi::BindingLayoutHandle layoutView;       // b0
    nvrhi::BindingLayoutHandle layoutViewTexture; // b0 + t0
    nvrhi::BindingLayoutHandle layoutTexture;     // t0
    nvrhi::BindingLayoutHandle layoutDeferred;    // b0 + t0..t4 (G-buffer, depth, shadow atlas) + t5..t7 (atmosphere) + s0, s1
    nvrhi::BindingLayoutHandle layoutSky;         // b0 + t0 (depth) + t1..t3 (atmosphere) + s0
    nvrhi::BindingLayoutHandle layoutMaterial;    // b0 + t0 (material buffer) + s0, for the passes that sample materials
    nvrhi::BindingLayoutHandle layoutSkin;        // t0 jobs, t1 source vertices, t2 influences, t3 palettes, u0 skinned vertices
    nvrhi::BindingLayoutHandle layoutBindless;    // the unbounded texture table, register space 1
    nvrhi::SamplerHandle materialSampler;         // anisotropic wrap
    nvrhi::SamplerHandle shadowSampler;           // hardware PCF compare, see deferred.slang
    nvrhi::InputLayoutHandle meshInput;
    MeshStore meshes;
    TextureStore textures;
    MaterialStore materials;
};

struct Renderer::Impl {
    RendererDesc desc;
    RenderSettings settings; // what the passes read (starts from `desc`)
    std::unique_ptr<GraphicsDevice> device;
    std::unique_ptr<ShaderLibrary> shaders;
    std::unique_ptr<PipelineCache> pipelines;
    GpuResources gpu;
    Clouds clouds;
    Water water;
    PostProcess post;
    Atmosphere atmosphere; // after `device`/`pipelines`/`shaders`: it must be destroyed before them
    MeshLookup meshLookup; // imported mesh asset -> mesh ids in gpu.meshes
    // Replaced/released mesh ids stay valid for a few frames: scene snapshots already extracted
    // (and frames in flight) may still reference them.
    struct RetiredMesh {
        u64 frame;
        u32 id;
    };
    std::vector<RetiredMesh> retiredMeshes;
    std::unique_ptr<RenderGraph> windowGraph;

    std::vector<SceneSlot> scenes;
    std::vector<std::unique_ptr<Viewport>> viewports; // creation order = submit order
    u32 nextId = 1;
    u64 frame = 0;

    // Per-frame job argument scratch (stable addresses while jobs run; no per-frame allocation).
    struct SceneJob {
        Impl* impl;
        SceneSlot* slot;
    };
    struct RecordJobArg {
        Viewport* vp;
        nvrhi::IDevice* dev;
    };
    std::vector<SceneJob> sceneJobs;
    std::vector<RecordJobArg> recordJobs;
    std::vector<Viewport*> readyViewports;
    std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();

    RgShaderServices services() const { return {shaders.get(), pipelines.get()}; }

    Viewport* findViewport(ViewportId id);
    const Viewport* findViewport(ViewportId id) const;
    SceneSlot* findScene(SceneId id);

    void createViewportTargets(Viewport& vp);
    void prepareScene(SceneSlot& slot);
    // Creates every mesh pipeline variant up front so the first masked/double-sided material does not stall a frame.
    void warmPipelines();
    // Resolve camera + constants, build and compile the viewport's graph (main thread).
    void buildViewportGraph(Viewport& vp);
};

} // namespace foh
