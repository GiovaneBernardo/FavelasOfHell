#pragma once

// Render graph (RHI tier, see graphics_device.h for the header-tier rule).
//
// One RenderGraph instance exists per render target chain (one per viewport, one
// per OS window). Each frame:
//
//     graph.reset(frame);                       // keeps pools and caches
//     graph.import...(); graph.addPass(...);    // declare, main thread
//     graph.compile(device);                    // cull, allocate, plan barriers
//     graph.execute(device);                    // record (parallel) + submit
//
// Passes run in declaration order; the graph never reorders them. What it adds
// on top of a flat list of passes:
//
//   * culling: a pass whose outputs nobody uses (and that is not NeverCull) is
//     dropped, together with the transient resources only it touched
//   * transient resources: created from a description, backed by a pool that
//     survives frames (no per-frame GPU allocations once warm) and reused by
//     later resources of equal description whose lifetimes do not overlap
//   * barriers: derived from the declared RgAccess of every pass and issued
//     through NVRHI's explicit state API, including across command lists
//   * parallel recording: the live passes are split into contiguous groups, one
//     NVRHI command list each, recorded on the job system and submitted in order
//
// Thread-safety: building, compile() and the execute() call itself are main
// thread. Inside execute(), pass bodies of DIFFERENT groups run concurrently:
// a pass body may only touch the RgPassContext, immutable per-frame data and
// thread-safe services (ShaderLibrary, PipelineCache). It must declare every
// resource it touches (ctx.texture()/buffer() only hand out declared ones).

#include <cstddef>
#include <memory>
#include <new>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <nvrhi/nvrhi.h>

#include "foh/platform.h"
#include "foh/types.h"

namespace foh {

class JobSystem;
class RenderGraph;
class RgPassBuilder;
class RgPassContext;

inline constexpr u32 kRgInvalid = 0xFFFFFFFFu;
inline constexpr u32 kRgMaxColorTargets = 8;

struct RgTexture {
    u32 index = kRgInvalid;
    constexpr bool valid() const { return index != kRgInvalid; }
    friend constexpr bool operator==(RgTexture, RgTexture) = default;
};
struct RgBuffer {
    u32 index = kRgInvalid;
    constexpr bool valid() const { return index != kRgInvalid; }
    friend constexpr bool operator==(RgBuffer, RgBuffer) = default;
};

// How a pass uses a resource. This single declaration drives barriers, culling
// and lifetimes; the pass body must then really use it that way.
enum class RgAccess : u8 {
    SampledRead,  // shader resource view (texture sample / structured buffer read)
    StorageRead,  // UAV, read only use
    StorageWrite, // UAV
    ColorTarget,  // render target (blending / load-op preserve counts as read+write)
    DepthWrite,
    DepthRead,
    CopySrc,
    CopyDst,
    VertexRead,
    IndexRead,
    ConstantRead,
    IndirectRead,
};

constexpr bool IsWriteAccess(RgAccess a) {
    return a == RgAccess::StorageWrite || a == RgAccess::ColorTarget || a == RgAccess::DepthWrite ||
           a == RgAccess::CopyDst;
}
FOH_API nvrhi::ResourceStates ToResourceStates(RgAccess access);

// Description of a transient texture. `name` must be a string literal (or
// otherwise outlive the frame): it is stored by pointer, not copied.
struct RgTextureDesc {
    u32 width = 1;
    u32 height = 1;
    u32 depth = 1;
    u32 arraySize = 1;
    u32 mipLevels = 1;
    u32 sampleCount = 1;
    nvrhi::Format format = nvrhi::Format::UNKNOWN;
    nvrhi::TextureDimension dimension = nvrhi::TextureDimension::Texture2D;
    bool renderTarget = false; // color or depth attachment (depth is implied by the format)
    bool storage = false;      // UAV
    nvrhi::Color clearValue{};
    bool useClearValue = false;
    const char* name = "rg_texture";

    // Equality for pool matching: everything except the name.
    bool sameShape(const RgTextureDesc& o) const;
};

struct RgBufferDesc {
    u64 byteSize = 0;
    u32 structStride = 0; // > 0: structured buffer
    bool storage = false; // UAV
    bool vertex = false;
    bool index = false;
    bool indirect = false;
    const char* name = "rg_buffer";

    bool sameShape(const RgBufferDesc& o) const;
};

enum class RgPassFlags : u8 {
    None = 0,
    // Keep the pass even if none of its outputs is consumed (presenting, debug
    // overlays, readbacks).
    NeverCull = 1,
};
constexpr RgPassFlags operator|(RgPassFlags a, RgPassFlags b) {
    return static_cast<RgPassFlags>(static_cast<u8>(a) | static_cast<u8>(b));
}

// ---- pass context --------------------------------------------------------------

// Handed to a pass body while recording. Valid only during that call.
class FOH_API RgPassContext {
public:
    nvrhi::ICommandList* cmd() const { return m_cmd; }
    nvrhi::IDevice* device() const { return m_device; }
    // Index of the command list group being recorded (0..groupCount-1).
    u32 group() const { return m_group; }

    // Physical resources behind the handles. Asserts the pass declared them.
    nvrhi::ITexture* texture(RgTexture handle) const;
    nvrhi::IBuffer* buffer(RgBuffer handle) const;

    // Framebuffer built from the attachments declared with colorTarget()/depthTarget().
    nvrhi::IFramebuffer* framebuffer() const { return m_framebuffer; }

    // Cached binding set for (desc, layout). The cache is per group, so this is
    // lock free and safe to call from pass bodies on any thread.
    nvrhi::IBindingSet* bindingSet(const nvrhi::BindingSetDesc& desc, nvrhi::IBindingLayout* layout) const;

    u64 frameIndex() const;

private:
    friend class RenderGraph;
    RgPassContext() = default;
    RenderGraph* m_graph = nullptr;
    nvrhi::ICommandList* m_cmd = nullptr;
    nvrhi::IDevice* m_device = nullptr;
    nvrhi::IFramebuffer* m_framebuffer = nullptr;
    u32 m_group = 0;
    u32 m_pass = 0;
};

// ---- pass builder ---------------------------------------------------------------

// Passed to the setup lambda of addPass. Everything a pass touches is declared
// here, once.
class FOH_API RgPassBuilder {
public:
    RgTexture createTexture(const RgTextureDesc& desc);
    RgBuffer createBuffer(const RgBufferDesc& desc);

    RgTexture read(RgTexture texture, RgAccess access = RgAccess::SampledRead);
    RgBuffer read(RgBuffer buffer, RgAccess access = RgAccess::SampledRead);
    RgTexture write(RgTexture texture, RgAccess access);
    RgBuffer write(RgBuffer buffer, RgAccess access);

    // Declares a render target / depth attachment AND the matching access, and
    // lets ctx.framebuffer() return a framebuffer made of them. The pass clears
    // or loads as it likes in its body.
    RgTexture colorTarget(u32 slot, RgTexture texture);
    RgTexture depthTarget(RgTexture texture, bool readOnly = false);

    void neverCull();

private:
    friend class RenderGraph;
    RgPassBuilder(RenderGraph& graph, u32 pass) : m_graph(&graph), m_pass(pass) {}
    RenderGraph* m_graph;
    u32 m_pass;
};

// ---- graph ------------------------------------------------------------------------

// Creates the GPU resources for transient graph resources. The default
// implementation uses an nvrhi::IDevice; tests plug in a fake.
class RgResourceFactory {
public:
    virtual ~RgResourceFactory() = default;
    virtual nvrhi::TextureHandle createTexture(const RgTextureDesc& desc) = 0;
    virtual nvrhi::BufferHandle createBuffer(const RgBufferDesc& desc) = 0;
};

struct RenderGraphConfig {
    JobSystem* jobs = nullptr;    // null: record every group on the calling thread
    u32 maxRecordingGroups = 4;   // upper bound of command lists / concurrent recorders
    u32 minPassesPerGroup = 4;    // below this a graph is recorded as ONE group (no job overhead)
    bool debugMarkers = true;     // one PIX/RenderDoc marker per pass
    u32 poolRetireFrames = 4;     // free pooled resources unused this many frames
};

// Per-pass info for the render graph debugger panel (valid after compile()).
struct RgPassInfo {
    const char* name;
    bool culled;
    u32 group;       // command list group (kRgInvalid if culled)
    u32 accessCount;
};

struct RgStats {
    u32 passCount = 0;
    u32 culledPasses = 0;
    u32 groups = 0;
    u32 transientTextures = 0;   // virtual
    u32 physicalTextures = 0;    // actually backing them this frame (after reuse)
    u32 pooledTextures = 0;      // alive in the pool
    u32 transientBuffers = 0;
    u32 physicalBuffers = 0;
    u32 pooledBuffers = 0;
    u32 barrierTransitions = 0;  // explicit state changes planned
};

class FOH_API RenderGraph {
public:
    explicit RenderGraph(const RenderGraphConfig& config = {});
    ~RenderGraph();
    RenderGraph(const RenderGraph&) = delete;
    RenderGraph& operator=(const RenderGraph&) = delete;

    // Starts a frame: forgets last frame's passes/handles, keeps pooled GPU
    // resources and caches (retiring the ones unused for a few frames).
    void reset(u64 frameIndex);

    // Brings an externally owned resource into the graph. `initial` is its state
    // when the graph starts, `final` the state it must be in afterwards (Unknown:
    // leave it wherever the last pass put it; see finalStateOf).
    RgTexture importTexture(nvrhi::ITexture* texture, nvrhi::ResourceStates initial, nvrhi::ResourceStates final,
                            const char* name);
    RgBuffer importBuffer(nvrhi::IBuffer* buffer, nvrhi::ResourceStates initial, nvrhi::ResourceStates final,
                          const char* name);

    // Declares a pass. `setup(RgPassBuilder&, Data&)` runs immediately and fills
    // Data (handles, parameters). `exec(const Data&, RgPassContext&)` runs during
    // execute(), possibly on a worker thread. Both Data and the exec callable
    // are stored in a per-frame arena and must be trivially destructible: capture
    // pointers/values, not owning containers.
    template <class Data, class Setup, class Exec>
    Data& addPass(const char* name, Setup&& setup, Exec&& exec, RgPassFlags flags = RgPassFlags::None) {
        static_assert(std::is_trivially_destructible_v<Data>, "pass data lives in a frame arena");
        static_assert(std::is_trivially_destructible_v<std::decay_t<Exec>>,
                      "pass bodies live in a frame arena: capture pointers or values, not owning types");
        using ExecT = std::decay_t<Exec>;
        auto* data = new (allocate(sizeof(Data), alignof(Data))) Data{};
        auto* fn = new (allocate(sizeof(ExecT), alignof(ExecT))) ExecT(std::forward<Exec>(exec));
        const u32 pass = beginPass(name, flags);
        RgPassBuilder builder(*this, pass);
        setup(builder, *data);
        setPassBody(pass, fn, data, [](const void* f, const void* d, RgPassContext& ctx) {
            (*static_cast<const ExecT*>(f))(*static_cast<const Data*>(d), ctx);
        });
        return *data;
    }

    // Cull, allocate transients from the pool, plan states and groups.
    void compile(RgResourceFactory& factory);
    void compile(nvrhi::IDevice* device); // uses the device to create transients

    // record(): fills the command lists of every live pass (groups in parallel).
    // Safe to call for DIFFERENT graphs from different threads at once.
    // submit(): sends them to the graphics queue; main thread only, in the order
    // the caller wants the graphs to execute on the GPU.
    // execute(): record() then submit().
    void record(nvrhi::IDevice* device);
    void submit(nvrhi::IDevice* device);
    void execute(nvrhi::IDevice* device);

    // State of an imported or transient resource after the graph (valid after compile()).
    nvrhi::ResourceStates finalStateOf(RgTexture texture) const;
    nvrhi::ResourceStates finalStateOf(RgBuffer buffer) const;

    // Debugger introspection (valid after compile()).
    std::span<const RgPassInfo> passInfos() const;
    const RgStats& stats() const;
    u64 frameIndex() const;

    // Drops every pooled resource and cache entry (e.g. before a device reset).
    void releaseAll();

    struct Impl; // opaque (src/rhi/render_graph.cpp)

private:
    friend class RgPassBuilder;
    friend class RgPassContext;

    void* allocate(size_t size, size_t align);
    u32 beginPass(const char* name, RgPassFlags flags);
    using InvokeFn = void (*)(const void* fn, const void* data, RgPassContext& ctx);
    void setPassBody(u32 pass, const void* fn, const void* data, InvokeFn invoke);

    std::unique_ptr<Impl> m_impl;
};

} // namespace foh
