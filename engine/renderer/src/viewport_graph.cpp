// The per-viewport render graph:
//
//   UploadInstances (copy)   scene instances -> transient vertex stream
//   DepthPrepass             depth of all opaque geometry (VS only)
//   ShadowCascades           sun shadow map: 4 cascades tiled in one depth atlas
//   Opaque                   G-buffer fill (albedo, normal, material), depth-tested
//                            against the pre-pass so every pixel is shaded once
//   Sky | ClearHdr           background into the HDR target (sky: precomputed atmosphere, empty-depth pixels only)
//   Deferred                 fullscreen lighting from the G-buffer + shadows + atmosphere -> HDR
//   CloudTrace/Resolve/Composite (volumetric clouds, sky viewports with the atmosphere on)
//                            half-resolution ray march, temporal accumulation, blend over the empty HDR pixels
//   Grid (editor views)      ground grid over the HDR color, occluded by depth
//   Tonemap                  HDR -> sRGB RGBA8, the viewport's output image
//
// Fullscreen passes go through AddFullscreenPass (rhi/render_graph_helpers.h).
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

void fillConstants(ViewConstants& c, const RenderViewParams& v, const SceneSlot* slot, u32 w, u32 h, f32 time,
                   const RenderSettings& settings) {
    c = ViewConstants{};
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
    c.params = Vec4(settings.exposure, time, v.nearZ, v.farZ);
    c.render = Vec4(settings.dither ? 1.0f : 0.0f, settings.aerialPerspective ? 1.0f : 0.0f, 0.0f, 0.0f);
}

// Fits the sun's cascades to the view and publishes them in the view constants.
void fillShadows(ViewFrame& vf, const RenderViewParams& v, const RenderSettings& settings) {
    ViewConstants& c = vf.constants;
    vf.shadowTile = std::clamp(settings.shadowCascadeSize, 256u, 8192u);
    vf.shadowsOn = settings.shadows && vf.scene && !vf.scene->instances.empty() && c.sunDirection.w > 0.5f;
    if (!vf.shadowsOn) return;

    ShadowCascadeDesc cd;
    cd.view = v.view;
    cd.proj = v.proj;
    cd.nearZ = v.nearZ;
    cd.farZ = v.farZ;
    cd.lightDirection = Vec3(c.sunDirection);
    cd.shadowDistance = settings.shadowDistance;
    cd.resolution = vf.shadowTile;
    vf.cascades = ComputeShadowCascades(cd);
    for (u32 i = 0; i < kShadowCascades; ++i) {
        c.cascadeViewProj[i] = vf.cascades.viewProj[i];
        c.cascadeSplits[i] = vf.cascades.splitFar[i];
        c.cascadeTexel[i] = vf.cascades.texelWorld[i];
    }
    c.shadowParams = Vec4(1.0f / static_cast<f32>(vf.shadowTile * 2), 0.5f, 0.0f, 1.0f);
}

nvrhi::ViewportState fullViewport(const ViewFrame& vf) {
    return nvrhi::ViewportState().addViewportAndScissorRect(
        nvrhi::Viewport(static_cast<f32>(vf.width), static_cast<f32>(vf.height)));
}

// Binding set for passes that use the view constants (+ optionally one texture).
// Writes the volatile constant buffer into this pass's command list too.
nvrhi::IBindingSet* viewBindings(RgPassContext& ctx, Impl& s, const ViewFrame& vf, nvrhi::ITexture* texture,
                                 nvrhi::Format srvFormat) {
    ctx.cmd()->writeBuffer(vf.viewCb, &vf.constants, sizeof(ViewConstants));
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
struct DepthPrepassData {
    RgBuffer instances; // invalid when the scene has no meshes
    RgTexture depth;
};
struct ShadowData {
    RgBuffer instances;
    RgTexture atlas;
};
struct OpaqueData {
    RgBuffer instances;
    RgTexture albedo;
    RgTexture normal;
    RgTexture material;
    RgTexture depth;
};
struct SkyData {
    RgTexture hdr;
    RgTexture depth; // invalid for the flat clear
};
struct DeferredData {
    RgTexture albedo;
    RgTexture normal;
    RgTexture material;
    RgTexture depth;
    RgTexture atlas;
    RgTexture hdr;
};
struct CloudTraceData {
    RgTexture depth;
    RgTexture out;
};
struct CloudResolveData {
    RgTexture current;
    RgTexture history;
    RgTexture out;
};
struct CloudCompositeData {
    RgTexture clouds;
    RgTexture depth;
    RgTexture hdr;
};
struct GridData {
    RgTexture hdr;
    RgTexture depth;
};
struct PickData {
    RgBuffer instances;
    RgTexture ids;
    RgTexture depth;
};
struct PickReadbackData {
    RgTexture ids;
};
struct TonemapData {
    RgTexture hdr;
    RgTexture output;
};

// ---- pass bodies ----

// Batches are grouped by draw class (see material_store.h): opaque single-sided geometry takes the cheapest path
// everywhere (depth passes without a pixel shader), alpha-masked and double-sided materials get their own pipelines.
constexpr DrawClass kAllClasses[kDrawClassCount] = {DrawClass::Opaque, DrawClass::Masked, DrawClass::OpaqueDoubleSided,
                                                    DrawClass::MaskedDoubleSided};

nvrhi::RasterCullMode cullOf(DrawClass cls) { return IsDoubleSided(cls) ? nvrhi::RasterCullMode::None : nvrhi::RasterCullMode::Back; }

// One instanced draw per mesh batch in [begin, end). Shared by the pre-pass, shadows, G-buffer and picking.
// `table` is the bindless texture table for passes that sample materials (null otherwise).
void drawBatches(Impl& s, const nvrhi::ViewportState& viewport, nvrhi::IFramebuffer* fb, nvrhi::IGraphicsPipeline* pso,
                 nvrhi::IBindingSet* set, nvrhi::IBindingSet* table, nvrhi::ICommandList* cmd, nvrhi::IBuffer* instances,
                 const SceneGpu& scene, u32 begin, u32 end) {
    if (begin >= end) return;
    nvrhi::GraphicsState gs;
    gs.setPipeline(pso).setFramebuffer(fb).setViewport(viewport).addBindingSet(set);
    if (table) gs.addBindingSet(table);
    gs.addVertexBuffer({s.gpu.meshes.vertexBuffer(), 0, 0});
    gs.addVertexBuffer({instances, 1, 0});
    gs.setIndexBuffer({s.gpu.meshes.indexBuffer(), nvrhi::Format::R32_UINT, 0});
    cmd->setGraphicsState(gs);
    for (u32 i = begin; i < end; ++i) {
        const SceneGpu::Batch& b = scene.batches[i];
        const MeshRange& r = s.gpu.meshes.range(b.mesh);
        cmd->drawIndexed(nvrhi::DrawArguments()
                             .setVertexCount(r.indexCount)
                             .setInstanceCount(b.count)
                             .setStartIndexLocation(r.firstIndex)
                             .setStartVertexLocation(static_cast<u32>(r.baseVertex))
                             .setStartInstanceLocation(b.first));
    }
}

// Pipeline for the instanced meshes: depth-only when `ps` is empty. `materials` adds the material buffer and the
// bindless texture table to the binding layouts (for pixel shaders that sample materials).
nvrhi::IGraphicsPipeline* meshPipeline(Impl& s, nvrhi::IFramebuffer* fb, const ShaderRef& vs, const ShaderRef& ps,
                                       u64 variant, bool depthWrite, nvrhi::RasterCullMode cull, bool materials) {
    const u64 key = shaderKey(vs, ps, variant * 4 + (cull == nvrhi::RasterCullMode::None ? 1 : 0) + (materials ? 2 : 0));
    return s.pipelines->graphics(key, fb, [&] {
        nvrhi::GraphicsPipelineDesc pd;
        pd.inputLayout = s.gpu.meshInput;
        pd.VS = vs.shader;
        if (ps) pd.PS = ps.shader;
        if (materials) pd.bindingLayouts = {s.gpu.layoutMaterial.Get(), s.gpu.layoutBindless.Get()};
        else pd.bindingLayouts = {s.gpu.layoutView.Get()};
        pd.renderState.depthStencilState.depthTestEnable = true;
        pd.renderState.depthStencilState.depthWriteEnable = depthWrite;
        pd.renderState.depthStencilState.depthFunc = nvrhi::ComparisonFunc::GreaterOrEqual; // reverse-Z
        pd.renderState.rasterState.cullMode = cull;
        pd.renderState.rasterState.frontCounterClockwise = true;
        return pd;
    });
}

// Binding set for passes that sample materials: `constants` (view or shadow constants), the material buffer and the
// sampler. The constant buffer itself must already have been written in this pass's command list.
nvrhi::IBindingSet* materialBindings(RgPassContext& ctx, Impl& s, nvrhi::IBuffer* constants) {
    nvrhi::BindingSetDesc bd;
    bd.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, constants));
    bd.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(0, s.gpu.materials.buffer()));
    bd.addItem(nvrhi::BindingSetItem::Sampler(0, s.gpu.materialSampler));
    return ctx.bindingSet(bd, s.gpu.layoutMaterial);
}

// Which mesh pass a pipeline is for. One place decides shaders, layouts and cull mode per (pass, draw class), so the
// pass bodies and the start-up warm-up cannot drift apart.
enum class MeshPass { Depth, Shadow, GBuffer, Pick };

nvrhi::IGraphicsPipeline* classPipeline(Impl& s, MeshPass pass, DrawClass cls, nvrhi::IFramebuffer* fb) {
    const bool masked = IsMasked(cls);
    switch (pass) {
        case MeshPass::Depth: {
            // Opaque: vertex shader only. Masked: + a pixel shader that tests the texture alpha.
            const ShaderRef vs = s.shaders->get(masked ? "depth.vsDepthMasked" : "depth.vsDepth", nvrhi::ShaderType::Vertex);
            const ShaderRef ps = masked ? s.shaders->get("depth.psDepthMasked", nvrhi::ShaderType::Pixel) : ShaderRef{};
            if (!vs || (masked && !ps)) return nullptr;
            return meshPipeline(s, fb, vs, ps, masked ? 9 : 7, true, cullOf(cls), masked);
        }
        case MeshPass::Shadow: {
            // No culling: single-sided casters (roofs, sheet metal) must still shadow from behind. Single- and
            // double-sided materials therefore share a pipeline.
            const ShaderRef vs = s.shaders->get(masked ? "shadow.vsShadowMasked" : "shadow.vsShadow", nvrhi::ShaderType::Vertex);
            const ShaderRef ps = masked ? s.shaders->get("shadow.psShadowMasked", nvrhi::ShaderType::Pixel) : ShaderRef{};
            if (!vs || (masked && !ps)) return nullptr;
            return meshPipeline(s, fb, vs, ps, masked ? 10 : 8, true, nvrhi::RasterCullMode::None, masked);
        }
        case MeshPass::GBuffer: {
            const ShaderRef vs = s.shaders->get("opaque.vsOpaque", nvrhi::ShaderType::Vertex);
            const ShaderRef ps = s.shaders->get(masked ? "opaque.psOpaqueMasked" : "opaque.psOpaque", nvrhi::ShaderType::Pixel);
            if (!vs || !ps) return nullptr;
            return meshPipeline(s, fb, vs, ps, 2, false, cullOf(cls), true);
        }
        case MeshPass::Pick: {
            const ShaderRef vs = s.shaders->get("opaque.vsOpaque", nvrhi::ShaderType::Vertex);
            const ShaderRef ps = s.shaders->get(masked ? "opaque.psPickMasked" : "opaque.psPick", nvrhi::ShaderType::Pixel);
            if (!vs || !ps) return nullptr;
            return meshPipeline(s, fb, vs, ps, 6, true, cullOf(cls), true);
        }
    }
    return nullptr;
}

void recordDepthPrepass(Impl& s, const ViewFrame& vf, const DepthPrepassData& d, RgPassContext& ctx) {
    nvrhi::ICommandList* cmd = ctx.cmd();
    // Reverse-Z: far is 0.
    cmd->clearDepthStencilTexture(ctx.texture(d.depth), nvrhi::AllSubresources, true, 0.0f, false, 0);
    if (!vf.scene || vf.scene->batches.empty() || !d.instances.valid()) return;

    nvrhi::IFramebuffer* fb = ctx.framebuffer();
    const SceneGpu& scene = *vf.scene;
    nvrhi::IBuffer* instances = ctx.buffer(d.instances);
    // viewBindings writes the view constants into this command list; the masked binding set uses the same buffer.
    nvrhi::IBindingSet* viewSet = viewBindings(ctx, s, vf, nullptr, nvrhi::Format::UNKNOWN);
    nvrhi::IBindingSet* materialSet = nullptr;
    for (DrawClass cls : kAllClasses) {
        const u32 c = static_cast<u32>(cls);
        if (scene.classBegin[c] >= scene.classBegin[c + 1]) continue;
        nvrhi::IGraphicsPipeline* pso = classPipeline(s, MeshPass::Depth, cls, fb);
        if (!pso) continue;
        if (IsMasked(cls) && !materialSet) materialSet = materialBindings(ctx, s, vf.viewCb);
        drawBatches(s, fullViewport(vf), fb, pso, IsMasked(cls) ? materialSet : viewSet, IsMasked(cls) ? s.gpu.textures.table() : nullptr, cmd,
                    instances, scene, scene.classBegin[c], scene.classBegin[c + 1]);
    }
}

// All cascades share one depth atlas (2x2 tiles); each is drawn with its own viewport.
void recordShadows(Impl& s, const ViewFrame& vf, const ShadowData& d, RgPassContext& ctx) {
    nvrhi::ICommandList* cmd = ctx.cmd();
    cmd->clearDepthStencilTexture(ctx.texture(d.atlas), nvrhi::AllSubresources, true, 0.0f, false, 0);
    if (!vf.shadowsOn || !vf.scene || vf.scene->batches.empty() || !d.instances.valid()) return;

    nvrhi::IFramebuffer* fb = ctx.framebuffer();
    const SceneGpu& scene = *vf.scene;
    nvrhi::IBuffer* instances = ctx.buffer(d.instances);
    nvrhi::BindingSetDesc bd;
    bd.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, vf.shadowCb));
    nvrhi::IBindingSet* viewSet = ctx.bindingSet(bd, s.gpu.layoutView);
    nvrhi::IBindingSet* materialSet = nullptr;

    // Pipelines resolved once, outside the cascade loop.
    nvrhi::IGraphicsPipeline* pso[kDrawClassCount];
    for (DrawClass cls : kAllClasses) {
        const u32 c = static_cast<u32>(cls);
        pso[c] = scene.classBegin[c] < scene.classBegin[c + 1] ? classPipeline(s, MeshPass::Shadow, cls, fb) : nullptr;
        if (pso[c] && IsMasked(cls) && !materialSet) materialSet = materialBindings(ctx, s, vf.shadowCb);
    }

    const f32 tile = static_cast<f32>(vf.shadowTile);
    for (u32 cascade = 0; cascade < kShadowCascades; ++cascade) {
        cmd->writeBuffer(vf.shadowCb, &vf.cascades.viewProj[cascade], sizeof(Mat4));
        const f32 x = static_cast<f32>(cascade & 1) * tile;
        const f32 y = static_cast<f32>(cascade >> 1) * tile;
        const nvrhi::ViewportState viewport =
            nvrhi::ViewportState().addViewportAndScissorRect(nvrhi::Viewport(x, x + tile, y, y + tile, 0.0f, 1.0f));
        for (DrawClass cls : kAllClasses) {
            const u32 c = static_cast<u32>(cls);
            if (!pso[c]) continue;
            drawBatches(s, viewport, fb, pso[c], IsMasked(cls) ? materialSet : viewSet, IsMasked(cls) ? s.gpu.textures.table() : nullptr, cmd,
                        instances, scene, scene.classBegin[c], scene.classBegin[c + 1]);
        }
    }
}

// Fills the G-buffer. Depth comes from the pre-pass: read-only, GreaterOrEqual, so only the
// visible surface passes (>= and not == keeps this robust against tiny VS differences).
void recordOpaque(Impl& s, const ViewFrame& vf, const OpaqueData& d, RgPassContext& ctx) {
    if (!vf.scene || vf.scene->batches.empty() || !d.instances.valid()) return;
    nvrhi::IFramebuffer* fb = ctx.framebuffer();
    const SceneGpu& scene = *vf.scene;
    nvrhi::ICommandList* cmd = ctx.cmd();
    cmd->writeBuffer(vf.viewCb, &vf.constants, sizeof(ViewConstants));
    nvrhi::IBindingSet* set = materialBindings(ctx, s, vf.viewCb);
    nvrhi::IBuffer* instances = ctx.buffer(d.instances);
    for (DrawClass cls : kAllClasses) {
        const u32 c = static_cast<u32>(cls);
        if (scene.classBegin[c] >= scene.classBegin[c + 1]) continue;
        nvrhi::IGraphicsPipeline* pso = classPipeline(s, MeshPass::GBuffer, cls, fb);
        if (!pso) continue;
        drawBatches(s, fullViewport(vf), fb, pso, set, s.gpu.textures.table(), cmd, instances, scene, scene.classBegin[c], scene.classBegin[c + 1]);
    }
}

nvrhi::IBindingSet* deferredBindings(Impl& s, const ViewFrame& vf, const DeferredData& d, RgPassContext& ctx) {
    ctx.cmd()->writeBuffer(vf.viewCb, &vf.constants, sizeof(ViewConstants));
    nvrhi::BindingSetDesc bd;
    bd.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, vf.viewCb));
    bd.addItem(nvrhi::BindingSetItem::Texture_SRV(0, ctx.texture(d.albedo)));
    bd.addItem(nvrhi::BindingSetItem::Texture_SRV(1, ctx.texture(d.normal)));
    bd.addItem(nvrhi::BindingSetItem::Texture_SRV(2, ctx.texture(d.material)));
    bd.addItem(nvrhi::BindingSetItem::Texture_SRV(3, ctx.texture(d.depth), nvrhi::Format::R32_FLOAT));
    bd.addItem(nvrhi::BindingSetItem::Texture_SRV(4, ctx.texture(d.atlas), nvrhi::Format::R32_FLOAT));
    bd.addItem(nvrhi::BindingSetItem::Texture_SRV(5, s.atmosphere.transmittance()));
    bd.addItem(nvrhi::BindingSetItem::Texture_SRV(6, s.atmosphere.scattering()));
    bd.addItem(nvrhi::BindingSetItem::Texture_SRV(7, s.atmosphere.irradiance()));
    bd.addItem(nvrhi::BindingSetItem::Sampler(0, s.gpu.shadowSampler));
    bd.addItem(nvrhi::BindingSetItem::Sampler(1, s.atmosphere.sampler()));
    return ctx.bindingSet(bd, s.gpu.layoutDeferred);
}

// The sky shades only the pixels the depth buffer says are empty, from the atmosphere tables.
nvrhi::IBindingSet* skyBindings(Impl& s, const ViewFrame& vf, const SkyData& d, RgPassContext& ctx) {
    ctx.cmd()->writeBuffer(vf.viewCb, &vf.constants, sizeof(ViewConstants));
    nvrhi::BindingSetDesc bd;
    bd.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, vf.viewCb));
    bd.addItem(nvrhi::BindingSetItem::Texture_SRV(0, ctx.texture(d.depth), nvrhi::Format::R32_FLOAT));
    bd.addItem(nvrhi::BindingSetItem::Texture_SRV(1, s.atmosphere.transmittance()));
    bd.addItem(nvrhi::BindingSetItem::Texture_SRV(2, s.atmosphere.scattering()));
    bd.addItem(nvrhi::BindingSetItem::Texture_SRV(3, s.atmosphere.irradiance()));
    bd.addItem(nvrhi::BindingSetItem::Sampler(0, s.atmosphere.sampler()));
    return ctx.bindingSet(bd, s.gpu.layoutSky);
}

// ---- clouds ----

// Both the view constants and the cloud constants are volatile buffers: every pass that reads them writes them first.
void writeCloudConstants(const ViewFrame& vf, RgPassContext& ctx) {
    ctx.cmd()->writeBuffer(vf.viewCb, &vf.constants, sizeof(ViewConstants));
    ctx.cmd()->writeBuffer(vf.cloudCb, &vf.clouds, sizeof(CloudConstants));
}

nvrhi::IBindingSet* cloudTraceBindings(Impl& s, const ViewFrame& vf, const CloudTraceData& d, RgPassContext& ctx) {
    writeCloudConstants(vf, ctx);
    nvrhi::BindingSetDesc bd;
    bd.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, vf.viewCb));
    bd.addItem(nvrhi::BindingSetItem::ConstantBuffer(1, vf.cloudCb));
    bd.addItem(nvrhi::BindingSetItem::Texture_SRV(0, ctx.texture(d.depth), nvrhi::Format::R32_FLOAT));
    bd.addItem(nvrhi::BindingSetItem::Texture_SRV(1, s.atmosphere.transmittance()));
    bd.addItem(nvrhi::BindingSetItem::Texture_SRV(2, s.atmosphere.scattering()));
    bd.addItem(nvrhi::BindingSetItem::Texture_SRV(3, s.atmosphere.irradiance()));
    bd.addItem(nvrhi::BindingSetItem::Texture_SRV(4, s.clouds.shape()));
    bd.addItem(nvrhi::BindingSetItem::Texture_SRV(5, s.clouds.detail()));
    bd.addItem(nvrhi::BindingSetItem::Texture_SRV(6, s.clouds.weather()));
    bd.addItem(nvrhi::BindingSetItem::Sampler(0, s.atmosphere.sampler()));
    bd.addItem(nvrhi::BindingSetItem::Sampler(1, s.clouds.wrapSampler()));
    return ctx.bindingSet(bd, s.clouds.traceLayout());
}

// Two textures for the resolve (new trace, history) and for the composite (resolved clouds, scene depth).
nvrhi::IBindingSet* cloudPostBindings(Impl& s, const ViewFrame& vf, nvrhi::ITexture* a, nvrhi::ITexture* b,
                                      nvrhi::Format formatB, RgPassContext& ctx) {
    writeCloudConstants(vf, ctx);
    nvrhi::BindingSetDesc bd;
    bd.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, vf.viewCb));
    bd.addItem(nvrhi::BindingSetItem::ConstantBuffer(1, vf.cloudCb));
    bd.addItem(nvrhi::BindingSetItem::Texture_SRV(0, a));
    bd.addItem(nvrhi::BindingSetItem::Texture_SRV(1, b, formatB));
    bd.addItem(nvrhi::BindingSetItem::Sampler(0, s.atmosphere.sampler()));
    return ctx.bindingSet(bd, s.clouds.postLayout());
}

// Half-resolution history of a viewport cloud image. A new one (first use, resize) has nothing to reproject from.
void ensureCloudTargets(Impl& s, Viewport& vp, u32 width, u32 height) {
    nvrhi::IDevice* dev = s.device->nvrhi();
    if (!vp.cloudCb) {
        nvrhi::BufferDesc cb;
        cb.byteSize = sizeof(CloudConstants);
        cb.isConstantBuffer = true;
        cb.isVolatile = true;
        cb.maxVersions = 16;
        cb.debugName = vp.name + " (cloud constants)";
        vp.cloudCb = dev->createBuffer(cb);
    }
    if (vp.cloudHistory[0] && vp.cloudHistoryWidth == width && vp.cloudHistoryHeight == height) return;
    for (u32 i = 0; i < 2; ++i) {
        nvrhi::TextureDesc td;
        td.width = width;
        td.height = height;
        td.format = kHdrFormat;
        td.isRenderTarget = true;
        td.initialState = nvrhi::ResourceStates::ShaderResource; // imported into the graph with this as initial AND final state
        td.keepInitialState = false;
        td.debugName = vp.name + " (cloud history)";
        vp.cloudHistory[i] = dev->createTexture(td);
    }
    vp.cloudHistoryWidth = width;
    vp.cloudHistoryHeight = height;
    vp.cloudHistoryValid = false;
}

// Entity ids instead of colors, same geometry and depth test as the G-buffer pass.
void recordPick(Impl& s, const ViewFrame& vf, const PickData& d, RgPassContext& ctx) {
    nvrhi::ICommandList* cmd = ctx.cmd();
    nvrhi::IFramebuffer* fb = ctx.framebuffer();
    cmd->clearTextureUInt(ctx.texture(d.ids), nvrhi::AllSubresources, 0);
    cmd->clearDepthStencilTexture(ctx.texture(d.depth), nvrhi::AllSubresources, true, 0.0f, false, 0);
    if (!vf.scene || vf.scene->batches.empty() || !d.instances.valid()) return;
    const SceneGpu& scene = *vf.scene;
    cmd->writeBuffer(vf.viewCb, &vf.constants, sizeof(ViewConstants));
    nvrhi::IBindingSet* set = materialBindings(ctx, s, vf.viewCb);
    nvrhi::IBuffer* instances = ctx.buffer(d.instances);
    for (DrawClass cls : kAllClasses) {
        const u32 c = static_cast<u32>(cls);
        if (scene.classBegin[c] >= scene.classBegin[c + 1]) continue;
        nvrhi::IGraphicsPipeline* pso = classPipeline(s, MeshPass::Pick, cls, fb);
        if (!pso) continue;
        drawBatches(s, fullViewport(vf), fb, pso, set, s.gpu.textures.table(), cmd, instances, scene, scene.classBegin[c], scene.classBegin[c + 1]);
    }
}

} // namespace

// Creates every mesh pipeline variant up front, against dummy framebuffers with the formats the viewport graphs use.
// Pipelines are keyed by framebuffer FORMATS, so these are the ones the real passes hit later. Without it the first
// masked / double-sided material that appears in a scene stalls the frame for the PSO compile (tens of ms, 160 in a
// Debug build) while a model is streaming in.
void Renderer::Impl::warmPipelines() {
    nvrhi::IDevice* dev = device->nvrhi();
    auto target = [&](nvrhi::Format format, bool depth) {
        nvrhi::TextureDesc td;
        td.width = td.height = 4;
        td.format = format;
        td.isRenderTarget = true;
        td.initialState = depth ? nvrhi::ResourceStates::DepthWrite : nvrhi::ResourceStates::RenderTarget;
        td.keepInitialState = true;
        td.debugName = "PipelineWarmup";
        return dev->createTexture(td);
    };
    nvrhi::TextureHandle depth = target(kDepthFormat, true);
    nvrhi::TextureHandle albedo = target(nvrhi::Format::SRGBA8_UNORM, false);
    nvrhi::TextureHandle normal = target(nvrhi::Format::RG16_UNORM, false);
    nvrhi::TextureHandle material = target(nvrhi::Format::RGBA8_UNORM, false);
    nvrhi::TextureHandle ids = target(nvrhi::Format::RG32_UINT, false);

    nvrhi::FramebufferHandle depthFb = dev->createFramebuffer(nvrhi::FramebufferDesc().setDepthAttachment(depth));
    nvrhi::FramebufferHandle gbufferFb = dev->createFramebuffer(nvrhi::FramebufferDesc()
                                                                    .addColorAttachment(albedo)
                                                                    .addColorAttachment(normal)
                                                                    .addColorAttachment(material)
                                                                    .setDepthAttachment(depth));
    nvrhi::FramebufferHandle pickFb = dev->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(ids).setDepthAttachment(depth));

    u32 created = 0;
    for (DrawClass cls : kAllClasses) {
        for (auto [pass, fb] : {std::pair{MeshPass::Depth, depthFb.Get()}, std::pair{MeshPass::Shadow, depthFb.Get()},
                                std::pair{MeshPass::GBuffer, gbufferFb.Get()}, std::pair{MeshPass::Pick, pickFb.Get()}}) {
            if (classPipeline(*this, pass, cls, fb)) ++created;
        }
    }
    FOH_DEBUG("renderer: warmed {} mesh pipeline lookups", created);
}

void Renderer::addPresentPass(RenderGraph& graph, RgTexture backbuffer, ViewportId viewport) {
    struct PresentData {
        RgTexture source;
        RgTexture target;
    };
    const RgTexture source = importViewportTexture(graph, viewport);
    Impl* self = m_impl.get();
    AddFullscreenPass<PresentData>(
        graph, self->services(), "Present", {"blit.psBlit", self->gpu.layoutTexture.Get()},
        [&](RgPassBuilder& b, PresentData& d) {
            d.source = b.read(source, RgAccess::SampledRead);
            d.target = b.colorTarget(0, backbuffer);
            b.neverCull();
        },
        [self](const PresentData& d, RgPassContext& ctx) {
            nvrhi::BindingSetDesc bd;
            bd.addItem(nvrhi::BindingSetItem::Texture_SRV(0, ctx.texture(d.source)));
            return ctx.bindingSet(bd, self->gpu.layoutTexture);
        });
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
    vf.shadowCb = vp.shadowCb.Get();
    vf.scene = slot ? &slot->gpu : nullptr;
    const f32 aspect = static_cast<f32>(vp.width) / static_cast<f32>(vp.height);
    const f32 time = std::chrono::duration<f32>(std::chrono::steady_clock::now() - startTime).count();
    const RenderViewParams view = resolveView(vp, slot, aspect);
    fillConstants(vf.constants, view, slot, vp.width, vp.height, time, settings);
    fillShadows(vf, view, settings);
    // Sky light and aerial perspective need the tables, a sun to light them, and a view that draws the sky.
    atmosphere.fillView(vf.constants);
    if (!settings.atmosphere || vf.constants.sunDirection.w < 0.5f || !HasFlag(vp.flags, ViewportFlags::Sky)) vf.constants.atmosphere1.y = 0.0f;

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
    const ViewFrame* frameData = &vp.frame;
    const RgShaderServices services = this->services();

    // The transient handles are created by the first pass that needs them.
    RgTexture hdr, depth, atlas, albedo, normal, material;
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
            [frameData](const UploadData& d, RgPassContext& ctx) {
                const auto& v = frameData->scene->instances;
                ctx.cmd()->writeBuffer(ctx.buffer(d.instances), v.data(), v.size() * sizeof(GpuInstance));
            });
    }

    g.addPass<DepthPrepassData>(
        "DepthPrepass",
        [&](RgPassBuilder& b, DepthPrepassData& d) {
            depth = b.createTexture(depthDesc);
            d.depth = b.depthTarget(depth);
            if (instanceCount > 0) d.instances = b.read(instances, RgAccess::VertexRead);
        },
        [self, frameData](const DepthPrepassData& d, RgPassContext& ctx) {
            recordDepthPrepass(*self, *frameData, d, ctx);
        });

    if (instanceCount > 0) {
        // Without a sun the atlas is a 1x1 stand-in the lighting shader never samples.
        RgTextureDesc atlasDesc = depthDesc;
        atlasDesc.width = atlasDesc.height = vf.shadowsOn ? vf.shadowTile * 2 : 1;
        atlasDesc.name = "ShadowAtlas";
        g.addPass<ShadowData>(
            "ShadowCascades",
            [&](RgPassBuilder& b, ShadowData& d) {
                atlas = b.createTexture(atlasDesc);
                d.atlas = b.depthTarget(atlas);
                d.instances = b.read(instances, RgAccess::VertexRead);
            },
            [self, frameData](const ShadowData& d, RgPassContext& ctx) { recordShadows(*self, *frameData, d, ctx); });

        RgTextureDesc gbDesc = hdrDesc;
        gbDesc.useClearValue = false; // every visible pixel is overwritten, sky pixels are never read
        gbDesc.format = nvrhi::Format::SRGBA8_UNORM;
        gbDesc.name = "GBufferAlbedo";
        RgTextureDesc gbNormal = gbDesc;
        gbNormal.format = nvrhi::Format::RG16_UNORM;
        gbNormal.name = "GBufferNormal";
        RgTextureDesc gbMaterial = gbDesc;
        gbMaterial.format = nvrhi::Format::RGBA8_UNORM;
        gbMaterial.name = "GBufferMaterial";
        g.addPass<OpaqueData>(
            "Opaque",
            [&](RgPassBuilder& b, OpaqueData& d) {
                albedo = b.createTexture(gbDesc);
                normal = b.createTexture(gbNormal);
                material = b.createTexture(gbMaterial);
                d.albedo = b.colorTarget(0, albedo);
                d.normal = b.colorTarget(1, normal);
                d.material = b.colorTarget(2, material);
                d.depth = b.depthTarget(depth, true);
                d.instances = b.read(instances, RgAccess::VertexRead);
            },
            [self, frameData](const OpaqueData& d, RgPassContext& ctx) { recordOpaque(*self, *frameData, d, ctx); });
    }

    // Background. The sky shades the pixels with empty depth and the lighting pass shades the rest, so together they
    // cover the target (no clear needed); without the sky flag a flat clear.
    if (HasFlag(vp.flags, ViewportFlags::Sky)) {
        AddFullscreenPass<SkyData>(
            g, services, "Sky", {"sky.psSky", gpu.layoutSky.Get()},
            [&](RgPassBuilder& b, SkyData& d) {
                hdr = b.createTexture(hdrDesc);
                d.hdr = b.colorTarget(0, hdr);
                d.depth = b.read(depth, RgAccess::SampledRead);
            },
            [self, frameData](const SkyData& d, RgPassContext& ctx) { return skyBindings(*self, *frameData, d, ctx); });
    } else {
        g.addPass<SkyData>(
            "ClearHdr",
            [&](RgPassBuilder& b, SkyData& d) {
                hdr = b.createTexture(hdrDesc);
                d.hdr = b.colorTarget(0, hdr); // clears are done in the render-target state
            },
            [](const SkyData& d, RgPassContext& ctx) {
                ctx.cmd()->clearTextureFloat(ctx.texture(d.hdr), nvrhi::AllSubresources,
                                             nvrhi::Color(0.02f, 0.02f, 0.025f, 1.0f));
            });
    }

    if (instanceCount > 0) {
        AddFullscreenPass<DeferredData>(
            g, services, "Deferred", {"deferred.psDeferred", gpu.layoutDeferred.Get()},
            [&](RgPassBuilder& b, DeferredData& d) {
                d.albedo = b.read(albedo, RgAccess::SampledRead);
                d.normal = b.read(normal, RgAccess::SampledRead);
                d.material = b.read(material, RgAccess::SampledRead);
                d.depth = b.read(depth, RgAccess::SampledRead);
                d.atlas = b.read(atlas, RgAccess::SampledRead);
                d.hdr = b.colorTarget(0, hdr); // keeps the sky where the shader discards
            },
            [self, frameData](const DeferredData& d, RgPassContext& ctx) {
                return deferredBindings(*self, *frameData, d, ctx);
            });
    }

    // Volumetric clouds over the empty pixels: trace (half res) -> resolve with the history -> composite.
    const bool cloudsOn = clouds.ready() && clouds.desc().enabled && HasFlag(vp.flags, ViewportFlags::Sky) &&
                          vf.constants.atmosphere1.y > 0.5f;
    if (cloudsOn) {
        const u32 cw = std::max(1u, (vp.width + 1) / 2), ch = std::max(1u, (vp.height + 1) / 2);
        ensureCloudTargets(*this, vp, cw, ch);
        // History from a frame that was not the previous one (viewport hidden, camera cut) is not trusted.
        if (vp.cloudLastFrame + 1 != frame) vp.cloudHistoryValid = false;
        clouds.fill(vf.clouds, time, frame, vp.cloudHistoryValid ? 0.12f : 1.0f);
        vf.clouds.prevViewProj = vp.cloudPrevViewProj;
        vf.cloudCb = vp.cloudCb.Get();

        const u32 write = vp.cloudWrite ^ 1;
        vp.cloudWrite = write;
        const RgTexture historyPrev = g.importTexture(vp.cloudHistory[write ^ 1].Get(), nvrhi::ResourceStates::ShaderResource,
                                                      nvrhi::ResourceStates::ShaderResource, "CloudHistoryPrev");
        const RgTexture historyNext = g.importTexture(vp.cloudHistory[write].Get(), nvrhi::ResourceStates::ShaderResource,
                                                      nvrhi::ResourceStates::ShaderResource, "CloudHistoryNext");
        RgTextureDesc cloudDesc;
        cloudDesc.width = cw;
        cloudDesc.height = ch;
        cloudDesc.format = kHdrFormat;
        cloudDesc.renderTarget = true;
        cloudDesc.name = "CloudsRaw";
        RgTexture raw;
        AddFullscreenPass<CloudTraceData>(
            g, services, "CloudTrace", {"clouds.psClouds", clouds.traceLayout()},
            [&](RgPassBuilder& b, CloudTraceData& d) {
                d.depth = b.read(depth, RgAccess::SampledRead);
                raw = b.createTexture(cloudDesc);
                d.out = b.colorTarget(0, raw);
            },
            [self, frameData](const CloudTraceData& d, RgPassContext& ctx) { return cloudTraceBindings(*self, *frameData, d, ctx); });
        AddFullscreenPass<CloudResolveData>(
            g, services, "CloudResolve", {"clouds_post.psCloudResolve", clouds.postLayout()},
            [&](RgPassBuilder& b, CloudResolveData& d) {
                d.current = b.read(raw, RgAccess::SampledRead);
                d.history = b.read(historyPrev, RgAccess::SampledRead);
                d.out = b.colorTarget(0, historyNext);
            },
            [self, frameData](const CloudResolveData& d, RgPassContext& ctx) {
                return cloudPostBindings(*self, *frameData, ctx.texture(d.current), ctx.texture(d.history), nvrhi::Format::UNKNOWN, ctx);
            });
        AddFullscreenPass<CloudCompositeData>(
            g, services, "CloudComposite", {"clouds_post.psCloudComposite", clouds.postLayout(), RgBlend::Premultiplied},
            [&](RgPassBuilder& b, CloudCompositeData& d) {
                d.clouds = b.read(historyNext, RgAccess::SampledRead);
                d.depth = b.read(depth, RgAccess::SampledRead);
                d.hdr = b.colorTarget(0, hdr);
            },
            [self, frameData](const CloudCompositeData& d, RgPassContext& ctx) {
                return cloudPostBindings(*self, *frameData, ctx.texture(d.clouds), ctx.texture(d.depth), nvrhi::Format::R32_FLOAT, ctx);
            });
        vp.cloudHistoryValid = true;
        vp.cloudLastFrame = frame;
    }
    vp.cloudPrevViewProj = vf.constants.viewProj;

    if (HasFlag(vp.flags, ViewportFlags::Grid)) {
        AddFullscreenPass<GridData>(
            g, services, "Grid", {"grid.psGrid", gpu.layoutViewTexture.Get(), RgBlend::Alpha},
            [&](RgPassBuilder& b, GridData& d) {
                d.hdr = b.colorTarget(0, hdr);
                d.depth = b.read(depth, RgAccess::SampledRead);
            },
            [self, frameData](const GridData& d, RgPassContext& ctx) {
                return viewBindings(ctx, *self, *frameData, ctx.texture(d.depth), nvrhi::Format::R32_FLOAT);
            });
    }

    // Picking: one extra id pass + a 1-texel copy, only on the frame a pick was requested.
    if (vp.pickState == Viewport::PickState::Requested) {
        if (instanceCount == 0) {
            vp.pickResult = 0;
            vp.pickState = Viewport::PickState::Ready;
        } else {
            if (!vp.pickStaging) {
                nvrhi::TextureDesc sd;
                sd.width = 1;
                sd.height = 1;
                sd.format = nvrhi::Format::RG32_UINT;
                sd.debugName = "pick readback";
                vp.pickStaging = device->nvrhi()->createStagingTexture(sd, nvrhi::CpuAccessMode::Read);
            }
            vf.pickStaging = vp.pickStaging.Get();
            vf.pickX = vp.pickX;
            vf.pickY = vp.pickY;
            RgTextureDesc idDesc;
            idDesc.width = vp.width;
            idDesc.height = vp.height;
            idDesc.format = nvrhi::Format::RG32_UINT;
            idDesc.renderTarget = true;
            idDesc.name = "PickIds";
            RgTextureDesc pdDesc = depthDesc;
            pdDesc.name = "PickDepth";
            RgTexture ids;
            g.addPass<PickData>(
                "PickIds",
                [&](RgPassBuilder& b, PickData& d) {
                    d.instances = b.read(instances, RgAccess::VertexRead);
                    ids = b.createTexture(idDesc);
                    d.ids = b.colorTarget(0, ids);
                    d.depth = b.depthTarget(b.createTexture(pdDesc));
                },
                [self, frameData](const PickData& d, RgPassContext& ctx) { recordPick(*self, *frameData, d, ctx); });
            g.addPass<PickReadbackData>(
                "PickReadback",
                [&](RgPassBuilder& b, PickReadbackData& d) {
                    d.ids = b.read(ids, RgAccess::CopySrc);
                    b.neverCull();
                },
                [frameData](const PickReadbackData& d, RgPassContext& ctx) {
                    nvrhi::TextureSlice src;
                    src.x = frameData->pickX;
                    src.y = frameData->pickY;
                    src.width = 1;
                    src.height = 1;
                    src.depth = 1;
                    nvrhi::TextureSlice dst;
                    dst.width = 1;
                    dst.height = 1;
                    dst.depth = 1;
                    ctx.cmd()->copyTexture(frameData->pickStaging, dst, ctx.texture(d.ids), src);
                });
            vp.pickState = Viewport::PickState::InFlight;
            vp.pickSubmittedFrame = frame;
        }
    }

    AddFullscreenPass<TonemapData>(
        g, services, "Tonemap", {"tonemap.psTonemap", gpu.layoutViewTexture.Get()},
        [&](RgPassBuilder& b, TonemapData& d) {
            d.hdr = b.read(hdr, RgAccess::SampledRead);
            d.output = b.colorTarget(0, output);
        },
        [self, frameData](const TonemapData& d, RgPassContext& ctx) {
            return viewBindings(ctx, *self, *frameData, ctx.texture(d.hdr), nvrhi::Format::UNKNOWN);
        });

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
