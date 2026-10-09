#include "foh/rhi/render_graph_helpers.h"

#include "foh/assert.h"

namespace foh {

namespace {

u64 layoutKey(const nvrhi::IBindingLayout* layout) { return reinterpret_cast<uintptr_t>(layout); }

} // namespace

bool RecordFullscreen(RgPassContext& ctx, const RgShaderServices& services, const RgFullscreenState& state,
                      nvrhi::IBindingSet* bindings) {
    nvrhi::IFramebuffer* fb = ctx.framebuffer();
    FOH_ASSERT(fb, "fullscreen pass without a color target");
    const ShaderRef vs = services.shaders->get(state.vertexShader, nvrhi::ShaderType::Vertex);
    const ShaderRef ps = services.shaders->get(state.pixelShader, nvrhi::ShaderType::Pixel);
    if (!vs || !ps) return false;

    u64 key = HashCombine(HashCombine(vs.uid, ps.uid), layoutKey(state.layout));
    key = HashCombine(key, static_cast<u64>(state.blend) + 0xF5u);
    nvrhi::IGraphicsPipeline* pso = services.pipelines->graphics(key, fb, [&] {
        nvrhi::GraphicsPipelineDesc pd;
        pd.VS = vs.shader;
        pd.PS = ps.shader;
        if (state.layout) pd.bindingLayouts = {state.layout};
        pd.renderState.depthStencilState.depthTestEnable = false;
        pd.renderState.depthStencilState.depthWriteEnable = false;
        pd.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
        if (state.blend != RgBlend::Opaque) {
            pd.renderState.blendState.targets[0]
                .setBlendEnable(true)
                .setSrcBlend(state.blend == RgBlend::Alpha ? nvrhi::BlendFactor::SrcAlpha : nvrhi::BlendFactor::One)
                .setDestBlend(state.blend == RgBlend::Additive ? nvrhi::BlendFactor::One : nvrhi::BlendFactor::InvSrcAlpha)
                .setSrcBlendAlpha(nvrhi::BlendFactor::Zero)
                .setDestBlendAlpha(nvrhi::BlendFactor::One);
        }
        return pd;
    });
    if (!pso) return false;

    const nvrhi::FramebufferInfoEx& info = fb->getFramebufferInfo();
    nvrhi::GraphicsState gs;
    gs.setPipeline(pso).setFramebuffer(fb);
    gs.viewport.addViewportAndScissorRect(
        nvrhi::Viewport(static_cast<f32>(info.width), static_cast<f32>(info.height)));
    if (bindings) gs.addBindingSet(bindings);
    ctx.cmd()->setGraphicsState(gs);
    ctx.cmd()->draw(nvrhi::DrawArguments().setVertexCount(3));
    return true;
}

bool RecordCompute(RgPassContext& ctx, const RgShaderServices& services, const RgComputeState& state,
                   nvrhi::IBindingSet* bindings, RgDispatch groups) {
    const ShaderRef cs = services.shaders->get(state.shader, nvrhi::ShaderType::Compute);
    if (!cs) return false;
    const u64 key = HashCombine(HashCombine(cs.uid, layoutKey(state.layout)), 0xC0u);
    nvrhi::IComputePipeline* pso = services.pipelines->compute(key, [&] {
        nvrhi::ComputePipelineDesc pd;
        pd.CS = cs.shader;
        if (state.layout) pd.bindingLayouts = {state.layout};
        return pd;
    });
    if (!pso) return false;

    nvrhi::ComputeState cs2;
    cs2.setPipeline(pso);
    if (bindings) cs2.addBindingSet(bindings);
    ctx.cmd()->setComputeState(cs2);
    ctx.cmd()->dispatch(groups.x, groups.y, groups.z);
    return true;
}

} // namespace foh
