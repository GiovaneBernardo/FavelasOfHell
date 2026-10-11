#include "foh/renderer/renderer.h"

#include <algorithm>
#include <cmath>

#include "foh/assert.h"
#include "foh/file_system.h"
#include "foh/log.h"
#include "renderer_impl.h"

namespace foh {

// ---- math ---------------------------------------------------------------------------

Mat4 MakePerspectiveReverseZ(f32 fovY, f32 aspect, f32 nearZ, f32 farZ) {
    const f32 t = 1.0f / std::tan(fovY * 0.5f);
    Mat4 m(0.0f);
    m[0][0] = t / aspect;
    m[1][1] = t;
    m[2][2] = nearZ / (farZ - nearZ);
    m[2][3] = -1.0f;
    m[3][2] = farZ * nearZ / (farZ - nearZ);
    return m;
}

RenderViewParams MakeViewParams(const Mat4& cameraWorld, f32 fovY, f32 aspect, f32 nearZ, f32 farZ) {
    RenderViewParams p;
    p.view = glm::inverse(cameraWorld);
    p.proj = MakePerspectiveReverseZ(fovY, aspect, nearZ, farZ);
    p.position = Vec3(cameraWorld[3]);
    p.nearZ = nearZ;
    p.farZ = farZ;
    return p;
}

// ---- lifecycle ------------------------------------------------------------------------

Renderer::Renderer() : m_impl(std::make_unique<Impl>()) {}

Renderer::~Renderer() {
    if (!m_impl || !m_impl->device) return;
    m_impl->device->waitIdle();
    // Drop everything that holds GPU objects before the device goes away.
    m_impl->viewports.clear();
    m_impl->windowGraph.reset();
    m_impl->atmosphere.shutdown();
    m_impl->water.shutdown();
    m_impl->clouds.shutdown();
    m_impl->post.shutdown();
    m_impl->gpu = {};
    m_impl->pipelines.reset();
    m_impl->shaders.reset();
    m_impl->device->nvrhi()->runGarbageCollection();
}

std::unique_ptr<Renderer> Renderer::create(const RendererDesc& desc) {
    std::unique_ptr<Renderer> self(new Renderer());
    Impl& s = *self->m_impl;
    s.desc = desc;
    s.settings.shadowCascadeSize = desc.shadowCascadeSize;
    s.settings.shadowDistance = desc.shadowDistance;

    GraphicsDeviceDesc dd;
    dd.validation = desc.validation;
    dd.framesInFlight = desc.framesInFlight;
    s.device = GraphicsDevice::create(dd);
    if (!s.device) return nullptr;
    nvrhi::IDevice* dev = s.device->nvrhi();

    const std::filesystem::path shaderDir =
        desc.shaderDirectory.empty() ? GetExecutableDirectory() / "shaders" : desc.shaderDirectory;
    s.shaders = std::make_unique<ShaderLibrary>(dev, s.device->api(), shaderDir);
    s.pipelines = std::make_unique<PipelineCache>(dev);

    // Fail early and loudly if the shader binaries were not built.
    if (!s.shaders->get("opaque.vsOpaque", nvrhi::ShaderType::Vertex) ||
        !s.shaders->get("tonemap.psTonemap", nvrhi::ShaderType::Pixel) ||
        !s.shaders->get("bloom.psBloomDown", nvrhi::ShaderType::Pixel) ||
        !s.shaders->get("exposure.csHistogram", nvrhi::ShaderType::Compute)) {
        FOH_ERROR("renderer: shader binaries missing in {} (build the 'foh_shaders' target)", shaderDir.string());
        return nullptr;
    }

    // Binding layouts shared by the built-in passes.
    {
        nvrhi::BindingLayoutDesc ld;
        ld.visibility = nvrhi::ShaderType::All;
        ld.addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0));
        s.gpu.layoutView = dev->createBindingLayout(ld);
        ld.addItem(nvrhi::BindingLayoutItem::Texture_SRV(0));
        s.gpu.layoutViewTexture = dev->createBindingLayout(ld);
        nvrhi::BindingLayoutDesc tl;
        tl.visibility = nvrhi::ShaderType::All;
        tl.addItem(nvrhi::BindingLayoutItem::Texture_SRV(0));
        s.gpu.layoutTexture = dev->createBindingLayout(tl);

        nvrhi::BindingLayoutDesc dl;
        dl.visibility = nvrhi::ShaderType::All;
        dl.addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0));
        // t0..t3 G-buffer + depth, t4 shadow atlas, t5..t7 atmosphere, t8 GTAO, t9 foliage transmission atlas,
        // t10..t14 sky environment levels, t15..t18 probe SH, t19 ground height (common/lighting.slangh)
        for (u32 t = 0; t < 20; ++t) dl.addItem(nvrhi::BindingLayoutItem::Texture_SRV(t));
        dl.addItem(nvrhi::BindingLayoutItem::Sampler(0));
        dl.addItem(nvrhi::BindingLayoutItem::Sampler(1)); // bilinear clamp for the atmosphere tables
        dl.addItem(nvrhi::BindingLayoutItem::Sampler(2)); // bilinear wrap for the probe grid
        s.gpu.layoutDeferred = dev->createBindingLayout(dl);
        dl.addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(1)); // ProbeConstants
        s.gpu.layoutProbeLight = dev->createBindingLayout(dl);

        nvrhi::BindingLayoutDesc gl;
        gl.visibility = nvrhi::ShaderType::All;
        gl.addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0));
        for (u32 t = 0; t < 3; ++t) gl.addItem(nvrhi::BindingLayoutItem::Texture_SRV(t));
        s.gpu.layoutGtao = dev->createBindingLayout(gl);

        nvrhi::BindingLayoutDesc pl;
        pl.visibility = nvrhi::ShaderType::All;
        pl.addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0));
        pl.addItem(nvrhi::BindingLayoutItem::Texture_SRV(0));
        for (u32 t = 0; t < 4; ++t) pl.addItem(nvrhi::BindingLayoutItem::Texture_UAV(t));
        s.gpu.layoutProbeSh = dev->createBindingLayout(pl);

        nvrhi::SamplerDesc psd;
        psd.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Wrap);
        s.gpu.probeSampler = dev->createSampler(psd);

        nvrhi::BindingLayoutDesc sl;
        sl.visibility = nvrhi::ShaderType::All;
        sl.addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0));
        for (u32 t = 0; t < 4; ++t) sl.addItem(nvrhi::BindingLayoutItem::Texture_SRV(t));
        sl.addItem(nvrhi::BindingLayoutItem::Sampler(0));
        s.gpu.layoutSky = dev->createBindingLayout(sl);

        // Passes that sample materials: view constants, the material buffer and one sampler (register space 0),
        // plus the bindless texture table (register space 1, a layout of its own).
        nvrhi::BindingLayoutDesc ml;
        ml.visibility = nvrhi::ShaderType::All;
        ml.addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0));
        ml.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0));
        ml.addItem(nvrhi::BindingLayoutItem::Sampler(0));
        s.gpu.layoutMaterial = dev->createBindingLayout(ml);

        // Pre-skinning (shaders/skin.slang).
        nvrhi::BindingLayoutDesc kl;
        kl.visibility = nvrhi::ShaderType::All;
        for (u32 t = 0; t < 4; ++t) kl.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(t));
        kl.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0));
        s.gpu.layoutSkin = dev->createBindingLayout(kl);

        nvrhi::BindlessLayoutDesc bl;
        bl.setVisibility(nvrhi::ShaderType::All)
            .setFirstSlot(0)
            .setMaxCapacity(TextureStore::kCapacity)
            .addRegisterSpace(nvrhi::BindingLayoutItem::Texture_SRV(1)); // slot = register space
        s.gpu.layoutBindless = dev->createBindlessLayout(bl);

        nvrhi::SamplerDesc msd;
        msd.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Wrap).setMaxAnisotropy(8.0f);
        s.gpu.materialSampler = dev->createSampler(msd);

        // NVRHI's D3D12 backend fixes the compare function of a Comparison sampler to LESS:
        // result = (reference < stored depth), i.e. 1 = in shadow with reverse-Z.
        nvrhi::SamplerDesc sd;
        sd.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
        sd.setReductionType(nvrhi::SamplerReductionType::Comparison);
        s.gpu.shadowSampler = dev->createSampler(sd);
    }

    // Mesh vertex stream 0 + per-instance stream 1 (see mesh_forward.slang).
    {
        nvrhi::VertexAttributeDesc attrs[7];
        attrs[0].name = "POSITION";
        attrs[0].format = nvrhi::Format::RGB32_FLOAT;
        attrs[0].offset = offsetof(MeshVertex, position);
        attrs[0].bufferIndex = 0;
        attrs[0].elementStride = sizeof(MeshVertex);
        attrs[1].name = "NORMAL";
        attrs[1].format = nvrhi::Format::RGB32_FLOAT;
        attrs[1].offset = offsetof(MeshVertex, normal);
        attrs[1].bufferIndex = 0;
        attrs[1].elementStride = sizeof(MeshVertex);
        attrs[2].name = "TEXCOORD";
        attrs[2].format = nvrhi::Format::RG32_FLOAT;
        attrs[2].offset = offsetof(MeshVertex, uv);
        attrs[2].bufferIndex = 0;
        attrs[2].elementStride = sizeof(MeshVertex);
        attrs[3].name = "INSTANCE";
        attrs[3].format = nvrhi::Format::RGBA32_FLOAT;
        attrs[3].arraySize = 4;
        attrs[3].offset = 0;
        attrs[3].bufferIndex = 1;
        attrs[3].elementStride = sizeof(GpuInstance);
        attrs[3].isInstanced = true;
        attrs[4].name = "ENTITY";
        attrs[4].format = nvrhi::Format::RG32_UINT;
        attrs[4].offset = offsetof(GpuInstance, entityLo);
        attrs[4].bufferIndex = 1;
        attrs[4].elementStride = sizeof(GpuInstance);
        attrs[4].isInstanced = true;
        attrs[5].name = "TANGENT";
        attrs[5].format = nvrhi::Format::RGBA8_SNORM;
        attrs[5].offset = offsetof(MeshVertex, tangent);
        attrs[5].bufferIndex = 0;
        attrs[5].elementStride = sizeof(MeshVertex);
        attrs[6].name = "MATERIAL";
        attrs[6].format = nvrhi::Format::R32_UINT;
        attrs[6].offset = offsetof(GpuInstance, material);
        attrs[6].bufferIndex = 1;
        attrs[6].elementStride = sizeof(GpuInstance);
        attrs[6].isInstanced = true;
        s.gpu.meshInput = dev->createInputLayout(attrs, 7, nullptr);
    }
    s.gpu.meshes = MeshStore::create(dev);
    if (!s.gpu.textures.init(dev, s.gpu.layoutBindless)) {
        FOH_ERROR("renderer: cannot create the bindless texture table (descriptor indexing needs D3D12 resource binding tier 3)");
        return nullptr;
    }
    s.gpu.materials.init(dev);
    s.gpu.materials.upload(dev);
    s.warmPipelines();

    // Stand-ins for the inputs of passes that are off (GTAO, foliage atlas, sky environment, probes): the shaders skip
    // them by a flag in the view constants, but the binding sets still need valid textures.
    {
        auto make = [&](bool array, const char* name) {
            nvrhi::TextureDesc td;
            td.width = td.height = 1;
            td.format = nvrhi::Format::RGBA16_FLOAT;
            if (array) {
                td.dimension = nvrhi::TextureDimension::Texture2DArray;
                td.arraySize = kProbeLayers;
            }
            td.initialState = nvrhi::ResourceStates::ShaderResource;
            td.keepInitialState = true;
            td.debugName = name;
            return dev->createTexture(td);
        };
        s.gpu.dummy2D = make(false, "Dummy2D");
        s.gpu.dummyArray = make(true, "DummyArray");
        nvrhi::CommandListHandle cmd = dev->createCommandList();
        const u16 zeros[4] = {};
        cmd->open();
        cmd->writeTexture(s.gpu.dummy2D, 0, 0, zeros, sizeof(zeros));
        for (u32 layer = 0; layer < kProbeLayers; ++layer) cmd->writeTexture(s.gpu.dummyArray, layer, 0, zeros, sizeof(zeros));
        cmd->close();
        dev->executeCommandList(cmd);
    }

    // Atmosphere tables: loaded from the cache, or generated once (in the background of the first frames).
    {
        const std::filesystem::path cacheDir = desc.cacheDirectory.empty() ? GetExecutableDirectory() / "cache" : desc.cacheDirectory;
        s.atmosphere.init(dev, s.shaders.get(), s.pipelines.get(), desc.jobs, cacheDir / "atmosphere.lut");
    }
    s.clouds.init(dev, s.shaders.get(), s.pipelines.get());
    s.water.init(dev, s.shaders.get(), s.pipelines.get());
    if (!s.post.init(dev)) {
        FOH_ERROR("renderer: cannot create the post-processing binding layouts");
        return nullptr;
    }

    RenderGraphConfig gc;
    gc.jobs = desc.jobs;
    gc.minPassesPerGroup = desc.minPassesPerGroup;
    s.windowGraph = std::make_unique<RenderGraph>(gc);
    return self;
}

// ---- lookup helpers ----------------------------------------------------------------------

Viewport* Renderer::Impl::findViewport(ViewportId id) {
    for (auto& v : viewports) {
        if (v->id == id.value) return v.get();
    }
    return nullptr;
}
const Viewport* Renderer::Impl::findViewport(ViewportId id) const {
    return const_cast<Impl*>(this)->findViewport(id);
}
SceneSlot* Renderer::Impl::findScene(SceneId id) {
    for (auto& s : scenes) {
        if (s.id == id.value) return &s;
    }
    return nullptr;
}

// ---- settings ---------------------------------------------------------------------------------------

void Renderer::setRenderSettings(const RenderSettings& settings) {
    RenderSettings& s = m_impl->settings;
    s = settings;
    s.shadowCascadeSize = std::clamp(s.shadowCascadeSize, 256u, 8192u);
    s.shadowDistance = std::clamp(s.shadowDistance, 1.0f, 5000.0f);
    s.exposure = std::clamp(s.exposure, 0.0f, 1000.0f);
    s.whitePoint = std::clamp(s.whitePoint, 0.5f, 100.0f);
    s.tonemapper = static_cast<u32>(s.tonemapper) < kTonemapperCount ? s.tonemapper : Tonemapper::Aces;
    s.exposureCompensation = std::clamp(s.exposureCompensation, -8.0f, 8.0f);
    s.exposureMinLuminance = std::clamp(s.exposureMinLuminance, -16.0f, 15.0f);
    s.exposureMaxLuminance = std::clamp(s.exposureMaxLuminance, s.exposureMinLuminance + 1.0f, 16.0f);
    s.adaptBrighten = std::clamp(s.adaptBrighten, 0.01f, 100.0f);
    s.adaptDarken = std::clamp(s.adaptDarken, 0.01f, 100.0f);
    s.meterCenterWeight = std::clamp(s.meterCenterWeight, 0.0f, 1.0f);
    s.bloomStrength = std::clamp(s.bloomStrength, 0.0f, 1.0f);
    s.bloomLevels = std::clamp(s.bloomLevels, 1u, kMaxBloomLevels);
    s.gtaoRadius = std::clamp(s.gtaoRadius, 0.1f, 8.0f);
    s.gtaoStrength = std::clamp(s.gtaoStrength, 0.1f, 4.0f);
    s.contactShadowLength = std::clamp(s.contactShadowLength, 0.05f, 4.0f);
    s.canopyTransmission = std::clamp(s.canopyTransmission, 0.0f, 2.0f);
    s.iblIntensity = std::clamp(s.iblIntensity, 0.0f, 8.0f);
    s.probeSpacing = std::clamp(s.probeSpacing, 1.0f, 16.0f);
    s.probesPerFrame = std::clamp(s.probesPerFrame, 1u, kProbeMaxPerFrame);
    s.probeInterval = std::clamp(s.probeInterval, 1u, 16u);
    s.probeIntensity = std::clamp(s.probeIntensity, 0.0f, 4.0f);
}
const RenderSettings& Renderer::renderSettings() const { return m_impl->settings; }

// ---- atmosphere ---------------------------------------------------------------------------------

void Renderer::generateAtmosphere(const AtmosphereDesc& desc) { m_impl->atmosphere.request(desc); }
const AtmosphereDesc& Renderer::atmosphereDesc() const { return m_impl->atmosphere.desc(); }
AtmosphereStatus Renderer::atmosphereStatus() const { return m_impl->atmosphere.status(); }

// ---- clouds -----------------------------------------------------------------------------------------

void Renderer::setClouds(const CloudDesc& desc) { m_impl->clouds.set(desc); }
const CloudDesc& Renderer::clouds() const { return m_impl->clouds.desc(); }
CloudWeather Renderer::cloudWeather() const { return m_impl->clouds.state(); }

// ---- water ------------------------------------------------------------------------------------------

void Renderer::setWater(const WaterDesc& desc) { m_impl->water.set(desc); }
const WaterDesc& Renderer::water() const { return m_impl->water.desc(); }
bool Renderer::setWaterBed(const WaterBedDesc& bed) { return m_impl->water.setBed(bed); }
void Renderer::clearWaterBed() { m_impl->water.clearBed(); }

// ---- scenes ---------------------------------------------------------------------------------

SceneId Renderer::createScene(const char* name) {
    SceneSlot slot;
    slot.id = m_impl->nextId++;
    slot.name = name;
    slot.scene = std::make_unique<RenderScene>();
    slot.scene->meshLookup = &m_impl->meshLookup;
    slot.scene->materialLookup = &m_impl->gpu.materials.lookup();
    m_impl->scenes.push_back(std::move(slot));
    return {m_impl->scenes.back().id};
}

void Renderer::destroyScene(SceneId id) {
    for (auto& v : m_impl->viewports) {
        if (v->scene == id) v->scene = {};
    }
    std::erase_if(m_impl->scenes, [&](const SceneSlot& s) { return s.id == id.value; });
}

RenderScene* Renderer::scene(SceneId id) {
    SceneSlot* s = m_impl->findScene(id);
    return s ? s->scene.get() : nullptr;
}

// ---- imported meshes --------------------------------------------------------------------------

bool Renderer::uploadMeshAsset(const AssetId& asset, const MeshGeometry& geometry, std::span<const MeshIndexRange> submeshes) {
    Impl& s = *m_impl;
    const size_t vertexCount = geometry.positions.size();
    if (!asset.isValid() || vertexCount == 0 || geometry.indices.empty() || submeshes.empty()) return false;
    if (!geometry.normals.empty() && geometry.normals.size() != vertexCount) return false;
    if (!geometry.uvs.empty() && geometry.uvs.size() != vertexCount) return false;
    if (!geometry.tangents.empty() && geometry.tangents.size() != vertexCount) return false;
    for (const u32 index : geometry.indices) {
        if (index >= vertexCount) return false;
    }
    for (const MeshIndexRange& r : submeshes) {
        if (r.firstIndex > geometry.indices.size() || r.indexCount > geometry.indices.size() - r.firstIndex) return false;
    }
    const bool skinned = !geometry.skinJoints.empty();
    if (skinned && (geometry.skinJoints.size() != vertexCount || geometry.skinWeights.size() != vertexCount)) return false;

    // Local bounding sphere (centre of the box, farthest vertex): lets every pass cull the instances that are not in view.
    Vec4 bounds(0.0f);
    {
        Vec3 lo(1e30f), hi(-1e30f);
        for (const Vec3& p : geometry.positions) {
            lo = glm::min(lo, p);
            hi = glm::max(hi, p);
        }
        const Vec3 centre = 0.5f * (lo + hi);
        f32 r2 = 0.0f;
        for (const Vec3& p : geometry.positions) r2 = std::max(r2, glm::dot(p - centre, p - centre));
        bounds = Vec4(centre, std::sqrt(r2));
    }
    std::vector<MeshVertex> vertices(vertexCount);
    for (size_t i = 0; i < vertexCount; ++i) {
        vertices[i].position = geometry.positions[i];
        vertices[i].normal = geometry.normals.empty() ? Vec3(0.0f, 1.0f, 0.0f) : geometry.normals[i];
        vertices[i].uv = geometry.uvs.empty() ? Vec2(0.0f) : geometry.uvs[i];
        if (!geometry.tangents.empty()) {
            vertices[i].tangent = PackTangent(geometry.tangents[i]);
        } else {
            // No tangents: any vector perpendicular to the normal keeps the TBN frame valid.
            const Vec3 n = vertices[i].normal;
            const Vec3 axis = std::abs(n.x) < 0.9f ? Vec3(1, 0, 0) : Vec3(0, 1, 0);
            vertices[i].tangent = PackTangent(Vec4(glm::normalize(axis - n * glm::dot(n, axis)), 1.0f));
        }
    }

    // Replacing (hot reload): the new ranges become visible together, then the old ones are released.
    const std::vector<u32> previous = s.meshLookup.ids(asset) ? *s.meshLookup.ids(asset) : std::vector<u32>{};
    std::vector<SkinInfluence> influences;
    if (skinned) {
        influences.resize(vertexCount);
        for (size_t i = 0; i < vertexCount; ++i) influences[i] = {geometry.skinJoints[i], geometry.skinWeights[i]};
    }
    const MeshStore::Block block = s.gpu.meshes.append(s.device->nvrhi(), vertices, geometry.indices, influences);
    std::vector<u32> ids;
    ids.reserve(submeshes.size());
    for (const MeshIndexRange& r : submeshes)
        ids.push_back(s.gpu.meshes.addRange({block.firstIndex + r.firstIndex, r.indexCount, block.baseVertex, static_cast<u32>(vertexCount), skinned, block.id, bounds}));
    s.meshLookup.set(asset, std::move(ids));
    for (const u32 id : previous) s.retiredMeshes.push_back({s.frame, id});
    return true;
}

void Renderer::releaseMeshAsset(const AssetId& asset) {
    Impl& s = *m_impl;
    if (const std::vector<u32>* ids = s.meshLookup.ids(asset)) {
        for (const u32 id : *ids) s.retiredMeshes.push_back({s.frame, id});
    }
    s.meshLookup.erase(asset);
}

bool Renderer::hasMeshAsset(const AssetId& asset) const { return m_impl->meshLookup.ids(asset) != nullptr; }

// ---- imported textures and materials ----------------------------------------------------------

bool Renderer::uploadTexture(TextureUpload&& upload) { return m_impl->gpu.textures.enqueue(std::move(upload)); }
void Renderer::releaseTexture(const AssetId& id) {
    Impl& s = *m_impl;
    s.gpu.textures.release(id, s.frame);
    s.gpu.materials.texturesChanged({id}, s.gpu.textures); // materials fall back to the stand-in
}
bool Renderer::hasTexture(const AssetId& id) const { return m_impl->gpu.textures.resident(id); }
TextureStats Renderer::textureStats() const { return m_impl->gpu.textures.stats(); }

void Renderer::setMaterial(const AssetId& id, const MaterialParams& params) {
    Impl& s = *m_impl;
    s.gpu.materials.set(id, params, s.gpu.textures);
}
void Renderer::releaseMaterial(const AssetId& id) { m_impl->gpu.materials.remove(id); }
bool Renderer::hasMaterial(const AssetId& id) const { return m_impl->gpu.materials.contains(id); }

// ---- viewports ----------------------------------------------------------------------------------

namespace {
u32 clampDim(u32 v) { return std::clamp(v, 8u, 8192u); }
} // namespace

void Renderer::Impl::createViewportTargets(Viewport& vp) {
    nvrhi::IDevice* dev = device->nvrhi();
    nvrhi::TextureDesc td;
    td.width = vp.width;
    td.height = vp.height;
    td.format = nvrhi::Format::RGBA8_UNORM;
    td.isRenderTarget = true;
    // The viewport image rests in ShaderResource between frames: the UI samples
    // it, and every render graph imports it with this as initial AND final state.
    td.initialState = nvrhi::ResourceStates::ShaderResource;
    td.keepInitialState = false;
    td.debugName = vp.name + " (output)";
    vp.output = dev->createTexture(td);
    vp.frame.width = vp.width;
    vp.frame.height = vp.height;
}

ViewportId Renderer::createViewport(const ViewportDesc& desc) {
    auto vp = std::make_unique<Viewport>();
    vp->id = m_impl->nextId++;
    vp->name = desc.name;
    vp->width = vp->pendingWidth = clampDim(desc.width);
    vp->height = vp->pendingHeight = clampDim(desc.height);
    vp->scene = desc.scene;
    vp->cameraEntity = desc.cameraEntity;
    vp->flags = desc.flags;

    nvrhi::BufferDesc cb;
    cb.byteSize = sizeof(ViewConstants);
    cb.isConstantBuffer = true;
    cb.isVolatile = true;
    cb.maxVersions = 32;
    cb.debugName = vp->name + " (view constants)";
    vp->viewCb = m_impl->device->nvrhi()->createBuffer(cb);
    cb.byteSize = sizeof(Mat4);
    cb.maxVersions = 16;
    cb.debugName = vp->name + " (shadow cascade)";
    vp->shadowCb = m_impl->device->nvrhi()->createBuffer(cb);
    cb.byteSize = sizeof(PostConstants);
    cb.debugName = vp->name + " (post constants)";
    vp->postCb = m_impl->device->nvrhi()->createBuffer(cb);

    // Adapted exposure: one float4, rests in ShaderResource between frames (every graph imports it that way).
    nvrhi::BufferDesc eb;
    eb.byteSize = sizeof(Vec4);
    eb.structStride = sizeof(Vec4);
    eb.canHaveUAVs = true;
    eb.initialState = nvrhi::ResourceStates::ShaderResource;
    eb.keepInitialState = false;
    eb.debugName = vp->name + " (exposure)";
    vp->exposureState = m_impl->device->nvrhi()->createBuffer(eb);

    RenderGraphConfig gc;
    gc.jobs = m_impl->desc.jobs;
    gc.minPassesPerGroup = m_impl->desc.minPassesPerGroup;
    vp->graph = std::make_unique<RenderGraph>(gc);

    m_impl->createViewportTargets(*vp);
    const ViewportId id{vp->id};
    m_impl->viewports.push_back(std::move(vp));
    return id;
}

void Renderer::destroyViewport(ViewportId id) {
    // In-flight command lists keep the GPU objects alive (NVRHI tracks them).
    std::erase_if(m_impl->viewports, [&](const auto& v) { return v->id == id.value; });
}

void Renderer::setViewportSize(ViewportId id, u32 width, u32 height) {
    if (Viewport* v = m_impl->findViewport(id)) {
        v->pendingWidth = clampDim(width);
        v->pendingHeight = clampDim(height);
    }
}
void Renderer::setViewportScene(ViewportId id, SceneId scene) {
    if (Viewport* v = m_impl->findViewport(id)) v->scene = scene;
}
void Renderer::setViewportCameraEntity(ViewportId id, u64 entityId) {
    if (Viewport* v = m_impl->findViewport(id)) v->cameraEntity = entityId;
}
void Renderer::setViewportFlags(ViewportId id, ViewportFlags flags) {
    if (Viewport* v = m_impl->findViewport(id)) v->flags = flags;
}
void Renderer::setViewportCamera(ViewportId id, const RenderViewParams& params) {
    if (Viewport* v = m_impl->findViewport(id)) {
        v->hasCameraOverride = true;
        v->cameraOverride = params;
    }
}
void Renderer::clearViewportCamera(ViewportId id) {
    if (Viewport* v = m_impl->findViewport(id)) v->hasCameraOverride = false;
}
void Renderer::setGpuTimerViewport(ViewportId id) { m_impl->gpuTimerViewport = id.value; }
void Renderer::setViewportActive(ViewportId id, bool active) {
    if (Viewport* v = m_impl->findViewport(id)) v->active = active;
}
u32 Renderer::viewportWidth(ViewportId id) const {
    const Viewport* v = m_impl->findViewport(id);
    return v ? v->width : 0;
}
u32 Renderer::viewportHeight(ViewportId id) const {
    const Viewport* v = m_impl->findViewport(id);
    return v ? v->height : 0;
}
nvrhi::ITexture* Renderer::viewportTexture(ViewportId id) const {
    const Viewport* v = m_impl->findViewport(id);
    return v ? v->output.Get() : nullptr;
}
float Renderer::viewportAspect(ViewportId id) const {
    const Viewport* v = m_impl->findViewport(id);
    return v && v->height ? static_cast<float>(v->width) / static_cast<float>(v->height) : 1.0f;
}
bool Renderer::viewportDebugInfo(ViewportId id, ViewportDebugInfo& out) const {
    const Viewport* v = m_impl->findViewport(id);
    if (!v) return false;
    out = v->debug;
    return true;
}

void Renderer::requestPick(ViewportId id, u32 x, u32 y) {
    Viewport* v = m_impl->findViewport(id);
    if (!v || v->pickState != Viewport::PickState::Idle) return;
    v->pickX = std::min(x, v->width ? v->width - 1 : 0);
    v->pickY = std::min(y, v->height ? v->height - 1 : 0);
    v->pickState = Viewport::PickState::Requested;
}

bool Renderer::takePickResult(ViewportId id, u64& entityId) {
    Viewport* v = m_impl->findViewport(id);
    if (!v || v->pickState != Viewport::PickState::Ready) return false;
    entityId = v->pickResult;
    v->pickState = Viewport::PickState::Idle;
    return true;
}

// ---- frame --------------------------------------------------------------------------------------------

namespace {

GpuInstance packInstance(const ExtractedMesh& m) {
    GpuInstance g;
    g.row0 = Vec4(m.world[0][0], m.world[1][0], m.world[2][0], m.world[3][0]);
    g.row1 = Vec4(m.world[0][1], m.world[1][1], m.world[2][1], m.world[3][1]);
    g.row2 = Vec4(m.world[0][2], m.world[1][2], m.world[2][2], m.world[3][2]);
    g.color = m.color;
    g.entityLo = static_cast<u32>(m.entity);
    g.entityHi = static_cast<u32>(m.entity >> 32);
    g.material = m.material;
    g.pad = 0;
    return g;
}

} // namespace

// Counting sort of the extracted meshes by mesh id into one GPU-ready array plus
// one batch (= one instanced draw) per mesh.
void Renderer::Impl::prepareScene(SceneSlot& slot) {
    const u32 meshCount = gpu.meshes.rangeCount();
    const auto& meshes = slot.scene->read().meshes;
    // One bucket per (draw class, mesh). The class comes from the instance's material.
    auto keyOf = [&](const ExtractedMesh& m) {
        const u32 mesh = m.mesh < meshCount ? m.mesh : 0u;
        return static_cast<u32>(gpu.materials.classOf(m.material)) * meshCount + mesh;
    };

    SceneGpu& packed = slot.gpu;
    const u32 buckets = kDrawClassCount * meshCount;
    packed.counts.assign(buckets, 0u);
    packed.cursor.assign(buckets, 0u);
    for (const ExtractedMesh& m : meshes) ++packed.counts[keyOf(m)];

    packed.batches.clear();
    u32 offset = 0;
    for (u32 cls = 0; cls < kDrawClassCount; ++cls) {
        packed.classBegin[cls] = static_cast<u32>(packed.batches.size());
        for (u32 mesh = 0; mesh < meshCount; ++mesh) {
            const u32 key = cls * meshCount + mesh;
            packed.cursor[key] = offset;
            if (packed.counts[key]) packed.batches.push_back({mesh, offset, packed.counts[key]});
            offset += packed.counts[key];
        }
    }
    packed.classBegin[kDrawClassCount] = static_cast<u32>(packed.batches.size());
    packed.instances.resize(meshes.size());
    for (const ExtractedMesh& m : meshes) packed.instances[packed.cursor[keyOf(m)]++] = packInstance(m);

    // Bounding sphere of every batch from the mesh bounds and its instances (one tile of terrain = one instance = an exact
    // sphere; many instances = the box of their centres, grown by the radius): the passes cull on it like on a static set.
    for (SceneGpu::Batch& b : packed.batches) {
        const MeshRange& range = gpu.meshes.range(b.mesh);
        if (range.skinned || range.bounds.w <= 0.0f) continue;
        Vec3 lo(1e30f), hi(-1e30f);
        f32 reach = 0.0f;
        for (u32 k = 0; k < b.count; ++k) {
            const GpuInstance& g = packed.instances[b.first + k];
            const Vec4 c(range.bounds.x, range.bounds.y, range.bounds.z, 1.0f);
            const Vec3 centre(glm::dot(g.row0, c), glm::dot(g.row1, c), glm::dot(g.row2, c));
            const f32 scale = std::sqrt(std::max({glm::dot(Vec3(g.row0), Vec3(g.row0)), glm::dot(Vec3(g.row1), Vec3(g.row1)), glm::dot(Vec3(g.row2), Vec3(g.row2))}));
            lo = glm::min(lo, centre);
            hi = glm::max(hi, centre);
            reach = std::max(reach, range.bounds.w * scale);
        }
        b.sphere = Vec4(0.5f * (lo + hi), 0.5f * glm::length(hi - lo) + reach + 0.5f);
    }

    // Static instance sets (trees, grass): packed after the entities' instances, one culled batch per set.
    struct StaticBatch {
        u32 cls;
        SceneGpu::Batch batch;
    };
    std::vector<StaticBatch> staticBatches;
    for (const auto& [setId, set] : slot.scene->staticSets()) {
        if (!set.enabled || set.instances.empty()) continue;
        const u32 mesh = meshLookup.find(set.mesh, set.submesh);
        if (mesh == MeshLookup::kInvalid || mesh >= meshCount) continue;
        const u32 material = set.material.isValid() ? gpu.materials.lookup().find(set.material) : 0u;
        const u32 count = std::clamp(static_cast<u32>(std::ceil(static_cast<f32>(set.instances.size()) * std::clamp(set.fraction, 0.0f, 1.0f))), 1u,
                                     static_cast<u32>(set.instances.size()));
        const u32 first = static_cast<u32>(packed.instances.size());
        for (u32 k = 0; k < count; ++k) {
            const StaticInstance& in = set.instances[k];
            const f32 c = in.c, sn = in.s;
            GpuInstance g;
            g.row0 = Vec4(c, 0.0f, sn, in.position.x);
            g.row1 = Vec4(0.0f, in.scale, 0.0f, in.position.y);
            g.row2 = Vec4(-sn, 0.0f, c, in.position.z);
            g.color = Vec4(static_cast<f32>(in.color & 0xFFu), static_cast<f32>((in.color >> 8) & 0xFFu), static_cast<f32>((in.color >> 16) & 0xFFu),
                           static_cast<f32>(in.color >> 24)) * (1.0f / 255.0f);
            g.entityLo = g.entityHi = 0;
            g.material = material;
            g.pad = 0;
            packed.instances.push_back(g);
        }
        staticBatches.push_back({static_cast<u32>(gpu.materials.classOf(material)), {mesh, first, count, -1, Vec4(set.center, set.radius), (set.castsShadow ? 0u : kBatchNoShadow) | (set.inProbes ? 0u : kBatchNoProbe)}});
    }

    // Skinned characters: one instance and one single-instance batch each (every character has its own deformed
    // vertices), slotted after the static batches of its draw class. The deformation work is shared between the
    // submeshes of one mesh and one pose.
    packed.skinJobs.clear();
    packed.skinDestination.clear();
    packed.skinnedVertexCount = 0;
    packed.skinGroups = 0;
    const ExtractedScene& extracted = slot.scene->read();
    if (extracted.skinned.empty() && staticBatches.empty()) {
        packed.palettes.clear();
        return;
    }
    packed.palettes = extracted.skinMatrices;
    struct SkinBatch {
        u32 cls;
        SceneGpu::Batch batch;
    };
    std::vector<SkinBatch> skinBatches;
    skinBatches.reserve(extracted.skinned.size());
    for (const ExtractedSkinned& sk : extracted.skinned) {
        if (sk.mesh.mesh >= meshCount) continue;
        const MeshRange& range = gpu.meshes.range(sk.mesh.mesh);
        if (!range.skinned || range.indexCount == 0 || range.vertexCount == 0) continue;
        const u64 key = (static_cast<u64>(static_cast<u32>(range.baseVertex)) << 32) | sk.palette;
        auto [it, fresh] = packed.skinDestination.try_emplace(key, packed.skinnedVertexCount);
        if (fresh) {
            const u32 groups = (range.vertexCount + 63) / 64;
            packed.skinJobs.push_back({static_cast<u32>(range.baseVertex), range.vertexCount, packed.skinnedVertexCount, sk.palette,
                                       packed.skinGroups, 0, 0, 0});
            packed.skinGroups += groups;
            packed.skinnedVertexCount += range.vertexCount;
        }
        const u32 instance = static_cast<u32>(packed.instances.size());
        packed.instances.push_back(packInstance(sk.mesh));
        skinBatches.push_back({static_cast<u32>(gpu.materials.classOf(sk.mesh.material)), {sk.mesh.mesh, instance, 1, static_cast<i32>(it->second)}});
    }
    for (SceneGpu::SkinJob& job : packed.skinJobs) job.jobCount = static_cast<u32>(packed.skinJobs.size());
    if (skinBatches.empty() && staticBatches.empty()) return;

    std::vector<SceneGpu::Batch>& merged = packed.classScratch;
    merged.clear();
    u32 begin[kDrawClassCount + 1];
    for (u32 cls = 0; cls < kDrawClassCount; ++cls) {
        begin[cls] = static_cast<u32>(merged.size());
        merged.insert(merged.end(), packed.batches.begin() + packed.classBegin[cls], packed.batches.begin() + packed.classBegin[cls + 1]);
        for (const StaticBatch& sb : staticBatches)
            if (sb.cls == cls) merged.push_back(sb.batch);
        for (const SkinBatch& sb : skinBatches)
            if (sb.cls == cls) merged.push_back(sb.batch);
    }
    begin[kDrawClassCount] = static_cast<u32>(merged.size());
    packed.batches.swap(merged);
    std::copy(begin, begin + kDrawClassCount + 1, packed.classBegin);
}

void Renderer::beginFrame() {
    Impl& s = *m_impl;
    s.device->beginFrame();
    s.frame = s.device->frameNumber();
    s.shaders->pollReload();
    s.atmosphere.pump();
    s.clouds.pump();
    {
        bool wanted = false;
        for (const auto& v : s.viewports) wanted |= v->active && HasFlag(v->flags, ViewportFlags::Water);
        s.water.simulate(wanted);
    }
    s.pipelines->collect(s.frame, 3600); // a minute: rarely used variants (masked, double-sided) must not recompile on every use
    {
        // Background texture uploads under a byte budget; materials then pick up the new slots.
        std::vector<AssetId> ready;
        s.gpu.textures.pump(s.device->nvrhi(), s.frame, s.desc.textureUploadBudget, ready);
        s.gpu.materials.texturesChanged(ready, s.gpu.textures);
        s.gpu.materials.upload(s.device->nvrhi());
    }
    std::erase_if(s.retiredMeshes, [&](const Impl::RetiredMesh& r) {
        if (r.frame + 4 > s.frame) return false;
        s.gpu.meshes.releaseRange(r.id);
        return true;
    });

    // Finish picks whose GPU work is done (beginFrame just waited for frame - framesInFlight).
    for (auto& v : s.viewports) {
        if (v->pickState != Viewport::PickState::InFlight) continue;
        if (s.frame < v->pickSubmittedFrame + s.device->framesInFlight()) continue;
        size_t pitch = 0;
        const void* data = s.device->nvrhi()->mapStagingTexture(v->pickStaging, nvrhi::TextureSlice(),
                                                                nvrhi::CpuAccessMode::Read, &pitch);
        v->pickResult = 0;
        if (data) {
            const u32* texel = static_cast<const u32*>(data);
            v->pickResult = static_cast<u64>(texel[0]) | (static_cast<u64>(texel[1]) << 32);
            s.device->nvrhi()->unmapStagingTexture(v->pickStaging);
        }
        v->pickState = Viewport::PickState::Ready;
    }

    // Apply pending viewport resizes now, while no graph is in flight.
    for (auto& v : s.viewports) {
        v->retired.clear();
        if (v->pendingWidth != v->width || v->pendingHeight != v->height) {
            v->width = v->pendingWidth;
            v->height = v->pendingHeight;
            v->retired.push_back(std::move(v->output));
            s.createViewportTargets(*v);
        }
    }

    // Publish what the extraction systems wrote and pack the scenes that are shown.
    for (auto& slot : s.scenes) {
        slot.scene->publish();
        slot.inUse = false;
    }
    for (auto& v : s.viewports) {
        if (!v->active) continue;
        if (SceneSlot* slot = s.findScene(v->scene)) slot->inUse = true;
    }
    JobSystem* jobs = s.desc.jobs;
    JobCounter counter;
    s.sceneJobs.clear();
    for (auto& slot : s.scenes) {
        if (slot.inUse) s.sceneJobs.push_back({&s, &slot});
    }
    for (Impl::SceneJob& a : s.sceneJobs) {
        if (jobs && s.sceneJobs.size() > 1) {
            jobs->submit([](void* p) { auto* x = static_cast<Impl::SceneJob*>(p); x->impl->prepareScene(*x->slot); }, &a,
                         counter);
        } else {
            s.prepareScene(*a.slot);
        }
    }
    if (jobs) jobs->wait(counter);
}

void Renderer::renderViewports() {
    Impl& s = *m_impl;
    nvrhi::IDevice* dev = s.device->nvrhi();

    // A: build + compile on this thread (cheap; allocates transients from the pools).
    std::vector<Viewport*>& ready = s.readyViewports;
    ready.clear();
    for (auto& v : s.viewports) {
        if (!v->active) continue;
        s.buildViewportGraph(*v);
        if (v->graphReady) ready.push_back(v.get());
    }

    // B: record the command lists. Viewports are independent, so they go wide on
    //    the job system; each graph may fan out again into recording groups.
    JobSystem* jobs = s.desc.jobs;
    if (jobs && ready.size() > 1) {
        JobCounter counter;
        s.recordJobs.clear();
        for (Viewport* v : ready) s.recordJobs.push_back({v, dev});
        for (Impl::RecordJobArg& a : s.recordJobs) {
            jobs->submit([](void* p) { auto* x = static_cast<Impl::RecordJobArg*>(p); x->vp->graph->record(x->dev); },
                         &a, counter);
        }
        jobs->wait(counter);
    } else {
        for (Viewport* v : ready) v->graph->record(dev);
    }

    // C: submit in creation order, so the GPU order is deterministic.
    for (Viewport* v : ready) {
        v->graph->submit(dev);
        v->graphReady = false;
    }
}

void Renderer::renderWindow(Swapchain& swapchain, WindowPassFn build, void* user) {
    Impl& s = *m_impl;
    nvrhi::IDevice* dev = s.device->nvrhi();
    RenderGraph& g = *s.windowGraph;
    g.reset(s.frame);
    const RgTexture backbuffer = g.importTexture(swapchain.currentBackbuffer(), nvrhi::ResourceStates::Present,
                                                 nvrhi::ResourceStates::Present, "Backbuffer");
    build(g, backbuffer, user);
    g.compile(dev);
    g.execute(dev);
}

RgTexture Renderer::importViewportTexture(RenderGraph& graph, ViewportId viewport) {
    Viewport* v = m_impl->findViewport(viewport);
    FOH_VERIFY(v, "importViewportTexture: unknown viewport");
    return graph.importTexture(v->output.Get(), nvrhi::ResourceStates::ShaderResource,
                               nvrhi::ResourceStates::ShaderResource, "ViewportImage");
}

void Renderer::endFrame() { m_impl->device->endFrame(); }

GraphicsDevice& Renderer::device() { return *m_impl->device; }
ShaderLibrary& Renderer::shaders() { return *m_impl->shaders; }
PipelineCache& Renderer::pipelines() { return *m_impl->pipelines; }
JobSystem* Renderer::jobs() const { return m_impl->desc.jobs; }
u64 Renderer::frameIndex() const { return m_impl->frame; }

} // namespace foh
