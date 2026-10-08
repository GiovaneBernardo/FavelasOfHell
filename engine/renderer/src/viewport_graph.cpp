// The per-viewport render graph:
//
//   UploadInstances (copy)   scene instances -> transient vertex stream
//   Forward                  sky + instanced meshes -> HDR color + depth
//   Grid (editor views)      ground grid over the HDR color, occluded by depth
//   Tonemap                  HDR -> sRGB RGBA8, the viewport's output image
//
// Pass bodies run on recording threads: they only read ViewFrame / the shared
// immutable GPU objects, and write through the command list they are given.

#include <algorithm>
#include <bit>

#include "foh/log.h"
#include "renderer_impl.h"

namespace foh {

namespace {

using Impl = Renderer::Impl;

constexpr nvrhi::Format kHdrFormat = nvrhi::Format::RGBA16_FLOAT;
constexpr nvrhi::Format kDepthFormat = nvrhi::Format::D32;

u64 shaderKey(const ShaderRef& vs, const ShaderRef& ps, u64 variant) {
    return HashCombine(HashCombine(vs.uid, ps.uid), variant);
}

// The camera a viewport renders with: explicit override > scene camera.
RenderViewParams resolveView(const Viewport& vp, const SceneSlot* slot, f32 aspect) {
    if (vp.hasCameraOverride) return vp.cameraOverride;
    if (slot) {
        const ExtractedCamera* best = nullptr;
        for (const ExtractedCamera& c : slot->scene->read().cameras) {
            if (vp.cameraEntity) {
                if (c.entity == vp.cameraEntity) {
                    best = &c;
                    break;
                }
            } else if (c.active && (!best || c.entity < best->entity)) {
                best = &c; // deterministic: lowest entity id among the active cameras
            }
        }
        if (best) return MakeViewParams(best->world, best->fovY, aspect, best->nearZ, best->farZ);
    }
    // No camera: look down -Z from a little above the origin so the viewport is not blank.
    const Mat4 world = glm::translate(Mat4(1.0f), Vec3(0.0f, 2.0f, 6.0f));
    return MakeViewParams(world, 1.0471976f, aspect, 0.1f, 4000.0f);
}

void fillConstants(ViewConstants& c, const RenderViewParams& v, const SceneSlot* slot, u32 w, u32 h, f32 time) {
    c.view = v.view;
    c.proj = v.proj;
    c.viewProj = v.proj * v.view;
    c.invViewProj = glm::inverse(c.viewProj);
    c.cameraPos = Vec4(v.position, 1.0f);
    c.sunDirection = Vec4(glm::normalize(Vec3(0.4f, -1.0f, 0.3f)), 0.0f);
    c.sunColor = Vec4(0.0f);
    if (slot) {
        const ExtractedSun& sun = slot->scene->read().sun;
        if (sun.present) {
            c.sunDirection = Vec4(glm::normalize(sun.direction), 1.0f);
            c.sunColor = Vec4(sun.color, 1.0f);
        }
    }
    c.viewport = Vec4(static_cast<f32>(w), static_cast<f32>(h), 1.0f / static_cast<f32>(w), 1.0f / static_cast<f32>(h));
    c.params = Vec4(1.0f, time, v.nearZ, v.farZ);
}

nvrhi::ViewportState fullViewport(const ViewFrame& vf) {
    return nvrhi::ViewportState().addViewportAndScissorRect(
        nvrhi::Viewport(static_cast<f32>(vf.width), static_cast<f32>(vf.height)));
}

// Binding set for passes that use the view constants (+ optionally one texture).
nvrhi::IBindingSet* viewBindings(RgPassContext& ctx, Impl& s, const ViewFrame& vf, nvrhi::ITexture* texture,
                                 nvrhi::Format srvFormat) {
    nvrhi::BindingSetDesc bd;
    bd.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, vf.viewCb));
    if (texture) {
        bd.addItem(nvrhi::BindingSetItem::Texture_SRV(0, texture, srvFormat));
        return ctx.bindingSet(bd, s.gpu.layoutViewTexture);
    }
    return ctx.bindingSet(bd, s.gpu.layoutView);
}

// ---- pass data ----

struct UploadData {
    RgBuffer instances;
};
struct ForwardData {
    RgBuffer instances; // invalid when the scene has no meshes
    RgTexture hdr;
    RgTexture depth;
};
struct GridData {
    RgTexture hdr;
    RgTexture depth;
};
struct TonemapData {
    RgTexture hdr;
    RgTexture output;
};

// ---- pass bodies ----

void recordForward(Impl& s, const ViewFrame& vf, const ForwardData& d, RgPassContext& ctx) {
    nvrhi::ICommandList* cmd = ctx.cmd();
    cmd->writeBuffer(vf.viewCb, &vf.constants, sizeof(ViewConstants));
    nvrhi::IFramebuffer* fb = ctx.framebuffer();

    // Reverse-Z: far is 0.
    cmd->clearDepthStencilTexture(ctx.texture(d.depth), nvrhi::AllSubresources, true, 0.0f, false, 0);

    nvrhi::IBindingSet* set = viewBindings(ctx, s, vf, nullptr, nvrhi::Format::UNKNOWN);

    if (HasFlag(vf.flags, ViewportFlags::Sky)) {
        const ShaderRef vs = s.shaders->get("sky.vsFullscreen", nvrhi::ShaderType::Vertex);
        const ShaderRef ps = s.shaders->get("sky.psSky", nvrhi::ShaderType::Pixel);
        if (vs && ps) {
            nvrhi::IGraphicsPipeline* pso = s.pipelines->graphics(shaderKey(vs, ps, 1), fb, [&] {
                nvrhi::GraphicsPipelineDesc pd;
                pd.VS = vs.shader;
                pd.PS = ps.shader;
                pd.bindingLayouts = {s.gpu.layoutView};
                pd.renderState.depthStencilState.depthTestEnable = false;
                pd.renderState.depthStencilState.depthWriteEnable = false;
                pd.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
                return pd;
            });
            if (pso) {
                nvrhi::GraphicsState gs;
                gs.setPipeline(pso).setFramebuffer(fb).setViewport(fullViewport(vf)).addBindingSet(set);
                cmd->setGraphicsState(gs);
                cmd->draw(nvrhi::DrawArguments().setVertexCount(3));
            }
        }
    } else {
        cmd->clearTextureFloat(ctx.texture(d.hdr), nvrhi::AllSubresources, nvrhi::Color(0.02f, 0.02f, 0.025f, 1.0f));
    }

    if (!vf.scene || vf.scene->batches.empty() || !d.instances.valid()) return;

    const ShaderRef vs = s.shaders->get("mesh_forward.vsMesh", nvrhi::ShaderType::Vertex);
    const ShaderRef ps = s.shaders->get("mesh_forward.psMesh", nvrhi::ShaderType::Pixel);
    if (!vs || !ps) return;
    nvrhi::IGraphicsPipeline* pso = s.pipelines->graphics(shaderKey(vs, ps, 2), fb, [&] {
        nvrhi::GraphicsPipelineDesc pd;
        pd.inputLayout = s.gpu.meshInput;
        pd.VS = vs.shader;
        pd.PS = ps.shader;
        pd.bindingLayouts = {s.gpu.layoutView};
        pd.renderState.depthStencilState.depthTestEnable = true;
        pd.renderState.depthStencilState.depthWriteEnable = true;
        pd.renderState.depthStencilState.depthFunc = nvrhi::ComparisonFunc::GreaterOrEqual; // reverse-Z
        pd.renderState.rasterState.cullMode = nvrhi::RasterCullMode::Back;
        pd.renderState.rasterState.frontCounterClockwise = true;
        return pd;
    });
    if (!pso) return;

    nvrhi::GraphicsState gs;
    gs.setPipeline(pso).setFramebuffer(fb).setViewport(fullViewport(vf)).addBindingSet(set);
    gs.addVertexBuffer({s.gpu.meshes.vertexBuffer(), 0, 0});
    gs.addVertexBuffer({ctx.buffer(d.instances), 1, 0});
    gs.setIndexBuffer({s.gpu.meshes.indexBuffer(), nvrhi::Format::R32_UINT, 0});
    cmd->setGraphicsState(gs);
    for (const SceneGpu::Batch& b : vf.scene->batches) {
        const MeshRange& r = s.gpu.meshes.range(b.mesh);
        cmd->drawIndexed(nvrhi::DrawArguments()
                             .setVertexCount(r.indexCount)
                             .setInstanceCount(b.count)
                             .setStartIndexLocation(r.firstIndex)
                             .setStartVertexLocation(static_cast<u32>(r.baseVertex))
                             .setStartInstanceLocation(b.first));
    }
}

void recordGrid(Impl& s, const ViewFrame& vf, const GridData& d, RgPassContext& ctx) {
    nvrhi::ICommandList* cmd = ctx.cmd();
    cmd->writeBuffer(vf.viewCb, &vf.constants, sizeof(ViewConstants));
    nvrhi::IFramebuffer* fb = ctx.framebuffer();
    const ShaderRef vs = s.shaders->get("grid.vsFullscreen", nvrhi::ShaderType::Vertex);
    const ShaderRef ps = s.shaders->get("grid.psGrid", nvrhi::ShaderType::Pixel);
    if (!vs || !ps) return;
    nvrhi::IGraphicsPipeline* pso = s.pipelines->graphics(shaderKey(vs, ps, 3), fb, [&] {
        nvrhi::GraphicsPipelineDesc pd;
        pd.VS = vs.shader;
        pd.PS = ps.shader;
        pd.bindingLayouts = {s.gpu.layoutViewTexture};
        pd.renderState.depthStencilState.depthTestEnable = false;
        pd.renderState.depthStencilState.depthWriteEnable = false;
        pd.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
        auto& rt = pd.renderState.blendState.targets[0];
        rt.setBlendEnable(true)
            .setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
            .setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
            .setSrcBlendAlpha(nvrhi::BlendFactor::Zero)
            .setDestBlendAlpha(nvrhi::BlendFactor::One);
        return pd;
    });
    if (!pso) return;
    nvrhi::IBindingSet* set = viewBindings(ctx, s, vf, ctx.texture(d.depth), nvrhi::Format::R32_FLOAT);
    nvrhi::GraphicsState gs;
    gs.setPipeline(pso).setFramebuffer(fb).setViewport(fullViewport(vf)).addBindingSet(set);
    cmd->setGraphicsState(gs);
    cmd->draw(nvrhi::DrawArguments().setVertexCount(3));
}

void recordTonemap(Impl& s, const ViewFrame& vf, const TonemapData& d, RgPassContext& ctx) {
    nvrhi::ICommandList* cmd = ctx.cmd();
    cmd->writeBuffer(vf.viewCb, &vf.constants, sizeof(ViewConstants));
    nvrhi::IFramebuffer* fb = ctx.framebuffer();
    const ShaderRef vs = s.shaders->get("tonemap.vsFullscreen", nvrhi::ShaderType::Vertex);
    const ShaderRef ps = s.shaders->get("tonemap.psTonemap", nvrhi::ShaderType::Pixel);
    if (!vs || !ps) return;
    nvrhi::IGraphicsPipeline* pso = s.pipelines->graphics(shaderKey(vs, ps, 4), fb, [&] {
        nvrhi::GraphicsPipelineDesc pd;
        pd.VS = vs.shader;
        pd.PS = ps.shader;
        pd.bindingLayouts = {s.gpu.layoutViewTexture};
        pd.renderState.depthStencilState.depthTestEnable = false;
        pd.renderState.depthStencilState.depthWriteEnable = false;
        pd.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
        return pd;
    });
    if (!pso) return;
    nvrhi::IBindingSet* set = viewBindings(ctx, s, vf, ctx.texture(d.hdr), nvrhi::Format::UNKNOWN);
    nvrhi::GraphicsState gs;
    gs.setPipeline(pso).setFramebuffer(fb).setViewport(fullViewport(vf)).addBindingSet(set);
    cmd->setGraphicsState(gs);
    cmd->draw(nvrhi::DrawArguments().setVertexCount(3));
}

struct PresentData {
    RgTexture source;
    RgTexture target;
};

void recordPresent(Impl& s, const PresentData& d, RgPassContext& ctx) {
    nvrhi::IFramebuffer* fb = ctx.framebuffer();
    const ShaderRef vs = s.shaders->get("blit.vsFullscreen", nvrhi::ShaderType::Vertex);
    const ShaderRef ps = s.shaders->get("blit.psBlit", nvrhi::ShaderType::Pixel);
    if (!vs || !ps) return;
    nvrhi::IGraphicsPipeline* pso = s.pipelines->graphics(shaderKey(vs, ps, 5), fb, [&] {
        nvrhi::GraphicsPipelineDesc pd;
        pd.VS = vs.shader;
        pd.PS = ps.shader;
        pd.bindingLayouts = {s.gpu.layoutTexture};
        pd.renderState.depthStencilState.depthTestEnable = false;
        pd.renderState.depthStencilState.depthWriteEnable = false;
        pd.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
        return pd;
    });
    if (!pso) return;
    nvrhi::BindingSetDesc bd;
    bd.addItem(nvrhi::BindingSetItem::Texture_SRV(0, ctx.texture(d.source)));
    const nvrhi::FramebufferInfoEx& info = fb->getFramebufferInfo();
    nvrhi::GraphicsState gs;
    gs.setPipeline(pso).setFramebuffer(fb).addBindingSet(ctx.bindingSet(bd, s.gpu.layoutTexture));
    gs.viewport.addViewportAndScissorRect(nvrhi::Viewport(static_cast<f32>(info.width), static_cast<f32>(info.height)));
    ctx.cmd()->setGraphicsState(gs);
    ctx.cmd()->draw(nvrhi::DrawArguments().setVertexCount(3));
}

} // namespace

void Renderer::addPresentPass(RenderGraph& graph, RgTexture backbuffer, ViewportId viewport) {
    const RgTexture source = importViewportTexture(graph, viewport);
    Impl* self = m_impl.get();
    graph.addPass<PresentData>(
        "Present",
        [&](RgPassBuilder& b, PresentData& d) {
            d.source = b.read(source, RgAccess::SampledRead);
            d.target = b.colorTarget(0, backbuffer);
            b.neverCull();
        },
        [self](const PresentData& d, RgPassContext& ctx) { recordPresent(*self, d, ctx); });
}

void Renderer::Impl::buildViewportGraph(Viewport& vp) {
    vp.graphReady = false;
    if (!vp.output || vp.width == 0 || vp.height == 0) return;

    const SceneSlot* slot = findScene(vp.scene);
    ViewFrame& vf = vp.frame;
    vf.width = vp.width;
    vf.height = vp.height;
    vf.flags = vp.flags;
    vf.viewCb = vp.viewCb.Get();
    vf.scene = slot ? &slot->gpu : nullptr;
    const f32 aspect = static_cast<f32>(vp.width) / static_cast<f32>(vp.height);
    const f32 time = std::chrono::duration<f32>(std::chrono::steady_clock::now() - startTime).count();
    fillConstants(vf.constants, resolveView(vp, slot, aspect), slot, vp.width, vp.height, time);

    RenderGraph& g = *vp.graph;
    g.reset(frame);

    const RgTexture output = g.importTexture(vp.output.Get(), nvrhi::ResourceStates::ShaderResource,
                                             nvrhi::ResourceStates::ShaderResource, "ViewportOutput");

    RgTextureDesc hdrDesc;
    hdrDesc.width = vp.width;
    hdrDesc.height = vp.height;
    hdrDesc.format = kHdrFormat;
    hdrDesc.renderTarget = true;
    hdrDesc.useClearValue = true; // declared up front: D3D12 clears are faster and the debug layer stays quiet
    hdrDesc.clearValue = nvrhi::Color(0.02f, 0.02f, 0.025f, 1.0f);
    hdrDesc.name = "SceneColorHDR";
    RgTextureDesc depthDesc = hdrDesc;
    depthDesc.format = kDepthFormat;
    depthDesc.clearValue = nvrhi::Color(0.0f, 0.0f, 0.0f, 0.0f); // depth 0 = far (reverse-Z), stencil 0
    depthDesc.name = "SceneDepth";

    Impl* self = this;
    const ViewFrame* view = &vp.frame;

    // The transient handles are created by the first pass that needs them.
    RgTexture hdr, depth;
    RgBuffer instances;
    const size_t instanceCount = vf.scene ? vf.scene->instances.size() : 0;

    if (instanceCount > 0) {
        // Size classes (powers of two) keep the pooled buffer stable while the
        // instance count wobbles from frame to frame.
        RgBufferDesc bd;
        bd.byteSize = std::max<u64>(4096, std::bit_ceil(instanceCount * sizeof(GpuInstance)));
        bd.vertex = true;
        bd.name = "InstanceStream";
        g.addPass<UploadData>(
            "UploadInstances",
            [&](RgPassBuilder& b, UploadData& d) {
                instances = b.createBuffer(bd);
                d.instances = b.write(instances, RgAccess::CopyDst);
            },
            [view](const UploadData& d, RgPassContext& ctx) {
                const auto& v = view->scene->instances;
                ctx.cmd()->writeBuffer(ctx.buffer(d.instances), v.data(), v.size() * sizeof(GpuInstance));
            });
    }

    g.addPass<ForwardData>(
        "Forward",
        [&](RgPassBuilder& b, ForwardData& d) {
            hdr = b.createTexture(hdrDesc);
            depth = b.createTexture(depthDesc);
            d.hdr = b.colorTarget(0, hdr);
            d.depth = b.depthTarget(depth);
            if (instanceCount > 0) d.instances = b.read(instances, RgAccess::VertexRead);
        },
        [self, view](const ForwardData& d, RgPassContext& ctx) { recordForward(*self, *view, d, ctx); });

    if (HasFlag(vp.flags, ViewportFlags::Grid)) {
        g.addPass<GridData>(
            "Grid",
            [&](RgPassBuilder& b, GridData& d) {
                d.hdr = b.colorTarget(0, hdr);
                d.depth = b.read(depth, RgAccess::SampledRead);
            },
            [self, view](const GridData& d, RgPassContext& ctx) { recordGrid(*self, *view, d, ctx); });
    }

    g.addPass<TonemapData>(
        "Tonemap",
        [&](RgPassBuilder& b, TonemapData& d) {
            d.hdr = b.read(hdr, RgAccess::SampledRead);
            d.output = b.colorTarget(0, output);
        },
        [self, view](const TonemapData& d, RgPassContext& ctx) { recordTonemap(*self, *view, d, ctx); });

    g.compile(device->nvrhi());

    // Snapshot for the render graph debugger panel.
    vp.debug.name = vp.name;
    vp.debug.width = vp.width;
    vp.debug.height = vp.height;
    vp.debug.instances = static_cast<u32>(instanceCount);
    const RgStats& st = g.stats();
    vp.debug.groups = st.groups;
    vp.debug.physicalTextures = st.physicalTextures;
    vp.debug.transientTextures = st.transientTextures;
    vp.debug.barriers = st.barrierTransitions;
    vp.debug.passes.clear();
    for (const RgPassInfo& p : g.passInfos()) vp.debug.passes.push_back({p.name, p.culled, p.group});

    vp.graphReady = true;
}

} // namespace foh
