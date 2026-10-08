#pragma once

// RHI tier: this header (like everything under foh/rhi/) is the NVRHI adapter
// and includes NVRHI on purpose. Only engine internals and the editor include
// it. Game modules talk to the renderer through foh/renderer/ (NVRHI-free).

#include <memory>
#include <string>

#include <nvrhi/nvrhi.h>

#include "foh/platform.h"
#include "foh/types.h"

namespace foh {

enum class GraphicsBackend : u8 {
    Default, // D3D12 on Windows, Vulkan elsewhere
    D3D12,
    Vulkan, // not implemented yet (see docs/adr/0003-render-graph.md)
};

struct GraphicsDeviceDesc {
    GraphicsBackend backend = GraphicsBackend::Default;
    // API debug layer + NVRHI validation layer. Slow; on by default in debug builds.
    bool validation =
#if defined(NDEBUG)
        false;
#else
        true;
#endif
    u32 framesInFlight = 2; // CPU may run this many frames ahead of the GPU
};

// Owns the GPU device and the frame pacing. One per process.
//
// Thread-safety: nvrhi() resource creation (textures, buffers, shaders,
// pipelines, binding sets) may be called from any thread. beginFrame, endFrame,
// waitIdle and executeCommandLists-style submission are main-thread only.
class FOH_API GraphicsDevice {
public:
    // Returns null (and logs why) if no suitable adapter/API is available.
    static std::unique_ptr<GraphicsDevice> create(const GraphicsDeviceDesc& desc);
    ~GraphicsDevice();
    GraphicsDevice(const GraphicsDevice&) = delete;
    GraphicsDevice& operator=(const GraphicsDevice&) = delete;

    nvrhi::IDevice* nvrhi() const;
    nvrhi::GraphicsAPI api() const;
    const std::string& adapterName() const;
    u32 framesInFlight() const;
    u64 frameNumber() const; // number of beginFrame() calls so far

    // Blocks until the GPU finished the frame that used this slot
    // `framesInFlight` frames ago, then recycles finished resources.
    void beginFrame();
    // Marks the end of this frame's submissions.
    void endFrame();
    void waitIdle();

    // Count of API/validation errors reported so far (smoke tests assert it stays 0).
    u32 errorCount() const;

    // Platform handles for the swapchain implementation (IDXGIFactory* on D3D12).
    void* nativeFactory() const;
    bool supportsTearing() const;

private:
    GraphicsDevice();
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace foh
