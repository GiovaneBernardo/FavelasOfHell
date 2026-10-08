#include "foh/rhi/render_graph.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>

#include "foh/assert.h"
#include "foh/jobs.h"
#include "foh/log.h"

namespace foh {

using nvrhi::ResourceStates;

// ---- small public helpers ---------------------------------------------------------

ResourceStates ToResourceStates(RgAccess access) {
    switch (access) {
        case RgAccess::SampledRead: return ResourceStates::ShaderResource;
        case RgAccess::StorageRead:
        case RgAccess::StorageWrite: return ResourceStates::UnorderedAccess;
        case RgAccess::ColorTarget: return ResourceStates::RenderTarget;
        case RgAccess::DepthWrite: return ResourceStates::DepthWrite;
        case RgAccess::DepthRead: return ResourceStates::DepthRead;
        case RgAccess::CopySrc: return ResourceStates::CopySource;
        case RgAccess::CopyDst: return ResourceStates::CopyDest;
        case RgAccess::VertexRead: return ResourceStates::VertexBuffer;
        case RgAccess::IndexRead: return ResourceStates::IndexBuffer;
        case RgAccess::ConstantRead: return ResourceStates::ConstantBuffer;
        case RgAccess::IndirectRead: return ResourceStates::IndirectArgument;
    }
    return ResourceStates::Common;
}

bool RgTextureDesc::sameShape(const RgTextureDesc& o) const {
    return width == o.width && height == o.height && depth == o.depth && arraySize == o.arraySize &&
           mipLevels == o.mipLevels && sampleCount == o.sampleCount && format == o.format &&
           dimension == o.dimension && renderTarget == o.renderTarget && storage == o.storage &&
           useClearValue == o.useClearValue && (!useClearValue || clearValue == o.clearValue);
}

bool RgBufferDesc::sameShape(const RgBufferDesc& o) const {
    return byteSize == o.byteSize && structStride == o.structStride && storage == o.storage &&
           vertex == o.vertex && index == o.index && indirect == o.indirect;
}

namespace {

// Default factory: transients are created on the device, always starting in
// the Common state that the state simulation assumes for fresh resources.
class DeviceFactory final : public RgResourceFactory {
public:
    explicit DeviceFactory(nvrhi::IDevice* device) : m_device(device) {}

    nvrhi::TextureHandle createTexture(const RgTextureDesc& d) override {
        nvrhi::TextureDesc td;
        td.width = d.width;
        td.height = d.height;
        td.depth = d.depth;
        td.arraySize = d.arraySize;
        td.mipLevels = d.mipLevels;
        td.sampleCount = d.sampleCount;
        td.format = d.format;
        td.dimension = d.dimension;
        td.isRenderTarget = d.renderTarget;
        td.isUAV = d.storage;
        td.useClearValue = d.useClearValue;
        td.clearValue = d.clearValue;
        td.initialState = ResourceStates::Common;
        td.keepInitialState = false;
        td.debugName = d.name;
        return m_device->createTexture(td);
    }

    nvrhi::BufferHandle createBuffer(const RgBufferDesc& d) override {
        nvrhi::BufferDesc bd;
        bd.byteSize = d.byteSize;
        bd.structStride = d.structStride;
        bd.canHaveUAVs = d.storage;
        bd.isVertexBuffer = d.vertex;
        bd.isIndexBuffer = d.index;
        bd.isDrawIndirectArgs = d.indirect;
        bd.initialState = ResourceStates::Common;
        bd.keepInitialState = false;
        bd.debugName = d.name;
        return m_device->createBuffer(bd);
    }

private:
    nvrhi::IDevice* m_device;
};

constexpr u32 kMaxGroups = 16;

bool isWriteState(ResourceStates s) {
    return (s & (ResourceStates::RenderTarget | ResourceStates::DepthWrite | ResourceStates::UnorderedAccess |
                 ResourceStates::CopyDest)) != ResourceStates::Unknown;
}

u64 hashCombine(u64 seed, u64 v) { return seed ^ (v + 0x9E3779B97F4A7C15ull + (seed << 6) + (seed >> 2)); }

} // namespace

// ---- implementation state -----------------------------------------------------------

struct RenderGraph::Impl {
    struct Chunk {
        std::unique_ptr<std::byte[]> memory;
        size_t size = 0;
        size_t used = 0;
    };
    struct Resource {
        bool isBuffer = false;
        bool imported = false;
        RgTextureDesc tex;
        RgBufferDesc buf;
        nvrhi::ITexture* importedTex = nullptr;
        nvrhi::IBuffer* importedBuf = nullptr;
        ResourceStates initial = ResourceStates::Common;
        ResourceStates final = ResourceStates::Unknown;
        const char* name = "";
        // compile results
        bool needed = false;
        i32 first = -1; // live-list positions of the first/last access
        i32 last = -1;
        u32 phys = kRgInvalid;
        ResourceStates endState = ResourceStates::Common;
    };
    struct Access {
        u32 res;
        RgAccess access;
    };
    struct Pass {
        const char* name = "";
        RgPassFlags flags = RgPassFlags::None;
        const void* fn = nullptr;
        const void* data = nullptr;
        InvokeFn invoke = nullptr;
        u32 accessBegin = 0;
        u32 accessCount = 0;
        RgTexture color[kRgMaxColorTargets];
        u32 colorCount = 0;
        RgTexture depth;
        bool depthReadOnly = false;
        bool culled = false;
        u32 group = kRgInvalid;
        u32 transBegin = 0;
        u32 transCount = 0;
        nvrhi::IFramebuffer* framebuffer = nullptr;
    };
    struct Transition {
        u32 phys;
        ResourceStates state;
    };
    struct Group {
        u32 liveBegin = 0, liveEnd = 0;
        u32 trackBegin = 0, trackCount = 0;
        u32 fixupBegin = 0, fixupCount = 0;
    };
    struct Phys {
        bool isBuffer = false;
        nvrhi::ITexture* tex = nullptr;
        nvrhi::IBuffer* buf = nullptr;
        ResourceStates state = ResourceStates::Common;
        i32 pool = -1; // pool entry, -1 for imported
        u32 groupStamp = kRgInvalid; // last group that began tracking this resource
    };
    struct PoolTex {
        RgTextureDesc desc;
        nvrhi::TextureHandle handle;
        ResourceStates state = ResourceStates::Common;
        u64 lastUsed = 0;
        bool inUse = false;
    };
    struct PoolBuf {
        RgBufferDesc desc;
        nvrhi::BufferHandle handle;
        ResourceStates state = ResourceStates::Common;
        u64 lastUsed = 0;
        bool inUse = false;
    };

    struct FbKey {
        nvrhi::ITexture* t[kRgMaxColorTargets + 1] = {};
        bool depthReadOnly = false;
        bool operator==(const FbKey& o) const {
            return std::memcmp(t, o.t, sizeof(t)) == 0 && depthReadOnly == o.depthReadOnly;
        }
        u64 hash() const {
            u64 h = depthReadOnly ? 1 : 0;
            for (auto* p : t) h = hashCombine(h, reinterpret_cast<std::uintptr_t>(p));
            return h;
        }
    };
    struct FbEntry {
        FbKey key;
        nvrhi::FramebufferHandle fb;
        u64 lastUsed = 0;
        // Touches an imported texture. The owner may destroy or resize those between
        // frames (swapchain buffers, viewport targets), so such entries never outlive
        // the frame: see submit().
        bool external = false;
    };
    struct BindEntry {
        nvrhi::BindingSetDesc desc;
        nvrhi::IBindingLayout* layout = nullptr;
        nvrhi::BindingSetHandle set;
        u64 lastUsed = 0;
    };

    RenderGraphConfig cfg;
    u64 frame = 0;
    bool compiled = false;
    bool recorded = false;
    bool buildingPass = false;

    std::vector<Chunk> chunks;
    size_t chunkCursor = 0;

    std::vector<Resource> resources;
    std::vector<Access> accesses;
    std::vector<Pass> passes;

    // plan
    std::vector<u32> live;
    std::vector<Group> groups;
    std::vector<Transition> transitions;
    std::vector<Transition> tracks;
    std::vector<Transition> fixups;
    std::vector<Phys> phys;
    std::vector<RgPassInfo> passInfos;
    RgStats stats;

    // persistent
    std::vector<PoolTex> texPool;
    std::vector<PoolBuf> bufPool;
    std::unordered_map<u64, FbEntry> fbCache;
    std::vector<std::unordered_map<u64, BindEntry>> bindCaches; // one per group
    std::vector<nvrhi::CommandListHandle> cmdLists;

    // compile() scratch, kept to avoid per-frame allocations
    std::vector<u32> scratchPoolTex, scratchPoolBuf;
    std::vector<Transition> scratchMerged;

    void* allocate(size_t size, size_t align);
    Pass& pass(u32 i) { return passes[i]; }
    void compile(RgResourceFactory& factory);
    void record(nvrhi::IDevice* device, RenderGraph& graph);
    void submit(nvrhi::IDevice* device);
    void recordGroup(nvrhi::IDevice* device, RenderGraph& graph, u32 group);
    void retire();
};

void* RenderGraph::Impl::allocate(size_t size, size_t align) {
    for (;;) {
        if (chunkCursor < chunks.size()) {
            Chunk& c = chunks[chunkCursor];
            const size_t offset = (c.used + align - 1) & ~(align - 1);
            if (offset + size <= c.size) {
                c.used = offset + size;
                return c.memory.get() + offset;
            }
            ++chunkCursor;
            continue;
        }
        Chunk c;
        c.size = std::max<size_t>(16 * 1024, size + align);
        c.memory = std::make_unique<std::byte[]>(c.size);
        chunks.push_back(std::move(c));
    }
}

// ---- RenderGraph: building -----------------------------------------------------------

RenderGraph::RenderGraph(const RenderGraphConfig& config) : m_impl(std::make_unique<Impl>()) {
    m_impl->cfg = config;
    m_impl->cfg.maxRecordingGroups = std::clamp(m_impl->cfg.maxRecordingGroups, 1u, kMaxGroups);
    if (m_impl->cfg.minPassesPerGroup < 1) m_impl->cfg.minPassesPerGroup = 1;
}

RenderGraph::~RenderGraph() = default;

void* RenderGraph::allocate(size_t size, size_t align) { return m_impl->allocate(size, align); }

void RenderGraph::reset(u64 frameIndex) {
    Impl& s = *m_impl;
    s.frame = frameIndex;
    s.compiled = false;
    s.recorded = false;
    s.buildingPass = false;
    for (Impl::Chunk& c : s.chunks) c.used = 0;
    s.chunkCursor = 0;
    s.resources.clear();
    s.accesses.clear();
    s.passes.clear();
    s.live.clear();
    s.groups.clear();
    s.transitions.clear();
    s.tracks.clear();
    s.fixups.clear();
    s.phys.clear();
    s.passInfos.clear();
    s.stats = {};
    for (auto& p : s.texPool) p.inUse = false;
    for (auto& p : s.bufPool) p.inUse = false;
    s.retire();
}

// Frees pooled resources and cache entries nobody used for a few frames.
void RenderGraph::Impl::retire() {
    const u64 keep = cfg.poolRetireFrames;
    const u64 limit = frame > keep ? frame - keep : 0;
    std::erase_if(texPool, [&](const PoolTex& p) { return !p.inUse && p.lastUsed < limit; });
    std::erase_if(bufPool, [&](const PoolBuf& p) { return !p.inUse && p.lastUsed < limit; });
    // Binding sets and framebuffers keep their textures alive, so they go quickly:
    // a resized viewport must not linger in memory because the UI cached a set for it.
    std::erase_if(fbCache, [&](const auto& kv) { return kv.second.lastUsed + 3 < frame; });
    for (auto& cache : bindCaches)
        std::erase_if(cache, [&](const auto& kv) { return kv.second.lastUsed + 3 < frame; });
}

void RenderGraph::releaseAll() {
    Impl& s = *m_impl;
    s.texPool.clear();
    s.bufPool.clear();
    s.fbCache.clear();
    s.bindCaches.clear();
    s.cmdLists.clear();
}

RgTexture RenderGraph::importTexture(nvrhi::ITexture* texture, ResourceStates initial, ResourceStates final,
                                     const char* name) {
    FOH_ASSERT(texture, "importTexture: null texture");
    Impl::Resource r;
    r.imported = true;
    r.importedTex = texture;
    r.initial = initial;
    r.final = final;
    r.name = name;
    r.needed = true;
    m_impl->resources.push_back(r);
    return {static_cast<u32>(m_impl->resources.size() - 1)};
}

RgBuffer RenderGraph::importBuffer(nvrhi::IBuffer* buffer, ResourceStates initial, ResourceStates final,
                                   const char* name) {
    FOH_ASSERT(buffer, "importBuffer: null buffer");
    Impl::Resource r;
    r.isBuffer = true;
    r.imported = true;
    r.importedBuf = buffer;
    r.initial = initial;
    r.final = final;
    r.name = name;
    r.needed = true;
    m_impl->resources.push_back(r);
    return {static_cast<u32>(m_impl->resources.size() - 1)};
}

u32 RenderGraph::beginPass(const char* name, RgPassFlags flags) {
    Impl& s = *m_impl;
    FOH_ASSERT(!s.buildingPass, "addPass called from inside a pass setup");
    FOH_ASSERT(!s.compiled, "addPass after compile(); call reset() first");
    s.buildingPass = true;
    Impl::Pass p;
    p.name = name;
    p.flags = flags;
    p.accessBegin = static_cast<u32>(s.accesses.size());
    s.passes.push_back(p);
    return static_cast<u32>(s.passes.size() - 1);
}

void RenderGraph::setPassBody(u32 pass, const void* fn, const void* data, InvokeFn invoke) {
    Impl::Pass& p = m_impl->passes[pass];
    p.fn = fn;
    p.data = data;
    p.invoke = invoke;
    m_impl->buildingPass = false;
}

// ---- RgPassBuilder ----------------------------------------------------------------------

using Impl = RenderGraph::Impl;

RgTexture RgPassBuilder::createTexture(const RgTextureDesc& desc) {
    auto& s = *m_graph->m_impl;
    Impl::Resource r;
    r.tex = desc;
    r.name = desc.name;
    s.resources.push_back(r);
    return {static_cast<u32>(s.resources.size() - 1)};
}

RgBuffer RgPassBuilder::createBuffer(const RgBufferDesc& desc) {
    auto& s = *m_graph->m_impl;
    Impl::Resource r;
    r.isBuffer = true;
    r.buf = desc;
    r.name = desc.name;
    s.resources.push_back(r);
    return {static_cast<u32>(s.resources.size() - 1)};
}

namespace {

void addAccess(RenderGraph::Impl& s, u32 pass, u32 res, RgAccess access) {
    FOH_ASSERT(res < s.resources.size(), "invalid render graph resource handle");
    auto& p = s.passes[pass];
    FOH_ASSERT(p.accessBegin + p.accessCount == s.accesses.size(), "pass accesses must be declared inside its setup");
    const bool bufferAccess = access == RgAccess::VertexRead || access == RgAccess::IndexRead ||
                              access == RgAccess::ConstantRead || access == RgAccess::IndirectRead;
    const bool textureOnly = access == RgAccess::ColorTarget || access == RgAccess::DepthRead ||
                             access == RgAccess::DepthWrite;
    FOH_ASSERT(!(bufferAccess && !s.resources[res].isBuffer), "buffer-only access used on a texture");
    FOH_ASSERT(!(textureOnly && s.resources[res].isBuffer), "texture-only access used on a buffer");
    (void)bufferAccess;
    (void)textureOnly;
    s.accesses.push_back({res, access});
    ++p.accessCount;
}

} // namespace

RgTexture RgPassBuilder::read(RgTexture t, RgAccess a) {
    FOH_ASSERT(!IsWriteAccess(a), "read() with a write access; use write()");
    addAccess(*m_graph->m_impl, m_pass, t.index, a);
    return t;
}
RgBuffer RgPassBuilder::read(RgBuffer b, RgAccess a) {
    FOH_ASSERT(!IsWriteAccess(a), "read() with a write access; use write()");
    addAccess(*m_graph->m_impl, m_pass, b.index, a);
    return b;
}
RgTexture RgPassBuilder::write(RgTexture t, RgAccess a) {
    FOH_ASSERT(IsWriteAccess(a), "write() with a read access; use read()");
    addAccess(*m_graph->m_impl, m_pass, t.index, a);
    return t;
}
RgBuffer RgPassBuilder::write(RgBuffer b, RgAccess a) {
    FOH_ASSERT(IsWriteAccess(a), "write() with a read access; use read()");
    addAccess(*m_graph->m_impl, m_pass, b.index, a);
    return b;
}

RgTexture RgPassBuilder::colorTarget(u32 slot, RgTexture texture) {
    FOH_VERIFY(slot < kRgMaxColorTargets, "color target slot out of range");
    auto& p = m_graph->m_impl->passes[m_pass];
    p.color[slot] = texture;
    p.colorCount = std::max(p.colorCount, slot + 1);
    addAccess(*m_graph->m_impl, m_pass, texture.index, RgAccess::ColorTarget);
    return texture;
}

RgTexture RgPassBuilder::depthTarget(RgTexture texture, bool readOnly) {
    auto& p = m_graph->m_impl->passes[m_pass];
    p.depth = texture;
    p.depthReadOnly = readOnly;
    addAccess(*m_graph->m_impl, m_pass, texture.index, readOnly ? RgAccess::DepthRead : RgAccess::DepthWrite);
    return texture;
}

void RgPassBuilder::neverCull() {
    auto& p = m_graph->m_impl->passes[m_pass];
    p.flags = p.flags | RgPassFlags::NeverCull;
}

// ---- compile --------------------------------------------------------------------------------

void RenderGraph::compile(nvrhi::IDevice* device) {
    DeviceFactory factory(device);
    compile(factory);
}

void RenderGraph::compile(RgResourceFactory& factory) { m_impl->compile(factory); }

void RenderGraph::Impl::compile(RgResourceFactory& factory) {
    FOH_ASSERT(!buildingPass, "compile() inside a pass setup");
    const u32 passCount = static_cast<u32>(passes.size());

    // 1. Culling, walking backwards: a pass lives if it is NeverCull or writes a
    //    resource somebody still needs. A live pass needs everything it touches
    //    (a write may preserve earlier contents, so earlier writers stay alive).
    for (Resource& r : resources) {
        r.needed = r.imported;
        r.first = r.last = -1;
        r.phys = kRgInvalid;
    }
    for (u32 i = passCount; i-- > 0;) {
        Pass& p = passes[i];
        bool keep = (static_cast<u8>(p.flags) & static_cast<u8>(RgPassFlags::NeverCull)) != 0;
        for (u32 a = 0; a < p.accessCount && !keep; ++a) {
            const Access& acc = accesses[p.accessBegin + a];
            keep = IsWriteAccess(acc.access) && resources[acc.res].needed;
        }
        p.culled = !keep;
        if (keep) {
            for (u32 a = 0; a < p.accessCount; ++a) resources[accesses[p.accessBegin + a].res].needed = true;
        }
    }
    live.clear();
    for (u32 i = 0; i < passCount; ++i) {
        if (!passes[i].culled) live.push_back(i);
    }

    // 2. Lifetimes over the live passes.
    for (u32 k = 0; k < live.size(); ++k) {
        const Pass& p = passes[live[k]];
        for (u32 a = 0; a < p.accessCount; ++a) {
            Resource& r = resources[accesses[p.accessBegin + a].res];
            if (r.first < 0) r.first = static_cast<i32>(k);
            r.last = static_cast<i32>(k);
        }
    }

    // 3. Command list groups: contiguous slices of the live passes. Small graphs
    //    stay in one group, so they pay nothing for the parallel machinery.
    const u32 liveCount = static_cast<u32>(live.size());
    u32 groupCount = 0;
    if (liveCount > 0) {
        groupCount = 1;
        if (cfg.jobs && cfg.jobs->workerCount() > 0) {
            groupCount = std::clamp((liveCount + cfg.minPassesPerGroup - 1) / cfg.minPassesPerGroup, 1u,
                                    cfg.maxRecordingGroups);
        }
    }
    groups.assign(groupCount, Group{});
    for (u32 g = 0, at = 0; g < groupCount; ++g) {
        const u32 n = liveCount / groupCount + (g < liveCount % groupCount ? 1 : 0);
        groups[g].liveBegin = at;
        groups[g].liveEnd = at + n;
        at += n;
        for (u32 k = groups[g].liveBegin; k < groups[g].liveEnd; ++k) passes[live[k]].group = g;
    }
    if (bindCaches.size() < groupCount) bindCaches.resize(groupCount);

    // 4. Allocation + state simulation in one sweep over the live passes.
    phys.clear();
    transitions.clear();
    tracks.clear();
    fixups.clear();
    auto& poolTexPhys = scratchPoolTex;
    auto& poolBufPhys = scratchPoolBuf;
    poolTexPhys.assign(texPool.size(), kRgInvalid);
    poolBufPhys.assign(bufPool.size(), kRgInvalid);
    stats = {};

    auto physForImported = [&](Resource& r) -> u32 {
        for (u32 i = 0; i < phys.size(); ++i) {
            if (phys[i].pool < 0 && ((!r.isBuffer && phys[i].tex == r.importedTex && !phys[i].isBuffer) ||
                                     (r.isBuffer && phys[i].buf == r.importedBuf && phys[i].isBuffer))) {
                return i;
            }
        }
        Phys ph;
        ph.isBuffer = r.isBuffer;
        ph.tex = r.importedTex;
        ph.buf = r.importedBuf;
        ph.state = r.initial;
        phys.push_back(ph);
        return static_cast<u32>(phys.size() - 1);
    };

    auto acquire = [&](Resource& r) -> u32 {
        if (r.imported) return physForImported(r);
        if (!r.isBuffer) {
            ++stats.transientTextures;
            i32 found = -1;
            for (size_t i = 0; i < texPool.size(); ++i) {
                if (!texPool[i].inUse && texPool[i].desc.sameShape(r.tex)) {
                    found = static_cast<i32>(i);
                    break;
                }
            }
            if (found < 0) {
                PoolTex e;
                e.desc = r.tex;
                e.handle = factory.createTexture(r.tex);
                texPool.push_back(std::move(e));
                poolTexPhys.push_back(kRgInvalid);
                found = static_cast<i32>(texPool.size() - 1);
            }
            PoolTex& e = texPool[found];
            e.inUse = true;
            e.lastUsed = frame;
            if (poolTexPhys[found] == kRgInvalid) {
                Phys ph;
                ph.tex = e.handle.Get();
                ph.state = e.state;
                ph.pool = found;
                phys.push_back(ph);
                poolTexPhys[found] = static_cast<u32>(phys.size() - 1);
                ++stats.physicalTextures;
            }
            return poolTexPhys[found];
        }
        ++stats.transientBuffers;
        i32 found = -1;
        for (size_t i = 0; i < bufPool.size(); ++i) {
            if (!bufPool[i].inUse && bufPool[i].desc.sameShape(r.buf)) {
                found = static_cast<i32>(i);
                break;
            }
        }
        if (found < 0) {
            PoolBuf e;
            e.desc = r.buf;
            e.handle = factory.createBuffer(r.buf);
            bufPool.push_back(std::move(e));
            poolBufPhys.push_back(kRgInvalid);
            found = static_cast<i32>(bufPool.size() - 1);
        }
        PoolBuf& e = bufPool[found];
        e.inUse = true;
        e.lastUsed = frame;
        if (poolBufPhys[found] == kRgInvalid) {
            Phys ph;
            ph.isBuffer = true;
            ph.buf = e.handle.Get();
            ph.state = e.state;
            ph.pool = found;
            phys.push_back(ph);
            poolBufPhys[found] = static_cast<u32>(phys.size() - 1);
            ++stats.physicalBuffers;
        }
        return poolBufPhys[found];
    };

    auto release = [&](Resource& r) {
        r.endState = phys[r.phys].state;
        if (r.imported) return;
        if (!r.isBuffer) {
            texPool[phys[r.phys].pool].inUse = false;
        } else {
            bufPool[phys[r.phys].pool].inUse = false;
        }
    };

    auto& merged = scratchMerged; // per pass
    for (u32 g = 0; g < groupCount; ++g) {
        Group& group = groups[g];
        group.trackBegin = static_cast<u32>(tracks.size());
        for (u32 k = group.liveBegin; k < group.liveEnd; ++k) {
            Pass& p = passes[live[k]];
            merged.clear();
            for (u32 a = 0; a < p.accessCount; ++a) {
                const Access& acc = accesses[p.accessBegin + a];
                Resource& r = resources[acc.res];
                if (r.first == static_cast<i32>(k) && r.phys == kRgInvalid) {
                    r.phys = acquire(r);
                    if (!r.imported && !IsWriteAccess(acc.access) && acc.access != RgAccess::DepthRead &&
                        acc.access != RgAccess::StorageRead) {
                        // Reading a transient before anything wrote it: legal (contents undefined) but
                        // almost always a declaration mistake.
                        FOH_WARN("render graph: pass '{}' reads transient '{}' before any pass wrote it", p.name,
                                 r.name);
                    }
                }
                const ResourceStates st = ToResourceStates(acc.access);
                bool found = false;
                for (Transition& t : merged) {
                    if (t.phys != r.phys) continue;
                    FOH_ASSERT(t.state == st || !(isWriteState(t.state) || isWriteState(st)),
                               "a pass uses one resource with two different states and at least one is a write");
                    t.state = t.state | st;
                    found = true;
                    break;
                }
                if (!found) merged.push_back({r.phys, st});
            }
            p.transBegin = static_cast<u32>(transitions.size());
            for (const Transition& t : merged) {
                Phys& ph = phys[t.phys];
                if (ph.groupStamp != g) {
                    ph.groupStamp = g;
                    tracks.push_back({t.phys, ph.state}); // state at the start of this group
                }
                if (ph.state != t.state) ++stats.barrierTransitions;
                ph.state = t.state;
                transitions.push_back(t);
            }
            p.transCount = static_cast<u32>(merged.size());

            for (u32 a = 0; a < p.accessCount; ++a) {
                Resource& r = resources[accesses[p.accessBegin + a].res];
                if (r.last == static_cast<i32>(k) && r.phys != kRgInvalid) release(r);
            }
        }
        group.trackCount = static_cast<u32>(tracks.size()) - group.trackBegin;
    }

    // Imported resources with a requested final state: transition at the very end.
    if (groupCount > 0) {
        Group& last = groups[groupCount - 1];
        last.fixupBegin = static_cast<u32>(fixups.size());
        for (Resource& r : resources) {
            if (!r.imported || r.phys == kRgInvalid) continue;
            Phys& ph = phys[r.phys];
            if (r.final != ResourceStates::Unknown && ph.state != r.final) {
                if (ph.groupStamp != groupCount - 1) {
                    ph.groupStamp = groupCount - 1;
                    tracks.push_back({r.phys, ph.state});
                    ++last.trackCount;
                }
                fixups.push_back({r.phys, r.final});
                ph.state = r.final;
                ++stats.barrierTransitions;
            }
            r.endState = ph.state;
        }
        last.fixupCount = static_cast<u32>(fixups.size()) - last.fixupBegin;
    }

    // Pooled resources remember the state they were left in for the next frame.
    for (size_t i = 0; i < texPool.size(); ++i) {
        if (poolTexPhys[i] != kRgInvalid) texPool[i].state = phys[poolTexPhys[i]].state;
    }
    for (size_t i = 0; i < bufPool.size(); ++i) {
        if (poolBufPhys[i] != kRgInvalid) bufPool[i].state = phys[poolBufPhys[i]].state;
    }

    // Debugger data.
    passInfos.clear();
    for (const Pass& p : passes) {
        passInfos.push_back({p.name, p.culled, p.culled ? kRgInvalid : p.group, p.accessCount});
    }
    stats.passCount = passCount;
    stats.culledPasses = passCount - liveCount;
    stats.groups = groupCount;
    stats.pooledTextures = static_cast<u32>(texPool.size());
    stats.pooledBuffers = static_cast<u32>(bufPool.size());
    compiled = true;
}

// ---- execute ----------------------------------------------------------------------------------

void RenderGraph::record(nvrhi::IDevice* device) { m_impl->record(device, *this); }
void RenderGraph::submit(nvrhi::IDevice* device) { m_impl->submit(device); }

void RenderGraph::execute(nvrhi::IDevice* device) {
    record(device);
    submit(device);
}

namespace {
struct RecordJob {
    RenderGraph::Impl* impl;
    RenderGraph* graph;
    nvrhi::IDevice* device;
    u32 group;
};
} // namespace

void RenderGraph::Impl::record(nvrhi::IDevice* device, RenderGraph& graph) {
    FOH_ASSERT(compiled, "record() before compile()");
    recorded = false;
    if (groups.empty()) return;

    // Framebuffers are resolved up front on this thread, so pass bodies only read.
    for (const u32 index : live) {
        Pass& p = passes[index];
        p.framebuffer = nullptr;
        if (p.colorCount == 0 && !p.depth.valid()) continue;
        FbKey key;
        nvrhi::FramebufferDesc fd;
        bool external = false;
        for (u32 c = 0; c < p.colorCount; ++c) {
            FOH_ASSERT(p.color[c].valid(), "color target slots must be contiguous");
            const Phys& ph = phys[resources[p.color[c].index].phys];
            key.t[c] = ph.tex;
            external |= ph.pool < 0;
            fd.addColorAttachment(ph.tex);
        }
        if (p.depth.valid()) {
            const Phys& ph = phys[resources[p.depth.index].phys];
            nvrhi::ITexture* t = ph.tex;
            external |= ph.pool < 0;
            key.t[kRgMaxColorTargets] = t;
            key.depthReadOnly = p.depthReadOnly;
            fd.depthAttachment = nvrhi::FramebufferAttachment().setTexture(t).setReadOnly(p.depthReadOnly);
        }
        FbEntry& entry = fbCache[key.hash()];
        if (!entry.fb || !(entry.key == key)) {
            entry.key = key;
            entry.fb = device->createFramebuffer(fd);
        }
        entry.external = external;
        entry.lastUsed = frame;
        p.framebuffer = entry.fb.Get();
    }

    const u32 groupCount = static_cast<u32>(groups.size());
    while (cmdLists.size() < groupCount) {
        nvrhi::CommandListParameters params;
        params.enableImmediateExecution = false;
        cmdLists.push_back(device->createCommandList(params));
    }

    RecordJob jobs[kMaxGroups];
    for (u32 g = 0; g < groupCount; ++g) jobs[g] = {this, &graph, device, g};

    if (groupCount == 1 || !cfg.jobs) {
        for (u32 g = 0; g < groupCount; ++g) recordGroup(device, graph, g);
    } else {
        JobCounter counter;
        for (u32 g = 1; g < groupCount; ++g) {
            cfg.jobs->submit(
                [](void* arg) {
                    auto* j = static_cast<RecordJob*>(arg);
                    j->impl->recordGroup(j->device, *j->graph, j->group);
                },
                &jobs[g], counter);
        }
        recordGroup(device, graph, 0); // the calling thread records too
        cfg.jobs->wait(counter);
    }

    recorded = true;
}

void RenderGraph::Impl::submit(nvrhi::IDevice* device) {
    if (!recorded) return;
    nvrhi::ICommandList* raw[kMaxGroups];
    const u32 groupCount = static_cast<u32>(groups.size());
    for (u32 g = 0; g < groupCount; ++g) raw[g] = cmdLists[g].Get();
    device->executeCommandLists(raw, groupCount);
    recorded = false;
    // Command lists hold their own references until the GPU is done; the cache must
    // not keep imported textures (e.g. swapchain buffers) alive beyond this frame.
    std::erase_if(fbCache, [](const auto& kv) { return kv.second.external; });
}

void RenderGraph::Impl::recordGroup(nvrhi::IDevice* device, RenderGraph& graph, u32 g) {
    nvrhi::ICommandList* cmd = cmdLists[g].Get();
    const Group& group = groups[g];
    cmd->open();

    for (u32 i = 0; i < group.trackCount; ++i) {
        const Transition& t = tracks[group.trackBegin + i];
        const Phys& ph = phys[t.phys];
        if (ph.isBuffer) {
            cmd->beginTrackingBufferState(ph.buf, t.state);
        } else {
            cmd->beginTrackingTextureState(ph.tex, nvrhi::AllSubresources, t.state);
        }
    }

    for (u32 k = group.liveBegin; k < group.liveEnd; ++k) {
        const u32 passIndex = live[k];
        const Pass& p = passes[passIndex];
        if (cfg.debugMarkers) cmd->beginMarker(p.name);

        for (u32 i = 0; i < p.transCount; ++i) {
            const Transition& t = transitions[p.transBegin + i];
            const Phys& ph = phys[t.phys];
            if (ph.isBuffer) {
                cmd->setBufferState(ph.buf, t.state);
            } else {
                cmd->setTextureState(ph.tex, nvrhi::AllSubresources, t.state);
            }
        }
        if (p.transCount) cmd->commitBarriers();

        RgPassContext ctx;
        ctx.m_graph = &graph;
        ctx.m_cmd = cmd;
        ctx.m_device = device;
        ctx.m_framebuffer = p.framebuffer;
        ctx.m_group = g;
        ctx.m_pass = passIndex;
        p.invoke(p.fn, p.data, ctx);

        if (cfg.debugMarkers) cmd->endMarker();
    }

    for (u32 i = 0; i < group.fixupCount; ++i) {
        const Transition& t = fixups[group.fixupBegin + i];
        const Phys& ph = phys[t.phys];
        if (ph.isBuffer) {
            cmd->setBufferState(ph.buf, t.state);
        } else {
            cmd->setTextureState(ph.tex, nvrhi::AllSubresources, t.state);
        }
    }
    if (group.fixupCount) cmd->commitBarriers();
    cmd->close();
}

// ---- pass context --------------------------------------------------------------------------------

nvrhi::ITexture* RgPassContext::texture(RgTexture handle) const {
    auto& s = *m_graph->m_impl;
    FOH_ASSERT(handle.index < s.resources.size() && !s.resources[handle.index].isBuffer, "invalid texture handle");
#if !defined(NDEBUG)
    const auto& p = s.passes[m_pass];
    bool declared = false;
    for (u32 a = 0; a < p.accessCount; ++a) declared |= s.accesses[p.accessBegin + a].res == handle.index;
    FOH_ASSERT(declared, "pass uses a texture it did not declare in its setup");
#endif
    return s.phys[s.resources[handle.index].phys].tex;
}

nvrhi::IBuffer* RgPassContext::buffer(RgBuffer handle) const {
    auto& s = *m_graph->m_impl;
    FOH_ASSERT(handle.index < s.resources.size() && s.resources[handle.index].isBuffer, "invalid buffer handle");
#if !defined(NDEBUG)
    const auto& p = s.passes[m_pass];
    bool declared = false;
    for (u32 a = 0; a < p.accessCount; ++a) declared |= s.accesses[p.accessBegin + a].res == handle.index;
    FOH_ASSERT(declared, "pass uses a buffer it did not declare in its setup");
#endif
    return s.phys[s.resources[handle.index].phys].buf;
}

nvrhi::IBindingSet* RgPassContext::bindingSet(const nvrhi::BindingSetDesc& desc,
                                              nvrhi::IBindingLayout* layout) const {
    auto& s = *m_graph->m_impl;
    auto& cache = s.bindCaches[m_group];
    const u64 key = hashCombine(std::hash<nvrhi::BindingSetDesc>{}(desc), reinterpret_cast<std::uintptr_t>(layout));
    auto& entry = cache[key];
    if (!entry.set || entry.layout != layout || !(entry.desc == desc)) {
        entry.desc = desc;
        entry.layout = layout;
        entry.set = m_device->createBindingSet(desc, layout);
    }
    entry.lastUsed = s.frame;
    return entry.set.Get();
}

u64 RgPassContext::frameIndex() const { return m_graph->m_impl->frame; }

// ---- accessors --------------------------------------------------------------------------------------

ResourceStates RenderGraph::finalStateOf(RgTexture t) const {
    const auto& r = m_impl->resources[t.index];
    return r.phys == kRgInvalid ? r.initial : r.endState;
}
ResourceStates RenderGraph::finalStateOf(RgBuffer b) const {
    const auto& r = m_impl->resources[b.index];
    return r.phys == kRgInvalid ? r.initial : r.endState;
}
std::span<const RgPassInfo> RenderGraph::passInfos() const { return m_impl->passInfos; }
const RgStats& RenderGraph::stats() const { return m_impl->stats; }
u64 RenderGraph::frameIndex() const { return m_impl->frame; }

} // namespace foh
