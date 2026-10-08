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
    m_impl->gpu = {};
    m_impl->pipelines.reset();
    m_impl->shaders.reset();
    m_impl->device->nvrhi()->runGarbageCollection();
}

std::unique_ptr<Renderer> Renderer::create(const RendererDesc& desc) {
    std::unique_ptr<Renderer> self(new Renderer());
    Impl& s = *self->m_impl;
    s.desc = desc;

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
        !s.shaders->get("tonemap.psTonemap", nvrhi::ShaderType::Pixel)) {
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
        for (u32 t = 0; t < 5; ++t) dl.addItem(nvrhi::BindingLayoutItem::Texture_SRV(t));
        dl.addItem(nvrhi::BindingLayoutItem::Sampler(0));
        s.gpu.layoutDeferred = dev->createBindingLayout(dl);

        // Passes that sample materials: view constants, the material buffer and one sampler (register space 0),
        // plus the bindless texture table (register space 1, a layout of its own).
        nvrhi::BindingLayoutDesc ml;
        ml.visibility = nvrhi::ShaderType::All;
        ml.addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0));
        ml.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0));
        ml.addItem(nvrhi::BindingLayoutItem::Sampler(0));
        s.gpu.layoutMaterial = dev->createBindingLayout(ml);

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
    const MeshStore::Block block = s.gpu.meshes.append(s.device->nvrhi(), vertices, geometry.indices);
    std::vector<u32> ids;
    ids.reserve(submeshes.size());
    for (const MeshIndexRange& r : submeshes) ids.push_back(s.gpu.meshes.addRange({block.firstIndex + r.firstIndex, r.indexCount, block.baseVertex}));
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
}

void Renderer::beginFrame() {
    Impl& s = *m_impl;
    s.device->beginFrame();
    s.frame = s.device->frameNumber();
    s.shaders->pollReload();
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
