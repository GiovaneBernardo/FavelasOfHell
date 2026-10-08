// The per-viewport render graph:
//
//   UploadInstances (copy)   scene instances -> transient vertex stream
//   DepthPrepass             depth of all opaque geometry (VS only)
//   ShadowCascades           sun shadow map: 4 cascades tiled in one depth atlas
//   Opaque                   G-buffer fill (albedo, normal, material), depth-tested
//                            against the pre-pass so every pixel is shaded once
//   Sky | ClearHdr           background into the HDR target
//   Deferred                 fullscreen lighting from the G-buffer + shadows -> HDR
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

void fillConstants(ViewConstants& c, const RenderViewParams& v, const SceneSlot* slot, u32 w, u32 h, f32 time) {
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
    c.params = Vec4(1.0f, time, v.nearZ, v.farZ);
}

// Fits the sun's cascades to the view and publishes them in the view constants.
void fillShadows(ViewFrame& vf, const RenderViewParams& v, const RendererDesc& desc) {
    ViewConstants& c = vf.constants;
    vf.shadowTile = std::clamp(desc.shadowCascadeSize, 256u, 8192u);
    vf.shadowsOn = vf.scene && !vf.scene->instances.empty() && c.sunDirection.w > 0.5f;
    if (!vf.shadowsOn) return;

    ShadowCascadeDesc cd;
    cd.view = v.view;
    cd.proj = v.proj;
    cd.nearZ = v.nearZ;
    cd.farZ = v.farZ;
    cd.lightDirection = Vec3(c.sunDirection);
    cd.shadowDistance = desc.shadowDistance;
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
};
struct DeferredData {
    RgTexture albedo;
    RgTexture normal;
    RgTexture material;
    RgTexture depth;
    RgTexture atlas;
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

// One instanced draw per mesh batch. Shared by the pre-pass, shadows, G-buffer and picking.
void drawMeshBatches(Impl& s, const nvrhi::ViewportState& viewport, nvrhi::IFramebuffer* fb,
                     nvrhi::IGraphicsPipeline* pso, nvrhi::IBindingSet* set, nvrhi::ICommandList* cmd,
                     nvrhi::IBuffer* instances, const SceneGpu& scene) {
    nvrhi::GraphicsState gs;
    gs.setPipeline(pso).setFramebuffer(fb).setViewport(viewport).addBindingSet(set);
    gs.addVertexBuffer({s.gpu.meshes.vertexBuffer(), 0, 0});
    gs.addVertexBuffer({instances, 1, 0});
    gs.setIndexBuffer({s.gpu.meshes.indexBuffer(), nvrhi::Format::R32_UINT, 0});
    cmd->setGraphicsState(gs);
    for (const SceneGpu::Batch& b : scene.batches) {
        const MeshRange& r = s.gpu.meshes.range(b.mesh);
        cmd->drawIndexed(nvrhi::DrawArguments()
                             .setVertexCount(r.indexCount)
                             .setInstanceCount(b.count)
                             .setStartIndexLocation(r.firstIndex)
                             .setStartVertexLocation(static_cast<u32>(r.baseVertex))
                             .setStartInstanceLocation(b.first));
    }
}

// Pipeline for the instanced meshes: depth-only when `ps` is empty.
nvrhi::IGraphicsPipeline* meshPipeline(Impl& s, nvrhi::IFramebuffer* fb, const ShaderRef& vs, const ShaderRef& ps,
                                       u64 variant, bool depthWrite, nvrhi::RasterCullMode cull) {
    return s.pipelines->graphics(shaderKey(vs, ps, variant), fb, [&] {
        nvrhi::GraphicsPipelineDesc pd;
        pd.inputLayout = s.gpu.meshInput;
        pd.VS = vs.shader;
        if (ps) pd.PS = ps.shader;
        pd.bindingLayouts = {s.gpu.layoutView.Get()};
        pd.renderState.depthStencilState.depthTestEnable = true;
        pd.renderState.depthStencilState.depthWriteEnable = depthWrite;
        pd.renderState.depthStencilState.depthFunc = nvrhi::ComparisonFunc::GreaterOrEqual; // reverse-Z
        pd.renderState.rasterState.cullMode = cull;
        pd.renderState.rasterState.frontCounterClockwise = true;
        return pd;
    });
}

void recordDepthPrepass(Impl& s, const ViewFrame& vf, const DepthPrepassData& d, RgPassContext& ctx) {
    nvrhi::ICommandList* cmd = ctx.cmd();
    // Reverse-Z: far is 0.
    cmd->clearDepthStencilTexture(ctx.texture(d.depth), nvrhi::AllSubresources, true, 0.0f, false, 0);
    if (!vf.scene || vf.scene->batches.empty() || !d.instances.valid()) return;

    nvrhi::IFramebuffer* fb = ctx.framebuffer();
    const ShaderRef vs = s.shaders->get("depth.vsDepth", nvrhi::ShaderType::Vertex);
    if (!vs) return;
    nvrhi::IGraphicsPipeline* pso = meshPipeline(s, fb, vs, {}, 7, true, nvrhi::RasterCullMode::Back);
    if (!pso) return;
    drawMeshBatches(s, fullViewport(vf), fb, pso, viewBindings(ctx, s, vf, nullptr, nvrhi::Format::UNKNOWN), cmd,
                    ctx.buffer(d.instances), *vf.scene);
}

// All cascades share one depth atlas (2x2 tiles); each is drawn with its own viewport.
void recordShadows(Impl& s, const ViewFrame& vf, const ShadowData& d, RgPassContext& ctx) {
    nvrhi::ICommandList* cmd = ctx.cmd();
    cmd->clearDepthStencilTexture(ctx.texture(d.atlas), nvrhi::AllSubresources, true, 0.0f, false, 0);
    if (!vf.shadowsOn || !vf.scene || vf.scene->batches.empty() || !d.instances.valid()) return;

    nvrhi::IFramebuffer* fb = ctx.framebuffer();
    const ShaderRef vs = s.shaders->get("shadow.vsShadow", nvrhi::ShaderType::Vertex);
    if (!vs) return;
    // No culling: single-sided casters (roofs, sheet metal) must still shadow from behind.
    nvrhi::IGraphicsPipeline* pso = meshPipeline(s, fb, vs, {}, 8, true, nvrhi::RasterCullMode::None);
    if (!pso) return;

    nvrhi::BindingSetDesc bd;
    bd.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, vf.shadowCb));
    nvrhi::IBindingSet* set = ctx.bindingSet(bd, s.gpu.layoutView);
    const f32 tile = static_cast<f32>(vf.shadowTile);
    for (u32 c = 0; c < kShadowCascades; ++c) {
        cmd->writeBuffer(vf.shadowCb, &vf.cascades.viewProj[c], sizeof(Mat4));
        const f32 x = static_cast<f32>(c & 1) * tile;
        const f32 y = static_cast<f32>(c >> 1) * tile;
        const nvrhi::ViewportState viewport =
            nvrhi::ViewportState().addViewportAndScissorRect(nvrhi::Viewport(x, x + tile, y, y + tile, 0.0f, 1.0f));
        drawMeshBatches(s, viewport, fb, pso, set, cmd, ctx.buffer(d.instances), *vf.scene);
    }
}

// Fills the G-buffer. Depth comes from the pre-pass: read-only, GreaterOrEqual, so only the
// visible surface passes (>= and not == keeps this robust against tiny VS differences).
void recordOpaque(Impl& s, const ViewFrame& vf, const OpaqueData& d, RgPassContext& ctx) {
    if (!vf.scene || vf.scene->batches.empty() || !d.instances.valid()) return;
    nvrhi::IFramebuffer* fb = ctx.framebuffer();
    const ShaderRef vs = s.shaders->get("opaque.vsOpaque", nvrhi::ShaderType::Vertex);
    const ShaderRef ps = s.shaders->get("opaque.psOpaque", nvrhi::ShaderType::Pixel);
    if (!vs || !ps) return;
    nvrhi::IGraphicsPipeline* pso = meshPipeline(s, fb, vs, ps, 2, false, nvrhi::RasterCullMode::Back);
    if (!pso) return;
    drawMeshBatches(s, fullViewport(vf), fb, pso, viewBindings(ctx, s, vf, nullptr, nvrhi::Format::UNKNOWN),
                    ctx.cmd(), ctx.buffer(d.instances), *vf.scene);
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
    bd.addItem(nvrhi::BindingSetItem::Sampler(0, s.gpu.shadowSampler));
    return ctx.bindingSet(bd, s.gpu.layoutDeferred);
}

// Entity ids instead of colors, same geometry and depth test as the G-buffer pass.
void recordPick(Impl& s, const ViewFrame& vf, const PickData& d, RgPassContext& ctx) {
    nvrhi::ICommandList* cmd = ctx.cmd();
    nvrhi::IFramebuffer* fb = ctx.framebuffer();
    cmd->clearTextureUInt(ctx.texture(d.ids), nvrhi::AllSubresources, 0);
    cmd->clearDepthStencilTexture(ctx.texture(d.depth), nvrhi::AllSubresources, true, 0.0f, false, 0);
    const ShaderRef vs = s.shaders->get("opaque.vsOpaque", nvrhi::ShaderType::Vertex);
    const ShaderRef ps = s.shaders->get("opaque.psPick", nvrhi::ShaderType::Pixel);
    if (!vs || !ps) return;
    nvrhi::IGraphicsPipeline* pso = meshPipeline(s, fb, vs, ps, 6, true, nvrhi::RasterCullMode::Back);
    if (!pso) return;
    drawMeshBatches(s, fullViewport(vf), fb, pso, viewBindings(ctx, s, vf, nullptr, nvrhi::Format::UNKNOWN), cmd,
                    ctx.buffer(d.instances), *vf.scene);
}

} // namespace

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
    fillConstants(vf.constants, view, slot, vp.width, vp.height, time);
    fillShadows(vf, view, desc);

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

    // Background. The sky draws every pixel (no clear needed); without it a flat clear.
    if (HasFlag(vp.flags, ViewportFlags::Sky)) {
        AddFullscreenPass<SkyData>(
            g, services, "Sky", {"sky.psSky", gpu.layoutView.Get()},
            [&](RgPassBuilder& b, SkyData& d) {
                hdr = b.createTexture(hdrDesc);
                d.hdr = b.colorTarget(0, hdr);
            },
            [self, frameData](const SkyData&, RgPassContext& ctx) {
                return viewBindings(ctx, *self, *frameData, nullptr, nvrhi::Format::UNKNOWN);
            });
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
