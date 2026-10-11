// The per-viewport render graph:
//
//   UploadInstances (copy)   scene instances -> transient vertex stream
//   DepthPrepass             depth of all opaque geometry (VS only)
//   ShadowCascades           sun shadow map: 4 cascades tiled in one depth atlas
//   FoliageTransmit          colour of the sun that leaks through the first leaf layer, same atlas layout as the shadow map
//   Opaque                   G-buffer fill (albedo, normal, material), depth-tested
//                            against the pre-pass so every pixel is shaded once
//   GtaoMain/GtaoBlur        ground-truth ambient occlusion from the depth + normal buffers
//   SkyEnv/EnvDown           the whole sky (atmosphere + clouds) as an octahedral image, then 4 blurrier levels (IBL)
//   ProbeClear/Capture/Light/Sh   (Probes viewports) refresh a few light probes: scene drawn from 6 faces, lit, projected on SH
//   Sky | ClearHdr           background into the HDR target (sky: precomputed atmosphere, empty-depth pixels only)
//   Deferred                 fullscreen lighting from the G-buffer + shadows + atmosphere -> HDR
//   CloudTrace/Resolve/Composite (volumetric clouds, sky viewports with the atmosphere on)
//                            half-resolution ray march, temporal accumulation, blend over the empty HDR pixels
//   WaterSkyProbe / WaterSceneCopy / WaterSurface (viewports with the Water flag)
//                            sky + clouds into a small octahedral map, copy of the HDR scene, then the sea drawn over it
//   Grid (editor views)      ground grid over the HDR color, occluded by depth
//   Tonemap                  HDR -> sRGB RGBA8, the viewport's output image
//
// Fullscreen passes go through AddFullscreenPass (rhi/render_graph_helpers.h).
// Pass bodies run on recording threads: they only read ViewFrame / the shared
// immutable GPU objects, and write through the command list they are given.

#include <algorithm>
#include <bit>
#include <cstdlib>

#include "foh/log.h"
#include "renderer_impl.h"

namespace foh {

namespace {

using Impl = Renderer::Impl;

constexpr nvrhi::Format kHdrFormat = nvrhi::Format::RGBA16_FLOAT;
constexpr nvrhi::Format kDepthFormat = nvrhi::Format::D32;
constexpr u32 kTransmitCascades = 2; // cascades with a foliage transmission pass (shaders/common/lighting.slangh reads the same count)

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
struct SkinUploadData {
    RgBuffer palettes;
    RgBuffer jobs;
};
struct SkinData {
    RgBuffer palettes;
    RgBuffer jobs;
    RgBuffer out; // the deformed vertices of every character, MeshVertex layout
};
struct DepthPrepassData {
    RgBuffer skinned; // invalid when nothing is skinned
    RgBuffer instances; // invalid when the scene has no meshes
    RgTexture depth;
};
struct ShadowData {
    RgBuffer skinned; // invalid when nothing is skinned
    RgBuffer instances;
    RgTexture atlas;
};
struct OpaqueData {
    RgBuffer skinned; // invalid when nothing is skinned
    RgBuffer instances;
    RgTexture albedo;
    RgTexture normal;
    RgTexture material;
    RgTexture depth;
};
struct TransmitData {
    RgBuffer skinned; // invalid when nothing is skinned
    RgBuffer instances;
    RgTexture atlas;  // the shadow atlas, read-only depth
    RgTexture color;
};
struct GtaoData {
    RgTexture depth;
    RgTexture normal;
    RgTexture noisy; // invalid for the main pass
    RgTexture out;
};
struct EnvData {
    RgTexture src; // invalid for the first level
    RgTexture dst;
};
struct ProbeClearData {
    RgTexture sh[4];
};
struct ProbeCaptureData {
    RgBuffer skinned;
    RgBuffer instances;
    RgTexture albedo;
    RgTexture normal;
    RgTexture material;
    RgTexture depth;
};
struct ProbeLightData {
    RgTexture albedo, normal, material, depth;
    RgTexture atlas;   // shadow atlas
    RgTexture foliage; // invalid when off
    RgTexture sh[4];   // the grid being refreshed: older captures feed the multi-bounce
    RgTexture lit;
};
struct ProbeShData {
    RgTexture lit;
    RgTexture sh[4];
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
    RgTexture gtao;    // the next four are invalid when their pass is off: stand-in textures are bound
    RgTexture foliage;
    RgTexture env[5];
    RgTexture sh[4];
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
struct SceneCopyData {
    RgTexture src;
    RgTexture dst;
};
struct WaterSurfaceData {
    RgTexture depth;
    RgTexture scene;
    RgTexture probe;
    RgTexture hdr;
};
struct GridData {
    RgTexture hdr;
    RgTexture depth;
};
struct PickData {
    RgBuffer skinned; // invalid when nothing is skinned
    RgBuffer instances;
    RgTexture ids;
    RgTexture depth;
};
struct PickReadbackData {
    RgTexture ids;
};
struct BloomData {
    RgTexture src;
    RgTexture dst;
};
struct ExposureClearData {
    RgBuffer bins;
};
struct HistogramData {
    RgTexture hdr;
    RgBuffer bins;
};
struct ExposureAverageData {
    RgBuffer bins;
    RgBuffer state;
};
struct TonemapData {
    RgTexture hdr;
    RgTexture bloom; // invalid when bloom is off
    RgBuffer exposure;
    RgTexture output;
};

// ---- pass bodies ----

// Batches are grouped by draw class (see material_store.h): opaque single-sided geometry takes the cheapest path
// everywhere (depth passes without a pixel shader), alpha-masked and double-sided materials get their own pipelines.
constexpr DrawClass kAllClasses[kDrawClassCount] = {DrawClass::Opaque, DrawClass::Masked, DrawClass::OpaqueDoubleSided,
                                                    DrawClass::MaskedDoubleSided};

nvrhi::RasterCullMode cullOf(DrawClass cls) { return IsDoubleSided(cls) ? nvrhi::RasterCullMode::None : nvrhi::RasterCullMode::Back; }

// Six clip planes from a view-projection matrix (Gribb-Hartmann; valid for the reverse-Z projection too, where near maps
// to 1 and far to 0: the inside test 0 <= z <= w is the same).
struct Frustum {
    // Plain floats and no vector-library calls in `sees`: it runs for every batch of every pass, and a Debug build pays
    // dearly for each tiny inline function there.
    f32 planes[6][4];
    explicit Frustum(const Mat4& m) {
        auto row = [&](int i) { return Vec4(m[0][i], m[1][i], m[2][i], m[3][i]); };
        const Vec4 r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
        const Vec4 p[6] = {r3 + r0, r3 - r0, r3 + r1, r3 - r1, r2, r3 - r2};
        for (int i = 0; i < 6; ++i) {
            const f32 inv = 1.0f / std::max(glm::length(Vec3(p[i])), 1e-6f);
            planes[i][0] = p[i].x * inv;
            planes[i][1] = p[i].y * inv;
            planes[i][2] = p[i].z * inv;
            planes[i][3] = p[i].w * inv;
        }
    }
    bool sees(const Vec4& sphere) const {
        const f32 x = sphere.x, y = sphere.y, z = sphere.z, w = -sphere.w;
        for (int i = 0; i < 6; ++i)
            if (planes[i][0] * x + planes[i][1] * y + planes[i][2] * z + planes[i][3] < w) return false;
        return true;
    }
};

// One instanced draw per mesh batch in [begin, end). Shared by the pre-pass, shadows, G-buffer and picking.
// `viewProj` (null = none) culls the static instance sets that carry a bounding sphere.
// `table` is the bindless texture table for passes that sample materials (null otherwise).
void drawBatches(Impl& s, const nvrhi::ViewportState& viewport, nvrhi::IFramebuffer* fb, nvrhi::IGraphicsPipeline* pso,
                 nvrhi::IBindingSet* set, nvrhi::IBindingSet* table, nvrhi::ICommandList* cmd, nvrhi::IBuffer* instances,
                 nvrhi::IBuffer* skinned, const SceneGpu& scene, u32 begin, u32 end, const Mat4* viewProj = nullptr, u32 skipFlags = 0) {
    if (begin >= end) return;
    const Frustum frustum(viewProj ? *viewProj : Mat4(1.0f));
    nvrhi::GraphicsState gs;
    gs.setPipeline(pso).setFramebuffer(fb).setViewport(viewport).addBindingSet(set);
    if (table) gs.addBindingSet(table);
    gs.addVertexBuffer({s.gpu.meshes.vertexBuffer(), 0, 0});
    gs.addVertexBuffer({instances, 1, 0});
    gs.setIndexBuffer({s.gpu.meshes.indexBuffer(), nvrhi::Format::R32_UINT, 0});
    cmd->setGraphicsState(gs);
    bool onSkinned = false;
    for (u32 i = begin; i < end; ++i) {
        const SceneGpu::Batch& b = scene.batches[i];
        if (b.flags & skipFlags) continue;
        if (viewProj && b.sphere.w > 0.0f && !frustum.sees(b.sphere)) continue;
        const MeshRange& r = s.gpu.meshes.range(b.mesh);
        const bool skin = b.skinBase >= 0 && skinned;
        if (b.skinBase >= 0 && !skinned) continue; // the pre-skinning pass did not run: nothing to draw
        if (skin != onSkinned) {
            // Static batches come first within a class, so this switches at most once per call.
            gs.vertexBuffers[0].buffer = skin ? skinned : s.gpu.meshes.vertexBuffer();
            cmd->setGraphicsState(gs);
            onSkinned = skin;
        }
        cmd->drawIndexed(nvrhi::DrawArguments()
                             .setVertexCount(r.indexCount)
                             .setInstanceCount(b.count)
                             .setStartIndexLocation(r.firstIndex)
                             .setStartVertexLocation(skin ? static_cast<u32>(b.skinBase) : static_cast<u32>(r.baseVertex))
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
enum class MeshPass { Depth, Shadow, GBuffer, ProbeGBuffer, Pick, Transmit };

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
        case MeshPass::ProbeGBuffer: {
            // A probe capture has no depth pre-pass, so unlike the camera's G-buffer it writes depth itself.
            const ShaderRef vs = s.shaders->get("opaque.vsOpaque", nvrhi::ShaderType::Vertex);
            const ShaderRef ps = s.shaders->get(masked ? "opaque.psOpaqueMasked" : "opaque.psOpaque", nvrhi::ShaderType::Pixel);
            if (!vs || !ps) return nullptr;
            return meshPipeline(s, fb, vs, ps, 12, true, cullOf(cls), true);
        }
        case MeshPass::Transmit: {
            // Only double-sided alpha-masked materials can be foliage; the pixel shader drops the ones that are not.
            if (cls != DrawClass::MaskedDoubleSided) return nullptr;
            const ShaderRef vs = s.shaders->get("shadow.vsShadowTransmit", nvrhi::ShaderType::Vertex);
            const ShaderRef ps = s.shaders->get("shadow.psShadowTransmit", nvrhi::ShaderType::Pixel);
            if (!vs || !ps) return nullptr;
            return meshPipeline(s, fb, vs, ps, 11, false, nvrhi::RasterCullMode::None, true);
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

// Characters (ADR 0009): one dispatch deforms every skinned mesh block of the scene into the skinned vertex buffer,
// which the depth, shadow, G-buffer and picking passes then draw like any other vertex buffer.
void recordSkin(Impl& s, const ViewFrame& vf, const SkinData& d, RgPassContext& ctx) {
    const SceneGpu& scene = *vf.scene;
    nvrhi::BindingSetDesc bd;
    bd.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(0, ctx.buffer(d.jobs)));
    bd.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(1, s.gpu.meshes.vertexBuffer()));
    bd.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(2, s.gpu.meshes.influenceBuffer()));
    bd.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(3, ctx.buffer(d.palettes)));
    bd.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(0, ctx.buffer(d.out)));
    RecordCompute(ctx, s.services(), {"skin.csSkin", s.gpu.layoutSkin}, ctx.bindingSet(bd, s.gpu.layoutSkin),
                  RgDispatch{scene.skinGroups, 1, 1});
}

void recordDepthPrepass(Impl& s, const ViewFrame& vf, const DepthPrepassData& d, RgPassContext& ctx) {
    nvrhi::ICommandList* cmd = ctx.cmd();
    // Reverse-Z: far is 0.
    cmd->clearDepthStencilTexture(ctx.texture(d.depth), nvrhi::AllSubresources, true, 0.0f, false, 0);
    if (!vf.scene || vf.scene->batches.empty() || !d.instances.valid()) return;

    nvrhi::IFramebuffer* fb = ctx.framebuffer();
    const SceneGpu& scene = *vf.scene;
    nvrhi::IBuffer* instances = ctx.buffer(d.instances);
    nvrhi::IBuffer* skinned = d.skinned.valid() ? ctx.buffer(d.skinned) : nullptr;
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
                    instances, skinned, scene, scene.classBegin[c], scene.classBegin[c + 1], &vf.constants.viewProj);
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
    nvrhi::IBuffer* skinned = d.skinned.valid() ? ctx.buffer(d.skinned) : nullptr;
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
                        instances, skinned, scene, scene.classBegin[c], scene.classBegin[c + 1], &vf.cascades.viewProj[cascade], kBatchNoShadow);
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
    nvrhi::IBuffer* skinned = d.skinned.valid() ? ctx.buffer(d.skinned) : nullptr;
    for (DrawClass cls : kAllClasses) {
        const u32 c = static_cast<u32>(cls);
        if (scene.classBegin[c] >= scene.classBegin[c + 1]) continue;
        nvrhi::IGraphicsPipeline* pso = classPipeline(s, MeshPass::GBuffer, cls, fb);
        if (!pso) continue;
        drawBatches(s, fullViewport(vf), fb, pso, set, s.gpu.textures.table(), cmd, instances, skinned, scene, scene.classBegin[c], scene.classBegin[c + 1],
                    &vf.constants.viewProj);
    }
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
    bd.addItem(nvrhi::BindingSetItem::Texture_SRV(6, s.clouds.weatherMap()));
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

// ---- water ----------------------------------------------------------------------------------------------------

// Volatile constant buffers of a viewport's water passes (the sky probe also reads the cloud constants, which exist
// even when the clouds are off: a zeroed struct switches the march off in the probe shader).
void ensureWaterBuffers(Impl& s, Viewport& vp) {
    nvrhi::IDevice* dev = s.device->nvrhi();
    auto make = [&](nvrhi::BufferHandle& handle, u32 size, const char* what) {
        if (handle) return;
        nvrhi::BufferDesc cb;
        cb.byteSize = size;
        cb.isConstantBuffer = true;
        cb.isVolatile = true;
        cb.maxVersions = 16;
        cb.debugName = vp.name + what;
        handle = dev->createBuffer(cb);
    };
    make(vp.waterCb, sizeof(WaterConstants), " (water constants)");
    make(vp.cloudCb, sizeof(CloudConstants), " (cloud constants)");
}

// The probe grid's textures and the two volatile constant buffers its passes write (created on first use).
bool ensureProbeResources(Impl& s, Viewport& vp) {
    nvrhi::IDevice* dev = s.device->nvrhi();
    if (!vp.probeCb) {
        nvrhi::BufferDesc cb;
        cb.byteSize = sizeof(ProbeConstants);
        cb.isConstantBuffer = true;
        cb.isVolatile = true;
        cb.maxVersions = 16;
        cb.debugName = vp.name + " (probe constants)";
        vp.probeCb = dev->createBuffer(cb);
        cb.byteSize = sizeof(ViewConstants);
        cb.maxVersions = 128; // one write per probe face drawn
        cb.debugName = vp.name + " (probe face constants)";
        vp.probeViewCb = dev->createBuffer(cb);
    }
    return vp.probeCb && vp.probeViewCb && vp.probes.ensureTextures(dev);
}

nvrhi::IBindingSet* waterSurfaceBindings(Impl& s, const ViewFrame& vf, const WaterSurfaceData& d, RgPassContext& ctx) {
    ctx.cmd()->writeBuffer(vf.viewCb, &vf.constants, sizeof(ViewConstants));
    ctx.cmd()->writeBuffer(vf.waterCb, &vf.water, sizeof(WaterConstants));
    nvrhi::BindingSetDesc bd;
    bd.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, vf.viewCb));
    bd.addItem(nvrhi::BindingSetItem::ConstantBuffer(1, vf.waterCb));
    bd.addItem(nvrhi::BindingSetItem::Texture_SRV(0, ctx.texture(d.depth), nvrhi::Format::R32_FLOAT));
    bd.addItem(nvrhi::BindingSetItem::Texture_SRV(1, ctx.texture(d.scene)));
    bd.addItem(nvrhi::BindingSetItem::Texture_SRV(2, ctx.texture(d.probe)));
    bd.addItem(nvrhi::BindingSetItem::Texture_SRV(3, s.water.displacement()));
    bd.addItem(nvrhi::BindingSetItem::Texture_SRV(4, s.water.slope()));
    bd.addItem(nvrhi::BindingSetItem::Texture_SRV(5, s.water.bed()));
    bd.addItem(nvrhi::BindingSetItem::Texture_SRV(6, s.atmosphere.transmittance()));
    bd.addItem(nvrhi::BindingSetItem::Texture_SRV(7, s.atmosphere.scattering()));
    bd.addItem(nvrhi::BindingSetItem::Texture_SRV(8, s.atmosphere.irradiance()));
    bd.addItem(nvrhi::BindingSetItem::Sampler(0, s.water.waveSampler()));
    bd.addItem(nvrhi::BindingSetItem::Sampler(1, s.atmosphere.sampler()));
    return ctx.bindingSet(bd, s.water.surfaceLayout());
}

// The clipmap: the rings (coarsest first, one instanced draw) and then the full innermost grid. No vertex buffer:
// the vertices are generated from SV_VertexID. The finest geometry is drawn last, so it wins where rings overlap.
void recordWaterSurface(Impl& s, const ViewFrame& vf, const WaterSurfaceData& d, RgPassContext& ctx) {
    const ShaderRef vsRing = s.shaders->get("water.vsWaterRing", nvrhi::ShaderType::Vertex);
    const ShaderRef vsCenter = s.shaders->get("water.vsWaterCenter", nvrhi::ShaderType::Vertex);
    const ShaderRef ps = s.shaders->get("water.psWater", nvrhi::ShaderType::Pixel);
    if (!vsRing || !vsCenter || !ps) return;

    nvrhi::IFramebuffer* fb = ctx.framebuffer();
    nvrhi::IBindingLayout* layout = s.water.surfaceLayout();
    auto pipeline = [&](const ShaderRef& vs) {
        return s.pipelines->graphics(HashCombine(shaderKey(vs, ps, 0x3A7E), reinterpret_cast<uintptr_t>(layout)), fb, [&] {
            nvrhi::GraphicsPipelineDesc pd;
            pd.VS = vs.shader;
            pd.PS = ps.shader;
            pd.bindingLayouts = {layout};
            pd.renderState.depthStencilState.depthTestEnable = false; // the pixel shader tests the scene depth itself
            pd.renderState.depthStencilState.depthWriteEnable = false;
            pd.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
            return pd;
        });
    };
    nvrhi::IGraphicsPipeline* ringPso = pipeline(vsRing);
    nvrhi::IGraphicsPipeline* centerPso = pipeline(vsCenter);
    if (!ringPso || !centerPso) return;

    nvrhi::IBindingSet* set = waterSurfaceBindings(s, vf, d, ctx);
    nvrhi::ICommandList* cmd = ctx.cmd();
    auto draw = [&](nvrhi::IGraphicsPipeline* pso, nvrhi::IBuffer* indices, u32 indexCount, u32 instances) {
        nvrhi::GraphicsState gs;
        gs.setPipeline(pso).setFramebuffer(fb).setViewport(fullViewport(vf)).addBindingSet(set);
        gs.setIndexBuffer({indices, nvrhi::Format::R16_UINT, 0});
        cmd->setGraphicsState(gs);
        cmd->drawIndexed(nvrhi::DrawArguments().setVertexCount(indexCount).setInstanceCount(instances));
    };
    if (vf.waterRings > 1) draw(ringPso, s.water.ringIndices(), s.water.ringIndexCount(), vf.waterRings - 1);
    draw(centerPso, s.water.centerIndices(), s.water.centerIndexCount(), 1);
}

// ---- indirect light and small-scale shadowing -------------------------------------------------------------------

// Everything the lighting shaders (deferred.slang, probe_light.slang) read through layoutDeferred. A null texture
// is replaced by a stand-in: the view constants carry a flag per feature, so the shader never reads it.
struct LightingInputs {
    nvrhi::ITexture* albedo = nullptr;
    nvrhi::ITexture* normal = nullptr;
    nvrhi::ITexture* material = nullptr;
    nvrhi::ITexture* depth = nullptr;
    nvrhi::ITexture* atlas = nullptr;
    nvrhi::ITexture* gtao = nullptr;
    nvrhi::ITexture* foliage = nullptr;
    nvrhi::ITexture* env[5] = {};
    nvrhi::ITexture* sh[4] = {};
};

nvrhi::ITexture* orStandIn(RgPassContext& ctx, RgTexture handle, nvrhi::ITexture* standIn) {
    return handle.valid() ? ctx.texture(handle) : standIn;
}

nvrhi::BindingSetDesc lightingBindingDesc(Impl& s, const ViewFrame& vf, const LightingInputs& in) {
    nvrhi::BindingSetDesc bd;
    nvrhi::ITexture* flat = s.gpu.dummy2D.Get();
    bd.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, vf.viewCb));
    bd.addItem(nvrhi::BindingSetItem::Texture_SRV(0, in.albedo));
    bd.addItem(nvrhi::BindingSetItem::Texture_SRV(1, in.normal));
    bd.addItem(nvrhi::BindingSetItem::Texture_SRV(2, in.material));
    bd.addItem(nvrhi::BindingSetItem::Texture_SRV(3, in.depth, nvrhi::Format::R32_FLOAT));
    bd.addItem(nvrhi::BindingSetItem::Texture_SRV(4, in.atlas, nvrhi::Format::R32_FLOAT));
    bd.addItem(nvrhi::BindingSetItem::Texture_SRV(5, s.atmosphere.transmittance()));
    bd.addItem(nvrhi::BindingSetItem::Texture_SRV(6, s.atmosphere.scattering()));
    bd.addItem(nvrhi::BindingSetItem::Texture_SRV(7, s.atmosphere.irradiance()));
    bd.addItem(nvrhi::BindingSetItem::Texture_SRV(8, in.gtao ? in.gtao : flat));
    bd.addItem(nvrhi::BindingSetItem::Texture_SRV(9, in.foliage ? in.foliage : flat));
    for (u32 i = 0; i < 5; ++i) bd.addItem(nvrhi::BindingSetItem::Texture_SRV(10 + i, in.env[i] ? in.env[i] : flat));
    for (u32 i = 0; i < 4; ++i) bd.addItem(nvrhi::BindingSetItem::Texture_SRV(15 + i, in.sh[i] ? in.sh[i] : s.gpu.dummyArray.Get()));
    nvrhi::ITexture* bed = s.water.bed() ? s.water.bed() : flat;
    bd.addItem(nvrhi::BindingSetItem::Texture_SRV(19, bed, nvrhi::Format::R32_FLOAT));
    bd.addItem(nvrhi::BindingSetItem::Sampler(0, s.gpu.shadowSampler));
    bd.addItem(nvrhi::BindingSetItem::Sampler(1, s.atmosphere.sampler()));
    bd.addItem(nvrhi::BindingSetItem::Sampler(2, s.gpu.probeSampler));
    return bd;
}

nvrhi::IBindingSet* deferredBindings(Impl& s, const ViewFrame& vf, const DeferredData& d, RgPassContext& ctx) {
    ctx.cmd()->writeBuffer(vf.viewCb, &vf.constants, sizeof(ViewConstants));
    LightingInputs in;
    in.albedo = ctx.texture(d.albedo);
    in.normal = ctx.texture(d.normal);
    in.material = ctx.texture(d.material);
    in.depth = ctx.texture(d.depth);
    in.atlas = ctx.texture(d.atlas);
    in.gtao = orStandIn(ctx, d.gtao, nullptr);
    in.foliage = orStandIn(ctx, d.foliage, nullptr);
    for (u32 i = 0; i < 5; ++i) in.env[i] = orStandIn(ctx, d.env[i], nullptr);
    for (u32 i = 0; i < 4; ++i) in.sh[i] = orStandIn(ctx, d.sh[i], nullptr);
    return ctx.bindingSet(lightingBindingDesc(s, vf, in), s.gpu.layoutDeferred);
}

// The same cascades as the shadow pass, drawn again with a colour target: where a foliage leaf is the first thing the sun
// hits, the pixel keeps the colour of the light that gets through it.
void recordTransmit(Impl& s, const ViewFrame& vf, const TransmitData& d, RgPassContext& ctx) {
    nvrhi::ICommandList* cmd = ctx.cmd();
    cmd->clearTextureFloat(ctx.texture(d.color), nvrhi::AllSubresources, nvrhi::Color(0.0f));
    if (!vf.shadowsOn || !vf.scene || vf.scene->batches.empty() || !d.instances.valid()) return;
    const SceneGpu& scene = *vf.scene;
    const u32 c = static_cast<u32>(DrawClass::MaskedDoubleSided);
    if (scene.classBegin[c] >= scene.classBegin[c + 1]) return;

    nvrhi::IFramebuffer* fb = ctx.framebuffer();
    nvrhi::IGraphicsPipeline* pso = classPipeline(s, MeshPass::Transmit, DrawClass::MaskedDoubleSided, fb);
    if (!pso) return;
    nvrhi::IBuffer* instances = ctx.buffer(d.instances);
    nvrhi::IBuffer* skinned = d.skinned.valid() ? ctx.buffer(d.skinned) : nullptr;
    nvrhi::IBindingSet* set = materialBindings(ctx, s, vf.shadowCb);
    const f32 tile = static_cast<f32>(vf.shadowTile);
    // Only the near cascades: far canopy is lit by the plain shadow term, and this pass redraws every leaf.
    for (u32 cascade = 0; cascade < kTransmitCascades; ++cascade) {
        cmd->writeBuffer(vf.shadowCb, &vf.cascades.viewProj[cascade], sizeof(Mat4));
        const f32 x = static_cast<f32>(cascade & 1) * tile;
        const f32 y = static_cast<f32>(cascade >> 1) * tile;
        const nvrhi::ViewportState viewport =
            nvrhi::ViewportState().addViewportAndScissorRect(nvrhi::Viewport(x, x + tile, y, y + tile, 0.0f, 1.0f));
        drawBatches(s, viewport, fb, pso, set, s.gpu.textures.table(), cmd, instances, skinned, scene, scene.classBegin[c], scene.classBegin[c + 1],
                    &vf.cascades.viewProj[cascade], kBatchNoShadow);
    }
}

nvrhi::IBindingSet* gtaoBindings(Impl& s, const ViewFrame& vf, const GtaoData& d, RgPassContext& ctx) {
    ctx.cmd()->writeBuffer(vf.viewCb, &vf.constants, sizeof(ViewConstants));
    nvrhi::BindingSetDesc bd;
    bd.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, vf.viewCb));
    bd.addItem(nvrhi::BindingSetItem::Texture_SRV(0, ctx.texture(d.depth), nvrhi::Format::R32_FLOAT));
    bd.addItem(nvrhi::BindingSetItem::Texture_SRV(1, ctx.texture(d.normal)));
    // The main pass has no noisy input: any texture does for t2.
    bd.addItem(nvrhi::BindingSetItem::Texture_SRV(2, d.noisy.valid() ? ctx.texture(d.noisy) : s.gpu.dummy2D.Get()));
    return ctx.bindingSet(bd, s.gpu.layoutGtao);
}

// One probe capture: the scene from the probe's position, 6 faces of kProbeFace^2 pixels per probe, side by side in one
// atlas (face f of job j at tile (f, j)). The G-buffer pass of the camera is reused as it is, drawn with the face's
// matrices; whatever is not covered keeps depth 0 and is treated as sky by the lighting.
void recordProbeCapture(Impl& s, const ViewFrame& vf, const ProbeCaptureData& d, RgPassContext& ctx) {
    nvrhi::ICommandList* cmd = ctx.cmd();
    cmd->clearDepthStencilTexture(ctx.texture(d.depth), nvrhi::AllSubresources, true, 0.0f, false, 0);
    if (vf.probeJobs.empty() || !vf.scene || vf.scene->batches.empty() || !d.instances.valid()) return;
    const SceneGpu& scene = *vf.scene;
    nvrhi::IFramebuffer* fb = ctx.framebuffer();
    nvrhi::IBuffer* instances = ctx.buffer(d.instances);
    nvrhi::IBuffer* skinned = d.skinned.valid() ? ctx.buffer(d.skinned) : nullptr;
    nvrhi::IGraphicsPipeline* pso[kDrawClassCount];
    for (DrawClass cls : kAllClasses) {
        const u32 c = static_cast<u32>(cls);
        pso[c] = scene.classBegin[c] < scene.classBegin[c + 1] ? classPipeline(s, MeshPass::ProbeGBuffer, cls, fb) : nullptr;
    }
    nvrhi::IBindingSet* set = materialBindings(ctx, s, vf.probeViewCb);
    const f32 face = static_cast<f32>(kProbeFace);
    for (u32 j = 0; j < vf.probeJobs.size(); ++j) {
        Mat4 view[kProbeFaces], proj;
        ProbeFaceViews(vf.probeJobs[j].position, view, proj);
        for (u32 f = 0; f < kProbeFaces; ++f) {
            ViewConstants c = vf.constants;
            c.view = view[f];
            c.proj = proj;
            c.viewProj = proj * view[f];
            c.invViewProj = glm::inverse(c.viewProj);
            c.cameraPos = Vec4(vf.probeJobs[j].position, 1.0f);
            cmd->writeBuffer(vf.probeViewCb, &c, sizeof(ViewConstants));
            const f32 x = static_cast<f32>(f) * face, y = static_cast<f32>(j) * face;
            const nvrhi::ViewportState viewport =
                nvrhi::ViewportState().addViewportAndScissorRect(nvrhi::Viewport(x, x + face, y, y + face, 0.0f, 1.0f));
            for (DrawClass cls : kAllClasses) {
                const u32 k = static_cast<u32>(cls);
                if (!pso[k]) continue;
                drawBatches(s, viewport, fb, pso[k], set, s.gpu.textures.table(), cmd, instances, skinned, scene, scene.classBegin[k],
                            scene.classBegin[k + 1], &c.viewProj, kBatchNoProbe);
            }
        }
    }
}

nvrhi::IBindingSet* probeLightBindings(Impl& s, const ViewFrame& vf, const ProbeLightData& d, RgPassContext& ctx) {
    ctx.cmd()->writeBuffer(vf.viewCb, &vf.constants, sizeof(ViewConstants));
    ctx.cmd()->writeBuffer(vf.probeCb, &vf.probe, sizeof(ProbeConstants));
    LightingInputs in;
    in.albedo = ctx.texture(d.albedo);
    in.normal = ctx.texture(d.normal);
    in.material = ctx.texture(d.material);
    in.depth = ctx.texture(d.depth);
    in.atlas = ctx.texture(d.atlas);
    in.foliage = orStandIn(ctx, d.foliage, nullptr);
    for (u32 i = 0; i < 4; ++i) in.sh[i] = ctx.texture(d.sh[i]);
    nvrhi::BindingSetDesc bd = lightingBindingDesc(s, vf, in);
    bd.addItem(nvrhi::BindingSetItem::ConstantBuffer(1, vf.probeCb));
    return ctx.bindingSet(bd, s.gpu.layoutProbeLight);
}

void recordProbeSh(Impl& s, const ViewFrame& vf, const ProbeShData& d, RgPassContext& ctx) {
    ctx.cmd()->writeBuffer(vf.probeCb, &vf.probe, sizeof(ProbeConstants));
    nvrhi::BindingSetDesc bd;
    bd.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, vf.probeCb));
    bd.addItem(nvrhi::BindingSetItem::Texture_SRV(0, ctx.texture(d.lit)));
    for (u32 i = 0; i < 4; ++i) bd.addItem(nvrhi::BindingSetItem::Texture_UAV(i, ctx.texture(d.sh[i])));
    RecordCompute(ctx, s.services(), {"probe_sh.csProbeSh", s.gpu.layoutProbeSh}, ctx.bindingSet(bd, s.gpu.layoutProbeSh),
                  RgDispatch{static_cast<u32>(vf.probeJobs.size()), 1, 1});
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
    nvrhi::IBuffer* skinned = d.skinned.valid() ? ctx.buffer(d.skinned) : nullptr;
    for (DrawClass cls : kAllClasses) {
        const u32 c = static_cast<u32>(cls);
        if (scene.classBegin[c] >= scene.classBegin[c + 1]) continue;
        nvrhi::IGraphicsPipeline* pso = classPipeline(s, MeshPass::Pick, cls, fb);
        if (!pso) continue;
        drawBatches(s, fullViewport(vf), fb, pso, set, s.gpu.textures.table(), cmd, instances, skinned, scene, scene.classBegin[c], scene.classBegin[c + 1],
                    &vf.constants.viewProj);
    }
}

// ---- post-processing ----------------------------------------------------------------------------------------------

// Every post pass writes the constants itself: the buffer is volatile, so a write belongs to one command list.
void writePostConstants(const ViewFrame& vf, RgPassContext& ctx) {
    ctx.cmd()->writeBuffer(vf.postCb, &vf.post, sizeof(PostConstants));
}

nvrhi::IBindingSet* bloomBindings(Impl& s, nvrhi::ITexture* source, RgPassContext& ctx) {
    nvrhi::BindingSetDesc bd;
    bd.addItem(nvrhi::BindingSetItem::Texture_SRV(0, source));
    bd.addItem(nvrhi::BindingSetItem::Sampler(0, s.post.sampler()));
    return ctx.bindingSet(bd, s.post.bloomLayout());
}

void recordExposureClear(const ExposureClearData& d, RgPassContext& ctx) {
    static const u32 kZeros[kHistogramBins] = {};
    ctx.cmd()->writeBuffer(ctx.buffer(d.bins), kZeros, sizeof(kZeros));
}

void recordHistogram(Impl& s, const ViewFrame& vf, const HistogramData& d, RgPassContext& ctx) {
    writePostConstants(vf, ctx);
    nvrhi::BindingSetDesc bd;
    bd.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, vf.postCb));
    bd.addItem(nvrhi::BindingSetItem::Texture_SRV(0, ctx.texture(d.hdr)));
    bd.addItem(nvrhi::BindingSetItem::Sampler(0, s.post.sampler()));
    bd.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(0, ctx.buffer(d.bins)));
    // One thread per 2x2 block of the image, in 16x16 groups (csHistogram).
    RecordCompute(ctx, s.services(), {"exposure.csHistogram", s.post.histogramLayout()},
                  ctx.bindingSet(bd, s.post.histogramLayout()),
                  DispatchForImage((vf.width + 1) / 2, (vf.height + 1) / 2, 16));
}

void recordExposureAverage(Impl& s, const ViewFrame& vf, const ExposureAverageData& d, RgPassContext& ctx) {
    writePostConstants(vf, ctx);
    nvrhi::BindingSetDesc bd;
    bd.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, vf.postCb));
    bd.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(1, ctx.buffer(d.bins)));
    bd.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(1, ctx.buffer(d.state)));
    RecordCompute(ctx, s.services(), {"exposure.csAverage", s.post.averageLayout()},
                  ctx.bindingSet(bd, s.post.averageLayout()), RgDispatch{});
}

nvrhi::IBindingSet* tonemapBindings(Impl& s, const ViewFrame& vf, const TonemapData& d, RgPassContext& ctx) {
    ctx.cmd()->writeBuffer(vf.viewCb, &vf.constants, sizeof(ViewConstants));
    writePostConstants(vf, ctx);
    nvrhi::ITexture* hdr = ctx.texture(d.hdr);
    nvrhi::BindingSetDesc bd;
    bd.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, vf.viewCb));
    bd.addItem(nvrhi::BindingSetItem::ConstantBuffer(1, vf.postCb));
    bd.addItem(nvrhi::BindingSetItem::Texture_SRV(0, hdr));
    bd.addItem(nvrhi::BindingSetItem::Texture_SRV(1, d.bloom.valid() ? ctx.texture(d.bloom) : hdr)); // strength is 0 without bloom
    bd.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(2, ctx.buffer(d.exposure)));
    bd.addItem(nvrhi::BindingSetItem::Sampler(0, s.post.sampler()));
    return ctx.bindingSet(bd, s.post.tonemapLayout());
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
    nvrhi::TextureHandle transmit = target(nvrhi::Format::RGBA8_UNORM, false);

    nvrhi::FramebufferHandle depthFb = dev->createFramebuffer(nvrhi::FramebufferDesc().setDepthAttachment(depth));
    nvrhi::FramebufferHandle gbufferFb = dev->createFramebuffer(nvrhi::FramebufferDesc()
                                                                    .addColorAttachment(albedo)
                                                                    .addColorAttachment(normal)
                                                                    .addColorAttachment(material)
                                                                    .setDepthAttachment(depth));
    nvrhi::FramebufferHandle pickFb = dev->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(ids).setDepthAttachment(depth));
    nvrhi::FramebufferHandle transmitFb = dev->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(transmit).setDepthAttachment(depth));

    u32 created = 0;
    for (DrawClass cls : kAllClasses) {
        for (auto [pass, fb] : {std::pair{MeshPass::Depth, depthFb.Get()}, std::pair{MeshPass::Shadow, depthFb.Get()},
                                std::pair{MeshPass::GBuffer, gbufferFb.Get()}, std::pair{MeshPass::ProbeGBuffer, gbufferFb.Get()},
                                std::pair{MeshPass::Pick, pickFb.Get()},
                                std::pair{MeshPass::Transmit, transmitFb.Get()}}) {
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

    // Post-processing: the exposure adapts over time, so it needs the time since this viewport's last frame. A
    // viewport that was not drawn last frame (hidden, auto exposure was off) has nothing to adapt from and snaps.
    vf.postCb = vp.postCb.Get();
    vf.exposureState = vp.exposureState.Get();
    vf.bloomLevels = settings.bloom && settings.bloomStrength > 0.0f ? BloomLevelsFor(settings, vp.width, vp.height) : 0;
    {
        const bool resetExposure = !vp.exposureValid || vp.exposureLastFrame + 1 != frame;
        const f32 dt = std::clamp(time - vp.exposureLastTime, 0.0f, 0.25f);
        FillPostConstants(vf.post, settings, vp.width, vp.height, dt, resetExposure, vf.bloomLevels > 0);
        vp.exposureValid = settings.autoExposure;
        if (settings.autoExposure) {
            vp.exposureLastFrame = frame;
            vp.exposureLastTime = time;
        }
    }
    // Sky light and aerial perspective need the tables, a sun to light them, and a view that draws the sky.
    atmosphere.fillView(vf.constants);
    if (!settings.atmosphere || vf.constants.sunDirection.w < 0.5f || !HasFlag(vp.flags, ViewportFlags::Sky)) vf.constants.atmosphere1.y = 0.0f;

    // Indirect light and small-scale shadowing: which passes run for this view, and the flags the shaders read.
    const bool sceneLit = vf.scene && !vf.scene->instances.empty();
    const bool atmosphereOn = vf.constants.atmosphere1.y > 0.5f;
    vf.gtaoOn = sceneLit && settings.gtao;
    vf.transmitOn = sceneLit && vf.shadowsOn && settings.canopyTransmission > 0.0f;
    vf.envOn = sceneLit && atmosphereOn && settings.ibl;
    vf.constants.ssao = Vec4(vf.gtaoOn ? 1.0f : 0.0f, settings.gtaoStrength, settings.contactShadows ? settings.contactShadowLength : 0.0f,
                             settings.gtaoRadius);
    vf.constants.canopy = Vec4(vf.transmitOn ? settings.canopyTransmission : 0.0f, vf.transmitOn ? 1.0f : 0.0f, 0.0f, 0.0f);
    vf.constants.ibl = Vec4(vf.envOn ? 1.0f : 0.0f, settings.iblIntensity, 0.0f, settings.probeSpacing);
    vf.probeJobs.clear();
    vf.probesOn = false;
    // Probes sit at a fixed height above the terrain, so they need its height field (the water's sea floor); a scene
    // without one (previews, test scenes) keeps the sky model for its ambient light.
    if (sceneLit && atmosphereOn && settings.probes && HasFlag(vp.flags, ViewportFlags::Probes) && water.hasBed() &&
        ensureProbeResources(*this, vp)) {
        vf.probesOn = true;
        vf.probeCb = vp.probeCb.Get();
        vf.probeViewCb = vp.probeViewCb.Get();
        // The grid still follows the camera every frame; only the captures (the expensive part) are spread out.
        vp.probes.update(view.position, settings.probeSpacing, frame, frame % settings.probeInterval == 0 ? settings.probesPerFrame : 0u,
                         [&](f32 x, f32 z) { return water.groundAt(x, z); }, vf.probeJobs);
        vf.probe = ProbeConstants{};
        for (u32 j = 0; j < vf.probeJobs.size(); ++j) {
            const ProbeJob& job = vf.probeJobs[j];
            Mat4 faceView[kProbeFaces], faceProj;
            ProbeFaceViews(job.position, faceView, faceProj);
            for (u32 f = 0; f < kProbeFaces; ++f) vf.probe.invViewProj[j * kProbeFaces + f] = glm::inverse(faceProj * faceView[f]);
            vf.probe.origin[j] = Vec4(job.position, 1.0f);
            vf.probe.slot[j][0] = job.x;
            vf.probe.slot[j][1] = job.z;
            vf.probe.slot[j][2] = job.layer;
            vf.probe.slot[j][3] = job.refresh ? 1u : 0u;
        }
        vf.probe.count[0] = static_cast<u32>(vf.probeJobs.size());
        const Vec2 centre = vp.probes.center();
        vf.constants.ibl.z = 1.0f;
        vf.constants.probeA = Vec4(centre.x, centre.y, static_cast<f32>(kProbeGridDim), settings.probeIntensity);
        vf.constants.probeB = Vec4(kProbeLayerHeight[0], kProbeLayerHeight[1], 0.0f, 0.0f);
        vf.constants.bed = water.bedMapping();
    }

    // Volumetric clouds over the empty pixels (passes below): the constants and the temporal history are set up here,
    // because the sky environment reads them before the lighting pass.
    const bool cloudsOn = clouds.ready() && clouds.desc().enabled && HasFlag(vp.flags, ViewportFlags::Sky) &&
                          vf.constants.atmosphere1.y > 0.5f;
    if (cloudsOn) {
        ensureCloudTargets(*this, vp, std::max(1u, (vp.width + 1) / 2), std::max(1u, (vp.height + 1) / 2));
        // History from a frame that was not the previous one (viewport hidden, camera cut) is not trusted.
        if (vp.cloudLastFrame + 1 != frame) vp.cloudHistoryValid = false;
        clouds.fill(vf.clouds, time, frame, vp.cloudHistoryValid ? 0.12f : 1.0f);
        vf.clouds.prevViewProj = vp.cloudPrevViewProj;
        vf.cloudCb = vp.cloudCb.Get();
    } else if (vf.envOn) {
        // The sky environment still marches the (zeroed: off) cloud constants.
        ensureWaterBuffers(*this, vp);
        vf.cloudCb = vp.cloudCb.Get();
        vf.clouds = CloudConstants{};
    }

    RenderGraph& g = *vp.graph;
    g.setGpuTimers(settings.gpuTimers && vp.id == gpuTimerViewport);
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
    RgTexture gtaoTex, foliageTex; // invalid unless their pass runs
    RgTexture envTex[5], probeSh[4];
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

    // Characters: upload their pose, deform their meshes, and let every geometry pass below draw the result.
    RgBuffer skinned;
    const bool skinning = vf.scene && !vf.scene->skinJobs.empty() && instanceCount > 0;
    if (skinning) {
        auto sized = [](size_t count, size_t stride, const char* name) {
            RgBufferDesc bd;
            bd.structStride = static_cast<u32>(stride);
            bd.byteSize = std::bit_ceil(std::max<size_t>(count, 64)) * stride; // size classes: the pooled buffer stays stable
            bd.name = name;
            return bd;
        };
        const SceneGpu& sg = *vf.scene;
        const RgBufferDesc paletteDesc = sized(sg.palettes.size(), sizeof(SkinMatrix), "SkinPalettes");
        const RgBufferDesc jobDesc = sized(sg.skinJobs.size(), sizeof(SceneGpu::SkinJob), "SkinJobs");
        RgBufferDesc outDesc = sized(sg.skinnedVertexCount, sizeof(MeshVertex), "SkinnedVertices");
        outDesc.storage = true;
        outDesc.vertex = true;
        RgBuffer paletteBuffer, jobBuffer;
        g.addPass<SkinUploadData>(
            "UploadSkinning",
            [&](RgPassBuilder& b, SkinUploadData& d) {
                paletteBuffer = b.createBuffer(paletteDesc);
                jobBuffer = b.createBuffer(jobDesc);
                d.palettes = b.write(paletteBuffer, RgAccess::CopyDst);
                d.jobs = b.write(jobBuffer, RgAccess::CopyDst);
            },
            [frameData](const SkinUploadData& d, RgPassContext& ctx) {
                const SceneGpu& scene = *frameData->scene;
                ctx.cmd()->writeBuffer(ctx.buffer(d.palettes), scene.palettes.data(), scene.palettes.size() * sizeof(SkinMatrix));
                ctx.cmd()->writeBuffer(ctx.buffer(d.jobs), scene.skinJobs.data(), scene.skinJobs.size() * sizeof(SceneGpu::SkinJob));
            });
        g.addPass<SkinData>(
            "SkinMeshes",
            [&](RgPassBuilder& b, SkinData& d) {
                d.palettes = b.read(paletteBuffer, RgAccess::SampledRead);
                d.jobs = b.read(jobBuffer, RgAccess::SampledRead);
                skinned = b.createBuffer(outDesc);
                d.out = b.write(skinned, RgAccess::StorageWrite);
            },
            [self, frameData](const SkinData& d, RgPassContext& ctx) { recordSkin(*self, *frameData, d, ctx); });
    }

    g.addPass<DepthPrepassData>(
        "DepthPrepass",
        [&](RgPassBuilder& b, DepthPrepassData& d) {
            depth = b.createTexture(depthDesc);
            d.depth = b.depthTarget(depth);
            if (instanceCount > 0) d.instances = b.read(instances, RgAccess::VertexRead);
            if (skinning) d.skinned = b.read(skinned, RgAccess::VertexRead);
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
                if (skinning) d.skinned = b.read(skinned, RgAccess::VertexRead);
            },
            [self, frameData](const ShadowData& d, RgPassContext& ctx) { recordShadows(*self, *frameData, d, ctx); });

        if (vf.transmitOn) {
            RgTextureDesc transmitDesc = atlasDesc;
            transmitDesc.format = nvrhi::Format::RGBA8_UNORM;
            transmitDesc.useClearValue = true; // declared up front: the clear below is then a fast one
            transmitDesc.clearValue = nvrhi::Color(0.0f);
            transmitDesc.name = "FoliageTransmission";
            g.addPass<TransmitData>(
                "FoliageTransmit",
                [&](RgPassBuilder& b, TransmitData& d) {
                    foliageTex = b.createTexture(transmitDesc);
                    d.color = b.colorTarget(0, foliageTex);
                    d.atlas = b.depthTarget(atlas, true);
                    d.instances = b.read(instances, RgAccess::VertexRead);
                    if (skinning) d.skinned = b.read(skinned, RgAccess::VertexRead);
                },
                [self, frameData](const TransmitData& d, RgPassContext& ctx) { recordTransmit(*self, *frameData, d, ctx); });
        }

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
                if (skinning) d.skinned = b.read(skinned, RgAccess::VertexRead);
            },
            [self, frameData](const OpaqueData& d, RgPassContext& ctx) { recordOpaque(*self, *frameData, d, ctx); });
    }

    // Indirect light: ambient occlusion from the depth + normal buffers, the sky as an image to reflect, and a few light
    // probes refreshed this frame. The lighting pass below reads all of it.
    if (instanceCount > 0) {
        if (vf.gtaoOn) {
            RgTextureDesc aoDesc = hdrDesc;
            aoDesc.useClearValue = false;
            aoDesc.format = nvrhi::Format::R8_UNORM;
            aoDesc.name = "GtaoRaw";
            RgTexture raw;
            AddFullscreenPass<GtaoData>(
                g, services, "GtaoMain", {"gtao.psGtao", gpu.layoutGtao.Get()},
                [&](RgPassBuilder& b, GtaoData& d) {
                    d.depth = b.read(depth, RgAccess::SampledRead);
                    d.normal = b.read(normal, RgAccess::SampledRead);
                    raw = b.createTexture(aoDesc);
                    d.out = b.colorTarget(0, raw);
                },
                [self, frameData](const GtaoData& d, RgPassContext& ctx) { return gtaoBindings(*self, *frameData, d, ctx); });
            aoDesc.name = "GtaoBlurred";
            AddFullscreenPass<GtaoData>(
                g, services, "GtaoBlur", {"gtao.psGtaoBlur", gpu.layoutGtao.Get()},
                [&](RgPassBuilder& b, GtaoData& d) {
                    d.depth = b.read(depth, RgAccess::SampledRead);
                    d.normal = b.read(normal, RgAccess::SampledRead);
                    d.noisy = b.read(raw, RgAccess::SampledRead);
                    gtaoTex = b.createTexture(aoDesc);
                    d.out = b.colorTarget(0, gtaoTex);
                },
                [self, frameData](const GtaoData& d, RgPassContext& ctx) { return gtaoBindings(*self, *frameData, d, ctx); });
        }

        if (vf.envOn) {
            // Level 0 is the sky itself (128^2 octahedral); every further level is half the size and about twice as blurry.
            RgTextureDesc envDesc;
            envDesc.width = envDesc.height = kEnvBaseSize;
            envDesc.format = nvrhi::Format::R11G11B10_FLOAT;
            envDesc.renderTarget = true;
            envDesc.name = "SkyEnvironment";
            AddFullscreenPass<CloudTraceData>(
                g, services, "SkyEnv", {"sky_probe.psSkyEnv", clouds.traceLayout()},
                [&](RgPassBuilder& b, CloudTraceData& d) {
                    d.depth = b.read(depth, RgAccess::SampledRead); // bound but unused: the layout is the cloud trace's
                    envTex[0] = b.createTexture(envDesc);
                    d.out = b.colorTarget(0, envTex[0]);
                },
                [self, frameData](const CloudTraceData& d, RgPassContext& ctx) { return cloudTraceBindings(*self, *frameData, d, ctx); });
            for (u32 level = 1; level < 5; ++level) {
                envDesc.width = envDesc.height = kEnvBaseSize >> level;
                AddFullscreenPass<EnvData>(
                    g, services, "EnvDown", {"env.psEnvDown", post.bloomLayout()},
                    [&](RgPassBuilder& b, EnvData& d) {
                        d.src = b.read(envTex[level - 1], RgAccess::SampledRead);
                        envTex[level] = b.createTexture(envDesc);
                        d.dst = b.colorTarget(0, envTex[level]);
                    },
                    [self](const EnvData& d, RgPassContext& ctx) { return bloomBindings(*self, ctx.texture(d.src), ctx); });
            }
        }

        if (vf.probesOn) {
            for (u32 i = 0; i < 4; ++i)
                probeSh[i] = g.importTexture(vp.probes.texture(i), nvrhi::ResourceStates::ShaderResource,
                                             nvrhi::ResourceStates::ShaderResource, "LightProbeSH");
            if (vp.probes.takeNeedsClear()) {
                // New textures hold garbage: alpha 0 everywhere = no probe captured yet.
                g.addPass<ProbeClearData>(
                    "ProbeClear",
                    [&](RgPassBuilder& b, ProbeClearData& d) {
                        for (u32 i = 0; i < 4; ++i) d.sh[i] = b.write(probeSh[i], RgAccess::StorageWrite);
                    },
                    [](const ProbeClearData& d, RgPassContext& ctx) {
                        for (u32 i = 0; i < 4; ++i)
                            ctx.cmd()->clearTextureFloat(ctx.texture(d.sh[i]), nvrhi::AllSubresources, nvrhi::Color(0.0f));
                    });
            }
            if (!vf.probeJobs.empty()) {
                RgTextureDesc atlasGb = hdrDesc;
                atlasGb.width = kProbeFace * kProbeFaces;
                atlasGb.height = kProbeFace * kProbeMaxPerFrame;
                atlasGb.useClearValue = false;
                atlasGb.format = nvrhi::Format::SRGBA8_UNORM;
                atlasGb.name = "ProbeGBufferAlbedo";
                RgTextureDesc atlasNormal = atlasGb;
                atlasNormal.format = nvrhi::Format::RG16_UNORM;
                atlasNormal.name = "ProbeGBufferNormal";
                RgTextureDesc atlasMaterial = atlasGb;
                atlasMaterial.format = nvrhi::Format::RGBA8_UNORM;
                atlasMaterial.name = "ProbeGBufferMaterial";
                RgTextureDesc atlasDepthDesc = depthDesc;
                atlasDepthDesc.width = atlasGb.width;
                atlasDepthDesc.height = atlasGb.height;
                atlasDepthDesc.name = "ProbeDepth";
                RgTextureDesc litDesc = atlasGb;
                litDesc.format = kHdrFormat;
                litDesc.name = "ProbeLit";
                RgTexture pAlbedo, pNormal, pMaterial, pDepth, pLit;
                g.addPass<ProbeCaptureData>(
                    "ProbeCapture",
                    [&](RgPassBuilder& b, ProbeCaptureData& d) {
                        pAlbedo = b.createTexture(atlasGb);
                        pNormal = b.createTexture(atlasNormal);
                        pMaterial = b.createTexture(atlasMaterial);
                        pDepth = b.createTexture(atlasDepthDesc);
                        d.albedo = b.colorTarget(0, pAlbedo);
                        d.normal = b.colorTarget(1, pNormal);
                        d.material = b.colorTarget(2, pMaterial);
                        d.depth = b.depthTarget(pDepth);
                        d.instances = b.read(instances, RgAccess::VertexRead);
                        if (skinning) d.skinned = b.read(skinned, RgAccess::VertexRead);
                    },
                    [self, frameData](const ProbeCaptureData& d, RgPassContext& ctx) { recordProbeCapture(*self, *frameData, d, ctx); });
                AddFullscreenPass<ProbeLightData>(
                    g, services, "ProbeLight", {"probe_light.psProbeLight", gpu.layoutProbeLight.Get()},
                    [&](RgPassBuilder& b, ProbeLightData& d) {
                        d.albedo = b.read(pAlbedo, RgAccess::SampledRead);
                        d.normal = b.read(pNormal, RgAccess::SampledRead);
                        d.material = b.read(pMaterial, RgAccess::SampledRead);
                        d.depth = b.read(pDepth, RgAccess::SampledRead);
                        d.atlas = b.read(atlas, RgAccess::SampledRead);
                        if (vf.transmitOn) d.foliage = b.read(foliageTex, RgAccess::SampledRead);
                        for (u32 i = 0; i < 4; ++i) d.sh[i] = b.read(probeSh[i], RgAccess::SampledRead);
                        pLit = b.createTexture(litDesc);
                        d.lit = b.colorTarget(0, pLit);
                    },
                    [self, frameData](const ProbeLightData& d, RgPassContext& ctx) { return probeLightBindings(*self, *frameData, d, ctx); });
                g.addPass<ProbeShData>(
                    "ProbeSh",
                    [&](RgPassBuilder& b, ProbeShData& d) {
                        d.lit = b.read(pLit, RgAccess::SampledRead);
                        for (u32 i = 0; i < 4; ++i) d.sh[i] = b.write(probeSh[i], RgAccess::StorageWrite);
                    },
                    [self, frameData](const ProbeShData& d, RgPassContext& ctx) { recordProbeSh(*self, *frameData, d, ctx); });
            }
        }
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
                if (vf.gtaoOn) d.gtao = b.read(gtaoTex, RgAccess::SampledRead);
                if (vf.transmitOn) d.foliage = b.read(foliageTex, RgAccess::SampledRead);
                if (vf.envOn) {
                    for (u32 i = 0; i < 5; ++i) d.env[i] = b.read(envTex[i], RgAccess::SampledRead);
                }
                if (vf.probesOn) {
                    for (u32 i = 0; i < 4; ++i) d.sh[i] = b.read(probeSh[i], RgAccess::SampledRead);
                }
                d.hdr = b.colorTarget(0, hdr); // keeps the sky where the shader discards
            },
            [self, frameData](const DeferredData& d, RgPassContext& ctx) {
                return deferredBindings(*self, *frameData, d, ctx);
            });
    }

    // Volumetric clouds over the empty pixels: trace (half res) -> resolve with the history -> composite.
    if (cloudsOn) {
        const u32 cw = std::max(1u, (vp.width + 1) / 2), ch = std::max(1u, (vp.height + 1) / 2);
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

    // Water: the sky above the camera into a small map (what reflections that leave the screen see), a copy of the
    // scene so the surface can read it while it writes the same target, then the sea itself.
    if (HasFlag(vp.flags, ViewportFlags::Water) && water.active()) {
        ensureWaterBuffers(*this, vp);
        vf.waterCb = vp.waterCb.Get();
        vf.cloudCb = vp.cloudCb.Get();
        if (!cloudsOn) vf.clouds = CloudConstants{}; // zeroed: the probe skips the cloud march
        const f32 cloudMid = clouds.desc().baseAltitude + 0.5f * clouds.desc().thickness;
        water.fill(vf.water, frame, cloudMid);
        vf.waterRings = water.ringLevels(view.farZ);
        vf.water.grid.z = static_cast<f32>(vf.waterRings);

        RgTextureDesc probeDesc;
        probeDesc.width = probeDesc.height = kWaterProbeSize;
        probeDesc.format = kHdrFormat;
        probeDesc.renderTarget = true;
        probeDesc.name = "WaterSkyProbe";
        RgTexture probe;
        AddFullscreenPass<CloudTraceData>(
            g, services, "WaterSkyProbe", {"sky_probe.psSkyProbe", clouds.traceLayout()},
            [&](RgPassBuilder& b, CloudTraceData& d) {
                d.depth = b.read(depth, RgAccess::SampledRead); // bound but unused: the layout is the cloud trace's
                probe = b.createTexture(probeDesc);
                d.out = b.colorTarget(0, probe);
            },
            [self, frameData](const CloudTraceData& d, RgPassContext& ctx) { return cloudTraceBindings(*self, *frameData, d, ctx); });

        RgTextureDesc copyDesc = hdrDesc;
        copyDesc.renderTarget = false;
        copyDesc.useClearValue = false;
        copyDesc.name = "WaterSceneCopy";
        RgTexture sceneCopy;
        g.addPass<SceneCopyData>(
            "WaterSceneCopy",
            [&](RgPassBuilder& b, SceneCopyData& d) {
                d.src = b.read(hdr, RgAccess::CopySrc);
                sceneCopy = b.createTexture(copyDesc);
                d.dst = b.write(sceneCopy, RgAccess::CopyDst);
            },
            [](const SceneCopyData& d, RgPassContext& ctx) {
                ctx.cmd()->copyTexture(ctx.texture(d.dst), nvrhi::TextureSlice(), ctx.texture(d.src), nvrhi::TextureSlice());
            });

        g.addPass<WaterSurfaceData>(
            "WaterSurface",
            [&](RgPassBuilder& b, WaterSurfaceData& d) {
                d.depth = b.read(depth, RgAccess::SampledRead);
                d.scene = b.read(sceneCopy, RgAccess::SampledRead);
                d.probe = b.read(probe, RgAccess::SampledRead);
                d.hdr = b.colorTarget(0, hdr); // keeps the scene where the shader discards
            },
            [self, frameData](const WaterSurfaceData& d, RgPassContext& ctx) { recordWaterSurface(*self, *frameData, d, ctx); });
    }

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
                    if (skinning) d.skinned = b.read(skinned, RgAccess::VertexRead);
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

    // Bloom (Jimenez, Call of Duty: Advanced Warfare): the HDR image is downsampled `levels` times with a 13-tap filter,
    // then walked back up with a tent filter that is added onto each larger level. Level i is half of level i - 1, the
    // first is half of the scene; the tonemap blends the largest into the image.
    const u32 bloomLevels = vf.bloomLevels;
    RgTexture bloomChain[kMaxBloomLevels];
    if (bloomLevels > 0) {
        RgTextureDesc bd;
        bd.format = nvrhi::Format::R11G11B10_FLOAT; // no alpha needed, half the bandwidth of RGBA16F
        bd.renderTarget = true;
        bd.name = "BloomLevel";
        RgTexture previous = hdr;
        u32 w = vp.width, h = vp.height;
        for (u32 i = 0; i < bloomLevels; ++i) {
            w = std::max(1u, (w + 1) / 2);
            h = std::max(1u, (h + 1) / 2);
            bd.width = w;
            bd.height = h;
            AddFullscreenPass<BloomData>(
                g, services, "BloomDown", {i == 0 ? "bloom.psBloomDownFirst" : "bloom.psBloomDown", post.bloomLayout()},
                [&](RgPassBuilder& b, BloomData& d) {
                    d.src = b.read(previous, RgAccess::SampledRead);
                    bloomChain[i] = b.createTexture(bd);
                    d.dst = b.colorTarget(0, bloomChain[i]);
                },
                [self](const BloomData& d, RgPassContext& ctx) { return bloomBindings(*self, ctx.texture(d.src), ctx); });
            previous = bloomChain[i];
        }
        for (u32 i = bloomLevels - 1; i-- > 0;) {
            AddFullscreenPass<BloomData>(
                g, services, "BloomUp", {"bloom.psBloomUp", post.bloomLayout(), RgBlend::Additive},
                [&](RgPassBuilder& b, BloomData& d) {
                    d.src = b.read(bloomChain[i + 1], RgAccess::SampledRead);
                    d.dst = b.colorTarget(0, bloomChain[i]); // holds the downsampled level: the pass adds onto it
                },
                [self](const BloomData& d, RgPassContext& ctx) { return bloomBindings(*self, ctx.texture(d.src), ctx); });
        }
    }

    // Auto exposure: histogram of the HDR image -> average -> adapted value in the viewport's persistent buffer.
    const RgBuffer exposure = g.importBuffer(vp.exposureState.Get(), nvrhi::ResourceStates::ShaderResource,
                                             nvrhi::ResourceStates::ShaderResource, "ExposureState");
    if (settings.autoExposure) {
        RgBufferDesc binsDesc;
        binsDesc.byteSize = kHistogramBins * sizeof(u32);
        binsDesc.structStride = sizeof(u32);
        binsDesc.storage = true;
        binsDesc.name = "ExposureHistogram";
        RgBuffer bins;
        // The bins are cleared by a copy so the barriers between clear -> histogram -> average are real state changes.
        g.addPass<ExposureClearData>(
            "ExposureClear",
            [&](RgPassBuilder& b, ExposureClearData& d) {
                bins = b.createBuffer(binsDesc);
                d.bins = b.write(bins, RgAccess::CopyDst);
            },
            [](const ExposureClearData& d, RgPassContext& ctx) { recordExposureClear(d, ctx); });
        g.addPass<HistogramData>(
            "ExposureHistogram",
            [&](RgPassBuilder& b, HistogramData& d) {
                d.hdr = b.read(hdr, RgAccess::SampledRead);
                d.bins = b.write(bins, RgAccess::StorageWrite);
            },
            [self, frameData](const HistogramData& d, RgPassContext& ctx) { recordHistogram(*self, *frameData, d, ctx); });
        g.addPass<ExposureAverageData>(
            "ExposureAverage",
            [&](RgPassBuilder& b, ExposureAverageData& d) {
                d.bins = b.read(bins, RgAccess::SampledRead);
                d.state = b.write(exposure, RgAccess::StorageWrite);
            },
            [self, frameData](const ExposureAverageData& d, RgPassContext& ctx) {
                recordExposureAverage(*self, *frameData, d, ctx);
            });
    }

    AddFullscreenPass<TonemapData>(
        g, services, "Tonemap", {"tonemap.psTonemap", post.tonemapLayout()},
        [&](RgPassBuilder& b, TonemapData& d) {
            d.hdr = b.read(hdr, RgAccess::SampledRead);
            if (bloomLevels > 0) d.bloom = b.read(bloomChain[0], RgAccess::SampledRead);
            d.exposure = b.read(exposure, RgAccess::SampledRead);
            d.output = b.colorTarget(0, output);
        },
        [self, frameData](const TonemapData& d, RgPassContext& ctx) { return tonemapBindings(*self, *frameData, d, ctx); });

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
    vp.debug.gpu.clear();
    for (const RgPassTiming& t : g.gpuTimings()) vp.debug.gpu.push_back({t.name, t.ms});
    vp.debug.passes.clear();
    for (const RgPassInfo& p : g.passInfos()) vp.debug.passes.push_back({p.name, p.culled, p.group});

    vp.graphReady = true;
}

} // namespace foh
