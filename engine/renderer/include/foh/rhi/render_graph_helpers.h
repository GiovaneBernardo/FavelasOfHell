#pragma once

// Helpers on top of the render graph for the two pass shapes that need no mesh
// data: fullscreen pixel-shader passes and compute dispatches. They own the
// boilerplate (shader lookup, pipeline cache key, viewport, draw/dispatch), so a
// pass only declares its resources and says which binding set to use.
//
//     AddFullscreenPass<Data>(graph, services, "Tonemap", {"tonemap.psTonemap", layout},
//         [&](RgPassBuilder& b, Data& d) { d.hdr = b.read(hdr); d.out = b.colorTarget(0, out); },
//         [=](const Data& d, RgPassContext& ctx) { return makeBindingSet(d, ctx); });
//
// Fullscreen passes draw ONE oversized triangle from SV_VertexID (shaders/fullscreen.slang,
// no vertex buffer). The rasterizer clips what hangs over the screen, so there is no
// diagonal seam (a quad would shade the pixels along it twice, in helper lanes) and
// nothing has to be discarded in the pixel shader.
//
// Everything captured by the bind/dispatch callables lives in the per-frame arena:
// capture pointers and values only (same rule as RenderGraph::addPass).
//
// Thread-safety: the Record* functions run inside pass bodies, on any recording
// thread; ShaderLibrary and PipelineCache are thread-safe.

#include "foh/rhi/pipeline_cache.h"
#include "foh/rhi/render_graph.h"
#include "foh/rhi/shader_library.h"

namespace foh {

// What pass bodies need to turn shader names into pipelines. Two pointers: copy freely.
struct RgShaderServices {
    ShaderLibrary* shaders = nullptr;
    PipelineCache* pipelines = nullptr;
};

enum class RgBlend : u8 {
    Opaque, // overwrite
    Alpha,  // src.a * src + (1 - src.a) * dst, destination alpha kept
    Premultiplied, // src + (1 - src.a) * dst, destination alpha kept
};

struct RgFullscreenState {
    const char* pixelShader = nullptr;                  // "<file>.<entry>"
    nvrhi::IBindingLayout* layout = nullptr;            // null: the shader binds nothing
    RgBlend blend = RgBlend::Opaque;
    const char* vertexShader = "fullscreen.vsFullscreen";
};

// Draws the fullscreen triangle into ctx.framebuffer() over its whole extent,
// with no depth test. `bindings` may be null if the state has no layout.
// Returns false (and draws nothing) while a shader is missing or failed to load.
FOH_API bool RecordFullscreen(RgPassContext& ctx, const RgShaderServices& services, const RgFullscreenState& state,
                              nvrhi::IBindingSet* bindings);

struct RgComputeState {
    const char* shader = nullptr;            // "<file>.<entry>"
    nvrhi::IBindingLayout* layout = nullptr; // null: the shader binds nothing
};

struct RgDispatch {
    u32 x = 1, y = 1, z = 1; // thread GROUPS
};

constexpr u32 DivideRoundUp(u32 value, u32 divisor) { return (value + divisor - 1) / divisor; }
// Thread groups needed to cover a width x height image with groupSize x groupSize tiles.
constexpr RgDispatch DispatchForImage(u32 width, u32 height, u32 groupSize = 8) {
    return {DivideRoundUp(width, groupSize), DivideRoundUp(height, groupSize), 1};
}

FOH_API bool RecordCompute(RgPassContext& ctx, const RgShaderServices& services, const RgComputeState& state,
                           nvrhi::IBindingSet* bindings, RgDispatch groups);

// `setup(RgPassBuilder&, Data&)` declares resources (use colorTarget() for what the pixel
// shader writes). `bind(const Data&, RgPassContext&) -> nvrhi::IBindingSet*` runs while
// recording: write per-pass constants and return the binding set (ctx.bindingSet()).
template <class Data, class Setup, class Bind>
Data& AddFullscreenPass(RenderGraph& graph, const RgShaderServices& services, const char* name,
                        const RgFullscreenState& state, Setup&& setup, Bind&& bind,
                        RgPassFlags flags = RgPassFlags::None) {
    return graph.template addPass<Data>(
        name, std::forward<Setup>(setup),
        [services, state, bind](const Data& d, RgPassContext& ctx) {
            RecordFullscreen(ctx, services, state, bind(d, ctx));
        },
        flags);
}

// Same for compute. `dispatch(const Data&) -> RgDispatch` gives the group counts. Declare
// written textures/buffers with RgAccess::StorageWrite (the graph then makes them UAVs).
template <class Data, class Setup, class Bind, class Dispatch>
Data& AddComputePass(RenderGraph& graph, const RgShaderServices& services, const char* name,
                     const RgComputeState& state, Setup&& setup, Bind&& bind, Dispatch&& dispatch,
                     RgPassFlags flags = RgPassFlags::None) {
    return graph.template addPass<Data>(
        name, std::forward<Setup>(setup),
        [services, state, bind, dispatch](const Data& d, RgPassContext& ctx) {
            RecordCompute(ctx, services, state, bind(d, ctx), dispatch(d));
        },
        flags);
}

} // namespace foh
